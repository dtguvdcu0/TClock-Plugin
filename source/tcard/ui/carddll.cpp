#include "card_document.h"
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl.h>
#include <new>
#include <strsafe.h>
#include <string>
#include <cwchar>
#include "card_api.h"
#include "card_theme.h"
#include "card_language.h"
#include "card_editor.h"
#include "card_rich_edit.h"
#include "card_fonts.h"
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

struct TCARD_WUI_HOST__ {
    HWND window = nullptr;
    TCARD_WUI_STATE state{};
    std::wstring titleText;
    std::wstring textText;
    std::wstring sourceText;
    std::wstring assetRoot;
    std::wstring fontFamilyText = L"Segoe UI";
    float fontSize = 14.0f;
    bool inheritFontFamily = false, inheritFontSize = false;
    float titleFontScale = 1.6f;
    bool titleBold = true;
    bool markdown = false;
    bool richHtml = false, loadingDraft = false, previewing = false, draftReady = false;
    bool toolsVisible = false, menuOpen = false, historyPending = false;
    bool checkpointRequested = false, readingScale = false;
    ULONGLONG saveDue = 0, historyDue = 0;
    HWND menuButton = nullptr;
    HWND minimizeButton = nullptr;
    HWND toolsTooltip = nullptr;
    std::wstring toolLabels[12];
    HWND editorOwner = nullptr;
    TCARD_WUI_COMMAND_CALLBACK callback = nullptr;
    void* callbackContext = nullptr;
    TCARD_WUI_SAVE_CALLBACK saveCallback = nullptr;
    void* saveCallbackContext = nullptr;
    bool topmost = false;
    bool editing = false;
    bool previewOnBlur = false;
    bool moving = false;
    bool editDirty = false;
    COLORREF editColor = 0;
    HWND titleEditor = nullptr;
    HWND sourceEditor = nullptr;
    HWND saveButton = nullptr;
    HWND cancelButton = nullptr;
    HWND appearanceButton = nullptr;
    HWND fontFamilyEditor = nullptr;
    HWND fontSizeEditor = nullptr;
    HWND closeButton = nullptr;
    bool appearanceOpen = false;
    HWND readEditor = nullptr;
    HFONT readFont = nullptr;
    HFONT uiFont = nullptr;
    HFONT buttonFont = nullptr;
    HBRUSH editorBrush = nullptr;
    HBRUSH readBrush = nullptr;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> backBrush;
    ComPtr<ID2D1SolidColorBrush> edgeBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
};

static constexpr size_t kMaxTextUnits = 4 * 1024 * 1024;
static HINSTANCE g_instance = nullptr;
static HMODULE g_rich_edit = nullptr;
static ComPtr<ID2D1Factory> g_d2dFactory;
static ComPtr<IDWriteFactory> g_writeFactory;
static TCARD_WUI_HOST g_legacy = nullptr;

const wchar_t* tcard_text(const wchar_t* key, const wchar_t* fallback)
{
    static thread_local std::wstring value;
    value = tcard_lang::text(key, fallback);
    return value.c_str();
}
constexpr int kEditTitle = 5001;
constexpr int kEditSource = 5002;
constexpr int kEditSave = 5003;
constexpr int kEditCancel = 5004;
constexpr int kEditClose = 5005;
constexpr int kEditMinimize = 5011;
constexpr int kEditAppearance = 5006;
constexpr int kEditFontFamily = 5007;
constexpr int kEditFontSize = 5008;
constexpr int kEditMarkdown = 5010;
constexpr int kReadBody = 5009;
constexpr int kColorFirst = 5030;
constexpr int kReadCloseWidthDip = 28;
constexpr int kReadCloseHeightDip = 24;
constexpr int kReadCloseSlotDip = 40;
constexpr int kEditCloseWidthDip = 36;
constexpr int kEditCloseHeightDip = 32;
constexpr int kEditCloseSlotDip = 48;
constexpr int kCloseRightInsetDip = 8;
struct CardColorChoice { COLORREF color; const wchar_t* key; const wchar_t* name; };
constexpr CardColorChoice kCardColors[] = {
    {RGB(255,245,168), L"color.yellow", L"Yellow"},
    {RGB(217,242,217), L"color.green", L"Green"},
    {RGB(205,235,255), L"color.blue", L"Blue"},
    {RGB(244,213,232), L"color.pink", L"Pink"},
    {RGB(233,221,247), L"color.lavender", L"Lavender"},
    {RGB(243,241,235), L"color.neutral", L"Neutral"},
    {RGB(255,255,255), L"color.white", L"White"}
};

static D2D1_COLOR_F card_color(COLORREF color)
{
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f, 1.0f);
}

static D2D1_COLOR_F card_edge(COLORREF color)
{
    return D2D1::ColorF(GetRValue(color) * 0.94f / 255.0f, GetGValue(color) * 0.94f / 255.0f,
        GetBValue(color) * 0.94f / 255.0f, 1.0f);
}

static void release_render(TCARD_WUI_HOST host)
{
    host->target.Reset();
    host->backBrush.Reset();
    host->edgeBrush.Reset();
    host->textBrush.Reset();
    host->titleFormat.Reset();
    host->bodyFormat.Reset();
}

static float card_font_size(const TCARD_WUI_HOST host)
{
    return host && host->fontSize >= 8.0f && host->fontSize <= 48.0f ? host->fontSize : 14.0f;
}

static constexpr float kDefaultTitleFontScale = 1.6f;
static constexpr float kMinTitleFontScale = 0.5f;
static constexpr float kMaxTitleFontScale = 3.0f;

static float read_title_scale()
{
    wchar_t configured[64]{};
    GetPrivateProfileStringW(L"TCard", L"TitleFontScale", L"1.6", configured, ARRAYSIZE(configured), tcard_lang::config_path().c_str());
    wchar_t* end = nullptr;
    const double value = wcstod(configured, &end);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (!end || end == configured || *end != L'\0' || value != value || value < kMinTitleFontScale || value > kMaxTitleFontScale)
        return kDefaultTitleFontScale;
    return static_cast<float>(value);
}

static void load_title_preferences(TCARD_WUI_HOST host)
{
    if (!host) return;
    host->titleFontScale = read_title_scale();
    host->titleBold = GetPrivateProfileIntW(L"TCard", L"TitleBold", 1, tcard_lang::config_path().c_str()) != 0;
}

static void populate_font_families(HWND combo, HWND owner)
{
    tcard_fonts::populate(combo,owner);
}

static int title_height_dip(TCARD_WUI_HOST host);
static int read_header_dip(TCARD_WUI_HOST host);

static void update_read_font(TCARD_WUI_HOST host)
{
    if (!host || !host->readEditor) return;
    if (host->readFont) { DeleteObject(host->readFont); host->readFont = nullptr; }
    const UINT dpi = GetDpiForWindow(host->window);
    const int height = -MulDiv(static_cast<int>(card_font_size(host) + 0.5f), static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
    const wchar_t* family = host->fontFamilyText.empty() ? L"Segoe UI" : host->fontFamilyText.c_str();
    host->readFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
    if (host->readFont) SendMessageW(host->readEditor, WM_SETFONT, reinterpret_cast<WPARAM>(host->readFont), TRUE);
    host->titleFormat.Reset();
    host->bodyFormat.Reset();
}

static BOOL create_render(TCARD_WUI_HOST host)
{
    if (!g_d2dFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2dFactory.GetAddressOf()))) return FALSE;
    if (!g_writeFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g_writeFactory.GetAddressOf())))) return FALSE;
    if (!host->target) {
        // Share the native presentation surface with RichEdit and the buttons.
        // A separate HWND render target exposes child-window holes during live resize.
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(g_d2dFactory->CreateDCRenderTarget(&properties,&host->target))) return FALSE;
    }
    if (FAILED(host->target->CreateSolidColorBrush(card_color(host->state.backColor), &host->backBrush))) return FALSE;
    if (FAILED(host->target->CreateSolidColorBrush(card_edge(host->state.backColor), &host->edgeBrush))) return FALSE;
    if (FAILED(host->target->CreateSolidColorBrush(card_color(host->state.textColor), &host->textBrush))) return FALSE;
    const wchar_t* family = host->fontFamilyText.empty() ? L"Segoe UI" : host->fontFamilyText.c_str();
    const float bodySize = card_font_size(host);
    const float titleSize = bodySize * host->titleFontScale;
    if (!host->titleFormat && FAILED(g_writeFactory->CreateTextFormat(family, nullptr,
        host->titleBold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, titleSize, L"", &host->titleFormat))) return FALSE;
    if (!host->bodyFormat && FAILED(g_writeFactory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, bodySize, L"", &host->bodyFormat))) return FALSE;
    host->titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    host->titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    host->bodyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return TRUE;
}

