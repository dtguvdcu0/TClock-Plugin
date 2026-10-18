#pragma once

#include "config.h"
#include <string>

namespace tcyc {

int RunReadOnlySettingsWindow(const RuntimeConfig& cfg, const std::wstring& iniPath, const std::wstring& exeDir, const std::string& preferredLanguage);

} // namespace tcyc

