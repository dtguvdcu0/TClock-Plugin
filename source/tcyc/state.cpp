#include "state.h"

#include <windows.h>
#include <time.h>

namespace tcyc {

long long UnixNow() {
    return static_cast<long long>(time(nullptr));
}

bool LoadRuntimeState(const std::wstring& statePath, RuntimeState& outState) {
    RuntimeState s{};
    const DWORD attrs = GetFileAttributesW(statePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        outState = s;
        return true;
    }

    s.bootCount = static_cast<int>(GetPrivateProfileIntW(L"Runtime", L"BootCount", 0, statePath.c_str()));

    wchar_t tmp[64] = {0};
    GetPrivateProfileStringW(L"Runtime", L"LastEvalUnix", L"0", tmp, static_cast<DWORD>(_countof(tmp)), statePath.c_str());
    s.lastEvalUnix = _wtoll(tmp);
    GetPrivateProfileStringW(L"Runtime", L"LastSaveUnix", L"0", tmp, static_cast<DWORD>(_countof(tmp)), statePath.c_str());
    s.lastSaveUnix = _wtoll(tmp);

    for (int i = 1; i <= 128; ++i) {
        wchar_t section[32] = {0};
        swprintf_s(section, L"Task.%d", i);
        TaskRuntimeState ts{};
        ts.firedCount = static_cast<int>(GetPrivateProfileIntW(section, L"FiredCount", 0, statePath.c_str()));
        GetPrivateProfileStringW(section, L"LastCheckUnix", L"0", tmp, static_cast<DWORD>(_countof(tmp)), statePath.c_str());
        ts.lastCheckUnix = _wtoll(tmp);
        GetPrivateProfileStringW(section, L"LastFireUnix", L"0", tmp, static_cast<DWORD>(_countof(tmp)), statePath.c_str());
        ts.lastFireUnix = _wtoll(tmp);
        ts.startupDone = GetPrivateProfileIntW(section, L"StartupDone", 0, statePath.c_str()) != 0;
        ts.watchdogRetryCount = static_cast<int>(GetPrivateProfileIntW(section, L"WatchdogRetryCount", 0, statePath.c_str()));
        GetPrivateProfileStringW(section, L"WatchdogNextRetryUnix", L"0", tmp, static_cast<DWORD>(_countof(tmp)), statePath.c_str());
        ts.watchdogNextRetryUnix = _wtoll(tmp);
        if (ts.firedCount != 0 || ts.lastCheckUnix != 0 || ts.lastFireUnix != 0 || ts.startupDone ||
            ts.watchdogRetryCount != 0 || ts.watchdogNextRetryUnix != 0) {
            s.tasks[i] = ts;
        }
    }

    outState = s;
    return true;
}

bool SaveRuntimeState(const std::wstring& statePath, const RuntimeState& state) {
    wchar_t num[64] = {0};
    swprintf_s(num, L"%d", state.bootCount);
    if (!WritePrivateProfileStringW(L"Runtime", L"BootCount", num, statePath.c_str())) return false;
    swprintf_s(num, L"%I64d", state.lastEvalUnix);
    if (!WritePrivateProfileStringW(L"Runtime", L"LastEvalUnix", num, statePath.c_str())) return false;
    swprintf_s(num, L"%I64d", state.lastSaveUnix);
    if (!WritePrivateProfileStringW(L"Runtime", L"LastSaveUnix", num, statePath.c_str())) return false;

    for (const auto& kv : state.tasks) {
        const int id = kv.first;
        const TaskRuntimeState& ts = kv.second;
        wchar_t section[32] = {0};
        swprintf_s(section, L"Task.%d", id);
        swprintf_s(num, L"%d", ts.firedCount);
        if (!WritePrivateProfileStringW(section, L"FiredCount", num, statePath.c_str())) return false;
        swprintf_s(num, L"%I64d", ts.lastCheckUnix);
        if (!WritePrivateProfileStringW(section, L"LastCheckUnix", num, statePath.c_str())) return false;
        swprintf_s(num, L"%I64d", ts.lastFireUnix);
        if (!WritePrivateProfileStringW(section, L"LastFireUnix", num, statePath.c_str())) return false;
        if (!WritePrivateProfileStringW(section, L"StartupDone", ts.startupDone ? L"1" : L"0", statePath.c_str())) return false;
        swprintf_s(num, L"%d", ts.watchdogRetryCount);
        if (!WritePrivateProfileStringW(section, L"WatchdogRetryCount", num, statePath.c_str())) return false;
        swprintf_s(num, L"%I64d", ts.watchdogNextRetryUnix);
        if (!WritePrivateProfileStringW(section, L"WatchdogNextRetryUnix", num, statePath.c_str())) return false;
    }
    return true;
}

} // namespace tcyc