static void paint_card(TCARD_WUI_HOST host,HDC dc)
{
    RECT client{};GetClientRect(host->window,&client);
    if(!dc||client.right<=0||client.bottom<=0)return;
    if (!create_render(host) || FAILED(host->target->BindDC(dc,&client))) {
        const COLORREF previous=SetDCBrushColor(dc,host->state.backColor);
        FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc,previous);
        return;
    }
    const UINT dpi=GetDpiForWindow(host->window);
    host->target->SetDpi(static_cast<FLOAT>(dpi?dpi:96),static_cast<FLOAT>(dpi?dpi:96));
    const D2D1_SIZE_F size = host->target->GetSize();
    const float inset = static_cast<float>(tcard_ui::kPaperInsetDip);
    const int headerDip = 32;
    const float header = static_cast<float>(headerDip);
    host->target->BeginDraw();
    host->target->Clear(card_color(host->state.backColor));
    host->target->FillRectangle(D2D1::RectF(0, 0, size.width, header), host->edgeBrush.Get());
    host->target->DrawRectangle(D2D1::RectF(0.5f, 0.5f, max(1.0f, size.width - 0.5f), max(1.0f, size.height - 0.5f)), host->edgeBrush.Get(), 1.0f);
    if (!host->editing && !host->titleText.empty()) {
        host->target->DrawTextW(host->titleText.c_str(), static_cast<UINT32>(host->titleText.size()), host->titleFormat.Get(), D2D1::RectF(inset, 34.0f, max(inset + 1.0f, size.width-inset), 34.0f+title_height_dip(host)), host->textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    const float divider = (host->editing ? 36.0f : 34.0f) + title_height_dip(host) + 1.0f;
    host->textBrush->SetOpacity(0.14f);
    host->target->DrawLine(D2D1::Point2F(inset,divider),D2D1::Point2F(max(inset+1.0f,size.width-inset),divider),host->textBrush.Get(),1.0f);
    host->textBrush->SetOpacity(1.0f);
    if (host->target->EndDraw() == D2DERR_RECREATE_TARGET) release_render(host);
}

static std::wstring read_window_text(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(copied));
    return value;
}

static void update_vertical_scroll(HWND control)
{
    tcard_ui::update_scroll(control);
}

static void sync_read_text(TCARD_WUI_HOST host)
{
    if (!host || !host->readEditor) return;
    const HWND control=host->readEditor;
    CHARRANGE selection{};POINT scroll{};
    SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    SendMessageW(control,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    // Present only the completed document, including background and zoom changes.
    // Hidden readers must remain hidden while the source editor is active.
    const bool redraw=(GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)!=0;
    if(redraw)SendMessageW(control,WM_SETREDRAW,FALSE,0);
    if(host->richHtml&&!host->markdown)
        tcard_rich_edit::load_preview(host->readEditor,host->textText,host->fontSize,host->fontFamilyText,host->state.backColor,host->state.textColor);
    else {
        tcard_rich_edit::detach(host->readEditor);
        tcard_md::render(host->readEditor, host->textText, host->markdown, host->fontSize, host->fontFamilyText, host->assetRoot, host->state.backColor);
        tcard_rich_edit::configure(host->readEditor,false,host->fontSize,host->fontFamilyText,host->state.backColor,host->state.textColor,false);
    }
    SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
    SendMessageW(control,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    update_vertical_scroll(control);
    if(redraw){
        SendMessageW(control,WM_SETREDRAW,TRUE,0);
        RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);
    }
}

static void update_read_brush(TCARD_WUI_HOST host)
{
    if (!host) return;
    if (host->readBrush) DeleteObject(host->readBrush);
    host->readBrush = CreateSolidBrush(host->state.backColor);
    if (host->readEditor && g_rich_edit) SendMessageW(host->readEditor, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(host->state.backColor));
}

static int title_height_dip(TCARD_WUI_HOST host)
{
    const int height = static_cast<int>(card_font_size(host) * host->titleFontScale * 1.35f + 0.5f);
    return max(24, height + 4);
}

static int read_header_dip(TCARD_WUI_HOST host)
{
    return max(tcard_ui::kPaperHeaderDip, title_height_dip(host) + 2);
}

static int card_px(TCARD_WUI_HOST host,int dip);
static void card_move_control(HWND control,int x,int y,int width,int height)
{
    if(control)SetWindowPos(control,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
}

static void layout_close_button(TCARD_WUI_HOST host,int width)
{
 const int w=card_px(host,28),h=card_px(host,26),y=card_px(host,3);
 card_move_control(host->closeButton,width-w-card_px(host,6),y,w,h);
 if(host->minimizeButton)card_move_control(host->minimizeButton,width-w*2-card_px(host,10),y,w,h);
 if(host->menuButton)card_move_control(host->menuButton,card_px(host,6),y,w,h);
}
static void layout_read_editor(TCARD_WUI_HOST host,int width,int height)
{
 const int inset=card_px(host,16),top=card_px(host,(host->editing?40:36)+title_height_dip(host));
 card_move_control(host->readEditor,inset,top,max(1,width-inset*2),
  max(1,height-top-card_px(host,(host->editing||host->markdown)?42:16)));
}

static void sync_legacy_state(TCARD_WUI_HOST host)
{
    StringCchCopyW(host->state.title, ARRAYSIZE(host->state.title), host->titleText.c_str());
    StringCchCopyW(host->state.text, ARRAYSIZE(host->state.text), host->textText.c_str());
    StringCchCopyW(host->state.source, ARRAYSIZE(host->state.source), host->sourceText.c_str());
    host->state.cb = sizeof(host->state);
    host->state.version = TCARD_WUI_STATE_ABI_VERSION;
}

#include "card_popout.h"

static void end_edit(TCARD_WUI_HOST host,bool save)
{
    if(!host||!host->editing)return;
    if(save&&(host->editDirty||host->historyPending)&&!card_save_draft(host,true))return;
    if(host->toolsTooltip){DestroyWindow(host->toolsTooltip);host->toolsTooltip=nullptr;}
    for(int i=0;i<kToolCount;++i)DestroyWindow(GetDlgItem(host->window,kToolFirst+i));
    DestroyWindow(GetDlgItem(host->window,kToolStatus));
    DestroyWindow(host->titleEditor);
    DestroyWindow(host->sourceEditor);
    DestroyWindow(host->saveButton);
    DestroyWindow(host->cancelButton);
    DestroyWindow(host->appearanceButton);
    DestroyWindow(host->fontFamilyEditor);
    DestroyWindow(host->fontSizeEditor);
    DestroyWindow(GetDlgItem(host->window, kEditMarkdown));
    for (int id : {5021, 5022, 5023, 5024, 5026}) DestroyWindow(GetDlgItem(host->window, id));
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kCardColors)); ++i) DestroyWindow(GetDlgItem(host->window, kColorFirst + i));
    if (host->editorBrush) { DeleteObject(host->editorBrush); host->editorBrush = nullptr; }
    host->titleEditor = nullptr;
    host->sourceEditor = nullptr;
    host->saveButton = nullptr;
    host->cancelButton = nullptr;
    host->appearanceButton = nullptr;
    host->fontFamilyEditor = nullptr;
    host->fontSizeEditor = nullptr;
    host->appearanceOpen = false;
    host->editing = false;
    host->editDirty = false;
    if (host->readEditor) {
        sync_read_text(host);
        ShowWindow(host->readEditor, SW_SHOW);
        RECT client{};
        GetClientRect(host->window, &client);
        SendMessageW(host->window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    }
    InvalidateRect(host->window, nullptr, FALSE);
}

