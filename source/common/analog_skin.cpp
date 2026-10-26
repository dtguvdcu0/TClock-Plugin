#define NOMINMAX
#include "analog_skin.h"
#include "skin_manifest.h"
#include <gdiplus.h>
#include <math.h>
#include <new>
#include <vector>
#include <algorithm>
#include <string>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
using namespace Gdiplus;

struct ACS_CONTEXT {
    ULONG_PTR token = 0;
    Bitmap* images[4] = {};
    Bitmap* faceCache = nullptr;
    std::vector<BYTE> pixels;
    int diameter = 0;
    bool legacy = false;
    Color lineColors[3];
    REAL lineLengths[3] = {}, lineWidths[3] = {};
    REAL centerX = 0, centerY = 0;
    REAL faceScale = 1;
    REAL pivotX[3] = {}, pivotY[3] = {}, scales[3] = {};
};

static bool acs_read_number(const SKN_MANIFEST& manifest, const WCHAR* section,
    const WCHAR* key, REAL* number)
{
    const WCHAR* value = manifest.read(section, key);
    WCHAR* end;
    if (!value || !*value) return false;
    double parsed = wcstod(value, &end);
    if (*end || !_finite(parsed) || parsed < 0 || parsed > 1254) return false;
    *number = (REAL)parsed;
    return true;
}

static Bitmap* acs_load_image(const std::wstring& root, const SKN_MANIFEST& manifest, const WCHAR* key)
{
    std::wstring path;
    if (!skn_resolve_asset(root, manifest.read(L"Assets", key), path)) return nullptr;
    Bitmap* image = Bitmap::FromFile(path.c_str(), FALSE);
    if (!image || image->GetLastStatus() != Ok || image->GetWidth() != 1254 || image->GetHeight() != 1254) {
        delete image;
        return nullptr;
    }
    return image;
}

ACS_CONTEXT* acs_create(HINSTANCE module, int mode, int face)
{
    // Explicit runtime ownership: callers initialize outside DllMain and destroy before module unload.
    if ((mode != EXT_MODE_NORMAL && mode != EXT_MODE_CLASSIC && mode != EXT_MODE_LEGACY) || face < 0 || face > 1) return nullptr;
    WCHAR path[32768];
    DWORD length = GetModuleFileNameW(module, path, _countof(path));
    if (!length || length >= _countof(path)) return nullptr;
    WCHAR* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    slash[1] = 0;
    ACS_CONTEXT* context = new(std::nothrow) ACS_CONTEXT;
    if (!context) return nullptr;
    GdiplusStartupInput input;
    if (GdiplusStartup(&context->token, &input, nullptr) != Ok) {
        delete context;
        return nullptr;
    }
    bool valid = false;
    try {
        std::wstring root = std::wstring(path) + (mode == EXT_MODE_LEGACY ? L"clock-skins\\legacy\\" :
            (mode == EXT_MODE_CLASSIC ? L"clock-skins\\classic\\" : L"clock-skins\\metal\\"));
        if (mode) face = 0;
        SKN_MANIFEST manifest;
        if (!manifest.load((root + L"skin.ini").c_str()) ||
            !manifest.matches(L"Skin", L"Version", L"2") ||
            !manifest.matches(L"Skin", L"Renderer", mode == EXT_MODE_LEGACY ? L"BitmapHands" : L"ImageHands")) {
            acs_destroy(context); return nullptr;
        }
        const WCHAR* faces[] = {L"Arabic", L"Ticks"};
        const WCHAR* faceKeys[] = {L"ArabicFace", L"TicksFace"};
        const WCHAR* hands[] = {L"HourHand", L"MinuteHand", L"SecondHand"};
        if (mode == EXT_MODE_LEGACY) {
            context->legacy = true;
            std::wstring file;
            if (skn_resolve_asset(root, manifest.read(L"Assets", L"Face"), file))
                context->images[0] = Bitmap::FromFile(file.c_str());
            valid =
                context->images[0] && context->images[0]->GetLastStatus() == Ok &&
                context->images[0]->GetWidth() == context->images[0]->GetHeight() &&
                context->images[0]->GetWidth() >= 16 && context->images[0]->GetWidth() <= 4096;
            for (int i = 0; i < 3; ++i) {
                unsigned long rgb = 0;
                valid = valid && skn_read_color(manifest, hands[i], L"Color", rgb) &&
                    acs_read_number(manifest, hands[i], L"Length", &context->lineLengths[i]) &&
                    acs_read_number(manifest, hands[i], L"Width", &context->lineWidths[i]) &&
                    context->lineLengths[i] > 0 && context->lineLengths[i] <= .5f &&
                    context->lineWidths[i] > 0 && context->lineWidths[i] <= .1f;
                context->lineColors[i] = Color(255, (BYTE)(rgb>>16), (BYTE)(rgb>>8), (BYTE)rgb);
            }
        } else {
            valid = acs_read_number(manifest, faces[face], L"CenterX", &context->centerX) &&
                acs_read_number(manifest, faces[face], L"CenterY", &context->centerY);
            if (manifest.read(faces[face], L"FaceScale"))
                valid = valid && acs_read_number(manifest, faces[face], L"FaceScale", &context->faceScale) &&
                    context->faceScale > 0 && context->faceScale <= 1;
            context->images[0] = acs_load_image(root, manifest, mode ? L"Face" : faceKeys[face]);
            valid = valid && context->images[0];
            for (int i = 0; i < 3; ++i) {
                context->images[i+1] = acs_load_image(root, manifest, hands[i]);
                valid = valid && context->images[i+1] &&
                    acs_read_number(manifest, hands[i], L"PivotX", &context->pivotX[i]) &&
                    acs_read_number(manifest, hands[i], L"PivotY", &context->pivotY[i]) &&
                    acs_read_number(manifest, hands[i], L"Scale", &context->scales[i]) &&
                    context->scales[i] > 0 && context->scales[i] <= 1;
            }
        }
    } catch (...) { valid = false; }
    if (!valid) { acs_destroy(context); return nullptr; }
    return context;
}

