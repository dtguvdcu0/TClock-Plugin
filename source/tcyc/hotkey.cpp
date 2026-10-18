#include "hotkey.h"

#include <windows.h>
#include <unordered_map>

namespace tcyc {

namespace {

struct HotkeySpec {
    UINT modifiers = 0;
    UINT key = 0;
};

std::unordered_map<UINT, int> g_hotkeyTaskById;
std::vector<UINT> g_hotkeyIds;
const UINT kHotkeyIdBase = 0x7B00;

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) b++;
    size_t e = s.size();
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) e--;
    return s.substr(b, e - b);
}

bool ParseHotkey(const std::wstring& text, HotkeySpec& out) {
    out = {};
    std::wstring token;
    std::wstring lower = ToLower(Trim(text));
    if (lower.empty()) return false;

    auto flushToken = [&](const std::wstring& raw) {
        const std::wstring t = Trim(raw);
        if (t.empty()) return;
        if (t == L"ctrl" || t == L"control") out.modifiers |= MOD_CONTROL;
        else if (t == L"shift") out.modifiers |= MOD_SHIFT;
        else if (t == L"alt") out.modifiers |= MOD_ALT;
        else if (t == L"win" || t == L"windows") out.modifiers |= MOD_WIN;
        else if (out.key == 0) {
            if (t.size() == 1) {
                wchar_t c = t[0];
                if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) out.key = static_cast<UINT>(c);
            } else if (t.size() >= 2 && t[0] == L'f') {
                int fn = _wtoi(t.c_str() + 1);
                if (fn >= 1 && fn <= 24) out.key = VK_F1 + static_cast<UINT>(fn - 1);
            } else if (t == L"printscreen" || t == L"prtsc" || t == L"prtscr") out.key = VK_SNAPSHOT;
            else if (t == L"insert") out.key = VK_INSERT;
            else if (t == L"delete") out.key = VK_DELETE;
            else if (t == L"home") out.key = VK_HOME;
            else if (t == L"end") out.key = VK_END;
            else if (t == L"pgup" || t == L"pageup") out.key = VK_PRIOR;
            else if (t == L"pgdn" || t == L"pagedown") out.key = VK_NEXT;
            else if (t == L"space" || t == L"spacebar") out.key = VK_SPACE;
            else if (t == L"enter" || t == L"return") out.key = VK_RETURN;
        }
    };

    for (wchar_t c : lower) {
        if (c == L'+') {
            flushToken(token);
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    flushToken(token);
    if (out.key == 0) return false;
    out.modifiers |= MOD_NOREPEAT;
    return true;
}

bool HasHotkeyTrigger(const TaskConfig& t) {
    if (t.triggerMask != 0) {
        return (t.triggerMask & (1 << 4)) != 0;
    }
    return t.trigger == TriggerType::HotkeyOnly;
}

} // namespace

std::wstring BuildHotkeyConfigSignature(const RuntimeConfig& cfg) {
    std::wstring sig;
    for (const auto& t : cfg.tasks) {
        if (!t.enabled) continue;
        if (!HasHotkeyTrigger(t)) continue;
        sig.append(std::to_wstring(t.id));
        sig.push_back(L':');
        sig.append(ToLower(Trim(t.hotkey)));
        sig.push_back(L';');
    }
    return sig;
}

void UnregisterAllHotkeys() {
    for (UINT id : g_hotkeyIds) {
        UnregisterHotKey(nullptr, static_cast<int>(id));
    }
    g_hotkeyIds.clear();
    g_hotkeyTaskById.clear();
}

bool ReloadHotkeys(const RuntimeConfig& cfg, std::wstring& outError) {
    outError.clear();
    UnregisterAllHotkeys();
    UINT serial = 0;
    for (const auto& t : cfg.tasks) {
        if (!t.enabled || !HasHotkeyTrigger(t)) continue;
        HotkeySpec spec{};
        if (!ParseHotkey(t.hotkey, spec)) continue;
        UINT id = kHotkeyIdBase + serial++;
        if (!RegisterHotKey(nullptr, static_cast<int>(id), spec.modifiers, spec.key)) {
            wchar_t msg[128] = {0};
            swprintf_s(msg, L"RegisterHotKey failed id=%u err=%lu", id, GetLastError());
            outError = msg;
            continue;
        }
        g_hotkeyIds.push_back(id);
        g_hotkeyTaskById[id] = t.id;
    }
    return true;
}

std::vector<int> DrainHotkeyTaskIds() {
    std::vector<int> out;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message != WM_HOTKEY) continue;
        UINT id = static_cast<UINT>(msg.wParam);
        auto it = g_hotkeyTaskById.find(id);
        if (it != g_hotkeyTaskById.end()) out.push_back(it->second);
    }
    return out;
}

} // namespace tcyc