static void begin_edit(TCARD_WUI_HOST host)
{
    if (!host || host->editing) return;
    RECT client{};
    GetClientRect(host->window, &client);
    const int width = max(120L, client.right - client.left);
    const int height = max(140L, client.bottom - client.top);
    const bool visible = IsWindowVisible(host->window) != FALSE;
    if (visible) SendMessageW(host->window, WM_SETREDRAW, FALSE, 0);
    host->editing = true;
    host->editDirty = false;
    host->loadingDraft = true;
    host->previewing = false;
    host->historyPending = false;
    host->editColor = host->state.backColor;
    if (host->readEditor) ShowWindow(host->readEditor, SW_HIDE);
    host->editorBrush = CreateSolidBrush(host->state.backColor);
    host->appearanceOpen = false;
    if (!host->uiFont) host->uiFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    if (!host->buttonFont) host->buttonFont = CreateFontW(tcard_lang::g_code == L"ja" ? -12 : -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    tcard_ui::create_label(host->window, 5021, tcard_text(L"label.font", L"Font"), host->uiFont);
    tcard_ui::create_label(host->window, 5022, tcard_text(L"label.size", L"Size"), host->uiFont);
    tcard_ui::create_label(host->window, 5023, tcard_text(L"label.text_format", L"Text format"), host->uiFont);
    tcard_ui::create_label(host->window, 5026, tcard_text(L"label.paper_color", L"Paper color"), host->uiFont);
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kCardColors)); ++i) {
        const auto& choice = kCardColors[i];
        HWND swatch = CreateWindowExW(0, L"BUTTON", tcard_text(choice.key, choice.name),
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 26, 26, host->window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kColorFirst + i)), g_instance, nullptr);
        SendMessageW(swatch, WM_SETFONT, reinterpret_cast<WPARAM>(host->uiFont), FALSE);
        SetWindowSubclass(swatch, tcard_ui::button_proc, 1, 0);
    }
    ShowWindow(tcard_ui::create_label(host->window, 5024, L"", host->uiFont), SW_SHOW);
    host->titleEditor = CreateWindowExW(0, L"EDIT", host->titleText.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, tcard_ui::kPaperInsetDip, tcard_ui::kEditHeaderDip + 8, width - tcard_ui::kPaperInsetDip * 2, 28, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditTitle)), g_instance, nullptr);
    host->sourceEditor = tcard_edit::create(host->window, kEditSource, kEditSave, host->markdown ? host->sourceText.c_str() : L"", tcard_ui::kPaperInsetDip, tcard_ui::kEditHeaderDip + 40, width - tcard_ui::kPaperInsetDip * 2, max(80, height - tcard_ui::kEditHeaderDip - tcard_ui::kEditorCommandDip - 40), g_instance, !host->markdown);
    tcard_ui::attach_scroll(host->sourceEditor, host->state.backColor);
    tcard_edit::attach(host->titleEditor, false, kEditSave);
    SendMessageW(host->sourceEditor, EM_SETBKGNDCOLOR, 0, host->state.backColor);
    host->appearanceButton = CreateWindowExW(0, L"BUTTON", tcard_text(L"button.appearance", L"Appearance"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, tcard_ui::kPaperInsetDip, 2, 100, 28, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditAppearance)), g_instance, nullptr);
    host->fontFamilyEditor = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr, WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP, tcard_ui::kPaperInsetDip, tcard_ui::kEditHeaderDip + 40, 180, 240, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditFontFamily)), g_instance, nullptr);
    populate_font_families(host->fontFamilyEditor, host->window);
    tcard_fonts::select(host->fontFamilyEditor,host->fontFamilyText);
    tcard_fonts::inheritance(host->fontFamilyEditor,false,host->inheritFontFamily);
    host->fontSizeEditor = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr, WS_CHILD | CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, tcard_ui::kPaperInsetDip + 188, tcard_ui::kEditHeaderDip + 40, 68, 24, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditFontSize)), g_instance, nullptr);
    tcard_fonts::select_size(host->fontSizeEditor,host->fontSize,host->inheritFontSize);
    HWND formatEditor = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP, tcard_ui::kPaperInsetDip, tcard_ui::kEditHeaderDip + 68, 140, 180, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMarkdown)), g_instance, nullptr);
    SendMessageW(formatEditor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.rich", L"Text")));
    SendMessageW(formatEditor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.markdown", L"Markdown")));
    SendMessageW(formatEditor, CB_SETCURSEL, host->markdown ? 1 : 0, 0);
    SendMessageW(host->fontFamilyEditor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"font.family_placeholder", L"Font family")));
    SendMessageW(host->fontSizeEditor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"label.size", L"Size")));
    ShowWindow(host->fontFamilyEditor, SW_HIDE);
    ShowWindow(host->fontSizeEditor, SW_HIDE);
    host->saveButton = CreateWindowExW(0, L"BUTTON", tcard_text(L"button.save", L"Save"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, max(16, width - 96), max(60, height - 40), 80, 28, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditSave)), g_instance, nullptr);
    host->cancelButton = CreateWindowExW(0, L"BUTTON", tcard_text(L"button.cancel", L"Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, max(16, width - 184), max(60, height - 40), 80, 28, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditCancel)), g_instance, nullptr);
    SendMessageW(host->sourceEditor, EM_SETLIMITTEXT, tcard_clip::max_source, 0);
    for (HWND control : {host->titleEditor, host->sourceEditor, host->saveButton, host->cancelButton, host->appearanceButton, host->fontFamilyEditor, host->fontSizeEditor, GetDlgItem(host->window, kEditMarkdown)})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(host->uiFont), TRUE);
    for (HWND button : {host->saveButton, host->cancelButton, host->appearanceButton}) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(host->buttonFont ? host->buttonFont : host->uiFont), TRUE);
        SendMessageW(button, BM_SETSTYLE, BS_OWNERDRAW, TRUE);
        SetWindowSubclass(button, tcard_ui::button_proc, 1, 0);
    }
    SendMessageW(host->titleEditor, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tcard_text(L"title.placeholder", L"Title")));
    SendMessageW(host->fontSizeEditor, CB_LIMITTEXT, 64, 0);
    ShowWindow(host->saveButton,SW_HIDE);ShowWindow(host->cancelButton,SW_HIDE);
    ShowWindow(host->appearanceButton,SW_HIDE);
    host->draftReady=host->markdown;
    if(!host->markdown)host->draftReady=tcard_rich_edit::load(host->sourceEditor,
      host->richHtml?tcard_rich::parse(host->sourceText):tcard_rich::plain(host->sourceText),
      host->fontSize,host->fontFamilyText,host->state.backColor,host->state.textColor,true);
    card_create_tools(host);
    host->loadingDraft=false;host->editDirty=false;
    if(!host->draftReady){EnableWindow(host->sourceEditor,FALSE);SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.load_failed",L"Load failed"));}
    SetTimer(host->window,kCardTimer,100,nullptr);
    GetClientRect(host->window, &client);
    SendMessageW(host->window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    if (visible) {
        SendMessageW(host->window, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(host->window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
        SetFocus(host->sourceEditor);
    }
}

static LRESULT CALLBACK card_wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* host = reinterpret_cast<TCARD_WUI_HOST>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        host = reinterpret_cast<TCARD_WUI_HOST>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        host->window = hwnd;
    }
    if (!host) return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam) return 0;
        break;
    case WM_NCHITTEST:
        {
            POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            ScreenToClient(hwnd, &point);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int edge = 6;
            const bool left = point.x < edge, right = point.x >= rc.right - edge;
            const bool top = point.y < edge, bottom = point.y >= rc.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            const int header = card_px(host,32);
            const int closeSlot = card_px(host,74);
            if (point.y < header && point.x >= card_px(host,38) && point.x < rc.right - closeSlot &&
                !(host->appearanceOpen && point.x < card_px(host,161))) return HTCAPTION;
            return HTCLIENT;
        }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = POINT{card_px(host,260), card_px(host,240)};
        return 0;
    case WM_DRAWITEM:
        {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlID >= kColorFirst && item->CtlID < kColorFirst + ARRAYSIZE(kCardColors)) {
                const COLORREF color = kCardColors[item->CtlID - kColorFirst].color;
                tcard_ui::paint_swatch(*item, color, host->editColor == color, RGB(36,36,36));
                return TRUE;
            }
            if (item->CtlID != kEditClose && item->CtlID != kEditMinimize && item->CtlID != kMenuButton && !(item->CtlID>=kToolFirst&&item->CtlID<kToolFirst+kToolCount) && item->CtlID != kEditSave && item->CtlID != kEditCancel && item->CtlID != kEditAppearance) break;
            if(item->CtlID==kMenuButton||(item->CtlID>=kToolFirst&&item->CtlID<kToolFirst+kToolCount)){
                POINT cursor{};GetCursorPos(&cursor);ScreenToClient(item->hwndItem,&cursor);
                const bool hot=PtInRect(&item->rcItem,cursor)||(item->itemState&ODS_SELECTED);
                HBRUSH paper=CreateSolidBrush(tcard_ui::shade_color(host->state.backColor,hot?0.91f:1.0f));
                FillRect(item->hDC,&item->rcItem,paper);DeleteObject(paper);
                SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,host->state.textColor);
                HGDIOBJ old=SelectObject(item->hDC,host->buttonFont?host->buttonFont:GetStockObject(DEFAULT_GUI_FONT));
                RECT label=item->rcItem;const auto text=read_window_text(item->hwndItem);
                DrawTextW(item->hDC,text.c_str(),-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                SelectObject(item->hDC,old);
                if(item->itemState&ODS_FOCUS){InflateRect(&label,-2,-2);DrawFocusRect(item->hDC,&label);}
                return TRUE;
            }
            if (item->CtlID != kEditClose && item->CtlID != kEditMinimize) {
                tcard_ui::paint_button(*item, host->state.backColor, host->state.textColor, host->buttonFont ? host->buttonFont : host->uiFont, item->CtlID == kEditSave);
                return TRUE;
            }
            HBRUSH brush = CreateSolidBrush(tcard_ui::shade_color(host->state.backColor, (item->itemState & ODS_SELECTED) ? 0.85f : 0.94f));
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
            HBRUSH edge = CreateSolidBrush(tcard_ui::shade_color(host->state.backColor, 0.80f));
            FrameRect(item->hDC, &item->rcItem, edge); DeleteObject(edge);
            if (item->CtlID != kEditClose && item->CtlID != kEditMinimize) {
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, host->state.textColor);
                HGDIOBJ font = SelectObject(item->hDC, host->uiFont ? host->uiFont : GetStockObject(DEFAULT_GUI_FONT));
                RECT label = item->rcItem;
                const std::wstring text = read_window_text(item->hwndItem);
                DrawTextW(item->hDC, text.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(item->hDC, font);
                if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &label);
                return TRUE;
            }
            HPEN pen = CreatePen(PS_SOLID, 1, host->state.textColor);
            HGDIOBJ old = SelectObject(item->hDC, pen);
            const int x = (item->rcItem.right + item->rcItem.left) / 2;
            const int y = (item->rcItem.bottom + item->rcItem.top) / 2;
            if(item->CtlID==kEditMinimize){
                MoveToEx(item->hDC,x-5,y+3,nullptr);LineTo(item->hDC,x+6,y+3);
            }else{
                MoveToEx(item->hDC, x - 5, y - 5, nullptr); LineTo(item->hDC, x + 6, y + 6);
                MoveToEx(item->hDC, x + 5, y - 5, nullptr); LineTo(item->hDC, x - 6, y + 6);
            }
            SelectObject(item->hDC, old); DeleteObject(pen);
            if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
            return TRUE;
        }
    case WM_COMMAND:
        if (LOWORD(wParam) == kEditClose && HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == host->closeButton) SendMessageW(hwnd, WM_CLOSE, 0, 0);
        else if(LOWORD(wParam)==kEditMinimize && HIWORD(wParam)==BN_CLICKED && reinterpret_cast<HWND>(lParam)==host->minimizeButton) {
            if(host->editing&&(host->editDirty||host->historyPending)&&!card_save_draft(host,true))return 0;
            ShowWindow(hwnd,SW_MINIMIZE);
        }
        else if(LOWORD(wParam)==kMenuButton && HIWORD(wParam)==BN_CLICKED && reinterpret_cast<HWND>(lParam)==host->menuButton) {
            RECT r{};GetWindowRect(host->menuButton,&r);
            SendMessageW(hwnd,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(hwnd),MAKELPARAM(r.left,r.bottom));
        }
        else if(LOWORD(wParam)>=kToolFirst&&LOWORD(wParam)<kToolFirst+kToolCount){
            if(LOWORD(wParam)==kToolFirst+11&&!host->editing)begin_edit(host);
            else card_run_tool(host,LOWORD(wParam)-kToolFirst);
        }
        else if (LOWORD(wParam) == kEditAppearance) {
            host->appearanceOpen = !host->appearanceOpen;
            ShowWindow(host->fontFamilyEditor, host->appearanceOpen ? SW_SHOW : SW_HIDE);
            ShowWindow(host->fontSizeEditor, host->appearanceOpen ? SW_SHOW : SW_HIDE);
            ShowWindow(GetDlgItem(hwnd, kEditMarkdown), host->appearanceOpen ? SW_SHOW : SW_HIDE);
            for (int id : {5021, 5022, 5023, 5026}) ShowWindow(GetDlgItem(hwnd, id), host->appearanceOpen ? SW_SHOW : SW_HIDE);
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kCardColors)); ++i)
                ShowWindow(GetDlgItem(hwnd, kColorFirst + i), host->appearanceOpen ? SW_SHOW : SW_HIDE);
            ShowWindow(host->titleEditor, host->appearanceOpen ? SW_HIDE : SW_SHOW);
            ShowWindow(host->sourceEditor, host->appearanceOpen||host->previewing ? SW_HIDE : SW_SHOW);
            ShowWindow(host->readEditor,!host->appearanceOpen&&host->previewing?SW_SHOW:SW_HIDE);
            ShowWindow(host->appearanceButton,host->appearanceOpen?SW_SHOW:SW_HIDE);
            SetWindowTextW(host->appearanceButton, host->appearanceOpen ? tcard_text(L"button.back_to_note", L"Back to note") : tcard_text(L"button.appearance", L"Appearance"));
            RECT client{};
            GetClientRect(hwnd, &client);
            SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
        }
        else if (LOWORD(wParam) == kEditSave) card_save_draft(host,true);
        else if (LOWORD(wParam) == kEditCancel) end_edit(host, false);
        else if ((LOWORD(wParam) == kEditTitle || LOWORD(wParam) == kEditSource || LOWORD(wParam) == kEditFontFamily || LOWORD(wParam) == kEditFontSize) && HIWORD(wParam) == EN_CHANGE) {
            card_mark_dirty(host);
            if(LOWORD(wParam)==kEditFontFamily||LOWORD(wParam)==kEditFontSize)card_refresh_style(host);
            if (LOWORD(wParam) == kEditSource) update_vertical_scroll(host->sourceEditor);
        }
        else if (LOWORD(wParam) == kEditMarkdown && HIWORD(wParam) == CBN_SELCHANGE) {
            if(!host->draftReady){SendMessageW(GetDlgItem(hwnd,kEditMarkdown),CB_SETCURSEL,host->markdown?1:0,0);return 0;}
            const bool markdown=SendMessageW(GetDlgItem(hwnd,kEditMarkdown),CB_GETCURSEL,0,0)==1;
            if(markdown!=host->markdown){
                if(!host->markdown&&MessageBoxW(hwnd,tcard_text(L"message.convert_markdown",L"Switch to Markdown? Text formatting will become plain text."),L"TCard",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK){
                    SendMessageW(GetDlgItem(hwnd,kEditMarkdown),CB_SETCURSEL,0,0);return 0;
                }
                if((host->editDirty||host->historyPending)&&!card_save_draft(host,true)){
                    SendMessageW(GetDlgItem(hwnd,kEditMarkdown),CB_SETCURSEL,host->markdown?1:0,0);return 0;
                }
                bool readable=true;
                const auto doc=host->markdown?tcard_rich::Document{}:tcard_rich_edit::document(host->sourceEditor,false,&readable);
                if(!readable){SendMessageW(GetDlgItem(hwnd,kEditMarkdown),CB_SETCURSEL,host->markdown?1:0,0);return 0;}
                const auto text=host->markdown?read_window_text(host->sourceEditor):tcard_rich::text(doc);
                host->loadingDraft=true;tcard_rich_edit::detach(host->sourceEditor);
                SetWindowTextW(host->sourceEditor,L"");
                SendMessageW(host->sourceEditor,EM_SETTEXTMODE,(markdown?TM_PLAINTEXT:TM_RICHTEXT)|TM_MULTILEVELUNDO,0);
                host->markdown=markdown;host->richHtml=!markdown;host->previewing=false;
                if(markdown){SetWindowTextW(host->sourceEditor,text.c_str());host->draftReady=true;}
                else host->draftReady=tcard_rich_edit::load(host->sourceEditor,tcard_rich::plain(text),host->fontSize,host->fontFamilyText,host->editColor,host->state.textColor,true);
                host->loadingDraft=false;
                if(!host->draftReady){EnableWindow(host->sourceEditor,FALSE);SetWindowTextW(GetDlgItem(hwnd,kToolStatus),tcard_text(L"status.load_failed",L"Load failed"));}
                card_mark_dirty(host);
            }
        }
        else if ((LOWORD(wParam) == kEditFontFamily || LOWORD(wParam) == kEditFontSize) && (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == CBN_EDITCHANGE)) {card_refresh_style(host);card_mark_dirty(host);}
        else if (host->editing && LOWORD(wParam) >= kColorFirst && LOWORD(wParam) < kColorFirst + ARRAYSIZE(kCardColors) && HIWORD(wParam) == BN_CLICKED) {
            host->editColor = kCardColors[LOWORD(wParam) - kColorFirst].color;
            card_refresh_style(host);card_mark_dirty(host);
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(kCardColors)); ++i)
                InvalidateRect(GetDlgItem(hwnd, kColorFirst + i), nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        {
            const HWND control = reinterpret_cast<HWND>(lParam);
            if (control == host->readEditor && host->readBrush) {
                SetBkColor(reinterpret_cast<HDC>(wParam), host->state.backColor);
                SetTextColor(reinterpret_cast<HDC>(wParam), host->state.textColor);
                return reinterpret_cast<LRESULT>(host->readBrush);
            }
            if (host->editorBrush && (control == host->titleEditor || control == host->sourceEditor ||
                (GetDlgCtrlID(control) >= 5021 && GetDlgCtrlID(control) <= 5026))) {
                SetBkColor(reinterpret_cast<HDC>(wParam), host->state.backColor);
                SetTextColor(reinterpret_cast<HDC>(wParam), host->state.textColor);
                return reinterpret_cast<LRESULT>(host->editorBrush);
            }
            break;
        }
    case WM_CLOSE:
        if(host->editing&&(host->editDirty||host->historyPending)&&!card_save_draft(host,true))return 0;
        DestroyWindow(hwnd);return 0;
    case WM_ACTIVATE:
        if(LOWORD(wParam)!=WA_INACTIVE)host->previewOnBlur=false;
        else if(host->editing&&!host->menuOpen&&!host->appearanceOpen){
            const HWND next=reinterpret_cast<HWND>(lParam);
            if(!next||GetAncestor(next,GA_ROOTOWNER)!=hwnd)host->previewOnBlur=true;
        }
        break;
    case tcard_fonts::refreshMessage:
        {
        // Reload captions without rebuilding editors or changing their draft/undo state.
        tcard_lang::initialize();
        HFONT font=CreateFontW(tcard_lang::g_code==L"ja"?-12:-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        if(font){
            const HFONT previous=host->buttonFont;host->buttonFont=font;
            for(int id:{kEditSave,kEditCancel,kEditAppearance,kToolStatus})if(HWND child=GetDlgItem(hwnd,id))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            for(int i=0;i<kToolCount;++i)if(HWND child=GetDlgItem(hwnd,kToolFirst+i))SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            if(previous)DeleteObject(previous);
        }
        SetWindowTextW(hwnd,tcard_text(L"app.title",L"TCard"));
        SetWindowTextW(host->closeButton,tcard_text(L"button.close",L"Close"));
        SetWindowTextW(host->minimizeButton,tcard_text(L"button.minimize",L"Minimize"));
        SetWindowTextW(host->saveButton,tcard_text(L"button.save",L"Save"));
        SetWindowTextW(host->cancelButton,tcard_text(L"button.cancel",L"Cancel"));
        SetWindowTextW(host->appearanceButton,tcard_text(host->appearanceOpen?L"button.back_to_note":L"button.appearance",host->appearanceOpen?L"Back to note":L"Appearance"));
        SetDlgItemTextW(hwnd,5021,tcard_text(L"label.font",L"Font"));
        SetDlgItemTextW(hwnd,5022,tcard_text(L"label.size",L"Size"));
        SetDlgItemTextW(hwnd,5023,tcard_text(L"label.text_format",L"Text format"));
        SetDlgItemTextW(hwnd,5026,tcard_text(L"label.paper_color",L"Paper color"));
        if(HWND format=GetDlgItem(hwnd,kEditMarkdown)){
            host->loadingDraft=true;
            SendMessageW(format,CB_RESETCONTENT,0,0);
            SendMessageW(format,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tcard_text(L"format.plain",L"Plain text")));
            SendMessageW(format,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tcard_text(L"format.markdown",L"Markdown")));
            SendMessageW(format,CB_SETCURSEL,host->markdown?1:0,0);
            host->loadingDraft=false;
        }
        SetDlgItemTextW(hwnd,kToolFirst+11,tcard_text(host->previewing?L"button.edit":L"button.preview",host->previewing?L"Edit":L"Preview"));
        if(HWND status=GetDlgItem(hwnd,kToolStatus))SetWindowTextW(status,tcard_text(host->editDirty?L"status.editing_source":L"status.saved",host->editDirty?L"Editing source":L"Saved"));
        for(size_t i=0;i<ARRAYSIZE(kCardColors);++i)SetDlgItemTextW(hwnd,kColorFirst+static_cast<int>(i),tcard_text(kCardColors[i].key,kCardColors[i].name));
        const wchar_t* keys[]={L"tool.bold",L"tool.italic",L"tool.underline",L"tool.strike",L"tool.list",L"tool.color",L"tool.scale",L"tool.checkpoint",L"tool.heading",L"tool.link",L"tool.code",L"tool.view"};
        const wchar_t* tips[]={L"Bold",L"Italic",L"Underline",L"Strikethrough",L"List",L"Text color",L"Selection scale (%)",L"Today's stopping point",L"Heading",L"Link",L"Code",L"Edit / Preview"};
        for(int i=0;i<kToolCount;++i){
            if(!GetDlgItem(hwnd,kToolFirst+i))continue;
            host->toolLabels[i]=tcard_text(keys[i],tips[i]);
            TTTOOLINFOW info{};info.cbSize=sizeof(info);info.hwnd=hwnd;
            info.uId=reinterpret_cast<UINT_PTR>(GetDlgItem(hwnd,kToolFirst+i));info.lpszText=host->toolLabels[i].data();
            SendMessageW(host->toolsTooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));
        }
        tcard_fonts::defaults(true);
        if(host->editing){
            const bool family=tcard_fonts::inherited(host->fontFamilyEditor),size=tcard_fonts::inherited(host->fontSizeEditor);
            host->loadingDraft=true;
            tcard_fonts::inheritance(host->fontFamilyEditor,false,family);
            tcard_fonts::inheritance(host->fontSizeEditor,true,size);
            host->loadingDraft=false;
            card_refresh_style(host);
        }
        if(host->inheritFontFamily)host->fontFamilyText=tcard_fonts::defaults().family;
        if(host->inheritFontSize)host->fontSize=static_cast<float>(tcard_fonts::defaults().size);
        update_read_font(host);release_render(host);
        if(!host->editing||host->previewing)sync_read_text(host);
        { RECT area{};GetClientRect(hwnd,&area);SendMessageW(hwnd,WM_SIZE,SIZE_RESTORED,MAKELPARAM(area.right,area.bottom)); }
        InvalidateRect(hwnd,nullptr,FALSE);
        return 0;
        }
    case WM_ENTERSIZEMOVE:
        host->moving=true;return 0;
    case WM_EXITSIZEMOVE:
        host->moving=false;return 0;
    case WM_TIMER:
        if(host->moving&&wParam==kCardTimer)return 0;
        if(wParam==kCardTimer){card_update_timer(host);return 0;}
        break;
    case WM_ERASEBKGND:
        {
            // Cover newly exposed pixels even before the D2D frame is presented.
            RECT client{};GetClientRect(hwnd,&client);
            HDC dc=reinterpret_cast<HDC>(wParam);
            const COLORREF previous=SetDCBrushColor(dc,host->state.backColor);
            FillRect(dc,&client,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SetDCBrushColor(dc,previous);
        }
        return 1;
    case WM_NCRBUTTONUP:
        SendMessageW(hwnd, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(hwnd), lParam);
        return 0;
    case WM_CONTEXTMENU:
        {
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            if(!host->editing&&!host->markdown)AppendMenuW(menu, MF_STRING, 1, tcard_text(L"context.edit", L"Edit"));
            else if(!host->markdown)AppendMenuW(menu,MF_STRING,103,tcard_text(L"button.preview",L"Preview"));
            AppendMenuW(menu,MF_STRING,100,tcard_text(L"button.appearance",L"Appearance"));
            AppendMenuW(menu,MF_STRING,101,tcard_text(L"copy.note",L"Copy title and note"));
            AppendMenuW(menu,MF_STRING,102,tcard_text(L"copy.plain",L"Copy selection as plain text"));
            AppendMenuW(menu, MF_STRING | (host->topmost ? MF_CHECKED : 0), 2, tcard_text(L"context.always_on_top", L"Always on top"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 4, tcard_text(L"context.show_list", L"Show list"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 5, tcard_text(L"context.delete", L"Delete"));
            HMENU clip = CreatePopupMenu();
            if (clip) {
                AppendMenuW(clip, MF_STRING, 6, tcard_text(L"clip.new", L"Create new note"));
                AppendMenuW(clip, MF_STRING, 7, tcard_text(L"clip.append", L"Append to this note"));
                AppendMenuW(clip, MF_STRING, 8, tcard_text(L"clip.replace", L"Replace this note"));
                AppendMenuW(clip, MF_STRING, 9, tcard_text(L"clip.undo", L"Undo last import"));
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(clip), tcard_text(L"clip.menu", L"Import from clipboard"));
            }
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            host->menuOpen=true;
            const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, hwnd, nullptr);
            DestroyMenu(menu);host->menuOpen=false;
            if(command==103){end_edit(host,true);return 0;}
            if(command==100){if(!host->editing)begin_edit(host);SendMessageW(hwnd,WM_COMMAND,kEditAppearance,0);return 0;}
            if(command==101||command==102){
                HWND editor=host->editing&&!host->previewing?host->sourceEditor:host->readEditor;
                if(host->markdown&&host->editing&&!host->previewing){
                    CHARRANGE selected{};SendMessageW(editor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selected));
                    if(command==101){
                        const auto title=read_window_text(host->titleEditor);
                        tcard_rich_edit::copy_text(editor,(title.empty()?L"":title+L"\r\n")+read_window_text(editor));
                    }else SendMessageW(editor,WM_COPY,0,0);
                    SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selected));
                }else tcard_rich_edit::copy(editor,command==101,command==102,host->editing?read_window_text(host->titleEditor):host->titleText);
                return 0;
            }
            if (command >= 6 && command <= 9) { TCardWuiExecuteCommand(host, command); return 0; }
            if (command == 5) { TCardWuiExecuteCommand(host, command); return 0; }
            if (command) TCardWuiExecuteCommand(host, command);
            if (host->editing) return 0;
        }
        return 0;
    case WM_NOTIFY:
        if(reinterpret_cast<NMHDR*>(lParam)->code==EN_SELCHANGE&&reinterpret_cast<NMHDR*>(lParam)->hwndFrom==host->sourceEditor&&!host->markdown&&!host->loadingDraft&&!host->readingScale){
            const auto label=std::to_wstring(card_get_scale(host))+L"%";
            SetWindowTextW(GetDlgItem(hwnd,kToolFirst+6),label.c_str());return 0;
        }
        return tcard_md::notify(lParam);
    case WM_PRINTCLIENT:
        paint_card(host,reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            paint_card(host,ps.hdc);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_SIZE:
        if(wParam==SIZE_MINIMIZED)return 0;
        {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            layout_close_button(host, width);
            layout_read_editor(host,width,height);
            if(host->editing){
                const int inset=card_px(host,16),titleTop=card_px(host,36);
                const int titleHeight=card_px(host,title_height_dip(host));
                card_move_control(host->titleEditor,inset,titleTop,max(1,width-inset*2),titleHeight);
                const int bodyTop=titleTop+titleHeight+card_px(host,4);
                card_move_control(host->sourceEditor,inset,bodyTop,max(1,width-inset*2),max(1,height-bodyTop-card_px(host,42)));
                const int settingsWidth=min(card_px(host,360),width-inset*2);
                tcard_ui::layout_setting(GetDlgItem(hwnd,5021),host->fontFamilyEditor,inset,card_px(host,60),settingsWidth,true);
                tcard_ui::layout_setting(GetDlgItem(hwnd,5022),host->fontSizeEditor,inset,card_px(host,100),settingsWidth,true);
                tcard_ui::layout_setting(GetDlgItem(hwnd,5023),GetDlgItem(hwnd,kEditMarkdown),inset,card_px(host,140),settingsWidth,true);
                card_move_control(GetDlgItem(hwnd,5026),inset,card_px(host,180),card_px(host,84),card_px(host,22));
                const int columns=max(1,(width-card_px(host,124))/card_px(host,30));
                for(int i=0;i<static_cast<int>(ARRAYSIZE(kCardColors));++i)
                    card_move_control(GetDlgItem(hwnd,kColorFirst+i),card_px(host,108)+(i%columns)*card_px(host,30),
                     card_px(host,180)+(i/columns)*card_px(host,30),card_px(host,26),card_px(host,26));
                card_move_control(host->appearanceButton,card_px(host,42),card_px(host,2),card_px(host,115),card_px(host,28));
                card_move_control(GetDlgItem(hwnd,5024),inset,height-card_px(host,76),width-inset*2,card_px(host,28));
                ShowWindow(GetDlgItem(hwnd,5024),host->appearanceOpen?SW_SHOW:SW_HIDE);
                card_layout_tools(host,width,height);update_vertical_scroll(host->sourceEditor);
            }else {card_read_tools(host,width,height);update_vertical_scroll(host->readEditor);}
        }
        // The sizing loop must not present a resized surface before its contents.
        // Finish layout first, then paint the parent and children in this same turn.
        RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME|RDW_UPDATENOW);
        return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd,kCardTimer);
        if (host->buttonFont) { DeleteObject(host->buttonFont); host->buttonFont = nullptr; }
        if (host->uiFont) { DeleteObject(host->uiFont); host->uiFont = nullptr; }
        if (host->editorBrush) { DeleteObject(host->editorBrush); host->editorBrush = nullptr; }
        if (host->readBrush) { DeleteObject(host->readBrush); host->readBrush = nullptr; }
        if (host->readFont) { DeleteObject(host->readFont); host->readFont = nullptr; }
        host->window = nullptr;
        host->target.Reset();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static BOOL register_card_class()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = g_instance;
    wc.lpfnWndProc = card_wnd_proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"TCardWuiWindow";
    return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) g_instance = instance;
    return TRUE;
}

