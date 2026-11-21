// UtilityPack: registers Windows tool, settings, folder and administrator shortcuts as TClock builtin commands.
// The private INI selects which commands are registered; the Settings button opens a checklist dialog.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "../common/plugin_api.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

enum Base { kSystem, kWindows, kUri, kTerminal };
enum Flag : unsigned { kAdmin = 1, kConfirm = 2, kHidden = 4, kBattery = 8 };
enum Group { kTools, kSettings, kFolders, kAdministrator, kGroupCount };

struct Item {
    LPCWSTR id;
    LPCWSTR japanese;
    LPCWSTR english;
    Group group;
    bool initial;
    Base base;
    LPCWSTR file;
    LPCWSTR parameters;  // %SYS% = System32, %TEMP% = user temporary folder
    unsigned flags;
};

// IDs are saved in host menu/mouse assignments as dll:utility.pack/<id>; never rename them.
const Item kItems[] = {
    {L"volume_mixer", L"音量ミキサー", L"Volume mixer", kTools, true, kUri, L"ms-settings:apps-volume", nullptr, 0},
    {L"resource_monitor", L"リソースモニター", L"Resource Monitor", kTools, true, kSystem, L"resmon.exe", nullptr, 0},
    {L"reliability_monitor", L"信頼性モニター", L"Reliability Monitor", kTools, true, kSystem, L"perfmon.exe", L"/rel", 0},
    {L"programs_features", L"プログラムと機能", L"Programs and Features", kTools, true, kSystem, L"control.exe", L"appwiz.cpl", 0},
    {L"windows_features", L"Windowsの機能の有効化", L"Turn Windows features on or off", kTools, true, kSystem, L"OptionalFeatures.exe", nullptr, 0},
    {L"services", L"サービス", L"Services", kTools, true, kSystem, L"services.msc", nullptr, 0},
    {L"environment_variables", L"環境変数の編集", L"Edit environment variables", kTools, true, kSystem, L"rundll32.exe", L"sysdm.cpl,EditEnvironmentVariables", 0},
    {L"device_manager", L"デバイスマネージャー", L"Device Manager", kTools, true, kSystem, L"devmgmt.msc", nullptr, 0},
    {L"task_scheduler", L"タスクスケジューラ", L"Task Scheduler", kTools, false, kSystem, L"taskschd.msc", nullptr, 0},
    {L"system_advanced", L"システムの詳細設定", L"Advanced system settings", kTools, false, kSystem, L"SystemPropertiesAdvanced.exe", nullptr, 0},
    {L"credential_manager", L"資格情報マネージャー", L"Credential Manager", kTools, false, kSystem, L"control.exe", L"/name Microsoft.CredentialManager", 0},
    {L"event_viewer", L"イベントビューアー", L"Event Viewer", kTools, false, kSystem, L"eventvwr.msc", nullptr, 0},
    {L"disk_cleanup", L"ディスククリーンアップ", L"Disk Cleanup", kTools, false, kSystem, L"cleanmgr.exe", nullptr, 0},
    {L"windows_update", L"Windows Update", L"Windows Update", kSettings, true, kUri, L"ms-settings:windowsupdate", nullptr, 0},
    {L"startup_apps", L"スタートアップアプリ", L"Startup apps", kSettings, true, kUri, L"ms-settings:startupapps", nullptr, 0},
    {L"default_apps", L"既定のアプリ", L"Default apps", kSettings, false, kUri, L"ms-settings:defaultapps", nullptr, 0},
    {L"startup_folder", L"スタートアップフォルダー", L"Startup folder", kFolders, true, kWindows, L"explorer.exe", L"shell:Startup", 0},
    {L"apps_folder", L"すべてのアプリ", L"All apps", kFolders, false, kWindows, L"explorer.exe", L"shell:AppsFolder", 0},
    {L"appdata_roaming", L"AppData (Roaming)", L"AppData (Roaming)", kFolders, false, kWindows, L"explorer.exe", L"shell:AppData", 0},
    {L"appdata_local", L"AppData (Local)", L"AppData (Local)", kFolders, false, kWindows, L"explorer.exe", L"shell:Local AppData", 0},
    {L"sendto_folder", L"送るフォルダー", L"SendTo folder", kFolders, false, kWindows, L"explorer.exe", L"shell:SendTo", 0},
    {L"admin_terminal", L"管理者ターミナル", L"Terminal (Admin)", kAdministrator, true, kTerminal, nullptr, nullptr, kAdmin},
    {L"admin_cmd", L"管理者コマンドプロンプト", L"Command Prompt (Admin)", kAdministrator, false, kSystem, L"cmd.exe", nullptr, kAdmin},
    {L"admin_powershell", L"管理者 PowerShell", L"PowerShell (Admin)", kAdministrator, false, kSystem, L"WindowsPowerShell\\v1.0\\powershell.exe", nullptr, kAdmin},
    {L"flush_dns", L"DNSキャッシュを消去", L"Flush DNS cache", kAdministrator, true, kSystem, L"cmd.exe",
        L"/d /c \"\"%SYS%\\ipconfig.exe\" /flushdns & \"%SYS%\\timeout.exe\" /t 5\"", kAdmin},
    {L"sleep_requests", L"スリープを妨げている要求", L"Requests blocking sleep", kAdministrator, true, kSystem, L"cmd.exe",
        L"/d /k \"\"%SYS%\\powercfg.exe\" /requests\"", kAdmin},
    {L"battery_report", L"バッテリーレポート", L"Battery report", kAdministrator, true, kSystem, L"cmd.exe",
        L"/d /c \"\"%SYS%\\powercfg.exe\" /batteryreport /output \"%TEMP%battery-report.html\" && start \"\" \"%TEMP%battery-report.html\"\"", kHidden | kBattery},
    {L"repair_windows", L"Windowsの修復（DISM → SFC）", L"Repair Windows (DISM, then SFC)", kAdministrator, false, kSystem, L"cmd.exe",
        L"/d /k \"\"%SYS%\\Dism.exe\" /Online /Cleanup-Image /RestoreHealth && \"%SYS%\\sfc.exe\" /scannow\"", kAdmin | kConfirm},
    {L"check_drive", L"ドライブのオンライン検査（chkdsk /scan）", L"Online drive scan (chkdsk /scan)", kAdministrator, false, kSystem, L"cmd.exe",
        L"/d /k \"\"%SYS%\\chkdsk.exe\" %DRIVE% /scan\"", kAdmin | kConfirm},
};
constexpr int kItemCount = ARRAYSIZE(kItems);
constexpr LPCWSTR kSection = L"Items";

