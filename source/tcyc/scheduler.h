#pragma once

#include "config.h"
#include "state.h"

#include <string>
#include <vector>

namespace tcyc {

struct DueTask {
    int taskId = 0;
    std::wstring reason;
};

std::vector<DueTask> EvaluateDueTasks(const RuntimeConfig& cfg,
                                      RuntimeState& state,
                                      long long nowUnix,
                                      const std::vector<const TaskConfig*>* taskLane = nullptr);
void ResetSchedulerCaches();
void MarkTaskFired(RuntimeState& state, int taskId, long long fireUnix);
void MarkTaskObservedRunning(RuntimeState& state, int taskId, long long nowUnix, bool consumeCount);

} // namespace tcyc