extern "C" TCARD_WUI_HOST WINAPI TCardWuiCreateCard(HWND owner)
{
    if (!register_card_class()) return nullptr;
    auto* host = new (std::nothrow) TCARD_WUI_HOST__;
    if (!host) return nullptr;
    host->editorOwner=owner;
    host->state.cb = sizeof(host->state);
    host->state.version = TCARD_WUI_STATE_ABI_VERSION;
    host->state.backColor = RGB(255, 245, 168);
    host->titleText = host->state.title;
    host->textText = host->state.text;
    host->sourceText = host->state.source;
    host->state.textColor = RGB(37, 37, 37);
    host->state.secondaryColor = RGB(102, 102, 102);
    g_rich_edit = LoadLibraryW(L"Msftedit.dll");
    tcard_lang::initialize();
    load_title_preferences(host);
    host->window = CreateWindowExW(WS_EX_APPWINDOW, L"TCardWuiWindow", tcard_text(L"app.title", L"TCard"), WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        120, 120, 340, 320, nullptr, nullptr, g_instance, host);
    if (!host->window) { delete host; return nullptr; }
    host->readEditor = CreateWindowExW(0, g_rich_edit ? L"RICHEDIT50W" : L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_READONLY | ES_NOHIDESEL | WS_VSCROLL | WS_TABSTOP, 0, 0, 120, 80, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReadBody)), g_instance, nullptr);
    if (!host->readEditor) { DestroyWindow(host->window); delete host; return nullptr; }
    SetWindowSubclass(host->readEditor,card_read_proc,0x54435250,reinterpret_cast<DWORD_PTR>(host));
    update_read_brush(host);
    tcard_ui::attach_scroll(host->readEditor, host->state.backColor);
    update_read_font(host);
    SendMessageW(host->readEditor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    host->closeButton = CreateWindowExW(0, L"BUTTON", tcard_text(L"button.close", L"Close"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP, 0, 2, kReadCloseWidthDip, kReadCloseHeightDip, host->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditClose)), g_instance, nullptr);
    host->minimizeButton=CreateWindowExW(0,L"BUTTON",tcard_text(L"button.minimize",L"Minimize"),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,28,26,host->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditMinimize)),g_instance,nullptr);
    SetTimer(host->window,kCardTimer,100,nullptr);
    host->menuButton=CreateWindowExW(0,L"BUTTON",L"\u2026",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,28,26,host->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMenuButton)),g_instance,nullptr);
    RECT client{};
    GetClientRect(host->window, &client);
    SendMessageW(host->window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    return host;
}

