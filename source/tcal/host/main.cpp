#include "tcalendar_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "WebView2.h"
#include <wrl.h>

#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cwctype>

#include "task_store.h"
#include "resource.h"

namespace {

constexpr const wchar_t* kTCalendarWindowClassName = L"TCalendarStandaloneWindowClass";
constexpr const wchar_t* kTCalendarUiMutexName = L"Local\\TCalendar.Win10mod.UIInstance";
constexpr const wchar_t* kTCalendarAlertMutexName = L"Local\\TCalendar.Win10mod.AlertInstance";
constexpr const wchar_t* kTCalendarIniFileName = L"tcalendar.ini";

struct WindowContext {
    tcalendar::TCalendarHost* host = nullptr;
    std::wstring initial_uri;
    std::wstring webview_user_data_dir;
    std::wstring ini_file_path;
    bool block_external_navigation = true;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    bool webview_ready = false;
    bool app_ready = false;
    bool startup_log_enabled = false;
    std::wstring startup_log_path;
    ULONGLONG startup_tick = 0;
    HRESULT init_hr = E_PENDING;
};

int get_frame_px(HWND hwnd);
int get_drag_px(HWND hwnd);

void LogStartupMark(WindowContext* context, const wchar_t* label) {
    if (!context || !context->startup_tick || !label) {
        return;
    }

    const ULONGLONG elapsed_ms = GetTickCount64() - context->startup_tick;
    wchar_t line[192] = {0};
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"[TCalendar][startup] +%llums %s\n", elapsed_ms, label);
    OutputDebugStringW(line);

    if (context->startup_log_enabled && !context->startup_log_path.empty()) {
        std::wofstream log(std::filesystem::path(context->startup_log_path), std::ios::app);
        if (log) {
            log << line;
        }
    }
}

void PaintStartupSkeleton(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    HBRUSH background = CreateSolidBrush(RGB(238, 242, 247));
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(27, 31, 36));

    HFONT title_font = CreateFontW(
        -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, title_font));

    RECT title_rect{32, 28, rc.right - 32, 62};
    DrawTextW(hdc, L"TCalendar", -1, &title_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, old_font);
    DeleteObject(title_font);

    HBRUSH panel = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH rail = CreateSolidBrush(RGB(217, 223, 232));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(213, 219, 228));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border));
    HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, panel));

    const int gap = 16;
    const int top = 86;
    const int bottom = rc.bottom - 28;
    const int right_width = max(320, min(420, (rc.right - 96) / 3));
    const int left = 28;
    const int right = rc.right - 28;
    const int split = right - right_width - gap;

    RoundRect(hdc, left, top, split, bottom, 12, 12);
    RoundRect(hdc, split + gap, top, right, bottom, 12, 12);

    SelectObject(hdc, rail);
    for (int row = 0; row < 6; ++row) {
        const int y = top + 28 + row * 34;
        RoundRect(hdc, left + 24, y, split - 24, y + 14, 8, 8);
    }
    for (int row = 0; row < 5; ++row) {
        const int y = top + 28 + row * 38;
        RoundRect(hdc, split + gap + 24, y, right - 24, y + 16, 8, 8);
    }

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(panel);
    DeleteObject(rail);
    DeleteObject(border);

    EndPaint(hwnd, &ps);
}

enum class RuntimeMode {
    Ui,
    Alert,
    Smoke,
    Compare,
};

struct RuntimeArgs {
    RuntimeMode mode = RuntimeMode::Ui;
    bool smoke_storage_error_mode = false;
    bool smoke_strict_mode = false;
    std::wstring compare_years_csv;
};

bool ParseDate(const std::wstring& value, int& y, int& m, int& d) {
    if (value.size() != 10 || value[4] != L'-' || value[7] != L'-') return false;
    if (!iswdigit(value[0]) || !iswdigit(value[1]) || !iswdigit(value[2]) || !iswdigit(value[3]) ||
        !iswdigit(value[5]) || !iswdigit(value[6]) || !iswdigit(value[8]) || !iswdigit(value[9])) {
        return false;
    }
    y = (value[0] - L'0') * 1000 + (value[1] - L'0') * 100 + (value[2] - L'0') * 10 + (value[3] - L'0');
    m = (value[5] - L'0') * 10 + (value[6] - L'0');
    d = (value[8] - L'0') * 10 + (value[9] - L'0');
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    return true;
}

