#pragma once

#include <string>

namespace tcyc {

void LogInit(const std::wstring& logPath);
void LogSetLevel(int level);
void LogWrite(int level, const wchar_t* fmt, ...);

} // namespace tcyc

