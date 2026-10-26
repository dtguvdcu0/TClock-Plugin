#ifndef TC_TEXT_FILE_UTF8_H
#define TC_TEXT_FILE_UTF8_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL tc_read_text_file_utf8(const char* path, char** outText, DWORD* outSize, BOOL* outHadBom);
BOOL tc_write_text_file_utf8(const char* path, const char* text, DWORD size, BOOL writeBom);
void tc_free_text_buffer(void* p);
BOOL tc_text_file_utf8_selfcheck(void);

#ifdef __cplusplus
}
#endif

#endif
