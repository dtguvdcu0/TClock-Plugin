#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "tcard_core.h"
#include "card_rich.h"
#include "card_assets.h"
#include "card_sync_store.h"
#include "card_native_root.h"

#include <algorithm>
#include <cmath>
#include <any>
#include <map>
#include <cwctype>
#include <memory>
#include <string_view>
#include <vector>
#include <iphlpapi.h>
#include <wlanapi.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "wlanapi.lib")
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

namespace tcard {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::IJsonValue;

struct RenderSnapshot {
    SYSTEMTIME time{};
    std::map<std::wstring, std::any> samples;
    std::map<std::wstring, std::wstring> rendered;
};
static thread_local RenderSnapshot* g_snapshot = nullptr;
struct RenderBatch::Impl {
    RenderSnapshot snapshot;
    RenderSnapshot* previous = nullptr;
};
RenderBatch::RenderBatch(const SYSTEMTIME& localTime) : impl_(std::make_unique<Impl>())
{
    impl_->snapshot.time = localTime;
    impl_->previous = g_snapshot;
    g_snapshot = &impl_->snapshot;
}
RenderBatch::~RenderBatch() { g_snapshot = impl_->previous; }

template<class Query>
static auto tcard_sample(const std::wstring& key, Query query) -> decltype(query())
{
    if (!g_snapshot) return query();
    auto found = g_snapshot->samples.find(key);
    if (found == g_snapshot->samples.end())
        found = g_snapshot->samples.emplace(key, query()).first;
    return std::any_cast<decltype(query())>(found->second);
}

static std::vector<BYTE> tcard_query_interfaces()
{
    return tcard_sample(L"interfaces", [] {
        ULONG size = 0;
        if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || !size)
            return std::vector<BYTE>{};
        std::vector<BYTE> bytes(size);
        DWORD result = GetIfTable(reinterpret_cast<PMIB_IFTABLE>(bytes.data()), &size, FALSE);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            bytes.resize(size);
            result = GetIfTable(reinterpret_cast<PMIB_IFTABLE>(bytes.data()), &size, FALSE);
        }
        if (result != NO_ERROR) bytes.clear();
        return bytes;
    });
}
static std::pair<BOOL, SYSTEM_POWER_STATUS> tcard_query_power()
{
    return tcard_sample(L"power", [] {
        SYSTEM_POWER_STATUS value{};
        const BOOL ok = GetSystemPowerStatus(&value);
        return std::make_pair(ok, value);
    });
}

static std::wstring g_tclockIniPath;

static std::wstring json_string(const JsonObject& object, const wchar_t* name, const wchar_t* fallback)
{
    if (!object.HasKey(name)) return fallback;
    auto value = object.GetNamedValue(name, nullptr);
    if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) return fallback;
    return value.GetString().c_str();
}

static bool json_bool(const JsonObject& object, const wchar_t* name, bool fallback)
{
    if (!object.HasKey(name)) return fallback;
    auto value = object.GetNamedValue(name, nullptr);
    if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Boolean) return fallback;
    return value.GetBoolean();
}

static double json_number(const JsonObject& object, const wchar_t* name, double fallback)
{
    if (!object.HasKey(name)) return fallback;
    auto value = object.GetNamedValue(name, nullptr);
    if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return fallback;
    const double number = value.GetNumber();
    return number == number ? number : fallback;
}

static std::wstring read_utf8(const std::wstring& path)
{
    if (tcard_storage::Required(path)) {
        const auto slash=path.find_last_of(L"\\/"); std::vector<BYTE> bytes;
        if(slash==std::wstring::npos||!tcard_asset::read(path.substr(0,slash),path.substr(slash+1),bytes))return {};
        return tcard_asset::decode(bytes);
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) return {};
    if (read >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    if (bytes.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring text(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), count);
    return text;
}

static bool write_utf8(const std::wstring& path, const std::wstring& text)
{
    if(tcard_storage::Required(path)){
        const auto slash=path.find_last_of(L"\\/");
        return slash!=std::wstring::npos&&tcard_asset::publish(path.substr(0,slash),path.substr(slash+1),tcard_asset::encode(text));
    }
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring directory = path.substr(0, slash);
        if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count < 0) return false;
    std::string bytes(static_cast<size_t>(count), '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(), count, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

static std::wstring locale_separator(LCTYPE type)
{
    return tcard_sample(L"locale:" + std::to_wstring(type), [&]() -> std::wstring {
        wchar_t buffer[16]{};
        const int length = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buffer, ARRAYSIZE(buffer));
        return length > 1 ? std::wstring(buffer, static_cast<size_t>(length - 1)) : std::wstring();
    });
}

static int provider_cpu_usage()
{
    return tcard_sample(L"cpu", [&]() -> int {
        FILETIME idle{}, kernel{}, user{};
        static ULONGLONG oldIdle = 0;
        static ULONGLONG oldTotal = 0;
        if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
        const ULONGLONG idleNow = (static_cast<ULONGLONG>(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
        const ULONGLONG totalNow = ((static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        if (oldTotal == 0 || totalNow <= oldTotal || idleNow < oldIdle) { oldTotal = totalNow; oldIdle = idleNow; return 0; }
        const ULONGLONG totalDelta = totalNow - oldTotal;
        const ULONGLONG idleDelta = idleNow - oldIdle;
        oldTotal = totalNow;
        oldIdle = idleNow;
        return static_cast<int>(((totalDelta > idleDelta ? totalDelta - idleDelta : 0) * 100ULL) / totalDelta);
    });
}

static std::wstring provider_ip_address()
{
    return tcard_sample(L"ip", [&]() -> std::wstring {
        ULONG size = 0;
        if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0) return L"N/A";
        std::vector<BYTE> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
        if (GetAdaptersInfo(adapters, &size) != NO_ERROR) return L"N/A";
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            const char* value = adapter->IpAddressList.IpAddress.String;
            if (!value || value[0] == '\0' || (value[0] == '0' && value[1] == '\0')) continue;
            std::wstring result;
            while (*value) result.push_back(static_cast<unsigned char>(*value++));
            return result;
        }
        return L"N/A";
    });
}

static std::wstring provider_number(ULONGLONG value, const std::wstring& body, size_t& index)
{
    const size_t begin = index;
    int width = 0;
    int spaces = 0;
    bool comma = false;
    while (index < body.size() && body[index] == L'_') { ++spaces; ++index; }
    while (index < body.size() && (body[index] == L'x' || body[index] == L',')) {
        if (body[index] == L',') comma = true;
        ++width;
        ++index;
    }
    if (width == 0) index = begin;
    std::wstring text = std::to_wstring(value);
    if (comma) {
        for (int position = static_cast<int>(text.size()) - 3; position > 0; position -= 3) text.insert(static_cast<size_t>(position), 1, L',');
    }
    if (width > static_cast<int>(text.size())) text.insert(0, static_cast<size_t>(width - text.size()), L'0');
    if (spaces > 0) text.insert(0, static_cast<size_t>(spaces), L' ');
    return text;
}

struct NetworkState
{
    bool wifiAdapter = false;
    bool wifiConnected = false;
    std::wstring ssid;
    bool ethernetAdapter = false;
    bool ethernetConnected = false;
    bool lteConnected = false;
    bool vpnConnected = false;
    ULONG wifiQuality = 0;
};

static std::wstring network_utf8_to_wide(const unsigned char* bytes, size_t length)
{
    if (!bytes || length == 0) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes), static_cast<int>(length), nullptr, 0);
    if (required <= 0) return {};
    std::wstring value(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes), static_cast<int>(length), value.data(), required);
    return value;
}

static NetworkState query_network_state()
{
    return tcard_sample(L"network", [&]() -> NetworkState {
        static ULONGLONG refreshedTick = 0;
        static NetworkState cached;
        const ULONGLONG now = GetTickCount64();
        if (refreshedTick != 0 && now - refreshedTick < 500) return cached;
        NetworkState state;
        HANDLE wlan = nullptr;
        DWORD negotiatedVersion = 0;
        if (WlanOpenHandle(2, nullptr, &negotiatedVersion, &wlan) == ERROR_SUCCESS) {
            PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
            if (WlanEnumInterfaces(wlan, nullptr, &interfaces) == ERROR_SUCCESS && interfaces) {
                for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
                    const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[i];
                    if (info.isState != wlan_interface_state_not_ready) state.wifiAdapter = true;
                    if (info.isState != wlan_interface_state_connected) continue;
                    state.wifiConnected = true;
                    DWORD dataSize = 0;
                    PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
                    if (WlanQueryInterface(wlan, &info.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &dataSize, reinterpret_cast<PVOID*>(&attributes), nullptr) == ERROR_SUCCESS && attributes) {
                        state.wifiQuality = attributes->wlanAssociationAttributes.wlanSignalQuality;
                        const DOT11_SSID& ssid = attributes->wlanAssociationAttributes.dot11Ssid;
                        state.ssid = network_utf8_to_wide(ssid.ucSSID, ssid.uSSIDLength);
                        WlanFreeMemory(attributes);
                    }
                }
                WlanFreeMemory(interfaces);
            }
            WlanCloseHandle(wlan, nullptr);
        }

        const auto bytes = tcard_query_interfaces();
        if (!bytes.empty()) {
            const auto* table = reinterpret_cast<const MIB_IFTABLE*>(bytes.data());
            {
                for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                    const MIB_IFROW& row = table->table[i];
                    const bool up = row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL;
                    if (row.dwType == MIB_IF_TYPE_ETHERNET) {
                        state.ethernetAdapter = true;
                        state.ethernetConnected = state.ethernetConnected || up;
                    } else if (row.dwType == IF_TYPE_WWANPP || row.dwType == IF_TYPE_WWANPP2) {
                        state.lteConnected = state.lteConnected || up;
                    }
                    if (up && (row.dwType == IF_TYPE_TUNNEL || row.dwType == IF_TYPE_PPP)) state.vpnConnected = true;
                }
            }
        }
        cached = state;
        refreshedTick = now;
        return cached;
    });
}

static std::wstring network_fixed_ssid(const std::wstring& ssid)
{
    int width = tcard_sample(L"ssid-width", [] { return static_cast<int>(GetPrivateProfileIntW(L"ETC", L"SSID_AP_Length", 10, g_tclockIniPath.c_str())); });
    width = std::clamp(width, 1, 64);
    std::wstring value = ssid.substr(0, static_cast<size_t>(width));
    if (value.size() < static_cast<size_t>(width)) value.append(static_cast<size_t>(width) - value.size(), L' ');
    return value;
}

struct NetworkTraffic
{
    ULONGLONG received = 0;
    ULONGLONG sent = 0;
    ULONGLONG receivedRate = 0;
    ULONGLONG sentRate = 0;
};

static NetworkTraffic query_network_traffic()
{
    return tcard_sample(L"traffic", [&]() -> NetworkTraffic {
        static ULONGLONG previousTick = 0;
        static ULONGLONG previousReceived = 0;
        static ULONGLONG previousSent = 0;
        static NetworkTraffic cached;
        const ULONGLONG now = GetTickCount64();
        if (previousTick != 0 && now - previousTick < 500) return cached;
        const auto buffer = tcard_query_interfaces();
        if (buffer.empty()) return cached;
        const auto* table = reinterpret_cast<const MIB_IFTABLE*>(buffer.data());
        ULONGLONG received = 0;
        ULONGLONG sent = 0;
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            received += table->table[i].dwInOctets;
            sent += table->table[i].dwOutOctets;
        }
        if (previousTick != 0 && now > previousTick) {
            const ULONGLONG elapsed = now - previousTick;
            cached.receivedRate = received >= previousReceived ? (received - previousReceived) * 1000ULL / elapsed : 0;
            cached.sentRate = sent >= previousSent ? (sent - previousSent) * 1000ULL / elapsed : 0;
        }
        cached.received = received;
        cached.sent = sent;
        previousTick = now;
        previousReceived = received;
        previousSent = sent;
        return cached;
    });
}

