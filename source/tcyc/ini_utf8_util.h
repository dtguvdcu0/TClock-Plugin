#pragma once

#include <string>
#include <vector>

namespace tcyc {

struct IniUpdate { std::wstring section, key, value; };
// Remove explicitly deleted sections in the same backed-up atomic transaction.
bool WriteIniUtf8Values(const std::wstring& iniPath, const std::vector<IniUpdate>& updates,
                       const std::vector<std::wstring>& removedSections = {});

bool ReadIniUtf8Value(const std::wstring& iniPath,
                      const std::wstring& section,
                      const std::wstring& key,
                      const std::wstring& defValue,
                      std::wstring& outValue);

bool WriteIniUtf8Value(const std::wstring& iniPath,
                       const std::wstring& section,
                       const std::wstring& key,
                       const std::wstring& value);

}

