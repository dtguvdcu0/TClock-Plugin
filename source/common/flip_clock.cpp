#define NOMINMAX
#include "flip_clock.h"
#include "analog_skin.h"
#include "skin_manifest.h"
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <string>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;
#include "nixie_draw.h"

struct FLP_CONTEXT {
    ULONG_PTR token = 0;
    Bitmap* tiles[13] = {};
    bool nixie = false;
    BOOL showBase = TRUE;
    NIX_STYLE nixieStyle = {};
    std::vector<BYTE> pixels;
    SIZE size = {};
    int height = 0;
    BOOL vertical = FALSE, seconds = FALSE, colon = FALSE;
    bool ready = false, active = false;
    int previous[6] = {}, target[6] = {};
    ULONGLONG sample = 0, lastTick = 0, start = 0;
};

static void flp_make_outline(GraphicsPath& path, REAL x, REAL y, REAL width, REAL height, REAL radius)
{
    REAL edge = radius * 2;
    path.AddArc(x, y, edge, edge, 180, 90);
    path.AddArc(x+width-edge, y, edge, edge, 270, 90);
    path.AddArc(x+width-edge, y+height-edge, edge, edge, 0, 90);
    path.AddArc(x, y+height-edge, edge, edge, 90, 90);
    path.CloseFigure();
}

struct FLP_STYLE { Color face, border, text, seam; };

static bool flp_paint_frame(Graphics& graphics, int height, const FLP_STYLE& style)
{
    GraphicsPath outline;
    flp_make_outline(outline, 12, 3, 150, (REAL)height-6, 9);
    SolidBrush face(style.face);
    Pen frame(style.border, 1.5f);
    return graphics.FillPath(&face, &outline) == Ok && graphics.DrawPath(&frame, &outline) == Ok;
}

static Bitmap* flp_make_tile(const FLP_STYLE& style, int digit)
{
    std::unique_ptr<Bitmap> tile(new Bitmap(174, 226, PixelFormat32bppPARGB));
    if (tile->GetLastStatus() != Ok) return nullptr;
    Graphics graphics(tile.get());
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    if (!flp_paint_frame(graphics, 226, style)) return nullptr;
    SolidBrush ink(style.text);
    if (digit < 10) {
        FontFamily family(L"Arial");
        GraphicsPath reference, glyph;
        StringFormat format(StringFormat::GenericTypographic());
        WCHAR text[] = {(WCHAR)(L'0'+digit), 0};
        if (reference.AddString(L"0", 1, &family, FontStyleBold, 180, PointF(0, 0), &format) != Ok ||
            glyph.AddString(text, 1, &family, FontStyleBold, 180, PointF(0, 0), &format) != Ok) return nullptr;
        RectF bounds, referenceBounds;
        reference.GetBounds(&referenceBounds); glyph.GetBounds(&bounds);
        if (referenceBounds.Width <= 0 || referenceBounds.Height <= 0) return nullptr;
        // Use one scale for every digit; center the real ink, including the narrow 1.
        REAL sx = 104/referenceBounds.Width, sy = 158/referenceBounds.Height;
        Matrix transform(sx, 0, 0, sy, 87-(bounds.X+bounds.Width/2)*sx, 113-(bounds.Y+bounds.Height/2)*sy);
        if (glyph.Transform(&transform) != Ok || graphics.FillPath(&ink, &glyph) != Ok) return nullptr;
    } else {
        if (graphics.FillEllipse(&ink, 76, 69, 22, 22) != Ok ||
            graphics.FillEllipse(&ink, 76, 135, 22, 22) != Ok) return nullptr;
    }
    Pen seam(style.seam, 1.5f);
    if (graphics.DrawLine(&seam, 13.0f, 113.0f, 161.0f, 113.0f) != Ok) return nullptr;
    graphics.Flush(FlushIntentionSync);
    return tile.release();
}