static std::wstring network_auto_label(ULONGLONG bytes, bool rate = false)
{
    const double kb = static_cast<double>(bytes) / 1000.0;
    const double mb = kb / 1000.0;
    const double gb = mb / 1000.0;
    wchar_t value[32]{};
    if (rate && bytes < 1000ULL) swprintf_s(value, L"%4.0fB", static_cast<double>(bytes));
    else if (rate && kb < 10.0) swprintf_s(value, L"%1.2fkB", kb);
    else if (rate && kb < 100.0) swprintf_s(value, L"%2.1fkB", kb);
    else if (kb < 1000.0) swprintf_s(value, L"%4.0fkB", kb);
    else if (mb < 10.0) swprintf_s(value, L"%1.2fMB", mb);
    else if (mb < 100.0) swprintf_s(value, L"%2.1fMB", mb);
    else if (mb < 1000.0) swprintf_s(value, L"%4.0fMB", mb);
    else if (gb < 10.0) swprintf_s(value, L"%1.2fGB", gb);
    else if (gb < 100.0) swprintf_s(value, L"%2.1fGB", gb);
    else swprintf_s(value, L"%4.0fGB", gb);
    return rate ? std::wstring(value) + L"/s" : std::wstring(value);
}

static bool network_token(const std::wstring& body, size_t& index, std::wstring& result)
{
    const size_t begin = index;
    const bool candidate = body.compare(index, 4, L"SSID") == 0 || body.compare(index, 4, L"WiFi") == 0 || body.compare(index, 4, L"EthS") == 0 ||
        body.compare(index, 4, L"EthL") == 0 || body.compare(index, 4, L"EWLL") == 0 || body.compare(index, 4, L"EWLS") == 0 ||
        body.compare(index, 3, L"ICP") == 0 || body.compare(index, 3, L"LTE") == 0 || body.compare(index, 4, L"VPNS") == 0 ||
        body.compare(index, 4, L"WANP") == 0 || body.compare(index, 3, L"APN") == 0 || body.compare(index, 4, L"NMX1") == 0 ||
        body.compare(index, 4, L"NMX2") == 0 || body.compare(index, 4, L"NRAA") == 0 || body.compare(index, 4, L"NSAA") == 0 ||
        (index + 3 < body.size() && (body[index] == L'N' || body[index] == L'n') && (body[index + 1] == L'R' || body[index + 1] == L'S') &&
         (body[index + 2] == L'A' || body[index + 2] == L'S'));
    if (!candidate) return false;
    const bool trafficToken = body.compare(index, 4, L"NRAA") == 0 || body.compare(index, 4, L"NSAA") == 0 ||
        (index + 3 < body.size() && (body[index] == L'N' || body[index] == L'n') &&
         (body[index + 1] == L'R' || body[index + 1] == L'S') &&
         (body[index + 2] == L'A' || body[index + 2] == L'S'));
    const NetworkState state = trafficToken ? NetworkState{} : query_network_state();
    if (body.compare(index, 4, L"SSID") == 0) { index += 4; result = network_fixed_ssid(state.wifiConnected ? state.ssid : L""); return true; }
    if (body.compare(index, 4, L"WiFi") == 0) { index += 4; result = state.wifiConnected ? L"WiFi*" : state.wifiAdapter ? L"WiFi " : L"     "; return true; }
    if (body.compare(index, 4, L"EthS") == 0) { index += 4; result = state.ethernetConnected ? L"Eth " : state.ethernetAdapter ? L"Eth " : L"    "; return true; }
    if (body.compare(index, 4, L"EthL") == 0) { index += 4; result = state.ethernetConnected ? L"Ethernet " : state.ethernetAdapter ? L"Ethernet " : L"         "; return true; }
    if (body.compare(index, 4, L"EWLL") == 0 || body.compare(index, 4, L"EWLS") == 0) {
        const bool compact = body.compare(index, 4, L"EWLS") == 0;
        result = state.ethernetConnected ? L"E*" : L"E ";
        if (!compact) result += L' ';
        result += state.wifiConnected ? L"W*" : state.wifiAdapter ? L"W " : L"  ";
        if (!compact) result += L' ';
        result += state.lteConnected ? L"L*" : L"  ";
        index += 4;
        return true;
    }
    if (body.compare(index, 3, L"ICP") == 0) { index += 3; result = state.wifiConnected ? L"W" : state.ethernetConnected ? L"E" : state.lteConnected ? L"L" : L"-"; return true; }
    if (body.compare(index, 3, L"LTE") == 0) { index += 3; result = state.lteConnected ? L"LTE*" : L"    "; return true; }
    if (body.compare(index, 4, L"VPNS") == 0) { index += 4; result = state.vpnConnected ? L"VPN" : L"   "; return true; }
    if (body.compare(index, 4, L"WANP") == 0) { index += 4; result = L"N/A"; return true; }
    if (body.compare(index, 3, L"APN") == 0) { index += 3; result = L"          "; return true; }
    if (body.compare(index, 4, L"NMX1") == 0 || body.compare(index, 4, L"NMX2") == 0) {
        index += 4;
        result = state.wifiConnected ? network_fixed_ssid(state.ssid) : state.ethernetConnected ? L"Ethernet" : state.lteConnected ? L"APN: N/A" : L"";
        return true;
    }
    if (body.compare(index, 4, L"NRSA") == 0 || body.compare(index, 4, L"NSSA") == 0) {
        const bool sent = body[index + 1] == L'S';
        index += 4;
        const NetworkTraffic traffic = query_network_traffic();
        result = network_auto_label(sent ? traffic.sentRate : traffic.receivedRate, true);
        return true;
    }
    if (body.compare(index, 4, L"NRAA") == 0 || body.compare(index, 4, L"NSAA") == 0) {
        const bool sent = body[index + 1] == L'S';
        index += 4;
        const NetworkTraffic traffic = query_network_traffic();
        result = network_auto_label(sent ? traffic.sent : traffic.received);
        return true;
    }
    if (index + 3 < body.size() && (body[index] == L'N' || body[index] == L'n') && (body[index + 1] == L'R' || body[index + 1] == L'S') && (body[index + 2] == L'A' || body[index + 2] == L'S')) {
        const bool sent = body[index + 1] == L'S';
        const bool rate = body[index + 2] == L'S';
        const wchar_t unit = body[index + 3];
        if (unit != L'B' && unit != L'K' && unit != L'M' && unit != L'G') { index = begin; return false; }
        index += 4;
        const NetworkTraffic traffic = query_network_traffic();
        const ULONGLONG bytes = rate ? (sent ? traffic.sentRate : traffic.receivedRate) : (sent ? traffic.sent : traffic.received);
        const ULONGLONG value = unit == L'B' ? bytes : unit == L'K' ? bytes / 1000ULL : unit == L'M' ? bytes / (1000ULL * 1000ULL) : bytes / (1000ULL * 1000ULL * 1000ULL);
        result = provider_number(value, body, index);
        return true;
    }
    index = begin;
    return false;
}

static bool provider_token(const std::wstring& body, size_t& index, std::wstring& result)
{
    const size_t begin = index;
    if (network_token(body, index, result)) return true;
    if (index + 3 < body.size() && body[index] == L'H' && (body[index + 1] == L'A' || body[index + 1] == L'U' || body[index + 1] == L'T') && body[index + 2] >= L'A' && body[index + 2] <= L'Z' && (body[index + 3] == L'M' || body[index + 3] == L'G' || body[index + 3] == L'T' || body[index + 3] == L'P')) {
        const wchar_t mode = body[index + 1];
        const wchar_t unit = body[index + 3];
        wchar_t root[] = { body[index + 2], L':', L'\\', L'\0' };
        struct DiskSample { BOOL ok; ULARGE_INTEGER available{}, total{}, freeBytes{}; };
        const auto disk = tcard_sample(std::wstring(L"disk:") + root, [&] {
            DiskSample value{};
            value.ok = GetDiskFreeSpaceExW(root, &value.available, &value.total, &value.freeBytes);
            return value;
        });
        const auto total = disk.total, freeBytes = disk.freeBytes;
        index += 4;
        if (!disk.ok) { result = L"0"; return true; }
        const ULONGLONG totalBytes = total.QuadPart;
        const ULONGLONG freeValue = unit == L'M' ? freeBytes.QuadPart / (1024ULL * 1024ULL) : unit == L'G' ? freeBytes.QuadPart / (1024ULL * 1024ULL * 1024ULL) : unit == L'T' ? freeBytes.QuadPart / (1024ULL * 1024ULL * 1024ULL * 1024ULL) : totalBytes ? freeBytes.QuadPart * 100ULL / totalBytes : 0ULL;
        const ULONGLONG usedBytes = totalBytes > freeBytes.QuadPart ? totalBytes - freeBytes.QuadPart : 0ULL;
        const ULONGLONG usedValue = unit == L'M' ? usedBytes / (1024ULL * 1024ULL) : unit == L'G' ? usedBytes / (1024ULL * 1024ULL * 1024ULL) : unit == L'T' ? usedBytes / (1024ULL * 1024ULL * 1024ULL * 1024ULL) : totalBytes ? usedBytes * 100ULL / totalBytes : 0ULL;
        const ULONGLONG totalValue = unit == L'M' ? totalBytes / (1024ULL * 1024ULL) : unit == L'G' ? totalBytes / (1024ULL * 1024ULL * 1024ULL) : unit == L'T' ? totalBytes / (1024ULL * 1024ULL * 1024ULL * 1024ULL) : 0ULL;
        result = provider_number(mode == L'A' ? freeValue : mode == L'U' ? usedValue : totalValue, body, index);
        return true;
    }
    if (body.compare(index, 2, L"IP") == 0 && index + 2 < body.size() && (body[index + 2] == L'E' || body[index + 2] == L'W' || body[index + 2] == L'L' || body[index + 2] == L'V' || body[index + 2] == L'A')) {
        index += 3;
        result = provider_ip_address();
        return true;
    }
    if (body.compare(index, 2, L"ST") == 0) {
        index += 2;
        const ULONGLONG seconds = tcard_sample(L"uptime", [] { return GetTickCount64(); }) / 1000ULL;
        result = std::to_wstring((seconds / 3600ULL) % 100ULL) + L":" +
            (seconds / 60ULL % 60ULL < 10 ? L"0" : L"") + std::to_wstring((seconds / 60ULL) % 60ULL) + L":" +
            (seconds % 60ULL < 10 ? L"0" : L"") + std::to_wstring(seconds % 60ULL);
        return true;
    }
    if (body.compare(index, 2, L"Sd") == 0 || body.compare(index, 2, L"Sa") == 0 ||
        body.compare(index, 2, L"Sh") == 0 || body.compare(index, 2, L"Sn") == 0 || body.compare(index, 2, L"Ss") == 0) {
        const wchar_t unit = body[index + 1];
        index += 2;
        const ULONGLONG ticks = tcard_sample(L"uptime", [] { return GetTickCount64(); });
        ULONGLONG value = unit == L'd' ? ticks / 86400000ULL : unit == L'a' ? ticks / 3600000ULL :
            unit == L'h' ? ticks / 3600000ULL % 24ULL : unit == L'n' ? ticks / 60000ULL % 60ULL : ticks / 1000ULL % 60ULL;
        result = provider_number(value, body, index);
        return true;
    }
    if (body.compare(index, 2, L"CU") == 0 || body.compare(index, 2, L"CC") == 0) {
        index += 2;
        while (index < body.size() && (iswdigit(body[index]) || body[index] == L'e')) ++index;
        result = provider_number(static_cast<ULONGLONG>(provider_cpu_usage()), body, index);
        return true;
    }
    if (body.compare(index, 5, L"PCORE") == 0 || body.compare(index, 5, L"LPROC") == 0) {
        index += 5;
        const auto info = tcard_sample(L"system-info", [] {
            SYSTEM_INFO value{};
            GetSystemInfo(&value);
            return value;
        });
        result = provider_number(info.dwNumberOfProcessors, body, index);
        return true;
    }
    if (body.compare(index, 2, L"BL") == 0) {
        index += 2;
        const auto sample = tcard_query_power();
        const auto& power = sample.second;
        const DWORD level = sample.first && power.BatteryLifePercent != 255 ? power.BatteryLifePercent : 0;
        result = provider_number(level, body, index);
        return true;
    }
    if (body.compare(index, 2, L"AD") == 0 || body.compare(index, 2, L"ad") == 0) {
        const bool upper = body[index] == L'A';
        index += 2;
        const auto sample = tcard_query_power();
        const auto& power = sample.second;
        const bool online = sample.first && power.ACLineStatus == 1;
        result = upper ? (online ? L"AC" : L"DC") : (online ? L"A" : L"D");
        return true;
    }
    if (body.compare(index, 3, L"BCS") == 0) {
        index += 3;
        const auto sample = tcard_query_power();
        const auto& power = sample.second;
        result = sample.first && power.BatteryFlag != 128 && power.BatteryFlag != 255 ? L"*" : L" ";
        return true;
    }
    if (index + 3 < body.size() && body.compare(index, 3, L"MAP") == 0 && (body[index + 3] == L'K' || body[index + 3] == L'M' || body[index + 3] == L'P' || body[index + 3] == L'G')) {
        const wchar_t unit = body[index + 3];
        index += 4;
        const auto sample = tcard_sample(L"memory", [] {
            MEMORYSTATUSEX value{};
            value.dwLength = sizeof(value);
            const BOOL ok = GlobalMemoryStatusEx(&value);
            return std::make_pair(ok, value);
        });
        const auto& memory = sample.second;
        if (!sample.first) { index = begin; return false; }
        const ULONGLONG value = unit == L'K' ? memory.ullAvailPhys / 1024ULL : unit == L'M' ? memory.ullAvailPhys / (1024ULL * 1024ULL) : unit == L'P' ? memory.ullAvailPhys * 100ULL / (memory.ullTotalPhys ? memory.ullTotalPhys : 1ULL) : memory.ullAvailPhys / (1024ULL * 1024ULL * 1024ULL);
        result = std::to_wstring(value);
        return true;
    }
    index = begin;
    return false;
}

