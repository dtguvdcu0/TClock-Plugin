#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#ifndef TCM_SETBKCOLOR
#define TCM_SETBKCOLOR (TCM_FIRST + 29)
#endif
#ifndef EM_SETBKGNDCOLOR
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <cwctype>

#include "settings.h"
#include "open_app.h"
#include "resource.h"
#include "../common/native_edit.h"

namespace fs = std::filesystem;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_APP_CAPTURE_REQUEST = WM_APP + 2;
constexpr UINT WM_APP_SHOW_SETTINGS = WM_APP + 3;
constexpr UINT WM_APP_CAPTURE_FINISHED = WM_APP + 4;
constexpr UINT WM_APP_SET_LANGUAGE = WM_APP + 5;
constexpr UINT ID_TRAY_SETTINGS = 2001;
constexpr UINT ID_TRAY_OPEN_OUTPUT = 2003;
constexpr UINT ID_TRAY_EXIT = 2004;
constexpr UINT ID_TRAY_CAPTURE_BASE = 2100; // per profile
constexpr UINT HOTKEY_ID_BASE = 3000;      // per profile
constexpr UINT TIMER_AUTO_BASE = 4000;     // per profile
constexpr UINT ID_TRAY_LANG_BASE = 5100;
constexpr int kCtrlProfileInlineRename = 143;
static const wchar_t* kAgentMainWindowClass = L"TCaptureAgentWindow";
static const wchar_t* kSettingsWindowClass = L"TCaptureSettings";
static const wchar_t* kSingletonMutexName = L"Local\\TCaptureMutex";
struct HotkeySpec {
    UINT modifiers = 0;
    UINT key = 0;
};

struct AppState {
    HINSTANCE hInstance = nullptr;
    HWND mainWindow = nullptr;
    HWND settingsWindow = nullptr;
    NOTIFYICONDATA nid{};
    fs::path iniPath;
    fs::path tclockIniPath;
    fs::path exeDir;
    std::string language = "en";
    std::unordered_map<std::wstring, std::wstring> translations;
    std::vector<std::string> availableLanguages;
    std::vector<ProfileSettings> profiles;
    std::vector<UINT> hotkeyIds; // 0 if not registered
    std::vector<HotkeySpec> hotkeySpecs;
    struct PrintScreenHotkey {
        int index = -1;
        UINT modifiers = 0;
        std::string profileName;
    };
    std::vector<PrintScreenHotkey> printScreenHotkeys;
    size_t printScreenHotkeyCount = 0;
    std::atomic<int> activeCaptureJobs{0};
    std::vector<UINT_PTR> autoTimerIds; // 0 if not registered
    bool enableTray = false;
    bool startWithSettings = false;
};

// Forward declarations
std::wstring translateId(const AppState& app, const std::wstring& id, const std::wstring& fallback);
bool loadLanguage(AppState& app, const std::string& code);
void setLanguage(AppState& app, const std::string& langCode);
void applyLanguageToProfiles(AppState& app);
HWND showSettingsWindow(AppState& app);
std::vector<std::string> listAvailableLanguages(const fs::path& root);

static fs::path getLanguageRoot(const fs::path& exeDir);
std::wstring languageLabel(const std::string& code);
void updateStatusBrush(struct SettingsDialog* dlg);
LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);

int RunCaptureMain(int argc, char* argv[]);

static HHOOK g_keyboardHook = nullptr;
static AppState* g_hookApp = nullptr;

void writeDefaultSettingsIni(const fs::path& path) {
    static const char* kDefaultIni =
        "[Default]\r\n"
        "output_dir=\r\n"
        "format=jpg\r\n"
        "compression_png=9\r\n"
        "compression_jpg=96\r\n"
        "compression=96\r\n"
        "burst_fps=0\r\n"
        "burst_seconds=1\r\n"
        "auto_capture=false\r\n"
        "auto_seconds=60\r\n"
        "displays=0\r\n"
        "\r\n"
        "[Active Window]\r\n"
        "output_dir=\r\n"
        "format=png\r\n"
        "compression_png=6\r\n"
        "compression_jpg=96\r\n"
        "compression=6\r\n"
        "burst_fps=0\r\n"
        "burst_seconds=1\r\n"
        "auto_capture=false\r\n"
        "auto_seconds=60\r\n"
        "displays=active_window\r\n"
        "\r\n"
        "[Active Display]\r\n"
        "output_dir=\r\n"
        "format=png\r\n"
        "compression_png=6\r\n"
        "compression_jpg=96\r\n"
        "burst_fps=0\r\n"
        "burst_seconds=1\r\n"
        "auto_capture=false\r\n"
        "auto_seconds=60\r\n"
        "displays=active_display\r\n"
        "\r\n"
        "[Integration]\r\n"
        "output_dir=.\r\n"
        "format=png\r\n"
        "compression_png=6\r\n"
        "compression_jpg=94\r\n"
        "burst_fps=0\r\n"
        "burst_seconds=1\r\n"
        "auto_capture=false\r\n"
        "auto_seconds=60\r\n"
        "displays=1,3\r\n"
;
    fs::path parent = path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return;
    out.write(kDefaultIni, static_cast<std::streamsize>(std::strlen(kDefaultIni)));
}


struct SettingsDialog {
    AppState* app = nullptr;
    HWND hwnd = nullptr;
    HWND panel = nullptr;
    HWND actionCombo = nullptr, openAppCombo = nullptr;
    std::vector<TcapAppChoice> openApps;
    std::string appsFormat;
    HFONT uiFont = nullptr, titleFont = nullptr, smallFont = nullptr;
    bool detailsOpen = false, arranging = false;
    int targetMode = 0, scrollOffset = 0, contentHeight = 0;
    UINT uiDpi = 96;
    struct Placement { HWND window; int x, y, width, height; bool visible; };
    std::vector<Placement> placements;
    std::vector<int> separators;
    HWND tab = nullptr;
    HWND addBtn = nullptr;
    HWND deleteBtn = nullptr;
    HWND renameBtn = nullptr;
    HWND captureBtn = nullptr;
    HWND outputEdit = nullptr;
    HWND formatCombo = nullptr;
    HWND compressionLabelPng = nullptr;
    HWND compressionLabelJpg = nullptr;
    HWND compressionEditPng = nullptr;
    HWND compressionEditJpg = nullptr;
    HWND burstFpsCombo = nullptr;
    HWND burstSecondsEdit = nullptr;
    HWND autoCaptureCheck = nullptr;
    HWND autoIntervalEdit = nullptr;
    HWND displaysEdit = nullptr;
    HWND displaysHelpBtn = nullptr;
    HWND hotkeyModCombo = nullptr;
    HWND hotkeyKeyCombo = nullptr;
    HWND autoStatusLabel = nullptr;
    bool autoStatusActive = false;
    HWND tooltip = nullptr;
    std::list<std::wstring> tooltipTexts;
    HBRUSH statusBrushActive = nullptr;
    HBRUSH statusBrushInactive = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBackgroundBrush = nullptr;
    HBRUSH listBackgroundBrush = nullptr;
    COLORREF backgroundColor = 0;
    COLORREF editBackgroundColor = 0;
    COLORREF listBackgroundColor = 0;
    int layoutClientWidth = 0;
    int layoutClientHeight = 0;
    int activeProfile = 0;
    bool suppressSave = false;
    HWND inlineEdit = nullptr;
    int inlineEditIndex = -1;
    bool inlineFinalizing = false;
    WNDPROC tabOldProc = nullptr;
    WNDPROC inlineEditOldProc = nullptr;
};

using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
static SetWindowThemeFn g_setWindowTheme = nullptr;

void ensureThemeProc() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (!theme) return;
    g_setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(theme, "SetWindowTheme"));
}

void applyControlTheme(SettingsDialog* dlg, HWND hwnd, bool setEditBackground) {
    if (!dlg || !hwnd) return;
    ensureThemeProc();
    if (g_setWindowTheme) {
        g_setWindowTheme(hwnd, L"", L"");
    }
    if (setEditBackground) {
        SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(dlg->editBackgroundColor));
    }
}

void tcap_sync_controls(SettingsDialog* dlg);
void tcap_fill_apps(SettingsDialog* dlg, const std::wstring& selected, const std::wstring& executable);
void tcap_layout_controls(SettingsDialog* dlg,bool scrolling = false);
void tcap_refresh_fonts(SettingsDialog* dlg);
bool tcap_draw_item(SettingsDialog* dlg, const DRAWITEMSTRUCT& item);
void tcap_paint_surface(SettingsDialog* dlg, HWND window, HDC dc);
bool tcap_handle_command(SettingsDialog* dlg, WPARAM wp, LPARAM lp);