static Bitmap* flp_make_dot(const FLP_STYLE& style)
{
    std::unique_ptr<Bitmap> tile(new Bitmap(174, 82, PixelFormat32bppPARGB));
    if (tile->GetLastStatus() != Ok) return nullptr;
    Graphics graphics(tile.get());
    graphics.Clear(Color(0, 0, 0, 0));
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    if (!flp_paint_frame(graphics, 82, style)) return nullptr;
    SolidBrush dot(style.text);
    if (graphics.FillEllipse(&dot, 76, 30, 22, 22) != Ok) return nullptr;
    graphics.Flush(FlushIntentionSync);
    return tile.release();
}

static FLP_CONTEXT* flp_create_clock(HINSTANCE module, int theme, bool nixie)
{
    if (theme < 0 || theme > 1) return nullptr;
    WCHAR path[32768];
    DWORD length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path)) return nullptr;
    WCHAR* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    slash[1] = 0;
    FLP_CONTEXT* context = new(std::nothrow) FLP_CONTEXT;
    if (!context) return nullptr;
    context->nixie = nixie;
    GdiplusStartupInput input;
    if (GdiplusStartup(&context->token, &input, nullptr) != Ok) {
        delete context; return nullptr;
    }
    bool valid = false;
    try {
        std::wstring root = std::wstring(path) + (nixie ? L"clock-skins\\nixie\\" : L"clock-skins\\flip\\");
        SKN_MANIFEST manifest;
        valid = manifest.load((root + L"skin.ini").c_str()) &&
            manifest.matches(L"Skin", L"Version", L"2") && manifest.matches(L"Skin", L"Renderer", L"Native");
        if (nixie) {
            NIX_STYLE style;
            Color* colors[] = {&style.glass, &style.mesh, &style.base, &style.glow, &style.wire, &style.core};
            const WCHAR* keys[] = {L"GlassColor", L"MeshColor", L"BaseColor", L"GlowColor", L"WireColor", L"CoreColor"};
            for (int i = 0; valid && i < 6; ++i) {
                unsigned long rgb = 0;
                valid = skn_read_color(manifest, L"Amber", keys[i], rgb);
                *colors[i] = Color(255, (BYTE)(rgb>>16), (BYTE)(rgb>>8), (BYTE)rgb);
            }
            context->nixieStyle = style;
            for (int i = 0; valid && i < 13; ++i) {
                context->tiles[i] = nix_make_tile(style, i == 12 ? -1 : i);
                valid = context->tiles[i] != nullptr;
            }
        } else {
            FLP_STYLE style;
            Color* colors[] = {&style.face, &style.border, &style.text, &style.seam};
            const WCHAR* keys[] = {L"FaceColor", L"BorderColor", L"TextColor", L"SeamColor"};
            for (int i = 0; valid && i < 4; ++i) {
                unsigned long rgb = 0;
                valid = skn_read_color(manifest, theme ? L"Black" : L"White", keys[i], rgb);
                *colors[i] = Color(255, (BYTE)(rgb>>16), (BYTE)(rgb>>8), (BYTE)rgb);
            }
            for (int i = 0; valid && i < 11; ++i) {
                context->tiles[i] = flp_make_tile(style, i);
                valid = context->tiles[i] != nullptr;
            }
            if (valid) { context->tiles[11] = flp_make_dot(style); valid = context->tiles[11] != nullptr; }
        }
    } catch (...) { valid = false; }
    if (!valid) { flp_destroy(context); return nullptr; }
    return context;
}

FLP_CONTEXT* flp_create(HINSTANCE module, int theme)
{
    return flp_create_clock(module, theme, false);
}

FLP_CONTEXT* flp_create_nixie(HINSTANCE module)
{
    return flp_create_clock(module, 0, true);
}

void flp_destroy(FLP_CONTEXT* context)
{
    if (!context) return;
    for (Bitmap* tile : context->tiles) delete tile;
    if (context->token) GdiplusShutdown(context->token);
    delete context;
}

void flp_reset(FLP_CONTEXT* context)
{
    if (context) { context->ready = false; context->active = false; }
}