struct CustomVarState
{
    std::wstring path;
    std::wstring value;
    std::wstring failValue = L"N/A";
    int refreshSeconds = 60;
    ULONGLONG nextRefreshTick = 0;
    int maxChars = 4096;
    bool keepWhitespace = false;
    bool jsonMode = false;
    bool jsonStringify = false;
    bool jsonNullAsEmpty = false;
    std::wstring jsonDefault;
    std::wstring jsonValue;
};

static std::vector<CustomVarState> g_customVars(32);

static std::wstring trim_custom_text(std::wstring value)
{
    const auto is_space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000; };
    size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

static bool custom_json_path_value(IJsonValue root, std::wstring_view path, IJsonValue& value)
{
    if (path.empty() || path.front() != L'$') return false;
    size_t cursor = 1;
    IJsonValue current = root;
    try {
        while (cursor < path.size()) {
            if (path[cursor] == L'.') {
                ++cursor;
                const size_t start = cursor;
                while (cursor < path.size() && path[cursor] != L'.' && path[cursor] != L'[') ++cursor;
                if (start == cursor) return false;
                const std::wstring segment(path.substr(start, cursor - start));
                if (current.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
                    auto object = current.GetObject();
                    if (!object.HasKey(winrt::hstring(segment))) return false;
                    current = object.GetNamedValue(winrt::hstring(segment), nullptr);
                } else if (current.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array && std::all_of(segment.begin(), segment.end(), iswdigit)) {
                    const size_t index = static_cast<size_t>(std::stoull(segment));
                    auto array = current.GetArray();
                    if (index >= array.Size()) return false;
                    current = array.GetAt(static_cast<unsigned>(index));
                } else {
                    return false;
                }
                continue;
            }
            if (path[cursor] == L'[') {
                ++cursor;
                const size_t start = cursor;
                while (cursor < path.size() && iswdigit(path[cursor])) ++cursor;
                if (start == cursor || cursor >= path.size() || path[cursor] != L']') return false;
                const size_t index = static_cast<size_t>(std::stoull(std::wstring(path.substr(start, cursor - start))));
                ++cursor;
                if (current.ValueType() != winrt::Windows::Data::Json::JsonValueType::Array) return false;
                auto array = current.GetArray();
                if (index >= array.Size()) return false;
                current = array.GetAt(static_cast<unsigned>(index));
                continue;
            }
            return false;
        }
    } catch (...) {
        return false;
    }
    value = current;
    return true;
}

static bool custom_json_scalar_text(const IJsonValue& value, bool stringify, bool nullAsEmpty, std::wstring& output)
{
    try {
        switch (value.ValueType()) {
        case winrt::Windows::Data::Json::JsonValueType::String:
            output = value.GetString().c_str();
            return true;
        case winrt::Windows::Data::Json::JsonValueType::Number:
        case winrt::Windows::Data::Json::JsonValueType::Boolean:
            output = value.Stringify().c_str();
            return true;
        case winrt::Windows::Data::Json::JsonValueType::Null:
            if (nullAsEmpty) { output.clear(); return true; }
            return false;
        case winrt::Windows::Data::Json::JsonValueType::Object:
        case winrt::Windows::Data::Json::JsonValueType::Array:
            if (!stringify) return false;
            output = value.Stringify().c_str();
            return true;
        default:
            return false;
        }
    } catch (...) {
        return false;
    }
}

static bool read_custom_json(const CustomVarState& variable, const std::wstring& json, std::wstring& output)
{
    try {
        JsonValue root = JsonValue::Parse(winrt::hstring(json));
        if (variable.jsonValue.empty()) {
            if (!variable.jsonStringify) return false;
            return custom_json_scalar_text(root, true, variable.jsonNullAsEmpty, output);
        }
        size_t cursor = 0;
        output.clear();
        while (cursor < variable.jsonValue.size()) {
            if (variable.jsonValue[cursor] == L'{' && cursor + 1 < variable.jsonValue.size() && variable.jsonValue[cursor + 1] == L'{') {
                output.push_back(L'{');
                cursor += 2;
                continue;
            }
            if (variable.jsonValue[cursor] == L'}' && cursor + 1 < variable.jsonValue.size() && variable.jsonValue[cursor + 1] == L'}') {
                output.push_back(L'}');
                cursor += 2;
                continue;
            }
            if (variable.jsonValue[cursor] == L'{') {
                const size_t close = variable.jsonValue.find(L'}', cursor + 1);
                if (close == std::wstring::npos) return false;
                const std::wstring path = trim_custom_text(variable.jsonValue.substr(cursor + 1, close - cursor - 1));
                IJsonValue target{ nullptr };
                if (!custom_json_path_value(root, path, target)) return false;
                std::wstring value;
                if (!custom_json_scalar_text(target, variable.jsonStringify, variable.jsonNullAsEmpty, value)) return false;
                output += value;
                cursor = close + 1;
                continue;
            }
            if (variable.jsonValue[cursor] == L'}') return false;
            const size_t next = variable.jsonValue.find_first_of(L"{}", cursor);
            output.append(variable.jsonValue, cursor, next == std::wstring::npos ? std::wstring::npos : next - cursor);
            cursor = next == std::wstring::npos ? variable.jsonValue.size() : next;
        }
        return true;
    } catch (...) {
        return false;
    }
}

static std::wstring read_custom_text(const std::wstring& path, bool trim = true)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024) { CloseHandle(file); return {}; }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return {};
    bytes.resize(read);
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xfe) {
        const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        const std::wstring value(wide, (bytes.size() - 2) / sizeof(wchar_t));
        return trim ? trim_custom_text(value) : value;
    }
    size_t start = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf ? 3 : 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + start, static_cast<int>(bytes.size() - start), nullptr, 0);
    UINT codePage = CP_UTF8;
    // Legacy CustomVars file-content compatibility only; file paths remain UTF-16/W APIs.
    if (count <= 0) { codePage = CP_ACP; count = MultiByteToWideChar(codePage, 0, bytes.data() + start, static_cast<int>(bytes.size() - start), nullptr, 0); }
    if (count <= 0) return {};
    std::wstring value(static_cast<size_t>(count), L'\0');
    // Decode the selected UTF-8/legacy content encoding; never convert a path or command.
    MultiByteToWideChar(codePage, 0, bytes.data() + start, static_cast<int>(bytes.size() - start), value.data(), count);
    return trim ? trim_custom_text(value) : value;
}

static std::wstring custom_setting(const std::wstring& iniPath, const wchar_t* key)
{
    wchar_t value[4096]{};
    const DWORD length = GetPrivateProfileStringW(L"CustomVars", key, L"", value, ARRAYSIZE(value), iniPath.c_str());
    return length ? std::wstring(value, length) : std::wstring();
}

static std::wstring custom_resolve_path(const std::wstring& iniPath, std::wstring value)
{
    value = trim_custom_text(value);
    if (value.empty()) return {};
    if ((value.size() > 1 && value[1] == L':') || value.rfind(L"\\", 0) == 0 || value.rfind(L"//", 0) == 0) return value;
    const size_t slash = iniPath.find_last_of(L"\\/");
    return slash == std::wstring::npos ? value : iniPath.substr(0, slash + 1) + value;
}

bool ConfigureCustomVariables(const std::wstring& iniPath)
{
    g_tclockIniPath = iniPath;
    const std::wstring globalFail = custom_setting(iniPath, L"FailValue");
    const int globalRefreshSeconds = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"CustomVars", L"RefreshSec", 60, iniPath.c_str())), 1, 86400);
    const int globalMaxChars = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"CustomVars", L"MaxChars", 4096, iniPath.c_str())), 1, 4096);
    const std::wstring globalWhitespace = custom_setting(iniPath, L"Whitespace");
    for (size_t i = 0; i < g_customVars.size(); ++i) {
        auto& variable = g_customVars[i];
        wchar_t key[64]{};
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uPath", static_cast<unsigned>(i + 1));
        variable.path = custom_resolve_path(iniPath, custom_setting(iniPath, key));
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uMode", static_cast<unsigned>(i + 1));
        variable.jsonMode = _wcsicmp(custom_setting(iniPath, key).c_str(), L"json") == 0;
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uFailValue", static_cast<unsigned>(i + 1));
        variable.failValue = custom_setting(iniPath, key);
        if (variable.failValue.empty()) variable.failValue = globalFail.empty() ? L"N/A" : globalFail;
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uRefreshSec", static_cast<unsigned>(i + 1));
        variable.refreshSeconds = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"CustomVars", key, static_cast<UINT>(globalRefreshSeconds), iniPath.c_str())), 1, 86400);
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uMaxChars", static_cast<unsigned>(i + 1));
        variable.maxChars = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"CustomVars", key, static_cast<UINT>(globalMaxChars), iniPath.c_str())), 1, 4096);
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uWhitespace", static_cast<unsigned>(i + 1));
        const std::wstring whitespace = custom_setting(iniPath, key);
        variable.keepWhitespace = _wcsicmp((whitespace.empty() ? globalWhitespace : whitespace).c_str(), L"keep") == 0;
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uJsonStringify", static_cast<unsigned>(i + 1));
        variable.jsonStringify = GetPrivateProfileIntW(L"CustomVars", key, 0, iniPath.c_str()) != 0;
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uJsonNullAsEmpty", static_cast<unsigned>(i + 1));
        variable.jsonNullAsEmpty = GetPrivateProfileIntW(L"CustomVars", key, 0, iniPath.c_str()) != 0;
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uJsonDefault", static_cast<unsigned>(i + 1));
        variable.jsonDefault = custom_setting(iniPath, key);
        swprintf_s(key, ARRAYSIZE(key), L"Custom%uJsonValue", static_cast<unsigned>(i + 1));
        variable.jsonValue = custom_setting(iniPath, key);
        variable.value.clear();
        variable.nextRefreshTick = 0;
    }
    RefreshCustomVariables();
    return true;
}