void acs_destroy(ACS_CONTEXT* context)
{
    if (!context) return;
    for (auto image : context->images) delete image;
    delete context->faceCache;
    ULONG_PTR token = context->token;
    delete context;
    if (token) GdiplusShutdown(token);
}

static Status acs_draw_hand(Graphics& graphics, ACS_CONTEXT* context, int hand, REAL angle)
{
    double radians = angle * 3.14159265358979323846 / 180.0;
    REAL scale = context->diameter / 1254.0f * context->scales[hand];
    REAL c = (REAL)cos(radians) * scale, s = (REAL)sin(radians) * scale;
    REAL centerX = context->centerX * context->diameter / 1254.0f;
    REAL centerY = context->centerY * context->diameter / 1254.0f;
    // Clockwise rotation in screen coordinates around each measured attachment pivot.
    Matrix transform(c, s, -s, c,
        centerX - c * context->pivotX[hand] + s * context->pivotY[hand],
        centerY - s * context->pivotX[hand] - c * context->pivotY[hand]);
    graphics.SetTransform(&transform);
    Status result = graphics.DrawImage(context->images[hand+1], 0, 0, 1254, 1254);
    graphics.ResetTransform();
    return result;
}

BOOL acs_render(ACS_CONTEXT* context, int diameter, const SYSTEMTIME* time, BOOL seconds)
{
    if (!context || !time || diameter < 4 || diameter > 1024) return FALSE;
    try {
        if (context->diameter != diameter) {
            Bitmap* face = new Bitmap(diameter, diameter, PixelFormat32bppPARGB);
            Status result = face->GetLastStatus();
            if (result == Ok) {
                Graphics graphics(face);
                graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                // Normalize artwork margins without changing hand lengths or the clock slot.
                REAL faceSize = diameter * context->faceScale;
                RectF faceRect(context->centerX * diameter / 1254.0f * (1-context->faceScale),
                    context->centerY * diameter / 1254.0f * (1-context->faceScale), faceSize, faceSize);
                if (context->legacy) {
                    graphics.Clear(Color(0, 0, 0, 0));
                    Color transparent;
                    context->images[0]->GetPixel(0, 0, &transparent);
                    ImageAttributes attributes;
                    attributes.SetColorKey(transparent, transparent, ColorAdjustTypeBitmap);
                    result = graphics.DrawImage(context->images[0], Rect(0, 0, diameter, diameter),
                        0, 0, (INT)context->images[0]->GetWidth(), (INT)context->images[0]->GetHeight(), UnitPixel, &attributes);
                } else result = graphics.DrawImage(context->images[0], faceRect);
            }
            if (result != Ok) {
                delete face;
                return FALSE;
            }
            std::vector<BYTE> pixels((size_t)diameter * diameter * 4);
            context->pixels.swap(pixels);
            delete context->faceCache;
            context->faceCache = face;
            context->diameter = diameter;
        }
        std::fill(context->pixels.begin(), context->pixels.end(), (BYTE)0);
        Bitmap target(diameter, diameter, diameter * 4, PixelFormat32bppPARGB, context->pixels.data());
        Graphics graphics(&target);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        if (graphics.DrawImage(context->faceCache, 0, 0, diameter, diameter) != Ok) return FALSE;
        REAL minute = time->wMinute + time->wSecond / 60.0f;
        REAL hour = (time->wHour % 12) + minute / 60.0f;
        if (context->legacy) {
            const REAL angles[] = {hour*30, minute*6, time->wSecond*6.0f};
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            for (int i = 0; i < (seconds ? 3 : 2); ++i) {
                REAL radians = angles[i]*(REAL)(3.141592653589793/180);
                REAL center = diameter/2.0f, length = diameter*context->lineLengths[i];
                Pen pen(context->lineColors[i], std::max(1.0f, diameter*context->lineWidths[i]));
                if (graphics.DrawLine(&pen, center, center, center+length*(REAL)sin(radians),
                    center-length*(REAL)cos(radians)) != Ok) return FALSE;
            }
            graphics.Flush(FlushIntentionSync);
            return TRUE;
        }
        if (acs_draw_hand(graphics, context, 0, hour * 30) != Ok ||
            acs_draw_hand(graphics, context, 1, minute * 6) != Ok) return FALSE;
        // Restore the bundled face cap over the attachment holes, including when seconds are hidden.
        REAL scale = diameter / 1254.0f;
        REAL capRadius = 38 * context->faceScale;
        RectF cap((context->centerX-capRadius)*scale, (context->centerY-capRadius)*scale,
            2*capRadius*scale, 2*capRadius*scale);
        if (graphics.DrawImage(context->images[0], cap, context->centerX-38,
            context->centerY-38, 76.0f, 76.0f, UnitPixel) != Ok) return FALSE;
        if (seconds && acs_draw_hand(graphics, context, 2, time->wSecond * 6.0f) != Ok) return FALSE;
        graphics.Flush(FlushIntentionSync);
        return TRUE;
    } catch (...) { return FALSE; }
}

