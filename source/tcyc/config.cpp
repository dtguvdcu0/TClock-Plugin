#include "config.h"
#include "ini_utf8_util.h"

#include <windows.h>
#include <shlwapi.h>
#include <time.h>
#include <stdexcept>

#pragma comment(lib, "Shlwapi.lib")

namespace tcyc {

namespace {

int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

std::wstring ReadIniString(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key, const wchar_t* defval) {
    std::wstring out;
    if (ReadIniUtf8Value(iniPath, section ? section : L"", key ? key : L"", defval ? defval : L"", out)) return out;
    throw std::runtime_error("INI read failed");
}

int ReadIniInt(const std::wstring& iniPath, const wchar_t* section, const wchar_t* key, int defval) {
    const std::wstring raw = ReadIniString(iniPath, section, key, L"");
    if (raw.empty()) return defval;
    wchar_t* end = nullptr;
    long v = wcstol(raw.c_str(), &end, 10);
    if (!end || *end != L'\0') return defval;
    return static_cast<int>(v);
}

bool TryParseTimeOfDay(const std::wstring& s, int& outSec) {
    int hh = 0, mm = 0, ss = 0;
    if (swscanf_s(s.c_str(), L"%d:%d:%d", &hh, &mm, &ss) < 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) return false;
    outSec = hh * 3600 + mm * 60 + ss;
    return true;
}

bool TryParseLocalDateTime(const std::wstring& s, long long& outUnix) {
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (swscanf_s(s.c_str(), L"%d-%d-%d %d:%d:%d", &y, &mo, &d, &hh, &mm, &ss) < 5) return false;
    std::tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    if (t == static_cast<time_t>(-1)) return false;
    outUnix = static_cast<long long>(t);
    return true;
}

int ParseWeekday(const std::wstring& raw) {
    const std::wstring s = ToLower(raw);
    if (s == L"sun" || s == L"sunday") return 0;
    if (s == L"mon" || s == L"monday") return 1;
    if (s == L"tue" || s == L"tuesday") return 2;
    if (s == L"wed" || s == L"wednesday") return 3;
    if (s == L"thu" || s == L"thursday") return 4;
    if (s == L"fri" || s == L"friday") return 5;
    if (s == L"sat" || s == L"saturday") return 6;
    return -1;
}

int ParseWeekdayMask(const std::wstring& raw) {
    std::wstring token;
    int mask = 0;
    auto flush = [&]() {
        const int d = ParseWeekday(token);
        if (d >= 0 && d <= 6) {
            mask |= (1 << d);
        }
        token.clear();
    };
    for (wchar_t ch : raw) {
        if (ch == L',' || ch == L';' || ch == L'|' || ch == L' ' || ch == L'\t') {
            if (!token.empty()) flush();
        } else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) flush();
    return mask;
}

bool ParseWeekdayEveryday(const std::wstring& raw) {
    std::wstring token;
    bool everyday = false;
    auto flush = [&]() {
        const std::wstring s = ToLower(token);
        if (s == L"everyday" || s == L"daily" || s == L"all") everyday = true;
        token.clear();
    };
    for (wchar_t ch : raw) {
        if (ch == L',' || ch == L';' || ch == L'|' || ch == L' ' || ch == L'\t') {
            if (!token.empty()) flush();
        } else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) flush();
    return everyday;
}

TriggerType ParseTriggerType(const std::wstring& raw) {
    const std::wstring s = ToLower(raw);
    if (s == L"interval") return TriggerType::Interval;
    if (s == L"datetime_interval_limited") return TriggerType::DateTimeIntervalLimited;
    if (s == L"weekly_time") return TriggerType::WeeklyTime;
    if (s == L"startup") return TriggerType::Startup;
    if (s == L"hotkey_only") return TriggerType::HotkeyOnly;
    if (s == L"non_running") return TriggerType::NonRunning;
    return TriggerType::Unknown;
}

int TriggerTypeToBit(TriggerType t) {
    switch (t) {
    case TriggerType::Interval: return (1 << 0);
    case TriggerType::DateTimeIntervalLimited: return (1 << 1);
    case TriggerType::WeeklyTime: return (1 << 2);
    case TriggerType::Startup: return (1 << 3);
    case TriggerType::HotkeyOnly: return (1 << 4);
    case TriggerType::NonRunning: return (1 << 5);
    default: return 0;
    }
}

TriggerType FirstTriggerTypeFromMask(int mask) {
    for (int i = 0; i < 6; ++i) {
        if ((mask & (1 << i)) == 0) continue;
        switch (i) {
        case 0: return TriggerType::Interval;
        case 1: return TriggerType::DateTimeIntervalLimited;
        case 2: return TriggerType::WeeklyTime;
        case 3: return TriggerType::Startup;
        case 4: return TriggerType::HotkeyOnly;
        case 5: return TriggerType::NonRunning;
        default: break;
        }
    }
    return TriggerType::Unknown;
}

int ParseTriggerMask(const std::wstring& raw) {
    std::wstring token;
    int mask = 0;
    auto flush = [&]() {
        const TriggerType t = ParseTriggerType(token);
        mask |= TriggerTypeToBit(t);
        token.clear();
    };
    for (wchar_t ch : raw) {
        if (ch == L',' || ch == L';' || ch == L'|' || ch == L' ' || ch == L'\t') {
            if (!token.empty()) flush();
        } else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) flush();
    return mask;
}

void LoadTasks(const std::wstring& iniPath, const std::wstring& exeDir, std::vector<TaskConfig>& outTasks) {
    outTasks.clear();
    for (int i = 1; i <= 128; ++i) {
        wchar_t section[32] = {0};
        swprintf_s(section, L"Task.%d", i);

        TaskConfig t{};
        t.id = i;
        t.enabled = ReadIniInt(iniPath, section, L"Enabled", 0) != 0;
        t.name = ReadIniString(iniPath, section, L"Name", L"");
        t.actionPath = ReadIniString(iniPath, section, L"ActionPath", L"");
        t.actionArgs = ReadIniString(iniPath, section, L"ActionArgs", L"");
        t.actionCwd = ReadIniString(iniPath, section, L"ActionCwd", L"");
        t.actionMode = ToLower(ReadIniString(iniPath, section, L"ActionMode", L"program"));
        if (t.actionMode != L"program" && t.actionMode != L"command" && t.actionMode != L"shell") {
            t.actionMode = L"program";
        }
        t.singleInstance = ReadIniInt(iniPath, section, L"SingleInstance", 1) != 0;
        t.watchdogEnabled = ReadIniInt(iniPath, section, L"WatchdogEnabled", 0) != 0;
        t.watchdogRetrySec = ClampInt(ReadIniInt(iniPath, section, L"WatchdogRetrySec", 10), 10, 3600);
        t.watchdogMaxRetry = ReadIniInt(iniPath, section, L"WatchdogMaxRetry", -1);
        t.watchdogRequireArgsMatch = ReadIniInt(iniPath, section, L"WatchdogRequireArgsMatch", 1) != 0;
        t.intervalSec = ClampInt(ReadIniInt(iniPath, section, L"IntervalSec", 600), 0, 86400);
        t.startDateTime = ReadIniString(iniPath, section, L"StartDateTime", L"");
        t.startDateTimeValid = TryParseLocalDateTime(t.startDateTime, t.startDateTimeUnix);
        t.repeatEverySec = ClampInt(ReadIniInt(iniPath, section, L"RepeatEverySec", 0), 0, 86400);
        t.repeatCount = ClampInt(ReadIniInt(iniPath, section, L"RepeatCount", 0), 0, 1000000);
        t.hotkey = ReadIniString(iniPath, section, L"Hotkey", L"");
        t.trigger = ParseTriggerType(ReadIniString(iniPath, section, L"TriggerType", L""));
        t.triggerMask = ParseTriggerMask(ReadIniString(iniPath, section, L"TriggerTypes", L""));
        if (t.triggerMask == 0) {
            t.triggerMask = TriggerTypeToBit(t.trigger);
        } else if (t.trigger == TriggerType::Unknown) {
            t.trigger = FirstTriggerTypeFromMask(t.triggerMask);
        }
        if (t.watchdogEnabled) {
            t.triggerMask |= TriggerTypeToBit(TriggerType::NonRunning);
            if (t.trigger == TriggerType::Unknown) t.trigger = TriggerType::NonRunning;
        }
        t.weekday = ParseWeekday(ReadIniString(iniPath, section, L"Weekday", L""));
        const std::wstring weekdaysRaw = ReadIniString(iniPath, section, L"Weekdays", L"");
        t.weeklyEveryday = (ReadIniInt(iniPath, section, L"EveryDay", 0) != 0) || ParseWeekdayEveryday(weekdaysRaw);
        t.weekdayMask = ParseWeekdayMask(weekdaysRaw);
        t.dateEnabled = ReadIniInt(iniPath, section, L"DateEnabled", 0) != 0;
        t.dateYmd = ReadIniString(iniPath, section, L"Date", L"");
        t.weekdayEnabled = ReadIniInt(iniPath, section, L"WeekdayEnabled", 1) != 0;
        t.timeEnabled = ReadIniInt(iniPath, section, L"TimeEnabled", 1) != 0;
        if (t.weeklyEveryday) {
            t.weekday = -1;
            t.weekdayMask = 0;
        } else if (t.weekdayMask == 0 && t.weekday >= 0 && t.weekday <= 6) {
            t.weekdayMask = (1 << t.weekday);
        }
        t.timeOfDaySec = -1;
        {
            int tod = -1;
            if (TryParseTimeOfDay(ReadIniString(iniPath, section, L"TimeOfDay", L""), tod)) t.timeOfDaySec = tod;
        }

        if (t.actionPath.empty() && t.name.empty() && t.trigger == TriggerType::Unknown && !t.enabled) {
            continue;
        }
        if (t.actionMode == L"program") {
            t.actionPath = ResolvePathFromExe(exeDir, t.actionPath);
        }
        if (!t.actionCwd.empty()) t.actionCwd = ResolvePathFromExe(exeDir, t.actionCwd);
        outTasks.push_back(t);
    }
}

} // namespace

std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));
    if (n == 0 || n >= _countof(path)) {
        return L".";
    }
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out.append(leaf);
    return out;
}

bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

std::wstring ResolvePathFromExe(const std::wstring& exeDir, const std::wstring& path) {
    if (path.empty()) return path;
    if (IsAbsolutePath(path)) return path;
    return JoinPath(exeDir, path);
}

bool EnsureDefaultIni(const std::wstring& iniPath) {
    DWORD attrs = GetFileAttributesW(iniPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring probe;
        return ReadIniUtf8Value(iniPath, L"TCycle", L"PollSec", L"1", probe);
    }
    if (attrs != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    return WriteIniUtf8Values(iniPath, {
        {L"TCycle", L"PollSec", L"1"}, {L"TCycle", L"GraceSec", L"60"},
        {L"TCycle", L"LogLevel", L"0"}, {L"TCycle", L"LogFile", L"tcycle.log"},
        {L"TCycle", L"StateEnabled", L"0"}, {L"TCycle", L"StateFile", L"tcycle.state.ini"},
        {L"Debug", L"ForceCmdlineReadFail", L"0"},
        {L"Integration", L"TClockIniPath", L"..\\tclock-win11.ini"}});
}

bool LoadRuntimeConfig(const std::wstring& iniPath, const std::wstring& exeDir, RuntimeConfig& outConfig, std::wstring& outError) {
    outError.clear();
    try {
    if (!EnsureDefaultIni(iniPath)) {
        outError = L"Failed to read or create ini: " + iniPath;
        return false;
    }

    RuntimeConfig cfg{};
    cfg.pollSec = ClampInt(ReadIniInt(iniPath, L"TCycle", L"PollSec", 1), 1, 60);
    const int grace_sec_raw = ReadIniInt(iniPath, L"TCycle", L"GraceSec", 60);
    int grace_sec = grace_sec_raw;
    if (grace_sec <= 0) grace_sec = 60;
    if (grace_sec > 300) grace_sec = 300;
    cfg.graceSec = grace_sec;
    if (grace_sec != grace_sec_raw) {
        wchar_t buf[16] = {0};
        swprintf_s(buf, L"%d", grace_sec);
        WriteIniUtf8Value(iniPath, L"TCycle", L"GraceSec", buf);
    }
    cfg.logLevel = ClampInt(ReadIniInt(iniPath, L"TCycle", L"LogLevel", 0), 0, 3);
    cfg.logFile = ReadIniString(iniPath, L"TCycle", L"LogFile", L"tcycle.log");
    if (cfg.logFile.empty()) cfg.logFile = L"tcycle.log";
    cfg.logFile = ResolvePathFromExe(exeDir, cfg.logFile);
    cfg.stateEnabled = ReadIniInt(iniPath, L"TCycle", L"StateEnabled", 0) != 0;
    cfg.stateFile = ReadIniString(iniPath, L"TCycle", L"StateFile", L"tcycle.state.ini");
    if (cfg.stateFile.empty()) cfg.stateFile = L"tcycle.state.ini";
    cfg.stateFile = ResolvePathFromExe(exeDir, cfg.stateFile);
    cfg.debugForceCmdlineReadFail = ReadIniInt(iniPath, L"Debug", L"ForceCmdlineReadFail", 0) != 0;

    cfg.tclockIniPath = ReadIniString(iniPath, L"Integration", L"TClockIniPath", L"..\\tclock-win11.ini");
    if (cfg.tclockIniPath.empty()) cfg.tclockIniPath = L"..\\tclock-win11.ini";
    cfg.tclockIniPath = ResolvePathFromExe(exeDir, cfg.tclockIniPath);
    LoadTasks(iniPath, exeDir, cfg.tasks);

    outConfig = cfg;
    return true;
    } catch (const std::exception&) {
        outError = L"Failed to read ini; previous configuration was kept: " + iniPath;
        return false;
    }
}

bool IsTClockGateDisabled(const RuntimeConfig& config) {
    const DWORD attrs = GetFileAttributesW(config.tclockIniPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const int gate = static_cast<int>(GetPrivateProfileIntW(
        L"TCycle",
        L"Enabled",
        1,
        config.tclockIniPath.c_str()));
    return gate == 0;
}

} // namespace tcyc
