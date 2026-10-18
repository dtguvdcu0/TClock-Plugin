#pragma once
#include "card_clipboard.h"
#include "card_assets.h"
#include "../ui/card_images.h"
#include <winhttp.h>
#include <atomic>
#pragma comment(lib, "winhttp.lib")

namespace tcard_download {
constexpr unsigned max_requests = 32;
constexpr ULONGLONG batch_ms = 30000;
struct Handle {
    HINTERNET value = nullptr;
    explicit Handle(HINTERNET handle) : value(handle) {}
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline bool split(const std::wstring& value, URL_COMPONENTS& parts)
{
    if (value.empty() || value.size() > 8192 || value.find(L'\0') != std::wstring::npos) return false;
    for (wchar_t c : value) if (c < 32 || c == 127) return false;
    parts = {}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(value.c_str(), static_cast<DWORD>(value.size()), 0, &parts)) return false;
    // Do not send credentials embedded in an image URL, including empty userinfo.
    const size_t authority = value.find(L"://");
    const size_t end = value.find_first_of(L"/?#", authority == std::wstring::npos ? 0 : authority + 3);
    return (parts.nScheme == INTERNET_SCHEME_HTTP || parts.nScheme == INTERNET_SCHEME_HTTPS) &&
        parts.dwHostNameLength && !parts.dwUserNameLength && !parts.dwPasswordLength &&
        value.substr(authority + 3, end == std::wstring::npos ? end : end - authority - 3).find(L'@') == std::wstring::npos;
}
inline std::wstring raster(const std::vector<BYTE>& bytes)
{
    if (bytes.empty() || bytes.size() > tcard_clip::max_image) return {};
    const wchar_t* type = nullptr;
    if (bytes.size() >= 8 && memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) type = L"png";
    else if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) type = L"jpeg";
    else if (bytes.size() >= 6 && (!memcmp(bytes.data(), "GIF87a", 6) || !memcmp(bytes.data(), "GIF89a", 6))) type = L"gif";
    else if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') type = L"bmp";
    if (!type) return {};
    auto uri = std::wstring(L"data:image/") + type + L";base64," + tcard_clip::base64(bytes);
    // Decode bounded pixels before persisting; MIME headers alone are not evidence of an image.
    return tcard_image::decode(uri).dib.empty() ? std::wstring{} : uri;
}
inline std::wstring request(std::wstring address, const std::atomic_bool& cancelled, ULONGLONG deadline, bool text = false)
{
    Handle session(WinHttpOpen(L"TCard/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value || !WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000)) return {};
    const size_t limit = text ? 2 * 1024 * 1024 : tcard_clip::max_image;
    for (unsigned redirect = 0; redirect <= 3; ++redirect) {
        if (cancelled || GetTickCount64() >= deadline) return {};
        URL_COMPONENTS parts{};
        if (!split(address, parts)) return {};
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : std::wstring{};
        if (path.empty()) path = L"/";
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const auto fragment = path.find(L'#'); if (fragment != std::wstring::npos) path.resize(fragment);
        Handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
        if (!connection.value) return {};
        const wchar_t* accept[] = {text ? L"text/html, text/css" : L"image/png, image/jpeg, image/gif, image/bmp", nullptr};
        Handle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, accept, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request.value) return {};
        DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
        DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD headerLimit = 32768;
        DWORD headerTimeout = 2000;
        if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy)) ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &headerLimit, sizeof(headerLimit)) ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &headerTimeout, sizeof(headerTimeout))) return {};
        // Some transports wait for response data inside SendRequest as well.
        const auto headerStarted = GetTickCount64();
        if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            cancelled || GetTickCount64() >= deadline) return {};
        if (!WinHttpReceiveResponse(request.value, nullptr) || cancelled || GetTickCount64() >= deadline ||
            GetTickCount64() - headerStarted >= headerTimeout) return {};
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return {};
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t location[8193]{}; length = sizeof(location);
            if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                location, &length, WINHTTP_NO_HEADER_INDEX)) return {};
            auto next = tcard_clip::url(location, address);
            URL_COMPONENTS destination{};
            if (!split(next, destination) || (parts.nScheme == INTERNET_SCHEME_HTTPS && destination.nScheme != INTERNET_SCHEME_HTTPS)) return {};
            address = std::move(next); continue;
        }
        if (status != 200) return {};
        DWORD contentLength = 0; length = sizeof(contentLength);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &length, WINHTTP_NO_HEADER_INDEX) && contentLength > limit) return {};
        std::vector<BYTE> bytes; bytes.reserve((std::min)(size_t(contentLength), limit));
        for (;;) {
            if (cancelled || GetTickCount64() >= deadline) return {};
            BYTE chunk[16384]; DWORD received = 0;
            if (!WinHttpReadData(request.value, chunk, sizeof(chunk), &received)) return {};
            if (!received) break;
            if (bytes.size() + received > limit) return {};
            bytes.insert(bytes.end(), chunk, chunk + received);
        }
        if (cancelled || GetTickCount64() >= deadline) return {};
        if (text) {
            try { return tcard_clip::utf8(std::string(bytes.begin(), bytes.end())); }
            catch (...) { return {}; }
        }
        return raster(bytes);
    }
    return {};
}
// Injectable fetcher keeps conversion/failure/budget tests independent of the network.
// Public GitHub attachment identities outlive their short-lived signed image URLs.
inline std::wstring attachment(const std::wstring& address)
{
    URL_COMPONENTS parts{};if(!split(address,parts))return {};
    const auto host=tcard_clip::lower(std::wstring(parts.lpszHostName,parts.dwHostNameLength));
    if(host!=L"private-user-images.githubusercontent.com")return {};
    const std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
    const auto slash=path.find_last_of(L'/'),dash=path.find(L'-',slash==std::wstring::npos?0:slash+1);
    if(dash==std::wstring::npos||path.size()<dash+38)return {};
    const auto id=path.substr(dash+1,36);
    for(size_t i=0;i<id.size();++i){
        const bool hyphen=i==8||i==13||i==18||i==23;
        if(hyphen?id[i]!=L'-':!iswxdigit(id[i]))return {};
    }
    if(path[dash+37]!=L'.')return {};
    return L"https://github.com/user-attachments/assets/"+id;
}
inline std::wstring fetch(const std::wstring& address,const std::atomic_bool& cancelled,ULONGLONG deadline)
{
    const auto stable=attachment(address);
    if(!stable.empty()){
        auto image=request(stable,cancelled,deadline);
        if(!image.empty())return image;
    }
    return request(address,cancelled,deadline);
}