void flp_set_base(FLP_CONTEXT* context, BOOL show)
{
    if (context && context->nixie && context->showBase != (show != FALSE)) {
        context->showBase = show != FALSE;
        flp_reset(context);
    }
}

BOOL flp_get_mode_size(int mode, int height, BOOL vertical, BOOL seconds, BOOL colon, SIZE* size)
{
    if (!size || height < 1 || height > 1024 || (mode != EXT_MODE_FLIP && mode != EXT_MODE_NIXIE)) return FALSE;
    bool nixie = mode == EXT_MODE_NIXIE;
    int width = std::max(1, nixie ? MulDiv(height, 74, 142) : MulDiv(height, 174, 226));
    int gap = std::max(1, nixie ? MulDiv(height, 5, 142) : MulDiv(height, 55, 1000));
    int separator = std::max(1, nixie ? MulDiv(height, vertical ? 54 : 32, 142) : MulDiv(height, 36, 100));
    int rowGap = nixie ? std::max(1, MulDiv(height, 8, 142)) : gap;
    int groups = seconds ? 3 : 2;
    size->cx = vertical ? 2*width+gap :
        groups*2*width+(groups*2-1)*gap+(colon ? (groups-1)*(separator+gap) : 0);
    size->cy = vertical ? groups*height+(groups-1)*(colon ? separator+2*rowGap : rowGap) : height;
    return TRUE;
}

int flp_get_mode_height(int mode, int heightDip, int cross, UINT dpi, BOOL vertical, BOOL stacked, BOOL seconds, BOOL colon)
{
    if (!dpi || heightDip < 0 || heightDip > 256 || (heightDip && heightDip < 16)) return 0;
    bool automaticNixie = mode == EXT_MODE_NIXIE && !heightDip;
    int margin = std::max(1, MulDiv(automaticNixie ? 4 : 1, dpi, 96));
    if (automaticNixie) margin = std::min(margin, std::max(1, cross/8));
    int available = cross - margin*2;
    if (available < 1) return 0;
    // Keep the automatic stacked clock near the approved mock; explicit sizes use the full width.
    if (automaticNixie && vertical && stacked) available = std::max(1, MulDiv(available, 60, 100));
    int high = std::min(1024, heightDip ? MulDiv(heightDip, dpi, 96) : 1024);
    int low = 0;
    while (low < high) {
        int candidate = (low+high+1)/2;
        SIZE size;
        if (!flp_get_mode_size(mode, candidate, vertical && stacked, seconds, colon, &size)) return 0;
        if ((vertical ? size.cx : size.cy) <= available) low = candidate;
        else high = candidate-1;
    }
    return low;
}

BOOL flp_get_size(int height, BOOL vertical, BOOL seconds, BOOL colon, SIZE* size)
{
    return flp_get_mode_size(EXT_MODE_FLIP, height, vertical, seconds, colon, size);
}

int flp_get_height(int heightDip, int cross, UINT dpi, BOOL vertical, BOOL stacked, BOOL seconds, BOOL colon)
{
    return flp_get_mode_height(EXT_MODE_FLIP, heightDip, cross, dpi, vertical, stacked, seconds, colon);
}

static Status flp_draw_half(Graphics& graphics, Bitmap* tile, REAL x, REAL y,
    REAL width, REAL height, bool top, REAL compression)
{
    if (compression < .001f) return Ok;
    REAL half = height/2, extent = half*compression;
    return graphics.DrawImage(tile, RectF(x, top ? y+half-extent : y+half, width, extent),
        0.0f, top ? 0.0f : 113.0f, 174.0f, 113.0f, UnitPixel);
}

