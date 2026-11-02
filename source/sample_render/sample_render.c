/*
 * 時計の文字部分を自分で描く、描画プラグインのサンプルです。
 * 本体の設定で有効にすると、時計の文字の代わりにこのDLLが描いた画像が表示されます。
 * 背景・グラフ・バーメーターは本体が描き、その上にこの画像が重なります。
 * このサンプルは時計の文字を、半透明の角丸の帯の上に描きます。
 * DLLはTClock本体(exe)の中で動き、Explorerには読み込まれません。
 */
#include "../common/plugin_api.h"
#include "../common/render_api.h"
#include <wchar.h>

#define SR_BADGE_COLOR RGB(0, 120, 215)
#define SR_BADGE_ALPHA 96u

/* 文字の描画に使う作業用の画面外ビットマップです。描画のたびに作り直します。 */
typedef struct SR_CANVAS { HDC dc; HBITMAP bitmap; HGDIOBJ old; UINT* bits; } SR_CANVAS;

static BOOL sr_open(SR_CANVAS* canvas, UINT width, UINT height) {
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER) } };
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height; /* 上の行から並べます。 */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    canvas->dc = CreateCompatibleDC(NULL);
    canvas->bitmap = canvas->dc ? CreateDIBSection(canvas->dc, &info, DIB_RGB_COLORS, (void**)&canvas->bits, NULL, 0) : NULL;
    if (!canvas->bitmap) {
        if (canvas->dc) DeleteDC(canvas->dc);
        return FALSE;
    }
    canvas->old = SelectObject(canvas->dc, canvas->bitmap);
    return TRUE;
}

static void sr_close(SR_CANVAS* canvas) {
    SelectObject(canvas->dc, canvas->old);
    DeleteObject(canvas->bitmap);
    DeleteDC(canvas->dc);
}

/* 本体の色指定(COLORREF)から、アルファ乗算済みの1画素(0xAARRGGBB)を作ります。 */
static UINT sr_pixel(COLORREF color, UINT alpha) {
    return (alpha << 24) | ((GetRValue(color) * alpha / 255) << 16) | ((GetGValue(color) * alpha / 255) << 8) | (GetBValue(color) * alpha / 255);
}

/* 上に重ねる画素 top を、下の画素 bottom に重ねます(どちらも乗算済み)。 */
static UINT sr_over(UINT top, UINT bottom) {
    UINT keep = 255 - (top >> 24), result = 0, shift;
    for (shift = 0; shift < 32; shift += 8)
        result |= ((((top >> shift) & 0xFF) + ((bottom >> shift) & 0xFF) * keep / 255) & 0xFF) << shift;
    return result;
}

/* 角丸の帯の内側なら TRUE です。角の半径は高さの1/4です。 */
static BOOL sr_inside(UINT x, UINT y, UINT width, UINT height) {
    int inset = 2, radius = (int)height / 4, left = inset, top = inset;
    int right = (int)width - inset - 1, bottom = (int)height - inset - 1, cx, cy;
    if ((int)x < left || (int)x > right || (int)y < top || (int)y > bottom) return FALSE;
    /* 一番近い角の円の中心からの距離で、角の丸みを判定します。 */
    cx = (int)x < left + radius ? left + radius : (int)x > right - radius ? right - radius : (int)x;
    cy = (int)y < top + radius ? top + radius : (int)y > bottom - radius ? bottom - radius : (int)y;
    return ((int)x - cx) * ((int)x - cx) + ((int)y - cy) * ((int)y - cy) <= radius * radius;
}

/*
 * 時計の文字を描きます。白い文字を黒地に描き、その明るさを文字の濃さとして使うので、
 * 本体の文字色と半透明の帯をきれいに重ねられます。
 */
