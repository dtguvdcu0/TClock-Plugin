#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include "../common/plugin_api.h"
#include "../common/color_value.h"
#include "resource.h"
static HINSTANCE cs_module;
static const WCHAR* cs_keys[9] = {
    L"Saturday_TextColor", L"Sunday_TextColor", L"Holiday_TextColor", L"VPN_TextColor",
    L"UseDateColor", L"UseDowColor", L"UseTimeColor", L"UseAllColor", L"UseVPNColor"
};
static const DWORD cs_defaults[9] = { 0x00c8ffc8, 0x00c8c8ff, 0x00c8c8ff, 0x00ffff00, 0, 0, 0, 0, 0 };
static DWORD cs_values[9];
typedef struct CS_PAGE {
    DWORD values[9];
    WCHAR ini[MAX_PATH];
    BOOL english, dirty;
    void (WINAPI *changed)(HWND);
} CS_PAGE;
static COLORREF cs_resolve(DWORD value) {
    return value & 0x80000000u ? GetSysColor(value & 0x00ffffffu) : value & 0x00ffffffu;
}
static void cs_read(const TC_EXTENSION_CONTEXT* context, DWORD* values) {
    UINT i;WCHAR text[128];
    for (i = 0; i < 9; ++i) {
        LPCWSTR section = L"ColorSettings2", path = context->ini_path;WCHAR* value;
        GetPrivateProfileStringW(section, cs_keys[i], L"", text, 128, path);
        if (!text[0]) {
            section = L"Color_Font";path = context->legacy_ini;
            GetPrivateProfileStringW(section, cs_keys[i], L"", text, 128, path);
        }
        value = text;while (*value == L' ' || *value == L'\t') ++value;
        if (i < 4 && (*value == L'#' || (*value >= L'A' && *value <= L'Z') || (*value >= L'a' && *value <= L'z'))) {
            COLORREF color;
            values[i] = tcv_parse(value, &color, NULL) ? color : cs_defaults[i];
        } else values[i] = GetPrivateProfileIntW(section, cs_keys[i], cs_defaults[i], path);
    }
}
static HRESULT WINAPI cs_reload(const TC_EXTENSION_CONTEXT* context) {
    if (!context || context->size < sizeof(*context) || !context->ini_path || !context->legacy_ini) return E_INVALIDARG;
    cs_read(context, cs_values);return S_OK;
}
static LPCWSTR WINAPI cs_label(LANGID language) {
    return PRIMARYLANGID(language) == LANG_JAPANESE ? L"色設定2" : L"Colors 2";
}
static COLORREF WINAPI cs_color(const TC_TEXT_CONTEXT* context) {
    COLORREF accent = context->base;
    if (context->holiday) accent = cs_resolve(cs_values[2]);
    else if (context->time.wDayOfWeek == 0) accent = cs_resolve(cs_values[1]);
    else if (context->time.wDayOfWeek == 6) accent = cs_resolve(cs_values[0]);
    if (context->kind == TC_TEXT_ACCENT) return accent;
    if (context->vpn && cs_values[8]) return cs_resolve(cs_values[3]);
    if (cs_values[7] || (context->kind == TC_TEXT_DATE && cs_values[4]) ||
        (context->kind == TC_TEXT_WEEKDAY && cs_values[5]) || (context->kind == TC_TEXT_TIME && cs_values[6])) return accent;
    return context->base;
}
static const int cs_combos[4] = { IDC_COL_SAT, IDC_COL_SUN, IDC_COL_HOLIDAY, IDC_COL_VPN };
static const int cs_buttons[4] = { IDC_CHOOSECOL_SAT, IDC_CHOOSECOL_SUN, IDC_CHOOSECOL_HOLIDAY, IDC_CHOOSECOL_VPN };
static const int cs_checks[5] = { IDC_CHKCOL_DATE, IDC_CHKCOL_DOW, IDC_CHKCOL_TIME, IDC_CHKCOL_ALL, IDC_CHKCOL_VPN };
static const COLORREF cs_palette[16] = {
    RGB(0,0,0), RGB(128,0,0), RGB(0,128,0), RGB(128,128,0),
    RGB(0,0,128), RGB(128,0,128), RGB(0,128,128), RGB(192,192,192),
    RGB(128,128,128), RGB(255,0,0), RGB(0,255,0), RGB(255,255,0),
    RGB(0,0,255), RGB(255,0,255), RGB(0,255,255), RGB(255,255,255)
};
static void cs_select(HWND combo, COLORREF color) {
    int i;
    for (i = 0; i < 16; ++i) if (cs_palette[i] == color) break;
    if (i == 16) {
        if (SendMessageW(combo, CB_GETCOUNT, 0, 0) == 16) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)color);
        else SendMessageW(combo, CB_SETITEMDATA, 16, (LPARAM)color);
    }
    SendMessageW(combo, CB_SETCURSEL, i, 0);
}
static void cs_fill(HWND page, CS_PAGE* state) {
    UINT i, j;
    HDC dc = CreateICW(L"DISPLAY", NULL, NULL, NULL);
    BOOL canChoose = dc && GetDeviceCaps(dc, BITSPIXEL) > 8;
    if (dc) DeleteDC(dc);
    for (i = 0; i < 4; ++i) {
        HWND combo = GetDlgItem(page, cs_combos[i]);
        if (!combo) continue;
        for (j = 0; j < 16; ++j) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)cs_palette[j]);
        cs_select(combo, state->values[i]);
        {
            RECT rect;int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
            int height = (int)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
            if (count > 10) count = 10;
            GetWindowRect(combo, &rect);
            SetWindowPos(combo, NULL, 0, 0, rect.right-rect.left,
                height*count+rect.bottom-rect.top+2,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
        }
        EnableWindow(GetDlgItem(page, cs_buttons[i]), canChoose);
    }
    for (i = 0; i < 5; ++i) CheckDlgButton(page, cs_checks[i], state->values[4+i] ? BST_CHECKED : BST_UNCHECKED);
}
static INT_PTR CALLBACK cs_dialog(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    CS_PAGE* state = (CS_PAGE*)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (message == WM_MEASUREITEM) {
        MEASUREITEMSTRUCT* measure = (MEASUREITEMSTRUCT*)lParam;
        measure->itemHeight = 7 * HIWORD(GetDialogBaseUnits()) / 8;
        return TRUE;
    }
    if (message == WM_INITDIALOG) {
        const TC_EXTENSION_CONTEXT* context = (const TC_EXTENSION_CONTEXT*)lParam;
        state = (CS_PAGE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (!state) return FALSE;
        SetWindowLongPtrW(page, GWLP_USERDATA, (LONG_PTR)state);
        state->english = PRIMARYLANGID(context->language) != LANG_JAPANESE;
        state->changed = context->changed;wcscpy_s(state->ini, MAX_PATH, context->ini_path);
        cs_read(context, state->values);
        cs_fill(page, state);return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_COMMAND) {
        UINT id = LOWORD(wParam), code = HIWORD(wParam), i;
        for (i = 0; i < 4; ++i) {
            HWND combo = GetDlgItem(page, cs_combos[i]);
            if (id == (UINT)cs_combos[i] && code == CBN_SELCHANGE) {
                LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
                if (selected == CB_ERR) return TRUE;
                state->values[i] = (DWORD)SendMessageW(combo, CB_GETITEMDATA, selected, 0);break;
            }
            if (id == (UINT)cs_buttons[i] && code == BN_CLICKED) {
                CHOOSECOLORW choose = { sizeof(choose) };COLORREF custom[16];UINT j;
                for (j = 0; j < 16; ++j) custom[j] = RGB(255,255,255);
                choose.hwndOwner = page;choose.rgbResult = cs_resolve(state->values[i]);
                choose.lpCustColors = custom;choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (!ChooseColorW(&choose)) return TRUE;
                state->values[i] = choose.rgbResult;cs_select(combo, choose.rgbResult);
                PostMessageW(page, WM_NEXTDLGCTL, 1, FALSE);break;
            }
        }
        if (i == 4) {
            for (i = 0; i < 5; ++i) if (id == (UINT)cs_checks[i] && code == BN_CLICKED) {
                state->values[4+i] = IsDlgButtonChecked(page, cs_checks[i]) == BST_CHECKED;break;
            }
            if (i == 5) return FALSE;
        }
        state->dirty = TRUE;
        if (state->changed) state->changed(page);
        return TRUE;
    }
    if (message == WM_DRAWITEM) {
        DRAWITEMSTRUCT* draw = (DRAWITEMSTRUCT*)lParam;
        COLORREF color = IsWindowEnabled(draw->hwndItem) ? cs_resolve((DWORD)draw->itemData) : GetSysColor(COLOR_3DFACE);
        HBRUSH brush;
        if (draw->CtlType != ODT_COMBOBOX || draw->itemID == (UINT)-1) return FALSE;
        if (draw->itemAction & (ODA_DRAWENTIRE | ODA_SELECT)) {
            brush = CreateSolidBrush(color);FillRect(draw->hDC, &draw->rcItem, brush);DeleteObject(brush);
        }
        if (draw->itemAction & (ODA_DRAWENTIRE | ODA_SELECT | ODA_FOCUS)) {
            brush = CreateSolidBrush(draw->itemState & ODS_FOCUS ? RGB(0,0,0) : color);
            FrameRect(draw->hDC, &draw->rcItem, brush);DeleteObject(brush);
        }
        return TRUE;
    }
    if (message == WM_DESTROY) { SetWindowLongPtrW(page, GWLP_USERDATA, 0);HeapFree(GetProcessHeap(), 0, state);return TRUE; }
    return FALSE;
}
static HWND WINAPI cs_create(const TC_EXTENSION_CONTEXT* context) {
    HWND page = CreateDialogParamW(cs_module, MAKEINTRESOURCEW(PRIMARYLANGID(context->language) == LANG_JAPANESE ? 101 : 102), context->owner, cs_dialog, (LPARAM)context);
    if (page && !GetWindowLongPtrW(page, GWLP_USERDATA)) { DestroyWindow(page);page = NULL; }
    return page;
}
static HRESULT WINAPI cs_apply(HWND page) {
    CS_PAGE* state = (CS_PAGE*)GetWindowLongPtrW(page, GWLP_USERDATA);
    WCHAR directory[MAX_PATH], temporary[MAX_PATH], text[64];WCHAR* slash;UINT i;
    DWORD error;HANDLE file;WORD bom = 0xfeff;
    if (!state) return E_INVALIDARG;
    if (!state->dirty) return S_OK;
    wcscpy_s(directory, MAX_PATH, state->ini);slash = wcsrchr(directory, L'\\');
    if (!slash) return E_INVALIDARG;
    slash[1] = 0;
    if (!GetTempFileNameW(directory, L"tc2", 0, temporary)) return HRESULT_FROM_WIN32(GetLastError());
    if (GetFileAttributesW(state->ini) != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(state->ini, temporary, FALSE)) goto fail;
    } else {
        DWORD written;
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) goto fail;
        if (!WriteFile(file, &bom, sizeof(bom), &written, NULL) || written != sizeof(bom)) {
            error = GetLastError();CloseHandle(file);SetLastError(error ? error : ERROR_WRITE_FAULT);goto fail;
        }
        CloseHandle(file);
    }
    for (i = 0; i < 9; ++i) {
        swprintf_s(text, _countof(text), L"%lu", state->values[i]);
        if (!WritePrivateProfileStringW(L"ColorSettings2", cs_keys[i], text, temporary)) goto fail;
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, temporary);
    for (i = 0; i < 9; ++i) {
        if (GetPrivateProfileIntW(L"ColorSettings2", cs_keys[i], ~state->values[i], temporary) != state->values[i]) {
            SetLastError(ERROR_WRITE_FAULT);goto fail;
        }
    }
    file = CreateFileW(temporary, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto fail;
    if (!FlushFileBuffers(file)) { error = GetLastError();CloseHandle(file);SetLastError(error);goto fail; }
    CloseHandle(file);
    if (!MoveFileExW(temporary, state->ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) goto fail;
    CopyMemory(cs_values, state->values, sizeof(cs_values));state->dirty = FALSE;return S_OK;
fail:
    error = GetLastError();DeleteFileW(temporary);
    return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
}
static HWND WINAPI cs_open_page(void* user, const TC_EXTENSION_CONTEXT* context) {
    (void)user;return cs_create(context);
}
static HRESULT WINAPI cs_save_page(void* user, HWND page) {
    (void)user;return cs_apply(page);
}
static HRESULT WINAPI cs_query_color(void* user, const TC_TEXT_CONTEXT* context, COLORREF* color) {
    (void)user;*color = cs_color(context);return S_OK;
}
static HRESULT WINAPI cs_read_context(const TC_PLUGIN_CONTEXT* context) {
    TC_EXTENSION_CONTEXT settings = { sizeof(settings) };
    if (!context || context->size < sizeof(*context)) return E_INVALIDARG;
    settings.ini_path = context->ini_path;settings.legacy_ini = context->legacy_ini;
    return cs_reload(&settings);
}
static HRESULT WINAPI cs_start(const TC_PLUGIN_CONTEXT* context) {
    const TC_HOST_REGISTRY* registry;TC_REGISTRATION token;HRESULT result;
    TC_PAGE_REGISTRATION page = { sizeof(page) };TC_COLOR_REGISTRATION color = { sizeof(color) };
    result = cs_read_context(context);if (FAILED(result)) return result;
    if (!context->query_host) return E_INVALIDARG;
    result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_REGISTRY_ID, (const void**)&registry);
    if (FAILED(result)) return result;
    page.id = L"colors";page.japanese = cs_label(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT));page.english = cs_label(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    page.create = cs_open_page;page.apply = cs_save_page;
    result = registry->page(context->plugin, &page, &token);if (FAILED(result)) return result;
    color.id = L"text";color.mode = TC_COLOR_TRANSFORM;color.query = cs_query_color;
    return registry->color(context->plugin, &color, &token);
}
static void WINAPI cs_stop(void) {
}
static const TC_PLUGIN_RUNTIME cs_runtime = { sizeof(TC_PLUGIN_RUNTIME), cs_start, cs_read_context, cs_stop };

static const TC_PLUGIN_INFO cs_info = { sizeof(TC_PLUGIN_INFO), L"color.settings2", L"Color Settings 2" };
static const TC_PLUGIN_EXTENSION cs_extension = { sizeof(TC_PLUGIN_EXTENSION), cs_reload, cs_label, cs_create, cs_apply, cs_color };
__declspec(dllexport) HRESULT WINAPI TClockGetInterface(UINT abi, LPCWSTR id, const void** out) {
    if (!out) return E_POINTER;*out = NULL;
    if (abi != TC_PLUGIN_ABI || !id) return E_NOINTERFACE;
    if (!wcscmp(id, TC_PLUGIN_INFO_ID)) *out = &cs_info;
    else if (!wcscmp(id, TC_PLUGIN_RUNTIME_ID)) *out = &cs_runtime;
    else if (!wcscmp(id, TC_PLUGIN_EXTENSION_ID)) *out = &cs_extension;
    else return E_NOINTERFACE;
    return S_OK;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) cs_module = module;
    return TRUE;
}
