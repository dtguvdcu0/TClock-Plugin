#pragma once
#include "led_clock.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct LED_SURFACE LED_SURFACE;
typedef struct LED_SURFACE_STATE {
    HWND target,taskbar,host;
    RECT bounds,clip;
    SIZE size;
    BOOL visible;
    LED_SNAPSHOT text;
} LED_SURFACE_STATE;
LED_SURFACE* led_start_surface(HINSTANCE instance,const LED_SURFACE_STATE* state);
BOOL led_publish_surface(LED_SURFACE* surface,const LED_SURFACE_STATE* state);
void led_stop_surface(LED_SURFACE* surface);
#ifdef __cplusplus
}
#endif
