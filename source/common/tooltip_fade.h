#pragma once
#include <windows.h>

#define TIP_FADE_DURATION 150
#define TIP_FADE_FRAME 15

typedef struct {
    ULONGLONG started;
    BOOL active, addedLayer;
} TIP_FADE_STATE;

static __inline void tip_stop_fade(TIP_FADE_STATE* state, HWND tip, HWND owner, UINT_PTR timer)
{
    if (owner) KillTimer(owner, timer);
    if (tip && state->active) {
        SetLayeredWindowAttributes(tip, 0, 255, LWA_ALPHA);
        if (state->addedLayer)
            SetWindowLongPtrW(tip, GWL_EXSTYLE, GetWindowLongPtrW(tip, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    state->active = state->addedLayer = FALSE;
}

// Prepare before activation so the first painted frame is already transparent.
static __inline void tip_begin_fade(TIP_FADE_STATE* state, HWND tip, HWND owner,
    UINT_PTR timer, BOOL enabled, ULONGLONG now)
{
    LONG_PTR style;
    tip_stop_fade(state, tip, owner, timer);
    if (!enabled || !tip || !owner) return;
    style = GetWindowLongPtrW(tip, GWL_EXSTYLE);
    state->addedLayer = !(style & WS_EX_LAYERED);
    state->active = TRUE;
    state->started = now;
    if (state->addedLayer) SetWindowLongPtrW(tip, GWL_EXSTYLE, style | WS_EX_LAYERED);
    if (!SetLayeredWindowAttributes(tip, 0, 0, LWA_ALPHA) || !SetTimer(owner, timer, TIP_FADE_FRAME, NULL))
        tip_stop_fade(state, tip, owner, timer);
}

static __inline void tip_step_fade(TIP_FADE_STATE* state, HWND tip, HWND owner,
    UINT_PTR timer, ULONGLONG now)
{
    ULONGLONG elapsed = now - state->started;
    if (!state->active || !tip || !IsWindowVisible(tip) || elapsed >= TIP_FADE_DURATION) {
        tip_stop_fade(state, tip, owner, timer);
        return;
    }
    SetLayeredWindowAttributes(tip, 0, (BYTE)(elapsed * 255 / TIP_FADE_DURATION), LWA_ALPHA);
}
