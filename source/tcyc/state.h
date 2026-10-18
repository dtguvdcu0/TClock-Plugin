#pragma once

#include <string>
#include <unordered_map>

namespace tcyc {

struct TaskRuntimeState {
    int firedCount = 0;
    long long lastCheckUnix = 0;
    long long lastFireUnix = 0;
    bool startupDone = false;
    int watchdogRetryCount = 0;
    long long watchdogNextRetryUnix = 0;
};

struct RuntimeState {
    int bootCount = 0;
    long long lastEvalUnix = 0;
    long long lastSaveUnix = 0;
    std::unordered_map<int, TaskRuntimeState> tasks;
};

bool LoadRuntimeState(const std::wstring& statePath, RuntimeState& outState);
bool SaveRuntimeState(const std::wstring& statePath, const RuntimeState& state);
long long UnixNow();

} // namespace tcyc