void adjustSettingsWindowSize(SettingsDialog* dlg) {
    if(!dlg||!dlg->hwnd)return;
    const UINT dpi=GetDpiForWindow(dlg->hwnd);
    RECT rect{0,0,MulDiv(760,dpi,96),MulDiv(504,dpi,96)};
    AdjustWindowRectExForDpi(&rect,static_cast<DWORD>(GetWindowLongPtrW(dlg->hwnd,GWL_STYLE)),FALSE,0,dpi);
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(dlg->hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
    RECT current{};GetWindowRect(dlg->hwnd,&current);
    const int width=std::min(rect.right-rect.left,monitor.rcWork.right-monitor.rcWork.left);
    const int height=std::min(rect.bottom-rect.top,monitor.rcWork.bottom-monitor.rcWork.top);
    SetWindowPos(dlg->hwnd,nullptr,std::clamp(current.left,monitor.rcWork.left,monitor.rcWork.right-width),
        std::clamp(current.top,monitor.rcWork.top,monitor.rcWork.bottom-height),width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}

bool namesEqual(const std::string& a, const std::string& b);

std::string displaysToString(const AppSettings& settings) {
    if (!settings.displaysRaw.empty()) return settings.displaysRaw;
    if (settings.displayIndices.empty()) return "";
    std::string s;
    for (size_t i = 0; i < settings.displayIndices.size(); ++i) {
        if (i > 0) s += ",";
        s += std::to_string(settings.displayIndices[i] + 1);
    }
    return s;
}

std::string trimCopy(const std::string& text) {
    return trim(text);
}

std::wstring trimCopy(const std::wstring& text) {
    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\u3000';
    };
    size_t start = 0;
    size_t end = text.size();
    while (start < end && isSpace(text[start])) ++start;
    while (end > start && isSpace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

std::wstring replaceEscapes(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            if (text[i + 1] == L'n') {
                result.push_back(L'\n');
                ++i;
                continue;
            }
            if (text[i + 1] == L'\\') {
                // Allow both "\n" and "\\n" from config files.
                if (i + 2 < text.size() && text[i + 2] == L'n') {
                    result.push_back(L'\n');
                    i += 2;
                    continue;
                }
                result.push_back(L'\\');
                ++i;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

std::wstring stripQuotes(const std::wstring& text) {
    std::wstring t = trimCopy(text);
    if (t.size() >= 2) {
        wchar_t first = t.front();
        wchar_t last = t.back();
        if ((first == L'"' && last == L'"') || (first == L'\'' && last == L'\'')) {
            return t.substr(1, t.size() - 2);
        }
    }
    return t;
}

bool parseHotkey(const std::string& text, HotkeySpec& spec) {
    std::string lower = toLower(trimCopy(text));
    if (lower.empty()) return false;

    UINT modifiers = 0;
    UINT vk = 0;
    std::string token;
    auto flushToken = [&](const std::string& raw) {
        std::string t = trimCopy(raw);
        if (t.empty()) return;
        if (t == "ctrl" || t == "control") {
            modifiers |= MOD_CONTROL;
        } else if (t == "shift") {
            modifiers |= MOD_SHIFT;
        } else if (t == "alt") {
            modifiers |= MOD_ALT;
        } else if (t == "win" || t == "windows") {
            modifiers |= MOD_WIN;
        } else {
            if (vk != 0) return;
            if (t.size() == 1) {
                char c = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
                if (c >= 'A' && c <= 'Z') vk = static_cast<UINT>(c);
                else if (c >= '0' && c <= '9') vk = static_cast<UINT>(c);
            } else if (t[0] == 'f' && t.size() <= 3) {
                int fn = 0;
                try { fn = std::stoi(t.substr(1)); } catch (...) { fn = 0; }
                if (fn >= 1 && fn <= 24) vk = VK_F1 + static_cast<UINT>(fn - 1);
            } else if (t == "printscreen" || t == "prtsc" || t == "prtscr") {
                vk = VK_SNAPSHOT;
            } else if (t == "insert") {
                vk = VK_INSERT;
            } else if (t == "delete") {
                vk = VK_DELETE;
            } else if (t == "home") {
                vk = VK_HOME;
            } else if (t == "end") {
                vk = VK_END;
            } else if (t == "pgup" || t == "pageup") {
                vk = VK_PRIOR;
            } else if (t == "pgdn" || t == "pagedown") {
                vk = VK_NEXT;
            } else if (t == "space" || t == "spacebar") {
                vk = VK_SPACE;
            } else if (t == "enter" || t == "return") {
                vk = VK_RETURN;
            }
        }
    };

    for (char c : lower) {
        if (c == '+') {
            flushToken(token);
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    flushToken(token);

    if (vk == 0) return false;
    spec.modifiers = modifiers | MOD_NOREPEAT;
    spec.key = vk;
    return true;
}

static bool buildHotkeySignature(const std::string& text, std::string& outSig) {
    std::string hotkey = trimCopy(text);
    if (hotkey.empty()) {
        outSig.clear();
        return false;
    }
    HotkeySpec spec;
    if (parseHotkey(hotkey, spec)) {
        UINT mods = spec.modifiers;
#ifdef MOD_NOREPEAT
        mods &= ~MOD_NOREPEAT;
#endif
        outSig = std::to_string(mods) + ":" + std::to_string(spec.key);
        return true;
    }
    outSig = toLower(hotkey);
    return true;
}

void showNotification(AppState& app, const std::wstring& title, const std::wstring& message, DWORD infoFlag = NIIF_INFO) {
    NOTIFYICONDATA data = app.nid;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, std::size(data.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, std::size(data.szInfo), message.c_str(), _TRUNCATE);
    data.dwInfoFlags = infoFlag;
    Shell_NotifyIcon(NIM_MODIFY, &data);
}

void buildTrayIcon(AppState& app) {
    app.nid = {};
    app.nid.cbSize = sizeof(NOTIFYICONDATA);
    app.nid.hWnd = app.mainWindow;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAYICON;
    app.nid.hIcon = static_cast<HICON>(LoadImageW(app.hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                  0, 0, LR_DEFAULTSIZE));
    if (!app.nid.hIcon) {
        app.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    wcsncpy_s(app.nid.szTip, std::size(app.nid.szTip), L"TCapture", _TRUNCATE);
    Shell_NotifyIcon(NIM_ADD, &app.nid);
}

std::wstring buildCommandLine(const fs::path& exePath, const std::string& profileName) {
    std::wstring cmd = L"\"";
    cmd += exePath.wstring();
    cmd += L"\" --profile \"";
    cmd += utf8ToWide(profileName);
    cmd += L"\"";
    return cmd;
}

bool triggerCapture(AppState& app, int profileIndex) {
    if (profileIndex < 0 || profileIndex >= static_cast<int>(app.profiles.size())) return false;
    const auto profileName = app.profiles[profileIndex].name;
    app.activeCaptureJobs.fetch_add(1, std::memory_order_relaxed);

    std::thread([&app, profileName]() {
        std::vector<std::string> args;
        args.push_back("TCapture.exe");
        args.push_back("--capture");
        args.push_back("--profile");
        args.push_back(profileName);

        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& s : args) {
            argv.push_back(const_cast<char*>(s.c_str()));
        }
        (void)RunCaptureMain(static_cast<int>(argv.size()), argv.data());
        app.activeCaptureJobs.fetch_sub(1, std::memory_order_relaxed);
        if (app.mainWindow) {
            PostMessageW(app.mainWindow, WM_APP_CAPTURE_FINISHED, 0, 0);
        }
    }).detach();

    return true;
}


fs::path resolveTClockIniPath(AppState& app) {
    if (!app.tclockIniPath.empty()) {
        return app.tclockIniPath;
    }

    fs::path tcapIniPath = app.iniPath.empty() ? (app.exeDir / "TCapture.ini") : app.iniPath;
    fs::path baseDir = tcapIniPath.parent_path().empty() ? app.exeDir : tcapIniPath.parent_path();
    std::wstring tcapIniW = tcapIniPath.wstring();
    wchar_t configured[MAX_PATH] = {};

    GetPrivateProfileStringW(L"Integration", L"TClockIniPath", L"", configured, MAX_PATH, tcapIniW.c_str());

    fs::path resolved;
    if (configured[0] != L'\0') {
        resolved = fs::path(configured);
        if (resolved.is_relative()) {
            resolved = baseDir / resolved;
        }
    }

    if (resolved.empty()) {
        fs::path exeLocal = app.exeDir / "tclock-win11.ini";
        if (fs::exists(exeLocal)) {
            resolved = exeLocal;
        } else {
            resolved = app.exeDir.parent_path() / "tclock-win11.ini";
        }
    }

    app.tclockIniPath = resolved;
    return app.tclockIniPath;
}

static bool readTextLinesPreserve(const fs::path& path, std::vector<std::string>& lines, bool& hasBom, bool& useCrLf)
{
    lines.clear();
    hasBom = false;
    useCrLf = true;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        hasBom = true;
        bytes.erase(0, 3);
    }
    useCrLf = (bytes.find("\r\n") != std::string::npos);

    std::string cur;
    for (char c : bytes) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
    }
    return true;
}

static bool writeTextLinesPreserve(const fs::path& path, const std::vector<std::string>& lines, bool hasBom, bool useCrLf)
{
    fs::path parent = path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    if (hasBom) {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        out.write(reinterpret_cast<const char*>(bom), 3);
    }

    const char* nl = useCrLf ? "\r\n" : "\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << nl;
    }
    out << nl;
    return true;
}

static bool parseIniKeyValue(const std::string& line, std::string& keyLower, std::string& value)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    keyLower = toLower(trimCopy(line.substr(0, eq)));
    value = trimCopy(line.substr(eq + 1));
    return !keyLower.empty();
}

static bool isSectionHeader(const std::string& line, std::string& sectionLower)
{
    std::string t = trimCopy(line);
    if (t.size() < 3) return false;
    if (t.front() != '[' || t.back() != ']') return false;
    sectionLower = toLower(trimCopy(t.substr(1, t.size() - 2)));
    return !sectionLower.empty();
}

static bool isManagedHotkeyKey(const std::string& keyLower)
{
    if (keyLower == "hotkeycount") return true;
    if (keyLower.rfind("hotkey", 0) != 0) return false;
    size_t i = 6;
    while (i < keyLower.size() && std::isdigit(static_cast<unsigned char>(keyLower[i]))) ++i;
    if (i == 6) return false;
    std::string tail = keyLower.substr(i);
    return (tail == "profile" || tail == "value");
}

void loadHotkeysFromTClockIni(AppState& app) {
    fs::path iniPath = resolveTClockIniPath(app);
    if (iniPath.empty() || !fs::exists(iniPath)) return;

    std::vector<std::string> lines;
    bool hasBom = false;
    bool useCrLf = true;
    if (!readTextLinesPreserve(iniPath, lines, hasBom, useCrLf)) return;

    bool inSection = false;
    int count = 0;
    std::unordered_map<int, std::string> profiles;
    std::unordered_map<int, std::string> hotkeys;

    for (const auto& raw : lines) {
        std::string sec;
        if (isSectionHeader(raw, sec)) {
            inSection = (sec == "tcapture");
            continue;
        }
        if (!inSection) continue;

        std::string keyLower, value;
        if (!parseIniKeyValue(raw, keyLower, value)) continue;
        if (keyLower == "hotkeycount") {
            try { count = std::stoi(value); } catch (...) { count = 0; }
            continue;
        }
        if (keyLower.rfind("hotkey", 0) != 0) continue;
        size_t i = 6;
        while (i < keyLower.size() && std::isdigit(static_cast<unsigned char>(keyLower[i]))) ++i;
        if (i == 6) continue;

        int idx = 0;
        try { idx = std::stoi(keyLower.substr(6, i - 6)); } catch (...) { idx = 0; }
        if (idx <= 0) continue;
        std::string tail = keyLower.substr(i);
        if (tail == "profile") profiles[idx] = value;
        else if (tail == "value") hotkeys[idx] = value;
    }

    if (count <= 0) {
        for (const auto& kv : profiles) count = std::max(count, kv.first);
        for (const auto& kv : hotkeys) count = std::max(count, kv.first);
    }
    if (count <= 0) return;

    std::unordered_map<std::string, std::string> map;
    for (int i = 1; i <= count; ++i) {
        auto itName = profiles.find(i);
        auto itHot = hotkeys.find(i);
        if (itName == profiles.end() || itHot == hotkeys.end()) continue;
        std::string profileName = trimCopy(itName->second);
        std::string hotkey = trimCopy(itHot->second);
        if (!profileName.empty() && !hotkey.empty()) {
            map[toLower(profileName)] = hotkey;
        }
    }

    for (auto& p : app.profiles) {
        auto it = map.find(toLower(p.name));
        if (it != map.end()) {
            p.settings.captureHotkey = it->second;
        }
    }
}

bool saveHotkeysToTClockIni(const AppState& app) {
    fs::path iniPath = app.tclockIniPath.empty() ? (app.exeDir / "tclock-win11.ini") : app.tclockIniPath;
    if (iniPath.empty()) return false;

    std::vector<std::string> lines;
    bool hasBom = false;
    bool useCrLf = true;
    if (!readTextLinesPreserve(iniPath, lines, hasBom, useCrLf)) {
        lines.clear();
        hasBom = true;
        useCrLf = true;
    }

    int secStart = -1;
    int secEnd = static_cast<int>(lines.size());
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string sec;
        if (isSectionHeader(lines[i], sec)) {
            if (sec == "tcapture") {
                secStart = i;
                secEnd = static_cast<int>(lines.size());
                for (int j = i + 1; j < static_cast<int>(lines.size()); ++j) {
                    std::string sec2;
                    if (isSectionHeader(lines[j], sec2)) {
                        secEnd = j;
                        break;
                    }
                }
                break;
            }
        }
    }

    if (secStart < 0) {
        if (!lines.empty() && !trimCopy(lines.back()).empty()) lines.push_back("");
        secStart = static_cast<int>(lines.size());
        lines.push_back("[TCapture]");
        secEnd = static_cast<int>(lines.size());
    }

    std::vector<std::string> preserved;
    for (int i = secStart + 1; i < secEnd; ++i) {
        std::string keyLower, value;
        if (parseIniKeyValue(lines[i], keyLower, value) && isManagedHotkeyKey(keyLower)) {
            continue;
        }
        preserved.push_back(lines[i]);
    }

    std::vector<std::string> generated;
    int index = 0;
    std::unordered_set<std::string> seenSignatures;
    for (const auto& p : app.profiles) {
        std::string hotkey = trimCopy(p.settings.captureHotkey);
        if (hotkey.empty()) continue;
        std::string sig;
        if (buildHotkeySignature(hotkey, sig)) {
            if (!sig.empty() && seenSignatures.find(sig) != seenSignatures.end()) {
                continue;
            }
            if (!sig.empty()) seenSignatures.insert(sig);
        }
        ++index;
        generated.push_back("Hotkey" + std::to_string(index) + "Profile=" + p.name);
        generated.push_back("Hotkey" + std::to_string(index) + "Value=" + hotkey);
    }
    generated.insert(generated.begin(), "HotkeyCount=" + std::to_string(index));

    std::vector<std::string> out;
    out.reserve(lines.size() + generated.size() + 8);
    for (int i = 0; i <= secStart; ++i) out.push_back(lines[i]);
    for (const auto& l : preserved) out.push_back(l);
    for (const auto& l : generated) out.push_back(l);
    for (int i = secEnd; i < static_cast<int>(lines.size()); ++i) out.push_back(lines[i]);

    return writeTextLinesPreserve(iniPath, out, hasBom, useCrLf);
}

bool hasAnyAutoCaptureEnabled(const AppState& app) {
    for (const auto& p : app.profiles) {
        if (p.settings.autoCapture && p.settings.autoSeconds > 0) {
            return true;
        }
    }
    return false;
}

static bool hasAnyAutoCaptureEnabledInIni(const AppState& app)
{
    if (app.iniPath.empty()) return false;
    auto profiles = loadSettingsProfiles(app.iniPath);
    for (const auto& p : profiles) {
        if (p.settings.autoCapture && p.settings.autoSeconds > 0) {
            return true;
        }
    }
    return false;
}

static void maybeStopAgentIfIdle(AppState& app)
{
    if (app.settingsWindow) return;
    if (app.activeCaptureJobs.load(std::memory_order_relaxed) > 0) return;
    if (hasAnyAutoCaptureEnabledInIni(app)) return;
    if (app.startWithSettings) {
        if (app.mainWindow) {
            PostMessageW(app.mainWindow, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (app.mainWindow) {
        PostMessageW(app.mainWindow, WM_CLOSE, 0, 0);
    }
}
bool loadProfilesWithFallback(AppState& app, const std::string& preferredLanguage = "") {
    fs::path primary = app.exeDir / "TCapture.ini";
    fs::path cwdIni = fs::current_path() / "TCapture.ini";
    if (!fs::exists(primary) && !fs::exists(cwdIni)) {
        writeDefaultSettingsIni(primary);
    }
    auto profiles = loadSettingsProfiles(primary);
    std::string globalLang = readGlobalLanguage(primary);
    app.iniPath = primary;
    if (profiles.empty()) {
        profiles = loadSettingsProfiles(cwdIni);
        globalLang = readGlobalLanguage(cwdIni);
        if (!profiles.empty()) {
            app.iniPath = cwdIni;
        }
    }
    if (profiles.empty()) {
        ProfileSettings p;
        p.name = "default";
        normalizeSettings(p.settings);
        profiles.push_back(p);
    }
    for (auto& p : profiles) {
        normalizeSettings(p.settings);
    }
    app.profiles = profiles;
    if (!globalLang.empty()) {
        app.language = toLower(globalLang);
    } else {
        std::string pref = toLower(preferredLanguage);
        if (pref == "ja" || pref == "en") {
            app.language = pref;
        } else {
            app.language = "en";
        }
    }
    app.availableLanguages = listAvailableLanguages(getLanguageRoot(app.exeDir));
    loadLanguage(app, app.language);
    applyLanguageToProfiles(app);
    app.hotkeyIds.assign(app.profiles.size(), 0);
    app.hotkeySpecs.assign(app.profiles.size(), HotkeySpec{});
    app.printScreenHotkeys.clear();
    app.printScreenHotkeyCount = 0;
    app.autoTimerIds.assign(app.profiles.size(), 0);
    loadHotkeysFromTClockIni(app);
    return true;
}

bool saveProfiles(AppState& app) {
    if (app.profiles.empty()) return false;
    applyLanguageToProfiles(app);
    if (!saveSettingsProfiles(app.profiles, app.iniPath, app.language)) return false;
    if (!saveHotkeysToTClockIni(app)) return false;
    return true;
}

void unregisterAllHotkeys(AppState& app) {
    for (UINT id : app.hotkeyIds) {
        if (id != 0 && app.mainWindow) {
            UnregisterHotKey(app.mainWindow, id);
        }
    }
    std::fill(app.hotkeyIds.begin(), app.hotkeyIds.end(), 0);
    app.printScreenHotkeyCount = 0;
    for (auto& spec : app.hotkeySpecs) {
        spec = HotkeySpec{};
    }
    app.printScreenHotkeys.clear();
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
        g_hookApp = nullptr;
    }
}

void applyLanguageToProfiles(AppState& app) {
    for (auto& p : app.profiles) {
        p.settings.language = app.language;
    }
}

std::wstring translateId(const AppState& app, const std::wstring& id, const std::wstring& fallback) {
    auto it = app.translations.find(id);
    if (it != app.translations.end()) {
        return it->second;
    }
    return fallback;
}

bool loadLanguage(AppState& app, const std::string& code) {
    std::string lang = code.empty() ? "en" : toLower(code);
    if (lang != "en" && lang != "ja") lang = "en";
    app.translations.clear();
    app.language = lang;
    app.availableLanguages = listAvailableLanguages(getLanguageRoot(app.exeDir));

    auto loadFile = [&](const std::string& langCode) -> bool {
        fs::path path = getLanguageRoot(app.exeDir) / langCode / "strings.ini";
        std::ifstream in(path);
        if (!in.is_open()) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trimCopy(line.substr(0, eq));
            std::string value = line.substr(eq + 1);
            std::wstring wkey = replaceEscapes(utf8ToWide(key));
            std::wstring wval = stripQuotes(replaceEscapes(utf8ToWide(value)));
            if (!wkey.empty()) {
                app.translations[wkey] = wval;
            }
        }
        return true;
    };

    bool loaded = loadFile(lang);
    if (!loaded && lang != "en") {
        loadFile("en");
        app.language = "en";
    }
    return loaded;
}

void setLanguage(AppState& app, const std::string& langCode) {
    std::string lang = toLower(langCode);
    if (lang.empty()) lang = "en";
    app.language = lang;
    applyLanguageToProfiles(app);
    saveProfiles(app);
    loadLanguage(app, app.language);
}

static fs::path getLanguageRoot(const fs::path& exeDir) {
    fs::path modern = exeDir / "tcapture" / "lang";
    if (fs::exists(modern) && fs::is_directory(modern)) return modern;
    return exeDir / "lang"; // legacy fallback
}

std::vector<std::string> listAvailableLanguages(const fs::path& root) {
    std::vector<std::string> codes;
    if (!fs::exists(root) || !fs::is_directory(root)) return codes;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        fs::path ini = entry.path() / "strings.ini";
        if (fs::exists(ini)) {
            codes.push_back(toLower(entry.path().filename().string()));
        }
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    if (codes.empty()) {
        codes.push_back("en");
    }
    return codes;
}

std::wstring languageLabel(const std::string& code) {
    std::string lc = toLower(code);
    // Try loading display name from the language file.
    fs::path exeDir = getExecutableDir();
    fs::path ini = getLanguageRoot(exeDir) / lc / "strings.ini";
    if (fs::exists(ini)) {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trimCopy(line.substr(0, eq));
            std::string value = line.substr(eq + 1);
            if (toLower(key) == "language_name") {
                std::wstring w = replaceEscapes(utf8ToWide(value));
                if (!w.empty()) return w;
            }
        }
    }
    if (lc == "ja") return L"Japanese";
    if (lc == "en") return L"English";
    std::wstring w = utf8ToWide(code);
    return w.empty() ? L"en" : w;
}

void stopAutoCaptureTimers(AppState& app) {
    if (!app.mainWindow) return;
    for (UINT_PTR id : app.autoTimerIds) {
        if (id != 0) {
            KillTimer(app.mainWindow, id);
        }
    }
    std::fill(app.autoTimerIds.begin(), app.autoTimerIds.end(), 0);
}

void updateAutoCaptureTimers(AppState& app) {
    if (!app.mainWindow) return;
    stopAutoCaptureTimers(app);
    if (app.autoTimerIds.size() != app.profiles.size()) {
        app.autoTimerIds.assign(app.profiles.size(), 0);
    }
    for (size_t i = 0; i < app.profiles.size(); ++i) {
        const auto& s = app.profiles[i].settings;
        if (!s.autoCapture || s.autoSeconds <= 0) continue;
        UINT_PTR timerId = TIMER_AUTO_BASE + static_cast<UINT_PTR>(i);
        if (SetTimer(app.mainWindow, timerId, static_cast<UINT>(s.autoSeconds * 1000), nullptr)) {
            app.autoTimerIds[i] = timerId;
        }
    }
}

bool isKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

UINT currentModifierState() {
    UINT mods = 0;
    if (isKeyDown(VK_LCONTROL) || isKeyDown(VK_RCONTROL)) mods |= MOD_CONTROL;
    if (isKeyDown(VK_LSHIFT) || isKeyDown(VK_RSHIFT)) mods |= MOD_SHIFT;
    if (isKeyDown(VK_LMENU) || isKeyDown(VK_RMENU) || isKeyDown(VK_MENU)) mods |= MOD_ALT;
    if (isKeyDown(VK_LWIN) || isKeyDown(VK_RWIN)) mods |= MOD_WIN;
    return mods;
}

UINT currentModifierStateForHook(const KBDLLHOOKSTRUCT& info) {
    UINT mods = currentModifierState();
    if (info.flags & LLKHF_ALTDOWN) {
        mods |= MOD_ALT;
    }
    return mods;
}

void installPrintScreenHook(AppState& app) {
    if (g_keyboardHook || app.printScreenHotkeyCount == 0) return;
    g_hookApp = &app;
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
    if (!g_keyboardHook) {
        g_hookApp = nullptr;
        showNotification(app, translateId(app, L"app_name", L"TCapture"),
                         L"Failed to install PrintScreen hook.", NIIF_WARNING);
    }
}

void registerAllHotkeys(AppState& app) {
    unregisterAllHotkeys(app);
    if (!app.mainWindow) return;
    if (app.hotkeySpecs.size() != app.profiles.size()) {
        app.hotkeySpecs.assign(app.profiles.size(), HotkeySpec{});
    }
    for (size_t i = 0; i < app.profiles.size(); ++i) {
        const auto& profile = app.profiles[i];
        if (profile.settings.captureHotkey.empty()) continue;
        HotkeySpec spec;
        if (!parseHotkey(profile.settings.captureHotkey, spec)) {
            continue;
        }
        app.hotkeySpecs[i] = spec;
        if (spec.key == VK_SNAPSHOT) {
            app.printScreenHotkeys.push_back({ static_cast<int>(i), spec.modifiers, profile.name });
            app.printScreenHotkeyCount = app.printScreenHotkeys.size();
            continue;
        }
        UINT id = HOTKEY_ID_BASE + static_cast<UINT>(i);
        if (RegisterHotKey(app.mainWindow, id, spec.modifiers, spec.key)) {
            app.hotkeyIds[i] = id;
        }
    }
    installPrintScreenHook(app);
    // Notify if nothing registered.
    bool any = app.printScreenHotkeyCount > 0;
    for (UINT id : app.hotkeyIds) {
        if (id != 0) {
            any = true;
            break;
        }
    }
    if (!any) {
                showNotification(app, translateId(app, L"app_name", L"TCapture"),
                         translateId(app, L"no_hotkeys", L"No hotkeys registered. Configure hotkeys in TCapture settings."), NIIF_WARNING);
    }
}

void openOutputFolder(const AppState& app, int profileIndex) {
    if (profileIndex < 0 || profileIndex >= static_cast<int>(app.profiles.size())) return;
    const auto& s = app.profiles[profileIndex].settings;
    fs::path dir = s.outputDir.empty() ? fs::path(".") : fs::path(s.outputDir);
    fs::path base = app.iniPath.empty() ? app.exeDir : app.iniPath.parent_path();
    if (!dir.is_absolute()) {
        dir = base / dir;
    }
    ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int findPrintScreenMatch(const AppState& app, UINT modifiers) {
    modifiers &= ~MOD_NOREPEAT;
    for (const auto& entry : app.printScreenHotkeys) {
        if (entry.index < 0) continue;
        if ((entry.modifiers & ~MOD_NOREPEAT) == modifiers) {
            return entry.index;
        }
    }
    return -1;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    static bool printScreenTriggered = false;
    static UINT modifierState = 0;
    if (code == HC_ACTION && g_hookApp && g_hookApp->printScreenHotkeyCount > 0) {
        const KBDLLHOOKSTRUCT* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (info) {
            const bool isKeyDownEvent = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isKeyUpEvent = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            const bool isDown = isKeyDownEvent && !(info->flags & LLKHF_UP);
            const bool isUp = isKeyUpEvent || (info->flags & LLKHF_UP);
            switch (info->vkCode) {
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_CONTROL:
                if (isDown) modifierState |= MOD_CONTROL;
                if (isUp) modifierState &= ~MOD_CONTROL;
                break;
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_SHIFT:
                if (isDown) modifierState |= MOD_SHIFT;
                if (isUp) modifierState &= ~MOD_SHIFT;
                break;
            case VK_LMENU:
            case VK_RMENU:
            case VK_MENU:
                if (isDown) modifierState |= MOD_ALT;
                if (isUp) modifierState &= ~MOD_ALT;
                break;
            case VK_LWIN:
            case VK_RWIN:
                if (isDown) modifierState |= MOD_WIN;
                if (isUp) modifierState &= ~MOD_WIN;
                break;
            default:
                break;
            }
        }
        if (info && info->vkCode == VK_SNAPSHOT) {
            const bool isKeyDownEvent = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isKeyUpEvent = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            if (isKeyDownEvent && !printScreenTriggered) {
                UINT mods = modifierState;
                if (info->flags & LLKHF_ALTDOWN) mods |= MOD_ALT;
                int match = findPrintScreenMatch(*g_hookApp, mods);
                if (match >= 0 && g_hookApp->mainWindow) {
                    PostMessageW(g_hookApp->mainWindow, WM_APP_CAPTURE_REQUEST,
                                 static_cast<WPARAM>(match), 0);
                    printScreenTriggered = true;
                }
            } else if (isKeyUpEvent) {
                if (!printScreenTriggered) {
                    UINT mods = modifierState;
                    if (info->flags & LLKHF_ALTDOWN) mods |= MOD_ALT;
                    int match = findPrintScreenMatch(*g_hookApp, mods);
                    if (match >= 0 && g_hookApp->mainWindow) {
                        PostMessageW(g_hookApp->mainWindow, WM_APP_CAPTURE_REQUEST,
                                     static_cast<WPARAM>(match), 0);
                    }
                }
                printScreenTriggered = false;
            }
            return 1; // Always swallow PrintScreen when configured to avoid OS capture UI.
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

int CALLBACK browseFolderCallback(HWND hwnd, UINT uMsg, LPARAM, LPARAM lpData) {
    if (uMsg == BFFM_INITIALIZED && lpData) {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);
    }
    return 0;
}

std::wstring browseForFolder(HWND owner, const std::wstring& initialPath, const std::wstring& title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title.empty() ? nullptr : title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    bi.lpfn = browseFolderCallback;
    bi.lParam = initialPath.empty() ? 0 : reinterpret_cast<LPARAM>(initialPath.c_str());
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t path[MAX_PATH] = {};
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok || path[0] == L'\0') return L"";
    return std::wstring(path);
}

void setControlText(HWND hwnd, const std::string& text) {
    std::wstring wide = utf8ToWide(text);
    SetWindowTextW(hwnd, wide.c_str());
}

std::string getControlText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return "";
    std::wstring buffer(static_cast<size_t>(len), L'\0');
    GetWindowTextW(hwnd, buffer.data(), len + 1);
    return wideToUtf8(buffer);
}

void ensureTooltip(SettingsDialog* dlg) {
    if (dlg->tooltip) return;
    dlg->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
                                   WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   dlg->hwnd, nullptr, dlg->app->hInstance, nullptr);
    SetWindowPos(dlg->tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(dlg->tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
}

void addTooltip(SettingsDialog* dlg, HWND target, const std::wstring& text) {
    ensureTooltip(dlg);
    if (!dlg->tooltip || !target) return;
    const std::wstring& stored = dlg->tooltipTexts.emplace_back(text);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(TOOLINFOW);
    ti.hwnd = target;
    ti.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
    ti.uId = 0;
    ti.lpszText = const_cast<wchar_t*>(stored.c_str());
    GetClientRect(target, &ti.rect);
    SendMessageW(dlg->tooltip, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
}

struct HotkeyModOption {
    std::wstring label;
    std::string text;
    UINT modifiers;
};

struct HotkeyKeyOption {
    std::wstring label;
    std::string text;
    UINT key;
};

const std::vector<HotkeyModOption>& modOptions() {
    static const std::vector<HotkeyModOption> opts = {
        {L"None", "", 0},
        {L"Ctrl", "Ctrl", MOD_CONTROL},
        {L"Alt", "Alt", MOD_ALT},
        {L"Shift", "Shift", MOD_SHIFT},
        {L"Ctrl+Alt", "Ctrl+Alt", MOD_CONTROL | MOD_ALT},
        {L"Ctrl+Shift", "Ctrl+Shift", MOD_CONTROL | MOD_SHIFT},
        {L"Alt+Shift", "Alt+Shift", MOD_ALT | MOD_SHIFT},
        {L"Ctrl+Alt+Shift", "Ctrl+Alt+Shift", MOD_CONTROL | MOD_ALT | MOD_SHIFT},
        {L"Win", "Win", MOD_WIN},
        {L"Ctrl+Win", "Ctrl+Win", MOD_CONTROL | MOD_WIN},
        {L"Alt+Win", "Alt+Win", MOD_ALT | MOD_WIN},
        {L"Ctrl+Alt+Win", "Ctrl+Alt+Win", MOD_CONTROL | MOD_ALT | MOD_WIN},
    };
    return opts;
}

const std::vector<HotkeyKeyOption>& keyOptions() {
    static std::vector<HotkeyKeyOption> opts;
    if (!opts.empty()) return opts;
    opts.push_back({L"(None)", "", 0});
    for (char c = 'A'; c <= 'Z'; ++c) {
        std::wstring label(1, static_cast<wchar_t>(c));
        std::string text(1, c);
        opts.push_back({label, text, static_cast<UINT>(c)});
    }
    for (char d = '0'; d <= '9'; ++d) {
        std::wstring label(1, static_cast<wchar_t>(d));
        std::string text(1, d);
        opts.push_back({label, text, static_cast<UINT>(d)});
    }
    for (int f = 1; f <= 24; ++f) {
        std::wstring label = L"F" + std::to_wstring(f);
        std::string text = "F" + std::to_string(f);
        opts.push_back({label, text, VK_F1 + static_cast<UINT>(f - 1)});
    }
    opts.push_back({L"PrintScreen", "PrintScreen", VK_SNAPSHOT});
    opts.push_back({L"Insert", "Insert", VK_INSERT});
    opts.push_back({L"Delete", "Delete", VK_DELETE});
    opts.push_back({L"Home", "Home", VK_HOME});
    opts.push_back({L"End", "End", VK_END});
    opts.push_back({L"PageUp", "PgUp", VK_PRIOR});
    opts.push_back({L"PageDown", "PgDn", VK_NEXT});
    opts.push_back({L"Space", "Space", VK_SPACE});
    opts.push_back({L"Enter", "Enter", VK_RETURN});
    return opts;
}

int clampBurstFps(int fps) {
    if (fps < 0) return 0;
    if (fps > 60) return 60;
    return fps;
}

void populateBurstFpsCombo(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0 (off)"));
    for (int i = 1; i <= 60; ++i) {
        std::wstring label = std::to_wstring(i) + L" fps";
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
}

int findModIndex(UINT modifiers) {
    UINT target = modifiers & ~MOD_NOREPEAT;
    const auto& opts = modOptions();
    for (size_t i = 0; i < opts.size(); ++i) {
        if (opts[i].modifiers == target) return static_cast<int>(i);
    }
    return 0;
}

int findKeyIndex(UINT key) {
    const auto& opts = keyOptions();
    for (size_t i = 0; i < opts.size(); ++i) {
        if (opts[i].key == key) return static_cast<int>(i);
    }
    return 0;
}

int burstFpsToIndex(int fps) {
    return clampBurstFps(fps);
}

int burstFpsFromCombo(HWND combo) {
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (sel < 0) sel = 0;
    return clampBurstFps(sel);
}

void setBurstFpsSelection(HWND combo, int fps) {
    int idx = burstFpsToIndex(fps);
    SendMessageW(combo, CB_SETCURSEL, idx, 0);
}

void setComboToValue(HWND combo, int value) {
    if (!combo) return;
    std::wstring text = std::to_wstring(value);
    LRESULT idx = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text.c_str()));
    if (idx >= 0) {
        SendMessageW(combo, CB_SETCURSEL, idx, 0);
    } else {
        SetWindowTextW(combo, text.c_str());
    }
}

void populateCompressionCombos(SettingsDialog* dlg) {
    if (!dlg) return;
    (void)dlg;
}

void updateCompressionFields(SettingsDialog* dlg) {
    if (!dlg || !dlg->formatCombo) return;
    int sel = static_cast<int>(SendMessageW(dlg->formatCombo, CB_GETCURSEL, 0, 0));
    bool usePng = (sel == 0);
    if (dlg->compressionEditPng) EnableWindow(dlg->compressionEditPng, usePng);
    if (dlg->compressionEditJpg) EnableWindow(dlg->compressionEditJpg, !usePng);
    if (dlg->compressionLabelPng) ShowWindow(dlg->compressionLabelPng, usePng ? SW_SHOW : SW_HIDE);
    if (dlg->compressionEditPng) ShowWindow(dlg->compressionEditPng, usePng ? SW_SHOW : SW_HIDE);
    if (dlg->compressionLabelJpg) ShowWindow(dlg->compressionLabelJpg, usePng ? SW_HIDE : SW_SHOW);
    if (dlg->compressionEditJpg) ShowWindow(dlg->compressionEditJpg, usePng ? SW_HIDE : SW_SHOW);
}

void updateAutoStatusLabel(SettingsDialog* dlg) {
    if (!dlg || !dlg->app || !dlg->autoStatusLabel) return;
    updateStatusBrush(dlg);
    bool active = false;
    int idx = dlg->activeProfile;
    if (idx >= 0 && idx < static_cast<int>(dlg->app->autoTimerIds.size())) {
        active = dlg->app->autoTimerIds[idx] != 0;
    }
    dlg->autoStatusActive = active;
    std::wstring text = active ? translateId(*dlg->app, L"auto_running", L"Auto capture: running")
                               : translateId(*dlg->app, L"auto_stopped", L"Auto capture: stopped");
    SetWindowTextW(dlg->autoStatusLabel, text.c_str());
    InvalidateRect(dlg->autoStatusLabel, nullptr, TRUE);
}

void updateStatusBrush(SettingsDialog* dlg) {
    if (!dlg) return;
    if (!dlg->statusBrushActive) {
        dlg->statusBrushActive = CreateSolidBrush(RGB(255, 230, 230));
    }
    if (!dlg->statusBrushInactive) {
        dlg->statusBrushInactive = CreateSolidBrush(dlg->backgroundColor);
    }
}

void updateSettingsWindowTitle(SettingsDialog* dlg) {
    if (!dlg || !dlg->app || !dlg->hwnd) return;
    std::wstring appTitle = translateId(*dlg->app, L"app_name", L"TCapture");
    std::wstring profileTitle;
    if (dlg->activeProfile >= 0 && dlg->activeProfile < static_cast<int>(dlg->app->profiles.size())) {
        profileTitle = utf8ToWide(dlg->app->profiles[dlg->activeProfile].name);
    }
    std::wstring title = appTitle;
    if (!profileTitle.empty()) {
        title += L" - " + profileTitle;
    }
    SetWindowTextW(dlg->hwnd, title.c_str());
}

void setHotkeyCombosFromSettings(SettingsDialog* dlg, const std::string& hotkey) {
    HotkeySpec spec;
    int modIndex = 0;
    int keyIndex = 0;
    if (parseHotkey(hotkey, spec)) {
        modIndex = findModIndex(spec.modifiers);
        keyIndex = findKeyIndex(spec.key);
    }
    SendMessageW(dlg->hotkeyModCombo, CB_SETCURSEL, modIndex, 0);
    SendMessageW(dlg->hotkeyKeyCombo, CB_SETCURSEL, keyIndex, 0);
}

std::string getHotkeyFromControls(SettingsDialog* dlg) {
    int modSel = static_cast<int>(SendMessageW(dlg->hotkeyModCombo, CB_GETCURSEL, 0, 0));
    int keySel = static_cast<int>(SendMessageW(dlg->hotkeyKeyCombo, CB_GETCURSEL, 0, 0));
    const auto& mods = modOptions();
    const auto& keys = keyOptions();
    if (modSel < 0 || modSel >= static_cast<int>(mods.size())) modSel = 0;
    if (keySel < 0 || keySel >= static_cast<int>(keys.size())) keySel = 0;
    const auto& mod = mods[modSel];
    const auto& key = keys[keySel];
    if (key.text.empty()) return "";
    if (mod.text.empty()) return key.text;
    return mod.text + "+" + key.text;
}

void loadProfileToControls(SettingsDialog* dlg, int index) {
    if (index < 0 || index >= static_cast<int>(dlg->app->profiles.size())) return;
    dlg->suppressSave = true;
    dlg->activeProfile = index;
    const auto& s = dlg->app->profiles[index].settings;
    setControlText(dlg->outputEdit, s.outputDir);
    SendMessageW(dlg->actionCombo, CB_SETCURSEL, s.openAfterCapture ? (s.saveBeforeOpen ? 2 : 1) : 0, 0);
    setBurstFpsSelection(dlg->burstFpsCombo, s.burstFps);
    setControlText(dlg->burstSecondsEdit, std::to_string(s.burstSeconds));
    SendMessageW(dlg->autoCaptureCheck, BM_SETCHECK, s.autoCapture ? BST_CHECKED : BST_UNCHECKED, 0);
    setControlText(dlg->autoIntervalEdit, std::to_string(s.autoSeconds));
    setControlText(dlg->displaysEdit, displaysToString(s));
    setHotkeyCombosFromSettings(dlg, s.captureHotkey);
    int fmtIndex = toLower(s.format) == "jpg" ? 1 : 0;
    SendMessageW(dlg->formatCombo, CB_SETCURSEL, fmtIndex, 0);
    tcap_fill_apps(dlg, s.openApp, s.openExecutable);
    populateCompressionCombos(dlg);
    setControlText(dlg->compressionEditPng, std::to_string(s.pngCompression));
    setControlText(dlg->compressionEditJpg, std::to_string(s.jpgQuality));
    dlg->suppressSave = false;
    updateCompressionFields(dlg);
    updateAutoStatusLabel(dlg);
    updateSettingsWindowTitle(dlg);
    tcap_sync_controls(dlg);
}

bool saveControlsToProfile(SettingsDialog* dlg, int index, bool showErrors) {
    if (index < 0 || index >= static_cast<int>(dlg->app->profiles.size())) return false;
    ProfileSettings& profile = dlg->app->profiles[index];
    profile.settings.outputDir = trimCopy(getControlText(dlg->outputEdit));
    const auto action = SendMessageW(dlg->actionCombo, CB_GETCURSEL, 0, 0);
    profile.settings.openAfterCapture = action > 0;
    profile.settings.saveBeforeOpen = action == 2;
    const auto appIndex = SendMessageW(dlg->openAppCombo, CB_GETCURSEL, 0, 0);
    if (appIndex >= 0 && static_cast<size_t>(appIndex) < dlg->openApps.size()) {
        const auto& choice = dlg->openApps[static_cast<size_t>(appIndex)];
        if (!choice.browse) {
            profile.settings.openApp = choice.executable ? L"" : choice.name;
            profile.settings.openExecutable = choice.executable ? choice.name : L"";
        }
    }
    profile.settings.format = trimCopy(getControlText(dlg->formatCombo));
    profile.settings.displaysRaw = trimCopy(getControlText(dlg->displaysEdit));
    profile.settings.captureHotkey = trimCopy(getHotkeyFromControls(dlg));
    profile.settings.autoCapture = (SendMessageW(dlg->autoCaptureCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    auto parseCompression = [&](HWND ctrl, int& dest, const wchar_t* errorText) -> bool {
        std::string txt = trimCopy(getControlText(ctrl));
        if (txt.empty()) return true; // keep existing
        try {
            dest = std::stoi(txt);
            return true;
        } catch (...) {
            if (showErrors && IsWindowEnabled(ctrl)) {
                MessageBoxW(dlg->hwnd, errorText, L"TCapture", MB_ICONWARNING);
            }
            return IsWindowEnabled(ctrl) ? false : true;
        }
    };
    if (!parseCompression(dlg->compressionEditPng, profile.settings.pngCompression, L"PNG compression must be a number.")) return false;
    if (!parseCompression(dlg->compressionEditJpg, profile.settings.jpgQuality, L"JPG quality must be a number.")) return false;
    profile.settings.burstFps = burstFpsFromCombo(dlg->burstFpsCombo);
    try {
        profile.settings.burstSeconds = std::stoi(trimCopy(getControlText(dlg->burstSecondsEdit)));
    } catch (...) {
        if (showErrors) {
            MessageBoxW(dlg->hwnd, L"Burst seconds must be a number.", L"TCapture", MB_ICONWARNING);
        }
        return false;
    }
    try {
        profile.settings.autoSeconds = std::stoi(trimCopy(getControlText(dlg->autoIntervalEdit)));
    } catch (...) {
        if (showErrors) {
            MessageBoxW(dlg->hwnd, L"Auto-capture seconds must be a number.", L"TCapture", MB_ICONWARNING);
        }
        return false;
    }
    bool parsedAll = false;
    bool activeDisplay = false;
    bool activeWindow = false;
    profile.settings.displayIndices = parseDisplayList(profile.settings.displaysRaw, parsedAll, activeDisplay, activeWindow);
    if (parsedAll) profile.settings.displayIndices.clear();
    profile.settings.captureActiveDisplay = activeDisplay;
    profile.settings.captureActiveWindow = activeWindow;
    normalizeSettings(profile.settings);

    if (!profile.settings.captureHotkey.empty()) {
        HotkeySpec spec;
        if (!parseHotkey(profile.settings.captureHotkey, spec)) {
            if (showErrors) {
                MessageBoxW(dlg->hwnd, L"Hotkey is invalid. Use Ctrl+Alt+S or Shift+F12, etc.", L"TCapture", MB_ICONWARNING);
            }
            return false;
        }
        std::string currentSig;
        if (buildHotkeySignature(profile.settings.captureHotkey, currentSig) && !currentSig.empty()) {
            for (int i = 0; i < static_cast<int>(dlg->app->profiles.size()); ++i) {
                if (i == index) continue;
                const std::string other = trimCopy(dlg->app->profiles[i].settings.captureHotkey);
                if (other.empty()) continue;
                std::string otherSig;
                if (!buildHotkeySignature(other, otherSig) || otherSig.empty()) continue;
                if (currentSig == otherSig) {
                    if (showErrors) {
                        std::wstring msg = L"Hotkey duplicates profile: " + utf8ToWide(dlg->app->profiles[i].name);
                        MessageBoxW(dlg->hwnd, msg.c_str(), L"TCapture", MB_ICONWARNING);
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

void populateTabs(SettingsDialog* dlg) {
    SendMessageW(dlg->tab, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < dlg->app->profiles.size(); ++i) {
        std::wstring name = utf8ToWide(dlg->app->profiles[i].name);
        SendMessageW(dlg->tab, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(dlg->tab, LB_SETCURSEL, static_cast<WPARAM>(dlg->activeProfile), 0);
    EnableWindow(dlg->deleteBtn, dlg->app->profiles.size() > 1);
    EnableWindow(dlg->renameBtn, !dlg->app->profiles.empty());
}

bool promptRenameProfile(SettingsDialog* dlg, int index) {
    if (!dlg || !dlg->app) return false;
    if (index < 0 || index >= static_cast<int>(dlg->app->profiles.size())) return false;
    saveControlsToProfile(dlg, dlg->activeProfile, false);

    std::wstring appTitle = translateId(*dlg->app, L"app_name", L"TCapture");
    std::wstring baseTitle = translateId(*dlg->app, L"rename_profile_title", L"Rename profile");
    std::wstring title = appTitle + L" - " + baseTitle;
    std::wstring initial = utf8ToWide(dlg->app->profiles[index].name);

    const wchar_t CLASS_NAME[] = L"RenamePrompt";
    struct PromptState {
        HWND hwnd;
        HWND edit;
        std::wstring result;
        bool accepted = false;
    } state{};

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            switch (m) {
            case WM_NCCREATE: {
                auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                st = reinterpret_cast<PromptState*>(cs->lpCreateParams);
                SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                return TRUE;
            }
            case WM_CREATE: {
                HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           10, 10, 220, 24, h, nullptr, nullptr, nullptr);
                SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                60, 44, 60, 24, h, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
                CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                                130, 44, 60, 24, h, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
                SetWindowTextW(st->edit, st->result.c_str());
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wp) == IDOK) {
                    int len = GetWindowTextLengthW(st->edit);
                    std::wstring buf(static_cast<size_t>(len), L'\0');
                    GetWindowTextW(st->edit, buf.data(), len + 1);
                    st->result = buf;
                    st->accepted = true;
                    DestroyWindow(h);
                    return 0;
                } else if (LOWORD(wp) == IDCANCEL) {
                    DestroyWindow(h);
                    return 0;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(h);
                return 0;
            }
            return DefWindowProcW(h, m, wp, lp);
        };
        wc.hInstance = dlg->app->hInstance;
        wc.lpszClassName = CLASS_NAME;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }

    state.result = initial;
    HWND promptWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, title.c_str(),
                                     WS_CAPTION | WS_SYSMENU | WS_POPUPWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 260, 120,
                                     dlg->hwnd, nullptr, dlg->app->hInstance, &state);
    if (!promptWnd) return false;
    native_edit::prepare(promptWnd);

    SetWindowPos(promptWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
    SetForegroundWindow(promptWnd);
    MSG msg;
    while (IsWindow(promptWnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (native_edit::translate(msg)) continue;
        if (IsDialogMessageW(promptWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!state.accepted) return false;

    std::string newName = trimCopy(wideToUtf8(state.result));
    if (newName.empty()) return false;

    auto isDup = [&](const std::string& name) {
        for (size_t i = 0; i < dlg->app->profiles.size(); ++i) {
            if (i == static_cast<size_t>(index)) continue;
            if (namesEqual(dlg->app->profiles[i].name, name)) return true;
        }
        return false;
    };
    if (isDup(newName)) {
        MessageBoxW(dlg->hwnd, L"Name already exists.", L"TCapture", MB_ICONWARNING);
        return false;
    }

    dlg->app->profiles[index].name = newName;
    populateTabs(dlg);
    if (index == dlg->activeProfile) {
        SendMessageW(dlg->tab, LB_SETCURSEL, static_cast<WPARAM>(dlg->activeProfile), 0);
    }
    saveProfiles(*dlg->app);
    updateSettingsWindowTitle(dlg);
    return true;
}

void destroyInlineRenameProfile(SettingsDialog* dlg);
void cancelInlineRenameProfile(SettingsDialog* dlg);
bool commitInlineRenameProfile(SettingsDialog* dlg);

LRESULT CALLBACK InlineProfileEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* dlg = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!dlg) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    }
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            commitInlineRenameProfile(dlg);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            cancelInlineRenameProfile(dlg);
            return 0;
        }
    } else if (msg == WM_CHAR) {
        if (wParam == VK_RETURN) {
            commitInlineRenameProfile(dlg);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            cancelInlineRenameProfile(dlg);
            return 0;
        }
    } else if (msg == WM_KILLFOCUS) {
        commitInlineRenameProfile(dlg);
        return 0;
    }
    if (dlg->inlineEditOldProc) {
        return CallWindowProcW(dlg->inlineEditOldProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool beginInlineRenameProfile(SettingsDialog* dlg, int index) {
    if (!dlg || !dlg->tab || dlg->inlineEdit) return false;
    if (index < 0 || index >= static_cast<int>(dlg->app->profiles.size())) return false;

    RECT rc{};
    if (SendMessageW(dlg->tab, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&rc)) == LB_ERR) return false;
    int x = rc.left;
    int y = rc.top;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    const int inset = MulDiv(10, dlg->uiDpi, 96);
    x += inset; y += inset; w -= inset * 2;
    h = MulDiv(30, dlg->uiDpi, 96);

    dlg->inlineEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      x, y, w, h, dlg->tab,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCtrlProfileInlineRename)),
                                      dlg->app->hInstance, nullptr);
    if (!dlg->inlineEdit) return false;
    native_edit::attach(dlg->inlineEdit);

    dlg->inlineEditIndex = index;
    SendMessageW(dlg->inlineEdit, WM_SETFONT, SendMessageW(dlg->tab, WM_GETFONT, 0, 0), TRUE);
    SetWindowTextW(dlg->inlineEdit, utf8ToWide(dlg->app->profiles[static_cast<size_t>(index)].name).c_str());
    SendMessageW(dlg->inlineEdit, EM_SETSEL, 0, -1);
    SetWindowLongPtrW(dlg->inlineEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
    dlg->inlineEditOldProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(dlg->inlineEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InlineProfileEditProc)));
    SetFocus(dlg->inlineEdit);
    SendMessageW(dlg->inlineEdit, EM_SETSEL, 0, -1);
    return true;
}

void destroyInlineRenameProfile(SettingsDialog* dlg) {
    if (!dlg || !dlg->inlineEdit) return;
    HWND edit = dlg->inlineEdit;
    if (dlg->inlineEditOldProc) {
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dlg->inlineEditOldProc));
        dlg->inlineEditOldProc = nullptr;
    }
    dlg->inlineEdit = nullptr;
    dlg->inlineEditIndex = -1;
    DestroyWindow(edit);
}

void cancelInlineRenameProfile(SettingsDialog* dlg) {
    if (!dlg || !dlg->inlineEdit) return;
    destroyInlineRenameProfile(dlg);
    if (dlg->tab) SetFocus(dlg->tab);
}

bool commitInlineRenameProfile(SettingsDialog* dlg) {
    if (!dlg || !dlg->inlineEdit) return false;
    if (dlg->inlineFinalizing) return false;
    dlg->inlineFinalizing = true;

    const int index = dlg->inlineEditIndex;
    const std::string newName = trimCopy(getControlText(dlg->inlineEdit));
    destroyInlineRenameProfile(dlg);

    bool changed = false;
    if (!newName.empty() && index >= 0 && index < static_cast<int>(dlg->app->profiles.size())) {
        auto isDup = [&](const std::string& name) {
            for (size_t i = 0; i < dlg->app->profiles.size(); ++i) {
                if (i == static_cast<size_t>(index)) continue;
                if (namesEqual(dlg->app->profiles[i].name, name)) return true;
            }
            return false;
        };
        if (!isDup(newName)) {
            dlg->app->profiles[static_cast<size_t>(index)].name = newName;
            dlg->activeProfile = index;
            populateTabs(dlg);
            SendMessageW(dlg->tab, LB_SETCURSEL, static_cast<WPARAM>(dlg->activeProfile), 0);
            saveProfiles(*dlg->app);
            updateSettingsWindowTitle(dlg);
            changed = true;
        } else {
            MessageBoxW(dlg->hwnd, L"Name already exists.", L"TCapture", MB_ICONWARNING);
        }
    }
    dlg->inlineFinalizing = false;
    return changed;
}

LRESULT CALLBACK ProfileListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* dlg = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (dlg && msg == WM_KEYDOWN && wParam == VK_F2) {
        int sel = static_cast<int>(SendMessageW(dlg->tab, LB_GETCURSEL, 0, 0));
        if (sel < 0) sel = dlg->activeProfile;
        beginInlineRenameProfile(dlg, sel);
        return 0;
    }
    if (dlg && dlg->tabOldProc) {
        return CallWindowProcW(dlg->tabOldProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring buildDisplayListText() {
    struct DisplayInfo {
        std::wstring name;
        RECT rect{};
    };

    std::vector<DisplayInfo> displays;
    auto enumProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM lpData) -> BOOL {
        auto* list = reinterpret_cast<std::vector<DisplayInfo>*>(lpData);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(MONITORINFOEXW);
        if (!GetMonitorInfoW(hMon, &info)) {
            return TRUE;
        }
        DisplayInfo d{};
        d.name = info.szDevice;
        d.rect = info.rcMonitor;
        list->push_back(d);
        return TRUE;
    };

    if (!EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&displays))) {
        return L"Failed to enumerate displays.";
    }
    if (displays.empty()) {
        return L"No displays detected.";
    }

    std::wostringstream oss;
    oss << L"Detected displays: " << displays.size() << L"\n";
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        int width = d.rect.right - d.rect.left;
        int height = d.rect.bottom - d.rect.top;
        oss << L"\n[" << (i + 1) << L"] " << d.name
            << L"\n  Resolution: " << width << L"x" << height
            << L"\n  Position: (" << d.rect.left << L", " << d.rect.top << L")";
    }
    oss << L"\n\nNote: Numbers may not match Windows display settings.";
    return oss.str();
}

bool namesEqual(const std::string& a, const std::string& b) {
    return toLower(a) == toLower(b);
}

std::vector<ProfileSettings> mergeProfilesForSave(SettingsDialog* dlg, const ProfileSettings& activeProfile) {
    std::vector<ProfileSettings> merged;
    auto diskProfiles = loadSettingsProfiles(dlg->app->iniPath);
    if (!diskProfiles.empty()) {
        for (const auto& p : diskProfiles) {
            if (namesEqual(p.name, activeProfile.name)) {
                merged.push_back(activeProfile);
            } else {
                merged.push_back(p);
            }
        }
        for (const auto& p : dlg->app->profiles) {
            bool exists = false;
            for (const auto& m : merged) {
                if (namesEqual(m.name, p.name)) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                merged.push_back(namesEqual(p.name, activeProfile.name) ? activeProfile : p);
            }
        }
    } else {
        merged = dlg->app->profiles;
        bool replaced = false;
        for (auto& p : merged) {
            if (namesEqual(p.name, activeProfile.name)) {
                p = activeProfile;
                replaced = true;
                break;
            }
        }
        if (!replaced) merged.push_back(activeProfile);
    }
    return merged;
}

#include "settings_ui.h"

bool persistActiveProfile(SettingsDialog* dlg, bool showErrors) {
    if (dlg->suppressSave) return true;
    if (!saveControlsToProfile(dlg, dlg->activeProfile, showErrors)) return false;
    normalizeSettings(dlg->app->profiles[dlg->activeProfile].settings);
    if (dlg->activeProfile < 0 || dlg->activeProfile >= static_cast<int>(dlg->app->profiles.size())) {
        dlg->activeProfile = static_cast<int>(std::max<size_t>(dlg->app->profiles.size(), 1)) - 1;
    }
    if (dlg->app->hotkeyIds.size() != dlg->app->profiles.size()) {
        dlg->app->hotkeyIds.assign(dlg->app->profiles.size(), 0);
    }
    int tabCount = static_cast<int>(SendMessageW(dlg->tab, LB_GETCOUNT, 0, 0));
    if (tabCount != static_cast<int>(dlg->app->profiles.size())) {
        populateTabs(dlg);
    }
    SendMessageW(dlg->tab, LB_SETCURSEL, static_cast<WPARAM>(dlg->activeProfile), 0);
    if (!saveProfiles(*dlg->app)) {
        if (showErrors) {
            MessageBoxW(dlg->hwnd, translateId(*dlg->app, L"write_failed", L"Failed to write TCapture.ini.").c_str(),
                        translateId(*dlg->app, L"app_name", L"TCapture").c_str(), MB_ICONERROR);
        }
        return false;
    }
    tcap_sync_controls(dlg);
    return true;
}

bool persistActiveProfileNoUI(SettingsDialog* dlg, bool showErrors) {
    if (dlg->suppressSave) return true;
    if (!saveControlsToProfile(dlg, dlg->activeProfile, showErrors)) return false;
    normalizeSettings(dlg->app->profiles[dlg->activeProfile].settings);
    if (dlg->activeProfile < 0 || dlg->activeProfile >= static_cast<int>(dlg->app->profiles.size())) {
        dlg->activeProfile = static_cast<int>(std::max<size_t>(dlg->app->profiles.size(), 1)) - 1;
    }
    if (dlg->app->hotkeyIds.size() != dlg->app->profiles.size()) {
        dlg->app->hotkeyIds.assign(dlg->app->profiles.size(), 0);
    }
    if (!saveProfiles(*dlg->app)) {
        if (showErrors) {
            MessageBoxW(dlg->hwnd, translateId(*dlg->app, L"write_failed", L"Failed to write TCapture.ini.").c_str(),
                        translateId(*dlg->app, L"app_name", L"TCapture").c_str(), MB_ICONERROR);
        }
        return false;
    }
    loadLanguage(*dlg->app, dlg->app->language);
    updateAutoCaptureTimers(*dlg->app);
    maybeStopAgentIfIdle(*dlg->app);
    updateAutoStatusLabel(dlg);
    tcap_sync_controls(dlg);
    return true;
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialog* dlg = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        dlg = reinterpret_cast<SettingsDialog*>(cs->lpCreateParams);
        dlg->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
        return TRUE;
    }
    case WM_CREATE:
        if (dlg && !dlg->backgroundBrush) {
            dlg->backgroundColor = RGB(255,255,255);
            dlg->backgroundBrush = CreateSolidBrush(dlg->backgroundColor);
            dlg->editBackgroundColor = GetSysColor(COLOR_WINDOW);
            dlg->editBackgroundBrush = CreateSolidBrush(dlg->editBackgroundColor);
            dlg->listBackgroundColor = RGB(246,248,251);
            dlg->listBackgroundBrush = CreateSolidBrush(dlg->listBackgroundColor);
        }
        buildSettingsLayout(dlg);
        if (dlg && dlg->tab) {
            SetWindowLongPtrW(dlg->tab, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
            dlg->tabOldProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(dlg->tab, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ProfileListSubclassProc)));
        }
        adjustSettingsWindowSize(dlg);
        populateTabs(dlg);
        loadProfileToControls(dlg, dlg->activeProfile);
        return 0;
    case WM_SIZE:
        if(dlg&&wParam!=SIZE_MINIMIZED)tcap_layout_controls(dlg);
        return 0;
    case WM_DPICHANGED:
        if(dlg){
            tcap_refresh_fonts(dlg);
            const auto* rect=reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
            tcap_layout_controls(dlg);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if(dlg){
            const UINT dpi=GetDpiForWindow(hwnd);
            auto* info=reinterpret_cast<MINMAXINFO*>(lParam);
            RECT minimum{0,0,MulDiv(680,dpi,96),MulDiv(410,dpi,96)};
            AdjustWindowRectExForDpi(&minimum,static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)),FALSE,
                                    static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)),dpi);
            info->ptMinTrackSize={minimum.right-minimum.left,minimum.bottom-minimum.top};
        }
        return 0;
    case WM_DRAWITEM:
        if(dlg&&tcap_draw_item(dlg,*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))return TRUE;
        break;
    case WM_MEASUREITEM:
        if(dlg){reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight=MulDiv(58,GetDpiForWindow(hwnd),96);return TRUE;}
        break;
    case WM_PAINT:
        if(dlg){PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);tcap_paint_surface(dlg,hwnd,ps.hdc);EndPaint(hwnd,&ps);return 0;}
        break;
    case WM_ERASEBKGND: {
        if (dlg && dlg->backgroundBrush) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, dlg->backgroundBrush);
            return 1;
        }
        break;
    }
    case WM_NOTIFY:
        break;
    case WM_CONTEXTMENU: {
        if (!dlg) break;
        HWND target = reinterpret_cast<HWND>(wParam);
        if (target == dlg->tab) {
            int hit = -1;
            if (lParam == -1) {
                hit = static_cast<int>(SendMessageW(dlg->tab, LB_GETCURSEL, 0, 0));
            } else {
                POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                POINT listPt = screenPt;
                ScreenToClient(dlg->tab, &listPt);
                DWORD res = static_cast<DWORD>(SendMessageW(dlg->tab, LB_ITEMFROMPOINT, 0, MAKELPARAM(listPt.x, listPt.y)));
                hit = LOWORD(res);
                if (HIWORD(res) != 0) hit = -1;
            }
            if (hit >= 0) {
                promptRenameProfile(dlg, hit);
                return 0;
            }
        }
        break;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CTLCOLORDLG: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (dlg && dlg->backgroundBrush) {
            SetBkColor(hdc, dlg->backgroundColor);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<INT_PTR>(dlg->backgroundBrush);
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND target = reinterpret_cast<HWND>(lParam);
        if (dlg && (GetDlgCtrlID(target) == 170 || GetDlgCtrlID(target) == 171)) {
            const COLORREF color = GetDlgCtrlID(target) == 170 ? RGB(246,248,251) : RGB(250,251,253);
            SetBkColor(hdc, color);
            SetDCBrushColor(hdc, color);
            return reinterpret_cast<INT_PTR>(GetStockObject(DC_BRUSH));
        }
        if (dlg && target == dlg->autoStatusLabel) {
            if (dlg->autoStatusActive) {
                SetTextColor(hdc, RGB(160, 0, 0));
            }
            if (dlg->backgroundBrush) {
                SetBkColor(hdc, dlg->backgroundColor);
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<INT_PTR>(dlg->backgroundBrush);
            }
        }
        if (dlg && dlg->backgroundBrush) {
            SetBkColor(hdc, dlg->backgroundColor);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<INT_PTR>(dlg->backgroundBrush);
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (dlg && dlg->editBackgroundBrush) {
            SetBkColor(hdc, dlg->editBackgroundColor);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<INT_PTR>(dlg->editBackgroundBrush);
        }
        break;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (dlg && dlg->listBackgroundBrush) {
            SetBkColor(hdc, dlg->listBackgroundColor);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<INT_PTR>(dlg->listBackgroundBrush);
        }
        break;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (dlg && dlg->backgroundBrush) {
            SetBkColor(hdc, dlg->backgroundColor);
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<INT_PTR>(dlg->backgroundBrush);
        }
        break;
    }
    case WM_COMMAND:
        if(dlg&&tcap_handle_command(dlg,wParam,lParam))return 0;
        switch (LOWORD(wParam)) {
        case 120: // profile list
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(dlg->tab, LB_GETCURSEL, 0, 0));
                if (dlg->inlineEdit && sel != dlg->inlineEditIndex) {
                    cancelInlineRenameProfile(dlg);
                }
                if (!persistActiveProfileNoUI(dlg, true)) {
                    SendMessageW(dlg->tab, LB_SETCURSEL, static_cast<WPARAM>(dlg->activeProfile), 0);
                    return 0;
                }
                if (sel >= 0) {
                    dlg->activeProfile = sel;
                    loadProfileToControls(dlg, sel);
                }
            } else if (HIWORD(wParam) == LBN_DBLCLK) {
                int sel = static_cast<int>(SendMessageW(dlg->tab, LB_GETCURSEL, 0, 0));
                if (sel < 0) sel = dlg->activeProfile;
                beginInlineRenameProfile(dlg, sel);
            }
            return 0;
        case 130: { // Add
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            saveControlsToProfile(dlg, dlg->activeProfile, false);
            ProfileSettings base = dlg->app->profiles.empty() ? ProfileSettings{} : dlg->app->profiles[dlg->activeProfile];
            base.settings.captureHotkey.clear(); // start new profile with no hotkey to avoid conflicts
            base.settings.autoCapture=false;
            std::string newNameBase = "profile";
            int suffix = static_cast<int>(dlg->app->profiles.size()) + 1;
            auto isDuplicate = [&](const std::string& name) {
                for (const auto& p : dlg->app->profiles) {
                    if (toLower(p.name) == toLower(name)) return true;
                }
                return false;
            };
            while (isDuplicate(newNameBase + std::to_string(suffix))) {
                ++suffix;
            }
            base.name = newNameBase + std::to_string(suffix);
            dlg->app->profiles.push_back(base);
            dlg->app->hotkeyIds.resize(dlg->app->profiles.size(), 0);
            dlg->app->autoTimerIds.resize(dlg->app->profiles.size(), 0);
            dlg->activeProfile = static_cast<int>(dlg->app->profiles.size() - 1);
            populateTabs(dlg);
            loadProfileToControls(dlg, dlg->activeProfile);
            persistActiveProfile(dlg, false);
            return 0;
        }
        case 131: { // Rename
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            int sel = static_cast<int>(SendMessageW(dlg->tab, LB_GETCURSEL, 0, 0));
            if (sel < 0) sel = dlg->activeProfile;
            if (sel >= 0) {
                beginInlineRenameProfile(dlg, sel);
            }
            return 0;
        }
        case 132: { // Delete
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (dlg->app->profiles.size() <= 1) {
                MessageBoxW(dlg->hwnd, translateId(*dlg->app, L"profile_required", L"At least one profile must remain.").c_str(),
                            translateId(*dlg->app, L"app_name", L"TCapture").c_str(), MB_ICONINFORMATION);
                return 0;
            }
            int idx = dlg->activeProfile;
            if (idx < 0 || idx >= static_cast<int>(dlg->app->profiles.size())) return 0;
            std::wstring prompt = translateId(*dlg->app, L"delete_profile_confirm", L"Delete the selected profile?") + L"\n" + utf8ToWide(dlg->app->profiles[idx].name);
            int confirm = MessageBoxW(dlg->hwnd, prompt.c_str(), L"Confirm delete", MB_ICONQUESTION | MB_YESNO);
            if (confirm != IDYES) return 0;
            dlg->app->profiles.erase(dlg->app->profiles.begin() + idx);
            dlg->app->hotkeyIds.resize(dlg->app->profiles.size(), 0);
            dlg->app->autoTimerIds.resize(dlg->app->profiles.size(), 0);
            if (idx >= static_cast<int>(dlg->app->profiles.size())) {
                idx = static_cast<int>(dlg->app->profiles.size()) - 1;
            }
            dlg->activeProfile = idx;
            populateTabs(dlg);
            loadProfileToControls(dlg, dlg->activeProfile);
            persistActiveProfile(dlg, false);
            return 0;
        }
        case 101: // output dir
        case 104: // displays
        case 108: // burst seconds
            if (HIWORD(wParam) == EN_CHANGE) {
                if (dlg->suppressSave) return 0;
                persistActiveProfile(dlg, false);
            }
            return 0;
        case 110: // auto interval
            if (HIWORD(wParam) == EN_CHANGE) {
                if (dlg->suppressSave) return 0;
                persistActiveProfileNoUI(dlg, false);
            }
            return 0;
        case 102: // format
        case 105: // hotkey modifiers
        case 106: // hotkey key
        case 107: // burst fps
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (LOWORD(wParam) == 102) {
                    updateCompressionFields(dlg);
                }
                if (dlg->suppressSave) return 0;
                persistActiveProfile(dlg, false);
            }
            return 0;
        case 103: // png compression edit
            if (HIWORD(wParam) == EN_CHANGE) {
                if (dlg->suppressSave) return 0;
                persistActiveProfile(dlg, false);
            }
            return 0;
        case 113: // jpg quality edit
            if (HIWORD(wParam) == EN_CHANGE) {
                if (dlg->suppressSave) return 0;
                persistActiveProfile(dlg, false);
            }
            return 0;
        case 140: { // browse output dir
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            std::wstring initial = utf8ToWide(trimCopy(getControlText(dlg->outputEdit)));
            std::wstring title = translateId(*dlg->app, L"output_dir", L"Output dir");
            std::wstring selected = browseForFolder(dlg->hwnd, initial, title);
            if (!selected.empty()) {
                dlg->suppressSave = true;
                setControlText(dlg->outputEdit, wideToUtf8(selected));
                dlg->suppressSave = false;
                persistActiveProfile(dlg, false);
            }
            return 0;
        }
        case 141: { // list displays
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            std::wstring info = buildDisplayListText();
            MessageBoxW(dlg->hwnd, info.c_str(), translateId(*dlg->app, L"displays", L"Display").c_str(), MB_ICONINFORMATION);
            return 0;
        }
        case 109: // auto capture
            if (HIWORD(wParam) == BN_CLICKED) {
                if (dlg->suppressSave) return 0;
                bool wasEnabled = false;
                if (dlg->activeProfile >= 0 && dlg->activeProfile < static_cast<int>(dlg->app->profiles.size())) {
                    wasEnabled = dlg->app->profiles[dlg->activeProfile].settings.autoCapture;
                }
                bool isEnabled = (SendMessageW(dlg->autoCaptureCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (persistActiveProfileNoUI(dlg, false) && !wasEnabled && isEnabled) {
                    triggerCapture(*dlg->app, dlg->activeProfile);
                }
            }
            return 0;
        case 142: // capture active profile now
            if (HIWORD(wParam) == BN_CLICKED) {
                if (dlg->suppressSave) return 0;
                if (!persistActiveProfileNoUI(dlg, true)) return 0;
                if (dlg->activeProfile >= 0 && dlg->activeProfile < static_cast<int>(dlg->app->profiles.size())) {
                    triggerCapture(*dlg->app, dlg->activeProfile);
                }
            }
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (dlg) {
            if (dlg->inlineEdit) {
                cancelInlineRenameProfile(dlg);
            }
            if (dlg->tab && dlg->tabOldProc) {
                SetWindowLongPtrW(dlg->tab, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(dlg->tabOldProc));
                dlg->tabOldProc = nullptr;
            }
            if (dlg->tooltip) DestroyWindow(dlg->tooltip);
            for(HFONT font:{dlg->uiFont,dlg->smallFont,dlg->titleFont})if(font)DeleteObject(font);
            if (dlg->statusBrushActive) DeleteObject(dlg->statusBrushActive);
            if (dlg->statusBrushInactive) DeleteObject(dlg->statusBrushInactive);
            if (dlg->backgroundBrush) DeleteObject(dlg->backgroundBrush);
            if (dlg->editBackgroundBrush) DeleteObject(dlg->editBackgroundBrush);
            if (dlg->listBackgroundBrush) DeleteObject(dlg->listBackgroundBrush);
            if (dlg->app) {
                dlg->app->settingsWindow = nullptr;
                maybeStopAgentIfIdle(*dlg->app);
            }
            delete dlg;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND showSettingsWindow(AppState& app) {
    if (app.settingsWindow) {
        ShowWindow(app.settingsWindow, SW_RESTORE);
        ShowWindow(app.settingsWindow, SW_SHOW);
        SetForegroundWindow(app.settingsWindow);
        return app.settingsWindow;
    }

    const wchar_t* CLASS_NAME = kSettingsWindowClass;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = app.hInstance;
        wc.lpszClassName = CLASS_NAME;
        wc.hIcon = app.nid.hIcon;
        RegisterClassW(&wc);
        registered = true;
    }

    SettingsDialog* dlg = new SettingsDialog();
    dlg->app = &app;
    dlg->activeProfile = 0;
    std::wstring appTitle = translateId(app, L"app_name", L"TCapture");
    std::wstring windowTitle = appTitle;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, CLASS_NAME, windowTitle.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 720, 660,
                                nullptr, nullptr, app.hInstance, dlg);
    if (hwnd) {
        HICON hBig = static_cast<HICON>(LoadImageW(app.hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        HICON hSmall = static_cast<HICON>(LoadImageW(app.hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!hBig) hBig = app.nid.hIcon ? app.nid.hIcon : LoadIconW(nullptr, IDI_APPLICATION);
        if (!hSmall) hSmall = hBig;
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBig));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall));
        app.settingsWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    } else {
        delete dlg;
    }
    return hwnd;
}