bool ParseTimeHM(const std::wstring& value, int& hh, int& mm) {
    if (value.size() != 5 || value[2] != L':') return false;
    if (!iswdigit(value[0]) || !iswdigit(value[1]) || !iswdigit(value[3]) || !iswdigit(value[4])) return false;
    hh = (value[0] - L'0') * 10 + (value[1] - L'0');
    mm = (value[3] - L'0') * 10 + (value[4] - L'0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    return true;
}

time_t FloorToMinute(time_t t) {
    std::tm local_tm{};
    localtime_s(&local_tm, &t);
    local_tm.tm_sec = 0;
    return mktime(&local_tm);
}

bool BuildTaskStartLocalMinute(const tcalendar::TaskItem& task, time_t& out_local_minute) {
    int y = 0, m = 0, d = 0;
    int hh = 0, mm = 0;
    if (!ParseDate(task.date, y, m, d)) return false;
    if (!ParseTimeHM(task.start_time, hh, mm)) return false;

    std::tm tm_local{};
    tm_local.tm_year = y - 1900;
    tm_local.tm_mon = m - 1;
    tm_local.tm_mday = d;
    tm_local.tm_hour = hh;
    tm_local.tm_min = mm;
    tm_local.tm_sec = 0;
    tm_local.tm_isdst = -1;

    const time_t t = mktime(&tm_local);
    if (t == static_cast<time_t>(-1)) return false;
    out_local_minute = t;
    return true;
}

bool IsTaskDateToday(const tcalendar::TaskItem& task, const std::tm& now_tm) {
    int y = 0, m = 0, d = 0;
    if (!ParseDate(task.date, y, m, d)) return false;
    return (y == now_tm.tm_year + 1900) && (m == now_tm.tm_mon + 1) && (d == now_tm.tm_mday);
}

bool ParseRuntimeArgs(int argc, wchar_t** argv, RuntimeArgs& out, std::wstring& out_error) {
    out = RuntimeArgs{};
    out_error.clear();

    bool saw_ui = false;
    bool saw_alert = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg == L"--compare-jp-provider") {
            out.mode = RuntimeMode::Compare;
            if (i + 1 >= argc) {
                out_error = L"Missing year list after --compare-jp-provider";
                return false;
            }
            out.compare_years_csv = argv[++i] ? argv[i] : L"";
            continue;
        }
        if (arg.rfind(L"--compare-jp-provider=", 0) == 0) {
            out.mode = RuntimeMode::Compare;
            out.compare_years_csv = arg.substr(wcslen(L"--compare-jp-provider="));
            continue;
        }
        if (arg == L"--smoke") {
            out.mode = RuntimeMode::Smoke;
            continue;
        }
        if (arg == L"--smoke-storage-error") {
            out.mode = RuntimeMode::Smoke;
            out.smoke_storage_error_mode = true;
            continue;
        }
        if (arg == L"--smoke-storage-error-strict") {
            out.mode = RuntimeMode::Smoke;
            out.smoke_storage_error_mode = true;
            out.smoke_strict_mode = true;
            continue;
        }
        if (arg == L"--ui") {
            saw_ui = true;
            continue;
        }
        if (arg == L"--alert") {
            saw_alert = true;
            continue;
        }
        out_error = L"Unknown option: " + arg;
        return false;
    }

    if (out.mode == RuntimeMode::Smoke) {
        if (saw_ui || saw_alert) {
            out_error = L"Smoke mode cannot be combined with --ui/--alert";
            return false;
        }
        return true;
    }

    if (out.mode == RuntimeMode::Compare) {
        if (saw_ui || saw_alert) {
            out_error = L"Compare mode cannot be combined with --ui/--alert";
            return false;
        }
        if (out.compare_years_csv.empty()) {
            out_error = L"Missing compare year list";
            return false;
        }
        return true;
    }

    if (saw_ui && saw_alert) {
        out_error = L"--ui and --alert cannot be combined";
        return false;
    }
    if (saw_alert) {
        out.mode = RuntimeMode::Alert;
    } else {
        out.mode = RuntimeMode::Ui;
    }
    return true;
}

std::wstring BuildNotifyBody(const tcalendar::TaskItem& task) {
    std::wstring body;
    body.reserve(512);
    body += task.date;
    if (!task.start_time.empty()) {
        body += L" ";
        body += task.start_time;
    }
    body += L"\n";
    body += task.title;
    if (!task.detail.empty()) {
        body += L"\n";
        body += task.detail;
    }
    return body;
}

std::wstring BuildNotifyKey(const tcalendar::TaskItem& task) {
    std::wstring key;
    key.reserve(task.id.size() + task.date.size() + task.start_time.size() + 3);
    key += task.id;
    key += L"|";
    key += task.date;
    key += L"|";
    key += task.start_time;
    return key;
}
void ShowAlertDialogWithAppIcon(const std::wstring& body) {
    MSGBOXPARAMSW params{};
    params.cbSize = sizeof(params);
    params.hwndOwner = nullptr;
    params.hInstance = GetModuleHandleW(nullptr);
    params.lpszText = body.c_str();
    params.lpszCaption = L"TClock-Win11";
    params.dwStyle = MB_OK | MB_SETFOREGROUND | MB_TOPMOST | MB_SYSTEMMODAL | MB_USERICON;
    params.lpszIcon = MAKEINTRESOURCEW(IDI_APPICON);
    if (MessageBoxIndirectW(&params) == 0) {
        MessageBoxW(nullptr, body.c_str(), L"TClock-Win11", MB_OK | MB_SETFOREGROUND | MB_TOPMOST | MB_SYSTEMMODAL | MB_ICONINFORMATION);
    }
}

