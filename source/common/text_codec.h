#ifndef TC_TEXT_CODEC_H
#define TC_TEXT_CODEC_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int tc_utf8_to_utf16(const char* utf8, wchar_t* outWide, int outWideCch);
int tc_utf16_to_utf8(const wchar_t* wide, char* outUtf8, int outUtf8Bytes);
int tc_ansi_to_utf16(UINT codepage, const char* ansi, wchar_t* outWide, int outWideCch);
/* Compatibility boundary: ANSI egress API is kept for legacy char-based call sites. */
int tc_utf16_to_ansi(UINT codepage, const wchar_t* wide, char* outAnsi, int outAnsiBytes);
int tc_ansi_to_utf16_compat(UINT preferredCodePage, const char* ansi, wchar_t* outWide, int outWideCch);
/* Compatibility boundary: preferred+fallback ANSI egress API is kept for legacy call sites. */
int tc_utf16_to_ansi_compat(UINT preferredCodePage, const wchar_t* wide, char* outAnsi, int outAnsiBytes);
int tc_ansi_bytes_to_utf16_compat(UINT preferredCodePage, const char* ansi, int ansiBytes, wchar_t* outWide, int outWideCch);
BOOL tc_path_utf8_or_ansi_to_utf16(const char* path, wchar_t* outWide, int outWideCch);
HANDLE tc_find_first_file_utf8_compat(const char* path, WIN32_FIND_DATAW* findData);
UINT tc_current_ansi_codepage(void);
BOOL tc_text_codec_selfcheck(void);

#ifdef __cplusplus
}
#endif

#endif
