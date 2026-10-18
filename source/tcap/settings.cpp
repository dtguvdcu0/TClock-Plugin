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
#include "settings.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string readGlobalLanguage(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (!line.empty() && line.front() == '[') break; // stop at first section
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (key == "ui_language" || key == "language" || key == "lang") {
            return value;
        }
    }
    return "";
}

std::vector<int> parseDisplayList(const std::string& text, bool& captureAll, bool& captureActiveDisplay, bool& captureActiveWindow) {
    std::vector<int> result;
    captureAll = false;
    captureActiveDisplay = false;
    captureActiveWindow = false;
    std::string current;
    auto flush = [&](const std::string& token) {
        std::string t = trim(token);
        if (t.empty()) return;
        std::string lower = toLower(t);
        if (lower == "all" || lower == "*") {
            captureAll = true;
            return;
        }
        if (lower == "active_display" || lower == "active-display" || lower == "active_d") {
            captureActiveDisplay = true;
            captureActiveWindow = false;
            result.clear();
            captureAll = false;
            return;
        }
        if (lower == "active_window" || lower == "active-window" || lower == "active_w") {
            captureActiveWindow = true;
            captureActiveDisplay = false;
            result.clear();
            captureAll = false;
            return;
        }
        try {
            int v = std::stoi(t);
            if (v == 0) {
                captureAll = true; // 0 means "all displays"
            } else if (v > 0) {
                result.push_back(v - 1); // convert 1-based -> 0-based
            }
        } catch (...) {
            // ignore invalid tokens
        }
    };

    for (char c : text) {
        if (c == ',') {
            flush(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    flush(current);

    // Deduplicate while preserving order.
    std::vector<int> unique;
    for (int v : result) {
        if (std::find(unique.begin(), unique.end(), v) == unique.end()) {
            unique.push_back(v);
        }
    }
    return unique;
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string narrow(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

void normalizeSettings(AppSettings& s) {
    s.format = toLower(s.format);
    if (s.format != "png" && s.format != "jpg") {
        s.format = "png";
    }
    // Opening images is interactive; never launch apps on an automatic or burst loop.
    if (s.openAfterCapture) { s.burstFps = 0; s.autoCapture = false; }
    s.pngCompression = std::clamp(s.pngCompression, 0, 9);
    s.jpgQuality = std::clamp(s.jpgQuality, 1, 100);
    s.compression = (s.format == "png") ? s.pngCompression : s.jpgQuality;
    s.burstFps = std::clamp(s.burstFps, 0, 60);
    s.burstSeconds = std::clamp(s.burstSeconds, 1, 3600);
    s.autoSeconds = std::clamp(s.autoSeconds, 1, 86400);
    if (s.outputDir.empty()) s.outputDir = ".";
    // Clean display indices: remove duplicates and negatives.
    std::vector<int> cleaned;
    for (int v : s.displayIndices) {
        if (v < 0) continue;
        if (std::find(cleaned.begin(), cleaned.end(), v) == cleaned.end()) {
            cleaned.push_back(v);
        }
    }
    s.displayIndices = std::move(cleaned);
    s.displaysRaw = trim(s.displaysRaw);
    if (s.captureActiveWindow) {
        s.captureActiveDisplay = false;
        s.displayIndices.clear();
    } else if (s.captureActiveDisplay) {
        s.displayIndices.clear();
    }
}

fs::path getExecutableDir() {
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (len == 0 || len == std::size(buffer)) {
        return fs::path(L".");
    }
    fs::path exePath(buffer);
    auto parent = exePath.parent_path();
    return parent.empty() ? fs::path(L".") : parent;
}

struct ParseCompressionState {
    bool legacySet = false;
    int legacyValue = 0;
    bool pngSet = false;
    bool jpgSet = false;
};

static void applyKeyValue(AppSettings& settings, ParseCompressionState& compState, const std::string& key, const std::string& value) {
    std::string lowerKey = toLower(trim(key));
    std::string trimmedValue = trim(value);
    if (lowerKey == "output_dir" || lowerKey == "output") {
        settings.outputDir = trimmedValue.empty() ? "." : trimmedValue;
    } else if (lowerKey == "capture_action") {
        const auto action = toLower(trimmedValue);
        settings.openAfterCapture = (action == "open" || action == "save_open");
        settings.saveBeforeOpen = (action == "save_open");
    } else if (lowerKey == "open_executable") {
        settings.openExecutable = utf8ToWide(trimmedValue);
    } else if (lowerKey == "open_app") {
        settings.openApp = utf8ToWide(trimmedValue);
    } else if (lowerKey == "format") {
        std::string fmt = toLower(trimmedValue);
        if (fmt == "png" || fmt == "jpg" || fmt == "jpeg") {
            settings.format = fmt == "jpeg" ? "jpg" : fmt;
        }
    } else if (lowerKey == "compression" || lowerKey == "quality") {
        try {
            int v = std::stoi(trimmedValue);
            compState.legacySet = true;
            compState.legacyValue = v;
            settings.pngCompression = v;
            settings.compression = v;
            if (toLower(settings.format) == "jpg" && !compState.jpgSet) {
                settings.jpgQuality = v;
                compState.jpgSet = true;
            }
        } catch (...) {
            // ignore invalid values
        }
    } else if (lowerKey == "compression_png" || lowerKey == "png_compression") {
        try {
            settings.pngCompression = std::stoi(trimmedValue);
            compState.pngSet = true;
        } catch (...) {
            // ignore invalid values
        }
    } else if (lowerKey == "compression_jpg" || lowerKey == "jpg_compression" || lowerKey == "jpg_quality") {
        try {
            settings.jpgQuality = std::stoi(trimmedValue);
            compState.jpgSet = true;
        } catch (...) {
            // ignore invalid values
        }
    } else if (lowerKey == "burst_fps" || lowerKey == "fps") {
        try {
            settings.burstFps = std::stoi(trimmedValue);
        } catch (...) {
            // ignore invalid values
        }
    } else if (lowerKey == "burst_seconds" || lowerKey == "burst_secs" || lowerKey == "seconds" || lowerKey == "duration") {
        try {
            settings.burstSeconds = std::stoi(trimmedValue);
        } catch (...) {
            // ignore invalid values
        }
    } else if (lowerKey == "auto_capture" || lowerKey == "auto" || lowerKey == "auto_enable") {
        std::string lowerVal = toLower(trimmedValue);
        settings.autoCapture = (lowerVal == "1" || lowerVal == "true" || lowerVal == "yes" || lowerVal == "on");
    } else if (lowerKey == "auto_seconds" || lowerKey == "auto_interval" || lowerKey == "interval") {
        try {
            settings.autoSeconds = std::stoi(trimmedValue);
        } catch (...) {
            // ignore
        }
    } else if (lowerKey == "language" || lowerKey == "lang") {
        settings.language = toLower(trimmedValue);
    } else if (lowerKey == "displays" || lowerKey == "display") {
        bool parsedAll = false;
        bool activeDisplay = false;
        bool activeWindow = false;
        settings.displayIndices = parseDisplayList(trimmedValue, parsedAll, activeDisplay, activeWindow);
        if (parsedAll) {
            settings.displayIndices.clear();
        }
        settings.captureActiveDisplay = activeDisplay;
        settings.captureActiveWindow = activeWindow;
        settings.displaysRaw = trimmedValue;
    } else if (lowerKey == "hotkey_capture" || lowerKey == "capture_hotkey" || lowerKey == "hotkey") {
        settings.captureHotkey = trimmedValue;
    }
}

static std::vector<ProfileSettings> parseIniProfiles(std::ifstream& in) {
    std::vector<ProfileSettings> profiles;
    ProfileSettings current;
    current.name = "default";
    ParseCompressionState compState{};
    bool hasLines = false;
    bool inIntegration = false;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            if (!inIntegration && (hasLines || profiles.empty())) {
                if (compState.legacySet && !compState.jpgSet && toLower(current.settings.format) == "jpg") {
                    current.settings.jpgQuality = compState.legacyValue;
                }
                normalizeSettings(current.settings);
                profiles.push_back(current);
            }
            current = ProfileSettings{};
            compState = ParseCompressionState{};
            std::string sectionName = trim(line.substr(1, line.size() - 2));
            current.name = sectionName.empty() ? "default" : sectionName;
            inIntegration = (toLower(current.name) == "integration");
            hasLines = false;
            continue;
        }
        if (inIntegration) continue;
        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;
        hasLines = true;
        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);
        applyKeyValue(current.settings, compState, key, value);
    }

    // Integration metadata is not a capture profile.
    if (!inIntegration) {
        if (compState.legacySet && !compState.jpgSet && toLower(current.settings.format) == "jpg") {
            current.settings.jpgQuality = compState.legacyValue;
        }
        normalizeSettings(current.settings);
        profiles.push_back(current);
    }
    return profiles;
}

std::vector<ProfileSettings> loadSettingsProfiles(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    auto profiles = parseIniProfiles(in);
    // Deduplicate by name: last wins.
    std::unordered_map<std::string, size_t> indexByName;
    std::vector<ProfileSettings> deduped;
    for (auto& p : profiles) {
        std::string lower = toLower(p.name);
        auto it = indexByName.find(lower);
        if (it == indexByName.end()) {
            indexByName[lower] = deduped.size();
            deduped.push_back(p);
        } else {
            deduped[it->second] = p;
        }
    }
    return deduped;
}

bool saveSettingsProfiles(const std::vector<ProfileSettings>& profiles, const fs::path& path, const std::string& language) {
    if (profiles.empty()) return false;
    fs::path parent = path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
    }
    // Preserve the integration destination before rewriting normalized profiles.
    std::string integration;
    {
        std::ifstream previous(path);
        if (!previous.is_open() && fs::exists(path)) return false;
        bool inIntegration = false;
        std::string line;
        while (std::getline(previous, line)) {
            const std::string clean = trim(line);
            if (!clean.empty() && clean.front() == '[' && clean.back() == ']') {
                inIntegration = (toLower(trim(clean.substr(1, clean.size() - 2))) == "integration");
            }
            if (inIntegration) integration += line + "\n";
        }
        if (previous.bad()) return false;
    }
    std::ofstream out(path);
    if (!out.is_open()) return false;

    (void)language;

    auto writeProfile = [&](const ProfileSettings& p) {
        out << "[" << p.name << "]\n";
        out << "output_dir=" << p.settings.outputDir << "\n";
        out << "capture_action=" << (p.settings.openAfterCapture ? (p.settings.saveBeforeOpen ? "save_open" : "open") : "save") << "\n";
        out << "open_app=" << wideToUtf8(p.settings.openApp) << "\n";
        out << "open_executable=" << wideToUtf8(p.settings.openExecutable) << "\n";
        out << "format=" << p.settings.format << "\n";
        out << "compression_png=" << p.settings.pngCompression << "\n";
        out << "compression_jpg=" << p.settings.jpgQuality << "\n";
        out << "burst_fps=" << p.settings.burstFps << "\n";
        out << "burst_seconds=" << p.settings.burstSeconds << "\n";
        out << "auto_capture=" << (p.settings.autoCapture ? "true" : "false") << "\n";
        out << "auto_seconds=" << p.settings.autoSeconds << "\n";
        if (p.settings.captureActiveWindow) {
            out << "displays=active_window\n";
        } else if (p.settings.captureActiveDisplay) {
            out << "displays=active_display\n";
        } else if (!p.settings.displaysRaw.empty()) {
            out << "displays=" << p.settings.displaysRaw << "\n";
        }
        out << "\n";
    };

    for (size_t i = 0; i < profiles.size(); ++i) {
        writeProfile(profiles[i]);
    }
    out << integration;
    out.close();
    return !out.fail();
}

bool loadSettings(AppSettings& settings, const fs::path& path, const std::string& profile) {
    auto profiles = loadSettingsProfiles(path);
    std::string target = toLower(profile.empty() ? "default" : profile);
    if (profiles.empty()) return false;
    for (const auto& p : profiles) {
        if (toLower(p.name) == target) {
            settings = p.settings;
            return true;
        }
    }
    // Fallback to default if requested profile missing.
    for (const auto& p : profiles) {
        if (toLower(p.name) == "default") {
            settings = p.settings;
            return true;
        }
    }
    settings = profiles.front().settings;
    return true;
}