void RefreshCustomVariables()
{
    const ULONGLONG now = GetTickCount64();
    for (auto& variable : g_customVars) {
        if (variable.nextRefreshTick != 0 && now < variable.nextRefreshTick) continue;
        if (variable.path.empty()) {
            variable.value.clear();
            variable.nextRefreshTick = now + static_cast<ULONGLONG>(variable.refreshSeconds) * 1000ULL;
            continue;
        }
        if (!variable.jsonMode) {
            std::wstring text = tcard_sample(L"custom:" + variable.path, [&] { return read_custom_text(variable.path, false); });
            const size_t lineEnd = text.find_first_of(L"\r\n");
            if (lineEnd != std::wstring::npos) text.resize(lineEnd);
            variable.value = variable.keepWhitespace ? text : trim_custom_text(text);
            if (variable.value.empty()) variable.value = variable.failValue;
            if (variable.value.size() > static_cast<size_t>(variable.maxChars)) variable.value.resize(static_cast<size_t>(variable.maxChars));
            variable.nextRefreshTick = now + static_cast<ULONGLONG>(variable.refreshSeconds) * 1000ULL;
            continue;
        }
        const std::wstring json = tcard_sample(L"custom:" + variable.path, [&] { return read_custom_text(variable.path, false); });
        std::wstring extracted;
        if (json.empty() || !read_custom_json(variable, json, extracted)) {
            variable.value = variable.jsonDefault.empty() ? variable.failValue : variable.jsonDefault;
        } else {
            variable.value = variable.keepWhitespace ? extracted : trim_custom_text(extracted);
        }
        if (variable.value.size() > static_cast<size_t>(variable.maxChars)) variable.value.resize(static_cast<size_t>(variable.maxChars));
        variable.nextRefreshTick = now + static_cast<ULONGLONG>(variable.refreshSeconds) * 1000ULL;
    }
}

static bool locale_token(const std::wstring& token, const SYSTEMTIME& time, std::wstring& result)
{
    wchar_t buffer[128]{};
    if (token == L"AMPM" || token == L"tt") {
        const int length = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, L"tt", buffer, ARRAYSIZE(buffer));
        if (length <= 1) return false;
        result.assign(buffer, static_cast<size_t>(length - 1));
        return true;
    }
    DWORD flags = 0;
    const wchar_t* format = nullptr;
    std::wstring formatStorage;
    if (token == L"DATE") flags = DATE_SHORTDATE;
    else if (token == L"LDATE") flags = DATE_LONGDATE;
    else if (token == L"TIME") return GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, nullptr, buffer, ARRAYSIZE(buffer)) > 1 && (result.assign(buffer), true);
    else if (token == L"mmm" || token == L"mme") format = L"MMM";
    else if (token == L"mmmm") format = L"MMMM";
    else if (token == L"ddd" || token == L"dde" || token == L"aaa") format = L"ddd";
    else if (token == L"dddd" || token == L"aaaa") format = L"dddd";
    else if (token == L"g" || token == L"gg") format = token.c_str();
    else if (!token.empty() && token.front() == L'Y') { formatStorage.assign(token.size(), L'y'); format = formatStorage.c_str(); }
    else return false;
    const int length = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &time, format, buffer, ARRAYSIZE(buffer), nullptr);
    if (length <= 1) return false;
    result.assign(buffer, static_cast<size_t>(length - 1));
    return true;
}

static std::wstring token_value(const std::wstring& token, const SYSTEMTIME& time)
{
    wchar_t value[32]{};
    if (token == L"y") { swprintf_s(value, L"%u", time.wYear % 10); return value; }
    if (token == L"yy") { swprintf_s(value, L"%02u", time.wYear % 100); return value; }
    if (token == L"yyyy") { swprintf_s(value, L"%04u", time.wYear); return value; }
    if (token == L"m") { swprintf_s(value, L"%u", time.wMonth); return value; }
    if (token == L"mm") { swprintf_s(value, L"%02u", time.wMonth); return value; }
    if (token == L"d") { swprintf_s(value, L"%u", time.wDay); return value; }
    if (token == L"dd") { swprintf_s(value, L"%02u", time.wDay); return value; }
    if (token == L"h") { swprintf_s(value, L"%u", time.wHour); return value; }
    if (token == L"hh") { swprintf_s(value, L"%02u", time.wHour); return value; }
    if (token == L"n") { swprintf_s(value, L"%u", time.wMinute); return value; }
    if (token == L"nn") { swprintf_s(value, L"%02u", time.wMinute); return value; }
    if (token == L"s") { swprintf_s(value, L"%u", time.wSecond); return value; }
    if (token == L"ss") { swprintf_s(value, L"%02u", time.wSecond); return value; }
    if (token == L"AM/PM") return time.wHour < 12 ? L"AM" : L"PM";
    if (token == L"am/pm") return time.wHour < 12 ? L"am" : L"pm";
    return {};
}

static bool shift_time(const SYSTEMTIME& input, int minutes, SYSTEMTIME& output)
{
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&input, &fileTime)) return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    const LONGLONG shifted = static_cast<LONGLONG>(ticks.QuadPart) + static_cast<LONGLONG>(minutes) * 600000000;
    if (shifted < 0) return false;
    ticks.QuadPart = static_cast<ULONGLONG>(shifted);
    fileTime.dwLowDateTime = ticks.LowPart;
    fileTime.dwHighDateTime = ticks.HighPart;
    return FileTimeToSystemTime(&fileTime, &output) != FALSE;
}

static bool parse_fixed_offset(const std::wstring& body, int& minutes)
{
    minutes = 0;
    if (body.size() < 8 || (body[0] != L't' && body[0] != L'T') || (body[1] != L'd' && body[1] != L'D') ||
        (body[2] != L'+' && body[2] != L'-') || body[5] != L':') return false;
    if (!iswdigit(body[3]) || !iswdigit(body[4]) || !iswdigit(body[6]) || !iswdigit(body[7])) return false;
    const int hours = (body[3] - L'0') * 10 + body[4] - L'0';
    const int mins = (body[6] - L'0') * 10 + body[7] - L'0';
    if (hours > 23 || mins > 59) return false;
    minutes = (hours * 60 + mins) * (body[2] == L'-' ? -1 : 1);
    return true;
}

static bool parse_hour_offset(const std::wstring& body, int& minutes)
{
    minutes = 0;
    if (body.size() < 4 || (body[0] != L'w' && body[0] != L'W') || (body[1] != L'+' && body[1] != L'-') ||
        !iswdigit(body[2]) || !iswdigit(body[3])) return false;
    const int hours = (body[2] - L'0') * 10 + body[3] - L'0';
    if (hours > 23) return false;
    minutes = hours * 60 * (body[1] == L'-' ? -1 : 1);
    return true;
}

std::wstring RenderMarkdown(const std::wstring& source)
{
    std::wstring output;
    size_t lineStart = 0;
    while (lineStart <= source.size()) {
        const size_t lineEnd = source.find_first_of(L"\r\n", lineStart);
        std::wstring line = source.substr(lineStart, lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - lineStart);
        size_t heading = 0;
        while (heading < line.size() && line[heading] == L'#') ++heading;
        if (heading && heading < line.size() && line[heading] == L' ') line.erase(0, heading + 1);
        else if (line.rfind(L"- ", 0) == 0 || line.rfind(L"* ", 0) == 0) line.replace(0, 2, L"• ");
        else if (line.rfind(L"> ", 0) == 0) line.replace(0, 2, L"│ ");
        std::wstring clean;
        for (size_t i = 0; i < line.size();) {
            if (i + 1 < line.size() && ((line[i] == L'*' && line[i + 1] == L'*') || (line[i] == L'_' && line[i + 1] == L'_') || (line[i] == L'~' && line[i + 1] == L'~'))) { i += 2; continue; }
            if (line[i] == L'*' || line[i] == L'_' || line[i] == L'\x60') { ++i; continue; }
            if (line[i] == L'[') { const size_t close = line.find(L']', i + 1); const size_t open = close == std::wstring::npos ? std::wstring::npos : line.find(L'(', close + 1); const size_t end = open == std::wstring::npos ? std::wstring::npos : line.find(L')', open + 1); if (end != std::wstring::npos) { clean.append(line, i + 1, close - i - 1); i = end + 1; continue; } }
            clean.push_back(line[i++]);
        }
        output += clean;
        if (lineEnd == std::wstring::npos) break;
        output += L"\r\n";
        lineStart = lineEnd + 1;
        if (lineStart < source.size() && source[lineEnd] == L'\r' && source[lineStart] == L'\n') ++lineStart;
    }
    return output;
}

