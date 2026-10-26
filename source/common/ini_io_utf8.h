#ifndef TC_INI_IO_UTF8_H
#define TC_INI_IO_UTF8_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL tc_ini_utf8_detect_file(const char* iniPath, BOOL* isUtf8, BOOL* hasBom);
void tc_ini_utf8_clear_cache(void);
BOOL tc_ini_beginW(LPCWSTR path);
void tc_ini_end(void);
/* Reads in a scope refresh after successful writers linked in this module; external or other-module edits appear in a fresh scope. */
int tc_ini_utf8_read_string(const char* iniPath, const char* section, const char* key,
                            const char* defval, char* outVal, int outSize);
int tc_ini_utf8_read_section_multisz(const char* iniPath, const char* section,
                                     char* outBuf, int outBytes);
int tc_ini_utf8_read_section_multisz_ex(const char* iniPath, const char* section,
                                        char* outBuf, int outBytes, BOOL* truncated);
BOOL tc_ini_utf8_write_string(const char* iniPath, const char* section, const char* key,
                              const char* val);
BOOL tc_ini_utf8_delete_key(const char* iniPath, const char* section, const char* key);
BOOL tc_ini_utf8_delete_section(const char* iniPath, const char* section);
BOOL tc_ini_utf8_selfcheck(void);
/* Atomic UTF-8 batch; each bounded multisz entry sets key=value or deletes key without an equals sign. */
BOOL tc_write_batchW(const wchar_t* iniPath, const char* section, const char* entries, DWORD entryBytes);

#ifdef __cplusplus
}
#endif

#endif