int RunAlertMode(const tcalendar::HostConfig& config) {
    tcalendar::TaskStore store;
    if (!store.Initialize(config.storage_db_path)) {
        std::wstring text = L"TCalendar alert storage init failed.\n\n" + store.GetLastError();
        MessageBoxW(nullptr, text.c_str(), L"TCalendar", MB_OK | MB_ICONERROR);
        return 1;
    }

    int window_minutes = 60;

    int tick_seconds = config.alert_dispatch_tick_seconds;
    if (tick_seconds < 60) tick_seconds = 60;

    int refresh_minutes = config.alert_refresh_minutes;
    if (refresh_minutes < 1) refresh_minutes = 1;

    std::unordered_set<std::wstring> delivered;
    std::vector<tcalendar::TaskItem> due_queue;

    auto refresh_due_queue = [&]() {
        if (!store.ReloadFromDb()) {
            return;
        }

        const time_t now = time(nullptr);
        const time_t now_minute = FloorToMinute(now);
        const time_t to_time = now_minute + static_cast<time_t>(window_minutes) * 60;

        std::tm from_tm{};
        localtime_s(&from_tm, &now_minute);
        wchar_t from_buf[16] = {0};
        wcsftime(from_buf, _countof(from_buf), L"%Y-%m-%d", &from_tm);

        std::tm to_tm{};
        localtime_s(&to_tm, &to_time);
        wchar_t to_buf[16] = {0};
        wcsftime(to_buf, _countof(to_buf), L"%Y-%m-%d", &to_tm);

        due_queue.clear();
        const auto all = store.GetTasksInRange(from_buf, to_buf);
        due_queue.reserve(all.size());
        for (const auto& t : all) {
            if (t.done) continue;
            if (!t.alert_enabled) continue;
            due_queue.push_back(t);
        }
    };

    std::filesystem::file_time_type last_write = (std::filesystem::file_time_type::min)();
    std::error_code ec;
    if (std::filesystem::exists(config.storage_db_path, ec)) {
        last_write = std::filesystem::last_write_time(config.storage_db_path, ec);
    }

    auto last_full_refresh = std::chrono::steady_clock::now() - std::chrono::minutes(refresh_minutes);
    refresh_due_queue();

    while (true) {
        const auto now_steady = std::chrono::steady_clock::now();
        bool need_refresh = false;

        if (now_steady - last_full_refresh >= std::chrono::minutes(refresh_minutes)) {
            need_refresh = true;
            last_full_refresh = now_steady;
        }

        ec.clear();
        if (std::filesystem::exists(config.storage_db_path, ec)) {
            const auto current_write = std::filesystem::last_write_time(config.storage_db_path, ec);
            if (!ec && current_write != last_write) {
                last_write = current_write;
                need_refresh = true;
            }
        }

        if (need_refresh) {
            refresh_due_queue();
        }

        const time_t now = time(nullptr);
        const time_t now_minute = FloorToMinute(now);
        std::tm now_tm{};
        localtime_s(&now_tm, &now);

        for (const auto& task : due_queue) {
            const std::wstring key = BuildNotifyKey(task);
            if (delivered.find(key) != delivered.end()) continue;

            if (task.start_time.empty()) {
                if (!IsTaskDateToday(task, now_tm)) continue;
            } else {
                time_t task_minute = 0;
                if (!BuildTaskStartLocalMinute(task, task_minute)) continue;
                if (task_minute != now_minute) continue;
            }

            const std::wstring body = BuildNotifyBody(task);
            if (config.alert_sound_enabled) {
                const wchar_t* sound_path = config.alert_sound_path.empty()
                    ? L"C:\\Windows\\Media\\notify.wav"
                    : config.alert_sound_path.c_str();
                PlaySoundW(sound_path, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
            }
            ShowAlertDialogWithAppIcon(body);
            delivered.insert(key);
        }

        int sleep_seconds = tick_seconds;
        if (tick_seconds <= 60) {
            std::tm sleep_tm{};
            localtime_s(&sleep_tm, &now);
            sleep_seconds = 60 - sleep_tm.tm_sec;
            if (sleep_seconds <= 0 || sleep_seconds > 60) sleep_seconds = 60;
        }
        std::this_thread::sleep_for(std::chrono::seconds(sleep_seconds));
    }
}

std::wstring BuildFileUriFromPath(const std::wstring& path) {
    std::wstring uri = L"file:///";
    uri.reserve(uri.size() + path.size() + 16);

    for (const wchar_t ch : path) {
        if (ch == L'\\') {
            uri.push_back(L'/');
        } else if (ch == L' ') {
            uri += L"%20";
        } else {
            uri.push_back(ch);
        }
    }
    return uri;
}


std::filesystem::path GetExecutableDirectory() {
    wchar_t module_path[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(module_path).parent_path();
}

std::filesystem::path ResolvePathFromExe(const std::filesystem::path& exe_dir, const std::wstring& raw_path) {
    if (raw_path.empty()) {
        return std::filesystem::path();
    }
    const std::filesystem::path p(raw_path);
    if (p.is_absolute()) {
        return p;
    }
    return exe_dir / p;
}

std::filesystem::path ResolveTClockIniFromExe(const std::filesystem::path& exe_dir) {
    const std::filesystem::path exe_local = exe_dir / L"tclock-win11.ini";
    if (std::filesystem::exists(exe_local)) {
        return exe_local;
    }
    return exe_dir.parent_path() / L"tclock-win11.ini";
}

std::wstring TrimAsciiWhitespace(const std::wstring& value) {
    size_t start = 0;
    while (start < value.size() && iswspace(value[start])) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && iswspace(value[end - 1])) {
        --end;
    }
    return value.substr(start, end - start);
}

bool DecodeIniText(const std::string& text, std::wstring& out_value) {
    out_value.clear();
    if (text.empty()) {
        return true;
    }
    const int utf8_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (utf8_count > 0) {
        out_value.resize(static_cast<size_t>(utf8_count));
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                   out_value.data(), utf8_count) > 0;
    }
    out_value.clear();
    out_value.reserve(text.size());
    for (unsigned char ch : text) {
        out_value.push_back(static_cast<wchar_t>(ch));
    }
    return true;
}

std::wstring ReadMergedIniValue(const std::wstring& ini_path, const std::wstring& section, const std::wstring& key) {
    std::ifstream input(std::filesystem::path(ini_path), std::ios::binary);
    if (!input) {
        return L"";
    }

    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::wstring text;
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t wchar_count = (bytes.size() - 2) / sizeof(wchar_t);
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), wchar_count);
    } else {
        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }
        if (!DecodeIniText(bytes, text)) {
            return L"";
        }
    }

    std::wstring current_section;
    std::wstring last_value;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L'\n', start);
        const size_t line_end = (end == std::wstring::npos) ? text.size() : end;
        std::wstring line = text.substr(start, line_end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }

        const std::wstring trimmed = TrimAsciiWhitespace(line);
        if (!trimmed.empty() && trimmed[0] != L';' && trimmed[0] != L'#') {
            if (trimmed.size() >= 2 && trimmed.front() == L'[' && trimmed.back() == L']') {
                current_section = TrimAsciiWhitespace(trimmed.substr(1, trimmed.size() - 2));
            } else if (_wcsicmp(current_section.c_str(), section.c_str()) == 0) {
                const size_t eq = trimmed.find(L'=');
                if (eq != std::wstring::npos) {
                    const std::wstring line_key = TrimAsciiWhitespace(trimmed.substr(0, eq));
                    if (_wcsicmp(line_key.c_str(), key.c_str()) == 0) {
                        const std::wstring candidate_value = TrimAsciiWhitespace(trimmed.substr(eq + 1));
                        if (!candidate_value.empty()) {
                            last_value = candidate_value;
                        }
                    }
                }
            }
        }

        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return last_value;
}


