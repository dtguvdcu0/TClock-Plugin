#ifndef TCLOCK_RENDER_API_H
#define TCLOCK_RENDER_API_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif

/* tclock.render/1: a selected provider draws the clock text content as pixels. The
   host keeps the background, graphs, bar meters and the small analog clock, and
   composes the provider image over them. See render_api.md. */
#define TC_PLUGIN_RENDER_ID L"tclock.render/1"
#define TC_RENDER_MAX_WIDTH 1024u
#define TC_RENDER_MAX_HEIGHT 512u
#define TC_RENDER_TEXT_CCH 512u
#define TC_RENDER_MAX_REGIONS 8u
#define TC_RENDER_MAX_ZONE 3u

/* Request flags. */
#define TC_RENDER_VERTICAL 0x1u /* the taskbar is vertical */
#define TC_RENDER_WINUI    0x2u /* the WinUI display backend composes the image */

#pragma pack(push, 8)
typedef struct TC_RENDER_REQUEST {
    UINT size;
    UINT flags;
    UINT width;          /* pixels of the image to draw */
    UINT height;
    UINT dpi;
    COLORREF foreColor;  /* configured text color */
    COLORREF backColor;  /* configured background color */
    UINT reserved;
    ULONGLONG revision;
    SYSTEMTIME time;     /* local display time */
    WCHAR text[TC_RENDER_TEXT_CCH]; /* the clock text the built-in formatter (or the
                                       selected formatter) produced, lines split by CR LF */
} TC_RENDER_REQUEST;

/* A click inside bounds selects mouse zone `zone` (1-3) on the Mouse page. */
typedef struct TC_RENDER_REGION {
    RECT bounds;
    UINT zone;
    UINT reserved;
} TC_RENDER_REGION;

typedef struct TC_RENDER_OUTPUT {
    UINT size;
    UINT preferredWidth;  /* content size the clock should measure; 0 keeps the built-in size */
    UINT preferredHeight;
    UINT regionCount;
    TC_RENDER_REGION regions[TC_RENDER_MAX_REGIONS];
    /* Host-owned, zero-filled, width * height pixels of request: premultiplied BGRA
       (0xAARRGGBB with R, G, B <= A), top row first. Valid only during render. */
    UINT* pixels;
} TC_RENDER_OUTPUT;

/* render runs on the host UI thread and must return promptly. */
typedef struct TC_RENDER_PROVIDER {
    UINT size;
    HRESULT (WINAPI *render)(void* user, const TC_RENDER_REQUEST* request, TC_RENDER_OUTPUT* output);
} TC_RENDER_PROVIDER;
#pragma pack(pop)

C_ASSERT(sizeof(TC_RENDER_REQUEST) == 56 + 2 * TC_RENDER_TEXT_CCH);
C_ASSERT(sizeof(TC_RENDER_REGION) == 24);

#ifdef __cplusplus
}
#endif
#endif