const BYTE* acs_get_pixels(const ACS_CONTEXT* context)
{
    return context && !context->pixels.empty() ? context->pixels.data() : nullptr;
}

BOOL acs_draw(ACS_CONTEXT* context, HDC dc, int x, int y, int diameter,
    const SYSTEMTIME* time, BOOL seconds)
{
    if (!acs_render(context, diameter, time, seconds)) return FALSE;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = diameter;
    info.bmiHeader.biHeight = -diameter;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(dc);
    BOOL result = FALSE;
    if (bitmap && memory) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        CopyMemory(pixels, context->pixels.data(), context->pixels.size());
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        result = AlphaBlend(dc, x, y, diameter, diameter, memory, 0, 0, diameter, diameter, blend);
        SelectObject(memory, old);
    }
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    return result;
}

void acs_blend(ACS_CONTEXT* context, RGBQUAD* pixels, int width, int height,
    int x, int y, const RECT* clip)
{
    if (!context || !pixels || !clip || context->pixels.empty() || width <= 0 || height <= 0) return;
    for (int sy = 0; sy < context->diameter; ++sy) {
        int dy = y + sy;
        if (dy < 0 || dy >= height || dy < clip->top || dy >= clip->bottom) continue;
        for (int sx = 0; sx < context->diameter; ++sx) {
            int dx = x + sx;
            if (dx < 0 || dx >= width || dx < clip->left || dx >= clip->right) continue;
            const BYTE* src = context->pixels.data() + ((size_t)sy * context->diameter + sx) * 4;
            BYTE* dst = (BYTE*)(pixels + (size_t)(height - 1 - dy) * width + dx);
            unsigned inverse = 255 - src[3];
            for (int channel = 0; channel < 4; ++channel)
                dst[channel] = (BYTE)(src[channel] + (dst[channel] * inverse + 127) / 255);
        }
    }
}

int acs_get_diameter(int diameterDip, int cross, UINT dpi)
{
    int margin = std::max(1, MulDiv(1, dpi, 96));
    int available = std::min(1024, cross - margin * 2);
    if (available < 4) return 0;
    return diameterDip > 0 ? std::min(available, MulDiv(diameterDip, dpi, 96)) : available;
}

UINT acs_get_dpi(HWND window)
{
    typedef UINT (WINAPI* GetDpiProc)(HWND);
    HMODULE module = GetModuleHandleW(L"user32.dll");
    GetDpiProc getDpi = module ? (GetDpiProc)GetProcAddress(module, "GetDpiForWindow") : nullptr;
    UINT dpi = getDpi && window ? getDpi(window) : 0;
    if (dpi) return dpi;
    HDC dc = GetDC(window);
    dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi ? dpi : 96;
}
