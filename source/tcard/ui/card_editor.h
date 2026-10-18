#pragma once
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <string>

namespace tcard_edit {
constexpr UINT_PTR subclass_id = 0x54434544;
struct Editor { bool rich = false, composing = false; UINT save = 0; };
inline LRESULT CALLBACK editor_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data);
inline bool composing(HWND window)
{
    return window && GetPropW(window, L"TCard.Editor.Composing") != nullptr;
}
inline bool translate(const MSG& message)
{
    if (message.message != WM_KEYDOWN || !GetPropW(message.hwnd, L"TCard.Editor.Active") ||
        !(GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) || composing(message.hwnd)) return false;
    WPARAM character = 0;
    switch (message.wParam) {
    case 'A': character = 1; break; case 'C': character = 3; break; case 'V': character = 22; break;
    case 'X': character = 24; break; case 'Z': character = 26; break; case 'Y': character = 25; break;
    case 'S': if (GetPropW(message.hwnd, L"TCard.Editor.Save")) character = 19; break;
    }
    if (!character) return false;
    SendMessageW(message.hwnd, WM_CHAR, character, 0);
    return true;
}
inline LRESULT CALLBACK editor_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
    auto* editor = reinterpret_cast<Editor*>(data);
    if (message == WM_NCDESTROY) {
        RemovePropW(window, L"TCard.Editor.Active"); RemovePropW(window, L"TCard.Editor.Save"); RemovePropW(window, L"TCard.Editor.Composing");
        RemoveWindowSubclass(window, editor_proc, id); delete editor; return DefSubclassProc(window, message, w, l);
    }
    if (message == WM_IME_STARTCOMPOSITION) { editor->composing = true; SetPropW(window, L"TCard.Editor.Composing", reinterpret_cast<HANDLE>(1)); }
    if (message == WM_IME_ENDCOMPOSITION) { editor->composing = false; RemovePropW(window, L"TCard.Editor.Composing"); }
    if (message == WM_SETTEXT && editor->rich) {
        const auto* text = reinterpret_cast<const wchar_t*>(l);
        size_t line = 0; bool wrap = true;
        if (text) for (const auto* p = text; *p; ++p) {
            if (*p == L'\r' || *p == L'\n') line = 0;
            else if (++line > 4096) { wrap = false; break; }
        }
        SendMessageW(window, EM_SETTARGETDEVICE, 0, wrap ? 0 : 1);
        ShowScrollBar(window, SB_HORZ, !wrap);
    }
    if (message == WM_CHAR && !editor->composing && !(GetKeyState(VK_MENU) & 0x8000)) {
        switch (w) {
        case 1: SendMessageW(window, EM_SETSEL, 0, -1); return 0;
        case 3: SendMessageW(window, WM_COPY, 0, 0); return 0;
        case 22: SendMessageW(window, WM_PASTE, 0, 0); return 0;
        case 24: SendMessageW(window, WM_CUT, 0, 0); return 0;
        case 26: SendMessageW(window, editor->rich && (GetKeyState(VK_SHIFT) & 0x8000) ? EM_REDO : EM_UNDO, 0, 0); return 0;
        case 25: if (editor->rich) SendMessageW(window, EM_REDO, 0, 0); return 0;
        case 19: if (editor->save) SendMessageW(GetParent(window), WM_COMMAND, editor->save, 0); return 0;
        }
    }
    return DefSubclassProc(window, message, w, l);
}
inline void attach(HWND window, bool rich, UINT save = 0)
{
    if (!window) return;
    auto* editor = new Editor{rich, false, save};
    if (!SetWindowSubclass(window, editor_proc, subclass_id, reinterpret_cast<DWORD_PTR>(editor))) { delete editor; return; }
    SetPropW(window, L"TCard.Editor.Active", reinterpret_cast<HANDLE>(1));
    if (save) SetPropW(window, L"TCard.Editor.Save", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(save)));
}
inline HWND create(HWND parent, int id, UINT save, const wchar_t* text, int x, int y, int width, int height, HINSTANCE instance, bool styled = false)
{
    static HMODULE module = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HWND window = CreateWindowExW(0, module ? L"RICHEDIT50W" : L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_NOHIDESEL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP |
        (module ? 0 : ES_AUTOHSCROLL | WS_HSCROLL), x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (!window) return nullptr;
    if (module) {
        SendMessageW(window, EM_SETTEXTMODE, (styled ? TM_RICHTEXT : TM_PLAINTEXT) | TM_MULTILEVELUNDO, 0);
        SendMessageW(window, EM_SETUNDOLIMIT, 32, 0);
        SendMessageW(window, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SCROLL);
        SendMessageW(window, EM_EXLIMITTEXT, 0, 2 * 1024 * 1024);
    } else SendMessageW(window, EM_SETLIMITTEXT, 2 * 1024 * 1024, 0);
    attach(window, module != nullptr, save);
    SetWindowTextW(window, text ? text : L"");
    SendMessageW(window, EM_EMPTYUNDOBUFFER, 0, 0);
    return window;
}
}