static bool flp_draw_digit(Graphics& graphics, FLP_CONTEXT* context, int oldDigit,
    int newDigit, REAL x, REAL y, int width, int height, REAL progress)
{
    Bitmap* before = context->tiles[oldDigit];
    Bitmap* after = context->tiles[newDigit];
    if (context->nixie) {
        if (graphics.DrawImage(context->tiles[12], x, y, (REAL)width, (REAL)height) != Ok) return false;
        if (oldDigit == newDigit) return nix_draw_alpha(graphics, after, x, y, width, height, 1);
        return nix_draw_alpha(graphics, before, x, y, width, height, 1-progress) &&
            nix_draw_alpha(graphics, after, x, y, width, height, progress);
    }
    if (progress <= 0 || oldDigit == newDigit || progress >= 1)
        return graphics.DrawImage(progress <= 0 ? before : after, x, y, (REAL)width, (REAL)height) == Ok;
    if (flp_draw_half(graphics, after, x, y, (REAL)width, (REAL)height, true, 1) != Ok ||
        flp_draw_half(graphics, before, x, y, (REAL)width, (REAL)height, false, 1) != Ok) return false;
    bool top = progress < .5f;
    REAL compression = top ? (REAL)std::cos(progress*3.141592653589793) :
        (REAL)std::sin((progress-.5f)*3.141592653589793);
    if (flp_draw_half(graphics, top ? before : after, x, y,
        (REAL)width, (REAL)height, top, compression) != Ok) return false;
    REAL extent = height/2.0f*compression;
    SolidBrush shade(Color((BYTE)(96*(top ? std::sin(progress*3.141592653589793) : 1-compression)), 0, 0, 0));
    graphics.FillRectangle(&shade, x+width*12.0f/174, top ? y+height/2.0f-extent : y+height/2.0f,
        width*150.0f/174, extent);
    SolidBrush seam(Color(140, 0, 0, 0));
    graphics.FillRectangle(&seam, x+width*12.0f/174, y+height/2.0f-.4f, width*150.0f/174, .8f);
    return true;
}

BOOL flp_render(FLP_CONTEXT* context, int height, BOOL vertical, BOOL seconds, BOOL colon,
    const SYSTEMTIME* time, ULONGLONG tick, BOOL animate, int duration)
{
    SIZE size;
    FILETIME stamp;
    if (!context || !time || duration < 100 || duration > 900 || !flp_get_mode_size(context->nixie ? EXT_MODE_NIXIE : EXT_MODE_FLIP, height, vertical, seconds, colon, &size) ||
        !SystemTimeToFileTime(time, &stamp)) return FALSE;
    try {
        if (context->height != height || context->vertical != vertical ||
            context->seconds != seconds || context->colon != colon) {
            std::vector<BYTE> pixels((size_t)size.cx*size.cy*4);
            context->pixels.swap(pixels);
            context->size = size; context->height = height;
            context->vertical = vertical; context->seconds = seconds; context->colon = colon;
            flp_reset(context);
        }
        ULARGE_INTEGER value;
        value.LowPart = stamp.dwLowDateTime; value.HighPart = stamp.dwHighDateTime;
        ULONGLONG sample = value.QuadPart/(seconds ? 10000000ULL : 600000000ULL);
        int digits[] = {time->wHour/10, time->wHour%10, time->wMinute/10,
            time->wMinute%10, time->wSecond/10, time->wSecond%10};
        if (!context->ready || sample != context->sample || tick < context->lastTick) {
            bool normal = context->ready && sample == context->sample+1 && tick >= context->lastTick &&
                tick-context->lastTick <= (seconds ? 2000ULL : 62000ULL);
            std::copy(context->target, context->target+6, context->previous);
            std::copy(digits, digits+6, context->target);
            context->active = animate && normal;
            context->start = tick; context->ready = true; context->sample = sample;
        }
        context->lastTick = tick;
        if (!animate || tick-context->start >= (ULONGLONG)duration) context->active = false;
        REAL progress = context->active ? (REAL)(tick-context->start)/(REAL)duration : 1.0f;
        std::fill(context->pixels.begin(), context->pixels.end(), (BYTE)0);
        Bitmap target(size.cx, size.cy, size.cx*4, PixelFormat32bppPARGB, context->pixels.data());
        Graphics graphics(&target);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        int width = std::max(1, context->nixie ? MulDiv(height, 74, 142) : MulDiv(height, 174, 226));
        int gap = std::max(1, context->nixie ? MulDiv(height, 5, 142) : MulDiv(height, 55, 1000));
        int separator = std::max(1, context->nixie ? MulDiv(height, vertical ? 54 : 32, 142) : MulDiv(height, 36, 100));
        int rowGap = context->nixie ? std::max(1, MulDiv(height, 8, 142)) : gap;
        int groups = seconds ? 3 : 2, x = 0;
        for (int group = 0; group < groups; ++group) {
            int y = vertical ? group*(height+(colon ? separator+2*rowGap : rowGap)) : 0;
            if (vertical) x = 0;
            for (int digit = 0; digit < 2; ++digit) {
                int index = group*2+digit;
                if (!flp_draw_digit(graphics, context, context->previous[index], context->target[index],
                    (REAL)x, (REAL)y, width, height, progress)) return FALSE;
                x += width+gap;
            }
            if (context->nixie && context->showBase && vertical)
                nix_paint_base(graphics, context->nixieStyle, 0, (REAL)y, (REAL)(2*width+gap), (REAL)height/142, false);
            if (colon && group < groups-1) {
                if (vertical) {
                    for (int column = 0; column < 2; ++column)
                        if (graphics.DrawImage(context->tiles[11], column*(width+gap),
                            y+height+rowGap, width, separator) != Ok) return FALSE;
                    if (context->nixie && context->showBase)
                        nix_paint_base(graphics, context->nixieStyle, 0, (REAL)(y+height+rowGap),
                            (REAL)(2*width+gap), (REAL)height/142, true);
                } else {
                    if (graphics.DrawImage(context->tiles[10], x, y, separator, height) != Ok) return FALSE;
                    x += separator+gap;
                }
            }
        }
        if (context->nixie && context->showBase && !vertical)
            nix_paint_base(graphics, context->nixieStyle, 0, 0, (REAL)size.cx, (REAL)height/142, false);
        graphics.Flush(FlushIntentionSync);
        return TRUE;
    } catch (...) { return FALSE; }
}