void showTrayMenu(AppState& app, POINT pt) {
    HMENU menu = CreatePopupMenu();
    HMENU captureMenu = CreatePopupMenu();
    for (size_t i = 0; i < app.profiles.size(); ++i) {
        std::wstring label = translateId(app, L"capture_prefix", L"Capture: ") + utf8ToWide(app.profiles[i].name);
        AppendMenuW(captureMenu, MF_STRING, ID_TRAY_CAPTURE_BASE + static_cast<UINT>(i), label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(captureMenu), translateId(app, L"capture", L"Capture").c_str());
    HMENU langMenu = CreatePopupMenu();
    UINT_PTR langBase = ID_TRAY_LANG_BASE;
    for (size_t i = 0; i < app.availableLanguages.size(); ++i) {
        const auto& code = app.availableLanguages[i];
        UINT flags = (toLower(code) == toLower(app.language)) ? MF_CHECKED : MF_UNCHECKED;
        AppendMenuW(langMenu, MF_STRING | flags, langBase + static_cast<UINT_PTR>(i), languageLabel(code).c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), translateId(app, L"language_label", L"Language").c_str());
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, translateId(app, L"settings_menu", L"Settings...").c_str());
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_OUTPUT, translateId(app, L"open_output_menu", L"Open output folder").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, translateId(app, L"exit_menu", L"Exit").c_str());

    SetForegroundWindow(app.mainWindow);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_RIGHTALIGN, pt.x, pt.y, 0, app.mainWindow, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return TRUE;
    }
    case WM_CREATE:
        app->mainWindow = hwnd;
        if (app->enableTray) {
            buildTrayIcon(*app);
        }
        updateAutoCaptureTimers(*app);
        if (!app->startWithSettings && !hasAnyAutoCaptureEnabled(*app)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (app->startWithSettings) {
            PostMessageW(hwnd, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id >= ID_TRAY_CAPTURE_BASE && id < ID_TRAY_CAPTURE_BASE + app->profiles.size()) {
            triggerCapture(*app, static_cast<int>(id - ID_TRAY_CAPTURE_BASE));
        } else {
            switch (id) {
            case ID_TRAY_SETTINGS:
                showSettingsWindow(*app);
                break;
            case ID_TRAY_OPEN_OUTPUT:
                openOutputFolder(*app, 0);
                break;
            case ID_TRAY_EXIT:
                DestroyWindow(hwnd);
                break;
            default:
                break;
            }
            if (id >= ID_TRAY_LANG_BASE && id < ID_TRAY_LANG_BASE + app->availableLanguages.size()) {
                size_t idx = id - ID_TRAY_LANG_BASE;
                if (idx < app->availableLanguages.size()) {
                    std::string lang = app->availableLanguages[idx];
                    setLanguage(*app, lang);
                    if (app->settingsWindow) {
                        DestroyWindow(app->settingsWindow);
                        app->settingsWindow = nullptr;
                        showSettingsWindow(*app);
                    }
                    showNotification(*app, translateId(*app, L"app_name", L"TCapture"),
                                    translateId(*app, L"language_changed", L"Language changed."));
                }
            }
        }
        return 0;
    }
    case WM_APP_CAPTURE_REQUEST: {
        int index = static_cast<int>(wParam);
        if (index >= 0 && index < static_cast<int>(app->profiles.size())) {
            triggerCapture(*app, index);
        }
        return 0;
    }
    case WM_APP_SHOW_SETTINGS:
        showSettingsWindow(*app);
        return 0;
    case WM_APP_SET_LANGUAGE: {
        const char* lang = (wParam == 1) ? "ja" : "en";
        const std::string prev = toLower(app->language);
        setLanguage(*app, lang);
        if (app->settingsWindow && prev != toLower(app->language)) {
            DestroyWindow(app->settingsWindow);
            app->settingsWindow = nullptr;
            showSettingsWindow(*app);
        }
        return 0;
    }
    case WM_APP_CAPTURE_FINISHED:
        maybeStopAgentIfIdle(*app);
        return 0;
    case WM_HOTKEY: {
        UINT id = static_cast<UINT>(wParam);
        if (id >= HOTKEY_ID_BASE) {
            size_t index = id - HOTKEY_ID_BASE;
            if (index < app->profiles.size()) {
                triggerCapture(*app, static_cast<int>(index));
            }
        }
        return 0;
    }
    case WM_TIMER: {
        UINT_PTR id = static_cast<UINT_PTR>(wParam);
        if (id >= TIMER_AUTO_BASE) {
            size_t index = id - TIMER_AUTO_BASE;
            if (index < app->profiles.size()) {
                triggerCapture(*app, static_cast<int>(index));
            }
        }
        return 0;
    }
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            showTrayMenu(*app, pt);
        } else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            showSettingsWindow(*app);
        }
        return 0;
    case WM_DESTROY:
        if (app->enableTray) {
            Shell_NotifyIcon(NIM_DELETE, &app->nid);
        }
        unregisterAllHotkeys(*app);
        stopAutoCaptureTimers(*app);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