inline tcard_clip::Clip embed(const tcard_clip::Clip& clip, const tcard_clip::ImageResolver& download,
    size_t sourceLimit = tcard_clip::max_source)
{
    if (!clip.externalImages || clip.htmlSource.empty() || clip.source.size() >= sourceLimit) return clip;
    size_t budget = sourceLimit - clip.source.size();
    std::map<std::wstring, std::wstring> cache;
    unsigned attempts = 0;
    auto resolve = [&](const std::wstring& address) -> std::wstring {
        URL_COMPONENTS parts{};
        if (!budget || !split(address, parts)) return {};
        auto found = cache.find(address);
        if (found == cache.end()) {
            if (attempts >= max_requests) return {};
            ++attempts;
            std::wstring image;
            try { image = download(address); } catch (...) { }
            if (image.size() + 1 > budget || tcard_clip::image_bytes(image).empty()) image.clear();
            found = cache.emplace(address, std::move(image)).first;
        }
        const auto& image = found->second;
        if (image.empty() || image.size() + 1 > budget) return {};
        budget -= image.size() + 1;
        return image;
    };
    auto result = tcard_clip::html(clip.htmlSource, clip.url, resolve);
    return result.source.size() <= sourceLimit ? result : clip;
}
inline tcard_clip::Clip embed(const tcard_clip::Clip& clip, const std::atomic_bool& cancelled,
    size_t sourceLimit = tcard_clip::max_source)
{
    const auto deadline = GetTickCount64() + batch_ms;
    return embed(clip, [&](const std::wstring& address) { return fetch(address, cancelled, deadline); }, sourceLimit);
}
inline tcard_clip::Clip materialize(const tcard_clip::Clip& clip, const std::wstring& root,
    const tcard_clip::ImageResolver& download, size_t sourceLimit = tcard_clip::max_source)
{
    if (!clip.externalImages || clip.htmlSource.empty()) return clip;
    std::map<std::wstring, std::wstring> cache;
    size_t remaining = 64 * 1024 * 1024;
    auto resolve = [&](const std::wstring& address) -> std::wstring {
        URL_COMPONENTS parts{}; if (!split(address, parts)) return {};
        auto found = cache.find(address);
        if (found == cache.end()) {
            if (cache.size() >= max_requests) return {};
            std::wstring name;
            try {
                const auto uri = remaining ? download(address) : std::wstring{};
                const auto bytes = tcard_clip::image_bytes(uri);
                if (!bytes.empty() && bytes.size() <= remaining) {
                    remaining -= bytes.size();
                    name = tcard_asset::image(root, uri);
                }
            } catch (...) { }
            found = cache.emplace(address, std::move(name)).first;
        }
        return found->second;
    };
    try {
        auto result = tcard_clip::html(clip.htmlSource, clip.url, resolve);
        return result.source.size() <= sourceLimit ? result : clip;
    } catch (...) { return clip; }
}
inline tcard_clip::Clip materialize(const tcard_clip::Clip& clip, const std::wstring& root,
    const std::atomic_bool& cancelled, size_t sourceLimit = tcard_clip::max_source)
{
    const auto deadline = GetTickCount64() + batch_ms;
    return materialize(clip, root, [&](const std::wstring& address) {
        if (cancelled) return std::wstring{};
        return fetch(address, cancelled, deadline);
    }, sourceLimit);
}
}
