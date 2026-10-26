#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LED_TEXT_MAX 160
#define LED_FORMAT_MAX 512
#define LED_MESSAGE_MAX 3
// Stay below a 16ms request, which can span two default Windows timer quanta.
#define LED_FRAME_MS 15
typedef struct LED_ITEM { WCHAR text[LED_FORMAT_MAX+1]; int seconds, effect, color; } LED_ITEM;
typedef struct LED_OPTIONS {
    BOOL clock, seconds, colon, frame, date;
    WCHAR dateFormat[LED_FORMAT_MAX+1];
    int count, columns, speed, brightness, clockSeconds, clockColor;
    LED_ITEM messages[LED_MESSAGE_MAX];
} LED_OPTIONS;
typedef struct LED_SNAPSHOT {
    LED_OPTIONS options;
    SYSTEMTIME time;
    int columns;
    WCHAR clockText[LED_TEXT_MAX+1],expanded[LED_MESSAGE_MAX][LED_TEXT_MAX+1];
} LED_SNAPSHOT;
typedef struct LED_CONTEXT LED_CONTEXT;
BOOL led_take_snapshot(LED_CONTEXT* context,const SYSTEMTIME* time,LED_SNAPSHOT* snapshot);
void led_apply_snapshot(LED_CONTEXT* context,const LED_SNAPSHOT* snapshot);
BOOL led_render_snapshot(LED_CONTEXT* context,SIZE size,ULONGLONG tick,BOOL animate);
typedef LONG (*LED_READ_LONG)(const char*, const char*, LONG);
typedef int (*LED_READ_STRING)(const char*, const char*, char*, int, const char*);
typedef BOOL (*LED_WRITE_LONG)(const char*, const char*, DWORD);
typedef BOOL (*LED_WRITE_STRING)(const char*, const char*, const char*);
void led_load(LED_OPTIONS* options, LED_READ_LONG number, LED_READ_STRING text);
BOOL led_store(const LED_OPTIONS* options, LED_WRITE_LONG number, LED_WRITE_STRING text);
BOOL led_validate_text(const WCHAR* text);
BOOL WINAPI FormatDisplayTextW(const WCHAR* format, const SYSTEMTIME* time, WCHAR* output, int capacity);
BOOL led_expand_text(const WCHAR* format, const SYSTEMTIME* time, WCHAR* output);
int led_get_columns(LED_CONTEXT* context, const SYSTEMTIME* time);
LED_CONTEXT* led_create(void);
void led_destroy(LED_CONTEXT* context);
void led_reset(LED_CONTEXT* context);
void led_configure(LED_CONTEXT* context, const LED_OPTIONS* options);
BOOL led_get_size(int heightDip, int cross, UINT dpi, BOOL vertical, int columns, SIZE* size);
BOOL led_expand_clock(const LED_OPTIONS* options, const SYSTEMTIME* time, WCHAR* output);
BOOL led_render_preview(LED_CONTEXT* context, SIZE size, const SYSTEMTIME* time, ULONGLONG tick, int target, BOOL animate);
BOOL led_render(LED_CONTEXT* context, SIZE size, const SYSTEMTIME* time, ULONGLONG tick, BOOL animate);
BOOL led_is_active(const LED_CONTEXT* context);
const BYTE* led_get_pixels(const LED_CONTEXT* context);
void led_draw(const LED_CONTEXT* context, HDC dc, int x, int y);
void led_blend(const LED_CONTEXT* context, RGBQUAD* pixels, int width, int height, int x, int y, const RECT* clip);
#ifdef __cplusplus
}
#endif