extern "C" void WINAPI TCardWuiDestroyCard(TCARD_WUI_HOST host)
{
    if (!host) return;
    if (host->window && IsWindow(host->window)) DestroyWindow(host->window);
    delete host;
}

extern "C" BOOL WINAPI TCardWuiSetCardState(TCARD_WUI_HOST host, const TCARD_WUI_STATE* state)
{
    if (!host || !state) return FALSE;
    const SIZE_T size = min(static_cast<SIZE_T>(state->cb), sizeof(host->state));
    ZeroMemory(&host->state, sizeof(host->state));
    CopyMemory(&host->state, state, size);
    host->state.cb = sizeof(host->state);
    host->state.version = TCARD_WUI_STATE_ABI_VERSION;
    host->titleText = host->state.title;
    host->textText = host->state.text;
    host->sourceText = host->state.source;
    host->fontFamilyText = L"Segoe UI";
    host->fontSize = 14.0f;
    update_read_brush(host);
    sync_read_text(host);
    release_render(host);
    if (host->window) {
        SetWindowTextW(host->window, host->state.title[0] ? host->state.title : tcard_text(L"app.title", L"TCard"));
        InvalidateRect(host->window, nullptr, FALSE);
    }
    return TRUE;
}

extern "C" BOOL WINAPI TCardWuiGetCardState(TCARD_WUI_HOST host, TCARD_WUI_STATE* state)
{
    if (!host || !state) return FALSE;
    const SIZE_T size = min(static_cast<SIZE_T>(state->cb), sizeof(host->state));
    CopyMemory(state, &host->state, size);
    state->cb = sizeof(host->state);
    return TRUE;
}