struct Runtime {
    void* plugin = nullptr;
    const TC_HOST_REGISTRY* registry = nullptr;
    std::wstring ini;
    TC_REGISTRATION tokens[kItemCount] = {};
} g_runtime;

bool japanese(LANGID language) { return PRIMARYLANGID(language) == LANG_JAPANESE; }
LPCWSTR item_label(const Item& item, LANGID language) { return japanese(language) ? item.japanese : item.english; }

bool has_battery()
{
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.BatteryFlag != 128 && status.BatteryFlag != 255;
}
bool available(const Item& item) { return !(item.flags & kBattery) || has_battery(); }

void read_choices(const std::wstring& ini, bool* choices)
{
    for (int i = 0; i < kItemCount; ++i) {
        const UINT value = ini.empty() ? 2u : GetPrivateProfileIntW(kSection, kItems[i].id, 2, ini.c_str());
        choices[i] = value == 2u ? kItems[i].initial : value != 0;
    }
}

bool write_choices(const std::wstring& ini, const bool* choices)
{
    for (int i = 0; i < kItemCount; ++i)
        if (!WritePrivateProfileStringW(kSection, kItems[i].id, choices[i] ? L"1" : L"0", ini.c_str())) return false;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    return true;
}

std::wstring folder(UINT (WINAPI* query)(LPWSTR, UINT))
{
    WCHAR path[MAX_PATH];
    const UINT length = query(path, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(path, length) : std::wstring();
}

void replace_all(std::wstring& text, LPCWSTR token, const std::wstring& value)
{
    const size_t size = wcslen(token);
    for (size_t at = text.find(token); at != std::wstring::npos; at = text.find(token, at + value.size())) text.replace(at, size, value);
}

std::wstring expand(LPCWSTR parameters, const std::wstring& system)
{
    std::wstring text = parameters ? parameters : L"";
    WCHAR temp[MAX_PATH + 1];
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    replace_all(text, L"%SYS%", system);
    replace_all(text, L"%TEMP%", length && length < ARRAYSIZE(temp) ? std::wstring(temp, length) : std::wstring());
    replace_all(text, L"%DRIVE%", system.size() >= 2 ? system.substr(0, 2) : std::wstring(L"C:"));
    return text;
}

HRESULT launch(HWND owner, LPCWSTR file, LPCWSTR parameters, unsigned flags)
{
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = (flags & kAdmin) ? L"runas" : nullptr;
    info.lpFile = file;
    info.lpParameters = parameters && *parameters ? parameters : nullptr;
    info.nShow = (flags & kHidden) ? SW_HIDE : SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return S_OK;
    const DWORD error = GetLastError();
    return error == ERROR_CANCELLED ? S_OK : HRESULT_FROM_WIN32(error);  // A declined UAC prompt is not an error.
}

// Windows Terminal is used through its per-user execution alias when present; otherwise Command Prompt.
HRESULT launch_terminal(HWND owner, const std::wstring& system)
{
    WCHAR local[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length && length < MAX_PATH) {
        const std::wstring terminal = std::wstring(local) + L"\\Microsoft\\WindowsApps\\wt.exe";
        if (GetFileAttributesW(terminal.c_str()) != INVALID_FILE_ATTRIBUTES) {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.hwnd = owner;info.lpVerb = L"runas";info.lpFile = terminal.c_str();info.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&info)) return S_OK;
            if (GetLastError() == ERROR_CANCELLED) return S_OK;
        }
    }
    return launch(owner, (system + L"\\cmd.exe").c_str(), nullptr, kAdmin);
}

