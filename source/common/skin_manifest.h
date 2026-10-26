#ifndef SKN_MANIFEST_H
#define SKN_MANIFEST_H
#include <windows.h>
#include <string>
#include <vector>
#include <cwctype>

// UTF-8/UTF-16 manifests and asset filenames stay Unicode at the filesystem boundary.
class SKN_MANIFEST {
    struct Entry { std::wstring section, key, value; };
    std::vector<Entry> entries;
    static std::wstring trim(const std::wstring& text) {
        size_t first = text.find_first_not_of(L" \t\r");
        return first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t\r")-first+1);
    }
public:
    const WCHAR* read(const WCHAR* section, const WCHAR* key) const {
        for (const auto& entry : entries)
            if (!_wcsicmp(entry.section.c_str(), section) && !_wcsicmp(entry.key.c_str(), key)) return entry.value.c_str();
        return nullptr;
    }
    bool matches(const WCHAR* section, const WCHAR* key, const WCHAR* value) const {
        const WCHAR* actual = read(section, key);
        return actual && !_wcsicmp(actual, value);
    }
    bool load(const WCHAR* path) {
        entries.clear();
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024*1024) { CloseHandle(file); return false; }
        std::vector<BYTE> bytes((size_t)size.QuadPart);
        DWORD count = 0;
        bool success = ReadFile(file, bytes.data(), (DWORD)bytes.size(), &count, nullptr) && count == bytes.size();
        CloseHandle(file);
        if (!success) return false;
        std::wstring text;
        if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
            if (bytes.size()%2) return false;
            for (size_t i = 2; i < bytes.size(); i += 2) text.push_back((WCHAR)(bytes[i] | (bytes[i+1]<<8)));
        } else {
            size_t offset = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
            // File contents are UTF-8, not a narrow filesystem path or command.
            const char* data = reinterpret_cast<const char*>(bytes.data()+offset);
            int length = (int)(bytes.size()-offset);
            int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
            if (!wide) return false;
            text.resize(wide);
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, length, &text[0], wide) != wide) return false;
        }
        if (text.find(L'\0') != std::wstring::npos) return false;
        std::wstring section;
        for (size_t first = 0; first < text.size();) {
            size_t end = text.find(L'\n', first);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = trim(text.substr(first, end-first));
            first = end+1;
            if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
            if (line[0] == L'[') {
                if (line.size() < 3 || line.back() != L']') return false;
                section = trim(line.substr(1, line.size()-2));
                if (section.empty()) return false;
                continue;
            }
            size_t equal = line.find(L'=');
            if (section.empty() || equal == std::wstring::npos) return false;
            std::wstring key = trim(line.substr(0, equal)), value = trim(line.substr(equal+1));
            if (key.empty() || read(section.c_str(), key.c_str())) return false;
            entries.push_back({section, key, value});
        }
        return !entries.empty();
    }
};

static inline bool skn_read_color(const SKN_MANIFEST& manifest, const WCHAR* section, const WCHAR* key, unsigned long& rgb)
{
    const WCHAR* value = manifest.read(section, key);
    if (!value || wcslen(value) != 6) return false;
    for (int i = 0; i < 6; ++i) if (!iswxdigit(value[i])) return false;
    WCHAR* end;
    rgb = wcstoul(value, &end, 16);
    return !*end && rgb <= 0xffffff;
}

static inline bool skn_resolve_asset(const std::wstring& root, const WCHAR* value, std::wstring& path)
{
    if (!value || !*value || value[0] == L'\\' || value[0] == L'/') return false;
    std::wstring relative(value);
    if (relative.find(L':') != std::wstring::npos) return false;
    for (size_t first = 0; first <= relative.size();) {
        size_t end = relative.find_first_of(L"\\/", first);
        if (end == std::wstring::npos) end = relative.size();
        std::wstring part = relative.substr(first, end-first);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        if (end == relative.size()) break;
        first = end+1;
    }
    path = root+relative;
    return path.size() < 32768;
}
#endif
