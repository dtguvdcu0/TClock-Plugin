#pragma once
#include <windows.h>

#define TIP_HOVER_MOVE_MS 300
#define TIP_HOVER_LEAVE_MS 320
#define TIP_HOVER_DISTANCE 28
#define TIP_HOVER_STILL 6
#define TIP_HOVER_MARGIN 4

static __inline BOOL tip_test_move(POINT point, POINT origin, int distance)
{
    LONGLONG dx = (LONGLONG)point.x - origin.x;
    LONGLONG dy = (LONGLONG)point.y - origin.y;
    return dx * dx + dy * dy > (LONGLONG)distance * distance;
}

static __inline int tip_pick_slot(const RECT* target, POINT point, BOOL vertical,
    int slots, int previous, int margin)
{
    int length = vertical ? target->bottom - target->top : target->right - target->left;
    int position = vertical ? point.y - target->top : point.x - target->left;
    int slot;
    if (slots < 2 || slots > 3 || length <= 0) return 0;
    if (slots == 3) slot = position < length / 3 ? 0 : position <= 2 * length / 3 ? 1 : 2;
    else slot = position <= length / 2 ? 0 : 1;
    if (previous >= 0 && previous < slots && slot != previous) {
        int boundary = slot > previous ? (previous + 1) * length / slots : previous * length / slots;
        margin = min(margin, max(1, length / slots / 4));
        if (slot > previous ? position <= boundary + margin : position >= boundary - margin) return previous;
    }
    return slot;
}


// Both presenters use this state machine; only window activation is backend-specific.
typedef struct {
    BOOL inside, suppressed, pending, visible, moving, hasPoint;
    POINT point, still;
    int slot;
    UINT moveDelay, autoDelay;
    ULONGLONG due, leaveDue, armed;
} TIP_HOVER_STATE;

typedef enum { TIP_HOVER_IDLE, TIP_HOVER_WAIT, TIP_HOVER_SHOW, TIP_HOVER_HIDE } TIP_HOVER_ACTION;

static __inline void tip_hide_hover(TIP_HOVER_STATE* state)
{
    state->pending = state->visible = state->moving = state->hasPoint = FALSE;
    state->due = state->leaveDue = 0;
}

static __inline void tip_config_hover(TIP_HOVER_STATE* state, UINT initial, UINT reshow, UINT autoPop)
{
    state->moveDelay = max((UINT)TIP_HOVER_MOVE_MS, max(initial, reshow));
    state->autoDelay = autoPop;
}

static __inline void tip_enter_hover(TIP_HOVER_STATE* state)
{
    state->inside = TRUE;
    state->leaveDue = 0;
}

static __inline void tip_leave_hover(TIP_HOVER_STATE* state, ULONGLONG now)
{
    state->inside = state->suppressed = FALSE;
    if (state->visible) state->leaveDue = now + TIP_HOVER_LEAVE_MS;
    else tip_hide_hover(state);
}

static __inline void tip_begin_hover(TIP_HOVER_STATE* state, ULONGLONG now, UINT initial)
{
    if (state->pending || state->visible || state->suppressed) return;
    state->pending = TRUE;
    state->due = now + initial;
}

// TRUE retains the visible snapshot; small motion must not postpone a pending deadline.
static __inline BOOL tip_move_hover(TIP_HOVER_STATE* state, POINT point, int slot,
    int distance, int still, ULONGLONG now)
{
    if (state->visible) {
        if (!tip_test_move(point, state->point, distance) && slot == state->slot) return TRUE;
        state->visible = FALSE;
        state->pending = state->moving = TRUE;
        state->still = point;
        state->due = now + state->moveDelay;
    } else if (state->moving && tip_test_move(point, state->still, still)) {
        state->still = point;
        state->due = now + state->moveDelay;
    }
    state->point = point;
    state->hasPoint = TRUE;
    return FALSE;
}

static __inline void tip_show_hover(TIP_HOVER_STATE* state, int slot, ULONGLONG now)
{
    state->pending = state->moving = FALSE;
    state->visible = TRUE;
    state->slot = slot;
    state->due = state->autoDelay ? now + state->autoDelay : 0;
}

static __inline void tip_refresh_hover(TIP_HOVER_STATE* state, ULONGLONG now)
{
    if (state->visible) state->due = state->autoDelay ? now + state->autoDelay : 0;
}

static __inline ULONGLONG tip_due_hover(const TIP_HOVER_STATE* state)
{
    if (!state->inside) return state->visible ? state->leaveDue : 0;
    return state->suppressed ? 0 : state->due;
}

static __inline TIP_HOVER_ACTION tip_poll_hover(TIP_HOVER_STATE* state,
    POINT point, BOOL valid, int still, ULONGLONG now)
{
    ULONGLONG due = tip_due_hover(state);
    if (!due) return TIP_HOVER_IDLE;
    if (!state->inside) {
        if (now < due) return TIP_HOVER_WAIT;
        tip_hide_hover(state);
        return TIP_HOVER_HIDE;
    }
    if (state->pending) {
        if (!valid) {
            tip_hide_hover(state);
            return TIP_HOVER_HIDE;
        }
        if (state->moving && tip_test_move(point, state->still, still)) {
            state->still = point;
            state->due = now + state->moveDelay;
        }
        state->point = point;
        state->hasPoint = TRUE;
        return now < state->due ? TIP_HOVER_WAIT : TIP_HOVER_SHOW;
    }
    if (now < due) return TIP_HOVER_WAIT;
    tip_hide_hover(state);
    state->suppressed = TRUE;
    return TIP_HOVER_HIDE;
}

// Reuse an armed deadline: repeated WM_MOUSEMOVE must not starve WM_TIMER.
static __inline BOOL tip_schedule_hover(TIP_HOVER_STATE* state, HWND owner,
    UINT_PTR timer, ULONGLONG now)
{
    ULONGLONG due = tip_due_hover(state);
    if (state->armed == due) return TRUE;
    KillTimer(owner, timer);
    state->armed = 0;
    if (!due) return TRUE;
    if (!SetTimer(owner, timer, due > now ? (UINT)(due - now) : 1, NULL)) return FALSE;
    state->armed = due;
    return TRUE;
}
