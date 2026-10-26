#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ACS_CONTEXT ACS_CONTEXT;
enum { EXT_MODE_NORMAL = 0, EXT_MODE_CLASSIC = 1, EXT_MODE_FLIP = 2, EXT_MODE_LEGACY = 3, EXT_MODE_NIXIE = 4, EXT_MODE_LED = 5 };
typedef struct ACS_OPTIONS {
    BOOL enabled;
    int face;
    int mode;
    int diameter;
    BOOL trailing;
    int offsetX;
    int offsetY;
    BOOL seconds;
    BOOL colon;
    BOOL nixieBase;
    BOOL flipStacked;
    int flipDuration;
} ACS_OPTIONS;
#define ACS_VALIDATE (WM_APP + 112)
ACS_CONTEXT* acs_create(HINSTANCE module, int mode, int face);
void acs_destroy(ACS_CONTEXT* context);
BOOL acs_render(ACS_CONTEXT* context, int diameter, const SYSTEMTIME* time, BOOL seconds);
const BYTE* acs_get_pixels(const ACS_CONTEXT* context);
BOOL acs_draw(ACS_CONTEXT* context, HDC dc, int x, int y, int diameter,
    const SYSTEMTIME* time, BOOL seconds);
void acs_blend(ACS_CONTEXT* context, RGBQUAD* pixels, int width, int height,
    int x, int y, const RECT* clip);
int acs_get_diameter(int diameterDip, int cross, UINT dpi);
UINT acs_get_dpi(HWND window);
#ifdef __cplusplus
}
#endif
