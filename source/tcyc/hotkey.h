#pragma once

#include "config.h"

#include <string>
#include <vector>

namespace tcyc {

std::wstring BuildHotkeyConfigSignature(const RuntimeConfig& cfg);
bool ReloadHotkeys(const RuntimeConfig& cfg, std::wstring& outError);
void UnregisterAllHotkeys();
std::vector<int> DrainHotkeyTaskIds();

} // namespace tcyc

