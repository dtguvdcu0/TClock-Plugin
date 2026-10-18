#include "../ui/card_document.h"
#include "../core/tcard_core.h"
#include "../ui/card_api.h"
#include "../ui/card_theme.h"
#include "../ui/card_time.h"
#include "../ui/card_language.h"
#include "../ui/card_editor.h"
#include "../ui/card_rich_edit.h"
#include "../ui/card_fonts.h"
#include "../core/card_download.h"
#include "../core/card_web.h"
#include <thread>
#include "card_sync_client.h"
// Shared log/transport types must be declared before the local UI anonymous namespace.
#include "../core/card_connection_log.h"
#include "../core/card_signal_sandbox.h"

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <strsafe.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <cstdint>

namespace {

constexpr int kList = 1001;
constexpr int kPreview = 1002;
constexpr int kNew = 1003;
constexpr int kOpen = 1004;
constexpr int kTitleEditor = 1005;
constexpr int kSourceEditor = 1006;
constexpr int kEdit = 1007;
constexpr int kSave = 1008;
constexpr int kCancel = 1009;
constexpr int kSearch = 1010;
constexpr int kClearSearch = 1011;
constexpr int kColorYellow = 1012;
constexpr int kColorGreen = 1013;
constexpr int kColorBlue = 1014;
constexpr int kColorPink = 1015;
constexpr int kColorLavender = 1016;
constexpr int kColorNeutral = 1017;
constexpr int kAppearance = 1018;
constexpr int kFontFamily = 1019;
constexpr int kFontSize = 1020;
constexpr int kDelete = 1021;
constexpr int kMarkdown = 1022;
constexpr int kSearchLabel = 1024;
constexpr int kHeading = 1025;
constexpr int kViewButton = 1040;
constexpr int kTrashInfo = 1041;
constexpr int kContextOpen = 1101;
constexpr int kContextEdit = 1102;
constexpr int kContextDelete = 1103;
constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kClipTimer = 2;
constexpr UINT_PTR kSyncTimer = 3;
constexpr UINT_PTR kTrashTimer = 4;
constexpr UINT_PTR kSignalTimer = 5;
constexpr UINT kSignalRequest = WM_APP + 8;
constexpr UINT kSignalComplete = WM_APP + 9;
constexpr UINT kRendererEdit = WM_APP + 1;
constexpr UINT kRendererSave = WM_APP + 2;
constexpr UINT kRendererShowList = WM_APP + 3;
constexpr UINT kRendererClip = WM_APP + 6;
constexpr UINT kRendererDelete = WM_APP + 4;
HICON g_app_icon = nullptr;
HICON g_app_icon_small = nullptr;
bool g_owns_app_icon = false;
bool g_owns_app_icon_small = false;

std::vector<tcard::CardRecord> g_cards;
std::vector<tcard::TrashRecord> g_trash_records;
std::vector<tcard::CardRecord> g_trash_cards;
bool g_trash_mode=false;
HWND g_view_button=nullptr,g_trash_info=nullptr;
std::wstring g_view_queries[2],g_view_ids[2];
void card_set_view(bool trash);
struct CardWindow {
    TCARD_WUI_HOST handle = nullptr;
    std::wstring id;
    uint64_t generation = 0;
    bool sizing = false;
    bool moving = false;
};
std::vector<std::unique_ptr<CardWindow>> g_card_windows;
uint64_t g_next_generation = 1;
HWND g_main = nullptr;
HWND g_list = nullptr;
HWND g_search = nullptr;
HWND g_preview = nullptr;
HWND g_title_editor = nullptr;
HWND g_title_line = nullptr;
HWND g_source_editor = nullptr;
HWND g_detail_title = nullptr;
HWND g_empty_state = nullptr;
HWND g_clear_search = nullptr;
HWND g_font_family_editor = nullptr;
HWND g_font_size_editor = nullptr;
HWND g_markdown = nullptr;
HMODULE g_renderer = nullptr;
HMODULE g_rich_edit = nullptr;
bool g_editing = false;
bool g_main_moving = false;
bool g_editor_markdown=false;
bool g_appearance_open = false;
decltype(&TCardWuiExecuteCommand) g_execute_card=nullptr;
decltype(&TCardWuiCreateHost) g_create = nullptr;
decltype(&TCardWuiDestroyHost) g_destroy = nullptr;
decltype(&TCardWuiSetState) g_set_state = nullptr;
decltype(&TCardWuiShow) g_show = nullptr;
decltype(&TCardWuiGetWindow) g_get_window = nullptr;
decltype(&TCardWuiSetCommandCallback) g_set_callback = nullptr;
decltype(&TCardWuiCreateCard) g_create_card = nullptr;
decltype(&TCardWuiDestroyCard) g_destroy_card = nullptr;
decltype(&TCardWuiSetCardState) g_set_card_state = nullptr;
decltype(&TCardWuiSetCardTextState) g_set_card_text_state = nullptr;
decltype(&TCardWuiSetCardAssetRoot) g_set_card_asset_root = nullptr;
decltype(&TCardWuiShowCard) g_show_card = nullptr;
decltype(&TCardWuiGetCardWindow) g_get_card_window = nullptr;
decltype(&TCardWuiGetCardState) g_get_card_state = nullptr;
decltype(&TCardWuiGetCardTextState) g_get_card_text_state = nullptr;
decltype(&TCardWuiSetCardCommandCallback) g_set_card_callback = nullptr;
decltype(&TCardWuiSetCardSaveCallback) g_set_card_save_callback = nullptr;
decltype(&TCardWuiIsCardEditing) g_is_card_editing = nullptr;
std::wstring g_file_path;
unsigned g_history_limit = 0;
tcard_bridge::Client g_sync_client;
// UI-thread admission flag grants the background job exclusive Client/native writer ownership.
bool g_signal_admitting=false;
bool g_storage_locked=false;
bool g_key_active=false;
std::wstring g_signal_status;
bool card_signal_busy(){return g_signal_admitting;}
struct TrashTask {
    std::atomic_bool done{false};
    bool result=false;
    std::jthread worker;
};
std::unique_ptr<TrashTask> g_trash_task;
void card_purge_trash(HWND hwnd)
{
    if(card_signal_busy())return;
    if(g_trash_task){
        if(!g_trash_task->done.load())return;
        const bool okay=g_trash_task->result;g_trash_task.reset();
        SetTimer(hwnd,kTrashTimer,okay?60*60*1000:60*1000,nullptr);if(g_trash_mode)card_set_view(true);return;
    }
    if(!IsWindowEnabled(hwnd)||g_appearance_open||g_editing)return;
    try{
        auto task=std::make_unique<TrashTask>();auto* state=task.get();const auto path=g_file_path;
        task->worker=std::jthread([state,path]{
            const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            if(SUCCEEDED(com)){
                try{state->result=tcard::PurgeTrash(path);}catch(...){state->result=false;}
                CoUninitialize();
            }
            state->done.store(true);
        });
        g_trash_task=std::move(task);SetTimer(hwnd,kTrashTimer,60*1000,nullptr);
    }catch(...){g_trash_task.reset();}
}
bool card_save_records(std::vector<tcard::CardRecord>& cards,bool checkpoint=true)
{
    return !g_storage_locked&&!card_signal_busy()&&tcard::SaveCards(g_file_path,cards,checkpoint,g_history_limit);
}
COLORREF g_preview_color = RGB(255, 245, 168);
HBRUSH g_preview_brush = nullptr;
HFONT g_ui_font = nullptr;
HFONT g_button_font = nullptr;
HFONT g_heading_font = nullptr;
HFONT g_detail_title_font = nullptr;
HFONT g_tile_title_font = nullptr;
HFONT g_tile_excerpt_font = nullptr;
std::vector<size_t> g_visible_indices;
std::wstring g_detail_title_family;
double g_detail_title_size = -1.0;

const wchar_t* tcard_text(const wchar_t* key, const wchar_t* fallback)
{
    static thread_local std::wstring value;
    value = tcard_lang::text(key, fallback);
    return value.c_str();
}

const wchar_t* tcard_color_name(int id)
{
    switch (id) {
    case kColorYellow: return tcard_text(L"color.yellow", L"Yellow");
    case kColorGreen: return tcard_text(L"color.green", L"Green");
    case kColorBlue: return tcard_text(L"color.blue", L"Blue");
    case kColorPink: return tcard_text(L"color.pink", L"Pink");
    case kColorLavender: return tcard_text(L"color.lavender", L"Lavender");
    case kColorNeutral: return tcard_text(L"color.neutral", L"Neutral");
    default: return L"";
    }
}

void refresh_language_ui(HWND hwnd);
LRESULT selected_index();

struct ColorChoice {
    int id;
    COLORREF color;
    const wchar_t* value;
    const wchar_t* name;
};

constexpr ColorChoice kColorChoices[] = {
    { kColorYellow, RGB(255, 245, 168), L"#FFF5A8", L"Yellow" },
    { kColorGreen, RGB(217, 242, 217), L"#D9F2D9", L"Green" },
    { kColorBlue, RGB(205, 235, 255), L"#CDEBFF", L"Blue" },
    { kColorPink, RGB(244, 213, 232), L"#F4D5E8", L"Pink" },
    { kColorLavender, RGB(233, 221, 247), L"#E9DDF7", L"Lavender" },
    { kColorNeutral, RGB(243, 241, 235), L"#F3F1EB", L"Neutral" },
};

bool card_window_live(const CardWindow* target)
{
    if (!target || !target->handle || !g_get_card_window || target->generation == 0) return false;
    for (const auto& entry : g_card_windows) {
        if (entry.get() == target && entry->generation == target->generation)
            return g_get_card_window(target->handle) != nullptr;
    }
    return false;
}

void set_editing(bool editing);

void WINAPI renderer_command(UINT command, void* context)
{
    if (command >= 6 && command <= 9 && g_main) { SendMessageW(g_main, kRendererClip, command, reinterpret_cast<LPARAM>(context)); return; }
    if (command == 1 && g_main) SendMessageW(g_main, kRendererEdit, reinterpret_cast<WPARAM>(context), 0);
    else if (command == 4 && g_main) SendMessageW(g_main, kRendererShowList, reinterpret_cast<WPARAM>(context), 0);
    else if (command == 5 && g_main) SendMessageW(g_main, kRendererDelete, reinterpret_cast<WPARAM>(context), 0);
}

BOOL WINAPI renderer_save(void* context)
{
    return g_main ? static_cast<BOOL>(SendMessageW(g_main, kRendererSave, reinterpret_cast<WPARAM>(context), 0)) : FALSE;
}

std::wstring module_dir()
{
    std::wstring path(260, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"." : path.substr(0, slash);
        }
        path.resize(path.size() * 2);
    }
}

void card_focus_window(HWND window)
{
    if(!window||!IsWindow(window))return;
    ShowWindow(window,IsIconic(window)?SW_RESTORE:SW_SHOW);
    const HWND popup=GetLastActivePopup(window);
    SetForegroundWindow(popup&&IsWindowVisible(popup)?popup:window);
}

