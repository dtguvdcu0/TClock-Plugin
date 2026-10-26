#pragma once
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct FLP_CONTEXT FLP_CONTEXT;
// The caller owns contexts on the rendering thread, outside DllMain.
FLP_CONTEXT* flp_create(HINSTANCE module, int theme);
FLP_CONTEXT* flp_create_nixie(HINSTANCE module);
BOOL flp_get_mode_size(int mode, int height, BOOL vertical, BOOL seconds, BOOL colon, SIZE* size);
int flp_get_mode_height(int mode, int heightDip, int cross, UINT dpi, BOOL vertical, BOOL stacked, BOOL seconds, BOOL colon);
void flp_destroy(FLP_CONTEXT* context);
void flp_reset(FLP_CONTEXT* context);
void flp_set_base(FLP_CONTEXT* context, BOOL show);
BOOL flp_get_size(int height, BOOL vertical, BOOL seconds, BOOL colon, SIZE* size);
int flp_get_height(int heightDip, int cross, UINT dpi, BOOL vertical, BOOL stacked, BOOL seconds, BOOL colon);
BOOL flp_render(FLP_CONTEXT* context, int height, BOOL vertical, BOOL seconds, BOOL colon,
    const SYSTEMTIME* time, ULONGLONG tick, BOOL animate, int duration);
BOOL flp_is_active(const FLP_CONTEXT* context);
const BYTE* flp_get_pixels(const FLP_CONTEXT* context, SIZE* size);
void flp_draw(const FLP_CONTEXT* context, HDC dc, int x, int y);
void flp_blend(const FLP_CONTEXT* context, RGBQUAD* pixels, int width, int height,
    int x, int y, const RECT* clip);
#ifdef __cplusplus
}
#endif
