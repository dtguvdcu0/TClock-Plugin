#pragma once

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <string>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")

namespace tcard_ui {

constexpr int kPaperInsetDip = 16;
constexpr int kPaperHeaderDip = 28;
constexpr int kEditHeaderDip = 36;
constexpr int kEditorCommandDip = 48;
constexpr float kNoteTitleSizeDip = 16.0f;
constexpr float kNoteBodySizeDip = 15.0f;

inline COLORREF shade_color(COLORREF color, float factor)
{
    const int red = min(255, max(0, static_cast<int>(GetRValue(color) * factor)));
    const int green = min(255, max(0, static_cast<int>(GetGValue(color) * factor)));
    const int blue = min(255, max(0, static_cast<int>(GetBValue(color) * factor)));
    return RGB(red, green, blue);
}

inline void paint_swatch(const DRAWITEMSTRUCT& item, COLORREF color, bool active, COLORREF ink)
{
    HBRUSH brush = CreateSolidBrush(item.itemState & ODS_SELECTED ? RGB(225, 222, 208) : color);
    FillRect(item.hDC, &item.rcItem, brush); DeleteObject(brush);
    HBRUSH border = CreateSolidBrush(active ? RGB(0, 95, 184) : shade_color(color, 0.78f));
    FrameRect(item.hDC, &item.rcItem, border); DeleteObject(border);
    if (active) {
        HPEN pen = CreatePen(PS_SOLID, 2, ink);
        HGDIOBJ old = SelectObject(item.hDC, pen);
        const int x = (item.rcItem.left + item.rcItem.right) / 2;
        const int y = (item.rcItem.top + item.rcItem.bottom) / 2;
        MoveToEx(item.hDC, x - 5, y, nullptr); LineTo(item.hDC, x - 1, y + 4); LineTo(item.hDC, x + 6, y - 5);
        SelectObject(item.hDC, old); DeleteObject(pen);
    }
    if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
}

inline int dip_to_px(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

// Shared settings rows keep labels centered and controls within the paper.
inline void layout_setting(HWND label, HWND control, int x, int y, int width, bool combo)
{
    const int labelWidth = 84;
    MoveWindow(label, x, y + 3, labelWidth, 22, TRUE);
    MoveWindow(control, x + labelWidth + 8, y, max(60, width - labelWidth - 8), combo ? 240 : 28, TRUE);
}

inline HWND create_label(HWND parent, int id, const wchar_t* text, HFONT font)
{
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | SS_CENTERIMAGE,
        0, 0, 80, 22, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return label;
}

inline bool valid_size(const std::wstring& text, double& value)
{
    wchar_t* end = nullptr;
    value = wcstod(text.c_str(), &end);
    return end != text.c_str() && *end == L'\0' && value >= 8.0 && value <= 48.0;
}

// Preserve the native scrollbar gutter so hover never changes text wrapping.
struct ScrollHover { COLORREF paper; bool hovered = false; };
inline constexpr UINT_PTR scroll_id = 0x54435348;
inline bool hover_card(HWND control)
{
    POINT point{};
    if (!GetCursorPos(&point)) return false;
    const HWND root = GetAncestor(control, GA_ROOT);
    const HWND hit = WindowFromPoint(point);
    const HWND capture = GetCapture();
    return (hit && GetAncestor(hit, GA_ROOT) == root) ||
        (capture && GetAncestor(capture, GA_ROOT) == root);
}
inline void conceal_scroll(HWND control, COLORREF paper)
{
    RECT window{}; GetWindowRect(control, &window);
    HDC dc = GetWindowDC(control);
    if (!dc) return;
    HBRUSH brush = CreateSolidBrush(paper);
    for (LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
        SCROLLBARINFO info{}; info.cbSize = sizeof(info);
        if (GetScrollBarInfo(control, object, &info) && !(info.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
            OffsetRect(&info.rcScrollBar, -window.left, -window.top);
            FillRect(dc, &info.rcScrollBar, brush);
        }
    }
    DeleteObject(brush); ReleaseDC(control, dc);
}
inline LRESULT CALLBACK scroll_proc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    auto* state = reinterpret_cast<ScrollHover*>(data);
    if (message == WM_NCDESTROY) {
        KillTimer(control, id); RemoveWindowSubclass(control, scroll_proc, id); delete state;
        return DefSubclassProc(control, message, wp, lp);
    }
    if (message == WM_TIMER && wp == id) {
        const bool hovered = hover_card(control);
        if (hovered != state->hovered) {
            state->hovered = hovered;
            RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        }
        return 0;
    }
    if (message == EM_SETBKGNDCOLOR) state->paper = static_cast<COLORREF>(lp);
    const auto result = DefSubclassProc(control, message, wp, lp);
    if (!state->hovered && (message == WM_NCPAINT || message == WM_PAINT || message == WM_VSCROLL ||
        message == WM_HSCROLL || message == WM_MOUSEWHEEL || message == WM_SIZE)) conceal_scroll(control, state->paper);
    return result;
}
inline void attach_scroll(HWND control, COLORREF paper)
{
    auto* state = new ScrollHover{paper, hover_card(control)};
    if (!SetWindowSubclass(control, scroll_proc, scroll_id, reinterpret_cast<DWORD_PTR>(state))) { delete state; return; }
    SetTimer(control, scroll_id, 100, nullptr);
}

inline void update_scroll(HWND control)
{
    if (!control) return;
    wchar_t name[32]{};
    GetClassNameW(control, name, ARRAYSIZE(name));
    bool needed = false;
    if (_wcsicmp(name, L"EDIT") == 0 || (_wcsicmp(name, L"RICHEDIT50W") == 0 &&
        (SendMessageW(control, EM_GETTEXTMODE, 0, 0) & TM_PLAINTEXT))) {
        RECT rect{};
        SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rect));
        HDC dc = GetDC(control);
        if (!dc) return;
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
        HGDIOBJ previous = SelectObject(dc, font ? font : GetStockObject(SYSTEM_FONT));
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(control, dc);
        const LRESULT lines = SendMessageW(control, EM_GETLINECOUNT, 0, 0);
        needed = GetWindowTextLengthW(control) > 0 && lines * metrics.tmHeight > rect.bottom - rect.top;
    } else {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE};
        if (GetScrollInfo(control, SB_VERT, &info))
            needed = info.nPage > 0 && static_cast<long long>(info.nMax) - info.nMin + 1 > info.nPage;
    }
    const bool visible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VSCROLL) != 0;
    if (visible != needed) ShowScrollBar(control, SB_VERT, needed);
}

