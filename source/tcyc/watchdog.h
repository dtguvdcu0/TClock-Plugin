#pragma once

#include "config.h"
#include "state.h"

#include <string>
#include <vector>

namespace tcyc {

struct WatchdogDue {
    int taskId = 0;
    std::wstring reason;
};

std::vector<WatchdogDue> EvaluateWatchdogDue(const RuntimeConfig& cfg,
                                             RuntimeState& state,
                                             long long nowUnix,
                                             const std::vector<const TaskConfig*>* taskLane = nullptr);
void MarkWatchdogLaunched(RuntimeState& state, int taskId, long long nextRetryUnix);

} // namespace tcyc

