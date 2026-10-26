#pragma once
#include <windows.h>
#include <stdint.h>
#include "analog_skin.h"
#include "led_clock.h"

// Pointer-free IPC. All rectangles are signed physical client pixels.
#define EXT_DETACH_DATA 0x54434431u
#define EXT_DETACH_VERSION 3u
#define EXT_DETACH_MAX 9u
#define EXT_DETACH_COMMIT (WM_APP + 128)
#define EXT_DETACH_ARM (WM_APP + 129)
#define EXT_DETACH_LAYOUT (WM_APP + 130)
#define EXT_DETACH_ACK 0x10000u
typedef struct EXT_DETACH_HOME {
    uint64_t target;
    uint64_t taskbar;
    uint64_t anchor;
    RECT bounds;
    RECT slot;
} EXT_DETACH_HOME;
typedef struct EXT_DETACH_PACKET {
    uint32_t version;
    uint32_t bytes;
    uint32_t count;
    uint32_t enabled;
    ACS_OPTIONS options;
    int32_t offsetMS;
    BOOL stacked, animate;
    SIZE baseSize;
    LED_SNAPSHOT text;
    EXT_DETACH_HOME homes[EXT_DETACH_MAX];
} EXT_DETACH_PACKET;
#ifdef __cplusplus
extern "C" {
#endif
void ext_init_host(HWND owner, HINSTANCE module);
BOOL ext_handle_message(HWND owner, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result);
void ext_close_hosts(void);
#ifdef __cplusplus
}
#endif
