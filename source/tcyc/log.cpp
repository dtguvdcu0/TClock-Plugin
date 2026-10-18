#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <vector>

namespace tcyc {

namespace {

CRITICAL_SECTION g_logCs;
bool g_logCsInit = false;
std::wstring g_logPath;
int g_logLevel = 1;
const unsigned long long kRotateBytes = 1024ull * 1024ull;
const int kRotateCount = 5;

void EnsureCs() {
    if (!g_logCsInit) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = true;
    }
}

std::wstring MakeRotatedPath(const std::wstring& base, int idx) {
    wchar_t suffix[32] = {0};
    swprintf_s(suffix, L".%d", idx);
    return base + suffix;
}

void EnsureParentDir(const std::wstring& filePath) {
    size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;
    std::wstring dir = filePath.substr(0, pos);
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
}

void RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad)) return;
    ULARGE_INTEGER uli{};
    uli.HighPart = fad.nFileSizeHigh;
    uli.LowPart = fad.nFileSizeLow;
    if (uli.QuadPart < kRotateBytes) return;

    for (int i = kRotateCount - 1; i >= 1; --i) {
        std::wstring oldPath = MakeRotatedPath(g_logPath, i);
        std::wstring newPath = MakeRotatedPath(g_logPath, i + 1);
        MoveFileExW(oldPath.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::wstring first = MakeRotatedPath(g_logPath, 1);
    MoveFileExW(g_logPath.c_str(), first.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void AppendUtf8Line(const std::wstring& line) {
    HANDLE h = CreateFileW(
        g_logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    const DWORD sizeHigh = GetFileSize(h, nullptr);
    if (sizeHigh == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        CloseHandle(h);
        return;
    }

    if (sizeHigh == 0) {
        const BYTE bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        WriteFile(h, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr);
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Bytes <= 1) {
        CloseHandle(h);
        return;
    }
    std::vector<char> buf(static_cast<size_t>(utf8Bytes));
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buf.data(), utf8Bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(h, buf.data(), static_cast<DWORD>(utf8Bytes - 1), &written, nullptr);
    CloseHandle(h);
}

} // namespace

void LogInit(const std::wstring& logPath) {
    EnsureCs();
    EnterCriticalSection(&g_logCs);
    g_logPath = logPath;
    EnsureParentDir(g_logPath);
    LeaveCriticalSection(&g_logCs);
}

void LogSetLevel(int level) {
    EnsureCs();
    EnterCriticalSection(&g_logCs);
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    g_logLevel = level;
    LeaveCriticalSection(&g_logCs);
}

void LogWrite(int level, const wchar_t* fmt, ...) {
    EnsureCs();
    EnterCriticalSection(&g_logCs);
    if (g_logPath.empty()) {
        LeaveCriticalSection(&g_logCs);
        return;
    }
    if (g_logLevel <= 0 || level > g_logLevel) {
        LeaveCriticalSection(&g_logCs);
        return;
    }

    RotateIfNeeded();

    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t body[2048] = {0};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t line[2304] = {0};
    swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u][L%d] %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, body);

    AppendUtf8Line(line);
    LeaveCriticalSection(&g_logCs);
}

} // namespace tcyc