void EnsureTCalendarIni(const std::filesystem::path& exe_dir) {
    const std::filesystem::path ini_path = exe_dir / kTCalendarIniFileName;
    if (std::filesystem::exists(ini_path)) {
        return;
    }

    WritePrivateProfileStringW(L"TCalendar", L"Skin", L"default", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"TemplateDefault", L"tcalendar\\template\\default\\index.html", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"TemplateUser", L"tcalendar\\template\\user\\index.html", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"StorageDbPath", L"tcalendar\\data\\tasks.db", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowWidth", L"960", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowHeight", L"640", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowLeft", L"", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowTop", L"", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"BlockExternalNavigation", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"EnableWebView2Bootstrap", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"DefaultViewMode", L"list", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"DefaultRangePresetDays", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"DefaultCustomRangeDays", L"7", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"DefaultUseCustomRange", L"0", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiFontFamily", L"Segoe UI", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiBaseFontSize", L"14", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiCalendarDateFontSize", L"13", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiTaskFontSize", L"14", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiPanelRightWidth", L"420", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiCalendarHeight", L"420", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"UiShowTaskPanel", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertScanWindowMinutes", L"60", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertDispatchTickSeconds", L"60", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertRefreshMinutes", L"10", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertGraceMinutes", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertSoundEnabled", L"1", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"AlertSoundPath", L"C:\\Windows\\Media\\notify.wav", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionFiles", L"", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionCatalog", L"", ini_path.wstring().c_str());
    WritePrivateProfileStringW(L"TCalendar", L"StartupLogEnabled", L"0", ini_path.wstring().c_str());
}

void LoadHostConfigFromIni(const std::filesystem::path& exe_dir, bool smoke_mode, bool smoke_storage_error_mode, tcalendar::HostConfig& out_config) {
    EnsureTCalendarIni(exe_dir);

    const std::filesystem::path runtime_root = exe_dir / L"tcalendar";
    std::error_code ec;
    std::filesystem::create_directories(runtime_root / L"data", ec);
    std::filesystem::create_directories(runtime_root / L"template" / L"default", ec);
    std::filesystem::create_directories(runtime_root / L"template" / L"user", ec);

    const std::filesystem::path ini_path = exe_dir / kTCalendarIniFileName;
    const std::wstring ini_file = ini_path.wstring();
    out_config.ini_file_path = ini_file;
    (void)tcalendar::CanonicalizeTCalendarIni(ini_file);

    // Migration cleanup: alert startup ownership moved to tclock-win11.ini [TCalendar].
    WritePrivateProfileStringW(L"TCalendar", L"Enable", nullptr, ini_file.c_str());
    WritePrivateProfileStringW(L"TCalendar", L"Alart", nullptr, ini_file.c_str());
    auto ensure_ini_key = [&](const wchar_t* key, const wchar_t* value) {
        wchar_t current[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"TCalendar", key, L"", current, MAX_PATH, ini_file.c_str());
        if (current[0] == L'\0') {
            WritePrivateProfileStringW(L"TCalendar", key, value, ini_file.c_str());
        }
    };
    ensure_ini_key(L"Skin", L"default");
    ensure_ini_key(L"WindowWidth", L"960");
    ensure_ini_key(L"WindowHeight", L"640");
    ensure_ini_key(L"WindowLeft", L"");
    ensure_ini_key(L"WindowTop", L"");
    ensure_ini_key(L"DefaultViewMode", L"list");
    ensure_ini_key(L"DefaultRangePresetDays", L"1");
    ensure_ini_key(L"DefaultCustomRangeDays", L"7");
    ensure_ini_key(L"DefaultUseCustomRange", L"0");
    ensure_ini_key(L"UiFontFamily", L"Segoe UI");
    ensure_ini_key(L"UiBaseFontSize", L"14");
    ensure_ini_key(L"UiCalendarDateFontSize", L"13");
    ensure_ini_key(L"UiTaskFontSize", L"14");
    ensure_ini_key(L"UiPanelRightWidth", L"420");
    ensure_ini_key(L"UiCalendarHeight", L"420");
    ensure_ini_key(L"UiShowTaskPanel", L"1");
    ensure_ini_key(L"AlertScanWindowMinutes", L"60");
    ensure_ini_key(L"AlertDispatchTickSeconds", L"60");
    ensure_ini_key(L"AlertRefreshMinutes", L"10");
    ensure_ini_key(L"AlertGraceMinutes", L"1");
    ensure_ini_key(L"AlertSoundEnabled", L"1");
    ensure_ini_key(L"AlertSoundPath", L"C:\\Windows\\Media\\notify.wav");
    ensure_ini_key(L"HolidaySubscriptionFiles", L"");
    ensure_ini_key(L"HolidaySubscriptionCatalog", L"");
    ensure_ini_key(L"StartupLogEnabled", L"0");

    wchar_t buf[MAX_PATH] = {0};
    wchar_t skin[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"TCalendar", L"Skin", L"default", skin, MAX_PATH, ini_file.c_str());

    GetPrivateProfileStringW(L"TCalendar", L"TemplateDefault", L"", buf, MAX_PATH, ini_file.c_str());
    if (buf[0] == L'\0') {
        std::wstring skin_name = skin[0] ? skin : L"default";
        const std::wstring default_template = std::wstring(L"tcalendar\\template\\") + skin_name + L"\\index.html";
        out_config.default_template_path = ResolvePathFromExe(exe_dir, default_template).wstring();
    } else {
        out_config.default_template_path = ResolvePathFromExe(exe_dir, buf).wstring();
    }

    GetPrivateProfileStringW(L"TCalendar", L"TemplateUser", L"tcalendar\\template\\user\\index.html", buf, MAX_PATH, ini_file.c_str());
    out_config.user_template_path = ResolvePathFromExe(exe_dir, buf).wstring();

    GetPrivateProfileStringW(L"TCalendar", L"StorageDbPath", L"tcalendar\\data\\tasks.db", buf, MAX_PATH, ini_file.c_str());
    std::filesystem::path db_path = ResolvePathFromExe(exe_dir, buf);
    if (smoke_mode) {
        db_path = runtime_root / L"data" / L"tasks-smoke.db";
    }
    out_config.storage_db_path = db_path.wstring();

    out_config.block_external_navigation = GetPrivateProfileIntW(L"TCalendar", L"BlockExternalNavigation", 1, ini_file.c_str()) != 0;
    out_config.enable_webview2_bootstrap = GetPrivateProfileIntW(L"TCalendar", L"EnableWebView2Bootstrap", 1, ini_file.c_str()) != 0;
    out_config.test_force_storage_write_failure = smoke_storage_error_mode;

    GetPrivateProfileStringW(L"TCalendar", L"DefaultViewMode", L"list", buf, MAX_PATH, ini_file.c_str());
    std::wstring view_mode = buf;
    if (view_mode != L"list" && view_mode != L"timeline") {
        view_mode = L"list";
    }
    out_config.default_view_mode = view_mode;

    int preset_days = GetPrivateProfileIntW(L"TCalendar", L"DefaultRangePresetDays", 1, ini_file.c_str());
    if (preset_days != 1 && preset_days != 7 && preset_days != 14 && preset_days != 30) {
        preset_days = 1;
    }
    out_config.default_range_preset_days = preset_days;

    int custom_days = GetPrivateProfileIntW(L"TCalendar", L"DefaultCustomRangeDays", 7, ini_file.c_str());
    if (custom_days < 1) custom_days = 1;
    if (custom_days > 365) custom_days = 365;
    out_config.default_custom_range_days = custom_days;
    out_config.default_use_custom_range = GetPrivateProfileIntW(L"TCalendar", L"DefaultUseCustomRange", 0, ini_file.c_str()) != 0;

    GetPrivateProfileStringW(L"TCalendar", L"UiFontFamily", L"Segoe UI", buf, MAX_PATH, ini_file.c_str());
    if (buf[0] == L'\0') {
        out_config.ui_font_family = L"Segoe UI";
    } else {
        out_config.ui_font_family = buf;
    }

    int ui_base_font_size = GetPrivateProfileIntW(L"TCalendar", L"UiBaseFontSize", 14, ini_file.c_str());
    if (ui_base_font_size < 9) ui_base_font_size = 9;
    if (ui_base_font_size > 28) ui_base_font_size = 28;
    out_config.ui_base_font_size = ui_base_font_size;

    int ui_calendar_date_font_size = GetPrivateProfileIntW(L"TCalendar", L"UiCalendarDateFontSize", 13, ini_file.c_str());
    if (ui_calendar_date_font_size < 9) ui_calendar_date_font_size = 9;
    if (ui_calendar_date_font_size > 28) ui_calendar_date_font_size = 28;
    out_config.ui_calendar_date_font_size = ui_calendar_date_font_size;

    int ui_task_font_size = GetPrivateProfileIntW(L"TCalendar", L"UiTaskFontSize", 14, ini_file.c_str());
    if (ui_task_font_size < 9) ui_task_font_size = 9;
    if (ui_task_font_size > 28) ui_task_font_size = 28;
    out_config.ui_task_font_size = ui_task_font_size;

    int ui_panel_right_width = GetPrivateProfileIntW(L"TCalendar", L"UiPanelRightWidth", 420, ini_file.c_str());
    if (ui_panel_right_width < 320) ui_panel_right_width = 320;
    if (ui_panel_right_width > 1600) ui_panel_right_width = 1600;
    out_config.ui_panel_right_width = ui_panel_right_width;

    int ui_calendar_height = GetPrivateProfileIntW(L"TCalendar", L"UiCalendarHeight", 420, ini_file.c_str());
    if (ui_calendar_height < 280) ui_calendar_height = 280;
    if (ui_calendar_height > 1200) ui_calendar_height = 1200;
    out_config.ui_calendar_height = ui_calendar_height;
    out_config.ui_show_task_panel = GetPrivateProfileIntW(L"TCalendar", L"UiShowTaskPanel", 1, ini_file.c_str()) != 0;

    // Ownership note: Alart toggle is stored in tclock-win11.ini [TCalendar], not in tcalendar.ini.
    const std::filesystem::path tclock_ini_path = ResolveTClockIniFromExe(exe_dir);
    out_config.tclock_ini_file_path = tclock_ini_path.wstring();
    out_config.tclock_alert_enabled =
        GetPrivateProfileIntW(L"TCalendar", L"Alart", 0, out_config.tclock_ini_file_path.c_str()) != 0;

    out_config.enable_task_start_notify = out_config.tclock_alert_enabled;

    out_config.alert_sound_enabled =
        GetPrivateProfileIntW(L"TCalendar", L"AlertSoundEnabled", 1, ini_file.c_str()) != 0;
    GetPrivateProfileStringW(L"TCalendar", L"AlertSoundPath", L"C:\\Windows\\Media\\notify.wav", buf, MAX_PATH, ini_file.c_str());
    if (buf[0] == L'\0') {
        out_config.alert_sound_path = L"C:\\Windows\\Media\\notify.wav";
    } else {
        out_config.alert_sound_path = buf;
    }
    const std::wstring merged_holiday_subscription_files =
        ReadMergedIniValue(ini_file, L"TCalendar", L"HolidaySubscriptionFiles");
    if (!merged_holiday_subscription_files.empty()) {
        out_config.holiday_subscription_files = merged_holiday_subscription_files;
    } else {
        GetPrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionFiles", L"", buf, MAX_PATH, ini_file.c_str());
        out_config.holiday_subscription_files = buf;
    }
    const std::wstring merged_holiday_subscription_catalog =
        ReadMergedIniValue(ini_file, L"TCalendar", L"HolidaySubscriptionCatalog");
    if (!merged_holiday_subscription_catalog.empty()) {
        out_config.holiday_subscription_catalog = merged_holiday_subscription_catalog;
    } else {
        GetPrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionCatalog", L"", buf, MAX_PATH, ini_file.c_str());
        out_config.holiday_subscription_catalog = buf;
    }

    out_config.alert_scan_window_minutes = 60;

    int alert_dispatch_tick_seconds = GetPrivateProfileIntW(L"TCalendar", L"AlertDispatchTickSeconds", 60, ini_file.c_str());
    if (alert_dispatch_tick_seconds < 30) alert_dispatch_tick_seconds = 30;
    if (alert_dispatch_tick_seconds > 3600) alert_dispatch_tick_seconds = 3600;
    out_config.alert_dispatch_tick_seconds = alert_dispatch_tick_seconds;

    int alert_refresh_minutes = GetPrivateProfileIntW(L"TCalendar", L"AlertRefreshMinutes", 10, ini_file.c_str());
    if (alert_refresh_minutes < 1) alert_refresh_minutes = 1;
    if (alert_refresh_minutes > 1440) alert_refresh_minutes = 1440;
    out_config.alert_refresh_minutes = alert_refresh_minutes;

    int alert_grace_minutes = GetPrivateProfileIntW(L"TCalendar", L"AlertGraceMinutes", 1, ini_file.c_str());
    if (alert_grace_minutes < 0) alert_grace_minutes = 0;
    if (alert_grace_minutes > 5) alert_grace_minutes = 5;
    out_config.alert_grace_minutes = alert_grace_minutes;

    out_config.startup_log_enabled = GetPrivateProfileIntW(L"TCalendar", L"StartupLogEnabled", 0, ini_file.c_str()) != 0;
    out_config.startup_log_path = (runtime_root / L"logs" / L"startup.log").wstring();
    if (out_config.startup_log_enabled) {
        std::filesystem::create_directories(runtime_root / L"logs", ec);
    }
}

void LoadWindowPlacementFromIni(const std::wstring& ini_file, int& x, int& y, int& width, int& height, bool& has_position) {
    int w = GetPrivateProfileIntW(L"TCalendar", L"WindowWidth", width, ini_file.c_str());
    int h = GetPrivateProfileIntW(L"TCalendar", L"WindowHeight", height, ini_file.c_str());
    if (w >= 320 && w <= 3840) {
        width = w;
    }
    if (h >= 240 && h <= 2160) {
        height = h;
    }

    wchar_t xbuf[32] = {0};
    wchar_t ybuf[32] = {0};
    GetPrivateProfileStringW(L"TCalendar", L"WindowLeft", L"", xbuf, _countof(xbuf), ini_file.c_str());
    GetPrivateProfileStringW(L"TCalendar", L"WindowTop", L"", ybuf, _countof(ybuf), ini_file.c_str());

    if (xbuf[0] == L'\0' || ybuf[0] == L'\0') {
        has_position = false;
        return;
    }

    const int parsed_x = _wtoi(xbuf);
    const int parsed_y = _wtoi(ybuf);
    const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_right = virtual_left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_bottom = virtual_top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (parsed_x > virtual_right - 80 || parsed_y > virtual_bottom - 40 ||
        parsed_x + width < virtual_left + 80 || parsed_y + height < virtual_top + 40) {
        has_position = false;
        return;
    }

    x = parsed_x;
    y = parsed_y;
    has_position = true;
}

void SaveWindowPlacementToIni(HWND hwnd, const std::wstring& ini_file) {
    if (!hwnd || ini_file.empty() || IsZoomed(hwnd) || IsIconic(hwnd)) {
        return;
    }

    RECT r{};
    if (!GetWindowRect(hwnd, &r)) {
        return;
    }

    const int width = r.right - r.left;
    const int height = r.bottom - r.top;
    if (width < 320 || height < 240) {
        return;
    }

    wchar_t wbuf[16] = {0};
    wchar_t hbuf[16] = {0};
    wchar_t xbuf[16] = {0};
    wchar_t ybuf[16] = {0};
    _snwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, L"%d", width);
    _snwprintf_s(hbuf, _countof(hbuf), _TRUNCATE, L"%d", height);
    _snwprintf_s(xbuf, _countof(xbuf), _TRUNCATE, L"%d", r.left);
    _snwprintf_s(ybuf, _countof(ybuf), _TRUNCATE, L"%d", r.top);

    WritePrivateProfileStringW(L"TCalendar", L"WindowWidth", wbuf, ini_file.c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowHeight", hbuf, ini_file.c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowLeft", xbuf, ini_file.c_str());
    WritePrivateProfileStringW(L"TCalendar", L"WindowTop", ybuf, ini_file.c_str());
    (void)tcalendar::CanonicalizeTCalendarIni(ini_file);
}

