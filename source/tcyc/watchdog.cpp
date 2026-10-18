#include "watchdog.h"
#include "log.h"
#include "runner.h"

namespace tcyc {

namespace {
bool HasNonRunningTrigger(const TaskConfig& t) {
    if (t.triggerMask != 0) return (t.triggerMask & (1 << 5)) != 0;
    return t.trigger == TriggerType::NonRunning;
}
}

std::vector<WatchdogDue> EvaluateWatchdogDue(const RuntimeConfig& cfg,
                                             RuntimeState& state,
                                             long long nowUnix,
                                             const std::vector<const TaskConfig*>* taskLane) {
    std::vector<WatchdogDue> out;
    auto evalTask = [&](const TaskConfig& t) {
        if (!t.enabled || !t.watchdogEnabled) return;
        if (!HasNonRunningTrigger(t)) return;
        if (t.actionPath.empty()) return;

        TaskRuntimeState& st = state.tasks[t.id];
        ProcessMatchMode mode = ProcessMatchMode::None;
        if (IsTaskProcessRunning(t, &mode, cfg.debugForceCmdlineReadFail)) {
            if (mode == ProcessMatchMode::PathOnlyFallbackArgsUnavailable) {
                LogWrite(1, L"Watchdog process match fallback: taskId=%d mode=path_only_fallback_args_unavailable", t.id);
            }
            st.watchdogRetryCount = 0;
            st.watchdogNextRetryUnix = 0;
            return;
        }

        if (st.watchdogNextRetryUnix == 0) {
            st.watchdogNextRetryUnix = nowUnix + t.watchdogRetrySec;
            return;
        }
        if (nowUnix < st.watchdogNextRetryUnix) return;

        if (t.watchdogMaxRetry >= 0 && st.watchdogRetryCount >= t.watchdogMaxRetry) {
            return;
        }
        out.push_back({t.id, L"watchdog_retry"});
    };

    if (taskLane) {
        for (const TaskConfig* tp : *taskLane) {
            if (!tp) continue;
            evalTask(*tp);
        }
    } else {
        for (const auto& t : cfg.tasks) {
            evalTask(t);
        }
    }
    return out;
}

void MarkWatchdogLaunched(RuntimeState& state, int taskId, long long nextRetryUnix) {
    TaskRuntimeState& st = state.tasks[taskId];
    st.watchdogRetryCount += 1;
    st.watchdogNextRetryUnix = nextRetryUnix;
}

} // namespace tcyc