enum class LaunchMode {
    Capture,
    Agent,
    Settings,
};

static LaunchMode detectLaunchMode(const std::vector<std::string>& args)
{
    for (size_t i = 1; i < args.size(); ++i) {
        std::string a = toLower(args[i]);
        if (a == "--settings") return LaunchMode::Settings;
        if (a == "--agent") return LaunchMode::Agent;
    }
    return LaunchMode::Capture;
}

static std::string detectPreferredLanguage(const std::vector<std::string>& args)
{
    for (size_t i = 1; i < args.size(); ++i) {
        std::string a = toLower(args[i]);
        if (a.rfind("--lang=", 0) == 0) {
            std::string v = a.substr(7);
            if (v == "ja" || v == "en") return v;
        }
        if (a == "--lang" && i + 1 < args.size()) {
            std::string v = toLower(args[i + 1]);
            if (v == "ja" || v == "en") return v;
        }
    }
    return std::string();
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    std::vector<std::string> args;
    if (argvW && argcW > 0) {
        args.reserve(static_cast<size_t>(argcW));
        for (int i = 0; i < argcW; ++i) {
            args.push_back(wideToUtf8(argvW[i]));
        }
    } else {
        args.push_back("TCapture.exe");
    }

    LaunchMode mode = detectLaunchMode(args);
    std::string preferredLanguage = detectPreferredLanguage(args);
    if (mode == LaunchMode::Capture) {
        std::vector<char*> argv;
        argv.reserve(args.size());
        for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        if (argvW) LocalFree(argvW);
        return RunCaptureMain(static_cast<int>(argv.size()), argv.data());
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HANDLE singleMutex = CreateMutexW(nullptr, FALSE, kSingletonMutexName);
    if (!singleMutex) {
        if (argvW) LocalFree(argvW);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mode == LaunchMode::Settings) {
            HWND existing = FindWindowW(kAgentMainWindowClass, nullptr);
            if (existing) {
                if (preferredLanguage == "ja" || preferredLanguage == "en") {
                    PostMessageW(existing, WM_APP_SET_LANGUAGE, (preferredLanguage == "ja") ? 1 : 0, 0);
                }
                PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
                SetForegroundWindow(existing);
            }
        }
        CloseHandle(singleMutex);
        if (argvW) LocalFree(argvW);
        return 0;
    }

    AppState app{};
    app.hInstance = hInstance;
    app.exeDir = getExecutableDir();
    app.enableTray = false;
    app.startWithSettings = (mode == LaunchMode::Settings);
    loadProfilesWithFallback(app, preferredLanguage);
    if (mode == LaunchMode::Agent && !hasAnyAutoCaptureEnabled(app)) {
        CloseHandle(singleMutex);
        if (argvW) LocalFree(argvW);
        return 0;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kAgentMainWindowClass;
    wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             0, 0, LR_DEFAULTSIZE));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, kAgentMainWindowClass, L"TCapture-Agent", WS_OVERLAPPED,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                nullptr, nullptr, hInstance, &app);
    if (!hwnd) {
        CloseHandle(singleMutex);
        if (argvW) LocalFree(argvW);
        return 1;
    }

    ShowWindow(hwnd, SW_HIDE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (native_edit::translate(msg)) continue;
        if (app.settingsWindow && IsDialogMessageW(app.settingsWindow, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (singleMutex) {
        CloseHandle(singleMutex);
    }
    if (argvW) LocalFree(argvW);
    return 0;
}