HRESULT WINAPI run_item(void* user, const TC_ACTION_CONTEXT* context)
{
    const auto* item = static_cast<const Item*>(user);
    if (!item || !context || context->size < sizeof(*context)) return E_INVALIDARG;
    const LANGID language = context->language;
    if (!available(*item)) {
        MessageBoxW(context->owner, japanese(language) ? L"このPCにはバッテリーがありません。" : L"This PC has no battery.",
            item_label(*item, language), MB_OK | MB_ICONINFORMATION);
        return S_OK;
    }
    if (item->flags & kConfirm) {
        const std::wstring text = japanese(language)
            ? std::wstring(L"「") + item->japanese + L"」を管理者として実行します。\n完了まで時間がかかることがあります。続行しますか？"
            : std::wstring(L"Run \"") + item->english + L"\" as administrator?\nIt may take a long time to finish.";
        if (MessageBoxW(context->owner, text.c_str(), item_label(*item, language), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return S_OK;
    }
    const std::wstring system = folder(GetSystemDirectoryW), windows = folder(GetWindowsDirectoryW);
    if (system.empty() || windows.empty()) return E_FAIL;
    switch (item->base) {
    case kTerminal: return launch_terminal(context->owner, system);
    case kUri: return launch(context->owner, item->file, nullptr, item->flags);
    case kWindows: return launch(context->owner, (windows + L"\\" + item->file).c_str(), expand(item->parameters, system).c_str(), item->flags);
    default: return launch(context->owner, (system + L"\\" + item->file).c_str(), expand(item->parameters, system).c_str(), item->flags);
    }
}

// Registers checked items and removes unchecked ones; unchanged registrations keep their tokens.
HRESULT sync_registrations()
{
    if (!g_runtime.registry) return E_UNEXPECTED;
    bool choices[kItemCount];
    read_choices(g_runtime.ini, choices);
    for (int i = 0; i < kItemCount; ++i) {
        const bool wanted = choices[i] && available(kItems[i]);
        if (!wanted && g_runtime.tokens[i]) {
            g_runtime.registry->remove(g_runtime.plugin, g_runtime.tokens[i]);
            g_runtime.tokens[i] = 0;
        } else if (wanted && !g_runtime.tokens[i]) {
            TC_COMMAND_REGISTRATION command{sizeof(command)};
            command.id = kItems[i].id;command.japanese = kItems[i].japanese;command.english = kItems[i].english;
            command.user = const_cast<Item*>(&kItems[i]);command.invoke = run_item;
            TC_REGISTRATION token = 0;
            const HRESULT result = g_runtime.registry->command(g_runtime.plugin, &command, &token);
            if (FAILED(result)) return result;
            g_runtime.tokens[i] = token;
        }
    }
    return S_OK;
}

HRESULT WINAPI runtime_start(const TC_PLUGIN_CONTEXT* context)
{
    if (!context || context->size < sizeof(*context) || !context->query_host) return E_INVALIDARG;
    const void* table = nullptr;
    const HRESULT result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_REGISTRY_ID, &table);
    if (FAILED(result)) return result;
    const auto* registry = static_cast<const TC_HOST_REGISTRY*>(table);
    if (!registry || registry->size < sizeof(*registry) || !registry->command || !registry->remove) return E_NOINTERFACE;
    g_runtime = Runtime{};
    g_runtime.plugin = context->plugin;g_runtime.registry = registry;g_runtime.ini = context->ini_path ? context->ini_path : L"";
    return sync_registrations();
}

HRESULT WINAPI runtime_reload(const TC_PLUGIN_CONTEXT* context)
{
    if (context && context->size >= sizeof(*context) && context->ini_path) g_runtime.ini = context->ini_path;
    return sync_registrations();
}

void WINAPI runtime_stop()
{
    // The host hides this provider's registrations before stop.
    g_runtime = Runtime{};
}

// ---- Settings dialog ----

constexpr int kNote = 100, kList = 101, kAllOn = 102, kDefaults = 103;

struct Dialog {
    LANGID language;
    std::wstring ini;
    bool choices[kItemCount];
    HFONT note = nullptr;
};

// Common Controls 6 (resource 3) provides checkbox list groups while the dialog is open.
struct Theme {
    HANDLE context = INVALID_HANDLE_VALUE;ULONG_PTR cookie = 0;bool active = false;
    Theme()
    {
        ACTCTXW request{sizeof(request)};
        request.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        WCHAR path[MAX_PATH];
        const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
        if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
        request.lpSource = path;request.hModule = module;request.lpResourceName = MAKEINTRESOURCEW(3);
        context = CreateActCtxW(&request);if (context == INVALID_HANDLE_VALUE) return;
        active = ActivateActCtx(context, &cookie) != FALSE;
        if (active) {INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);}
    }
    ~Theme() {if (active) DeactivateActCtx(0, cookie);if (context != INVALID_HANDLE_VALUE) ReleaseActCtx(context);}
    Theme(const Theme&) = delete;Theme& operator=(const Theme&) = delete;
};