BOOL flp_is_active(const FLP_CONTEXT* context)
{
    return context && context->active;
}

const BYTE* flp_get_pixels(const FLP_CONTEXT* context, SIZE* size)
{
    if (!context || context->pixels.empty()) return nullptr;
    if (size) *size = context->size;
    return context->pixels.data();
}

void flp_draw(const FLP_CONTEXT* context, HDC dc, int x, int y)
{
    if (!context || context->pixels.empty() || !dc) return;
    Bitmap image(context->size.cx, context->size.cy, context->size.cx*4,
        PixelFormat32bppPARGB, const_cast<BYTE*>(context->pixels.data()));
    Graphics graphics(dc);
    graphics.DrawImage(&image, x, y);
}

void flp_blend(const FLP_CONTEXT* context, RGBQUAD* pixels, int width, int height,
    int x, int y, const RECT* clip)
{
    if (!context || context->pixels.empty() || !pixels || !clip || width <= 0 || height <= 0) return;
    int left = std::max(0, std::max((int)clip->left, x));
    int top = std::max(0, std::max((int)clip->top, y));
    int right = std::min(width, std::min((int)clip->right, (int)(x+context->size.cx)));
    int bottom = std::min(height, std::min((int)clip->bottom, (int)(y+context->size.cy)));
    for (int dy = top; dy < bottom; ++dy) {
        for (int dx = left; dx < right; ++dx) {
            const BYTE* source = context->pixels.data()+((size_t)(dy-y)*context->size.cx+dx-x)*4;
            BYTE* target = (BYTE*)(pixels+(size_t)(height-1-dy)*width+dx);
            unsigned inverse = 255-source[3];
            for (int channel = 0; channel < 4; ++channel)
                target[channel] = (BYTE)(source[channel]+(target[channel]*inverse+127)/255);
        }
    }
}