static std::wstring tcard_render_source(const std::wstring& source, const SYSTEMTIME& localTime)
{
    std::wstring output;
    size_t offset = 0;
    while (offset < source.size()) {
        size_t start = source.find(L"<%", offset);
        if (start == std::wstring::npos) { output.append(source, offset, std::wstring::npos); break; }
        output.append(source, offset, start - offset);
        size_t end = source.find(L"%>", start + 2);
        if (end == std::wstring::npos) { output.append(source, start, std::wstring::npos); break; }
        std::wstring body = source.substr(start + 2, end - start - 2);
        std::wstring formatted;
        SYSTEMTIME displayTime = localTime;
        int offsetMinutes = 0;
        size_t tokenStart = 0;
        if (parse_fixed_offset(body, offsetMinutes)) tokenStart = 8;
        else if (parse_hour_offset(body, offsetMinutes)) tokenStart = 4;
        if (tokenStart && !shift_time(localTime, offsetMinutes, displayTime)) tokenStart = 0;
        bool valid = !body.empty() && tokenStart <= body.size();
        for (size_t i = tokenStart; valid && i < body.size();) {
            if (body[i] == L'\\' && i + 1 < body.size() && body[i + 1] == L'n') { formatted.append(L"\r\n"); i += 2; continue; }
            if (body[i] == L'"') {
                const size_t close = body.find(L'"', i + 1);
                if (close == std::wstring::npos) { valid = false; break; }
                formatted.append(body, i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
            if (_wcsnicmp(body.c_str() + i, L"CUSTOM", 6) == 0) {
                size_t cursor = i + 6;
                unsigned number = 0;
                while (cursor < body.size() && body[cursor] >= L'0' && body[cursor] <= L'9') {
                    number = number * 10U + static_cast<unsigned>(body[cursor] - L'0');
                    ++cursor;
                    if (number > g_customVars.size()) break;
                }
                if (cursor == i + 6 || number == 0 || number > g_customVars.size() || (cursor < body.size() && (iswalnum(body[cursor]) || body[cursor] == L'_'))) { valid = false; break; }
                formatted += g_customVars[number - 1].value;
                i = cursor;
                continue;
            }
            std::wstring providerValue;
            if (provider_token(body, i, providerValue)) { formatted += providerValue; continue; }
            if (body[i] == L'Y' || body[i] == L'g') {
                const wchar_t marker = body[i];
                const size_t begin = i;
                while (i < body.size() && body[i] == marker) ++i;
                const std::wstring token = body.substr(begin, i - begin);
                std::wstring value;
                if (!locale_token(token, displayTime, value)) { valid = false; break; }
                formatted += value;
                continue;
            }
            static const wchar_t* tokens[] = { L"AM/PM", L"am/pm", L"LDATE", L"DATE", L"TIME", L"AMPM", L"mmmm", L"dddd", L"yyyy", L"mmm", L"mme", L"ddd", L"dde", L"aaa", L"aaaa", L"yy", L"mm", L"dd", L"hh", L"nn", L"ss", L"tt", L"m", L"d", L"h", L"n", L"s" };
            const wchar_t* matched = nullptr;
            for (const wchar_t* token : tokens) {
                size_t length = wcslen(token);
                if (i + length <= body.size() && _wcsnicmp(body.c_str() + i, token, length) == 0 && (!matched || length > wcslen(matched))) matched = token;
            }
            if (matched) {
                if (wcsncmp(body.c_str() + i, L"am/pm", 5) == 0) matched = L"am/pm";
                std::wstring value = token_value(matched, displayTime);
                if (value.empty() && !locale_token(matched, displayTime, value)) { valid = false; break; }
                formatted += value;
                i += wcslen(matched);
                continue;
            }
            if (iswalpha(body[i])) { valid = false; break; }
            if (body[i] == L'/') {
                const std::wstring separator = locale_separator(LOCALE_SDATE);
                formatted += separator.empty() ? L"/" : separator;
            } else if (body[i] == L':') {
                const std::wstring separator = locale_separator(LOCALE_STIME);
                formatted += separator.empty() ? L":" : separator;
            } else formatted.push_back(body[i]);
            ++i;
        }
        if (valid) output += formatted;
        else output.append(source, start, end + 2 - start);
        offset = end + 2;
    }
    return output;
}

std::wstring Render(const std::wstring& source, const SYSTEMTIME& localTime)
{
    if (!g_snapshot) {
        RenderBatch batch(localTime);
        return Render(source, localTime);
    }
    auto found = g_snapshot->rendered.find(source);
    if (found == g_snapshot->rendered.end())
        found = g_snapshot->rendered.emplace(source, tcard_render_source(source, g_snapshot->time)).first;
    return found->second;
}

static std::wstring storage_directory(const std::wstring& path)
{
    // Saved bundle/history paths can exceed MAX_PATH in dedicated acquisition namespaces.
    return tcard_native::Extended(tcard_native::Resolve(path));
}

static std::wstring legacy_storage_file(const std::wstring& path, const std::wstring& directory)
{
    if(tcard_storage::Required(directory))return directory+L"\\cards.json";
    if (path.size() > 5 && _wcsicmp(path.c_str() + path.size() - 5, L".json") == 0) return path;
    return directory + L".json";
}

static std::wstring safe_card_stem(const std::wstring& id)
{
    std::wstring stem = id;
    for (wchar_t& ch : stem) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') ch = L'_';
    }
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' ')) stem.pop_back();
    return stem.empty() ? L"card" : stem;
}

static bool directory_exists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring CardDirectory(const std::wstring& filePath, const std::wstring& id)
{
    std::lock_guard guard(tcard_sync::sync_mutex);
    try{tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return {};
        return storage_directory(filePath) + L"\\" + safe_card_stem(id);
    }catch(...){return {};}
}

static bool parse_card_metadata(const JsonObject& object, CardRecord& card)
{
    card.id = json_string(object, L"Id", L"");
    card.title = json_string(object, L"Title", L"Untitled card");
    card.color = json_string(object, L"Color", L"");
    card.fontFamily = json_string(object, L"FontFamily", L"");
    card.fontSize = json_number(object, L"FontSize", 14.0);
    card.inheritFontFamily = !object.HasKey(L"FontFamily");
    card.inheritFontSize = !object.HasKey(L"FontSize");
    card.live = json_bool(object, L"Live", true);
    card.markdown = json_bool(object, L"Markdown", false);
    card.richHtml = json_string(object, L"ContentFormat", L"") == L"RichHtml";
    if (object.HasKey(L"Display")) {
        auto value = object.GetNamedValue(L"Display", nullptr);
        if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
            const JsonObject display = value.GetObject();
            card.color = json_string(display, L"Color", card.color.empty() ? L"#FFF5A8" : card.color.c_str());
            card.fontFamily = json_string(display, L"FontFamily", card.fontFamily.empty() ? L"Segoe UI" : card.fontFamily.c_str());
            card.fontSize = json_number(display, L"FontSize", card.fontSize);
            card.inheritFontFamily = json_bool(display, L"InheritFontFamily", card.inheritFontFamily && !display.HasKey(L"FontFamily"));
            card.inheritFontSize = json_bool(display, L"InheritFontSize", card.inheritFontSize && !display.HasKey(L"FontSize"));
            card.live = json_bool(display, L"Live", card.live);
            card.markdown = json_bool(display, L"Markdown", card.markdown);
            const double width = json_number(display, L"WindowWidthDip", 0);
            const double height = json_number(display, L"WindowHeightDip", 0);
            if (width >= 100 && width <= 16384 && height >= 100 && height <= 16384) {
                card.windowWidthDip = static_cast<int>(width);
                card.windowHeightDip = static_cast<int>(height);
            }
        }
    }
    if (card.richHtml) card.markdown = false;
    if (card.id.empty()) return false;
    if (card.title.empty()) card.title = L"Untitled card";
    if (card.color.empty()) card.color = L"#FFF5A8";
    if (card.fontFamily.empty()) card.fontFamily = L"Segoe UI";
    card.fontSize = std::clamp(card.fontSize, 8.0, 48.0);
    return true;
}

static void read_times(const JsonObject& object, CardRecord& card, FILETIME created, FILETIME updated,
    const std::wstring& directory={})
{
    const auto read = [&](const wchar_t* key) {
        const double ms = json_number(object, key, 0);
        if (!std::isfinite(ms) || ms <= 0 || ms > 1.0e15 || floor(ms)!=ms) return FILETIME{};
        return tcard_sync::sync_make_time(static_cast<uint64_t>(ms));
    };
    card.createdUtc=read(L"CreatedUtcMs");card.updatedUtc=read(L"UpdatedUtcMs");
    const auto missing=[](FILETIME value){return !value.dwLowDateTime&&!value.dwHighDateTime;};
    if(!missing(card.createdUtc)&&!missing(card.updatedUtc))return;
    const auto fill=[&](const tcard_sync::Dates& dates){
        if(missing(card.createdUtc))card.createdUtc=tcard_sync::sync_make_time(dates.created);
        if(missing(card.updatedUtc))card.updatedUtc=tcard_sync::sync_make_time(dates.updated);
    };
    // Prefer retained authored metadata; replay only when a missing field has no committed date.
    if(object.HasKey(L"SyncState")){
        const auto dates=tcard_sync::sync_read_dates(object.GetNamedObject(L"SyncState"));if(dates)fill(*dates);
    }
    if((missing(card.createdUtc)||missing(card.updatedUtc))&&!directory.empty()&&object.HasKey(L"SyncVersion")){
        tcard_sync::Snapshot state;std::wstring error;
        if(!tcard_sync::sync_merge(tcard_sync::sync_load(directory,card.id,tcard_sync::sync_ids(object)),state,error))
            throw std::runtime_error("Cannot recover retained dates");
        if(state.dates)fill(*state.dates);
    }
    // Old history points are estimates when exact authored dates are absent, never operation Time.
    if((missing(card.createdUtc)||missing(card.updatedUtc))&&object.HasKey(L"History")){
        uint64_t first=0,last=0;const auto history=object.GetNamedArray(L"History");
        for(unsigned i=0;i<history.Size();++i){const double ms=json_number(history.GetObjectAt(i),L"UtcMs",0);
            if(!std::isfinite(ms)||ms<=0||ms>1.0e15||floor(ms)!=ms)continue;
            const auto value=static_cast<uint64_t>(ms);first=first?(std::min)(first,value):value;last=(std::max)(last,value);}
        if(first)fill({first,last});
    }
    if(missing(card.createdUtc))card.createdUtc=created;
    if(missing(card.updatedUtc))card.updatedUtc=updated;
}