LPCWSTR group_label(int group, LANGID language)
{
    static const LPCWSTR ja[] = {L"ツール", L"設定画面", L"フォルダー", L"管理者"};
    static const LPCWSTR en[] = {L"Tools", L"Settings pages", L"Folders", L"Administrator"};
    return japanese(language) ? ja[group] : en[group];
}

void fill_list(HWND list, const Dialog& state)
{
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < kItemCount; ++i) ListView_SetCheckState(list, i, state.choices[i]);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);InvalidateRect(list, nullptr, TRUE);
}

void read_list(HWND list, Dialog& state)
{
    for (int i = 0; i < kItemCount; ++i) state.choices[i] = ListView_GetCheckState(list, i) != FALSE;
}

void place(HWND dialog, int id, int x, int y, int width, int height)
{
    RECT rect{x, y, x + width, y + height};MapDialogRect(dialog, &rect);
    MoveWindow(GetDlgItem(dialog, id), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, FALSE);
}

HWND add(HWND dialog, LPCWSTR type, LPCWSTR text, DWORD style, int id)
{
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
    return control;
}

void init_dialog(HWND dialog, Dialog& state)
{
    const bool ja = japanese(state.language);
    SetWindowTextW(dialog, ja ? L"ユーティリティパックの設定" : L"UtilityPack settings");
    add(dialog, L"STATIC", ja ? L"チェックした項目を、右クリックメニューやマウス操作の「操作」から選べるようにします。\r\n使用中の項目をオフにすると、その割り当ては動作しなくなります。"
        : L"Checked items appear as operations for menus and mouse clicks.\r\nTurning off an item in use disables its assignment.", SS_NOPREFIX, kNote);
    HWND list = add(dialog, WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kList);
    add(dialog, L"BUTTON", ja ? L"すべてオン" : L"Turn all on", WS_TABSTOP | BS_PUSHBUTTON, kAllOn);
    add(dialog, L"BUTTON", ja ? L"初期状態に戻す" : L"Restore defaults", WS_TABSTOP | BS_PUSHBUTTON, kDefaults);
    add(dialog, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    add(dialog, L"BUTTON", ja ? L"キャンセル" : L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);

    // Layout in DLUs: 7 DLU margins, 14 DLU buttons, list takes the remaining height.
    place(dialog, kNote, 7, 6, 266, 18);
    place(dialog, kList, 7, 28, 266, 196);
    place(dialog, kAllOn, 7, 230, 60, 14);
    place(dialog, kDefaults, 71, 230, 66, 14);
    place(dialog, IDOK, 159, 230, 55, 14);
    place(dialog, IDCANCEL, 218, 230, 55, 14);

    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    RECT client{};GetClientRect(list, &client);
    LVCOLUMNW column{};column.mask = LVCF_WIDTH;column.cx = client.right - GetSystemMetrics(SM_CXVSCROLL);
    ListView_InsertColumn(list, 0, &column);
    ListView_EnableGroupView(list, TRUE);
    for (int g = 0; g < kGroupCount; ++g) {
        LVGROUP group{sizeof(group)};
        group.mask = LVGF_HEADER | LVGF_GROUPID;group.iGroupId = g;group.pszHeader = const_cast<LPWSTR>(group_label(g, state.language));
        ListView_InsertGroup(list, -1, &group);
    }
    const bool battery = has_battery();
    for (int i = 0; i < kItemCount; ++i) {
        std::wstring text = item_label(kItems[i], state.language);
        if ((kItems[i].flags & kBattery) && !battery) text += ja ? L"（バッテリーなし）" : L" (no battery)";
        LVITEMW item{};item.mask = LVIF_TEXT | LVIF_GROUPID;item.iItem = i;item.iGroupId = kItems[i].group;
        item.pszText = text.data();
        ListView_InsertItem(list, &item);
    }
    fill_list(list, state);

    // Center over the TClock settings window, staying inside its monitor work area.
    HWND owner = GetWindow(dialog, GW_OWNER);RECT frame{}, anchor{};GetWindowRect(dialog, &frame);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(owner ? owner : dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    if (!owner || !GetWindowRect(owner, &anchor)) anchor = monitor.rcWork;
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    int x = (anchor.left + anchor.right - width) / 2, y = (anchor.top + anchor.bottom - height) / 2;
    x = max(monitor.rcWork.left, min(x, static_cast<int>(monitor.rcWork.right) - width));
    y = max(monitor.rcWork.top, min(y, static_cast<int>(monitor.rcWork.bottom) - height));
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dialog, DWLP_USER, lp);state = reinterpret_cast<Dialog*>(lp);
        init_dialog(dialog, *state);SetFocus(GetDlgItem(dialog, kList));return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kAllOn:
            for (bool& choice : state->choices) choice = true;
            fill_list(GetDlgItem(dialog, kList), *state);return TRUE;
        case kDefaults:
            for (int i = 0; i < kItemCount; ++i) state->choices[i] = kItems[i].initial;
            fill_list(GetDlgItem(dialog, kList), *state);return TRUE;
        case IDOK:
            read_list(GetDlgItem(dialog, kList), *state);
            if (!write_choices(state->ini, state->choices)) {
                // Keep the dialog open so the user can retry without losing the edits.
                const bool ja = japanese(state->language);
                MessageBoxW(dialog, ja ? L"設定を保存できませんでした。" : L"Could not save the settings.", nullptr, MB_OK | MB_ICONERROR);
                return TRUE;
            }
            EndDialog(dialog, IDOK);return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);return TRUE;
        }
        break;
    }
    return FALSE;
}

