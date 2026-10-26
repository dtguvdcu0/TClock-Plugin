#ifndef TCLOCK_COLOR_VALUE_H
#define TCLOCK_COLOR_VALUE_H

#include <windows.h>

typedef enum TCV_KIND {
    TCV_KIND_RGB = 1,
    TCV_KIND_HEX = 2,
    TCV_KIND_NAME = 3,
    TCV_KIND_DECIMAL = 4
} TCV_KIND;

BOOL tcv_parse_next(const WCHAR** cursor, COLORREF* color, TCV_KIND* kind);
BOOL tcv_parse(const WCHAR* text, COLORREF* color, TCV_KIND* kind);
BOOL tcv_parse_ascii(const char* text, COLORREF* color, TCV_KIND* kind);

#endif
