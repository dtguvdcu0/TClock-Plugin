#pragma once

#include <string>
#include <vector>

namespace tcyc {

enum class TriggerType {
    Interval,
    DateTimeIntervalLimited,
    WeeklyTime,
    Startup,
    HotkeyOnly,
    NonRunning,
    Unknown
};

struct TaskConfig {
    int id = 0;
    bool enabled = false;
    std::wstring name;
    TriggerType trigger = TriggerType::Unknown;
    int triggerMask = 0; // bit0=interval bit1=datetime_interval_limited bit2=weekly_time bit3=startup bit4=hotkey_only bit5=non_running
    std::wstring actionPath;
    std::wstring actionArgs;
    std::wstring actionCwd;
    std::wstring actionMode = L"program"; // program | command | shell
    bool singleInstance = true;
    bool watchdogEnabled = false;
    int watchdogRetrySec = 10;
    int watchdogMaxRetry = -1;
    bool watchdogRequireArgsMatch = true;
    int intervalSec = 0;
    std::wstring startDateTime;
    bool startDateTimeValid = false;
    long long startDateTimeUnix = 0;
    int repeatEverySec = 0;
    int repeatCount = 0;
    bool weeklyEveryday = false;
    int weekday = -1; // 0=Sun ... 6=Sat
    int weekdayMask = 0; // bit0=Sun ... bit6=Sat
    bool dateEnabled = false;
    std::wstring dateYmd;
    bool weekdayEnabled = true;
    bool timeEnabled = true;
    int timeOfDaySec = -1;
    std::wstring hotkey;
};

struct RuntimeConfig {
    int pollSec = 1;
    int graceSec = 60;
    int logLevel = 1;
    std::wstring logFile;
    bool stateEnabled = false;
    std::wstring stateFile;
    bool debugForceCmdlineReadFail = false; // Test-only: force args-read fallback mode when matching by path.
    std::wstring tclockIniPath;
    std::vector<TaskConfig> tasks;
};

std::wstring GetExeDirectory();
std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf);
bool IsAbsolutePath(const std::wstring& path);
std::wstring ResolvePathFromExe(const std::wstring& exeDir, const std::wstring& path);

bool EnsureDefaultIni(const std::wstring& iniPath);
bool LoadRuntimeConfig(const std::wstring& iniPath, const std::wstring& exeDir, RuntimeConfig& outConfig, std::wstring& outError);
bool IsTClockGateDisabled(const RuntimeConfig& config);

} // namespace tcyc