std::wstring card_resolve_owner()
{
    const auto base=module_dir();if(base.empty())return {};
    const auto root=base+L"\\tcard\\cards";bool existing=true;
    HANDLE directoryHandle=CreateFileW(root.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directoryHandle==INVALID_HANDLE_VALUE){
        const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return {};
        existing=false;directoryHandle=CreateFileW(base.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    }
    tcard_asset::File directory(directoryHandle);if(!directory)return {};
    const auto size=GetFinalPathNameByHandleW(directory.value,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size)return {};std::wstring path(size,L'\0');
    const auto copied=GetFinalPathNameByHandleW(directory.value,path.data(),size,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!copied||copied>=size)return {};path.resize(copied);
    if(!existing)path+=L"\\tcard\\cards";
    for(auto& ch:path)ch=static_cast<wchar_t>(towlower(ch));return path;
}

struct CardOwner {
    HANDLE mutex=nullptr,activation=nullptr;
    std::wstring name;
    bool owned=false;
    ~CardOwner(){
        if(owned)ReleaseMutex(mutex);
        if(activation)CloseHandle(activation);
        if(mutex)CloseHandle(mutex);
    }
    int acquire()
    {
        const auto root=card_resolve_owner();if(root.empty())return -1;
        name=L"Local\\TCard.Owner."+tcard_asset::hash(tcard_asset::encode(root));
        mutex=CreateMutexW(nullptr,FALSE,name.c_str());if(!mutex)return -1;
        // Open the shared event before claiming ownership, so an early relaunch cannot lose its signal.
        activation=CreateEventW(nullptr,FALSE,FALSE,(name+L".activate").c_str());if(!activation)return -1;
        const auto locked=WaitForSingleObject(mutex,0);
        owned=locked==WAIT_OBJECT_0||locked==WAIT_ABANDONED;
        if(!owned&&locked!=WAIT_TIMEOUT)return -1;
        if(owned)return 1;
        EnumWindows([](HWND window,LPARAM data)->BOOL{
            const auto* owner=reinterpret_cast<CardOwner*>(data);
            if(GetPropW(window,owner->name.c_str())){
                DWORD pid=0;GetWindowThreadProcessId(window,&pid);AllowSetForegroundWindow(pid);
                const auto popup=GetLastActivePopup(window);
                if(popup&&IsWindowVisible(popup))SetForegroundWindow(popup);return FALSE;
            }
            return TRUE;
        },reinterpret_cast<LPARAM>(this));
        return SetEvent(activation)?0:-1;
    }
};

UINT refresh_interval_ms()
{
    const std::wstring cardIni = module_dir() + L"\\TCard.ini";
    wchar_t configured[32]{};
    GetPrivateProfileStringW(L"TCard", L"RefreshSeconds", L"1", configured, ARRAYSIZE(configured), cardIni.c_str());
    unsigned long seconds = wcstoul(configured, nullptr, 10);
    if (seconds < 1) seconds = 1;
    if (seconds > 3600) seconds = 3600;
    return static_cast<UINT>(seconds * 1000UL);
}

void tcard_initialize_defaults()
{
    const std::wstring ini = module_dir() + L"\\TCard.ini";
    HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const wchar_t defaults[] = L"\ufeff[TCard]\r\nRefreshSeconds=1\r\nDefaultFontFamily=Yu Gothic UI\r\nDefaultFontSize=10\r\nTitleFontScale=1.6\r\nTitleBold=1\r\nTClockIni=..\\tclock-win11.ini\r\n";
    const DWORD bytes = static_cast<DWORD>(sizeof(defaults) - sizeof(wchar_t));
    DWORD written = 0;
    const BOOL complete = WriteFile(file, defaults, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    if (!complete) DeleteFileW(ini.c_str());
}

void tcard_apply_defaults(tcard::CardRecord& card)
{
    const auto& defaults=tcard_fonts::defaults();
    card.fontFamily=defaults.family;card.fontSize=defaults.size;
    card.inheritFontFamily=true;card.inheritFontSize=true;
}

tcard_fonts::Defaults tcard_resolve_font(const tcard::CardRecord& card)
{
    const auto& defaults=tcard_fonts::defaults();
    return {card.inheritFontFamily?defaults.family:card.fontFamily,card.inheritFontSize?defaults.size:card.fontSize};
}

std::wstring resolve_tclock_ini()
{
    const std::wstring cardIni = module_dir() + L"\\TCard.ini";
    wchar_t configured[32768]{};
    GetPrivateProfileStringW(L"TCard", L"TClockIni", L"..\\tclock-win11.ini", configured, ARRAYSIZE(configured), cardIni.c_str());
    std::wstring value(configured);
    if (value.size() > 1 && value[1] == L':') return value;
    if (value.rfind(L"\\\\", 0) == 0 || value.rfind(L"//", 0) == 0) return value;
    const size_t slash = cardIni.find_last_of(L"\\/");
    return slash == std::wstring::npos ? value : cardIni.substr(0, slash + 1) + value;
}

COLORREF parse_color(const std::wstring& value)
{
    wchar_t* end = nullptr;
    const wchar_t* text = value.c_str();
    if (*text == L'#') ++text;
    unsigned long number = wcstoul(text, &end, 16);
    if (!end || *end || (end - text) != 6) return RGB(255, 245, 168);
    return RGB((number >> 16) & 0xff, (number >> 8) & 0xff, number & 0xff);
}

bool load_renderer()
{
    if (g_renderer) return true;
    const std::wstring base = module_dir();
    const std::wstring candidates[] = { base + L"\\tcwui-card.dll", base + L"\\..\\tcwui-card.dll" };
    for (const auto& candidate : candidates) {
        g_renderer = LoadLibraryW(candidate.c_str());
        if (g_renderer) break;
    }
    if (!g_renderer) return false;
    g_execute_card=reinterpret_cast<decltype(g_execute_card)>(GetProcAddress(g_renderer,"TCardWuiExecuteCommand"));
    g_create = reinterpret_cast<decltype(g_create)>(GetProcAddress(g_renderer, "TCardWuiCreateHost"));
    g_destroy = reinterpret_cast<decltype(g_destroy)>(GetProcAddress(g_renderer, "TCardWuiDestroyHost"));
    g_set_state = reinterpret_cast<decltype(g_set_state)>(GetProcAddress(g_renderer, "TCardWuiSetState"));
    g_show = reinterpret_cast<decltype(g_show)>(GetProcAddress(g_renderer, "TCardWuiShow"));
    g_get_window = reinterpret_cast<decltype(g_get_window)>(GetProcAddress(g_renderer, "TCardWuiGetWindow"));
    g_set_callback = reinterpret_cast<decltype(g_set_callback)>(GetProcAddress(g_renderer, "TCardWuiSetCommandCallback"));
    g_create_card = reinterpret_cast<decltype(g_create_card)>(GetProcAddress(g_renderer, "TCardWuiCreateCard"));
    g_destroy_card = reinterpret_cast<decltype(g_destroy_card)>(GetProcAddress(g_renderer, "TCardWuiDestroyCard"));
    g_set_card_state = reinterpret_cast<decltype(g_set_card_state)>(GetProcAddress(g_renderer, "TCardWuiSetCardState"));
    g_set_card_text_state = reinterpret_cast<decltype(g_set_card_text_state)>(GetProcAddress(g_renderer, "TCardWuiSetCardTextState"));
    g_set_card_asset_root = reinterpret_cast<decltype(g_set_card_asset_root)>(GetProcAddress(g_renderer, "TCardWuiSetCardAssetRoot"));
    g_show_card = reinterpret_cast<decltype(g_show_card)>(GetProcAddress(g_renderer, "TCardWuiShowCard"));
    g_get_card_window = reinterpret_cast<decltype(g_get_card_window)>(GetProcAddress(g_renderer, "TCardWuiGetCardWindow"));
    g_get_card_state = reinterpret_cast<decltype(g_get_card_state)>(GetProcAddress(g_renderer, "TCardWuiGetCardState"));
    g_get_card_text_state = reinterpret_cast<decltype(g_get_card_text_state)>(GetProcAddress(g_renderer, "TCardWuiGetCardTextState"));
    g_set_card_callback = reinterpret_cast<decltype(g_set_card_callback)>(GetProcAddress(g_renderer, "TCardWuiSetCardCommandCallback"));
    g_set_card_save_callback = reinterpret_cast<decltype(g_set_card_save_callback)>(GetProcAddress(g_renderer, "TCardWuiSetCardSaveCallback"));
    g_is_card_editing = reinterpret_cast<decltype(g_is_card_editing)>(GetProcAddress(g_renderer, "TCardWuiIsCardEditing"));
    if (!g_execute_card || !g_create || !g_destroy || !g_set_state || !g_show || !g_get_window || !g_set_callback || !g_create_card || !g_destroy_card || !g_set_card_state || !g_set_card_text_state || !g_set_card_asset_root || !g_show_card || !g_get_card_window || !g_get_card_state || !g_get_card_text_state || !g_set_card_callback || !g_set_card_save_callback || !g_is_card_editing) {
        FreeLibrary(g_renderer);
        g_renderer = nullptr;
        return false;
    }
    return true;
}

void seed_cards_if_empty()
{
    if (!g_cards.empty()) return;
    tcard::CardRecord first;
    tcard_apply_defaults(first);
    first.id = L"welcome";
    first.title = L"Welcome to TCard";
    first.source = L"A native Sticky Notes-like card.\r\n\r\nNow: <%yyyy/mm/dd(ddd) hh:nn:ss%>";
    first.color = L"#FFF5A8";
    g_cards.push_back(first);
    tcard::CardRecord second;
    tcard_apply_defaults(second);
    second.id = L"edit";
    second.title = L"Make yourself a note";
    second.source = L"Double-click a note to place it on your desktop.\r\n\r\nRight-click a detached note to edit it, keep it on top, or return to the list.\r\n\r\nChoose Appearance while editing to change the paper and text.";
    second.color = L"#CDEBFF";
    g_cards.push_back(second);
    tcard::CardRecord fixture;
    tcard_apply_defaults(fixture);
    fixture.id = L"format-check";
    fixture.title = L"Format quick check";
    fixture.source = L"[Clock] <%yyyy/mm/dd hh:nn:ss%>\r\n[Weekday] <%ddd%>\r\n[Locale] <%DATE%> <%TIME%> <%AMPM%> <%AM/PM%>\r\n[Offset] <%w+01hh:nn%> | <%td-01:30hh:nn%>\r\n[System] CPU <%CU%> | Memory <%MAPM%> | Uptime <%ST%>\r\n[Power] Battery <%BL%> | Cores <%PCORE%>\r\n[Custom] <%CUSTOM1%>\r\n[Escaped] literal <%\"yyyy\"%> | line <%yyyy\\nmm%>\r\n[Fallback] <%UNSUPPORTED%>";
    fixture.color = L"#E9DDF7";
    g_cards.push_back(fixture);
    card_save_records(g_cards);
}

std::wstring window_text(HWND window);

void update_vertical_scroll(HWND control)
{
    tcard_ui::update_scroll(control);
}

std::wstring note_excerpt(const std::wstring& source)
{
    std::wstring result;
    result.reserve(90);
    bool previous_space = false;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source.compare(i, 13, L"](data:image/") == 0) {
            const size_t end = source.find(L')', i + 13);
            if (end != std::wstring::npos) { i = end; continue; }
        }
        wchar_t ch = source[i];
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (ch == L' ') {
            if (previous_space) continue;
            previous_space = true;
        } else {
            previous_space = false;
        }
        result.push_back(ch);
        if (result.size() >= 86) { result.resize(86); result += L"..."; break; }
    }
    return result;
}

std::wstring fold_text(const std::wstring& value)
{
    std::wstring folded = value;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return folded;
}

bool contains_text(const std::wstring& value, const std::wstring& query)
{
    return query.empty() || fold_text(value).find(fold_text(query)) != std::wstring::npos;
}

const ColorChoice* color_choice(int id)
{
    for (const auto& choice : kColorChoices) if (choice.id == id) return &choice;
    return nullptr;
}

double parse_font_size(const std::wstring& value, double fallback)
{
    wchar_t* end = nullptr;
    const double parsed = wcstod(value.c_str(), &end);
    if (!end || end == value.c_str() || *end != L'\0' || parsed != parsed) return fallback;
    return std::clamp(parsed, 8.0, 48.0);
}

