#pragma once

#include <windows.h>

// Place the whole popup on the desktop side of its own taskbar, with a DPI-scaled gap.
static __inline POINT tip_place_outside(const RECT* bounds, const RECT* bar, POINT anchor,
    int width, int height, int gap)
{
    RECT available = *bounds;
    POINT position = { anchor.x, anchor.y - height };
    if (bar->right - bar->left >= bar->bottom - bar->top) {
        if (bar->top + bar->bottom >= bounds->top + bounds->bottom) {
            available.bottom = min(available.bottom, bar->top - gap);
            position.y = available.bottom - height;
        } else {
            available.top = max(available.top, bar->bottom + gap);
            position.y = available.top;
        }
    } else {
        if (bar->left + bar->right >= bounds->left + bounds->right) {
            available.right = min(available.right, bar->left - gap);
            position.x = available.right - width;
        } else {
            available.left = max(available.left, bar->right + gap);
            position.x = available.left;
        }
    }
    // Oversized popups keep their size; pin their origin to the available desktop.
    position.x = max(available.left, min(position.x, available.right - width));
    position.y = max(available.top, min(position.y, available.bottom - height));
    return position;
}

static __inline BOOL tip_position_outside(HWND owner, POINT anchor, int width, int height, POINT* position)
{
    HWND taskbar = GetAncestor(owner, GA_ROOT);
    WCHAR name[80];
    RECT bar;
    MONITORINFO monitor = { sizeof(monitor) };
    typedef UINT (WINAPI *TIP_DPI_FN)(HWND);
    static TIP_DPI_FN getDpi = NULL;
    static BOOL resolved = FALSE;
    HDC dc;
    int dpi = 96;
    if (!taskbar || !GetClassNameW(taskbar, name, _countof(name)) ||
        (lstrcmpW(name, L"Shell_TrayWnd") && lstrcmpW(name, L"Shell_SecondaryTrayWnd")) ||
        !GetWindowRect(taskbar, &bar) ||
        !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor)) return FALSE;
    if (!resolved) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) getDpi = (TIP_DPI_FN)GetProcAddress(user32, "GetDpiForWindow");
        resolved = TRUE;
    }
    if (getDpi) dpi = (int)getDpi(owner);
    else {
        dc = GetDC(owner);
        if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(owner, dc); }
    }
    if (dpi <= 0) dpi = 96;
    *position = tip_place_outside(&monitor.rcMonitor, &bar, anchor, width, height, MulDiv(8, dpi, 96));
    return TRUE;
}
