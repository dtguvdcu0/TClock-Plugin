// Encoding-aware INI access shared by plugins.
//
// The Profile API (GetPrivateProfile*W / WritePrivateProfile*W) decodes every file without a UTF-16LE
// BOM as the ANSI code page. INIs written as UTF-8, such as the TClock host INI, must therefore be
// read and updated here. Lookup follows the Profile API rules: the first matching section and key,
// case-insensitive names, trimmed values, ';' comment lines, and one pair of enclosing quotes removed.
#pragma once

#include <windows.h>

#include <cstring>
#include <cwchar>
#include <string>

enum class IniEncoding { Missing, Utf16, Utf8, Utf8Bom, Legacy };

struct IniText {
    IniEncoding encoding = IniEncoding::Missing;
    std::wstring text;
};

inline std::wstring ini_trim_text(const std::wstring& value)
{
    const wchar_t* spaces = L" \t\r\n";
    const size_t first = value.find_first_not_of(spaces);
    if (first == std::wstring::npos) return {};
    return value.substr(first, value.find_last_not_of(spaces) - first + 1);
}

inline bool ini_read_bytes(const std::wstring& path, std::string& bytes)
{
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart < (16ll << 20);
    if (ok && size.QuadPart > 0) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE && read == bytes.size();
    }
    CloseHandle(file);
    if (!ok) bytes.clear();
    return ok;
}

inline bool ini_decode_utf8(const char* data, size_t size, std::wstring& out)
{
    out.clear();
    if (size == 0) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(size), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(size), out.data(), count) == count;
}

inline IniText ini_load_text(const std::wstring& path)
{
    IniText result;
    std::string bytes;
    if (!ini_read_bytes(path, bytes)) return result;
    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        result.encoding = IniEncoding::Utf16;
        result.text.resize((bytes.size() - 2) / sizeof(wchar_t));
        if (!result.text.empty()) std::memcpy(result.text.data(), bytes.data() + 2, result.text.size() * sizeof(wchar_t));
        return result;
    }
    const bool bom = bytes.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF;
    const size_t start = bom ? 3 : 0;
    if (ini_decode_utf8(bytes.data() + start, bytes.size() - start, result.text)) {
        result.encoding = bom ? IniEncoding::Utf8Bom : IniEncoding::Utf8;
        return result;
    }
    // Legacy compatibility boundary: content that is not valid UTF-8 was written by the Profile API in the
    // ANSI code page, which is also how the Profile API itself decodes it. Paths never take this route.
    result.encoding = IniEncoding::Legacy;
    const int count = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    result.text.resize(count > 0 ? static_cast<size_t>(count) : 0);
    if (count > 0) MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), result.text.data(), count);
    return result;
}

// Returns the trimmed section name when the line is a section header.
inline bool ini_parse_header(const std::wstring& line, std::wstring& name)
{
    if (line.empty() || line.front() != L'[') return false;
    const size_t close = line.find(L']');
    if (close == std::wstring::npos) return false;
    name = ini_trim_text(line.substr(1, close - 1));
    return true;
}

// Locates the first section/key pair. lineBegin/lineEnd bound the key line without its terminator.
inline bool ini_locate_key(const std::wstring& text, const wchar_t* section, const wchar_t* key,
                           size_t& lineBegin, size_t& lineEnd, size_t& insertAt, bool& sectionFound)
{
    sectionFound = false;
    insertAt = std::wstring::npos;
    bool inSection = false;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find(L'\n', pos);
        const size_t next = end == std::wstring::npos ? text.size() : end + 1;
        end = end == std::wstring::npos ? text.size() : end;
        size_t content = end;
        if (content > pos && text[content - 1] == L'\r') --content;
        const std::wstring line = ini_trim_text(text.substr(pos, content - pos));
        std::wstring header;
        if (ini_parse_header(line, header)) {
            if (inSection) return false;
            inSection = _wcsicmp(header.c_str(), section) == 0;
            if (inSection) {
                sectionFound = true;
                insertAt = next;
            }
        } else if (inSection && !line.empty()) {
            insertAt = next;
            const size_t eq = line.find(L'=');
            if (line.front() != L';' && eq != std::wstring::npos && _wcsicmp(ini_trim_text(line.substr(0, eq)).c_str(), key) == 0) {
                lineBegin = pos;
                lineEnd = content;
                return true;
            }
        }
        pos = next;
    }
    return false;
}