void ResizeWebViewToClient(WindowContext* context, HWND hwnd) {
    if (!context || !context->controller) return;

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    const int drag_height = get_drag_px(hwnd);
    if (bounds.bottom > drag_height) {
        bounds.top = drag_height;
    }
    context->controller->put_Bounds(bounds);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool IsTrustedWebViewUri(const WindowContext* context, const std::wstring& uri) {
    return context && _wcsicmp(uri.c_str(), context->initial_uri.c_str()) == 0;
}

bool StartWebView2ForWindow(WindowContext* context, HWND hwnd) {
    if (!context) return false;

    LogStartupMark(context, L"webview2 environment create begin");
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        context->webview_user_data_dir.empty() ? nullptr : context->webview_user_data_dir.c_str(),
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [context, hwnd](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                context->init_hr = result;
                LogStartupMark(context, SUCCEEDED(result) ? L"webview2 environment ready" : L"webview2 environment failed");
                if (FAILED(result) || !environment) {
                    return S_OK;
                }

                context->environment = environment;
                return environment->CreateCoreWebView2Controller(
                    hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [context, hwnd](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                            context->init_hr = controller_result;
                            LogStartupMark(context, SUCCEEDED(controller_result) ? L"webview2 controller ready" : L"webview2 controller failed");
                            if (FAILED(controller_result) || !controller) {
                                return S_OK;
                            }

                            context->controller = controller;
                            context->controller->get_CoreWebView2(&context->webview);
                            ResizeWebViewToClient(context, hwnd);

                            if (context->webview) {
                                EventRegistrationToken navigation_token{};
                                context->webview->add_NavigationStarting(
                                    Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                                        [context](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                            if (!context || !args || !context->block_external_navigation) return S_OK;
                                            LPWSTR uri = nullptr;
                                            const HRESULT uri_hr = args->get_Uri(&uri);
                                            const bool trusted = SUCCEEDED(uri_hr) && uri && IsTrustedWebViewUri(context, uri);
                                            if (uri) CoTaskMemFree(uri);
                                            if (!trusted) args->put_Cancel(TRUE);
                                            return S_OK;
                                        }).Get(), &navigation_token);

                                EventRegistrationToken web_message_token{};
                                context->webview->add_WebMessageReceived(
                                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [context, hwnd](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            if (!context || !context->host || !sender || !args) {
                                                return S_OK;
                                            }

                                            LPWSTR source_uri = nullptr;
                                            const HRESULT source_hr = args->get_Source(&source_uri);
                                            const bool trusted = SUCCEEDED(source_hr) && source_uri && IsTrustedWebViewUri(context, source_uri);
                                            if (source_uri) CoTaskMemFree(source_uri);
                                            if (!trusted) return S_OK;

                                            LPWSTR message_text = nullptr;
                                            const HRESULT message_hr = args->TryGetWebMessageAsString(&message_text);
                                            if (FAILED(message_hr) || !message_text) {
                                                return S_OK;
                                            }

                                            std::wstring request_json(message_text);
                                            CoTaskMemFree(message_text);

                                            std::wstring response_json;
                                            if (!context->host->HandleWebMessage(request_json, response_json)) {
                                                return S_OK;
                                            }

                                            if (request_json.find(L"\"method\":\"system.appReady\"") != std::wstring::npos) {
                                                context->app_ready = true;
                                                context->webview_ready = true;
                                                LogStartupMark(context, L"web app first render ready");
                                                InvalidateRect(hwnd, nullptr, FALSE);
                                            }

                                            sender->PostWebMessageAsString(response_json.c_str());
                                            return S_OK;
                                        })
                                        .Get(),
                                    &web_message_token);

                                LogStartupMark(context, L"webview2 navigate begin");
                                context->webview->Navigate(context->initial_uri.c_str());
                            }
                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    context->init_hr = hr;
    return SUCCEEDED(hr);
}

void ActivateExistingTCalendarWindow() {
    HWND hwnd = FindWindowW(kTCalendarWindowClassName, nullptr);
    if (!hwnd) {
        return;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    SetForegroundWindow(hwnd);
}

int get_frame_px(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int get_drag_px(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    const int caption = GetSystemMetricsForDpi(SM_CYCAPTION, dpi);
    return max(8, caption / 3);
}

LRESULT CALLBACK TCalendarWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* context = reinterpret_cast<WindowContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }

    auto* context = reinterpret_cast<WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(hwnd, msg, wparam, lparam);
            if (hit != HTCLIENT) {
                return hit;
            }

            POINT cursor{
                static_cast<LONG>(static_cast<short>(LOWORD(lparam))),
                static_cast<LONG>(static_cast<short>(HIWORD(lparam)))
            };
            ScreenToClient(hwnd, &cursor);

            RECT client{};
            GetClientRect(hwnd, &client);
            const int frame = get_frame_px(hwnd);

            // Keep a minimal drag affordance after removing the native caption.
            if (cursor.y >= 0 && cursor.y < get_drag_px(hwnd) && cursor.x >= frame && cursor.x < client.right - frame) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (context && !context->webview_ready) {
                PaintStartupSkeleton(hwnd);
                return 0;
            }
            break;

        case WM_SIZE:
            ResizeWebViewToClient(context, hwnd);
            return 0;

        case WM_EXITSIZEMOVE:
            if (context) {
                SaveWindowPlacementToIni(hwnd, context->ini_file_path);
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_MAXIMIZE) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);

        case WM_DESTROY:
            if (context) {
                SaveWindowPlacementToIni(hwnd, context->ini_file_path);
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int RunSmokeMode(tcalendar::TCalendarHost& host, const std::filesystem::path& exe_dir, bool force_storage_error_test, bool strict_assert) {
    std::wstring response;
    const auto smoke_subscription_path = exe_dir / L"holiday-subscription-smoke.txt";
    bool smoke_subscription_written = false;

    {
        std::ofstream smoke_output(smoke_subscription_path, std::ios::binary | std::ios::trunc);
        if (smoke_output) {
            smoke_output << "2026-02-01|Smoke Existing|subscription\n";
            smoke_subscription_written = true;
        }
    }

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-1","method":"system.getVersion","params":{}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-2","method":"task.create","params":{"date":"2026-02-23","title":"hello"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-2b","method":"task.create","params":{"date":"2026-02-23","title":"quote \"x\" and slash \\"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-3","method":"calendar.getDayTasks","params":{"date":"2026-02-23"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-4","method":"task.toggleDone","params":{"id":"t-1","done":true}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-5","method":"task.create","params":{"date":true,"title":"bad"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-6","method":"task.create","params":{"date":"2026-02-23","title":"broken"})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-7","method":"task.create"})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-8","method":"task.updateTitle","params":{"id":"t-1","title":"hello2"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-9","method":"task.delete","params":{"id":"t-1"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-10","method":"calendar.getDayTasks","params":{"date":"2026-02-23"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-10b","method":"system.getStartupState","params":{"selectedDate":"2026-02-23","monthFrom":"2026-02-01","monthTo":"2026-02-28"}})", response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(
        LR"({"apiVersion":"1.0","requestId":"smoke-10c","method":"system.setViewConfig","params":{"defaultViewMode":"list","rangePreset":"1","customRangeDays":"7","uiFontFamily":"Segoe UI","uiBaseFontSize":"14","uiCalendarDateFontSize":"13","uiTaskFontSize":"14","uiPanelRightWidth":"420","uiCalendarHeight":"420","uiShowTaskPanel":"1","tclockAlertEnabled":"0","alertSoundEnabled":"1","alertSoundPath":"C:\\Windows\\Media\\notify.wav","holidaySubscriptionFiles":"jp-public-holiday","holidaySubscriptionCatalog":"1 jp-public-holiday"}})",
        response);
    std::wcout << response << std::endl;

    host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-10d","method":"system.getHolidaySubscriptionStatus","params":{}})", response);
    std::wcout << response << std::endl;

    const bool ok_subscription_status =
        response.find(L"\"requestId\":\"smoke-10d\"") != std::wstring::npos &&
        response.find(L"holiday-subscription-smoke.txt") != std::wstring::npos &&
        response.find(L"missing-subscription-smoke.txt") != std::wstring::npos &&
        response.find(L"\"loaded\":true") != std::wstring::npos &&
        response.find(L"\"exists\":false") != std::wstring::npos;

    if (force_storage_error_test) {
        host.HandleWebMessage(LR"({"apiVersion":"1.0","requestId":"smoke-11","method":"task.create","params":{"date":"2026-02-23","title":"forced-storage-error-check"}})", response);
        std::wcout << response << std::endl;

        if (strict_assert) {
            const bool ok_storage = response.find(L"\"requestId\":\"smoke-11\"") != std::wstring::npos &&
                                    response.find(L"\"code\":\"STORAGE_ERROR\"") != std::wstring::npos;
            if (!ok_storage) {
                std::wcerr << L"smoke strict check failed: expected STORAGE_ERROR for smoke-11" << std::endl;
                return 21;
            }
        }
    }

    if (smoke_subscription_written) {
        std::error_code ec;
        std::filesystem::remove(smoke_subscription_path, ec);
    }

    if (strict_assert && !ok_subscription_status) {
        std::wcerr << L"smoke strict check failed: expected subscription status for smoke-10d" << std::endl;
        return 22;
    }

    return 0;
}

int RunCompareMode(tcalendar::TCalendarHost& host, const std::wstring& years_csv) {
    std::wstring response;
    const std::wstring request = std::wstring(L"{\"apiVersion\":\"1.0\",\"requestId\":\"compare-1\",\"method\":\"debug.compareJpHolidayProviders\",\"params\":{\"yearsCsv\":\"") +
        years_csv + L"\"}}";
    host.HandleWebMessage(request, response);
    std::wcout << response << std::endl;
    if (response.find(L"\"requestId\":\"compare-1\"") == std::wstring::npos) {
        return 31;
    }
    if (response.find(L"\"ok\":true") == std::wstring::npos) {
        return 32;
    }
    return (response.find(L"\"totalMismatchCount\":0") != std::wstring::npos) ? 0 : 33;
}

int RunStandaloneWindowMode(tcalendar::TCalendarHost& host, const tcalendar::HostConfig& config, const std::filesystem::path& exe_dir, HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TCalendarWndProc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kTCalendarWindowClassName;

    if (!RegisterClassExW(&wc)) {
        return 2;
    }

    WindowContext context{};
    context.host = &host;
    context.startup_tick = GetTickCount64();
    context.startup_log_enabled = config.startup_log_enabled;
    context.startup_log_path = config.startup_log_path;
    context.initial_uri = BuildFileUriFromPath(std::filesystem::absolute(config.default_template_path).wstring());
    context.webview_user_data_dir = (std::filesystem::absolute(config.storage_db_path).parent_path() / L"webview2").wstring();
    context.ini_file_path = (exe_dir / kTCalendarIniFileName).wstring();
    context.block_external_navigation = config.block_external_navigation;

    int window_x = CW_USEDEFAULT;
    int window_y = CW_USEDEFAULT;
    int window_width = 960;
    int window_height = 640;
    bool has_window_position = false;
    LoadWindowPlacementFromIni(context.ini_file_path, window_x, window_y, window_width, window_height, has_window_position);

    HWND hwnd = CreateWindowExW(
        0,
        kTCalendarWindowClassName,
        L"TCalendar",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        has_window_position ? window_x : CW_USEDEFAULT,
        has_window_position ? window_y : CW_USEDEFAULT,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance,
        &context);

    if (!hwnd) {
        UnregisterClassW(kTCalendarWindowClassName, instance);
        return 3;
    }

    host.SetHostWindow(hwnd);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    LogStartupMark(&context, L"native window shown");

    if (!StartWebView2ForWindow(&context, hwnd)) {
        DestroyWindow(hwnd);
        UnregisterClassW(kTCalendarWindowClassName, instance);
        return 4;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    context.webview.Reset();
    context.controller.Reset();
    context.environment.Reset();

    UnregisterClassW(kTCalendarWindowClassName, instance);
    return static_cast<int>(msg.wParam);
}

} // namespace

int RunEntryMain(int argc, wchar_t** argv) {
    RuntimeArgs args{};
    std::wstring arg_error;
    if (!ParseRuntimeArgs(argc, argv, args, arg_error)) {
        MessageBoxW(nullptr, arg_error.c_str(), L"TCalendar", MB_OK | MB_ICONERROR);
        return 2;
    }

    HANDLE single_instance_mutex = nullptr;
    if (args.mode != RuntimeMode::Smoke && args.mode != RuntimeMode::Compare) {
        HWND console = GetConsoleWindow();
        if (console) {
            ShowWindow(console, SW_HIDE);
        }
        // UI/alert both run without visible console in production launch.
        FreeConsole();

        const wchar_t* mutex_name = (args.mode == RuntimeMode::Alert)
            ? kTCalendarAlertMutexName
            : kTCalendarUiMutexName;

        single_instance_mutex = CreateMutexW(nullptr, FALSE, mutex_name);
        if (single_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (args.mode == RuntimeMode::Ui) {
                ActivateExistingTCalendarWindow();
            }
            CloseHandle(single_instance_mutex);
            return 0;
        }
    }

    const bool smoke_mode = (args.mode == RuntimeMode::Smoke);
    const bool compare_mode = (args.mode == RuntimeMode::Compare);

    const std::filesystem::path exe_dir = GetExecutableDirectory();
    tcalendar::HostConfig config{};
    LoadHostConfigFromIni(exe_dir, smoke_mode, args.smoke_storage_error_mode, config);

    int rc = 0;
    if (args.mode == RuntimeMode::Alert) {
        rc = RunAlertMode(config);
        if (single_instance_mutex) {
            CloseHandle(single_instance_mutex);
        }
        return rc;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (smoke_mode) {
        std::error_code ec;
        std::filesystem::remove(config.storage_db_path, ec);
    }

    tcalendar::TCalendarHost host(config);
    if (!host.Initialize()) {
        if (!smoke_mode) {
            const std::wstring detail = host.GetLastInitializeError();
            const std::wstring text = detail.empty()
                ? L"TCalendar initialization failed."
                : (L"TCalendar initialization failed.\n\n" + detail);
            MessageBoxW(nullptr, text.c_str(), L"TCalendar", MB_OK | MB_ICONERROR);
        }
        CoUninitialize();
        if (single_instance_mutex) {
            CloseHandle(single_instance_mutex);
        }
        return 1;
    }

    if (smoke_mode) {
        rc = RunSmokeMode(host, exe_dir, args.smoke_storage_error_mode, args.smoke_strict_mode);
    } else if (compare_mode) {
        rc = RunCompareMode(host, args.compare_years_csv);
    } else {
        rc = RunStandaloneWindowMode(host, config, exe_dir, GetModuleHandleW(nullptr));
    }

    host.Shutdown();
    CoUninitialize();
    if (single_instance_mutex) {
        CloseHandle(single_instance_mutex);
    }
    return rc;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc <= 0) {
        MessageBoxW(nullptr, L"Failed to parse command line.", L"TCalendar", MB_OK | MB_ICONERROR);
        if (argv) {
            LocalFree(argv);
        }
        return 2;
    }

    const int rc = RunEntryMain(argc, argv);
    LocalFree(argv);
    return rc;
}
