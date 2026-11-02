#ifndef TCLOCK_CHIME_API_H
#define TCLOCK_CHIME_API_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif

/* tclock.chime/1: a selected provider decides when the clock chimes. While its plan
   is current the built-in chime decision (primary, secondary and blink) is suppressed;
   the host still plays the configured sounds and blinks. See chime_api.md. */
#define TC_PLUGIN_CHIME_ID L"tclock.chime/1"
#define TC_CHIME_MAX_EVENTS 32
#define TC_CHIME_MAX_STRIKES 24

/* Event effects. PRIMARY plays the primary sound `strikes` times, one per second. */
#define TC_CHIME_PRIMARY   0x1u
#define TC_CHIME_SECONDARY 0x2u
#define TC_CHIME_BLINK     0x4u
#define TC_CHIME_EFFECTS   0x7u

/* Reported exactly once for every event the host accepted. */
#define TC_CHIME_COMPLETED 1u /* all effects were issued */
#define TC_CHIME_UNKNOWN   2u /* started, completion not observed; never replayed */
#define TC_CHIME_SKIPPED   3u /* not started: time missed or chime paused */
#define TC_CHIME_CANCELLED 4u /* not started: replaced, owner changed, disabled or shut down */

#pragma pack(push, 8)
/* Local wall-clock time with second resolution (wMilliseconds is ignored). */
typedef struct TC_CHIME_EVENT {
    ULONGLONG id;
    SYSTEMTIME time;
    UINT effects;
    UINT strikes;
} TC_CHIME_EVENT;

typedef struct TC_CHIME_REQUEST {
    UINT size;
    UINT windowSeconds;
    SYSTEMTIME from;
} TC_CHIME_REQUEST;

typedef struct TC_CHIME_PLAN {
    UINT size;
    UINT count;
    TC_CHIME_EVENT events[TC_CHIME_MAX_EVENTS];
} TC_CHIME_PLAN;

/* Both callbacks run on the host UI thread and must return promptly.
   plan: fill output (size preset by the host) with the events in [from, from + window).
   report: outcome of an accepted event; the id is the provider's own. */
typedef struct TC_CHIME_PROVIDER {
    UINT size;
    HRESULT (WINAPI *plan)(void* user, const TC_CHIME_REQUEST* request, TC_CHIME_PLAN* output);
    void (WINAPI *report)(void* user, ULONGLONG id, UINT outcome);
} TC_CHIME_PROVIDER;
#pragma pack(pop)

C_ASSERT(sizeof(TC_CHIME_EVENT) == 32);
C_ASSERT(sizeof(TC_CHIME_REQUEST) == 24);
C_ASSERT(sizeof(TC_CHIME_PLAN) == 8 + 32 * TC_CHIME_MAX_EVENTS);

#ifdef __cplusplus
}
#endif
#endif