inline bool ini_find_value(const std::wstring& text, const wchar_t* section, const wchar_t* key, std::wstring& value)
{
    size_t begin = 0, end = 0, insertAt = 0;
    bool sectionFound = false;
    if (!ini_locate_key(text, section, key, begin, end, insertAt, sectionFound)) return false;
    const std::wstring line = text.substr(begin, end - begin);
    value = ini_trim_text(line.substr(line.find(L'=') + 1));
    if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    return true;
}

// GetPrivateProfileInt rules: leading decimal digits (or 0x hex), 0 for negative or non-numeric values.
inline int ini_parse_int(const std::wstring& value)
{
    const std::wstring text = ini_trim_text(value);
    if (text.empty() || text.front() == L'-') return 0;
    const bool hex = text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X');
    const unsigned long parsed = std::wcstoul(text.c_str() + (hex ? 2 : 0), nullptr, hex ? 16 : 10);
    return parsed > 0x7FFFFFFFul ? 0x7FFFFFFF : static_cast<int>(parsed);
}

inline std::wstring ini_get_string(const IniText& ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
{
    std::wstring value;
    return ini_find_value(ini.text, section, key, value) ? value : std::wstring(fallback ? fallback : L"");
}

inline int ini_get_int(const IniText& ini, const wchar_t* section, const wchar_t* key, int fallback)
{
    std::wstring value;
    return ini_find_value(ini.text, section, key, value) ? ini_parse_int(value) : fallback;
}

inline std::wstring ini_read_string(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback)
{
    return ini_get_string(ini_load_text(path), section, key, fallback);
}

inline int ini_read_int(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback)
{
    return ini_get_int(ini_load_text(path), section, key, fallback);
}

// Replaces the file content through a sibling temporary file so readers never see a partial file.
inline bool ini_replace_file(const std::wstring& path, const std::string& bytes)
{
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(directory.c_str(), L"ini", 0, temporary)) return false;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD written = 0;
        ok = (bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                                written == bytes.size())) && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
    }
    ok = ok && (ReplaceFileW(path.c_str(), temporary, nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE ||
                MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE);
    if (!ok) DeleteFileW(temporary);
    return ok;
}

// Writes one value. UTF-8 files keep their encoding, BOM, newline style and other lines unchanged.
// UTF-16, legacy ANSI and missing files keep the Profile API, which writes them in their own encoding.
inline bool ini_write_string(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    IniText ini = ini_load_text(path);
    if (ini.encoding != IniEncoding::Utf8 && ini.encoding != IniEncoding::Utf8Bom) {
        return WritePrivateProfileStringW(section, key, value, path.c_str()) != FALSE;
    }
    const std::wstring newline = ini.text.find(L"\r\n") != std::wstring::npos || ini.text.find(L'\n') == std::wstring::npos ? L"\r\n" : L"\n";
    const std::wstring entry = std::wstring(key) + L"=" + value;
    size_t begin = 0, end = 0, insertAt = 0;
    bool sectionFound = false;
    std::wstring& text = ini.text;
    if (ini_locate_key(text, section, key, begin, end, insertAt, sectionFound)) {
        const std::wstring line = text.substr(begin, end - begin);
        const size_t eq = line.find(L'=');
        text.replace(begin, end - begin, line.substr(0, eq + 1) + value);
    } else if (sectionFound) {
        if (insertAt == text.size() && !text.empty() && text.back() != L'\n') {
            text += newline;
            insertAt = text.size();
        }
        text.insert(insertAt, entry + newline);
    } else {
        if (!text.empty() && text.back() != L'\n') text += newline;
        text += L"[" + std::wstring(section) + L"]" + newline + entry + newline;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 && !text.empty()) return false;
    std::string bytes = ini.encoding == IniEncoding::Utf8Bom ? std::string("\xEF\xBB\xBF") : std::string();
    const size_t offset = bytes.size();
    bytes.resize(offset + static_cast<size_t>(size > 0 ? size : 0));
    if (size > 0) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), bytes.data() + offset, size, nullptr, nullptr);
    return ini_replace_file(path, bytes);
}
