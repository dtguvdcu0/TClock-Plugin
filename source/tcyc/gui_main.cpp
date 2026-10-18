#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gui_main.h"
#include "ini_utf8_util.h"
#include "runner.h"
#include "../common/native_edit.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cwctype>
#include <ctime>

namespace {
namespace fs = std::filesystem;

constexpr wchar_t kWindowClassName[] = L"TCycleSettingsWindow";
constexpr wchar_t kRenameClassName[] = L"TCycleRenameDialog";
constexpr wchar_t kReloadEventName[] = L"Local\\TCycle_Reload_Config_Event";

constexpr int kCtrlPollSec = 1002;
constexpr int kCtrlGraceSec = 1003;
constexpr int kCtrlTaskList = 1005;
constexpr int kCtrlTaskEnabled = 1006;
constexpr int kCtrlSingleInstance = 1007;
constexpr int kCtrlTriggerInterval = 1008;
constexpr int kCtrlIntervalSec = 1009;
constexpr int kCtrlRepeatCount = 1012;
constexpr int kCtrlTimeOfDay = 1013;
constexpr int kCtrlHotkeyMod = 1016;
constexpr int kCtrlHotkeyKey = 1017;
constexpr int kCtrlStatus = 1018;
constexpr int kCtrlTaskAdd = 1019;
constexpr int kCtrlTaskDelete = 1020;
constexpr int kCtrlTaskRename = 1021;
constexpr int kCtrlTaskTestRun = 1026;
constexpr int kCtrlTaskInlineRename = 1060;
constexpr int kCtrlActionMode = 1022;
constexpr int kCtrlActionPath = 1023;
constexpr int kCtrlActionArgs = 1024;
constexpr int kCtrlActionCwd = 1025;
constexpr int kCtrlWeekdaySun = 1030;
constexpr int kCtrlWeekdayMon = 1031;
constexpr int kCtrlWeekdayTue = 1032;
constexpr int kCtrlWeekdayWed = 1033;
constexpr int kCtrlWeekdayThu = 1034;
constexpr int kCtrlWeekdayFri = 1035;
constexpr int kCtrlWeekdaySat = 1036;
constexpr int kCtrlWeekdayEveryday = 1049;
constexpr int kCtrlDateEnabled = 1050;
constexpr int kCtrlDateValue = 1051;
constexpr int kCtrlWeekdayEnabled = 1052;
constexpr int kCtrlTimeEnabled = 1053;
constexpr int kCtrlTriggerNonRunning = 1054;
constexpr int kCtrlWatchdogRetrySec = 1055;
constexpr int kCtrlSpinWatchdogRetrySec = 1056;
constexpr int kCtrlTriggerWeeklyTime = 1038;
constexpr int kCtrlTriggerStartup = 1039;
constexpr int kCtrlTriggerHotkeyOnly = 1040;
constexpr int kCtrlSpinPollSec = 1041;
constexpr int kCtrlSpinGraceSec = 1042;
constexpr int kCtrlSpinIntervalSec = 1043;
constexpr int kCtrlSpinRepeatCount = 1045;
constexpr UINT_PTR kTaskListTimerId = 1;
constexpr UINT kMsgFocusInlineRename = WM_APP + 21;
constexpr int kTcycleIconResId = 101; // IDI_APP_ICON in tcycle.rc

struct HotkeyModOption {
    const wchar_t* label;
    bool ctrl;
    bool shift;
    bool alt;
    bool win;
};

struct HotkeyKeyOption {
    std::wstring label;
    int vk;
};

struct RenameDialogState {
    std::wstring title;
    std::wstring initialText;
    std::wstring resultText;
    HWND edit = nullptr;
    bool accepted = false;
};

struct WindowState {
    tcyc::RuntimeConfig config;
    std::wstring iniPath;
    std::wstring exeDir;
    std::string languageCode = "en";
    std::unordered_map<std::wstring, std::wstring> translations;

    HWND panel = nullptr;
    HFONT uiFont = nullptr, smallFont = nullptr, titleFont = nullptr;
    UINT dpi = 96;
    bool globalView = false, arranging = false, retryDirty = false;
    int scrollOffset = 0, contentHeight = 0;
    struct Placement { HWND control; int x, y, width, height; };
    std::vector<Placement> placements;
    std::vector<RECT> blocks;
    std::vector<int> separators;
    std::vector<std::wstring> deletedTaskSections;
    HWND pollSec = nullptr;
    HWND graceSec = nullptr;

    HWND taskList = nullptr;
    HWND taskAdd = nullptr;
    HWND taskDelete = nullptr;
    HWND taskRename = nullptr;
    HWND taskTestRun = nullptr;

