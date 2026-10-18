#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppSettings {
    std::string outputDir = ".";
    bool openAfterCapture = false;
    bool saveBeforeOpen = false;
    std::wstring openExecutable; // Optional executable selected directly instead of a Shell handler.
    std::wstring openApp; // Shell handler identity; empty selects the Windows default app.
    std::string format = "png"; // png or jpg
    int pngCompression = 6;     // 0-9
    int jpgQuality = 94;        // 1-100
    int compression = 6;        // deprecated single value for compatibility
    int burstFps = 0;           // 0 = off, otherwise captures repeatedly
    int burstSeconds = 1;       // duration in seconds for burst mode
    std::string language = "en"; // UI language: en, ja
    bool autoCapture = false;   // run capture on an interval (GUI)
    int autoSeconds = 60;       // seconds between auto captures
    std::vector<int> displayIndices; // empty -> all
    std::string displaysRaw;
    std::string captureHotkey;
    bool captureActiveDisplay = false;
    bool captureActiveWindow = false;
};

struct ProfileSettings {
    std::string name;
    AppSettings settings;
};

std::string toLower(std::string s);
std::string trim(const std::string& s);
std::vector<int> parseDisplayList(const std::string& text, bool& captureAll, bool& captureActiveDisplay, bool& captureActiveWindow);
std::wstring utf8ToWide(const std::string& text);
std::string wideToUtf8(const std::wstring& text);
bool loadSettings(AppSettings& settings, const std::filesystem::path& path = "TCapture.ini", const std::string& profile = "default");
std::vector<ProfileSettings> loadSettingsProfiles(const std::filesystem::path& path = "TCapture.ini");
bool saveSettingsProfiles(const std::vector<ProfileSettings>& profiles, const std::filesystem::path& path, const std::string& language = "en");
std::filesystem::path getExecutableDir();
void normalizeSettings(AppSettings& settings);
std::string readGlobalLanguage(const std::filesystem::path& path);