inline LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, button_proc, 1);
    }
    return DefSubclassProc(window, message, w, l);
}

inline void paint_button(const DRAWITEMSTRUCT& item, COLORREF paper, COLORREF, HFONT font, bool primary, bool active = false)
{
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(item.hwndItem, &cursor);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool hot = !disabled && PtInRect(&item.rcItem, cursor);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    // Command states use a neutral palette independently of note paper and ink.
    const COLORREF fill = disabled ? RGB(243,243,243) : primary ?
        (pressed ? RGB(0,75,145) : hot ? RGB(0,85,164) : RGB(0,95,184)) :
        active ? (pressed ? RGB(201,226,247) : hot ? RGB(220,235,248) : RGB(233,241,248)) :
        (pressed ? RGB(226,226,226) : hot ? RGB(237,237,237) : RGB(255,255,255));
    HBRUSH background = CreateSolidBrush(paper);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary && !disabled ? fill : active && !disabled ? RGB(0,95,184) : RGB(209,209,209));
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush), oldPen = SelectObject(item.hDC, pen);
    const int radius = dip_to_px(10, GetDpiForWindow(item.hwndItem));
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, radius, radius);
    SelectObject(item.hDC, oldBrush); SelectObject(item.hDC, oldPen);
    DeleteObject(brush); DeleteObject(pen);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(160,160,160) : primary ? RGB(255,255,255) : active ? RGB(0,83,150) : RGB(26,26,26));
    HGDIOBJ oldFont = SelectObject(item.hDC, font);
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, 128);
    RECT label = item.rcItem;
    InflateRect(&label, -6, 0);
    if (pressed && !disabled) OffsetRect(&label, 0, 1);
    DrawTextW(item.hDC, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(item.hDC, oldFont);
    if ((item.itemState & ODS_FOCUS) && !disabled) {
        InflateRect(&label, -3, -3);
        HBRUSH focus = CreateSolidBrush(primary ? RGB(255,255,255) : RGB(0,95,184));
        FrameRect(item.hDC, &label, focus);
        DeleteObject(focus);
    }
}

// Settings use paired colors independently of the OS palette and note appearance.
inline constexpr COLORREF settings_paper = RGB(246,245,242);
inline constexpr COLORREF settings_ink = RGB(37,37,37);
inline constexpr UINT_PTR settings_button_id = 0x54434442;

inline LRESULT CALLBACK handle_settings_button(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    if (message == BM_SETSTYLE) {
        data = (wp & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
        SetWindowSubclass(window, handle_settings_button, id, data);
        wp = (wp & ~static_cast<WPARAM>(BS_TYPEMASK)) | BS_OWNERDRAW;
    } else if (message == WM_GETDLGCODE) {
        const LRESULT flags = DefSubclassProc(window, message, wp, lp);
        return (flags & ~(DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) |
            (data ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
    } else if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track); InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, handle_settings_button, id);
    }
    return DefSubclassProc(window, message, wp, lp);
}

inline void prepare_settings(HWND dialog)
{
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[32]{};
        if (!GetClassNameW(child, name, ARRAYSIZE(name)) || _wcsicmp(name, L"BUTTON") != 0) continue;
        const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
        const LONG_PTR type = style & BS_TYPEMASK;
        if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) continue;
        if (!SetWindowSubclass(child, handle_settings_button, settings_button_id, type == BS_DEFPUSHBUTTON)) continue;
        SetWindowLongPtrW(child, GWL_STYLE, (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | BS_OWNERDRAW);
        InvalidateRect(child, nullptr, TRUE);
    }
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
}

inline INT_PTR paint_settings(HWND, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_DRAWITEM && lp) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item.CtlType != ODT_BUTTON) return 0;
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
        if (!font) font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        paint_button(item, settings_paper, settings_ink, font, item.CtlID == IDOK);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ||
        message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        const COLORREF paper = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ? RGB(255,255,255) : settings_paper;
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, paper); SetTextColor(dc, settings_ink); SetDCBrushColor(dc, paper);
        return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
    }
    return 0;
}

}