INT_PTR show_dialog(HWND owner, Dialog& state)
{
    Theme theme;
    DLGTEMPLATE dialog{};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;dialog.cx = 280;dialog.cy = 251;
    std::vector<WORD> data(sizeof(dialog) / sizeof(WORD));
    memcpy(data.data(), &dialog, sizeof(dialog));
    data.insert(data.end(), {0, 0, 0, 9});  // no menu, default class, empty title, 9 pt
    const wchar_t face[] = L"Segoe UI";
    data.insert(data.end(), face, face + ARRAYSIZE(face));
    return DialogBoxIndirectParamW(reinterpret_cast<HINSTANCE>(&__ImageBase), reinterpret_cast<const DLGTEMPLATE*>(data.data()),
        owner, dialog_proc, reinterpret_cast<LPARAM>(&state));
}

HRESULT WINAPI settings_reload(const TC_SETTINGS_CONTEXT* context)
{
    return context && context->size >= sizeof(*context) ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI settings_show(const TC_SETTINGS_CONTEXT* context)
{
    if (!context || context->size < sizeof(*context) || !context->ini_path || !*context->ini_path) return E_INVALIDARG;
    Dialog state{context->language, context->ini_path, {}};
    read_choices(state.ini, state.choices);
    const INT_PTR result = show_dialog(context->owner, state);
    if (result == -1) return HRESULT_FROM_WIN32(GetLastError());
    return result == IDOK ? S_OK : S_FALSE;  // The host refreshes providers afterwards, which re-syncs registrations.
}

const TC_PLUGIN_INFO kInfo = {sizeof(TC_PLUGIN_INFO), L"utility.pack", L"UtilityPack"};
const TC_PLUGIN_RUNTIME kRuntime = {sizeof(TC_PLUGIN_RUNTIME), runtime_start, runtime_reload, runtime_stop};
const TC_PLUGIN_SETTINGS kSettingsTable = {sizeof(TC_PLUGIN_SETTINGS), settings_reload, settings_show};

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI TClockGetInterface(UINT abi, LPCWSTR id, const void** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    if (abi != TC_PLUGIN_ABI || !id) return E_NOINTERFACE;
    if (wcscmp(id, TC_PLUGIN_INFO_ID) == 0) *out = &kInfo;
    else if (wcscmp(id, TC_PLUGIN_RUNTIME_ID) == 0) *out = &kRuntime;
    else if (wcscmp(id, TC_PLUGIN_SETTINGS_ID) == 0) *out = &kSettingsTable;
    else return E_NOINTERFACE;
    return S_OK;
}