static bool assign_text(std::wstring& target, const WCHAR* value, DWORD length)
{
    if (length > kMaxTextUnits || (length && !value)) return false;
    target.assign(value ? value : L"", length);
    return true;
}

static void copy_text(WCHAR* destination, DWORD capacity, DWORD& length, const std::wstring& source)
{
    length = static_cast<DWORD>(source.size());
    if (!destination || capacity == 0) return;
    const DWORD copied = min(length, capacity - 1);
    if (copied) CopyMemory(destination, source.data(), static_cast<SIZE_T>(copied) * sizeof(WCHAR));
    destination[copied] = L'\0';
}

extern "C" BOOL WINAPI TCardWuiSetCardAssetRoot(TCARD_WUI_HOST host, const WCHAR* root, DWORD length)
{
    if (!host || length > 32767 || (!root && length)) return FALSE;
    try {
        std::wstring value = length ? std::wstring(root, length) : std::wstring{};
        if (value.find(L'\0') != std::wstring::npos) return FALSE;
        if (host->assetRoot != value) { host->assetRoot = std::move(value); if (!host->editing) sync_read_text(host); }
        return TRUE;
    } catch (...) { return FALSE; }
}

extern "C" BOOL WINAPI TCardWuiSetCardTextState(TCARD_WUI_HOST host, const TCARD_WUI_TEXT_STATE* state)
{
    if (!host || !state || state->cb < TCARD_WUI_TEXT_STATE_MIN_CB || state->version < TCARD_WUI_STATE_ABI_VERSION) return FALSE;
    try {
        std::wstring title, text, source, family;
        if (!assign_text(title, state->title, state->titleLength) ||
            !assign_text(text, state->text, state->textLength) ||
            !assign_text(source, state->source, state->sourceLength) ||
            !assign_text(family, state->fontFamily, state->fontFamilyLength)) return FALSE;
        if (family.empty()) family = L"Segoe UI";
        const float size = state->fontSize >= 8.0f && state->fontSize <= 48.0f ? state->fontSize : 14.0f;
        const bool titleChanged = host->titleText != title;
        const bool textChanged = host->textText != text;
        const bool fontChanged = !host->editing && (host->fontFamilyText != family || host->fontSize != size);
        const bool rich=state->cb>=offsetof(TCARD_WUI_TEXT_STATE,inheritFontFamily)&&state->richHtml!=FALSE;
        const bool inheritFamily=state->cb>=sizeof(*state)&&state->inheritFontFamily!=FALSE;
        const bool inheritSize=state->cb>=sizeof(*state)&&state->inheritFontSize!=FALSE;
        const bool inheritanceChanged=host->inheritFontFamily!=inheritFamily||host->inheritFontSize!=inheritSize;
        const bool formatChanged = host->markdown != (state->markdown != FALSE)||host->richHtml!=rich;
        const bool colorChanged = host->state.backColor != state->backColor ||
            host->state.textColor != state->textColor || host->state.secondaryColor != state->secondaryColor;
        const bool sourceChanged = host->sourceText != source;
        if (!titleChanged && !textChanged && !fontChanged && !formatChanged && !colorChanged && !sourceChanged && !inheritanceChanged) return TRUE;
        host->titleText = std::move(title);
        host->textText = std::move(text);
        host->sourceText = std::move(source);
        host->state.backColor = state->backColor;
        host->state.textColor = state->textColor;
        host->state.secondaryColor = state->secondaryColor;
        host->markdown = state->markdown != FALSE;
        host->richHtml=rich;
        host->inheritFontFamily=inheritFamily;host->inheritFontSize=inheritSize;
        if (fontChanged) {
            host->fontFamilyText = std::move(family);
            host->fontSize = size;
            update_read_font(host);
        }
        if (colorChanged) update_read_brush(host);
        sync_legacy_state(host);
        if (textChanged || formatChanged || fontChanged || colorChanged) sync_read_text(host);
        if (colorChanged || fontChanged) release_render(host);
        if (host->window) {
            if (titleChanged) SetWindowTextW(host->window, host->titleText.empty() ? tcard_text(L"app.title", L"TCard") : host->titleText.c_str());
            if (!host->editing && (titleChanged || fontChanged || formatChanged)) {
                RECT client{};
                GetClientRect(host->window, &client);
                layout_read_editor(host, client.right, client.bottom);
                card_read_tools(host,client.right,client.bottom);
            }
            if (titleChanged || fontChanged || colorChanged) InvalidateRect(host->window, nullptr, FALSE);
        }
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

static bool valid_output_buffer(const WCHAR* buffer, DWORD capacity)
{
    return capacity == 0 || buffer != nullptr;
}

extern "C" BOOL WINAPI TCardWuiGetCardTextState(TCARD_WUI_HOST host, TCARD_WUI_TEXT_STATE_BUFFER* state)
{
    if (!host || !state || state->cb < TCARD_WUI_TEXT_STATE_BUFFER_MIN_CB || state->version < TCARD_WUI_STATE_ABI_VERSION) return FALSE;
    if (!valid_output_buffer(state->title, state->titleCapacity) ||
        !valid_output_buffer(state->text, state->textCapacity) ||
        !valid_output_buffer(state->source, state->sourceCapacity)) return FALSE;
    const DWORD capacity=state->cb;
    state->cb = min(capacity,static_cast<DWORD>(sizeof(*state)));
    state->version = TCARD_WUI_STATE_ABI_VERSION;
    state->backColor = host->state.backColor;
    state->textColor = host->state.textColor;
    state->secondaryColor = host->state.secondaryColor;
    copy_text(state->title, state->titleCapacity, state->titleLength, host->titleText);
    copy_text(state->text, state->textCapacity, state->textLength, host->textText);
    copy_text(state->source, state->sourceCapacity, state->sourceLength, host->sourceText);
    copy_text(state->fontFamily, state->fontFamilyCapacity, state->fontFamilyLength, host->fontFamilyText);
    state->fontSize = card_font_size(host);
    state->markdown = host->markdown ? TRUE : FALSE;
    if(capacity>=offsetof(TCARD_WUI_TEXT_STATE_BUFFER,inheritFontFamily)){state->richHtml=host->richHtml;state->checkpoint=host->checkpointRequested;}
    if(capacity>=sizeof(*state)){state->inheritFontFamily=host->inheritFontFamily;state->inheritFontSize=host->inheritFontSize;}
    return TRUE;
}

extern "C" BOOL WINAPI TCardWuiShowCard(TCARD_WUI_HOST host, BOOL visible)
{
    if (!host || !host->window || !IsWindow(host->window)) return FALSE;
    ShowWindow(host->window, visible ? (IsIconic(host->window)?SW_RESTORE:SW_SHOW) : SW_HIDE);
    if (visible) {
        UpdateWindow(host->window);
        if (host->editing && !host->previewing && !host->appearanceOpen) SetFocus(host->sourceEditor);
    }
    return TRUE;
}

extern "C" HWND WINAPI TCardWuiGetCardWindow(TCARD_WUI_HOST host)
{
    return host ? host->window : nullptr;
}

extern "C" void WINAPI TCardWuiSetCardCommandCallback(TCARD_WUI_HOST host, TCARD_WUI_COMMAND_CALLBACK callback, void* context)
{
    if (!host) return;
    host->callback = callback;
    host->callbackContext = context;
}

extern "C" void WINAPI TCardWuiSetCardSaveCallback(TCARD_WUI_HOST host, TCARD_WUI_SAVE_CALLBACK callback, void* context)
{
    if (!host) return;
    host->saveCallback = callback;
    host->saveCallbackContext = context;
}

extern "C" BOOL WINAPI TCardWuiExecuteCommand(TCARD_WUI_HOST host, UINT command)
{
    if (!host || !host->window) return FALSE;
    if (command == 1) { begin_edit(host); return TRUE; }
    if (command >= 6 && command <= 9) {
        if(host->editing){end_edit(host,true);if(host->editing)return FALSE;}
        if (host->callback) { host->callback(command, host->callbackContext); return TRUE; }
        return FALSE;
    }
    if (command == 3) {
        if(host->editing)return card_save_draft(host,true);
        if (host->saveCallback) return host->saveCallback(host->saveCallbackContext);
        if (host->callback) host->callback(3, host->callbackContext);
        return TRUE;
    }
    if (command == 2) {
        host->topmost = !host->topmost;
        SetWindowPos(host->window, host->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
    }
    if (command == 4) {
        if (host->callback) { host->callback(4, host->callbackContext); return TRUE; }
        return FALSE;
    }
    if (command == 5) {
        if (host->callback) { host->callback(5, host->callbackContext); return TRUE; }
        return FALSE;
    }
    return FALSE;
}

extern "C" BOOL WINAPI TCardWuiIsTopmost(TCARD_WUI_HOST host)
{
    return host && host->topmost ? TRUE : FALSE;
}

extern "C" BOOL WINAPI TCardWuiIsCardEditing(TCARD_WUI_HOST host)
{
    return host && host->editing ? TRUE : FALSE;
}

extern "C" BOOL WINAPI TCardWuiCreateHost(HWND owner)
{
    if (g_legacy) return TRUE;
    g_legacy = TCardWuiCreateCard(owner);
    return g_legacy != nullptr;
}

extern "C" void WINAPI TCardWuiDestroyHost(void)
{
    TCardWuiDestroyCard(g_legacy);
    g_legacy = nullptr;
}

extern "C" BOOL WINAPI TCardWuiSetState(const TCARD_WUI_STATE* state)
{
    return TCardWuiSetCardState(g_legacy, state);
}

extern "C" BOOL WINAPI TCardWuiShow(BOOL visible)
{
    return TCardWuiShowCard(g_legacy, visible);
}

extern "C" HWND WINAPI TCardWuiGetWindow(void)
{
    return TCardWuiGetCardWindow(g_legacy);
}

extern "C" void WINAPI TCardWuiSetCommandCallback(TCARD_WUI_COMMAND_CALLBACK callback, void* context)
{
    TCardWuiSetCardCommandCallback(g_legacy, callback, context);
}