int title_height()
{
    HDC dc = GetDC(g_main);
    if (!dc) return 32;
    HGDIOBJ old = SelectObject(dc, g_detail_title_font ? g_detail_title_font : g_heading_font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(g_main, dc);
    return max(28, metrics.tmHeight + metrics.tmExternalLeading + 8);
}

std::wstring format_font_size(double value)
{
    wchar_t text[32]{};
    swprintf_s(text, ARRAYSIZE(text), L"%.1f", std::clamp(value, 8.0, 48.0));
    return text;
}

std::wstring color_hex(COLORREF color)
{
    wchar_t value[8]{};
    swprintf_s(value, ARRAYSIZE(value), L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return value;
}

COLORREF color_shade(COLORREF color, float factor)
{
    const int red = min(255, max(0, static_cast<int>(GetRValue(color) * factor)));
    const int green = min(255, max(0, static_cast<int>(GetGValue(color) * factor)));
    const int blue = min(255, max(0, static_cast<int>(GetBValue(color) * factor)));
    return RGB(red, green, blue);
}

void update_detail_title_font(const tcard::CardRecord& card)
{
    const auto font=tcard_resolve_font(card);
    const std::wstring family = font.family.empty() ? L"Segoe UI" : font.family;
    if (g_detail_title_font && g_detail_title_family == family && g_detail_title_size == font.size) return;
    if (g_detail_title_font) DeleteObject(g_detail_title_font);
    const UINT dpi = GetDpiForWindow(g_main);
    const int points = static_cast<int>(font.size * 1.6 + 0.5);
    const int height = -MulDiv(max(8, points), static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
    g_detail_title_font = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family.c_str());
    g_detail_title_family = family;
    g_detail_title_size = font.size;
    if (g_detail_title_font && g_detail_title) SendMessageW(g_detail_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_detail_title_font), TRUE);
}

COLORREF tile_ink(COLORREF paper)
{
    const int luminance = (299 * GetRValue(paper) + 587 * GetGValue(paper) + 114 * GetBValue(paper)) / 1000;
    return luminance < 150 ? RGB(245, 243, 236) : RGB(36, 36, 36);
}

const std::wstring& card_excerpt(const tcard::CardRecord& card)
{
    struct Entry {std::wstring id,source,text;bool rich=false;};
    static std::vector<Entry> cache;static std::wstring uncached;
    const auto found=std::find_if(cache.begin(),cache.end(),[&](const Entry& e){return e.id==card.id&&e.source==card.source&&e.rich==card.richHtml;});
    if(found!=cache.end()){std::rotate(cache.begin(),found,found+1);return cache.front().text;}
    Entry entry{card.id,card.source,note_excerpt(card.richHtml?tcard_rich::text(tcard_rich::parse(card.source)):card.source),card.richHtml};
    const auto size=[](const Entry& e){return (e.id.size()+e.source.size()+e.text.size())*sizeof(wchar_t);};
    constexpr size_t limit=4*1024*1024;size_t bytes=size(entry);
    if(bytes>limit){uncached=std::move(entry.text);return uncached;}
    for(auto it=cache.begin();it!=cache.end();)if(it->id==card.id)it=cache.erase(it);else ++it;
    for(const auto& item:cache)bytes+=size(item);
    while(!cache.empty()&&(cache.size()>=128||bytes>limit)){bytes-=size(cache.back());cache.pop_back();}
    cache.insert(cache.begin(),std::move(entry));return cache.front().text;
}

const std::vector<tcard::CardRecord>& card_list()
{
    return g_trash_mode?g_trash_cards:g_cards;
}
LRESULT card_view_index()
{
    const auto row=SendMessageW(g_list,LB_GETCURSEL,0,0);
    return row>=0&&static_cast<size_t>(row)<g_visible_indices.size()?static_cast<LRESULT>(g_visible_indices[static_cast<size_t>(row)]):-1;
}
bool card_load_trash()
{
    std::vector<tcard::TrashRecord> records;
    if(!tcard::LoadTrash(g_file_path,records))return false;
    g_trash_records=std::move(records);g_trash_cards.clear();
    for(const auto& record:g_trash_records)g_trash_cards.push_back(record.card);
    return true;
}
std::wstring card_describe_trash(size_t index)
{
    const auto& record=g_trash_records[index];
    if(!record.readable)return tcard_text(L"trash.unreadable",L"This retained copy could not be read. Its data has been preserved.");
    if(!record.deletedUtcMs)return tcard_text(L"trash.unknown_date",L"Deletion date unavailable; automatic expiry starts after maintenance.");
    FILETIME utc{};ULARGE_INTEGER value{};value.QuadPart=record.deletedUtcMs*10000ULL;
    utc.dwLowDateTime=value.LowPart;utc.dwHighDateTime=value.HighPart;SYSTEMTIME date{};
    if(!tcard_ui::convert_time(utc,date))return tcard_text(L"trash.unknown_date",L"Deletion date unavailable; automatic expiry starts after maintenance.");
    FILETIME now{};GetSystemTimeAsFileTime(&now);value.LowPart=now.dwLowDateTime;value.HighPart=now.dwHighDateTime;
    const auto nowMs=value.QuadPart/10000ULL;
    constexpr ULONGLONG day=86400000ULL,retention=30*day;
    const auto remaining=nowMs<record.deletedUtcMs?30ULL:nowMs-record.deletedUtcMs>=retention?0ULL:(retention-(nowMs-record.deletedUtcMs)+day-1)/day;
    wchar_t text[256]{};StringCchPrintfW(text,ARRAYSIZE(text),tcard_text(L"trash.details",L"Deleted %04u-%02u-%02u %02u:%02u · %llu days remaining"),date.wYear,date.wMonth,date.wDay,date.wHour,date.wMinute,remaining);
    return text;
}

void draw_list_item(const DRAWITEMSTRUCT& item)
{
    if (!item.hDC || item.itemID == static_cast<UINT>(-1) || item.itemID >= g_visible_indices.size()) return;
    const auto index=g_visible_indices[item.itemID];
    if(index>=card_list().size()||(g_trash_mode&&index>=g_trash_records.size()))return;
    const auto& card = card_list()[index];
    const COLORREF paper = parse_color(card.color);
    const COLORREF ink = tile_ink(paper);
    RECT tile = item.rcItem;
    InflateRect(&tile, -4, -4);
    HBRUSH shell = CreateSolidBrush(color_shade(paper, 0.92f));
    FillRect(item.hDC, &item.rcItem, shell);
    DeleteObject(shell);
    HBRUSH fill = CreateSolidBrush(paper);
    FillRect(item.hDC, &tile, fill);
    DeleteObject(fill);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, ink);
    HFONT previous = static_cast<HFONT>(SelectObject(item.hDC, g_tile_title_font ? g_tile_title_font : g_ui_font));
    RECT title = tile;
    title.left += 12; title.top += 10; title.right -= 12; title.bottom = title.top + 20;
    DrawTextW(item.hDC, card.title.empty() ? tcard_text(L"status.untitled_note", L"Untitled note") : card.title.c_str(), -1, &title, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, g_tile_excerpt_font ? g_tile_excerpt_font : g_ui_font);
    RECT excerpt = tile;
    excerpt.left += 12; excerpt.top += 36; excerpt.right -= 12; excerpt.bottom -= 10;
    if(g_trash_mode)excerpt.bottom-=20;
    const auto& summary=card_excerpt(card);
    DrawTextW(item.hDC, summary.c_str(), -1, &excerpt, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    if(g_trash_mode){RECT details=excerpt;details.top=tile.bottom-28;details.bottom=tile.bottom-6;
        const auto text=card_describe_trash(g_visible_indices[item.itemID]);DrawTextW(item.hDC,text.c_str(),-1,&details,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
    SelectObject(item.hDC, previous);
    if (item.itemState & ODS_SELECTED) {
        HBRUSH accent = CreateSolidBrush(RGB(0, 95, 184));
        FrameRect(item.hDC, &tile, accent);
        DeleteObject(accent);
    }
    if (item.itemState & ODS_FOCUS) {
        RECT focus = tile;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(item.hDC, &focus);
    }
}

bool card_sync_start(HWND owner,const tcard_bridge::Settings& settings,bool startup=false);
bool card_rotate_key(HWND owner,std::wstring& display);
bool card_join_key(HWND owner,const std::wstring& words,std::wstring& display);
bool g_source_dialog=false;
void card_signal_request(bool hosting);
bool card_signal_available();
#include "card_preferences.h"

void fill_list()
{
    const std::wstring query = window_text(g_search);
    const LRESULT current = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    const size_t current_index = current >= 0 && static_cast<size_t>(current) < g_visible_indices.size() ? g_visible_indices[static_cast<size_t>(current)] : 0;
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_visible_indices.clear();
    for (size_t i = 0; i < card_list().size(); ++i) {
        const auto& card = card_list()[i];
        if (!contains_text(card.title, query) && !contains_text(card.source, query)) continue;
        g_visible_indices.push_back(i);
    }
    std::stable_sort(g_visible_indices.begin(), g_visible_indices.end(), [](size_t a, size_t b) {
        const auto& left = card_list()[a]; const auto& right = card_list()[b];
        if(g_trash_mode)return g_trash_records[a].deletedUtcMs>g_trash_records[b].deletedUtcMs;
        int order = 0;
        if (g_sort_field == 2) order = CompareStringOrdinal(left.title.c_str(), -1, right.title.c_str(), -1, TRUE) - CSTR_EQUAL;
        else order = CompareFileTime(g_sort_field == 0 ? &left.createdUtc : &left.updatedUtc, g_sort_field == 0 ? &right.createdUtc : &right.updatedUtc);
        if (!order) return left.id < right.id;
        return g_sort_descending ? order > 0 : order < 0;
    });
    for (size_t index : g_visible_indices) SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(card_list()[index].title.c_str()));
    if (!g_visible_indices.empty()) {
        size_t selected = 0;
        for (size_t i = 0; i < g_visible_indices.size(); ++i) if (g_visible_indices[i] == current_index) { selected = i; break; }
        SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    }
    const bool empty = g_visible_indices.empty();
    if (g_empty_state) {
        SetWindowTextW(g_empty_state, card_list().empty() ? (g_trash_mode?tcard_text(L"trash.empty_state",L"Trash is empty"):tcard_text(L"empty.no_notes", L"No notes yet")) : tcard_text(L"empty.no_match", L"No matching notes"));
        ShowWindow(g_empty_state, empty ? SW_SHOW : SW_HIDE);
    }
    if (g_clear_search) ShowWindow(g_clear_search, empty && !query.empty() ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_list, nullptr, FALSE);
}

void refresh_preview();

LRESULT selected_index();

void populate_font_families(HWND combo)
{
    tcard_fonts::populate(combo,g_main);
}

void select_text_format(HWND combo, bool markdown)
{
    SendMessageW(combo, CB_SETCURSEL, markdown ? 1 : 0, 0);
}

void select_font_family(HWND combo, const std::wstring& family)
{
    tcard_fonts::select(combo,family);
}

std::wstring window_text(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

LRESULT selected_index()
{
    if(g_trash_mode)return -1;
    const LRESULT selected = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    return selected >= 0 && static_cast<size_t>(selected) < g_visible_indices.size() ? static_cast<LRESULT>(g_visible_indices[static_cast<size_t>(selected)]) : -1;
}

void set_editing(bool editing)
{
    if (editing && g_editing) return;
    const LRESULT selected = selected_index();
    if (editing && selected < 0) return;
    if (editing && selected >= 0) {
        const auto& target = g_cards[static_cast<size_t>(selected)];
        for (const auto& entry : g_card_windows) {
            if (entry->id == target.id && entry->handle && IsWindow(g_get_card_window(entry->handle))) {
                // Keep one editor for this ID: an existing card owns its draft.
                if(!g_is_card_editing(entry->handle))g_execute_card(entry->handle,1);
                g_show_card(entry->handle,TRUE);card_focus_window(g_get_card_window(entry->handle));
                return;
            }
        }
        const auto& card = g_cards[static_cast<size_t>(selected)];
        const auto font=tcard_resolve_font(card);
        SetWindowTextW(g_title_editor, card.title.c_str());
        g_editor_markdown=card.markdown;
        tcard_rich_edit::detach(g_source_editor);
        SetWindowTextW(g_source_editor,L"");
        SendMessageW(g_source_editor,EM_SETTEXTMODE,(card.markdown?TM_PLAINTEXT:TM_RICHTEXT)|TM_MULTILEVELUNDO,0);
        if(card.markdown)SetWindowTextW(g_source_editor,card.source.c_str());
        else if(!tcard_rich_edit::load(g_source_editor,card.richHtml?tcard_rich::parse(card.source):tcard_rich::plain(card.source),
          font.size,font.family,parse_color(card.color),RGB(37,37,37),true)){
            MessageBoxW(g_main,tcard_text(L"status.load_failed",L"Could not load; the stored note was kept."),L"TCard",MB_OK|MB_ICONERROR);return;
        }
        SendMessageW(g_source_editor, EM_SETBKGNDCOLOR, 0, parse_color(card.color));
        select_font_family(g_font_family_editor, font.family.empty() ? L"Segoe UI" : font.family);
        tcard_fonts::inheritance(g_font_family_editor,false,card.inheritFontFamily);
        tcard_fonts::select_size(g_font_size_editor,font.size,card.inheritFontSize);
        select_text_format(g_markdown, card.markdown);
        update_vertical_scroll(g_source_editor);
    }
    g_editing = editing;
    g_appearance_open = false;
    SetWindowTextW(GetDlgItem(g_main, kAppearance), tcard_text(L"button.appearance", L"Appearance"));
    for (int id : {1030, 1031, 1032, 1033}) ShowWindow(GetDlgItem(g_main, id), SW_HIDE);
    EnableWindow(g_view_button,!editing);
    EnableWindow(g_list, !editing);
    EnableWindow(g_search, !editing);
    EnableWindow(GetDlgItem(g_main, kNew), !editing);
    EnableWindow(GetDlgItem(g_main, kDelete), !editing);
    ShowWindow(GetDlgItem(g_main, kOpen), editing ? SW_HIDE : SW_SHOW);
    ShowWindow(g_preview, editing ? SW_HIDE : SW_SHOW);
    ShowWindow(g_detail_title, editing ? SW_HIDE : SW_SHOW);
    ShowWindow(g_title_editor, editing ? SW_SHOW : SW_HIDE);
    ShowWindow(g_source_editor, editing ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_main, kEdit), editing ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(g_main, kSave), editing ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_main, kCancel), editing ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_main, kAppearance), editing ? SW_SHOW : SW_HIDE);
    ShowWindow(g_font_family_editor, SW_HIDE);
    ShowWindow(g_font_size_editor, SW_HIDE);
    ShowWindow(g_markdown, SW_HIDE);
    for (const auto& choice : kColorChoices) ShowWindow(GetDlgItem(g_main, choice.id), SW_HIDE);
    ShowWindow(g_title_line, SW_SHOW);
    if (editing) SetFocus(g_source_editor);
    RECT client{};
    GetClientRect(g_main, &client);
    SendMessageW(g_main, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    if (!editing) refresh_preview();
}

void save_edit()
{
    const LRESULT selected = selected_index();
    if (!g_editing || selected < 0) return;
    double validatedSize = 0;
    if (!tcard_fonts::read_size(g_font_size_editor, validatedSize)) {
        if (!g_appearance_open) SendMessageW(g_main, WM_COMMAND, kAppearance, 0);
        MessageBoxW(g_main, tcard_text(L"message.font_size", L"Enter a font size from 8 to 48."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONWARNING);
        SetFocus(g_font_size_editor);
        SendMessageW(g_font_size_editor, EM_SETSEL, 0, -1);
        return;
    }
    auto& card = g_cards[static_cast<size_t>(selected)];
    const auto previous = card;
    card.color = color_hex(g_preview_color);
    const std::wstring title = window_text(g_title_editor);
    card.title = title.empty() ? tcard_text(L"status.untitled_card", L"Untitled card") : title;
    bool readable=true;
    const auto document=g_editor_markdown?tcard_rich::Document{}:tcard_rich_edit::document(g_source_editor,false,&readable);
    if(!readable){card=previous;MessageBoxW(g_main,tcard_text(L"message.save_failed",L"Could not read the draft; it remains open."),L"TCard",MB_OK|MB_ICONERROR);return;}
    const bool requestedMarkdown=SendMessageW(g_markdown,CB_GETCURSEL,0,0)==1;
    if(requestedMarkdown&&!g_editor_markdown&&MessageBoxW(g_main,tcard_text(L"message.convert_markdown",L"Switch to Markdown? Text formatting will become plain text."),L"TCard",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK){
        card=previous;select_text_format(g_markdown,g_editor_markdown);return;
    }
    if(requestedMarkdown)card.source=g_editor_markdown?window_text(g_source_editor):tcard_rich::text(document);
    else card.source=tcard_rich::serialize(g_editor_markdown?tcard_rich::plain(window_text(g_source_editor)):document);
    const std::wstring family = tcard_fonts::resolved_family(g_font_family_editor);
    card.fontFamily = family.empty() ? L"Segoe UI" : family;
    card.fontSize = validatedSize;
    card.inheritFontFamily=tcard_fonts::inherited(g_font_family_editor);
    card.inheritFontSize=tcard_fonts::inherited(g_font_size_editor);
    card.markdown = requestedMarkdown;
    card.richHtml = !requestedMarkdown;
    std::vector<tcard::CardRecord> changed{card};
    if (!card_save_records(changed)) {
        card = previous;
        MessageBoxW(g_main, tcard_text(L"message.save_failed", L"Save failed; the draft remains open."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    card=changed[0];
    fill_list();
    for (size_t row = 0; row < g_visible_indices.size(); ++row) {
        if (g_visible_indices[row] == static_cast<size_t>(selected)) {
            SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
            break;
        }
    }
    set_editing(false);
    refresh_preview();
}

void refresh_preview()
{
    if (g_editing) return;
    const LRESULT selected = card_view_index();
    if (selected < 0 || static_cast<size_t>(selected) >= card_list().size()) {
        SetWindowTextW(g_detail_title, L"");SetWindowTextW(g_trash_info,L"");
        for (int id : {kEdit, kOpen, kDelete}) EnableWindow(GetDlgItem(g_main, id), FALSE);
        SetWindowTextW(g_preview, tcard_text(L"status.select_card", L"Select a card"));
        if(g_trash_mode)EnableWindow(GetDlgItem(g_main,kDelete),FALSE);
        return;
    }
    for (int id : {kEdit, kOpen, kDelete}) EnableWindow(GetDlgItem(g_main, id), TRUE);
    const auto& card = card_list()[static_cast<size_t>(selected)];
    if(g_trash_mode){
        const auto index=static_cast<size_t>(selected);const auto text=card_describe_trash(index);if(window_text(g_trash_info)!=text)SetWindowTextW(g_trash_info,text.c_str());
        const bool readable=g_trash_records[index].readable;
        EnableWindow(GetDlgItem(g_main,kOpen),readable);EnableWindow(GetDlgItem(g_main,kEdit),readable);
        EnableWindow(GetDlgItem(g_main,kDelete),FALSE);
    }
    const auto font=tcard_resolve_font(card);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wstring rendered = card.live && !g_trash_mode ? (card.richHtml?tcard::RenderRich(card.source,now):tcard::Render(card.source, now)) : card.source;
    static std::wstring previousText, previousTitle, previousFamily, previousAssetRoot;
    static double previousSize = -1.0;
    static bool previousMarkdown = false,previousRich=false;
    const COLORREF previewColor = parse_color(card.color);
    const bool colorChanged = previewColor != g_preview_color || !g_preview_brush;
    const bool fontChanged = previousSize != font.size || previousFamily != font.family;
    const auto assetRoot = g_trash_mode?g_trash_records[static_cast<size_t>(selected)].directory:tcard::CardDirectory(g_file_path, card.id);
    const bool textChanged = previousText != rendered || previousAssetRoot != assetRoot || (!card.richHtml&&!tcard_md::matches(g_preview, rendered))||previousRich!=card.richHtml;
    const bool titleChanged = previousTitle != card.title;
    if (!colorChanged && !fontChanged && !textChanged && !titleChanged && previousMarkdown == card.markdown) return;
    if (previewColor != g_preview_color || !g_preview_brush) {
        if (g_preview_brush) DeleteObject(g_preview_brush);
        g_preview_color = previewColor;
        g_preview_brush = CreateSolidBrush(g_preview_color);
    }
    CHARRANGE selection{};POINT scroll{};
    SendMessageW(g_preview,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    SendMessageW(g_preview,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    const bool redraw=(GetWindowLongPtrW(g_preview,GWL_STYLE)&WS_VISIBLE)!=0;
    if(redraw)SendMessageW(g_preview,WM_SETREDRAW,FALSE,0);
    if (g_rich_edit && g_preview) SendMessageW(g_preview, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(g_preview_color));
    update_detail_title_font(card);
    RECT client{};
    if (fontChanged && GetClientRect(g_main, &client)) SendMessageW(g_main, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    SetWindowTextW(g_detail_title, card.title.c_str());
    if(card.richHtml)tcard_rich_edit::load_preview(g_preview,rendered,font.size,font.family,previewColor,RGB(37,37,37));
    else{
        tcard_rich_edit::detach(g_preview);
        tcard_md::render(g_preview, rendered, card.markdown, static_cast<float>(font.size), font.family, assetRoot, previewColor);
        tcard_rich_edit::configure(g_preview,false,font.size,font.family,previewColor,RGB(37,37,37),false);
    }
    SendMessageW(g_preview,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
    SendMessageW(g_preview,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    update_vertical_scroll(g_preview);
    if(redraw){SendMessageW(g_preview,WM_SETREDRAW,TRUE,0);RedrawWindow(g_preview,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME);}
    if (colorChanged) RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    previousText = rendered; previousAssetRoot = assetRoot;
    previousTitle = card.title;
    previousFamily = font.family;
    previousSize = font.size;
    previousMarkdown = card.markdown;previousRich=card.richHtml;
}

void card_set_view(bool trash)
{
    if(g_editing)return;
    const bool changed=g_trash_mode!=trash;
    if(changed){
        const auto index=card_view_index();
        g_view_queries[g_trash_mode?1:0]=window_text(g_search);
        g_view_ids[g_trash_mode?1:0]=index>=0?(g_trash_mode?g_trash_records[static_cast<size_t>(index)].token:card_list()[static_cast<size_t>(index)].id):L"";
        // LB_SETCURSEL can synchronously draw the old row; keep its original model active.
        SendMessageW(g_list,LB_SETCURSEL,static_cast<WPARAM>(-1),0);
    }
    g_trash_mode=trash;
    if(trash&&!card_load_trash()){
        g_trash_records.clear();g_trash_cards.clear();
        MessageBoxW(g_main,tcard_text(L"trash.load_failed",L"Could not read the trash. Existing data was preserved."),L"TCard",MB_OK|MB_ICONERROR);
    }
    if(changed){
        SetWindowTextW(g_search,g_view_queries[trash?1:0].c_str());
    }
    SetWindowTextW(g_view_button,trash?tcard_text(L"trash.back",L"Notes"):tcard_text(L"trash.tab",L"Trash"));
    SetWindowTextW(GetDlgItem(g_main,kHeading),trash?tcard_text(L"trash.tab",L"Trash"):tcard_text(L"sidebar.heading",L"Notes"));
    InvalidateRect(g_view_button,nullptr,FALSE);
    SetWindowTextW(GetDlgItem(g_main,kOpen),trash?tcard_text(L"trash.restore",L"Restore"):tcard_text(L"button.open",L"Pop out"));
    SetWindowTextW(GetDlgItem(g_main,kEdit),trash?tcard_text(L"trash.remove",L"Delete permanently"):tcard_text(L"button.edit",L"Edit"));
    SetWindowTextW(GetDlgItem(g_main,kDelete),tcard_text(L"button.delete",L"Delete"));
    ShowWindow(g_trash_info,trash?SW_SHOW:SW_HIDE);EnableWindow(GetDlgItem(g_main,kSortField),!trash);
    fill_list();
    if(changed){
        const auto& id=g_view_ids[trash?1:0];
        for(size_t row=0;row<g_visible_indices.size();++row)if((trash?g_trash_records[g_visible_indices[row]].token:card_list()[g_visible_indices[row]].id)==id){SendMessageW(g_list,LB_SETCURSEL,row,0);break;}
    }
    refresh_preview();RECT client{};GetClientRect(g_main,&client);
    SendMessageW(g_main,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));
}
void card_restore_trash()
{
    if(card_signal_busy())return;
    const auto index=card_view_index();if(!g_trash_mode||index<0)return;
    tcard::CardRecord restored;
    std::vector<tcard::CardRecord> loaded;
    if(!tcard::RestoreTrash(g_file_path,g_trash_records[static_cast<size_t>(index)].token,restored)||!tcard::LoadCardSnapshot(g_file_path,loaded)){
        MessageBoxW(g_main,tcard_text(L"trash.restore_failed",L"Could not restore this copy. It may have changed or already been restored. Refresh the trash and retry."),L"TCard",MB_OK|MB_ICONERROR);
        card_set_view(true);return;
    }
    g_cards=std::move(loaded);g_view_queries[0].clear();card_set_view(false);SetWindowTextW(g_search,L"");fill_list();
    for(size_t row=0;row<g_visible_indices.size();++row)if(g_cards[g_visible_indices[row]].id==restored.id){SendMessageW(g_list,LB_SETCURSEL,row,0);break;}
    refresh_preview();
}
void card_remove_trash(bool all)
{
    if(card_signal_busy())return;
    const auto index=card_view_index();if(!g_trash_mode||(!all&&index<0))return;
    std::vector<std::wstring> tokens;
    if(all){for(const auto& record:g_trash_records)if(record.readable)tokens.push_back(record.token);}
    else tokens.push_back(g_trash_records[static_cast<size_t>(index)].token);
    if(tokens.empty())return;
    const auto message=all?tcard_text(L"trash.confirm_empty",L"Permanently delete all readable copies in this device's trash? Deleted-note synchronization records are retained."):
        tcard_text(L"trash.confirm_remove",L"Permanently delete this retained copy on this device? Deleted-note synchronization records are retained.");
    if(MessageBoxW(g_main,message,L"TCard",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
    bool okay=true;for(const auto& token:tokens)if(!tcard::RemoveTrash(g_file_path,token))okay=false;
    if(!okay)MessageBoxW(g_main,tcard_text(L"trash.remove_failed",L"Some copies could not be deleted. Remaining data was preserved."),L"TCard",MB_OK|MB_ICONERROR);
    card_set_view(true);
}


bool delete_card_id(const std::wstring& id)
{
    if (card_signal_busy() || g_editing || id.empty()) return false;
    const auto card = std::find_if(g_cards.begin(), g_cards.end(), [&](const auto& value) { return value.id == id; });
    if (card == g_cards.end()) return false;
    std::wstring prompt = tcard_lang::text(L"message.delete_prompt", L"Delete '%s'?");
    const size_t marker = prompt.find(L"%s");
    if (marker != std::wstring::npos) prompt.replace(marker, 2, card->title.empty() ? tcard_text(L"status.untitled_card", L"Untitled card") : card->title);
    if (MessageBoxW(g_main, prompt.c_str(), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;
    if (!tcard::DeleteCard(g_file_path, id)) {
        MessageBoxW(g_main, tcard_text(L"message.delete_failed", L"Delete failed; the note was kept."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONERROR);
        return false;
    }
    for (auto it = g_card_windows.begin(); it != g_card_windows.end(); ++it) {
        if ((*it)->id == id) {
            if ((*it)->handle && g_destroy_card) g_destroy_card((*it)->handle);
            g_card_windows.erase(it);
            break;
        }
    }
    g_cards.erase(card);
    fill_list();
    refresh_preview();
    return true;
}

void delete_selected()
{
    const LRESULT selected = selected_index();
    if (selected >= 0 && static_cast<size_t>(selected) < g_cards.size())
        delete_card_id(g_cards[static_cast<size_t>(selected)].id);
}

void apply_card_state(CardWindow* entry, const tcard::CardRecord& card)
{
    const auto font=tcard_resolve_font(card);
    if (!entry || !entry->handle) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wstring rendered = card.live ? (card.richHtml?tcard::RenderRich(card.source,now):tcard::Render(card.source, now)) : card.source;
    TCARD_WUI_TEXT_STATE state{};
    state.cb = sizeof(state);
    state.version = TCARD_WUI_STATE_ABI_VERSION;
    state.backColor = parse_color(card.color);
    state.textColor = RGB(37, 37, 37);
    state.secondaryColor = RGB(102, 102, 102);
    state.title = card.title.c_str();
    state.titleLength = static_cast<DWORD>(card.title.size());
    state.text = rendered.c_str();
    state.textLength = static_cast<DWORD>(rendered.size());
    state.source = card.source.c_str();
    state.sourceLength = static_cast<DWORD>(card.source.size());
    state.fontFamily = font.family.empty() ? L"Segoe UI" : font.family.c_str();
    state.fontFamilyLength = static_cast<DWORD>(font.family.empty() ? wcslen(L"Segoe UI") : font.family.size());
    state.fontSize = static_cast<FLOAT>(font.size);
    state.inheritFontFamily=card.inheritFontFamily;state.inheritFontSize=card.inheritFontSize;
    state.markdown = card.markdown ? TRUE : FALSE;
    state.richHtml=card.richHtml;
    const auto assetRoot = tcard::CardDirectory(g_file_path, card.id);
    if (g_set_card_asset_root) g_set_card_asset_root(entry->handle, assetRoot.c_str(), static_cast<DWORD>(assetRoot.size()));
    g_set_card_text_state(entry->handle, &state);
}

bool card_is_moving()
{
    if(g_main_moving)return true;
    for(const auto& entry:g_card_windows)if(entry->moving)return true;
    return false;
}

void card_resume_updates()
{
    if(card_is_moving())return;
    PostMessageW(g_main,WM_TIMER,kTimer,0);
    PostMessageW(g_main,WM_TIMER,kSyncTimer,0);
}

LRESULT CALLBACK card_size_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
    auto* entry = reinterpret_cast<CardWindow*>(data);
    if (message == WM_ENTERSIZEMOVE) {entry->sizing=false;entry->moving=true;}
    if (message == WM_EXITSIZEMOVE) {entry->moving=false;card_resume_updates();}
    if (message == WM_SIZING) entry->sizing = true;
    if (message == WM_EXITSIZEMOVE && entry->sizing && card_window_live(entry) && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
        auto card = std::find_if(g_cards.begin(), g_cards.end(), [&](const auto& value) { return value.id == entry->id; });
        RECT rect{};
        if (card != g_cards.end() && GetWindowRect(hwnd, &rect)) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const int width = MulDiv(rect.right - rect.left, 96, dpi ? dpi : 96);
            const int height = MulDiv(rect.bottom - rect.top, 96, dpi ? dpi : 96);
            if (width != card->windowWidthDip || height != card->windowHeightDip) {
                std::vector<tcard::CardRecord> changed{*card};
                changed[0].windowWidthDip = width;
                changed[0].windowHeightDip = height;
                if (card_save_records(changed)) {
                    card->windowWidthDip = width;
                    card->windowHeightDip = height;
                } else {
                    MessageBoxW(hwnd, tcard_text(L"message.save_failed", L"The changes could not be saved."), L"TCard", MB_OK | MB_ICONERROR);
                }
            }
        }
    } else if (message == WM_NCDESTROY) {
        entry->moving=false;card_resume_updates();
        RemoveWindowSubclass(hwnd, card_size_proc, id);
    }
    return DefSubclassProc(hwnd, message, w, l);
}

void open_selected()
{
    if (g_editing) return;
    const LRESULT selected = selected_index();
    if (selected < 0 || !load_renderer()) return;
    auto& card = g_cards[static_cast<size_t>(selected)];
    CardWindow* entry = nullptr;
    for (auto& candidate : g_card_windows) if (candidate->id == card.id && candidate->handle && IsWindow(g_get_card_window(candidate->handle))) { entry = candidate.get(); break; }
    if(entry){
        // Reopening only activates the live window; never reload or restart its editor.
        g_show_card(entry->handle,TRUE);card_focus_window(g_get_card_window(entry->handle));return;
    }
    if (!entry) {
        auto candidate = std::make_unique<CardWindow>();
        candidate->id = card.id;
        candidate->generation = g_next_generation++;
        if (g_next_generation == 0) g_next_generation = 1;
        candidate->handle = g_create_card(g_main);
        if (!candidate->handle) return;
        entry = candidate.get();
        g_card_windows.push_back(std::move(candidate));
        g_set_card_callback(entry->handle, renderer_command, entry);
        g_set_card_save_callback(entry->handle, renderer_save, entry);
        SetWindowSubclass(g_get_card_window(entry->handle), card_size_proc, 2, reinterpret_cast<DWORD_PTR>(entry));
    }
    apply_card_state(entry, card);
    if (!g_is_card_editing(entry->handle) && card.windowWidthDip > 0 && card.windowHeightDip > 0) {
        HWND window = g_get_card_window(entry->handle);
        const UINT dpi = GetDpiForWindow(window);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = min(MulDiv(card.windowWidthDip, dpi ? dpi : 96, 96), monitor.rcWork.right - monitor.rcWork.left);
        const int height = min(MulDiv(card.windowHeightDip, dpi ? dpi : 96, 96), monitor.rcWork.bottom - monitor.rcWork.top);
        SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Live variable cards retain their rendered widget view until clicked to edit.
    const auto visibleSource=card.richHtml?tcard_rich::text(tcard_rich::parse(card.source)):card.source;
    if(!card.live||visibleSource.find(L"<%")==std::wstring::npos)g_execute_card(entry->handle,1);
    g_show_card(entry->handle, TRUE);
    card_focus_window(g_get_card_window(entry->handle));
}

void update_open_card()
{
    if (!g_renderer) return;
    for (auto it = g_card_windows.begin(); it != g_card_windows.end();) {
        auto& entry = *it;
        auto card = std::find_if(g_cards.begin(), g_cards.end(), [&](const auto& value) { return value.id == entry->id; });
        if (!entry->handle || !g_get_card_window(entry->handle)) {
            it = g_card_windows.erase(it);
            continue;
        }
        if (card == g_cards.end()) {
            g_destroy_card(entry->handle);
            it = g_card_windows.erase(it);
            continue;
        }
        if(!g_is_card_editing(entry->handle))apply_card_state(entry.get(), *card);
        ++it;
    }
    if (g_main && !IsWindowVisible(g_main) && g_card_windows.empty()) DestroyWindow(g_main);
}

void choose_color(COLORREF color)
{
    const LRESULT selected = selected_index();
    if (selected < 0 || static_cast<size_t>(selected) >= g_cards.size()) return;
    if (g_editing) {
        if (g_preview_brush) DeleteObject(g_preview_brush);
        g_preview_color = color;
        g_preview_brush = CreateSolidBrush(color);
        SendMessageW(g_source_editor, EM_SETBKGNDCOLOR, 0, color);
        RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return;
    }
    auto& card = g_cards[static_cast<size_t>(selected)];
    const std::wstring previous = card.color;
    card.color = color_hex(color);
    if (!card_save_records(g_cards)) {
        card.color = previous;
        MessageBoxW(g_main, tcard_text(L"message.color_failed", L"The color change could not be saved."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    fill_list();
    if (g_editing) {
        if (g_preview_brush) DeleteObject(g_preview_brush);
        g_preview_color = color;
        g_preview_brush = CreateSolidBrush(g_preview_color);
        if (g_rich_edit && g_preview) SendMessageW(g_preview, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(g_preview_color));
        RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    } else {
        refresh_preview();
    }
    update_open_card();
}

struct ClipUndo { tcard::CardRecord previous; bool created = false; };
std::map<std::wstring, ClipUndo> g_clip_undo;
struct ClipJob {
    tcard_clip::Clip clip;
    tcard::CardRecord previous;
    std::wstring target;
    UINT command = 0;
    std::atomic_bool done{false}, cancelled{false};
    std::thread worker;
};
std::shared_ptr<ClipJob> g_clip_job;

void clip_menu(HMENU menu, bool target)
{
    HMENU clip = CreatePopupMenu();
    if (!clip) return;
    AppendMenuW(clip, MF_STRING, 1306, tcard_text(L"clip.new", L"Create new note"));
    AppendMenuW(clip, MF_STRING | (target ? 0 : MF_GRAYED), 1307, tcard_text(L"clip.append", L"Append to this note"));
    AppendMenuW(clip, MF_STRING | (target ? 0 : MF_GRAYED), 1308, tcard_text(L"clip.replace", L"Replace this note"));
    AppendMenuW(clip, MF_STRING | (target ? 0 : MF_GRAYED), 1309, tcard_text(L"clip.undo", L"Undo last import"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(clip), tcard_text(L"clip.menu", L"Import from clipboard"));
}

void import_clip(UINT command, const std::wstring& targetId, const tcard_clip::Clip* ready = nullptr,
    const tcard::CardRecord* previous = nullptr)
{
    if(card_signal_busy()){MessageBoxW(g_main,tcard_text(L"sync.signal_host_busy",L"Finish editing, image imports and cleanup, then save Sync settings again to start hosting."),L"TCard",MB_OK|MB_ICONINFORMATION);return;}
    if (g_clip_job) {
        MessageBoxW(g_main, tcard_text(L"clip.busy", L"Images are being downloaded. Please wait before importing again."), L"TCard", MB_OK); return;
    }
    if (g_editing) {
        MessageBoxW(g_main, tcard_text(L"clip.finish", L"Save or cancel the current edit first."), L"TCard", MB_OK);
        return;
    }
    for (const auto& entry : g_card_windows) {
        if (entry->id == targetId && card_window_live(entry.get()) && g_is_card_editing(entry->handle)) {
            MessageBoxW(g_main, tcard_text(L"clip.finish", L"Save or cancel the current edit first."), L"TCard", MB_OK); return;
        }
    }
    auto target = std::find_if(g_cards.begin(), g_cards.end(), [&](const auto& card) { return card.id == targetId; });
    if (ready && command != 6 && (target == g_cards.end() || !previous || target->source != previous->source ||
        target->title != previous->title || target->markdown != previous->markdown || target->richHtml != previous->richHtml || target->live != previous->live)) {
        MessageBoxW(g_main, tcard_text(L"clip.changed", L"The note changed while images were downloading. Import was cancelled; please paste again."), L"TCard", MB_OK | MB_ICONWARNING); return;
    }
    if (command != 6 && target == g_cards.end()) return;
    if (command == 9) {
        auto undo = g_clip_undo.find(targetId);
        if (undo == g_clip_undo.end()) { MessageBoxW(g_main, tcard_text(L"clip.no_undo", L"No import to undo in this session."), L"TCard", MB_OK); return; }
        if (MessageBoxW(g_main, tcard_text(L"clip.undo_confirm", L"Undo the last import? Later text edits to this note will also be replaced."), L"TCard", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
        bool ok;
        if (undo->second.created) {
            ok = tcard::DeleteCard(g_file_path, targetId);
            if (ok) g_cards.erase(target);
        } else {
            auto restored = *target;
            restored.title = undo->second.previous.title;
            restored.source = undo->second.previous.source;
            restored.live = undo->second.previous.live;
            restored.markdown = undo->second.previous.markdown;
            restored.richHtml = undo->second.previous.richHtml;
            std::vector<tcard::CardRecord> batch{restored};
            ok = card_save_records(batch);
            if (ok) *target = batch[0];
        }
        if (!ok) { MessageBoxW(g_main, tcard_text(L"clip.failed", L"Import could not be saved. The existing note has been kept."), L"TCard", MB_OK | MB_ICONERROR); return; }
        g_clip_undo.erase(undo); fill_list(); refresh_preview(); update_open_card(); return;
    }
    if (!ready && command == 8 && MessageBoxW(g_main, tcard_text(L"clip.confirm", L"Replace this note with the clipboard content?"), L"TCard", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    if (!ready && command == 7 && target->live && MessageBoxW(g_main, tcard_text(L"clip.format", L"Convert this note to static content and append the clipboard content?"), L"TCard", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    tcard_clip::Clip clip;
    try { clip = ready ? *ready : tcard_clip::read(g_main); }
    catch (...) { MessageBoxW(g_main, tcard_text(L"clip.invalid", L"The clipboard is unavailable, unsupported, or exceeds the size limit."), L"TCard", MB_OK | MB_ICONWARNING); return; }
    if (clip.source.empty()) { MessageBoxW(g_main, tcard_text(L"clip.invalid", L"The clipboard is unavailable, unsupported, or exceeds the size limit."), L"TCard", MB_OK); return; }
    if (!ready && (!clip.htmlSource.empty() || clip.source.find(L"data:image/") != std::wstring::npos)) {
        const size_t existing = command == 7 && !target->source.empty() ? target->source.size() + 12 : 0;
        if (existing > (clip.htmlSource.empty() ? tcard_clip::max_source : 4 * 1024 * 1024) || clip.source.size() > tcard_clip::max_import) {
            MessageBoxW(g_main, tcard_text(L"clip.invalid", L"The clipboard is unavailable, unsupported, or exceeds the size limit."), L"TCard", MB_OK); return;
        }
        auto job = std::make_shared<ClipJob>();
        job->clip = std::move(clip); job->command = command; job->target = targetId;
        if (command == 6) {
            GUID guid{}; wchar_t id[40]{};
            if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) return;
            job->target = id;
        }
        if (command != 6) job->previous = *target;
        const auto assetRoot = tcard::CardDirectory(g_file_path, job->target);
        if(assetRoot.empty()){
            MessageBoxW(g_main,tcard_text(L"clip.failed",L"Import could not be saved. The existing note has been kept."),L"TCard",MB_OK|MB_ICONERROR);return;
        }
        if (!SetTimer(g_main, kClipTimer, 100, nullptr)) {
            MessageBoxW(g_main, tcard_text(L"clip.failed", L"Import could not be saved. The existing note has been kept."), L"TCard", MB_OK | MB_ICONERROR); return;
        }
        g_clip_job = job;
        try {
            job->worker = std::thread([job, existing, assetRoot]() {
                try {
                    if (!job->clip.htmlSource.empty()) job->clip = tcard_web::import(job->clip, job->cancelled);
                    else {
                        job->clip = tcard_download::materialize(job->clip, assetRoot, job->cancelled, tcard_clip::max_source - existing);
                        if (!job->cancelled) job->clip.source = tcard_asset::externalize(assetRoot, job->clip.source);
                    }
                }
                catch (...) { job->clip.source.clear(); /* Never silently commit an incomplete web conversion. */ }
                job->done.store(true);
            });
        } catch (...) {
            KillTimer(g_main, kClipTimer); g_clip_job.reset();
            MessageBoxW(g_main, tcard_text(L"clip.failed", L"Import could not be saved. The existing note has been kept."), L"TCard", MB_OK | MB_ICONERROR); return;
        }
        SetWindowTextW(g_main, tcard_text(L"clip.downloading", L"TCard - downloading images..."));
        return;
    }
    tcard::CardRecord next;
    ClipUndo undo;
    if (command == 6) {
        GUID guid{}; wchar_t id[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, ARRAYSIZE(id))) return;
        next.id = ready && !targetId.empty() ? targetId : id;
        tcard_apply_defaults(next);
        next.title = tcard_text(L"clip.title", L"Web clip"); undo.created = true;
    } else { next = *target; undo.previous = *target; }
    if (clip.richHtml || (command == 7 && next.richHtml)) {
        auto incoming = clip.richHtml ? tcard_rich::parse(clip.source) : tcard_rich::plain(clip.source);
        if (command == 7 && !next.source.empty()) {
            auto combined = next.richHtml ? tcard_rich::parse(next.source) : tcard_rich::plain(next.source);
            combined.paragraphs.emplace_back();
            combined.paragraphs.insert(combined.paragraphs.end(), incoming.paragraphs.begin(), incoming.paragraphs.end());
            incoming = std::move(combined);
        }
        next.source = tcard_rich::serialize(incoming);next.richHtml = true;next.markdown = false;
    } else {
        next.source = command == 7 && !next.source.empty() ? next.source + L"\r\n\r\n---\r\n\r\n" + clip.source : clip.source;
        next.markdown = true;next.richHtml = false;
    }
    if (next.source.size() > (next.richHtml ? 4 * 1024 * 1024 : tcard_clip::max_source)) { MessageBoxW(g_main, tcard_text(L"clip.invalid", L"The clipboard is unavailable, unsupported, or exceeds the size limit."), L"TCard", MB_OK); return; }
    next.live = false;
    std::vector<tcard::CardRecord> batch{next};
    if (!card_save_records(batch)) { MessageBoxW(g_main, tcard_text(L"clip.failed", L"Import could not be saved. The existing note has been kept."), L"TCard", MB_OK | MB_ICONERROR); return; }
    g_clip_undo[next.id] = undo;
    if (command == 6) g_cards.insert(g_cards.begin(), batch[0]); else *target = batch[0];
    SetWindowTextW(g_search, L""); fill_list();
    for (size_t i = 0; i < g_visible_indices.size(); ++i) if (g_cards[g_visible_indices[i]].id == next.id) { SendMessageW(g_list, LB_SETCURSEL, i, 0); break; }
    refresh_preview(); update_open_card();
    if (command == 6) { ShowWindow(g_main, SW_SHOW); SetForegroundWindow(g_main); }
    if (clip.externalImages) MessageBoxW(g_main, clip.richHtml ?
        tcard_text(L"clip.offline_images", L"Some images could not be saved. Placeholders were kept; reopening the note will not retry downloads.") :
        tcard_text(L"clip.external", L"Some images could not be embedded (download, format or size limit). Their original URLs were kept as links."), L"TCard", MB_OK | MB_ICONINFORMATION);
}

struct SignalWindow {HWND hwnd=nullptr;bool enabled=false;};
struct SignalTask {
    std::atomic_bool done=false;
    bool okay=false,hosting=false,stopped=false,stopOnly=false;
    std::wstring pin,error;
    std::vector<SignalWindow> windows;
    std::unique_ptr<tcard_signal_sandbox::Profile> product;
    std::unique_ptr<tcard_signal_sandbox::Runtime> runtime;
    std::unique_ptr<tcard_signal_sandbox::Child> child;
    std::jthread worker; // Joined before releasing result fields or restoring Client access.
};
// Keep profile/runtime guards through the complete normal child lifetime.
std::unique_ptr<tcard_signal_sandbox::Profile> g_signal_product;
std::unique_ptr<tcard_signal_sandbox::Runtime> g_signal_runtime;
std::unique_ptr<tcard_signal_sandbox::Child> g_signal_child;
std::unique_ptr<SignalTask> g_signal_task; // Joins before global child/runtime/profile guards are released.
std::wstring g_signal_pin;
bool g_signal_close=false,g_signal_pending=false,g_signal_stop_pending=false;
bool card_signal_available(){return !card_signal_busy()&&g_sync_client.SignalAvailable();}
void card_signal_request(bool hosting){if(!g_main||(!hosting&&!g_signal_child&&!g_signal_task))return;g_signal_pending=true;g_signal_stop_pending=!hosting;
    if(!SetTimer(g_main,kSignalTimer,500,nullptr)){g_signal_pending=false;g_signal_status=L"sync.signal_host_failed";return;}
    PostMessageW(g_main,kSignalRequest,0,0);
}
void card_signal_restore(SignalTask& task)
{
    for(const auto& entry:task.windows)if(IsWindow(entry.hwnd)&&GetPropW(entry.hwnd,L"TCardSignal.Admission")==reinterpret_cast<HANDLE>(&task)){
        RemovePropW(entry.hwnd,L"TCardSignal.Admission");EnableWindow(entry.hwnd,entry.enabled);
    }
}
void card_signal_complete(HWND hwnd)
{
    if(!g_signal_task||!g_signal_task->done.load())return;
    auto task=std::move(g_signal_task);if(task->worker.joinable())task->worker.join();
    g_signal_child=std::move(task->child);g_signal_runtime=std::move(task->runtime);g_signal_product=std::move(task->product);g_signal_pin=task->hosting?task->pin:std::wstring();
    card_signal_restore(*task);g_signal_admitting=false;
    g_signal_status=task->okay?(task->hosting?L"sync.signal_host_running":std::wstring()):L"sync.signal_host_failed";
    // Admission returns Client ownership; stop-only never borrowed it. Restart uses current persisted values.
    try{if(task->stopped){const auto current=tcard_bridge::ReadSettings(preferences_path());
        if(!tcard_bridge::ValidSettings(current)||!g_sync_client.Start(current))g_signal_status=L"sync.signal_host_failed";}
    }catch(...){g_signal_status=L"sync.signal_host_failed";}
    if(!task->okay)MessageBoxW(hwnd,tcard_text(L"sync.signal_host_failed",L"The signaling node could not start. Sync settings and local data were kept."),L"TCard",MB_OK|MB_ICONWARNING);
    if(g_signal_close){g_signal_close=false;PostMessageW(hwnd,WM_CLOSE,0,0);}
}
void card_signal_reconcile(HWND hwnd)
{
    if(g_signal_task){g_signal_pending=true;return;}
    // Modal settings own main-window enablement. Wait for them to close before taking a snapshot.
    if(!IsWindowEnabled(hwnd)&&!g_signal_stop_pending){g_signal_pending=true;return;}
    const auto settings=tcard_bridge::ReadSettings(preferences_path());
    if(settings.hostSignal&&!IsWindowEnabled(hwnd)){g_signal_pending=true;g_signal_stop_pending=false;return;}g_signal_pending=false;
    if(g_signal_child&&g_signal_child->Exited()){g_signal_child.reset();g_signal_runtime.reset();g_signal_product.reset();g_signal_pin.clear();g_signal_status=L"sync.signal_host_failed";}
    if(!settings.hostSignal&&!g_signal_child)return;
    if(settings.hostSignal&&g_signal_child&&g_signal_pin==settings.hostSignalPin)return;
    if(settings.hostSignal&&(!card_signal_available()||!tcard_sync::sync_hex(settings.hostSignalPin))){
        g_signal_status=L"sync.signal_host_failed";MessageBoxW(hwnd,tcard_text(L"sync.signal_host_failed",L"The signaling node could not start. Sync settings and local data were kept."),L"TCard",MB_OK|MB_ICONWARNING);return;
    }
    if(settings.hostSignal){
    if(g_trash_task&&g_trash_task->done.load())g_trash_task.reset();
    bool busy=g_editing||g_appearance_open||g_clip_job||g_trash_task||card_is_moving();
    for(const auto& entry:g_card_windows){const HWND window=entry->handle&&g_get_card_window?g_get_card_window(entry->handle):nullptr;
        if(entry->sizing||(window&&!IsWindowEnabled(window))||(entry->handle&&g_is_card_editing&&g_is_card_editing(entry->handle)))busy=true;
    }
    if(busy){g_signal_status=L"sync.signal_host_busy";MessageBoxW(hwnd,tcard_text(L"sync.signal_host_busy",L"Finish editing, image imports and cleanup, then save Sync settings again to start hosting."),L"TCard",MB_OK|MB_ICONINFORMATION);return;}
    }
    auto task=std::make_unique<SignalTask>();auto* state=task.get();state->pin=settings.hostSignalPin;state->stopOnly=!settings.hostSignal;
    const auto executable=module_dir()+L"\\TCardSignal.exe",native=g_file_path,profile=g_sync_client.Profile();const bool hosting=settings.hostSignal;
    try{
        if(!SetTimer(hwnd,kSignalTimer,500,nullptr))throw std::runtime_error("Signal admission completion timer unavailable");
        const auto pause=[&](HWND window){if(!window||!IsWindow(window))return;
            state->windows.push_back({window,IsWindowEnabled(window)!=FALSE});
            if(!SetPropW(window,L"TCardSignal.Admission",reinterpret_cast<HANDLE>(state)))throw std::runtime_error("Signal admission window identity failed");
            EnableWindow(window,FALSE);
        };
        if(!state->stopOnly){pause(hwnd);for(const auto& entry:g_card_windows)pause(entry->handle&&g_get_card_window?g_get_card_window(entry->handle):nullptr);}
        state->child=std::move(g_signal_child);state->runtime=std::move(g_signal_runtime);state->product=std::move(g_signal_product);g_signal_admitting=!state->stopOnly;if(state->stopOnly)g_signal_status=L"sync.signal_host_stopping";
        state->worker=std::jthread([state,hwnd,executable,native,profile,hosting](std::stop_token stop){
            try{
                winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                if(state->child){state->child->Command('S');state->child->control.Reset();DWORD code=1;
                    if(!state->child->Wait(code))throw std::runtime_error("Signal node normal stop pending");state->child.reset();state->runtime.reset();state->product.reset();}
                if(stop.stop_requested())throw std::runtime_error("Signal admission cancelled");
                // Saved Host OFF closes only the public node; editors, native writers and Sync keep running.
                if(!state->stopOnly){if(!g_sync_client.Stop())throw std::runtime_error("Signal sync worker normal stop pending");state->stopped=true;}
                if(hosting){
                    if(!g_sync_client.PrepareSignalProtection(state->error,stop))throw std::runtime_error("Signal private protection refused");
                    // Product is deliberately nonmovable; direct prvalue initialization uses guaranteed C++17 elision.
                    state->product.reset(new tcard_signal_sandbox::Profile(tcard_signal_sandbox::Profile::Product()));
                    const auto directory=state->product->Directory()+L"\\Signal\\"+state->pin;
                    const auto config=GetFileAttributesW(tcard_native::Extended(directory+L"\\node-config.json").c_str());
                    if(config==INVALID_FILE_ATTRIBUTES||(config&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))throw std::runtime_error("Signal node provisioning unavailable");
                    state->runtime=std::make_unique<tcard_signal_sandbox::Runtime>(*state->product,executable);
                    state->child=std::make_unique<tcard_signal_sandbox::Child>();
                    tcard_signal_sandbox::LaunchSignal(*state->child,*state->runtime,*state->product,directory,stop);
                    if(stop.stop_requested()||!tcard_signal_protection::Check(native,profile,state->error)||!state->child->Command('G'))throw std::runtime_error("Signal GO admission refused");
                    tcard_signal_sandbox::Packet packet{};bool received=false;const auto end=GetTickCount64()+tcard_signal_sandbox::waitMilliseconds;
                    while(!stop.stop_requested()&&GetTickCount64()<end&&!state->child->Exited())if(tcard_signal_sandbox::ReadPacket(state->child->result.value,packet,50)){received=true;break;}
                    if(stop.stop_requested()||!received||packet.kind!=tcard_signal_sandbox::resultPacket||packet.pid!=state->child->pid||!packet.code||packet.code>65535)
                        throw std::runtime_error("Signal listener start refused");
                    state->hosting=true;
                }
                if(stop.stop_requested())throw std::runtime_error("Signal admission cancelled");
                state->okay=true;
            }catch(...){
                if(state->child){state->child->Command('S');state->child->control.Reset();DWORD code=1;if(state->child->Wait(code))state->child.reset();}
                if(!state->child){state->runtime.reset();state->product.reset();}
                state->okay=false;
            }
            state->done.store(true);PostMessageW(hwnd,kSignalComplete,0,0);
        });
        g_signal_task=std::move(task);
    }catch(...){
        g_signal_child=std::move(state->child);g_signal_runtime=std::move(state->runtime);g_signal_product=std::move(state->product);card_signal_restore(*state);g_signal_admitting=false;g_signal_status=L"sync.signal_host_failed";
        MessageBoxW(hwnd,tcard_text(L"sync.signal_host_failed",L"The signaling node could not start. Sync settings and local data were kept."),L"TCard",MB_OK|MB_ICONWARNING);
    }
}
void card_signal_shutdown()
{
    if(g_signal_task){g_signal_task->worker.request_stop();if(g_signal_task->worker.joinable())g_signal_task->worker.join();
        g_signal_child=std::move(g_signal_task->child);g_signal_runtime=std::move(g_signal_task->runtime);g_signal_product=std::move(g_signal_task->product);card_signal_restore(*g_signal_task);g_signal_task.reset();g_signal_admitting=false;}
    if(g_signal_child){g_signal_child->Command('S');g_signal_child->control.Reset();DWORD code=1;
        if(g_signal_child->Wait(code)){g_signal_child.reset();g_signal_runtime.reset();g_signal_product.reset();}}
}

bool card_is_busy()
{
    if(g_trash_task&&g_trash_task->done.load())g_trash_task.reset();
    if(g_storage_locked||g_source_dialog||card_signal_busy()||g_signal_task||g_editing||g_appearance_open||g_clip_job||g_trash_task||card_is_moving())return true;
    for(const auto& entry:g_card_windows){const HWND window=entry->handle&&g_get_card_window?g_get_card_window(entry->handle):nullptr;
        if(entry->sizing||(window&&!IsWindowEnabled(window))||(entry->handle&&g_is_card_editing&&g_is_card_editing(entry->handle)))return true;
    }return false;
}
// Native activation remains on this thread; only paint and sent messages are serviced.
struct CardKeyActivity {
    HWND owner;std::wstring label;std::vector<HWND> viewers;ULONGLONG painted=0;
    std::vector<std::pair<HWND,bool>> controls;bool closeEnabled=false;
    explicit CardKeyActivity(HWND window):owner(window),label(window_text(GetDlgItem(window,1725))){
        for(int id:{1724,1726,1727,IDCANCEL}){const auto child=GetDlgItem(owner,id);controls.emplace_back(child,IsWindowEnabled(child)!=FALSE);}
        for(const auto& card:g_card_windows){const HWND viewer=card->handle&&g_get_card_window?g_get_card_window(card->handle):nullptr;
            if(viewer&&IsWindowEnabled(viewer))viewers.push_back(viewer);}
        const auto menu=GetSystemMenu(owner,FALSE);closeEnabled=(GetMenuState(menu,SC_CLOSE,MF_BYCOMMAND)&(MF_DISABLED|MF_GRAYED))==0;
        const auto working=tcard_text(L"sync.key_working",L"Updating the key. Please wait.");
        g_key_active=true;
        for(const auto& control:controls)EnableWindow(control.first,FALSE);
        for(const auto viewer:viewers)EnableWindow(viewer,FALSE);
        EnableMenuItem(menu,SC_CLOSE,MF_BYCOMMAND|MF_GRAYED);DrawMenuBar(owner);
        SetDlgItemTextW(owner,1725,working);
        Paint();
    }
    void Paint(){
        const auto now=GetTickCount64();if(painted&&now-painted<25)return;painted=now;
        // Do not dispatch queued commands, timers, input or shutdown while roots are changing.
        MSG message{};for(unsigned count=0;count<16&&PeekMessageW(&message,owner,WM_PAINT,WM_PAINT,PM_REMOVE);++count)DispatchMessageW(&message);
        UpdateWindow(GetDlgItem(owner,1725));
    }
    ~CardKeyActivity(){
        SetDlgItemTextW(owner,1725,label.c_str());
        for(const auto& control:controls)EnableWindow(control.first,control.second);
        for(const auto viewer:viewers)if(IsWindow(viewer))EnableWindow(viewer,TRUE);
        EnableMenuItem(GetSystemMenu(owner,FALSE),SC_CLOSE,MF_BYCOMMAND|(closeEnabled?MF_ENABLED:MF_GRAYED));DrawMenuBar(owner);
        g_key_active=false;
    }
};
bool card_rotate_key(HWND owner,std::wstring& display)
{
    if(card_is_busy()){g_sync_client.status=L"sync.key_busy";return false;}
    if(MessageBoxW(owner,tcard_text(L"sync.key_confirm",L"Create a new key and re-encrypt all saved notes, history and private copies? Keep the new words and use them on your other PCs. Interrupted changes resume on restart."),
        L"TCard",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES){g_sync_client.status=L"sync.key_cancelled";return false;}
    if(card_is_busy()){g_sync_client.status=L"sync.key_busy";return false;}
    // Keep live activation on this UI thread, with writers closed and key commands blocked.
    const bool encrypted=tcard_storage::Required(g_file_path)||tcard_storage::Required(g_sync_client.Profile());
    g_storage_locked=true; // Exceptions keep the owner closed until startup recovery.
    CardKeyActivity activity(owner);const auto progress=[&](const std::wstring&,size_t,size_t){activity.Paint();};
    const bool okay=encrypted?g_sync_client.Rotate(display,progress):g_sync_client.Key(L"new",L"",display,L"",L"",[&]{activity.Paint();});
    g_storage_locked=encrypted&&!g_sync_client.Recover();
    if(okay&&encrypted){std::vector<tcard::CardRecord> loaded;
        if(!tcard::LoadCardSnapshot(g_file_path,loaded)){g_storage_locked=true;g_sync_client.status=L"sync.storage_failed";return false;}
        g_cards=std::move(loaded);fill_list();refresh_preview();update_open_card();
    }
    return okay;
}
bool card_join_key(HWND owner,const std::wstring& words,std::wstring& display)
{
    if(g_source_dialog){g_sync_client.status=L"sync.key_busy";return false;}
    bool same=false;
    if(!g_sync_client.Match(words,same))return false;
    if(same)return g_sync_client.Key(L"import",words,display);
    if(card_is_busy()){g_sync_client.status=L"sync.key_busy";return false;}
    const auto choice=card_choose_join(owner);if(choice!=IDYES&&choice!=IDNO){g_sync_client.status=L"sync.key_cancelled";return false;}
    if(card_is_busy()){g_sync_client.status=L"sync.key_busy";return false;}
    g_storage_locked=true; // Explicit join owns initial encryption and activation; exceptions remain closed.
    if(!g_sync_client.Encrypted()&&!g_sync_client.Prepare(tcard_bridge::ReadSettings(preferences_path()))){
        g_storage_locked=!tcard_storage::Ready(g_file_path)||!tcard_storage::Ready(g_sync_client.Profile());return false;
    }
    std::wstring job;
    if(choice==IDNO){
        g_storage_locked=false;
        if(!card_choose_source(owner,words,job)){g_sync_client.status=L"sync.key_cancelled";return false;}
        if(card_is_busy()){g_sync_client.status=L"sync.key_busy";return false;}
        tcard_source_job::Intent intent;tcard_source_job::ReadIntent(g_sync_client.Profile(),job,intent);
        if(!card_source_policy(intent,tcard_bridge::ReadSettings(preferences_path()))){g_sync_client.status=L"sync.key_busy";return false;}
        g_storage_locked=true;
    }
    const bool okay=choice==IDNO?(g_sync_client.Replace(job)&&g_sync_client.Key(L"show",L"",display)):g_sync_client.Merge(words,display);
    g_storage_locked=!g_sync_client.Recover();
    if(okay&&!g_storage_locked){std::vector<tcard::CardRecord> loaded;
        if(!tcard::LoadCardSnapshot(g_file_path,loaded)){g_storage_locked=true;g_sync_client.status=L"sync.storage_failed";return false;}
        g_cards=std::move(loaded);
        if(choice==IDNO){g_clip_undo.clear();if(!card_load_trash()){g_storage_locked=true;g_sync_client.status=L"sync.storage_failed";return false;}for(auto it=g_card_windows.begin();it!=g_card_windows.end();){
            if(std::none_of(g_cards.begin(),g_cards.end(),[&](const auto& card){return card.id==(*it)->id;})){if((*it)->handle)g_destroy_card((*it)->handle);it=g_card_windows.erase(it);}else ++it;
        }}
        fill_list();refresh_preview();update_open_card();
    }
    return okay&&!g_storage_locked;
}
bool card_sync_start(HWND owner,const tcard_bridge::Settings& settings,bool startup)
{
    if(g_storage_locked||g_source_dialog||card_signal_busy())return false;
    static bool deferred=false;
    if(!settings.enabled||!g_sync_client.Available())return g_sync_client.Start(settings);
    if(deferred){g_sync_client.status=L"sync.storage_deferred";return false;}
    auto busy=[](){
        if(g_editing||g_clip_job)return true;
        for(const auto& entry:g_card_windows)if(entry->handle&&g_is_card_editing&&g_is_card_editing(entry->handle))return true;
        return false;
    };
    if(busy()){MessageBoxW(owner,tcard_text(L"sync.storage_busy",L"Finish editing and image imports before starting synchronization."),L"TCard",MB_OK|MB_ICONINFORMATION);return false;}
    if(!g_sync_client.Stop())return false;
    if(!g_sync_client.Encrypted()){
        if(MessageBoxW(owner,tcard_text(L"sync.storage_confirm",L"Encrypt this device's saved notes, history, images and private synchronization copies with the existing sync key before connecting? Interrupted encryption will resume. Choose No to keep synchronization paused for this launch."),
            L"TCard",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES){deferred=true;g_sync_client.status=L"sync.storage_deferred";return false;}
    }
    if(busy()){g_sync_client.status=L"sync.storage_busy";return false;}
    if(!g_sync_client.Prepare(settings)){
        MessageBoxW(owner,tcard_text(L"sync.storage_failed",L"Encryption could not be completed. Synchronization is paused and existing data is preserved. Check the key and restart to retry."),L"TCard",MB_OK|MB_ICONERROR);return false;
    }
    // Startup resumes migration before loading cards; it admits networking only after that load.
    if(startup)return true;
    std::vector<tcard::CardRecord> loaded;
    if(!tcard::LoadCardSnapshot(g_file_path,loaded)){g_sync_client.status=L"sync.storage_failed";return false;}
    g_cards=std::move(loaded);fill_list();refresh_preview();update_open_card();
    return g_sync_client.Start(settings);
}

void card_sync_poll(HWND hwnd)
{
    if(card_signal_busy())return;
    if(!IsWindowEnabled(hwnd)||g_appearance_open||g_clip_job)return;
    bool blocked=g_editing;
    for(const auto& entry:g_card_windows){
        const HWND card=entry->handle&&g_get_card_window?g_get_card_window(entry->handle):nullptr;
        if(card&&!IsWindowEnabled(card))return;
        if(card&&g_is_card_editing&&g_is_card_editing(entry->handle))blocked=true;
    }
    try{
        if(!g_sync_client.Ready())return;
        const bool published=g_sync_client.SendAsync(g_file_path);
        SetTimer(hwnd,kSyncTimer,(g_sync_client.Sending()||g_sync_client.Receiving())?100:3000,nullptr);
        if(!published&&!g_sync_client.Receiving())return;
        std::vector<tcard::CardRecord> previous;
        if(g_sync_client.Receive(g_file_path,blocked,g_cards,&previous)){
            if(g_trash_mode)card_load_trash();
            for(const auto& old:previous){
                const auto current=std::find_if(g_cards.begin(),g_cards.end(),[&](const auto& value){return value.id==old.id;});
                if(current==g_cards.end()||old.source!=current->source||old.title!=current->title||old.markdown!=current->markdown||old.richHtml!=current->richHtml)
                    g_clip_undo.erase(old.id);
            }
            // Only changed viewers receive new source. Active editors and their undo/selection remain untouched.
            for(auto it=g_card_windows.begin();it!=g_card_windows.end();){
                const auto current=std::find_if(g_cards.begin(),g_cards.end(),[&](const auto& value){return value.id==(*it)->id;});
                if(current==g_cards.end()){
                    if((*it)->handle)g_destroy_card((*it)->handle);it=g_card_windows.erase(it);continue;
                }
                const auto assetRoot=tcard::CardDirectory(g_file_path,current->id);
                if(g_set_card_asset_root)g_set_card_asset_root((*it)->handle,assetRoot.c_str(),static_cast<DWORD>(assetRoot.size()));
                const auto old=std::find_if(previous.begin(),previous.end(),[&](const auto& value){return value.id==current->id;});
                if(old!=previous.end()&&(old->source!=current->source||old->title!=current->title||old->markdown!=current->markdown||old->richHtml!=current->richHtml||tcard_sync::sync_display(*old)!=tcard_sync::sync_display(*current)))
                    apply_card_state(it->get(),*current);
                ++it;
            }
            fill_list();refresh_preview();
        }
    }catch(...){g_sync_client.status=L"sync.failed";}
}

void refresh_language_ui(HWND hwnd)
{
    SetWindowTextW(hwnd, tcard_text(L"app.title", L"TCard"));
    SetWindowTextW(GetDlgItem(hwnd, kHeading), g_trash_mode?tcard_text(L"trash.tab",L"Trash"):tcard_text(L"sidebar.heading", L"Notes"));
    SetWindowTextW(GetDlgItem(hwnd, kSearchLabel), tcard_text(L"search.label", L"Search notes"));
    SendMessageW(g_search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"search.placeholder", L"Search notes")));
    SetWindowTextW(g_empty_state, g_cards.empty() ? tcard_text(L"empty.no_notes", L"No notes yet") : tcard_text(L"empty.no_match", L"No matching notes"));
    SetWindowTextW(g_clear_search, tcard_text(L"button.clear_search", L"Clear search"));
    SetWindowTextW(GetDlgItem(hwnd, kSortField), tcard_text(L"button.sort", L"Sort"));
    SetWindowTextW(GetDlgItem(hwnd,kGlobalSettings),tcard_text(L"button.global_settings",L"Settings"));
    SetWindowTextW(GetDlgItem(hwnd, kNew), tcard_text(L"button.new", L"+  New"));
    SetWindowTextW(GetDlgItem(hwnd, kDelete), tcard_text(L"button.delete", L"Delete"));
    SetWindowTextW(GetDlgItem(hwnd, kOpen), g_trash_mode?tcard_text(L"trash.restore",L"Restore"):tcard_text(L"button.open", L"Pop out"));
    SetWindowTextW(GetDlgItem(hwnd, kEdit), g_trash_mode?tcard_text(L"trash.remove",L"Delete permanently"):tcard_text(L"button.edit", L"Edit"));
    SetWindowTextW(GetDlgItem(hwnd, kSave), tcard_text(L"button.save", L"Save"));
    SetWindowTextW(GetDlgItem(hwnd, kCancel), tcard_text(L"button.cancel", L"Cancel"));
    SetWindowTextW(GetDlgItem(hwnd, kAppearance), g_appearance_open ? tcard_text(L"button.back_to_note", L"Back to note") : tcard_text(L"button.appearance", L"Appearance"));
    for (const auto& choice : kColorChoices) SetWindowTextW(GetDlgItem(hwnd, choice.id), tcard_color_name(choice.id));
    SetWindowTextW(GetDlgItem(hwnd, 1030), tcard_text(L"label.font", L"Font"));
    SetWindowTextW(GetDlgItem(hwnd, 1031), tcard_text(L"label.size", L"Size"));
    SetWindowTextW(GetDlgItem(hwnd, 1032), tcard_text(L"label.text_format", L"Text format"));
    SetWindowTextW(GetDlgItem(hwnd, 1033), tcard_text(L"label.paper_color", L"Paper color"));
    SendMessageW(g_title_editor, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tcard_text(L"title.placeholder", L"Title")));
    SendMessageW(g_font_family_editor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"font.family_placeholder", L"Font family")));
    SendMessageW(g_font_size_editor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"label.size", L"Size")));
    SendMessageW(g_markdown, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_markdown, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.plain", L"Plain text")));
    SendMessageW(g_markdown, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.markdown", L"Markdown")));
    const LRESULT selected = selected_index();
    select_text_format(g_markdown, g_editing?g_editor_markdown:(selected >= 0 && g_cards[static_cast<size_t>(selected)].markdown));
    SetWindowTextW(g_view_button,g_trash_mode?tcard_text(L"trash.back",L"Notes"):tcard_text(L"trash.tab",L"Trash"));
    if (!g_editing) refresh_preview();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case kSignalRequest:card_signal_reconcile(hwnd);return 0;
    case kSignalComplete:card_signal_complete(hwnd);return 0;
    case kRendererClip:
        if (lParam) {
            auto* entry = reinterpret_cast<CardWindow*>(lParam);
            if (card_window_live(entry)) { const std::wstring id = entry->id; import_clip(static_cast<UINT>(wParam), id); }
        }
        return 0;
    case kRendererSave:
        if(g_storage_locked||card_signal_busy())return FALSE; // Keep the renderer draft; no false successful save.
        if (wParam) {
            auto* entry = reinterpret_cast<CardWindow*>(wParam);
            if (!card_window_live(entry)) return FALSE;
            TCARD_WUI_TEXT_STATE_BUFFER state{};
            state.cb = sizeof(state);
            state.version = TCARD_WUI_STATE_ABI_VERSION;
            if (g_get_card_text_state(entry->handle, &state)) {
                std::wstring title(static_cast<size_t>(state.titleLength) + 1, L'\0');
                std::wstring source(static_cast<size_t>(state.sourceLength) + 1, L'\0');
                state.title = &title[0];
                state.titleCapacity = static_cast<DWORD>(title.size());
                state.source = &source[0];
                state.sourceCapacity = static_cast<DWORD>(source.size());
                std::wstring family(static_cast<size_t>(state.fontFamilyLength) + 1, L'\0');
                state.fontFamily = &family[0];
                state.fontFamilyCapacity = static_cast<DWORD>(family.size());
                if (!g_get_card_text_state(entry->handle, &state)) return FALSE;
                auto card = std::find_if(g_cards.begin(), g_cards.end(), [&](const auto& value) { return value.id == entry->id; });
                if (card != g_cards.end()) {
                    const auto previous = *card;
                    card->title = state.titleLength ? std::wstring(state.title, state.titleLength) : tcard_text(L"status.untitled_card", L"Untitled card");
                    card->source = state.sourceLength ? std::wstring(state.source, state.sourceLength) : std::wstring();
                    card->fontFamily = state.fontFamilyLength ? std::wstring(state.fontFamily, state.fontFamilyLength) : L"Segoe UI";
                    card->fontSize = parse_font_size(format_font_size(state.fontSize), card->fontSize);
                    card->inheritFontFamily=state.inheritFontFamily!=FALSE;card->inheritFontSize=state.inheritFontSize!=FALSE;
                    card->markdown = state.markdown != FALSE;
                    card->richHtml = state.richHtml != FALSE;
                    card->color = color_hex(state.backColor);
                    std::vector<tcard::CardRecord> batch{*card};
                    if (!card_save_records(batch,state.checkpoint!=FALSE)) {
                        *card=previous;return FALSE;
                    }
                    *card=batch[0];
                    apply_card_state(entry,*card);
                    fill_list();
                    refresh_preview();
                    return TRUE;
                }
            }
        }
        return FALSE;
    case kRendererDelete:
        if (wParam) delete_card_id(reinterpret_cast<CardWindow*>(wParam)->id);
        return 0;
    case kRendererShowList:
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        return 0;
    case kRendererEdit:
        if(g_storage_locked||card_signal_busy())return FALSE;
        if (g_editing) {
            MessageBoxW(g_main, tcard_text(L"message.finish_edit", L"Finish the current list edit before editing a standalone note."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (wParam) {
            auto* entry = reinterpret_cast<CardWindow*>(wParam);
            if (!card_window_live(entry)) return 0;
            if(g_trash_mode)card_set_view(false);
            bool visible = false;
            for (size_t i = 0; i < g_visible_indices.size(); ++i) {
                if (g_cards[g_visible_indices[i]].id == entry->id) {
                    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                    visible = true;
                    break;
                }
            }
            if (!visible) {
                SetWindowTextW(g_search, L"");
                fill_list();
                for (size_t i = 0; i < g_visible_indices.size(); ++i) {
                    if (g_cards[g_visible_indices[i]].id == entry->id) {
                        SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
                        break;
                    }
                }
            }
        }
        set_editing(true);
        return 0;

    case WM_CONTEXTMENU:
        if(g_storage_locked||card_signal_busy())return FALSE;
        if (reinterpret_cast<HWND>(wParam) == g_list || reinterpret_cast<HWND>(wParam) == g_preview || reinterpret_cast<HWND>(wParam) == hwnd) {
            POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            POINT clientPoint = point;
            ScreenToClient(g_list, &clientPoint);
            const LRESULT hit = SendMessageW(g_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(clientPoint.x, clientPoint.y));
            if (reinterpret_cast<HWND>(wParam) == g_list && !HIWORD(hit) && LOWORD(hit) < g_visible_indices.size()) SendMessageW(g_list, LB_SETCURSEL, LOWORD(hit), 0);
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            if(g_trash_mode){
                const auto selected=card_view_index();const bool readable=selected>=0&&g_trash_records[static_cast<size_t>(selected)].readable;
                AppendMenuW(menu,MF_STRING|(readable?0:MF_GRAYED),kOpen,tcard_text(L"trash.restore",L"Restore"));
                AppendMenuW(menu,MF_STRING|(readable?0:MF_GRAYED),kEdit,tcard_text(L"trash.remove",L"Delete permanently"));
                const auto command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,hwnd,nullptr);DestroyMenu(menu);
                if(command)SendMessageW(hwnd,WM_COMMAND,command,0);return 0;
            }
            AppendMenuW(menu, MF_STRING, kContextOpen, tcard_text(L"context.open", L"Open"));
            AppendMenuW(menu, MF_STRING, kContextEdit, tcard_text(L"context.edit", L"Edit"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kContextDelete, tcard_text(L"context.delete", L"Delete"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            clip_menu(menu, selected_index() >= 0);
            const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, hwnd, nullptr);
            DestroyMenu(menu);
            if (command == kContextOpen) open_selected();
            else if (command == kContextEdit) set_editing(true);
            else if (command == kContextDelete) delete_selected();
            else if (command >= 1306 && command <= 1309)
                SendMessageW(hwnd,WM_COMMAND,command,0);
            return 0;
        }
        break;
    case WM_CLOSE:
        if(g_key_active)return 0;
        if(card_signal_busy()){g_signal_close=true;g_signal_task->worker.request_stop();return 0;}
        {
        if (g_editing) {
            const int choice = MessageBoxW(hwnd, tcard_text(L"message.save_changes", L"Save changes to this card?"), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON3);
            if (choice == IDCANCEL) return 0;
            if (choice == IDYES) {
                save_edit();
                if (g_editing) return 0;
            } else {
                set_editing(false);
            }
        }
        bool has_live_cards = false;
        if (g_get_card_window) {
            for (const auto& entry : g_card_windows) {
                if (entry->handle && g_get_card_window(entry->handle)) { has_live_cards = true; break; }
            }
        }
        if (has_live_cards) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
        }
    case WM_ERASEBKGND:
        {
            RECT rc{}; GetClientRect(hwnd, &rc);
            HBRUSH shell = CreateSolidBrush(RGB(246, 245, 242));
            FillRect(reinterpret_cast<HDC>(wParam), &rc, shell); DeleteObject(shell);
            rc.left = 288;
            if (g_preview_brush) FillRect(reinterpret_cast<HDC>(wParam), &rc, g_preview_brush);
            RECT underline{20, 98, rc.left - 20, 99};
            SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(210, 208, 199));
            FillRect(reinterpret_cast<HDC>(wParam), &underline, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            return 1;
        }
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(37, 37, 37));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(246, 245, 242));
        SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(246, 245, 242));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == g_search) {
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(235, 235, 235));
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(37, 37, 37));
            SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(235, 235, 235));
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        if ((reinterpret_cast<HWND>(lParam) == g_preview ||
             reinterpret_cast<HWND>(lParam) == g_detail_title ||
             reinterpret_cast<HWND>(lParam) == g_title_editor ||
             reinterpret_cast<HWND>(lParam) == g_title_line ||
             reinterpret_cast<HWND>(lParam) == g_source_editor ||
             (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) >= 1030 && GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) <= 1033)) && g_preview_brush) {
            SetBkColor(reinterpret_cast<HDC>(wParam), g_preview_color);
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(37, 37, 37));
            return reinterpret_cast<LRESULT>(g_preview_brush);
        }
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(37, 37, 37));
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(246, 245, 242));
        SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(246, 245, 242));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CREATE: {
        g_main = hwnd;
        tcard_lang::initialize();
        g_ui_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_button_font = CreateFontW(tcard_lang::g_code == L"ja" ? -12 : -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_heading_font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_tile_title_font = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_tile_excerpt_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_search = CreateWindowExW(0, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 56, 260, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearch)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        SendMessageW(g_search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"search.placeholder", L"Search notes")));
        HWND searchLabel = CreateWindowExW(0, L"STATIC", tcard_text(L"search.label", L"Search notes"), WS_CHILD, 20, 48, 160, 18, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchLabel)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(searchLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_tile_excerpt_font), TRUE);
        g_view_button=CreateWindowExW(0,L"BUTTON",tcard_text(L"trash.tab",L"Trash"),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,16,0,72,32,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kViewButton)),GetModuleHandleW(nullptr),nullptr);
        g_trash_info=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|SS_ENDELLIPSIS,312,560,400,36,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTrashInfo)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(g_trash_info,WM_SETFONT,reinterpret_cast<WPARAM>(g_tile_excerpt_font),TRUE);
        g_list = CreateWindowExW(0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 16, 104, 260, 300, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        g_rich_edit = LoadLibraryW(L"Msftedit.dll");
        g_preview = CreateWindowExW(0, g_rich_edit ? L"RICHEDIT50W" : L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 245, 0, 400, 300, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreview)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_preview, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(24, 18));
        SendMessageW(g_preview, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        g_detail_title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 312, 64, 320, 28, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_detail_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_heading_font), TRUE);
        g_title_editor = CreateWindowExW(0, L"EDIT", nullptr,
            WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP, 245, 0, 400, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleEditor)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_title_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(16, 16));
        SendMessageW(g_title_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_heading_font), TRUE);
        g_title_line = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | SS_OWNERDRAW,
            245, 30, 400, 2, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_source_editor = tcard_edit::create(hwnd, kSourceEditor, kSave, L"", 245, 34, 400, 266, GetModuleHandleW(nullptr));
        ShowWindow(g_source_editor, SW_HIDE);
        tcard_edit::attach(g_title_editor, false, kSave);
        tcard_edit::attach(g_search, false);
        g_font_family_editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr, WS_CHILD | CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP, 0, 0, 180, 240, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFontFamily)), GetModuleHandleW(nullptr), nullptr);
        populate_font_families(g_font_family_editor);
        for (HWND control : {g_font_family_editor}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        g_font_size_editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr, WS_CHILD | CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 0, 0, 200, 240, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFontSize)), GetModuleHandleW(nullptr), nullptr);
        tcard_ui::create_label(hwnd, 1030, tcard_text(L"label.font", L"Font"), g_ui_font);
        tcard_ui::create_label(hwnd, 1031, tcard_text(L"label.size", L"Size"), g_ui_font);
        tcard_ui::create_label(hwnd, 1032, tcard_text(L"label.text_format", L"Text format"), g_ui_font);
        tcard_ui::create_label(hwnd, 1033, tcard_text(L"label.paper_color", L"Paper color"), g_ui_font);
        SendMessageW(g_font_size_editor, CB_LIMITTEXT, 64, 0);
        SendMessageW(g_title_editor, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tcard_text(L"title.placeholder", L"Title")));
        g_markdown = CreateWindowExW(0, L"COMBOBOX", nullptr, WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST | WS_TABSTOP, 0, 0, 140, 180, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMarkdown)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_markdown, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.plain", L"Plain text")));
        SendMessageW(g_markdown, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tcard_text(L"format.markdown", L"Markdown")));
        SendMessageW(g_markdown, CB_SETCURSEL, 0, 0);
        for (HWND control : {g_font_size_editor, g_markdown}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        SendMessageW(g_font_family_editor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"font.family_placeholder", L"Font family")));
        SendMessageW(g_font_size_editor, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(tcard_text(L"label.size", L"Size")));
        SendMessageW(g_source_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(24, 18));
        SendMessageW(g_source_editor, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        SendMessageW(g_source_editor, EM_SETLIMITTEXT, tcard_clip::max_source, 0);
        g_empty_state = CreateWindowExW(0, L"STATIC", tcard_text(L"empty.no_notes", L"No notes yet"), WS_CHILD | SS_CENTER, 16, 160, 250, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_empty_state, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        g_clear_search = CreateWindowExW(0, L"BUTTON", tcard_text(L"button.clear_search", L"Clear search"), WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 16, 196, 140, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kClearSearch)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_clear_search, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
        HWND heading = CreateWindowExW(0, L"STATIC", tcard_text(L"sidebar.heading", L"Notes"), WS_CHILD | WS_VISIBLE, 16, 12, 70, 24, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHeading)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_heading_font ? g_heading_font : g_ui_font), TRUE);
        for (const auto& choice : kColorChoices) {
            HWND swatch = CreateWindowExW(0, L"BUTTON", tcard_color_name(choice.id), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                0, 0, 21, 21, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(choice.id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(swatch, WM_SETFONT, reinterpret_cast<WPARAM>(g_tile_excerpt_font), TRUE);
        }
        CreateWindowExW(0, L"BUTTON", tcard_text(L"button.new", L"+  New"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 8, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNew)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"button.delete", L"Delete"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 8, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDelete)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"context.open", L"Open"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 84, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpen)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"context.edit", L"Edit"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 160, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"button.save", L"Save"), WS_CHILD | BS_PUSHBUTTON, 236, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSave)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"button.cancel", L"Cancel"), WS_CHILD | BS_PUSHBUTTON, 312, 340, 70, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancel)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", tcard_text(L"button.appearance", L"Appearance"), WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 104, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppearance)), GetModuleHandleW(nullptr), nullptr);
        for (int id : {kViewButton, kNew, kDelete, kOpen, kEdit, kSave, kCancel, kClearSearch, kAppearance, kColorYellow, kColorGreen, kColorBlue, kColorPink, kColorLavender, kColorNeutral}) {
            HWND button = GetDlgItem(hwnd, id);
            SetWindowLongPtrW(button, GWL_STYLE, GetWindowLongPtrW(button, GWL_STYLE) | WS_TABSTOP);
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_tile_excerpt_font), TRUE);
            SendMessageW(button, BM_SETSTYLE, BS_OWNERDRAW, TRUE);
            SetWindowSubclass(button, tcard_ui::button_proc, 1, 0);
        }
        SetWindowTextW(GetDlgItem(hwnd, kNew), tcard_text(L"button.new", L"+  New"));
        SetWindowTextW(GetDlgItem(hwnd, kDelete), tcard_text(L"button.delete", L"Delete"));
        SetWindowTextW(GetDlgItem(hwnd, kOpen), g_trash_mode?tcard_text(L"trash.restore",L"Restore"):tcard_text(L"button.open", L"Pop out"));
        ShowWindow(GetDlgItem(hwnd, kAppearance), SW_HIDE);
        ShowWindow(g_font_family_editor, SW_HIDE);
        ShowWindow(g_font_size_editor, SW_HIDE);
    ShowWindow(g_markdown, SW_HIDE);
        ShowWindow(g_title_line, SW_SHOW);
        for (const auto& choice : kColorChoices) ShowWindow(GetDlgItem(hwnd, choice.id), SW_HIDE);
        SendMessageW(g_preview, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(g_source_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(g_title_editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        g_sync_client.SetDirectory(module_dir());
        create_sort(hwnd);
        refresh_language_ui(hwnd);
        SetTimer(hwnd, kTimer, refresh_interval_ms(), nullptr);
        g_file_path = module_dir() + L"\\tcard\\cards";
        tcard::ConfigureCustomVariables(resolve_tclock_ini());
        g_sync_client.SetStorage(g_file_path);
        const auto syncSettings=tcard_bridge::ReadSettings(preferences_path());
        g_storage_locked=!g_sync_client.Recover();
        const bool syncPrepared=!g_storage_locked&&card_sync_start(hwnd,syncSettings,true);
        const auto existingRoot=GetFileAttributesW(g_file_path.c_str());
        const auto rootError=existingRoot==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
        const bool newStore=existingRoot==INVALID_FILE_ATTRIBUTES&&(rootError==ERROR_FILE_NOT_FOUND||rootError==ERROR_PATH_NOT_FOUND);
        const bool cardsLoaded=!g_storage_locked&&(tcard_storage::Required(g_file_path)?tcard::LoadCardSnapshot(g_file_path,g_cards):tcard::LoadCards(g_file_path,g_cards));
        if (!cardsLoaded) {
            if (!g_storage_locked&&!tcard_storage::Required(g_file_path)&&newStore) seed_cards_if_empty();
            else if (g_storage_locked||!tcard_storage::Ready(g_file_path)) MessageBoxW(hwnd,tcard_text(L"storage.locked",L"Encrypted card storage is locked or migration is incomplete. Existing data has been preserved."),L"TCard",MB_OK|MB_ICONERROR);
        }
        if(syncPrepared&&cardsLoaded)g_sync_client.Start(syncSettings);
        if(cardsLoaded)card_signal_request(syncSettings.hostSignal);
        SetTimer(hwnd,kSyncTimer,3000,nullptr);
        SetTimer(hwnd,kTrashTimer,60*1000,nullptr);
        fill_list();
        refresh_preview();
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (lParam) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = 720;
            limits->ptMinTrackSize.y = 480;
            return 0;
        }
        break;
    case WM_SIZE:
        {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            const int listWidth = 288;
            const int contentX = listWidth + 24;
            const int contentWidth = (std::max)(1, width - contentX - 24);
            MoveWindow(g_trash_info,contentX,height-48,contentWidth,32,TRUE);
            MoveWindow(g_search, 20, 44, (std::max)(120, listWidth - 40), 24, TRUE);
            MoveWindow(g_list, 16, 76, (std::max)(120, listWidth - 32), (std::max)(80, height - 136), TRUE);
            const int footerY = height - 48;
            MoveWindow(g_view_button,16,footerY,72,32,TRUE);
            MoveWindow(GetDlgItem(hwnd, kSortField), 96, footerY, 80, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd,kGlobalSettings),184,footerY,88,32,TRUE);
            MoveWindow(g_empty_state, 16, 132, (std::max)(120, listWidth - 32), 24, TRUE);
            MoveWindow(g_clear_search, 16, 168, (std::max)(120, listWidth - 32), 32, TRUE);
            const int titleHeight = title_height();
            MoveWindow(g_detail_title, contentX, 64, contentWidth, titleHeight, TRUE);
            MoveWindow(GetDlgItem(hwnd, kAppearance), contentX, 8, 104, 32, TRUE);
            for (size_t i = 0; i < ARRAYSIZE(kColorChoices); ++i) {
                MoveWindow(GetDlgItem(hwnd, kColorChoices[i].id), contentX + 92 + static_cast<int>(i) * 30, 188, 26, 26, TRUE);
            }
            const int settingsWidth = min(360, contentWidth);
            tcard_ui::layout_setting(GetDlgItem(hwnd, 1030), g_font_family_editor, contentX, 64, settingsWidth, true);
            tcard_ui::layout_setting(GetDlgItem(hwnd, 1031), g_font_size_editor, contentX, 104, settingsWidth, true);
            tcard_ui::layout_setting(GetDlgItem(hwnd, 1032), g_markdown, contentX, 144, settingsWidth, true);
            MoveWindow(GetDlgItem(hwnd, 1033), contentX, 191, 84, 22, TRUE);
            const int editTop = 64;
            const int sourceTop = editTop + 44;
            const int previewTop = 64 + titleHeight + 8;
            MoveWindow(g_preview, contentX, previewTop, contentWidth, max(1, height - previewTop - (g_trash_mode?56:16)), TRUE);
            MoveWindow(g_title_editor, contentX, editTop, contentWidth, 30, TRUE);
            MoveWindow(g_title_line, contentX, g_editing ? editTop + 31 : 64 + titleHeight + 3, contentWidth, 1, TRUE);
            MoveWindow(g_source_editor, contentX, sourceTop, contentWidth, (std::max)(1, height - sourceTop - 16), TRUE);
            update_vertical_scroll(g_preview);
            update_vertical_scroll(g_source_editor);
            MoveWindow(GetDlgItem(hwnd, kDelete), listWidth - 156, 8, 64, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, kNew), listWidth - 84, 8, 68, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, kOpen), width - (g_trash_mode?256:192), 8, 96, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, kEdit), width - (g_trash_mode?152:88), 8, g_trash_mode?128:64, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, kSave), width - 96, 8, 72, 32, TRUE);
            MoveWindow(GetDlgItem(hwnd, kCancel), width - 176, 8, 72, 32, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_NOTIFY:
        return tcard_md::notify(lParam);
    case WM_MEASUREITEM:
        if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlID == kList) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 112;
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->hwndItem == g_title_line) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            SetDCBrushColor(item.hDC,tcard_ui::shade_color(g_preview_color,0.88f));
            FillRect(item.hDC,&item.rcItem,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            return TRUE;
        }
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (const auto* choice = color_choice(item.CtlID)) {
                const LRESULT selected = g_list ? SendMessageW(g_list, LB_GETCURSEL, 0, 0) : -1;
                const bool active = selected >= 0 && static_cast<size_t>(selected) < g_visible_indices.size() &&
                    (g_editing ? g_preview_color : parse_color(g_cards[g_visible_indices[static_cast<size_t>(selected)]].color)) == choice->color;
                tcard_ui::paint_swatch(item, choice->color, active, tile_ink(choice->color));
                return TRUE;
            }
            const COLORREF paper = item.CtlID == kViewButton || item.CtlID == kNew || item.CtlID == kDelete || item.CtlID == kClearSearch || item.CtlID == kSortField || item.CtlID == kGlobalSettings ? RGB(246, 245, 242) : g_preview_color;
            tcard_ui::paint_button(item, paper, tile_ink(paper), g_button_font ? g_button_font : g_ui_font, item.CtlID == kSave || item.CtlID == kNew, item.CtlID == kViewButton && g_trash_mode);
            return TRUE;
        }
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlID == kList) {
            draw_list_item(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if(g_storage_locked)return FALSE;
        if(card_signal_busy()){MessageBoxW(hwnd,tcard_text(L"sync.signal_host_busy",L"Finish editing, image imports and cleanup, then save Sync settings again to start hosting."),L"TCard",MB_OK|MB_ICONINFORMATION);return FALSE;}
        if(LOWORD(wParam)==kViewButton&&HIWORD(wParam)==BN_CLICKED){card_set_view(!g_trash_mode);return 0;}
        if(!lParam&&HIWORD(wParam)==0&&LOWORD(wParam)>=1306&&LOWORD(wParam)<=1309){
            const LRESULT index=selected_index();
            import_clip(LOWORD(wParam)-1300,index>=0?g_cards[static_cast<size_t>(index)].id:L"");
            return 0;
        }
        if (LOWORD(wParam) == kSourceEditor && HIWORD(wParam) == EN_CHANGE) {
            update_vertical_scroll(g_source_editor);
        }
        if(LOWORD(wParam)==kGlobalSettings&&HIWORD(wParam)==BN_CLICKED){
            if(!card_show_settings())return 0;
            const auto language=tcard_lang::g_code;tcard_lang::initialize();
            if(language!=tcard_lang::g_code){
                HFONT font=CreateFontW(tcard_lang::g_code==L"ja"?-12:-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
                if(font){if(g_button_font)DeleteObject(g_button_font);g_button_font=font;}
                refresh_language_ui(hwnd);InvalidateRect(hwnd,nullptr,FALSE);
            }
            tcard_fonts::defaults(true);
            for(const auto& entry:g_card_windows)if(entry->handle)
                SendMessageW(g_get_card_window(entry->handle),tcard_fonts::refreshMessage,0,0);
            if(g_editing){
                const bool family=tcard_fonts::inherited(g_font_family_editor),size=tcard_fonts::inherited(g_font_size_editor);
                tcard_fonts::inheritance(g_font_family_editor,false,family);
                tcard_fonts::inheritance(g_font_size_editor,true,size);
                double points=0;if(!g_editor_markdown&&tcard_fonts::read_size(g_font_size_editor,points))
                    tcard_rich_edit::configure(g_source_editor,true,points,tcard_fonts::resolved_family(g_font_family_editor),g_preview_color,RGB(37,37,37),true);
            }
            refresh_preview();update_open_card();return 0;
        }
        if (LOWORD(wParam) == kSortField && HIWORD(wParam) == BN_CLICKED) {
            show_sort();
            return 0;
        }
        if (LOWORD(wParam) == kSearch && HIWORD(wParam) == EN_CHANGE) { fill_list(); refresh_preview(); }
        else if (LOWORD(wParam) == kList && HIWORD(wParam) == LBN_SELCHANGE) { if (g_editing) set_editing(false); refresh_preview(); }
        else if (LOWORD(wParam) == kList && HIWORD(wParam) == LBN_DBLCLK) { if(g_trash_mode)card_restore_trash();else open_selected(); }
        else if (LOWORD(wParam) == kOpen) { if(g_trash_mode)card_restore_trash();else open_selected(); }
        else if (LOWORD(wParam) == kEdit) { if(g_trash_mode)card_remove_trash(false);else set_editing(true); }
        else if (LOWORD(wParam) == kDelete) { if(!g_trash_mode)delete_selected(); }
        else if (LOWORD(wParam) == kSave) save_edit();
        else if (LOWORD(wParam) == kCancel) set_editing(false);
        else if (LOWORD(wParam) == kClearSearch) {
            SetWindowTextW(g_search, L"");
            fill_list();
            refresh_preview();
        }
        else if (LOWORD(wParam) == kAppearance && HIWORD(wParam) == BN_CLICKED) {
            g_appearance_open = !g_appearance_open;
            ShowWindow(g_font_family_editor, g_appearance_open ? SW_SHOW : SW_HIDE);
            ShowWindow(g_font_size_editor, g_appearance_open ? SW_SHOW : SW_HIDE);
            ShowWindow(g_markdown, g_appearance_open ? SW_SHOW : SW_HIDE);
            for (int id : {1030, 1031, 1032, 1033}) ShowWindow(GetDlgItem(hwnd, id), g_appearance_open ? SW_SHOW : SW_HIDE);
            ShowWindow(g_title_editor, g_appearance_open ? SW_HIDE : SW_SHOW);
            ShowWindow(g_title_line, g_appearance_open ? SW_HIDE : SW_SHOW);
            ShowWindow(g_source_editor, g_appearance_open ? SW_HIDE : SW_SHOW);
            SetWindowTextW(GetDlgItem(hwnd, kAppearance), g_appearance_open ? tcard_text(L"button.back_to_note", L"Back to note") : tcard_text(L"button.appearance", L"Appearance"));
            for (const auto& choice : kColorChoices) ShowWindow(GetDlgItem(hwnd, choice.id), g_appearance_open ? SW_SHOW : SW_HIDE);
            RECT client{};
            GetClientRect(hwnd, &client);
            SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
        }
        else if (LOWORD(wParam) == kNew) {
            if (g_editing) return 0;
            if(g_trash_mode)card_set_view(false);
            tcard::CardRecord card;
            tcard_apply_defaults(card);
            card.id = std::to_wstring(GetTickCount64());
            card.title = L"New";
            card.richHtml=!card.markdown;
            card.source=card.richHtml?tcard_rich::serialize(tcard_rich::plain(L"")):L"";
            card.color = L"#FFF5A8";
            std::vector<tcard::CardRecord> created{card};
            if (!card_save_records(created)) {
                MessageBoxW(g_main, tcard_text(L"message.create_failed", L"Could not create a note. Check the storage location."), tcard_lang::text(L"app.title", L"TCard").c_str(), MB_OK | MB_ICONERROR);
                return 0;
            }
            g_cards.insert(g_cards.begin(),created[0]);
            SetWindowTextW(g_search, L"");
            fill_list();
            for (size_t row = 0; row < g_visible_indices.size(); ++row) {
                if (g_cards[g_visible_indices[row]].id == card.id) {
                    SendMessageW(g_list, LB_SETCURSEL, row, 0);
                    break;
                }
            }
            refresh_preview();
            open_selected();
        }
        else if (HIWORD(wParam) == BN_CLICKED) {
            if (const auto* choice = color_choice(LOWORD(wParam))) choose_color(choice->color);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        g_main_moving=true;return 0;
    case WM_EXITSIZEMOVE:
        g_main_moving=false;card_resume_updates();return 0;
    case WM_TIMER:
        if(wParam==kSignalTimer){card_signal_complete(hwnd);if(g_signal_pending&&!g_signal_task&&(IsWindowEnabled(hwnd)||g_signal_stop_pending))card_signal_reconcile(hwnd);if(!g_signal_task&&g_signal_child&&g_signal_child->Exited()){g_signal_child.reset();g_signal_runtime.reset();g_signal_product.reset();g_signal_pin.clear();g_signal_status=L"sync.signal_host_failed";}if(!g_signal_task&&!g_signal_child&&!g_signal_pending)KillTimer(hwnd,kSignalTimer);return 0;}
        if(g_storage_locked||(card_signal_busy()&&wParam!=kSignalTimer))return 0;
        // The modal move loop dispatches timers on this UI thread. Keep disk/import work out of it.
        // The separate sync worker and prepared IPC jobs continue; apply them after release.
        if((wParam==kTimer||wParam==kSyncTimer||wParam==kClipTimer)&&card_is_moving())return 0;
        if(wParam==kSyncTimer){card_sync_poll(hwnd);return 0;}
        if(wParam==kTrashTimer){card_purge_trash(hwnd);return 0;}
        if (wParam == kClipTimer && g_clip_job && g_clip_job->done.load() && IsWindowEnabled(hwnd)) {
            // Do not commit from a nested modal message loop while another action holds card state.
            for (const auto& entry : g_card_windows) {
                const HWND card = entry->handle && g_get_card_window ? g_get_card_window(entry->handle) : nullptr;
                if (card && !IsWindowEnabled(card)) return 0;
            }
            auto job = std::move(g_clip_job);
            if (job->worker.joinable()) job->worker.join();
            KillTimer(hwnd, kClipTimer);
            SetWindowTextW(hwnd, tcard_text(L"app.title", L"TCard"));
            import_clip(job->command, job->target, &job->clip, &job->previous);
        }
        if (wParam == kTimer) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            tcard::RenderBatch batch(now);
            tcard::RefreshCustomVariables();
            refresh_preview();
            update_open_card();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd,kSignalTimer);card_signal_shutdown();
        KillTimer(hwnd,kTrashTimer);g_trash_task.reset();
        KillTimer(hwnd,kSyncTimer);
        g_sync_client.Stop();
        KillTimer(hwnd, kClipTimer);
        if (g_clip_job) { g_clip_job->cancelled.store(true); if (g_clip_job->worker.joinable()) g_clip_job->worker.join(); g_clip_job.reset(); }
        KillTimer(hwnd, kTimer);
        for (auto& entry : g_card_windows) if (entry->handle) g_destroy_card(entry->handle);
        g_card_windows.clear();
        if (g_destroy) g_destroy();
        if (g_renderer) FreeLibrary(g_renderer);
        if (g_rich_edit) FreeLibrary(g_rich_edit);
        if (g_preview_brush) DeleteObject(g_preview_brush);
        if (g_detail_title_font) DeleteObject(g_detail_title_font);
        if (g_heading_font) DeleteObject(g_heading_font);
        if (g_tile_title_font) DeleteObject(g_tile_title_font);
        if (g_tile_excerpt_font) DeleteObject(g_tile_excerpt_font);
        if (g_button_font) DeleteObject(g_button_font);
        if (g_ui_font) DeleteObject(g_ui_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // Claim this storage root before defaults, migration, native reads or worker startup.
    CardOwner owner;const int acquired=owner.acquire();if(acquired<=0)return acquired==0?0:1;
    const HRESULT ole = OleInitialize(nullptr);
    struct OleScope { bool active; ~OleScope() { if (active) OleUninitialize(); } } oleScope{SUCCEEDED(ole)};
    tcard_initialize_defaults();
    auto release_icons = []() {
        if (g_owns_app_icon && g_app_icon) DestroyIcon(g_app_icon);
        if (g_owns_app_icon_small && g_app_icon_small && g_app_icon_small != g_app_icon) DestroyIcon(g_app_icon_small);
        g_app_icon = nullptr;
        g_app_icon_small = nullptr;
        g_owns_app_icon = false;
        g_owns_app_icon_small = false;
    };
    const int icon_width = GetSystemMetrics(SM_CXICON);
    const int icon_height = GetSystemMetrics(SM_CYICON);
    const int icon_small_width = GetSystemMetrics(SM_CXSMICON);
    const int icon_small_height = GetSystemMetrics(SM_CYSMICON);
    g_app_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, icon_width, icon_height, 0));
    g_owns_app_icon = g_app_icon != nullptr;
    g_app_icon_small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, icon_small_width, icon_small_height, 0));
    g_owns_app_icon_small = g_app_icon_small != nullptr;
    const std::wstring iconPath = module_dir() + L"\\icon3.ico";
    if (!g_app_icon) {
        g_app_icon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, icon_width, icon_height, LR_LOADFROMFILE));
        g_owns_app_icon = g_app_icon != nullptr;
    }
    if (!g_app_icon_small) {
        g_app_icon_small = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, icon_small_width, icon_small_height, LR_LOADFROMFILE));
        g_owns_app_icon_small = g_app_icon_small != nullptr;
    }
    if (!g_app_icon) g_app_icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_app_icon_small) g_app_icon_small = LoadIconW(nullptr, IDI_APPLICATION);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = instance;
    wc.lpfnWndProc = wnd_proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = g_app_icon;
    wc.hIconSm = g_app_icon_small;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TCardNativeHost";
    if (!RegisterClassExW(&wc)) {
        release_icons();
        return 1;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, tcard_text(L"app.title", L"TCard"), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        release_icons();
        return 1;
    }
    if(!SetPropW(hwnd,owner.name.c_str(),reinterpret_cast<HANDLE>(1))){DestroyWindow(hwnd);release_icons();return 1;}
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_app_icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_app_icon_small));
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG message{};bool running=true;int exitCode=0;
    while(running){
        const auto wake=MsgWaitForMultipleObjectsEx(1,&owner.activation,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(wake==WAIT_OBJECT_0)card_focus_window(hwnd);
        else if(wake!=WAIT_OBJECT_0+1){DestroyWindow(hwnd);exitCode=1;break;}
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT){exitCode=static_cast<int>(message.wParam);running=false;break;}
            HWND dialog = message.hwnd ? GetAncestor(message.hwnd, GA_ROOT) : hwnd;
            if (tcard_edit::translate(message)) continue;
            if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) &&
                !(GetKeyState(VK_MENU) & 0x8000) && !tcard_edit::composing(GetFocus())) {
                if (message.wParam == 'S') {
                    if (dialog == hwnd) { if (g_editing) save_edit(); }
                    else SendMessageW(dialog, WM_COMMAND, 5003, 0);
                    continue;
                }
                if (dialog == hwnd && !g_editing && message.wParam == 'N') {
                    SendMessageW(hwnd, WM_COMMAND, kNew, 0);
                    continue;
                }
                if (dialog == hwnd && !g_editing && message.wParam == 'F') {
                    SetFocus(g_search);
                    SendMessageW(g_search, EM_SETSEL, 0, -1);
                    continue;
                }
            }
            if (!dialog || !IsDialogMessageW(dialog, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    release_icons();
    return exitCode;
}