static HRESULT WINAPI sr_render(void* user, const TC_RENDER_REQUEST* request, TC_RENDER_OUTPUT* output) {
    SR_CANVAS canvas;
    HFONT font, old;
    RECT area;
    SIZE extent = { 0 };
    const WCHAR* line;
    UINT x, y, lines = 1, longest = 0;
    int fontHeight;
    (void)user;
    for (line = request->text; *line; ++line) if (*line == L'\n') ++lines;
    fontHeight = (int)request->height * 3 / (int)(4 * lines + 1);
    if (fontHeight < 6) fontHeight = 6;
    if (!sr_open(&canvas, request->width, request->height)) return E_OUTOFMEMORY;
    font = CreateFontW(-fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    old = SelectObject(canvas.dc, font);
    SetTextColor(canvas.dc, RGB(255, 255, 255));
    SetBkMode(canvas.dc, TRANSPARENT);
    SetRect(&area, 0, 0, (int)request->width, (int)request->height);
    DrawTextW(canvas.dc, request->text, -1, &area, DT_CENTER | DT_NOPREFIX | (lines == 1 ? DT_VCENTER | DT_SINGLELINE : 0));
    /* 横長のタスクバーでは、いちばん長い行が収まる幅を本体に伝えます。 */
    for (line = request->text; *line;) {
        const WCHAR* end = line;
        while (*end && *end != L'\r' && *end != L'\n') ++end;
        if (GetTextExtentPoint32W(canvas.dc, line, (int)(end - line), &extent) && (UINT)extent.cx > longest) longest = (UINT)extent.cx;
        line = *end == L'\r' ? end + 2 : *end ? end + 1 : end;
    }
    GdiFlush();
    for (y = 0; y < request->height; ++y) {
        for (x = 0; x < request->width; ++x) {
            UINT coverage = canvas.bits[(SIZE_T)y * request->width + x] & 0xFF;
            UINT pixel = sr_inside(x, y, request->width, request->height) ? sr_pixel(SR_BADGE_COLOR, SR_BADGE_ALPHA) : 0;
            if (coverage) pixel = sr_over(sr_pixel(request->foreColor, coverage), pixel);
            output->pixels[(SIZE_T)y * request->width + x] = pixel;
        }
    }
    SelectObject(canvas.dc, old);
    DeleteObject(font);
    sr_close(&canvas);
    if (!(request->flags & TC_RENDER_VERTICAL) && longest) {
        output->preferredWidth = longest + request->height / 2;
        if (output->preferredWidth > TC_RENDER_MAX_WIDTH) output->preferredWidth = TC_RENDER_MAX_WIDTH;
        output->preferredHeight = request->height;
    }
    /* 領域を返さないので、クリックの区分(ゾーン)は本体の設定どおりです。 */
    return S_OK;
}

static const TC_RENDER_PROVIDER sr_provider = { sizeof(TC_RENDER_PROVIDER), sr_render };

static HRESULT WINAPI sr_start(const TC_PLUGIN_CONTEXT* context) {
    const TC_HOST_FEATURES* features;
    TC_FEATURE_REGISTRATION entry = { sizeof(entry) };
    TC_REGISTRATION token;
    HRESULT result;
    if (!context || context->size < sizeof(*context) || !context->query_host) return E_INVALIDARG;
    result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_FEATURES_ID, (const void**)&features);
    if (FAILED(result)) return result;
    entry.id = L"render";
    entry.interface_id = TC_PLUGIN_RENDER_ID;
    entry.priority = 0;
    entry.table = &sr_provider;
    return features->add(context->plugin, &entry, &token);
}

/* 登録は本体が停止時に取り消します。このサンプルに解放する資源はありません。 */
static void WINAPI sr_stop(void) {
}

static const TC_PLUGIN_RUNTIME sr_runtime = { sizeof(TC_PLUGIN_RUNTIME), sr_start, NULL, sr_stop };

/* IDは .rc の TC_PLUGIN_HEADER_ID と揃えます。 */
static const TC_PLUGIN_INFO sr_info = { sizeof(TC_PLUGIN_INFO), L"sample.render", L"Sample clock renderer" };

/* 本体が最初に呼ぶ公開関数です。この名前は sample_render.def と揃えます。 */
HRESULT WINAPI TClockGetInterface(UINT abi, LPCWSTR id, const void** out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (abi != TC_PLUGIN_ABI || !id) return E_NOINTERFACE;
    if (wcscmp(id, TC_PLUGIN_INFO_ID) == 0) *out = &sr_info;
    else if (wcscmp(id, TC_PLUGIN_RUNTIME_ID) == 0) *out = &sr_runtime;
    else return E_NOINTERFACE;
    return S_OK;
}
