#pragma once

#include <windows.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace tcard_lang {

inline std::wstring g_code = L"en";
inline std::wstring g_file;
inline std::unordered_map<std::wstring,std::wstring> g_strings;

inline std::wstring module_dir()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&module_dir), &module);
    std::wstring path(260, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (!length) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"." : path.substr(0, slash);
        }
        path.resize(path.size() * 2);
    }
}

inline std::wstring config_path()
{
    return module_dir() + L"\\TCard.ini";
}

inline std::wstring language_dir()
{
    const std::wstring primary = module_dir() + L"\\tcard\\lang";
    if (GetFileAttributesW(primary.c_str()) != INVALID_FILE_ATTRIBUTES) return primary;
    return module_dir() + L"\\lang";
}

inline std::wstring language_file(const std::wstring& code)
{
    if (code.empty() || code.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return {};
    return language_dir() + L"\\" + code + L".ini";
}

inline std::wstring read_catalog(const std::wstring& file)
{
    if (file.empty()) return {};
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(handle);
        return {};
    }
    std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    CloseHandle(handle);
    if (!ok || read != bytes.size()) return {};
    size_t offset = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf ? 3 : 0;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
        static_cast<int>(bytes.size() - offset), nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
        static_cast<int>(bytes.size() - offset), text.data(), length)) return {};
    return text;
}

inline std::wstring trim_catalog(std::wstring value)
{
    const size_t first = value.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t");
    return value.substr(first, last - first + 1);
}

inline void parse_catalog(const std::wstring& text)
{
    g_strings.clear();
    bool inside=false;
    size_t start=0;
    while(start<=text.size()){
        const size_t end=text.find(L'\n',start);
        auto line=text.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        line=trim_catalog(line);
        if(!line.empty()&&line.front()==L'['&&line.back()==L']')inside=line==L"[Strings]";
        else if(inside){
            const size_t equals=line.find(L'=');
            if(equals!=std::wstring::npos)g_strings.try_emplace(trim_catalog(line.substr(0,equals)),trim_catalog(line.substr(equals+1)));
        }
        if(end==std::wstring::npos)break;
        start=end+1;
    }
}

inline bool load(const std::wstring& code)
{
    // Catalog IO belongs to explicit initialization/settings changes, never painting or timers.
    const auto candidate=language_file(code);
    auto catalog=read_catalog(candidate);
    const bool found=!catalog.empty();
    g_code=found?code:L"en";
    g_file=found?candidate:language_file(L"en");
    if(!found)catalog=read_catalog(g_file);
    parse_catalog(catalog);
    return found;
}

inline void initialize()
{
    wchar_t configured[64]{};
    GetPrivateProfileStringW(L"TCard", L"Language", L"", configured, ARRAYSIZE(configured), config_path().c_str());
    if (!configured[0]) {
        const std::wstring parentIni = module_dir() + L"\\..\\tclock-win11.ini";
        wchar_t inherited[16]{};
        GetPrivateProfileStringW(L"Main", L"EnglishMenu", L"", inherited, ARRAYSIZE(inherited), parentIni.c_str());
        const wchar_t* code = wcscmp(inherited, L"0") == 0 ? L"ja" : wcscmp(inherited, L"1") == 0 ? L"en" : nullptr;
        if (code) {
            wcscpy_s(configured, code);
            WritePrivateProfileStringW(L"TCard", L"Language", code, config_path().c_str());
        }
    }
    load(configured[0] ? configured : L"en");
}

inline std::wstring text(const wchar_t* key, const wchar_t* fallback)
{
    const auto value=g_strings.find(key);
    return value==g_strings.end()?std::wstring(fallback):value->second;
}

}
