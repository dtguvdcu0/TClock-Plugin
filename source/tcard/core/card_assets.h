#pragma once
#include "card_clipboard.h"
#include "card_storage.h"
#include "../ui/card_images.h"
#include <bcrypt.h>
#include <set>
#include <memory>
#include <cstdint>
#ifndef MD4C_USE_UTF16
#define MD4C_USE_UTF16
#endif
#include "../third_party/md4c/md4c.h"
#pragma comment(lib, "bcrypt.lib")

namespace tcard_asset {
constexpr size_t max_file = 16 * 1024 * 1024;
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit File(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
inline bool regular(HANDLE file, bool directory)
{
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}
inline HANDLE open_dir(const std::wstring& path, bool create)
{
    std::unique_ptr<File> parent;
    if (create && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t absolute[32768]{};
        const DWORD count = GetFullPathNameW(path.c_str(), 32768, absolute, nullptr);
        if (!count || count >= 32768) return INVALID_HANDLE_VALUE;
        const std::wstring full = absolute; const auto slash = full.find_last_of(L'\\');
        if (slash == std::wstring::npos) return INVALID_HANDLE_VALUE;
        parent = std::make_unique<File>(open_dir(full.substr(0, slash == 2 ? 3 : slash), false));
        if (!*parent) return INVALID_HANDLE_VALUE;
    }
    if (create && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return INVALID_HANDLE_VALUE;
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle != INVALID_HANDLE_VALUE && !regular(handle, true)) { CloseHandle(handle); return INVALID_HANDLE_VALUE; }
    if (handle != INVALID_HANDLE_VALUE) {
        wchar_t absolute[32768]{}, finalPath[32768]{};
        const DWORD a = GetFullPathNameW(path.c_str(), 32768, absolute, nullptr);
        const DWORD f = GetFinalPathNameByHandleW(handle, finalPath, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        const auto normalize = [](std::wstring value) {
            if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) value = L"\\\\" + value.substr(8);
            else if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
            while (value.size() > 3 && value.back() == L'\\') value.pop_back();
            return value;
        };
        if (!a || a >= 32768 || !f || f >= 32768 || _wcsicmp(normalize(absolute).c_str(), normalize(finalPath).c_str()) != 0) {
            CloseHandle(handle); return INVALID_HANDLE_VALUE;
        }
    }
    return handle;
}
inline std::wstring hash(const std::vector<BYTE>& bytes)
{
    if (bytes.size() > max_file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE digest = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BYTE value[32]{};
    bool ok = BCryptCreateHash(algorithm, &digest, nullptr, 0, nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(digest, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 && BCryptFinishHash(digest, value, sizeof(value), 0) >= 0;
    if (digest) BCryptDestroyHash(digest); BCryptCloseAlgorithmProvider(algorithm, 0);
    std::wstring result;
    if (ok) for (BYTE byte : value) { result += L"0123456789abcdef"[byte >> 4]; result += L"0123456789abcdef"[byte & 15]; }
    return result;
}
inline std::vector<BYTE> encode(const std::wstring& text)
{
    if (text.size() > max_file) throw std::runtime_error("Source too large");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count && !text.empty()) throw std::runtime_error("Invalid source encoding");
    std::vector<BYTE> bytes(count);
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), reinterpret_cast<LPSTR>(bytes.data()), count, nullptr, nullptr);
    return bytes;
}
inline std::wstring decode(const std::vector<BYTE>& bytes)
{
    size_t skip = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    return tcard_clip::utf8(std::string(bytes.begin() + skip, bytes.end()));
}
inline bool leaf(const std::wstring& name)
{
    return !name.empty() && name != L"." && name != L".." && name.size() < 240 &&
        name.back() != L'.' && name.back() != L' ' && name.find_first_of(L"\\/:*?\"<>|\r\n") == std::wstring::npos &&
        std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; });
}
inline bool relative(const std::wstring& name, bool image)
{
    const std::wstring prefix = image ? L"images/" : L"note-";
    if (name.rfind(prefix, 0) != 0 || name.size() < prefix.size() + 67) return false;
    const size_t dot = prefix.size() + 64;
    if (name[dot] != L'.') return false;
    for (size_t i = prefix.size(); i < dot; ++i) if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) return false;
    const auto extension = name.substr(dot);
    return image ? extension == L".png" || extension == L".jpg" || extension == L".gif" || extension == L".bmp" : extension == L".md" || extension == L".txt" || extension == L".html";
}
// Roots are host-owned paths, never supplied by Markdown. Hold directories against rename while accessing children.
inline bool read(const std::wstring& root, const std::wstring& name, std::vector<BYTE>& bytes, size_t limit = max_file)
{
    bytes.clear();
    File directory(open_dir(root, false)); if (!directory) return false;
    std::wstring path = root + L"\\";
    const auto slash = name.find(L'/');
    std::unique_ptr<File> child;
    if (slash != std::wstring::npos) {
        if (!relative(name, true) && !relative(name, false)) return false;
        path += name.substr(0, slash); child = std::make_unique<File>(open_dir(path, false));
        if (!*child) return false;
        path += L"\\" + name.substr(slash + 1);
    } else { if (!leaf(name)) return false; path += name; }
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file || !regular(file.value, false)) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 || static_cast<ULONGLONG>(size.QuadPart) > limit + tcard_crypto::envelopeOverhead) return false;
    bytes.resize(static_cast<size_t>(size.QuadPart)); DWORD count = 0;
    if (!bytes.empty() && (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())) { bytes.clear(); return false; }
    std::vector<BYTE> plain;
    if (!tcard_storage::Unpack(path, bytes, plain) || plain.size() > limit) { bytes.clear(); return false; }
    bytes = std::move(plain); return true;
}
inline bool publish(const std::wstring& root, const std::wstring& name, const std::vector<BYTE>& bytes, bool immutable = false)
{
    if (bytes.size() > max_file) return false;
    File directory(open_dir(root, true)); if (!directory) return false;
    std::wstring folder = root, finalName = name;
    std::unique_ptr<File> child;
    const auto slash = name.find(L'/');
    if (slash != std::wstring::npos) {
        if (!relative(name, true) && !relative(name, false)) return false;
        folder += L"\\" + name.substr(0, slash); finalName = name.substr(slash + 1);
        child = std::make_unique<File>(open_dir(folder, true)); if (!*child) return false;
    } else if (!leaf(name)) return false;
    const auto target = folder + L"\\" + finalName;
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
        if (immutable) { std::vector<BYTE> old; return read(root, name, old) && old == bytes; }
    }
    std::vector<BYTE> stored; if (!tcard_storage::Pack(target, bytes, stored)) return false;
    GUID guid{}; wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, 40)) return false;
    const auto temporary = folder + L"\\" + token + L".tmp";
    bool ok = false;
    {
        File file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return false;
        DWORD written = 0;
        ok = (stored.empty() || (WriteFile(file.value, stored.data(), static_cast<DWORD>(stored.size()), &written, nullptr) && written == stored.size())) && FlushFileBuffers(file.value);
    }
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH | (immutable ? 0 : MOVEFILE_REPLACE_EXISTING)) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}
// Remove only unreferenced immutable source versions after their replacement manifest is verified.
inline bool remove_version(const std::wstring& root,const std::wstring& name)
{
    if(!relative(name,false)||!leaf(name))return false;
    File directory(open_dir(root,false));if(!directory)return false;
    File file(CreateFileW((root+L"\\"+name).c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file)return GetLastError()==ERROR_FILE_NOT_FOUND;
    if(!regular(file.value,false))return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1||
       !GetFileSizeEx(file.value,&size)||size.QuadPart<0||size.QuadPart>max_file+tcard_crypto::envelopeOverhead)return false;
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));DWORD count=0;
    if(!bytes.empty()&&(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size()))return false;
    std::vector<BYTE> plain;
    if(!tcard_storage::Unpack(root+L"\\"+name,bytes,plain)||hash(plain)!=name.substr(5,64))return false;
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE;
}
inline bool version(const std::wstring& root, const std::wstring& text, bool markdown, std::wstring& name, bool richHtml = false)
{
    const auto bytes = encode(text); const auto digest = hash(bytes); if (digest.empty()) return false;
    name = L"note-" + digest + (richHtml ? L".html" : markdown ? L".md" : L".txt");
    std::vector<BYTE> verified;
    return publish(root, name, bytes, true) && read(root, name, verified) && verified == bytes;
}
inline std::wstring image(const std::wstring& root, const std::wstring& uri)
{
    const auto bytes = tcard_clip::image_bytes(uri);
    if (bytes.empty() || tcard_image::decode(uri).dib.empty()) return {};
    const auto digest = hash(bytes); if (digest.empty()) throw std::runtime_error("Image hash failed");
    const auto type = tcard_clip::lower(uri.substr(0, uri.find(L';')));
    const auto extension = type == L"data:image/png" ? L".png" : type == L"data:image/jpeg" ? L".jpg" : type == L"data:image/gif" ? L".gif" : L".bmp";
    const auto name = L"images/" + digest + extension;
    std::vector<BYTE> verified;
    if (!publish(root, name, bytes, true) || !read(root, name, verified, tcard_clip::max_image) || verified != bytes) throw std::runtime_error("Image could not be saved");
    return name;
}
inline std::wstring load_image(const std::wstring& root, const std::wstring& name)
{
    if (root.empty() || !relative(name, true)) return {};
    std::vector<BYTE> bytes;
    if (!read(root, name, bytes, tcard_clip::max_image) || hash(bytes) != name.substr(7, 64)) return {};
    const auto extension = name.substr(71);
    const auto type = extension == L".png" ? L"png" : extension == L".jpg" ? L"jpeg" : extension == L".gif" ? L"gif" : L"bmp";
    return std::wstring(L"data:image/") + type + L";base64," + tcard_clip::base64(bytes);
}
struct Destinations {
    const std::wstring* source;
    std::set<std::pair<size_t, size_t>> ranges;
    static int block(MD_BLOCKTYPE, void*, void*) { return 0; }
    static int leave(MD_SPANTYPE, void*, void*) { return 0; }
    static int text(MD_TEXTTYPE, const MD_CHAR*, MD_SIZE, void*) { return 0; }
    static int enter(MD_SPANTYPE type, void* detail, void* context) {
        if (type != MD_SPAN_IMG) return 0;
        auto& self = *static_cast<Destinations*>(context);
        const auto& src = static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src;
        const auto begin = reinterpret_cast<uintptr_t>(self.source->data()), at = reinterpret_cast<uintptr_t>(src.text);
        if (at < begin || (at - begin) % sizeof(wchar_t)) return 0;
        const size_t offset = (at - begin) / sizeof(wchar_t);
        if (offset <= self.source->size() && src.size <= self.source->size() - offset &&
            self.source->compare(offset, 11, L"data:image/") == 0) self.ranges.emplace(offset, src.size);
        return 0;
    }
};
inline std::wstring externalize(const std::wstring& root, const std::wstring& source)
{
    if (source.find(L"data:image/") == std::wstring::npos) return source;
    if (source.size() > tcard_clip::max_import) throw std::runtime_error("Source too large");
    Destinations destinations{&source, {}};
    MD_PARSER parser{}; parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_FOOTNOTES | MD_FLAG_ADMONITIONS;
    parser.enter_block = parser.leave_block = Destinations::block;
    parser.enter_span = Destinations::enter; parser.leave_span = Destinations::leave; parser.text = Destinations::text;
    if (md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, &destinations)) throw std::runtime_error("Markdown parse failed");
    std::wstring result = source;
    std::map<std::wstring, std::wstring> cache;
    for (auto it = destinations.ranges.rbegin(); it != destinations.ranges.rend(); ++it) {
        const auto uri = source.substr(it->first, it->second);
        auto found = cache.find(uri);
        if (found == cache.end()) {
            if (cache.size() >= 256) throw std::runtime_error("Too many distinct embedded images");
            found = cache.emplace(uri, image(root, uri)).first;
        }
        const auto& relativeName = found->second;
        if (!relativeName.empty()) result.replace(it->first, it->second, relativeName);
    }
    return result;
}
}