    HWND taskEnabled = nullptr;
    HWND singleInstance = nullptr;
    HWND actionPrimaryLabel = nullptr;
    HWND actionMode = nullptr;
    HWND actionPath = nullptr;
    HWND actionArgs = nullptr;
    HWND actionCwd = nullptr;
    HWND triggerChecks[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    HWND intervalSec = nullptr;
    HWND watchdogRetrySec = nullptr;
    HWND repeatCount = nullptr;
    HWND dateEnabled = nullptr;
    HWND dateValue = nullptr;
    HWND weekdayEnabled = nullptr;
    HWND timeEnabled = nullptr;
    HWND weekdayEveryday = nullptr;
    HWND weekdayChecks[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    HWND timeOfDay = nullptr;
    HWND hotkeyMod = nullptr;
    HWND hotkeyKey = nullptr;
    HWND grpInterval = nullptr;
    HWND grpDateTime = nullptr;
    HWND grpWeekly = nullptr;
    HWND grpNonRunning = nullptr;
    HWND grpStartup = nullptr;
    HWND grpHotkey = nullptr;

    HWND status = nullptr;
    HWND mainWindow = nullptr;
    HWND inlineEdit = nullptr;
    int inlineEditIndex = -1;
    bool inlineFinalizing = false;
    WNDPROC taskListOldProc = nullptr;
    WNDPROC inlineEditOldProc = nullptr;

    int selectedTask = -1;
    bool suppressEvents = false;
    bool actionPathDirty = false;
    bool actionArgsDirty = false;
    bool actionCwdDirty = false;
    bool dateEnabledDirty = false;
    bool dateValueDirty = false;
    bool weekdayDirty = false;
    bool timeEnabledDirty = false;
    bool timeOfDayDirty = false;
    bool hotkeyDirty = false;
};

void tcyc_sync_controls(WindowState* st);
void tcyc_layout_controls(WindowState* st,bool scrolling = false);
void tcyc_refresh_fonts(WindowState* st);
bool tcyc_draw_item(WindowState* st, const DRAWITEMSTRUCT& item);
void tcyc_paint_surface(WindowState* st, HWND window, HDC dc);
bool tcyc_handle_command(WindowState* st, WPARAM wp, LPARAM lp);

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::wstring UnescapeText(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            if (s[i + 1] == L'n') {
                out.push_back(L'\n');
                ++i;
                continue;
            }
            if (s[i + 1] == L'\\') {
                out.push_back(L'\\');
                ++i;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::wstring StripQuotes(const std::wstring& s) {
    if (s.size() >= 2) {
        if ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\'')) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::wstring TrimWide(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

fs::path GetLanguageRoot(const std::wstring& exeDir) {
    fs::path base(exeDir);
    fs::path modern = base / "tcyc" / "lang";
    if (fs::exists(modern) && fs::is_directory(modern)) return modern;
    return base / "lang";
}

bool LoadLanguage(WindowState* st, const std::string& code) {
    if (!st) return false;
    std::string lang = ToLowerAscii(code.empty() ? std::string("en") : code);
    if (lang != "ja" && lang != "en") lang = "en";
    st->translations.clear();
    st->languageCode = lang;

    auto loadFile = [&](const std::string& langCode) -> bool {
        fs::path path = GetLanguageRoot(st->exeDir) / langCode / "strings.ini";
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = TrimAscii(line.substr(0, eq));
            std::string val = line.substr(eq + 1);
            std::wstring wkey = Utf8ToWide(key);
            std::wstring wval = StripQuotes(UnescapeText(Utf8ToWide(val)));
            if (!wkey.empty()) st->translations[wkey] = wval;
        }
        return true;
    };

    bool loaded = loadFile(lang);
    if (!loaded && lang != "en") {
        loadFile("en");
        st->languageCode = "en";
    }
    return loaded;
}

std::wstring Tr(const WindowState* st, const wchar_t* id, const wchar_t* fallback) {
    if (!st || !id) return fallback ? std::wstring(fallback) : L"";
    auto it = st->translations.find(id);
    if (it != st->translations.end() && !it->second.empty()) return it->second;
    return fallback ? std::wstring(fallback) : L"";
}

std::string ReadLanguageFromIni(const std::wstring& iniPath) {
    std::wstring ws;
    if (!tcyc::ReadIniUtf8Value(iniPath, L"TCycle", L"Language", L"", ws)) ws.clear();
    ws = TrimWide(ws);
    if (ws == L"ja" || ws == L"JA" || ws == L"Ja" || ws == L"jA") return "ja";
    if (ws == L"en" || ws == L"EN" || ws == L"En" || ws == L"eN") return "en";
    return "en";
}

std::wstring TriggerToString(tcyc::TriggerType trigger) {
    switch (trigger) {
    case tcyc::TriggerType::Interval: return L"interval";
    case tcyc::TriggerType::DateTimeIntervalLimited: return L"datetime_interval_limited";
    case tcyc::TriggerType::WeeklyTime: return L"weekly_time";
    case tcyc::TriggerType::Startup: return L"startup";
    case tcyc::TriggerType::HotkeyOnly: return L"hotkey_only";
    case tcyc::TriggerType::NonRunning: return L"non_running";
    default: return L"unknown";
    }
}

int TriggerTypeToIndex(tcyc::TriggerType trigger) {
    switch (trigger) {
    case tcyc::TriggerType::Interval: return 0;
    case tcyc::TriggerType::DateTimeIntervalLimited: return 1;
    case tcyc::TriggerType::WeeklyTime: return 2;
    case tcyc::TriggerType::Startup: return 3;
    case tcyc::TriggerType::HotkeyOnly: return 4;
    case tcyc::TriggerType::NonRunning: return 5;
    default: return 0;
    }
}

tcyc::TriggerType TriggerBitToType(int bit) {
    switch (bit) {
    case 0: return tcyc::TriggerType::Interval;
    case 1: return tcyc::TriggerType::DateTimeIntervalLimited;
    case 2: return tcyc::TriggerType::WeeklyTime;
    case 3: return tcyc::TriggerType::Startup;
    case 4: return tcyc::TriggerType::HotkeyOnly;
    case 5: return tcyc::TriggerType::NonRunning;
    default: return tcyc::TriggerType::Unknown;
    }
}

int TriggerTypeToBit(tcyc::TriggerType trigger) {
    const int idx = TriggerTypeToIndex(trigger);
    if (idx < 0 || idx > 5) return 0;
    return (1 << idx);
}

std::wstring TriggerMaskToString(int mask) {
    std::wstring out;
    for (int i = 0; i < 6; ++i) {
        if ((mask & (1 << i)) == 0) continue;
        if (!out.empty()) out += L",";
        out += TriggerToString(TriggerBitToType(i));
    }
    return out;
}

int FirstTriggerFromMask(int mask) {
    for (int i = 0; i < 6; ++i) {
        if ((mask & (1 << i)) != 0) return i;
    }
    return 0;
}

tcyc::TriggerType TriggerIndexToType(int idx) {
    switch (idx) {
    case 0: return tcyc::TriggerType::Interval;
    case 1: return tcyc::TriggerType::DateTimeIntervalLimited;
    case 2: return tcyc::TriggerType::WeeklyTime;
    case 3: return tcyc::TriggerType::Startup;
    case 4: return tcyc::TriggerType::HotkeyOnly;
    case 5: return tcyc::TriggerType::NonRunning;
    default: return tcyc::TriggerType::Unknown;
    }
}

int ActionModeToIndex(const std::wstring& modeRaw) {
    std::wstring mode = modeRaw;
    for (auto& ch : mode) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    if (mode == L"command") return 1;
    if (mode == L"shell") return 2;
    return 0;
}

std::wstring ActionModeFromIndex(int idx) {
    if (idx == 1) return L"command";
    if (idx == 2) return L"shell";
    return L"program";
}

void ApplyActionModeUi(WindowState* st) {
    if (!st) return;
    if (st->actionPrimaryLabel) {
        const std::wstring label = Tr(st, L"label_action_path_only", L"Path");
        SetWindowTextW(st->actionPrimaryLabel, label.c_str());
    }
}

std::wstring WeekdayToString(int d) {
    static const wchar_t* kNames[] = {L"sun", L"mon", L"tue", L"wed", L"thu", L"fri", L"sat"};
    if (d < 0 || d > 6) return L"";
    return kNames[d];
}

std::wstring WeekdayMaskToString(int mask) {
    std::wstring out;
    for (int i = 0; i < 7; ++i) {
        if ((mask & (1 << i)) == 0) continue;
        if (!out.empty()) out += L",";
        out += WeekdayToString(i);
    }
    return out;
}

int FirstWeekdayFromMask(int mask) {
    for (int i = 0; i < 7; ++i) {
        if ((mask & (1 << i)) != 0) return i;
    }
    return -1;
}

std::wstring TimeOfDayToString(int sec) {
    if (sec < 0) sec = 0;
    int hh = sec / 3600;
    int mm = (sec % 3600) / 60;
    wchar_t buf[16] = {0};
    swprintf_s(buf, L"%02d:%02d", hh, mm);
    return std::wstring(buf);
}

std::wstring CurrentDateYmd() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[16] = {0};
    swprintf_s(buf, L"%04d-%02d-%02d", static_cast<int>(now.wYear), static_cast<int>(now.wMonth), static_cast<int>(now.wDay));
    return buf;
}

bool ParseTimeOfDay(const std::wstring& s, int& outSec) {
    int hh = 0, mm = 0, ss = 0;
    int n = swscanf_s(s.c_str(), L"%d:%d:%d", &hh, &mm, &ss);
    if (n < 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;
    outSec = hh * 3600 + mm * 60 + ss;
    return true;
}

bool ParseDateYmd(const std::wstring& s, int& y, int& mo, int& d) {
    y = 0; mo = 0; d = 0;
    if (s.empty()) return false;
    int n = swscanf_s(s.c_str(), L"%d-%d-%d", &y, &mo, &d);
    if (n != 3) return false;
    if (y < 1970 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    return true;
}

std::wstring FormatLocalYmdHm(time_t t) {
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wchar_t buf[32] = {0};
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return std::wstring(buf);
}

bool TryComputeNextRunTime(const tcyc::TaskConfig& t, time_t now, time_t& outNext) {
    int triggerMask = t.triggerMask;
    if (triggerMask == 0) triggerMask = TriggerTypeToBit(t.trigger);

    if ((triggerMask & TriggerTypeToBit(tcyc::TriggerType::Interval)) != 0 && t.intervalSec > 0) {
        outNext = now + static_cast<time_t>(t.intervalSec);
        return true;
    }

    if ((triggerMask & TriggerTypeToBit(tcyc::TriggerType::WeeklyTime)) == 0) {
        return false;
    }
    if (!t.timeEnabled || t.timeOfDaySec < 0) {
        return false;
    }

    std::tm nowTm{};
    localtime_s(&nowTm, &now);

    if (t.dateEnabled) {
        int y = 0, mo = 0, d = 0;
        if (!ParseDateYmd(t.dateYmd, y, mo, d)) return false;
        std::tm due{};
        due.tm_year = y - 1900;
        due.tm_mon = mo - 1;
        due.tm_mday = d;
        due.tm_hour = t.timeOfDaySec / 3600;
        due.tm_min = (t.timeOfDaySec % 3600) / 60;
        due.tm_sec = 0;
        time_t cand = mktime(&due);
        if (cand == static_cast<time_t>(-1) || cand < now) return false;
        outNext = cand;
        return true;
    }

    int weekdayMask = 0;
    if (!t.weekdayEnabled) {
        weekdayMask = 0x7F;
    } else if (t.weeklyEveryday) {
        weekdayMask = 0x7F;
    } else {
        weekdayMask = t.weekdayMask;
        if (weekdayMask == 0 && t.weekday >= 0 && t.weekday <= 6) {
            weekdayMask = (1 << t.weekday);
        }
        if (weekdayMask == 0) return false;
    }

    for (int offset = 0; offset <= 14; ++offset) {
        std::tm candTm = nowTm;
        candTm.tm_mday += offset;
        candTm.tm_hour = t.timeOfDaySec / 3600;
        candTm.tm_min = (t.timeOfDaySec % 3600) / 60;
        candTm.tm_sec = 0;
        time_t cand = mktime(&candTm);
        if (cand == static_cast<time_t>(-1)) continue;
        std::tm norm{};
        localtime_s(&norm, &cand);
        if ((weekdayMask & (1 << norm.tm_wday)) == 0) continue;
        if (cand < now) continue;
        outNext = cand;
        return true;
    }
    return false;
}

std::wstring BuildNextRunStatusSuffix(WindowState* st, const tcyc::TaskConfig& t) {
    if (!t.enabled) return L"  " + Tr(st, L"status_task_disabled", L"Task disabled");
    if (tcyc::IsTClockGateDisabled(st->config)) return L"  " + Tr(st, L"status_gate_disabled", L"Disabled in TClock settings");
    const int mask = t.triggerMask ? t.triggerMask : TriggerTypeToBit(t.trigger);
    if ((mask & TriggerTypeToBit(tcyc::TriggerType::Interval)) && t.intervalSec > 0)
        return L"  " + Tr(st, L"status_interval_prefix", L"Interval (seconds):") + L" " + std::to_wstring(t.intervalSec);
    time_t now = time(nullptr);
    time_t next = 0;
    if (!TryComputeNextRunTime(t, now, next)) return L"";
    std::wstring prefix = Tr(st, L"status_schedule_estimate", L"  Estimated schedule:") + L" ";
    return prefix + FormatLocalYmdHm(next);
}

std::wstring BuildStatusWithNextRun(WindowState* st, const std::wstring& base, int taskIndex) {
    std::wstring out = base;
    if (!st) return out;
    if (taskIndex >= 0 && taskIndex < static_cast<int>(st->config.tasks.size())) {
        out += BuildNextRunStatusSuffix(st, st->config.tasks[static_cast<size_t>(taskIndex)]);
    }
    return out;
}


int ParseIntOrDefault(const std::wstring& s, int def, int lo, int hi) {
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0') return def;
    if (v < lo || v > hi) return def;
    return static_cast<int>(v);
}

std::wstring GetEditText(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return L"";
    std::wstring out;
    out.resize(static_cast<size_t>(len + 1), L'\0');
    int copied = GetWindowTextW(h, out.data(), len + 1);
    if (copied <= 0) return L"";
    out.resize(static_cast<size_t>(copied));
    return out;
}

void SetEditText(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
}

void SetEditInt(HWND h, int v) {
    wchar_t buf[64] = {0};
    swprintf_s(buf, L"%d", v);
    SetWindowTextW(h, buf);
}

int ReadWeekdayMaskFromChecks(const WindowState* st) {
    if (!st) return 0;
    int mask = 0;
    for (int i = 0; i < 7; ++i) {
        if (st->weekdayChecks[i] && SendMessageW(st->weekdayChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= (1 << i);
        }
    }
    return mask;
}

bool IsEverydayChecked(const WindowState* st) {
    return st && st->weekdayEveryday && (SendMessageW(st->weekdayEveryday, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

bool IsChecked(HWND h) {
    return h && (SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

void SetChecked(HWND h, bool on) {
    if (!h) return;
    SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SetWeekdayChecksFromMask(WindowState* st, int mask) {
    if (!st) return;
    for (int i = 0; i < 7; ++i) {
        if (!st->weekdayChecks[i]) continue;
        SendMessageW(st->weekdayChecks[i], BM_SETCHECK, (mask & (1 << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void SetWeekdayEveryday(WindowState* st, bool on) {
    if (!st || !st->weekdayEveryday) return;
    SendMessageW(st->weekdayEveryday, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void ApplyWeekdayUiState(WindowState* st, bool isWeeklyEnabled, bool weekdayConditionEnabled) {
    if (!st) return;
    const bool enableWeekdays = isWeeklyEnabled && weekdayConditionEnabled && !IsEverydayChecked(st);
    for (int i = 0; i < 7; ++i) {
        if (st->weekdayChecks[i]) EnableWindow(st->weekdayChecks[i], enableWeekdays ? TRUE : FALSE);
    }
}

int ReadTriggerMaskFromChecks(const WindowState* st) {
    if (!st) return 0;
    int mask = 0;
    for (int i = 0; i < 6; ++i) {
        if (st->triggerChecks[i] && SendMessageW(st->triggerChecks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= (1 << i);
        }
    }
    return mask;
}

void SetTriggerChecksFromMask(WindowState* st, int mask) {
    if (!st) return;
    mask &= ((1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5));
    for (int i = 0; i < 6; ++i) {
        if (!st->triggerChecks[i]) continue;
        SendMessageW(st->triggerChecks[i], BM_SETCHECK, ((mask & (1 << i)) != 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

void EnsureAtLeastOneTriggerChecked(WindowState* st, int fallbackIndex) {
    if (!st) return;
    if (ReadTriggerMaskFromChecks(st) != 0) return;
    if (fallbackIndex < 0 || fallbackIndex > 5) fallbackIndex = 0;
    if (!st->triggerChecks[fallbackIndex]) fallbackIndex = 0;
    SendMessageW(st->triggerChecks[fallbackIndex], BM_SETCHECK, BST_CHECKED, 0);
}

void EnsureAtLeastOneDateConditionChecked(WindowState* st, int fallbackCtrlId) {
    if (!st) return;
    const bool any = IsChecked(st->dateEnabled) || IsChecked(st->weekdayEnabled) || IsChecked(st->timeEnabled);
    if (any) return;
    HWND fallback = st->timeEnabled;
    if (fallbackCtrlId == kCtrlDateEnabled) fallback = st->dateEnabled;
    else if (fallbackCtrlId == kCtrlWeekdayEnabled) fallback = st->weekdayEnabled;
    if (!fallback) fallback = st->timeEnabled;
    SetChecked(fallback, true);
}

bool HasTriggerEnabled(const WindowState* st, tcyc::TriggerType trigger) {
    const int mask = ReadTriggerMaskFromChecks(st);
    return (mask & TriggerTypeToBit(trigger)) != 0;
}

bool CanMonitorFromUi(WindowState* st) {
    if (!st) return false;
    tcyc::TaskConfig task;
    const int actionModeIdx = static_cast<int>(SendMessageW(st->actionMode, CB_GETCURSEL, 0, 0));
    task.actionMode = ActionModeFromIndex(actionModeIdx);
    task.actionPath = TrimWide(GetEditText(st->actionPath));
    task.actionArgs = TrimWide(GetEditText(st->actionArgs));
    return tcyc::CanMonitorTask(task);
}

void SetEnabled(HWND h, bool enabled) {
    if (h) EnableWindow(h, enabled ? TRUE : FALSE);
}

void ApplyTriggerUiState(WindowState* st) {
    if (!st) return;
    const bool supportsNonRunning = CanMonitorFromUi(st);
    if (!supportsNonRunning && IsChecked(st->triggerChecks[5])) {
        SetChecked(st->triggerChecks[5], false);
        EnsureAtLeastOneTriggerChecked(st, 3);
    }
    const bool isInterval = HasTriggerEnabled(st, tcyc::TriggerType::Interval);
    const bool isWeekly = HasTriggerEnabled(st, tcyc::TriggerType::WeeklyTime);
    const bool isHotkeyOnly = HasTriggerEnabled(st, tcyc::TriggerType::HotkeyOnly);
    const bool isNonRunning = supportsNonRunning && HasTriggerEnabled(st, tcyc::TriggerType::NonRunning);
    if (isWeekly) {
        if (IsChecked(st->dateEnabled) && IsChecked(st->weekdayEnabled)) {
            SetChecked(st->weekdayEnabled, false);
        }
        if (!IsChecked(st->timeEnabled)) {
            SetChecked(st->timeEnabled, true);
        }
    }
    const bool isDateCondition = IsChecked(st->dateEnabled);
    const bool isWeekdayCondition = IsChecked(st->weekdayEnabled);
    const bool isTimeCondition = IsChecked(st->timeEnabled);

    SetEnabled(st->grpInterval, true);
    SetEnabled(st->intervalSec, isInterval);

    SetEnabled(st->grpWeekly, true);
    SetEnabled(st->dateEnabled, isWeekly);
    SetEnabled(st->dateValue, isWeekly && isDateCondition);
    SetEnabled(st->weekdayEnabled, isWeekly);
    SetEnabled(st->weekdayEveryday, isWeekly && isWeekdayCondition);
    for (int i = 0; i < 7; ++i) SetEnabled(st->weekdayChecks[i], isWeekly && isWeekdayCondition && !IsEverydayChecked(st));
    SetEnabled(st->timeEnabled, isWeekly);
    SetEnabled(st->timeOfDay, isWeekly && isTimeCondition);

    SetEnabled(st->grpHotkey, true);
    SetEnabled(st->hotkeyMod, isHotkeyOnly);
    SetEnabled(st->hotkeyKey, isHotkeyOnly);

    SetEnabled(st->triggerChecks[5], supportsNonRunning);
    SetEnabled(st->grpNonRunning, supportsNonRunning);
    SetEnabled(st->watchdogRetrySec, isNonRunning);
    SetEnabled(st->repeatCount, isNonRunning);

    SetEnabled(st->grpStartup, true);
    ApplyWeekdayUiState(st, isWeekly, isWeekdayCondition);
    tcyc_sync_controls(st);
}

void SetStatus(WindowState* st, const std::wstring& msg) {
    if (st && st->status) SetWindowTextW(st->status, msg.c_str());
}

std::wstring BuildMainWindowTitle(WindowState* st) {
    std::wstring appTitle = Tr(st, L"app_name", L"TCycle");
    std::wstring title = appTitle.empty() ? L"TCycle" : appTitle;
    if (st && st->selectedTask >= 0 && st->selectedTask < static_cast<int>(st->config.tasks.size())) {
        const std::wstring& taskName = st->config.tasks[static_cast<size_t>(st->selectedTask)].name;
        if (!taskName.empty()) {
            title += L" - ";
            title += taskName;
        }
    }
    if (title.empty() || title == L" - ") {
        title = L"TCycle";
    }
    return title;
}

void UpdateMainWindowTitle(WindowState* st) {
    if (!st || !st->mainWindow) return;
    std::wstring title = BuildMainWindowTitle(st);
    if (st->taskList) {
        const int lbSel = static_cast<int>(SendMessageW(st->taskList, LB_GETCURSEL, 0, 0));
        if (lbSel >= 0 && lbSel < static_cast<int>(st->config.tasks.size())) {
            const std::wstring& taskName = st->config.tasks[static_cast<size_t>(lbSel)].name;
            if (!taskName.empty()) {
                std::wstring base;
                const std::wstring appTitle = Tr(st, L"app_name", L"TCycle");
                base = appTitle.empty() ? L"TCycle" : appTitle;
                title = base + L" - " + taskName;
            }
        }
    }
    SetWindowTextW(st->mainWindow, title.c_str());
}

const std::vector<HotkeyModOption>& HotkeyModOptions() {
    static const std::vector<HotkeyModOption> k = {
        {L"(none)", false, false, false, false},
        {L"Ctrl", true, false, false, false},
        {L"Shift", false, true, false, false},
        {L"Alt", false, false, true, false},
        {L"Win", false, false, false, true},
        {L"Ctrl+Shift", true, true, false, false},
        {L"Ctrl+Alt", true, false, true, false},
        {L"Ctrl+Win", true, false, false, true},
        {L"Shift+Alt", false, true, true, false},
        {L"Shift+Win", false, true, false, true},
        {L"Alt+Win", false, false, true, true},
        {L"Ctrl+Shift+Alt", true, true, true, false},
        {L"Ctrl+Shift+Win", true, true, false, true},
        {L"Ctrl+Alt+Win", true, false, true, true},
        {L"Shift+Alt+Win", false, true, true, true},
        {L"Ctrl+Shift+Alt+Win", true, true, true, true},
    };
    return k;
}

const std::vector<HotkeyKeyOption>& HotkeyKeyOptions() {
    static std::vector<HotkeyKeyOption> k;
    if (!k.empty()) return k;
    k.push_back({L"(none)", 0});
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        wchar_t label[2] = {c, 0};
        k.push_back({label, c});
    }
    for (wchar_t c = L'0'; c <= L'9'; ++c) {
        wchar_t label[2] = {c, 0};
        k.push_back({label, c});
    }
    for (int i = 1; i <= 24; ++i) {
        wchar_t buf[8] = {0};
        swprintf_s(buf, L"F%d", i);
        k.push_back({buf, VK_F1 + (i - 1)});
    }
    k.push_back({L"PrintScreen", VK_SNAPSHOT});
    k.push_back({L"Insert", VK_INSERT});
    k.push_back({L"Delete", VK_DELETE});
    k.push_back({L"Home", VK_HOME});
    k.push_back({L"End", VK_END});
    k.push_back({L"PgUp", VK_PRIOR});
    k.push_back({L"PgDn", VK_NEXT});
    k.push_back({L"Space", VK_SPACE});
    k.push_back({L"Enter", VK_RETURN});
    return k;
}

bool ParseHotkeyString(const std::wstring& hotkey, bool& ctrl, bool& shift, bool& alt, bool& win, int& keyVk) {
    ctrl = false;
    shift = false;
    alt = false;
    win = false;
    keyVk = 0;
    if (hotkey.empty()) return true;
    bool hasUnknownToken = false;

    std::wstring token;
    auto flush = [&](const std::wstring& t) {
        std::wstring x = TrimWide(t);
        if (x.empty()) return;
        std::wstring lower = x;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (lower == L"ctrl" || lower == L"control") ctrl = true;
        else if (lower == L"shift") shift = true;
        else if (lower == L"alt") alt = true;
        else if (lower == L"win" || lower == L"windows") win = true;
        else {
            const auto& keys = HotkeyKeyOptions();
            for (const auto& k : keys) {
                std::wstring l = k.label;
                std::transform(l.begin(), l.end(), l.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                if (l == lower) {
                    keyVk = k.vk;
                    return;
                }
            }
            if (lower.size() == 1) {
                wchar_t c = static_cast<wchar_t>(towupper(lower[0]));
                if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
                    keyVk = c;
                    return;
                }
            }
            hasUnknownToken = true;
        }
    };

    for (wchar_t c : hotkey) {
        if (c == L'+') {
            flush(token);
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    flush(token);
    if (hasUnknownToken) return false;
    if (keyVk == 0) return false;
    return true;
}

std::wstring ComposeHotkeyString(WindowState* st) {
    int modSel = static_cast<int>(SendMessageW(st->hotkeyMod, CB_GETCURSEL, 0, 0));
    int keySel = static_cast<int>(SendMessageW(st->hotkeyKey, CB_GETCURSEL, 0, 0));
    if (modSel < 0 || modSel >= static_cast<int>(HotkeyModOptions().size())) modSel = 0;
    if (keySel < 0 || keySel >= static_cast<int>(HotkeyKeyOptions().size())) keySel = 0;

    const auto& m = HotkeyModOptions()[static_cast<size_t>(modSel)];
    const auto& k = HotkeyKeyOptions()[static_cast<size_t>(keySel)];
    if (k.vk == 0) return L"";

    std::wstring out;
    if (m.ctrl) out += L"Ctrl+";
    if (m.shift) out += L"Shift+";
    if (m.alt) out += L"Alt+";
    if (m.win) out += L"Win+";
    out += k.label;
    return out;
}

bool ShouldPersistHotkeySelection(WindowState* st) {
    if (!st) return false;
    int modSel = static_cast<int>(SendMessageW(st->hotkeyMod, CB_GETCURSEL, 0, 0));
    int keySel = static_cast<int>(SendMessageW(st->hotkeyKey, CB_GETCURSEL, 0, 0));
    if (modSel < 0 || modSel >= static_cast<int>(HotkeyModOptions().size())) modSel = 0;
    if (keySel < 0 || keySel >= static_cast<int>(HotkeyKeyOptions().size())) keySel = 0;
    const auto& key = HotkeyKeyOptions()[static_cast<size_t>(keySel)];
    if (key.vk > 0) return true;
    return (modSel == 0 && keySel == 0);
}

void SetHotkeyCombosFromString(WindowState* st, const std::wstring& hotkey) {
    bool ctrl = false, shift = false, alt = false, win = false;
    int keyVk = 0;
    (void)ParseHotkeyString(hotkey, ctrl, shift, alt, win, keyVk);

    int modIndex = 0;
    for (int i = 0; i < static_cast<int>(HotkeyModOptions().size()); ++i) {
        const auto& m = HotkeyModOptions()[static_cast<size_t>(i)];
        if (m.ctrl == ctrl && m.shift == shift && m.alt == alt && m.win == win) {
            modIndex = i;
            break;
        }
    }
    int keyIndex = 0;
    for (int i = 0; i < static_cast<int>(HotkeyKeyOptions().size()); ++i) {
        if (HotkeyKeyOptions()[static_cast<size_t>(i)].vk == keyVk) {
            keyIndex = i;
            break;
        }
    }
    if (hotkey.empty()) {
        SendMessageW(st->hotkeyMod, CB_SETCURSEL, 0, 0);
        SendMessageW(st->hotkeyKey, CB_SETCURSEL, 0, 0);
        return;
    }
    SendMessageW(st->hotkeyMod, CB_SETCURSEL, static_cast<WPARAM>(modIndex), 0);
    if (keyVk > 0) {
        SendMessageW(st->hotkeyKey, CB_SETCURSEL, static_cast<WPARAM>(keyIndex), 0);
    } else {
        SendMessageW(st->hotkeyKey, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
}

long long UnixNowSec() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    constexpr ULONGLONG kEpochDiff = 116444736000000000ULL;
    return static_cast<long long>((ui.QuadPart - kEpochDiff) / 10000000ULL);
}

int ReadWatchdogCountdownSec(const std::wstring& statePath, int taskId, int fallbackSec) {
    if (statePath.empty() || taskId <= 0) return fallbackSec;
    wchar_t section[32] = {0};
    swprintf_s(section, L"Task.%d", taskId);
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(section, L"WatchdogNextRetryUnix", L"", buf, static_cast<DWORD>(std::size(buf)), statePath.c_str());
    if (buf[0] == L'\0') return fallbackSec;
    const long long nextRetry = _wtoi64(buf);
    if (nextRetry <= 0) return fallbackSec;
    long long remain = nextRetry - UnixNowSec();
    if (remain < 0) remain = 0;
    if (remain > 99999) remain = 99999;
    return static_cast<int>(remain);
}

void PopulateTaskList(WindowState* st) {
    SendMessageW(st->taskList, LB_RESETCONTENT, 0, 0);
    for (const auto& t : st->config.tasks) {
        wchar_t line[256] = {0};
        if (t.watchdogEnabled) {
            const int countdown = ReadWatchdogCountdownSec(st->config.stateFile, t.id, t.watchdogRetrySec);
            swprintf_s(line, L"[%d][%s] %s", countdown, t.enabled ? L"on" : L"off", t.name.c_str());
        } else {
            swprintf_s(line, L"[%s] %s", t.enabled ? L"on" : L"off", t.name.c_str());
        }
        SendMessageW(st->taskList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    if (st->config.tasks.empty()) {
        st->selectedTask = -1;
        return;
    }
    if (st->selectedTask < 0 || st->selectedTask >= static_cast<int>(st->config.tasks.size())) st->selectedTask = 0;
    SendMessageW(st->taskList, LB_SETCURSEL, st->selectedTask, 0);
}

void LoadGlobalControls(WindowState* st) {
    SetEditInt(st->pollSec, st->config.pollSec);
    SetEditInt(st->graceSec, st->config.graceSec);
}

void LoadTaskControls(WindowState* st, int idx) {
    st->selectedTask = idx;
    if (idx < 0 || idx >= static_cast<int>(st->config.tasks.size())) {
        SendMessageW(st->taskEnabled, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(st->singleInstance, BM_SETCHECK, BST_CHECKED, 0);
        SetTriggerChecksFromMask(st, TriggerTypeToBit(tcyc::TriggerType::Startup));
        SetEditText(st->intervalSec, L"600");
        SendMessageW(st->actionMode, CB_SETCURSEL, 0, 0);
        SetEditText(st->actionPath, L"");
        SetEditText(st->actionArgs, L"");
        SetEditText(st->actionCwd, L"");
        SetEditText(st->watchdogRetrySec, L"10");
        SetEditText(st->repeatCount, L"5");
        SetChecked(st->dateEnabled, false);
        SetEditText(st->dateValue, CurrentDateYmd());
        SetChecked(st->weekdayEnabled, false);
        SetWeekdayEveryday(st, false);
        SetWeekdayChecksFromMask(st, 0);
        SetChecked(st->timeEnabled, true);
        SetEditText(st->timeOfDay, L"00:00");
        SetHotkeyCombosFromString(st, L"");
        ApplyTriggerUiState(st);
        st->actionPathDirty = false;
        st->actionArgsDirty = false;
        st->actionCwdDirty = false;
        UpdateMainWindowTitle(st);
        return;
    }
    const auto& t = st->config.tasks[static_cast<size_t>(idx)];
    SendMessageW(st->taskEnabled, BM_SETCHECK, t.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(st->singleInstance, BM_SETCHECK, t.singleInstance ? BST_CHECKED : BST_UNCHECKED, 0);
    int triggerMask = t.triggerMask;
    if (triggerMask == 0) triggerMask = TriggerTypeToBit(t.trigger);
    if (t.watchdogEnabled) triggerMask |= TriggerTypeToBit(tcyc::TriggerType::NonRunning);
    SetTriggerChecksFromMask(st, triggerMask);
    SetEditInt(st->intervalSec, (t.intervalSec > 0) ? t.intervalSec : 600);
    SendMessageW(st->actionMode, CB_SETCURSEL, ActionModeToIndex(t.actionMode), 0);
    SetEditText(st->actionPath, t.actionPath);
    SetEditText(st->actionArgs, t.actionArgs);
    SetEditText(st->actionCwd, t.actionCwd);
    SetEditInt(st->watchdogRetrySec, t.watchdogRetrySec);
    SetEditText(st->repeatCount, t.watchdogMaxRetry < 0 ? L"" : std::to_wstring(t.watchdogMaxRetry));
    SetChecked(st->dateEnabled, t.dateEnabled);
    SetEditText(st->dateValue, t.dateYmd.empty() ? CurrentDateYmd() : t.dateYmd);
    SetChecked(st->weekdayEnabled, t.weekdayEnabled && !t.dateEnabled);
    int weekdayMask = t.weekdayMask;
    if (weekdayMask == 0 && t.weekday >= 0 && t.weekday <= 6) {
        weekdayMask = (1 << t.weekday);
    }
    SetWeekdayEveryday(st, t.weeklyEveryday);
    SetWeekdayChecksFromMask(st, weekdayMask);
    SetChecked(st->timeEnabled, t.timeEnabled);
    SetEditText(st->timeOfDay, TimeOfDayToString((t.timeOfDaySec >= 0) ? t.timeOfDaySec : 0));
    SetHotkeyCombosFromString(st, t.hotkey);
    ApplyActionModeUi(st);
    ApplyTriggerUiState(st);
    st->actionPathDirty = false;
    st->actionArgsDirty = false;
    st->actionCwdDirty = false;
    st->dateEnabledDirty = false;
    st->dateValueDirty = false;
    st->weekdayDirty = false;
    st->timeEnabledDirty = false;
    st->timeOfDayDirty = false;
    st->hotkeyDirty = false;
    st->retryDirty = false;
    UpdateMainWindowTitle(st);
    tcyc_sync_controls(st);
}

bool SignalReloadEvent() {
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, kReloadEventName);
    if (!h) return false;
    BOOL ok = SetEvent(h);
    CloseHandle(h);
    return ok == TRUE;
}

bool SaveAllToIni(WindowState* st, std::wstring& err) {
    if (!st) return false;
    const std::wstring iniPath = st->iniPath;

    st->config.pollSec = ParseIntOrDefault(GetEditText(st->pollSec), st->config.pollSec, 1, 60);
    st->config.graceSec = ParseIntOrDefault(GetEditText(st->graceSec), st->config.graceSec, 0, 300);
    if (st->config.graceSec <= 0) st->config.graceSec = 60;

    int effectiveTaskIndex = st->selectedTask;
    bool hasValidTaskSelection = false;
    if (st->taskList) {
        const int lbSel = static_cast<int>(SendMessageW(st->taskList, LB_GETCURSEL, 0, 0));
        if (lbSel >= 0 && lbSel < static_cast<int>(st->config.tasks.size())) {
            effectiveTaskIndex = lbSel;
            st->selectedTask = lbSel;
            hasValidTaskSelection = true;
        }
    }
    if (effectiveTaskIndex < 0 && !st->config.tasks.empty()) {
        effectiveTaskIndex = 0;
        st->selectedTask = 0;
    }

    if (hasValidTaskSelection && effectiveTaskIndex >= 0 && effectiveTaskIndex < static_cast<int>(st->config.tasks.size())) {
        auto& t = st->config.tasks[static_cast<size_t>(effectiveTaskIndex)];
        if (st->taskEnabled) {
            t.enabled = (SendMessageW(st->taskEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        if (st->singleInstance) {
            t.singleInstance = (SendMessageW(st->singleInstance, BM_GETCHECK, 0, 0) == BST_CHECKED);
        }
        if (t.name.empty()) {
            wchar_t buf[64] = {0};
            swprintf_s(buf, L"Task.%d", t.id);
            t.name = buf;
        }
        const int uiTriggerMask = ReadTriggerMaskFromChecks(st);
        if (uiTriggerMask != 0) {
            t.triggerMask = uiTriggerMask | (t.triggerMask & (1 << 1));
        } else if (t.triggerMask == 0) {
            t.triggerMask = TriggerTypeToBit(tcyc::TriggerType::Startup);
            SetTriggerChecksFromMask(st, t.triggerMask);
        }
        t.trigger = TriggerIndexToType(FirstTriggerFromMask(t.triggerMask));
        t.intervalSec = ParseIntOrDefault(GetEditText(st->intervalSec), (t.intervalSec > 0 ? t.intervalSec : 600), 0, 86400);
        const int actionSel = static_cast<int>(SendMessageW(st->actionMode, CB_GETCURSEL, 0, 0));
        if (actionSel >= 0) {
            t.actionMode = ActionModeFromIndex(actionSel);
        }
        if (st->actionPathDirty) {
            t.actionPath = TrimWide(GetEditText(st->actionPath));
        }
        if (st->actionArgsDirty) {
            t.actionArgs = TrimWide(GetEditText(st->actionArgs));
        }
        if (st->actionCwdDirty) {
            t.actionCwd = TrimWide(GetEditText(st->actionCwd));
        }
        t.watchdogEnabled = ((t.triggerMask & TriggerTypeToBit(tcyc::TriggerType::NonRunning)) != 0);
        t.watchdogRetrySec = ParseIntOrDefault(GetEditText(st->watchdogRetrySec), t.watchdogRetrySec, 10, 3600);
        const int prevWatchdogMaxRetry = t.watchdogMaxRetry;
        const std::wstring retryInput = TrimWide(GetEditText(st->repeatCount));
        if (st->retryDirty) {
            t.watchdogMaxRetry = ParseIntOrDefault(retryInput, (prevWatchdogMaxRetry >= 0 ? prevWatchdogMaxRetry : 5), 1, 1000000);
        }
        if (st->dateEnabledDirty) {
            t.dateEnabled = IsChecked(st->dateEnabled);
        }
        const std::wstring prevDateYmd = t.dateYmd;
        if (st->dateValueDirty || st->dateEnabledDirty) {
            const std::wstring inputDateYmd = TrimWide(GetEditText(st->dateValue));
            if (!inputDateYmd.empty()) {
                t.dateYmd = inputDateYmd;
            } else {
                t.dateYmd = prevDateYmd.empty() ? CurrentDateYmd() : prevDateYmd;
            }
        }
        if (st->weekdayDirty || st->dateEnabledDirty) {
            t.weekdayEnabled = IsChecked(st->weekdayEnabled);
            t.weeklyEveryday = t.weekdayEnabled && IsEverydayChecked(st);
            if (!t.weekdayEnabled) {
                t.weekday = -1;
                t.weekdayMask = 0;
            } else if (t.weeklyEveryday) {
                t.weekday = -1;
                t.weekdayMask = 0;
            } else {
                t.weekdayMask = ReadWeekdayMaskFromChecks(st);
                t.weekday = FirstWeekdayFromMask(t.weekdayMask);
            }
        }

        if (st->timeEnabledDirty) {
            t.timeEnabled = IsChecked(st->timeEnabled);
        }
        if (t.timeEnabled && (st->timeOfDayDirty || st->timeEnabledDirty)) {
            const std::wstring tod = TrimWide(GetEditText(st->timeOfDay));
            if (tod.empty()) {
                if (t.timeOfDaySec < 0) t.timeOfDaySec = 0;
            } else {
                int sec = -1;
                if (ParseTimeOfDay(tod, sec)) t.timeOfDaySec = sec;
                else if (t.timeOfDaySec < 0) t.timeOfDaySec = 0;
            }
        }
        const int modSel = static_cast<int>(SendMessageW(st->hotkeyMod, CB_GETCURSEL, 0, 0));
        const int keySel = static_cast<int>(SendMessageW(st->hotkeyKey, CB_GETCURSEL, 0, 0));
        if (st->hotkeyDirty) {
            if (modSel >= 0 && keySel >= 0) {
                const auto& keyOptions = HotkeyKeyOptions();
                const bool keyInRange = (keySel < static_cast<int>(keyOptions.size()));
                const bool keySelected = keyInRange && (keyOptions[static_cast<size_t>(keySel)].vk > 0);
                const bool clearHotkey = (modSel == 0 && keySel == 0);
                if (keySelected || clearHotkey) {
                    t.hotkey = ComposeHotkeyString(st);
                }
            }
        }
    }

    std::vector<tcyc::IniUpdate> updates{
        {L"TCycle", L"PollSec", std::to_wstring(st->config.pollSec)},
        {L"TCycle", L"GraceSec", std::to_wstring(st->config.graceSec)},
        {L"TCycle", L"Language", Utf8ToWide(st->languageCode)}};
    auto writeTaskInt = [&](const wchar_t* sec, const wchar_t* key, int value) -> bool {
        updates.push_back({sec, key, std::to_wstring(value)});
        return true;
    };
    auto writeTaskStr = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& value) -> bool {
        updates.push_back({sec, key, value});
        return true;
    };

    for (const auto& t : st->config.tasks) {
        wchar_t sec[32] = {0};
        swprintf_s(sec, L"Task.%d", t.id);
        if (!writeTaskInt(sec, L"Enabled", t.enabled ? 1 : 0)) return false;
        if (!writeTaskStr(sec, L"Name", t.name)) return false;
        if (!writeTaskStr(sec, L"TriggerTypes", TriggerMaskToString(t.triggerMask))) return false;
        if (!writeTaskStr(sec, L"TriggerType", TriggerToString(t.trigger))) return false;
        if (!writeTaskInt(sec, L"IntervalSec", t.intervalSec)) return false;
        if (!writeTaskStr(sec, L"ActionMode", t.actionMode)) return false;
        if (!writeTaskStr(sec, L"ActionPath", t.actionPath)) return false;
        if (!writeTaskStr(sec, L"ActionArgs", t.actionArgs)) return false;
        if (!writeTaskStr(sec, L"ActionCwd", t.actionCwd)) return false;
        if (!writeTaskInt(sec, L"SingleInstance", t.singleInstance ? 1 : 0)) return false;
        if (!writeTaskInt(sec, L"WatchdogEnabled", t.watchdogEnabled ? 1 : 0)) return false;
        if (!writeTaskInt(sec, L"WatchdogRetrySec", t.watchdogRetrySec)) return false;
        if (!writeTaskInt(sec, L"WatchdogMaxRetry", t.watchdogMaxRetry)) return false;
        if (!writeTaskStr(sec, L"StartDateTime", t.startDateTime)) return false;
        if (!writeTaskInt(sec, L"RepeatEverySec", t.repeatEverySec)) return false;
        if (!writeTaskInt(sec, L"DateEnabled", t.dateEnabled ? 1 : 0)) return false;
        if (!writeTaskStr(sec, L"Date", t.dateYmd)) return false;
        if (!writeTaskInt(sec, L"WeekdayEnabled", t.weekdayEnabled ? 1 : 0)) return false;
        if (!writeTaskInt(sec, L"EveryDay", t.weeklyEveryday ? 1 : 0)) return false;
        if (!writeTaskStr(sec, L"Weekdays", (t.weeklyEveryday ? L"everyday" : WeekdayMaskToString(t.weekdayMask)))) return false;
        if (!writeTaskStr(sec, L"Weekday", WeekdayToString(t.weekday))) return false;
        if (!writeTaskInt(sec, L"TimeEnabled", t.timeEnabled ? 1 : 0)) return false;
        if (!writeTaskStr(sec, L"TimeOfDay", TimeOfDayToString(t.timeOfDaySec))) return false;
        if (!writeTaskStr(sec, L"Hotkey", t.hotkey)) return false;
    }
    if (!tcyc::WriteIniUtf8Values(iniPath, updates, st->deletedTaskSections)) {
        err = Tr(st, L"err_save_global", L"Failed to save global settings to ini.");
        return false;
    }
    st->actionPathDirty = false;
    st->actionArgsDirty = false;
    st->actionCwdDirty = false;
    st->dateEnabledDirty = false;
    st->dateValueDirty = false;
    st->weekdayDirty = false;
    st->timeEnabledDirty = false;
    st->timeOfDayDirty = false;
    st->hotkeyDirty = false;
    st->retryDirty = false;
    st->deletedTaskSections.clear();
    return true;
}

void PersistRealtime(WindowState* st, bool showErrorDialog) {
    if (!st || st->suppressEvents) return;
    std::wstring err;
    if (!SaveAllToIni(st, err)) {
        SetStatus(st, err);
        if (showErrorDialog) MessageBoxW(nullptr, err.c_str(), Tr(st, L"app_name", L"TCycle").c_str(), MB_ICONERROR);
        return;
    }
    (void)SignalReloadEvent();
    int statusTaskIndex = st->selectedTask;
    if (st->taskList) {
        const int lbSel = static_cast<int>(SendMessageW(st->taskList, LB_GETCURSEL, 0, 0));
        if (lbSel >= 0 && lbSel < static_cast<int>(st->config.tasks.size())) {
            statusTaskIndex = lbSel;
        }
    }
    const std::wstring status = BuildStatusWithNextRun(st, Tr(st, L"status_realtime_saved", L"Realtime saved."), statusTaskIndex);
    SetStatus(st, status.c_str());

    int prevSel = st->selectedTask;
    st->suppressEvents = true;
    PopulateTaskList(st);
    if (prevSel >= 0 && prevSel < static_cast<int>(st->config.tasks.size())) {
        LoadTaskControls(st, prevSel);
    }
    st->suppressEvents = false;
}

LRESULT CALLBACK RenameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<RenameDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* init = reinterpret_cast<RenameDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(init));
        return TRUE;
    }
    case WM_CREATE: {
        if (!st) return -1;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->initialText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 12, 320, 24, hwnd, nullptr, nullptr, nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            172, 46, 76, 26, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            256, 46, 76, 26, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(st->edit, EM_SETSEL, 0, -1);
        SetFocus(st->edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            st->resultText = TrimWide(GetEditText(st->edit));
            st->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool PromptRename(HWND owner, const std::wstring& title, const std::wstring& current, std::wstring& outName) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RenameWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kRenameClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    RenameDialogState state{};
    state.title = title;
    state.initialText = current;

    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kRenameClassName, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 350, 115,
        owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!wnd) return false;
    native_edit::prepare(wnd);

    EnableWindow(owner, FALSE);
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);

    MSG msg{};
    while (IsWindow(wnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (native_edit::translate(msg)) continue;
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (!state.accepted || state.resultText.empty()) return false;
    outName = state.resultText;
    return true;
}

void CancelInlineRenameTask(WindowState* st);
bool CommitInlineRenameTask(WindowState* st);
void RefreshAll(WindowState* st);

LRESULT CALLBACK InlineEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            CommitInlineRenameTask(st);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            CancelInlineRenameTask(st);
            return 0;
        }
    } else if (msg == WM_CHAR) {
        if (wParam == VK_RETURN) {
            CommitInlineRenameTask(st);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            CancelInlineRenameTask(st);
            return 0;
        }
    } else if (msg == WM_KILLFOCUS) {
        CommitInlineRenameTask(st);
        return 0;
    }
    if (st->inlineEditOldProc) return CallWindowProcW(st->inlineEditOldProc, hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool BeginInlineRenameTask(WindowState* st) {
    if (!st || !st->taskList || !st->mainWindow || st->inlineEdit) return false;
    const int idx = st->selectedTask;
    if (idx < 0 || idx >= static_cast<int>(st->config.tasks.size())) return false;
    RECT rc{};
    if (SendMessageW(st->taskList, LB_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&rc)) == LB_ERR) return false;
    // Use listbox-local coordinates and parent to avoid hit-test/focus mismatch.
    int x = rc.left;
    int y = rc.top;
    int w = (rc.right - rc.left);
    int h = (rc.bottom - rc.top);
    const int inset=MulDiv(10,st->dpi,96);
    x+=inset;y+=inset;w-=inset*2;h=MulDiv(30,st->dpi,96);
    st->inlineEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x, y, w, h,
        st->taskList, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlTaskInlineRename)), nullptr, nullptr);
    if (!st->inlineEdit) return false;
    native_edit::attach(st->inlineEdit);

    st->inlineEditIndex = idx;
    SendMessageW(st->inlineEdit, WM_SETFONT, SendMessageW(st->taskList, WM_GETFONT, 0, 0), TRUE);
    SetWindowTextW(st->inlineEdit, st->config.tasks[static_cast<size_t>(idx)].name.c_str());
    SendMessageW(st->inlineEdit, EM_SETSEL, 0, -1);
    SetWindowLongPtrW(st->inlineEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    st->inlineEditOldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(st->inlineEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InlineEditSubclassProc)));
    SetFocus(st->inlineEdit);
    SendMessageW(st->inlineEdit, EM_SETSEL, 0, -1);
    PostMessageW(st->mainWindow, kMsgFocusInlineRename, 0, 0);
    return true;
}

LRESULT CALLBACK TaskListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (st && msg == WM_KEYDOWN && wParam == VK_F2) {
        BeginInlineRenameTask(st);
        return 0;
    }
    if (st && msg == WM_LBUTTONDOWN && st->globalView) {
        st->globalView = false;
        st->scrollOffset = 0;
        tcyc_sync_controls(st);
    }
    if (st && msg == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            RECT bounds{}; GetWindowRect(hwnd, &bounds);
            point = {bounds.left + 16, bounds.top + 16};
        }
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kCtrlTaskRename, Tr(st, L"button_rename", L"Rename").c_str());
        const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, st->mainWindow, nullptr);
        DestroyMenu(menu);
        if (choice == kCtrlTaskRename) BeginInlineRenameTask(st);
        return 0;
    }
    if (st && st->taskListOldProc) return CallWindowProcW(st->taskListOldProc, hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DestroyInlineRenameTask(WindowState* st) {
    if (!st || !st->inlineEdit) return;
    HWND edit = st->inlineEdit;
    if (st->inlineEditOldProc) {
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(st->inlineEditOldProc));
        st->inlineEditOldProc = nullptr;
    }
    st->inlineEdit = nullptr;
    st->inlineEditIndex = -1;
    DestroyWindow(edit);
}

void CancelInlineRenameTask(WindowState* st) {
    if (!st || !st->inlineEdit) return;
    DestroyInlineRenameTask(st);
    if (st->taskList) SetFocus(st->taskList);
}

bool CommitInlineRenameTask(WindowState* st) {
    if (!st || !st->inlineEdit) return false;
    if (st->inlineFinalizing) return false;
    st->inlineFinalizing = true;
    const int idx = st->inlineEditIndex;
    const std::wstring newName = TrimWide(GetEditText(st->inlineEdit));
    DestroyInlineRenameTask(st);
    if (!newName.empty() && idx >= 0 && idx < static_cast<int>(st->config.tasks.size())) {
        st->config.tasks[static_cast<size_t>(idx)].name = newName;
        st->selectedTask = idx;
        PersistRealtime(st, true);
        RefreshAll(st);
    }
    st->inlineFinalizing = false;
    return true;
}

void AddTask(WindowState* st) {
    int maxId = 0;
    for (const auto& t : st->config.tasks) maxId = (t.id > maxId) ? t.id : maxId;
    tcyc::TaskConfig t{};
    t.id = maxId + 1;
    wchar_t nameBuf[64] = {0};
    swprintf_s(nameBuf, L"Task.%d", t.id);
    t.name = nameBuf;
    t.enabled = true;
    t.trigger = tcyc::TriggerType::Startup;
    t.triggerMask = TriggerTypeToBit(tcyc::TriggerType::Startup);
    t.intervalSec = 600;
    t.actionMode = L"program";
    t.watchdogEnabled = false;
    t.watchdogRetrySec = 10;
    t.watchdogMaxRetry = 5;
    t.startDateTime = L"";
    t.repeatEverySec = 600;
    t.repeatCount = 5;
    t.dateEnabled = false;
    t.dateYmd = L"";
    t.weekdayEnabled = true;
    t.weeklyEveryday = false;
    t.weekday = 6;
    t.weekdayMask = (1 << 6);
    t.timeEnabled = true;
    t.timeOfDaySec = 0;
    t.hotkey = L"";
    st->config.tasks.push_back(t);
    st->selectedTask = static_cast<int>(st->config.tasks.size()) - 1;
}

void DeleteSelectedTask(WindowState* st) {
    if (st->selectedTask < 0 || st->selectedTask >= static_cast<int>(st->config.tasks.size())) return;
    if (st->config.tasks.size() <= 1) return;
    st->deletedTaskSections.push_back(L"Task." + std::to_wstring(st->config.tasks[st->selectedTask].id));
    st->config.tasks.erase(st->config.tasks.begin() + st->selectedTask);
    if (st->selectedTask >= static_cast<int>(st->config.tasks.size())) st->selectedTask = static_cast<int>(st->config.tasks.size()) - 1;
}

void InitializeCombos(WindowState* st) {
    SendMessageW(st->actionMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(st, L"action_mode_program", L"Program").c_str()));
    SendMessageW(st->actionMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(st, L"action_mode_command", L"Command").c_str()));
    SendMessageW(st->actionMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(st, L"action_mode_shell", L"Shell").c_str()));

    for (const auto& m : HotkeyModOptions()) {
        const std::wstring label = wcscmp(m.label, L"(none)") == 0 ? Tr(st, L"option_none", L"None") : m.label;
        SendMessageW(st->hotkeyMod, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    for (const auto& k : HotkeyKeyOptions()) {
        const auto label = k.label == L"(none)" ? Tr(st, L"option_none", L"None") : k.label;
        SendMessageW(st->hotkeyKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}

void RefreshAll(WindowState* st) {
    st->suppressEvents = true;
    LoadGlobalControls(st);
    PopulateTaskList(st);
    LoadTaskControls(st, st->selectedTask);
    UpdateMainWindowTitle(st);
    st->suppressEvents = false;
}

#include "settings_ui.h"

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* createState = reinterpret_cast<WindowState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createState));
        return TRUE;
    }
    case WM_CREATE: {
        if (!st) return -1;
        st->mainWindow = hwnd;
        st->suppressEvents = true;
        tcyc_build_controls(st);
        SetWindowLongPtrW(st->taskList, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->taskListOldProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(st->taskList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TaskListSubclassProc)));
        InitializeCombos(st);
        ApplyActionModeUi(st);
        RefreshAll(st);
        tcyc_size_window(st);
        SetStatus(st, BuildStatusWithNextRun(st, Tr(st, L"status_ready", L"Ready."), st->selectedTask));
        SetTimer(hwnd, kTaskListTimerId, 1000, nullptr);
        native_edit::prepare(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kTaskListTimerId && st && !st->suppressEvents) {
            if (st->inlineEdit) return 0;
            // Redraw live countdowns without resetting selection or list scroll.
            InvalidateRect(st->taskList, nullptr, FALSE);
        }
        return 0;
    case WM_SIZE:
        if(st && wParam!=SIZE_MINIMIZED)tcyc_layout_controls(st);
        return 0;
    case WM_DPICHANGED:
        if(st){
            tcyc_refresh_fonts(st);
            const auto* r=reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            tcyc_layout_controls(st);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if(st){
            UINT dpi=GetDpiForWindow(hwnd);if(!dpi)dpi=96;
            RECT r{0,0,MulDiv(720,dpi,96),MulDiv(430,dpi,96)};
            AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)),FALSE,
                                    static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)),dpi);
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize={r.right-r.left,r.bottom-r.top};
        }
        return 0;
    case WM_DRAWITEM:
        if(st && tcyc_draw_item(st,*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))return TRUE;
        return FALSE;
    case WM_MEASUREITEM:
        if(st)reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight=MulDiv(70,st->dpi,96);
        return TRUE;
    case WM_PAINT:
        if(st){PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);tcyc_paint_surface(st,hwnd,ps.hdc);EndPaint(hwnd,&ps);}
        return 0;
    case WM_ERASEBKGND:
        if(st)tcyc_paint_surface(st,hwnd,reinterpret_cast<HDC>(wParam));
        return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return tcyc_control_color(st,msg,wParam,lParam);
    case kMsgFocusInlineRename:
        if (st && st->inlineEdit && IsWindow(st->inlineEdit)) {
            SetFocus(st->inlineEdit);
            SendMessageW(st->inlineEdit, EM_SETSEL, 0, -1);
        }
        return 0;
    case WM_COMMAND: {
        if (!st) return 0;
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if(st->suppressEvents)return 0;
        if(tcyc_handle_command(st,wParam,lParam))return 0;

        if (id == kCtrlTaskList && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            int idx = static_cast<int>(SendMessageW(st->taskList, LB_GETCURSEL, 0, 0));
            if (st->inlineEdit && idx != st->inlineEditIndex) {
                CancelInlineRenameTask(st);
            }
            if (code == LBN_DBLCLK) {
                BeginInlineRenameTask(st);
                return 0;
            }
            st->globalView=false;
            st->scrollOffset=0;
            st->suppressEvents = true;
            LoadTaskControls(st, idx);
            st->suppressEvents = false;
            SetStatus(st, BuildStatusWithNextRun(st, Tr(st, L"status_task_selected", L"Task selected."), idx));
            return 0;
        }
        if (id == kCtrlTaskAdd && code == BN_CLICKED) {
            AddTask(st);
            st->globalView=false;
            RefreshAll(st);
            PersistRealtime(st, true);
            RefreshAll(st);
            return 0;
        }
        if (id == kCtrlTaskDelete && code == BN_CLICKED) {
            if (st->config.tasks.size() <= 1) {
                SetStatus(st, Tr(st, L"status_keep_one_task", L"At least one task must remain."));
                return 0;
            }
            DeleteSelectedTask(st);
            RefreshAll(st);
            PersistRealtime(st, true);
            RefreshAll(st);
            return 0;
        }
        if (id == kCtrlTaskRename && code == BN_CLICKED) {
            if (st->selectedTask < 0 || st->selectedTask >= static_cast<int>(st->config.tasks.size())) return 0;
            if (!st->inlineEdit) {
                BeginInlineRenameTask(st);
            }
            return 0;
        }
        if (id == kCtrlTaskTestRun && code == BN_CLICKED) {
            std::wstring saveErr;
            if (!SaveAllToIni(st, saveErr)) {
                SetStatus(st, saveErr);
                MessageBoxW(hwnd, saveErr.c_str(), Tr(st, L"app_name", L"TCycle").c_str(), MB_ICONERROR);
                return 0;
            }
            (void)SignalReloadEvent();

            int idx = st->selectedTask;
            if (st->taskList) {
                const int lbSel = static_cast<int>(SendMessageW(st->taskList, LB_GETCURSEL, 0, 0));
                if (lbSel >= 0 && lbSel < static_cast<int>(st->config.tasks.size())) {
                    idx = lbSel;
                    st->selectedTask = lbSel;
                }
            }
            if (idx < 0 || idx >= static_cast<int>(st->config.tasks.size())) {
                SetStatus(st, Tr(st, L"status_test_run_no_task", L"No task selected."));
                return 0;
            }

            tcyc::TaskConfig runTask = st->config.tasks[static_cast<size_t>(idx)];
            if (runTask.actionMode == L"program") {
                runTask.actionPath = tcyc::ResolvePathFromExe(st->exeDir, runTask.actionPath);
            }
            if (!runTask.actionCwd.empty()) {
                runTask.actionCwd = tcyc::ResolvePathFromExe(st->exeDir, runTask.actionCwd);
            }

            if (runTask.actionPath.empty()) {
                SetStatus(st, Tr(st, L"status_test_run_path_empty", L"Test run skipped: action path is empty."));
                return 0;
            }
            if (runTask.singleInstance) {
                tcyc::ProcessMatchMode mode = tcyc::ProcessMatchMode::None;
                if (tcyc::IsTaskProcessRunning(runTask, &mode, st->config.debugForceCmdlineReadFail)) {
                    SetStatus(st, Tr(st, L"status_test_run_already_running", L"Test run skipped: already running."));
                    return 0;
                }
            }

            std::wstring launchErr;
            if (!tcyc::LaunchTask(runTask, launchErr)) {
                std::wstring failMsg = Tr(st, L"status_test_run_failed_prefix", L"Test run failed: ");
                failMsg += launchErr;
                SetStatus(st, failMsg);
                MessageBoxW(hwnd, failMsg.c_str(), Tr(st, L"app_name", L"TCycle").c_str(), MB_ICONERROR);
                return 0;
            }
            SetStatus(st, Tr(st, L"status_test_run_ok", L"Test run launched."));
            return 0;
        }
        if (id == kCtrlActionMode && code == CBN_SELCHANGE) {
            ApplyActionModeUi(st);
            ApplyTriggerUiState(st);
            PersistRealtime(st, false);
            return 0;
        }
        if (id == kCtrlActionPath && code == EN_CHANGE) {
            st->actionPathDirty = true;
            ApplyTriggerUiState(st);
            return 0;
        }
        if (id == kCtrlActionArgs && code == EN_CHANGE) {
            st->actionArgsDirty = true;
            ApplyTriggerUiState(st);
            return 0;
        }
        if (id == kCtrlActionCwd && code == EN_CHANGE) {
            st->actionCwdDirty = true;
            return 0;
        }

        const bool triggerClick =
            (id == kCtrlTriggerInterval) ||
            (id == kCtrlTriggerWeeklyTime) ||
            (id == kCtrlTriggerStartup) ||
            (id == kCtrlTriggerHotkeyOnly) ||
            (id == kCtrlTriggerNonRunning);
        if (triggerClick && code == BN_CLICKED) {
            int clickedIndex = 0;
            if (id == kCtrlTriggerInterval) clickedIndex = 0;
            else if (id == kCtrlTriggerWeeklyTime) clickedIndex = 2;
            else if (id == kCtrlTriggerStartup) clickedIndex = 3;
            else if (id == kCtrlTriggerHotkeyOnly) clickedIndex = 4;
            else if (id == kCtrlTriggerNonRunning) clickedIndex = 5;
            EnsureAtLeastOneTriggerChecked(st, clickedIndex);
            if (id == kCtrlTriggerWeeklyTime && SendMessageW(st->triggerChecks[2], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                EnsureAtLeastOneDateConditionChecked(st, kCtrlTimeEnabled);
            }
            ApplyTriggerUiState(st);
            PersistRealtime(st, false);
            return 0;
        }

        const bool conditionClick =
            (id == kCtrlDateEnabled) ||
            (id == kCtrlWeekdayEnabled) ||
            (id == kCtrlTimeEnabled) ||
            (id == kCtrlWeekdayEveryday);
        if (conditionClick && code == BN_CLICKED) {
            if (id == kCtrlDateEnabled) st->dateEnabledDirty = true;
            if (id == kCtrlWeekdayEnabled || id == kCtrlWeekdayEveryday) st->weekdayDirty = true;
            if (id == kCtrlTimeEnabled) st->timeEnabledDirty = true;
            if (HasTriggerEnabled(st, tcyc::TriggerType::WeeklyTime)) {
                if (id == kCtrlDateEnabled && IsChecked(st->dateEnabled)) {
                    SetChecked(st->weekdayEnabled, false);
                    st->weekdayDirty = true;
                } else if (id == kCtrlWeekdayEnabled && IsChecked(st->weekdayEnabled)) {
                    SetChecked(st->dateEnabled, false);
                    st->dateEnabledDirty = true;
                }
                if (!IsChecked(st->timeEnabled)) {
                    SetChecked(st->timeEnabled, true);
                    st->timeEnabledDirty = true;
                }
                if (id == kCtrlDateEnabled || id == kCtrlWeekdayEnabled || id == kCtrlTimeEnabled) {
                    EnsureAtLeastOneDateConditionChecked(st, id);
                }
            }
            ApplyTriggerUiState(st);
            PersistRealtime(st, false);
            return 0;
        }

        const bool realtimeChange =
            (id == kCtrlPollSec && code == EN_CHANGE) ||
            (id == kCtrlGraceSec && code == EN_CHANGE) ||
            (id == kCtrlTaskEnabled && code == BN_CLICKED) ||
            (id == kCtrlSingleInstance && code == BN_CLICKED) ||
            (id == kCtrlActionPath && code == EN_KILLFOCUS) ||
            (id == kCtrlActionArgs && code == EN_KILLFOCUS) ||
            (id == kCtrlActionCwd && code == EN_KILLFOCUS) ||
            (id == kCtrlIntervalSec && code == EN_CHANGE) ||
            (id == kCtrlWatchdogRetrySec && code == EN_CHANGE) ||
            (id == kCtrlRepeatCount && code == EN_CHANGE) ||
            ((id >= kCtrlWeekdaySun && id <= kCtrlWeekdaySat) && code == BN_CLICKED) ||
            (id == kCtrlDateValue && code == EN_KILLFOCUS) ||
            (id == kCtrlTimeOfDay && code == EN_KILLFOCUS) ||
            (id == kCtrlHotkeyMod && code == CBN_SELCHANGE) ||
            (id == kCtrlHotkeyKey && code == CBN_SELCHANGE);

        if (realtimeChange) {
            if(id==kCtrlRepeatCount)st->retryDirty=true;
            if ((id >= kCtrlWeekdaySun && id <= kCtrlWeekdaySat) && code == BN_CLICKED) st->weekdayDirty = true;
            if (id == kCtrlDateValue && code == EN_KILLFOCUS) st->dateValueDirty = true;
            if (id == kCtrlTimeOfDay && code == EN_KILLFOCUS) st->timeOfDayDirty = true;
            const bool hotkeyChanged = ((id == kCtrlHotkeyMod && code == CBN_SELCHANGE) || (id == kCtrlHotkeyKey && code == CBN_SELCHANGE));
            if (hotkeyChanged) {
                st->hotkeyDirty = true;
                if (!ShouldPersistHotkeySelection(st)) {
                    SetStatus(st, Tr(st, L"status_hotkey_key_pending", L"Hotkey modifier selected. Choose key to save."));
                    return 0;
                }
            }
            PersistRealtime(st, false);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, kTaskListTimerId);
        if (st && st->inlineEdit) {
            CancelInlineRenameTask(st);
        }
        if (st && st->taskList && st->taskListOldProc) {
            SetWindowLongPtrW(st->taskList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(st->taskListOldProc));
            st->taskListOldProc = nullptr;
        }
        if (st && !st->suppressEvents) {
            PersistRealtime(st, false);
            st->suppressEvents = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if(st){for(HFONT font:{st->uiFont,st->smallFont,st->titleFont})if(font)DeleteObject(font);}
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int tcyc::RunReadOnlySettingsWindow(const RuntimeConfig& cfg, const std::wstring& iniPath, const std::wstring& exeDir, const std::string& preferredLanguage) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WindowState state{};
    state.config = cfg;
    state.iniPath = iniPath;
    state.exeDir = exeDir;
    state.languageCode = preferredLanguage.empty() ? ReadLanguageFromIni(iniPath) : ToLowerAscii(preferredLanguage);
    LoadLanguage(&state, state.languageCode);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kTcycleIconResId), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    std::wstring windowTitle = BuildMainWindowTitle(&state);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kWindowClassName,
        windowTitle.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 742, 648,
        nullptr,
        nullptr,
        inst,
        &state);
    if (!hwnd) return 1;

    SetWindowTextW(hwnd, windowTitle.c_str());
    UpdateMainWindowTitle(&state);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (native_edit::translate(msg)) continue;
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