static bool load_legacy_cards(const std::wstring& filePath, std::vector<CardRecord>& cards)
{
    const std::wstring text = read_utf8(filePath);
    if (text.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA times{};
    GetFileAttributesExW(filePath.c_str(),GetFileExInfoStandard,&times);
    try {
        const JsonArray array = JsonArray::Parse(text);
        for (auto value : array) {
            if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
            const JsonObject object = value.GetObject();
            CardRecord card;
            if (!parse_card_metadata(object, card)) continue;
            card.source = json_string(object, L"Source", L"");
            read_times(object,card,times.ftCreationTime,times.ftLastWriteTime);
            cards.push_back(std::move(card));
        }
    } catch (...) {
        cards.clear();
        return false;
    }
    return !cards.empty();
}

static bool load_bundle(const std::wstring& directory, CardRecord& card, const std::wstring& journalRoot={})
{
    try {
        std::vector<BYTE> bytes;
        if (!tcard_asset::read(directory, L"card.json", bytes)) return false;
        const auto object = JsonObject::Parse(tcard_asset::decode(bytes));
        if (!parse_card_metadata(object, card)) return false;
        const auto source = json_string(object, L"SourceFile", L"");
        if (!tcard_asset::relative(source, false) || !tcard_asset::read(directory, source, bytes) ||
            tcard_asset::hash(bytes) != source.substr(5, 64)) return false;
        card.source = tcard_asset::decode(bytes);
        const auto slash=directory.find_last_of(L"\\/");
        read_times(object,card,{},{},!journalRoot.empty()?journalRoot:slash==std::wstring::npos?L"":directory.substr(0,slash));
        if(!tcard_sync::sync_read_time(card.createdUtc)||!tcard_sync::sync_read_time(card.updatedUtc)){
            WIN32_FILE_ATTRIBUTE_DATA times{};
            if(GetFileAttributesExW((directory+L"\\card.json").c_str(),GetFileExInfoStandard,&times)&&
                !(times.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
                if(!tcard_sync::sync_read_time(card.createdUtc))card.createdUtc=times.ftCreationTime;
                if(!tcard_sync::sync_read_time(card.updatedUtc))card.updatedUtc=times.ftLastWriteTime;
            }
        }
        return true;
    } catch (...) { return false; }
}

bool LoadCardSubset(const std::wstring& filePath,const std::vector<std::wstring>& ids,std::vector<CardRecord>& cards)
{
    std::lock_guard guard(tcard_sync::sync_mutex);
    try{
        tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return false;
        const auto directory=storage_directory(filePath);if(!tcard_storage::Ready(directory))return false;
        tcard_asset::File root(tcard_asset::open_dir(directory,false));if(!root)return false;
        std::vector<CardRecord> loaded;std::set<std::wstring> seen;
        for(const auto& id:ids){
            if(!seen.insert(id).second)continue;
            const auto stem=safe_card_stem(id);
            if(!tcard_asset::leaf(stem)||stem[0]==L'_')return false;
            if(tcard_sync::sync_deleted(directory,stem))continue;
            const auto bundle=directory+L"\\"+stem;
            const auto attributes=GetFileAttributesW(bundle.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){
                const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)continue;
                return false;
            }
            CardRecord card;if(!load_bundle(bundle,card)||card.id!=id)return false;
            loaded.push_back(std::move(card));
        }
        cards=std::move(loaded);return true;
    }catch(...){return false;}
}

bool LoadCards(const std::wstring& filePath, std::vector<CardRecord>& cards)
{
    return LoadCardSnapshot(filePath,cards)&&!cards.empty();
}
bool LoadCardSnapshot(const std::wstring& filePath, std::vector<CardRecord>& cards,bool migrate)
{
    std::lock_guard syncGuard(tcard_sync::sync_mutex);
    tcard_sync::SyncLock syncLock(tcard_native::Base(filePath)); if (!syncLock) return false;
    cards.clear();
    try{
    const std::wstring directory = storage_directory(filePath);
    if(!tcard_storage::Ready(directory))return false;
    if (!directory_exists(directory)) {
        if (!migrate) return false;
        if (load_legacy_cards(legacy_storage_file(filePath, directory), cards)) {
            SaveCards(directory, cards);
            return true;
        }
        return false;
    }
    tcard_asset::File storage(tcard_asset::open_dir(directory, false));
    if (!storage) return false;
    std::set<std::wstring> bundled;bool unreadableBundle=false;
    WIN32_FIND_DATAW data{};
    HANDLE bundles = FindFirstFileW((directory + L"\\*").c_str(), &data);
    if(bundles==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    if (bundles != INVALID_HANDLE_VALUE) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const std::wstring name = data.cFileName;
            if (!tcard_asset::leaf(name) || name[0] == L'_') continue;
            if (tcard_sync::sync_deleted(directory, name)) continue;
            CardRecord card;
            if (load_bundle(directory + L"\\" + name, card) && safe_card_stem(card.id) == name) {
                bundled.insert(card.id); cards.push_back(std::move(card));
            }else if(tcard_sync::sync_exists(directory+L"\\"+name,L"card.json"))unreadableBundle=true;
        } while (FindNextFileW(bundles, &data));
        const auto scanError=GetLastError();FindClose(bundles);
        if(scanError!=ERROR_NO_MORE_FILES)return false;
    }
    const std::wstring pattern = directory + L"\\*.json";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if(find==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    bool loaded = !cards.empty();
    if (find != INVALID_HANDLE_VALUE) do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring metadataName = data.cFileName;
        if (_wcsicmp(metadataName.c_str(), L"cards.json") == 0) continue;
        const size_t dot = metadataName.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) continue;
        const std::wstring stem = metadataName.substr(0, dot);
        if (tcard_sync::sync_deleted(directory, stem)) continue;
        const std::wstring sourcePath = directory + L"\\" + stem + L".txt";
        if (GetFileAttributesW(sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<BYTE> metadataBytes;
        if (!tcard_asset::read(directory, metadataName, metadataBytes)) { unreadableBundle=true; continue; }
        try {
            const std::wstring text = tcard_asset::decode(metadataBytes);
            if (text.empty()) continue;
            const JsonObject object = JsonObject::Parse(text);
            CardRecord card;
            if (!parse_card_metadata(object, card)) continue;
            if (bundled.count(card.id)) continue;
            if (safe_card_stem(card.id) != stem) continue;
            if (card.id.empty()) card.id = stem;
            std::vector<BYTE> sourceBytes;
            if (!tcard_asset::read(directory, stem + L".txt", sourceBytes)) { unreadableBundle=true; continue; }
            card.source = tcard_asset::decode(sourceBytes);
            read_times(object,card,data.ftCreationTime,data.ftLastWriteTime);
            cards.push_back(std::move(card));
            loaded = true;
        } catch (...) {
            unreadableBundle=true; continue;
        }
    } while (FindNextFileW(find, &data));
    if (find != INVALID_HANDLE_VALUE){const auto scanError=GetLastError();FindClose(find);if(scanError!=ERROR_NO_MORE_FILES)return false;}
    if(unreadableBundle&&tcard_storage::Required(directory)){cards.clear();return false;}
    if (!loaded && load_legacy_cards(legacy_storage_file(filePath, directory), cards)) {
        cards.erase(std::remove_if(cards.begin(), cards.end(), [&](const auto& card) {
            return tcard_sync::sync_deleted(directory, safe_card_stem(card.id));
        }), cards.end());
        if (cards.empty()) return !unreadableBundle;
        if (!migrate) return false;
        SaveCards(directory, cards);
        return true;
    }
    // Retain old flat files and immutable pre-conversion source versions for recovery.
    for (auto& card : cards) {
        if (!bundled.count(card.id) || (card.markdown && !card.live && card.source.find(L"data:image/") != std::wstring::npos)) {
            if (!migrate) return false;
            std::vector<CardRecord> migration{card};
            if (SaveCards(directory, migration)) card = std::move(migration.front());
        }
    }
    std::sort(cards.begin(), cards.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    // A readable empty root (including all-deleted cards) is a successful snapshot.
    return loaded||!unreadableBundle;
    }catch(...){cards.clear();return false;}
}

std::wstring RenderRich(const std::wstring& source, const SYSTEMTIME& localTime)
{
    auto document = tcard_rich::parse(source);
    for (auto& paragraph : document.paragraphs)
        for (auto& run : paragraph.runs) run.text = Render(run.text, localTime);
    return tcard_rich::serialize(document);
}

bool SaveCards(const std::wstring& filePath, std::vector<CardRecord>& cards, bool checkpoint, unsigned historyLimit)
{
    std::lock_guard syncGuard(tcard_sync::sync_mutex);
    tcard_sync::SyncLock syncLock(tcard_native::Base(filePath)); if (!syncLock) return false;
    try {
    const std::wstring directory = storage_directory(filePath);
    if(!tcard_storage::Ready(directory))return false;
    tcard_asset::File storage(tcard_asset::open_dir(directory, true)); if (!storage) return false;
    for (CardRecord& card : cards) {
        if(card.richHtml&&(card.source.size()>4*1024*1024||tcard_rich::parse(card.source).paragraphs.empty()))return false;
        const std::wstring stem = safe_card_stem(card.id);
        if (!tcard_asset::leaf(stem) || stem[0] == L'_') return false;
        const std::wstring bundle = directory + L"\\" + stem;
        tcard_asset::File owned(tcard_asset::open_dir(bundle, true)); if (!owned) return false;
        CardRecord previous;
        bool unchanged = false;
        if (load_bundle(bundle, previous)) {
            if (previous.id != card.id) return false;
            unchanged = previous.title == card.title && previous.color == card.color && previous.fontFamily == card.fontFamily &&
                previous.fontSize == card.fontSize && previous.inheritFontFamily == card.inheritFontFamily && previous.inheritFontSize == card.inheritFontSize && previous.live == card.live && previous.markdown == card.markdown && previous.richHtml == card.richHtml && previous.source == card.source;
        } else {
            try {
                const auto old = JsonObject::Parse(read_utf8(directory + L"\\" + stem + L".json"));
                if (parse_card_metadata(old, previous)) {
                    if (previous.id != card.id) return false;
                    previous.source = read_utf8(directory + L"\\" + stem + L".txt");
                    unchanged = previous.title == card.title && previous.color == card.color && previous.fontFamily == card.fontFamily &&
                        previous.fontSize == card.fontSize && previous.inheritFontFamily == card.inheritFontFamily && previous.inheritFontSize == card.inheritFontSize && previous.live == card.live && previous.markdown == card.markdown && previous.richHtml == card.richHtml && previous.source == card.source;
                }
            } catch (...) { }
        }
        auto next = card;
        if (card.markdown && !card.live && !tcard_sync::sync_importing) {
            next.source = tcard_asset::externalize(bundle, card.source);
            if (next.source != card.source && !previous.id.empty() && previous.source == card.source) {
                std::wstring backup;
                if (!tcard_asset::version(bundle, card.source, card.markdown, backup)) return false;
            }
        }
        std::wstring sourceFile;
        if (!tcard_asset::version(bundle, next.source, next.markdown, sourceFile, next.richHtml)) return false;
        FILETIME created = card.createdUtc, updated = card.updatedUtc;
        if (!created.dwLowDateTime && !created.dwHighDateTime) created=previous.createdUtc;
        if (!updated.dwLowDateTime && !updated.dwHighDateTime) updated=previous.updatedUtc;
        if (!created.dwLowDateTime && !created.dwHighDateTime) GetSystemTimeAsFileTime(&created);
        if ((!previous.id.empty() && !unchanged && !tcard_sync::sync_importing) || (!updated.dwLowDateTime && !updated.dwHighDateTime)) GetSystemTimeAsFileTime(&updated);
        next.createdUtc=created;next.updatedUtc=updated;
        JsonObject display;
        display.Insert(L"Color", JsonValue::CreateStringValue(card.color.empty() ? L"#FFF5A8" : card.color));
        display.Insert(L"FontFamily", JsonValue::CreateStringValue(card.fontFamily.empty() ? L"Segoe UI" : card.fontFamily));
        display.Insert(L"FontSize", JsonValue::CreateNumberValue(std::clamp(card.fontSize, 8.0, 48.0)));
        display.Insert(L"InheritFontFamily", JsonValue::CreateBooleanValue(card.inheritFontFamily));
        display.Insert(L"InheritFontSize", JsonValue::CreateBooleanValue(card.inheritFontSize));
        display.Insert(L"Live", JsonValue::CreateBooleanValue(card.live));
        display.Insert(L"Markdown", JsonValue::CreateBooleanValue(card.markdown));
        display.Insert(L"WindowWidthDip", JsonValue::CreateNumberValue(card.windowWidthDip));
        display.Insert(L"WindowHeightDip", JsonValue::CreateNumberValue(card.windowHeightDip));
        JsonObject previousManifest;
        JsonArray history;
        std::wstring previousFile;
        std::set<std::wstring> expired;
        try {
            const auto oldManifest = JsonObject::Parse(read_utf8(bundle + L"\\card.json"));
            previousManifest = oldManifest;
            if (oldManifest.HasKey(L"History")) history = oldManifest.GetNamedArray(L"History");
            previousFile=json_string(oldManifest,L"SourceFile",L"");
        } catch (...) {
            if (GetFileAttributesW((bundle + L"\\card.json").c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        }
        // Keep the pre-edit baseline when an older bundle has no indexed history.
        if (!history.Size() && !previous.id.empty()) {
            std::wstring baselineFile;
            if (!tcard_asset::version(bundle,previous.source,previous.markdown,baselineFile,previous.richHtml))return false;
            JsonObject baseline;
            baseline.Insert(L"SourceFile",JsonValue::CreateStringValue(baselineFile));
            baseline.Insert(L"Title",JsonValue::CreateStringValue(previous.title));
            baseline.Insert(L"ContentFormat",JsonValue::CreateStringValue(previous.richHtml?L"RichHtml":previous.markdown?L"Markdown":L"Plain"));
            baseline.Insert(L"UtcMs",JsonValue::CreateNumberValue(0));
            history.Append(baseline);
        }
        if (checkpoint) {
            bool duplicate = false;
            if (history.Size()) {
                const auto last = history.GetObjectAt(history.Size() - 1);
                duplicate = json_string(last, L"SourceFile", L"") == sourceFile &&
                    json_string(last, L"Title", L"") == card.title &&
                    json_string(last, L"Display", L"") == display.Stringify().c_str();
            }
            if (!duplicate) {
                JsonObject point;
                point.Insert(L"SourceFile", JsonValue::CreateStringValue(sourceFile));
                point.Insert(L"Title", JsonValue::CreateStringValue(card.title));
                point.Insert(L"ContentFormat",JsonValue::CreateStringValue(card.richHtml?L"RichHtml":card.markdown?L"Markdown":L"Plain"));
                point.Insert(L"Display", JsonValue::CreateStringValue(display.Stringify()));
                FILETIME time{}; GetSystemTimeAsFileTime(&time);
                ULARGE_INTEGER ticks{}; ticks.LowPart = time.dwLowDateTime; ticks.HighPart = time.dwHighDateTime;
                point.Insert(L"UtcMs", JsonValue::CreateNumberValue(static_cast<double>(ticks.QuadPart / 10000ULL)));
                history.Append(point);
            }
        }
        if(historyLimit&&checkpoint&&history.Size()>historyLimit){
            JsonArray retained;
            const auto removeCount=history.Size()-historyLimit;
            for(unsigned i=0;i<history.Size();++i){
                if(i<removeCount)expired.insert(json_string(history.GetObjectAt(i),L"SourceFile",L""));
                else retained.Append(history.GetAt(i));
            }
            history=retained;
        }
        JsonObject object;
        object.Insert(L"StorageVersion", JsonValue::CreateNumberValue(1));
        object.Insert(L"SourceFile", JsonValue::CreateStringValue(sourceFile));
        object.Insert(L"ContentFormat", JsonValue::CreateStringValue(card.richHtml ? L"RichHtml" : card.markdown ? L"Markdown" : L"Plain"));
        object.Insert(L"Id", JsonValue::CreateStringValue(card.id));
        object.Insert(L"Title", JsonValue::CreateStringValue(card.title.empty() ? L"Untitled card" : card.title));
        object.Insert(L"Display", display);
        object.Insert(L"History", history);
        const auto milliseconds = [](FILETIME time) { ULARGE_INTEGER ticks{}; ticks.LowPart = time.dwLowDateTime; ticks.HighPart = time.dwHighDateTime; return static_cast<double>(ticks.QuadPart / 10000ULL); };
        object.Insert(L"CreatedUtcMs", JsonValue::CreateNumberValue(milliseconds(created)));
        object.Insert(L"UpdatedUtcMs", JsonValue::CreateNumberValue(milliseconds(updated)));
        if (!tcard_sync::sync_stage(directory, card.id, previous, next, previousManifest, object)) return false;
        if (!tcard_asset::publish(bundle, L"card.json", tcard_asset::encode(object.Stringify().c_str()))) return false;
        CardRecord verified;
        if (!load_bundle(bundle, verified) || verified.source != next.source || verified.id != card.id) return false;
        const auto marker = directory + L"\\" + stem + L".deleted";
        if (!DeleteFileW(marker.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        if(historyLimit){
            if(!previousFile.empty())expired.insert(previousFile);
            std::set<std::wstring> retained{sourceFile};
            for(unsigned i=0;i<history.Size();++i)retained.insert(json_string(history.GetObjectAt(i),L"SourceFile",L""));
            for(const auto& name:expired)if(!retained.contains(name))tcard_asset::remove_version(bundle,name);
        }
        next.createdUtc = created; next.updatedUtc = updated; card = std::move(next);
    }
    return true;
    } catch (...) { return false; }
}

namespace {
constexpr ULONGLONG trashLifetimeMs = 30ULL * 24 * 60 * 60 * 1000;
ULONGLONG trash_now()
{
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks{}; ticks.LowPart=now.dwLowDateTime; ticks.HighPart=now.dwHighDateTime;
    return ticks.QuadPart / 10000;
}
bool trash_stamp(const std::wstring& bundle,JsonObject& metadata,ULONGLONG now)
{
    if(metadata.HasKey(L"TrashDeletedUtcMs")){
        if(metadata.GetNamedValue(L"TrashDeletedUtcMs").ValueType()!=winrt::Windows::Data::Json::JsonValueType::Number)return false;
        const auto value=metadata.GetNamedNumber(L"TrashDeletedUtcMs");
        if(value!=0)return std::isfinite(value)&&value>0&&value<=1.0e15&&std::floor(value)==value;
    }
    if(!now||now>1000000000000000ULL)return false;
    auto observed=JsonObject::Parse(std::wstring(metadata.Stringify()));
    observed.Insert(L"TrashDeletedUtcMs",JsonValue::CreateNumberValue(static_cast<double>(now)));
    // First observation is durable once. A failed publication must not appear committed in memory.
    if(!tcard_sync::sync_write(bundle,L"card.json",observed))return false;
    metadata=std::move(observed);return true;
}
std::unique_ptr<tcard_asset::File> trash_pin(const std::wstring& path,bool directory)
{
    auto file=std::make_unique<tcard_asset::File>(CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if(!*file||!tcard_asset::regular(file->value,directory)||!GetFileInformationByHandle(file->value,&info)||
        (!directory&&info.nNumberOfLinks!=1))return {};
    wchar_t absolute[32768]{},finalPath[32768]{};
    const auto a=GetFullPathNameW(path.c_str(),32768,absolute,nullptr);
    const auto f=GetFinalPathNameByHandleW(file->value,finalPath,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    const auto normalize=[](std::wstring value){
        if(value.starts_with(L"\\\\?\\UNC\\"))value=L"\\\\"+value.substr(8);
        else if(value.starts_with(L"\\\\?\\"))value.erase(0,4);
        return value;
    };
    if(!a||a>=32768||!f||f>=32768||_wcsicmp(normalize(absolute).c_str(),normalize(finalPath).c_str()))return {};
    return file;
}
bool trash_collect(const std::wstring& directory,unsigned depth,
    std::vector<std::unique_ptr<tcard_asset::File>>& pinned)
{
    if(depth>2||pinned.size()>=4096)return false;
    auto folder=trash_pin(directory,true);if(!folder)return false;
    std::unique_ptr<tcard_asset::File> metadata;
    WIN32_FIND_DATAW item{};HANDLE handle=FindFirstFileW((directory+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){
        if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    }else{
        struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
        do{
            const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
            if(!tcard_asset::leaf(name)||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||pinned.size()>=4096)return false;
            const auto child=directory+L"\\"+name;
            if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!trash_collect(child,depth+1,pinned))return false;}
            else{
                auto file=trash_pin(child,false);if(!file)return false;
                if(name==L"card.json")metadata=std::move(file);else pinned.push_back(std::move(file));
            }
        }while(FindNextFileW(handle,&item));
        if(GetLastError()!=ERROR_NO_MORE_FILES)return false;
    }
    // Postorder handles keep every ancestor fixed until all regular children are disposed.
    if(metadata)pinned.push_back(std::move(metadata));
    pinned.push_back(std::move(folder));return true;
}
bool trash_remove(const std::wstring& bundle)
{
    std::vector<std::unique_ptr<tcard_asset::File>> pinned;
    if(!trash_collect(bundle,0,pinned))return false;
    FILE_DISPOSITION_INFO dispose{TRUE};
    for(auto& file:pinned){
        if(!SetFileInformationByHandle(file->value,FileDispositionInfo,&dispose,sizeof(dispose)))return false;
        file.reset();
    }
    return true;
}
}

bool PurgeTrash(const std::wstring& filePath,ULONGLONG nowUtcMs)
{
    try{
    std::unique_lock syncGuard(tcard_sync::sync_mutex,std::try_to_lock);if(!syncGuard)return false;
    tcard_sync::SyncLock syncLock(tcard_native::Base(filePath));if(!syncLock)return false;
    const auto directory=storage_directory(filePath);if(!tcard_storage::Ready(directory))return false;
    tcard_asset::File storage(tcard_asset::open_dir(directory,false));if(!storage)return false;
    const auto trash=directory+L"\\_trash";
    const auto attributes=GetFileAttributesW(trash.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND;
    tcard_asset::File archive(tcard_asset::open_dir(trash,false));if(!archive)return false;
    const auto now=nowUtcMs?nowUtcMs:trash_now();bool okay=true;unsigned processed=0;
    // Retry bundles left outside trash by a locked deletion. Never purge a live-root directory.
    WIN32_FIND_DATAW markerItem{};HANDLE markers=FindFirstFileW((directory+L"\\*.deleted").c_str(),&markerItem);
    if(markers!=INVALID_HANDLE_VALUE){
        struct Markers{HANDLE value;~Markers(){FindClose(value);}} markerSearch{markers};
        do{
            const std::wstring markerName=markerItem.cFileName;
            if(markerItem.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){okay=false;continue;}
            const auto stem=markerName.substr(0,markerName.size()-8),bundle=directory+L"\\"+stem;
            if(!tcard_asset::leaf(stem)||stem[0]==L'_'||!tcard_sync::sync_deleted(directory,stem))continue;
            if(GetFileAttributesW(bundle.c_str())==INVALID_FILE_ATTRIBUTES)continue;
            JsonObject metadata;GUID guid{};wchar_t token[40]{};
            if(!tcard_sync::sync_read(bundle,L"card.json",metadata)||!metadata.HasKey(L"Id")||
                safe_card_stem(std::wstring(metadata.GetNamedString(L"Id")))!=stem||!trash_stamp(bundle,metadata,now)||
                FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,token,40)||
                !MoveFileExW(bundle.c_str(),(trash+L"\\"+token).c_str(),MOVEFILE_WRITE_THROUGH))okay=false;
        }while(FindNextFileW(markers,&markerItem));
        if(GetLastError()!=ERROR_NO_MORE_FILES)okay=false;
    }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)return false;
    WIN32_FIND_DATAW item{};HANDLE handle=FindFirstFileW((trash+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
        do{
            const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
            GUID guid{};
            if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
                !tcard_asset::leaf(name)||name.size()!=38||name.front()!=L'{'||name.back()!=L'}'||
                FAILED(CLSIDFromString(name.c_str(),&guid))){okay=false;continue;}
            const auto bundle=trash+L"\\"+name;JsonObject metadata;
            if(!tcard_sync::sync_read(bundle,L"card.json",metadata)||!metadata.HasKey(L"Id")){
                // A completed payload disposal can leave an empty folder after interruption.
                // Never remove nonempty unknown archives without authenticated deletion metadata.
                auto empty=trash_pin(bundle,true);
                FILE_DISPOSITION_INFO dispose{TRUE};
                if(!empty||!SetFileInformationByHandle(empty->value,FileDispositionInfo,&dispose,sizeof(dispose)))okay=false;
                continue;
            }
            if(metadata.GetNamedValue(L"Id").ValueType()!=winrt::Windows::Data::Json::JsonValueType::String||
                std::wstring(metadata.GetNamedString(L"Id")).empty()){okay=false;continue;}
            // Old archives have no reliable deletion time. First observation starts a full grace period.
            if(!trash_stamp(bundle,metadata,now)){okay=false;continue;}
            if(metadata.GetNamedValue(L"TrashDeletedUtcMs").ValueType()!=winrt::Windows::Data::Json::JsonValueType::Number){okay=false;continue;}
            const double value=metadata.GetNamedNumber(L"TrashDeletedUtcMs",0);
            if(!std::isfinite(value)||value<=0||value>1.0e15||std::floor(value)!=value){okay=false;continue;}
            const auto deleted=static_cast<ULONGLONG>(value);
            if(now>=deleted&&now-deleted>=trashLifetimeMs){
                if(!trash_remove(bundle))okay=false;
                if(++processed>=64)return false; // Retry bounded batches on the next maintenance tick.
            }
        }while(FindNextFileW(handle,&item));
        return GetLastError()==ERROR_NO_MORE_FILES&&okay;
    }catch(...){return false;}
}

namespace {
bool trash_check_guid(const std::wstring& name)
{
    GUID guid{};return name.size()==38&&name.front()==L'{'&&name.back()==L'}'&&SUCCEEDED(CLSIDFromString(name.c_str(),&guid));
}
bool trash_read(const std::wstring& root,const std::wstring& token,TrashRecord& record)
{
    const bool archive=token.starts_with(L"a:");
    const auto name=token.size()>2?token.substr(2):L"";
    if((!archive&&!token.starts_with(L"r:"))||!tcard_asset::leaf(name)||name.front()==L'_'||(archive&&!trash_check_guid(name)))return false;
    record={};record.token=token;record.directory=root+L"\\"+(archive?L"_trash\\"+name:name);
    JsonObject metadata;
    if(!tcard_sync::sync_read(record.directory,L"card.json",metadata)||!parse_card_metadata(metadata,record.card)||record.card.id.empty())return false;
    const auto stem=safe_card_stem(record.card.id);
    if(!tcard_asset::leaf(stem)||stem.front()==L'_'||(!archive&&stem!=name)||!tcard_sync::sync_deleted(root,stem))return false;
    const bool unknown=!metadata.HasKey(L"TrashDeletedUtcMs")||
        (metadata.GetNamedValue(L"TrashDeletedUtcMs").ValueType()==winrt::Windows::Data::Json::JsonValueType::Number&&
            metadata.GetNamedNumber(L"TrashDeletedUtcMs")==0);
    if(unknown){
        // Keep an accessible retained copy listed even if a lock prevents recording its estimate.
        if(trash_stamp(record.directory,metadata,trash_now()))record.deletedUtcMs=static_cast<ULONGLONG>(metadata.GetNamedNumber(L"TrashDeletedUtcMs"));
    }else{
        if(metadata.GetNamedValue(L"TrashDeletedUtcMs").ValueType()!=winrt::Windows::Data::Json::JsonValueType::Number)return false;
        const auto value=metadata.GetNamedNumber(L"TrashDeletedUtcMs");
        if(!std::isfinite(value)||value<=0||value>1.0e15||std::floor(value)!=value)return false;
        record.deletedUtcMs=static_cast<ULONGLONG>(value);
    }
    const auto metadataCard=record.card;
    record.readable=load_bundle(record.directory,record.card,root);
    if(!record.readable){record.card=metadataCard;record.card.source.clear();}
    return true;
}
bool trash_copy(const std::wstring& source,const std::wstring& destination,size_t& entries,unsigned depth=0)
{
    if(depth>1)return false;
    tcard_asset::File from(tcard_asset::open_dir(source,false)),to(tcard_asset::open_dir(destination,true));if(!from||!to)return false;
    WIN32_FIND_DATAW data{};HANDLE handle=FindFirstFileW((source+L"\\*").c_str(),&data);
    if(handle==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{
        const std::wstring name=data.cFileName;if(name==L"."||name==L".."||name==L"card.json")continue;
        if(++entries>4096||(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            if(depth||name!=L"images"||!trash_copy(source+L"\\"+name,destination+L"\\"+name,entries,1))return false;
        }else{
            const auto dot=name.find_last_of(L'.');const auto stem=dot==std::wstring::npos?name:name.substr(0,dot);
            const auto hash=depth?stem:name.starts_with(L"note-")?stem.substr(5):L"";
            if(!tcard_sync::sync_hex(hash)||!tcard_asset::relative(depth?L"images/"+name:name,depth!=0))return false;
            std::vector<BYTE> bytes;
            if(!tcard_asset::read(source,name,bytes)||tcard_asset::hash(bytes)!=hash||!tcard_asset::publish(destination,name,bytes,true))return false;
        }
    }while(FindNextFileW(handle,&data));
    return GetLastError()==ERROR_NO_MORE_FILES;
}
}
bool LoadTrash(const std::wstring& filePath,std::vector<TrashRecord>& records)
{
    records.clear();std::lock_guard guard(tcard_sync::sync_mutex);
    try{
        tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return false;
        const auto root=storage_directory(filePath);
        tcard_asset::File storage(tcard_asset::open_dir(root,false));if(!lock||!storage||!tcard_storage::Ready(root))return false;
        for(const bool archives:{true,false}){
            const auto directory=archives?root+L"\\_trash":root;
            if(archives&&GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES){if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;return false;}
            tcard_asset::File folder(tcard_asset::open_dir(directory,false));if(!folder)return false;
            WIN32_FIND_DATAW data{};HANDLE handle=FindFirstFileW((directory+L"\\*").c_str(),&data);
            if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;return false;}
            struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
            do{
                const std::wstring name=data.cFileName;
                if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
                    !tcard_asset::leaf(name)||name.front()==L'_'||(archives&&!trash_check_guid(name)))continue;
                TrashRecord record;
                try{if(trash_read(root,(archives?L"a:":L"r:")+name,record))records.push_back(std::move(record));}catch(...){continue;}
            }while(FindNextFileW(handle,&data));
            if(GetLastError()!=ERROR_NO_MORE_FILES)return false;
        }
        std::stable_sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.deletedUtcMs!=b.deletedUtcMs?a.deletedUtcMs>b.deletedUtcMs:a.token<b.token;});
        return true;
    }catch(...){records.clear();return false;}
}
bool RetainCardHistory(const std::wstring& filePath,const std::wstring& id,CardRecord& retained)
{
    std::lock_guard guard(tcard_sync::sync_mutex);
    try{
        tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return false;
        const auto root=storage_directory(filePath),stem=safe_card_stem(id);
        if(!lock||id.empty()||!tcard_asset::leaf(stem)||stem[0]==L'_'||!tcard_storage::Ready(root))return false;
        // New notes do not enumerate Trash; only an existing local tombstone needs retained metadata.
        if(!tcard_sync::sync_deleted(root,stem))return true;
        const auto bundle=root+L"\\"+stem;
        if(tcard_sync::sync_exists(bundle,L"card.json"))return load_bundle(bundle,retained,root)&&retained.id==id;
        const auto folder=root+L"\\_trash";WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW((folder+L"\\*").c_str(),&item);
        if(handle==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND;
        struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};TrashRecord selected;size_t visited=0;
        do{
            const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
            if(++visited>10000)return false;
            if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||!trash_check_guid(name))continue;
            JsonObject metadata;if(!tcard_sync::sync_read(folder+L"\\"+name,L"card.json",metadata)||json_string(metadata,L"Id",L"")!=id)continue;
            TrashRecord candidate;if(!trash_read(root,L"a:"+name,candidate)||!candidate.readable)return false;
            if(selected.token.empty()||candidate.deletedUtcMs>selected.deletedUtcMs||
                (candidate.deletedUtcMs==selected.deletedUtcMs&&candidate.token<selected.token))selected=std::move(candidate);
        }while(FindNextFileW(handle,&item));
        if(GetLastError()!=ERROR_NO_MORE_FILES)return false;if(selected.token.empty())return true;
        size_t entries=0;if(!trash_copy(selected.directory,bundle,entries))return false;
        JsonObject metadata;if(!tcard_sync::sync_read(selected.directory,L"card.json",metadata)||!tcard_sync::sync_write(bundle,L"card.json",metadata))return false;
        retained=std::move(selected.card);return true;
    }catch(...){return false;}
}
bool RestoreTrash(const std::wstring& filePath,const std::wstring& token,CardRecord& restored)
{
    restored={};std::lock_guard guard(tcard_sync::sync_mutex);
    try{
        tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return false;
        const auto root=storage_directory(filePath);
        tcard_asset::File storage(tcard_asset::open_dir(root,false));if(!lock||!storage||!tcard_storage::Ready(root))return false;
        TrashRecord record;if(!trash_read(root,token,record)||!record.readable)return false;
        // Read the selected retained version anew; stale UI data must never overwrite an active note.
        const auto bundle=root+L"\\"+safe_card_stem(record.card.id);
        if(record.directory!=bundle){
            size_t entries=0;if(!trash_copy(record.directory,bundle,entries))return false;
            if(!tcard_sync::sync_exists(bundle,L"card.json")){
                JsonObject metadata;if(!tcard_sync::sync_read(record.directory,L"card.json",metadata)||!tcard_sync::sync_write(bundle,L"card.json",metadata))return false;
            }
        }
        std::vector<CardRecord> cards{record.card};
        if(!SaveCards(root,cards))return false;
        restored=std::move(cards.front());
        // A verified normal save commits the restore and its causal parents before payload cleanup.
        if(record.directory!=bundle)trash_remove(record.directory);
        return true;
    }catch(...){return false;}
}
bool RemoveTrash(const std::wstring& filePath,const std::wstring& token)
{
    std::lock_guard guard(tcard_sync::sync_mutex);
    try{
        tcard_sync::SyncLock lock(tcard_native::Base(filePath));if(!lock)return false;
        const auto root=storage_directory(filePath);
        tcard_asset::File storage(tcard_asset::open_dir(root,false));if(!lock||!storage||!tcard_storage::Ready(root))return false;
        TrashRecord record;if(!trash_read(root,token,record)||!record.readable)return false;
        if(token.starts_with(L"r:")){
            const auto trash=root+L"\\_trash";tcard_asset::File folder(tcard_asset::open_dir(trash,true));
            GUID guid{};wchar_t name[40]{};
            if(!folder||FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,name,40))return false;
            const auto destination=trash+L"\\"+name;
            if(!MoveFileExW(record.directory.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))return false;
            record.directory=destination;
        }
        return trash_remove(record.directory);
    }catch(...){return false;}
}

bool DeleteCard(const std::wstring& filePath, const std::wstring& id)
{
    std::lock_guard syncGuard(tcard_sync::sync_mutex);
    tcard_sync::SyncLock syncLock(tcard_native::Base(filePath)); if (!syncLock) return false;
    try {
    const std::wstring directory = storage_directory(filePath);
    const std::wstring stem = safe_card_stem(id);
    if (id.empty() || !tcard_asset::leaf(stem) || stem[0] == L'_') return false;
    tcard_asset::File storage(tcard_asset::open_dir(directory, false)); if (!storage) return false;
    const auto bundle = directory + L"\\" + stem;
    CardRecord previous; load_bundle(bundle, previous);
    CardRecord next = previous; next.id = id;
    JsonObject oldManifest, marker;
    if (tcard_sync::sync_exists(bundle, L"card.json") && !tcard_sync::sync_read(bundle, L"card.json", oldManifest)) return false;
    if (!tcard_sync::sync_stage(directory, id, previous, next, oldManifest, marker, true)) return false;
    marker.Insert(L"Id", JsonValue::CreateStringValue(id));
    if (!tcard_sync::sync_write(directory, stem + L".deleted", marker)) return false;
    const DWORD attributes = GetFileAttributesW(bundle.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        const auto trash = directory + L"\\_trash";
        tcard_asset::File archive(tcard_asset::open_dir(trash, true));
        GUID guid{}; wchar_t token[40]{};
        // Keep deletion time local to the retained copy; sync operation IDs and tombstones remain untouched.
        if (archive && oldManifest.HasKey(L"Id") && trash_stamp(bundle,oldManifest,trash_now()) &&
            SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, token, 40))
            MoveFileExW(bundle.c_str(), (trash + L"\\" + token).c_str(), MOVEFILE_WRITE_THROUGH);
    }
    // A locked bundle may remain in place; the durable tombstone still hides it without losing data.
    return true;
    } catch (...) { return false; }
}

}
