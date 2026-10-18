#pragma once
#include <windows.h>
#include <commctrl.h>

namespace native_edit {
inline LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_IME_STARTCOMPOSITION) SetPropW(window, L"Helper.Edit.IME", reinterpret_cast<HANDLE>(1));
    if (message == WM_IME_ENDCOMPOSITION) RemovePropW(window, L"Helper.Edit.IME");
    if (message == WM_CHAR && w == 1 && !GetPropW(window, L"Helper.Edit.IME") && !(GetKeyState(VK_MENU) & 0x8000)) {
        SendMessageW(window, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, L"Helper.Edit.IME"); RemovePropW(window, L"Helper.Edit.Active");
        RemoveWindowSubclass(window, edit_proc, id);
    }
    return DefSubclassProc(window, message, w, l);
}
inline void attach(HWND window) {
    if (window && SetWindowSubclass(window, edit_proc, 0x48454449, 0))
        SetPropW(window, L"Helper.Edit.Active", reinterpret_cast<HANDLE>(1));
}
inline BOOL CALLBACK prepare(HWND window, LPARAM) {
    wchar_t name[32]{}; GetClassNameW(window, name, 32);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const bool edit = _wcsicmp(name, L"EDIT") == 0;
    const bool button = _wcsicmp(name, L"BUTTON") == 0 && (style & BS_TYPEMASK) != BS_GROUPBOX;
    if (edit || button || _wcsicmp(name, L"COMBOBOX") == 0 || _wcsicmp(name, L"LISTBOX") == 0)
        SetWindowLongPtrW(window, GWL_STYLE, style | WS_TABSTOP);
    if (edit) attach(window);
    return TRUE;
}
inline void prepare(HWND parent) { EnumChildWindows(parent, prepare, 0); }
inline bool translate(const MSG& message) {
    if (message.message != WM_KEYDOWN || message.wParam != 'A' ||
        !GetPropW(message.hwnd, L"Helper.Edit.Active") || GetPropW(message.hwnd, L"Helper.Edit.IME") ||
        !(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return false;
    SendMessageW(message.hwnd, WM_CHAR, 1, 0);
    return true;
}
}
