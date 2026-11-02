#ifndef TCLOCK_FORMATTER_API_H
#define TCLOCK_FORMATTER_API_H
#include "format_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TC_PLUGIN_FORMATTER_ID L"tclock.formatter/1"
#define TCF_FRAME_VERSION 1u
#define TCF_FRAME_CCH 4096
#define TCF_FRAME_SPANS 256
#define TCF_SELECTOR_CCH 32
#define TCF_FONT_CCH 32
#define TCF_CONSUMERS_ALL 0x3Fu
#define TCF_INTERVAL_MIN 20u
#define TCF_INTERVAL_MAX 60000u

#define TCF_ZONE_TEXT 1u
#define TCF_ZONE_DATE 2u
#define TCF_ZONE_WEEKDAY 4u
#define TCF_ZONE_TIME 8u
#define TCF_ZONE_ACCENT 88u

#define TCF_STYLE_FORE_COLOR   0x001u
#define TCF_STYLE_FONT         0x002u
#define TCF_STYLE_FONT_SIZE    0x004u
#define TCF_STYLE_BOLD         0x008u
#define TCF_STYLE_ITALIC       0x010u
#define TCF_STYLE_SHADOW       0x020u
#define TCF_STYLE_BORDER       0x040u
#define TCF_STYLE_SHADOW_COLOR 0x080u
#define TCF_STYLE_SHADOW_RANGE 0x100u
#define TCF_STYLE_ALL          0x1FFu
#define TCF_TRAIT_BOLD   0x1u
#define TCF_TRAIT_ITALIC 0x2u
#define TCF_TRAIT_SHADOW 0x4u
#define TCF_TRAIT_BORDER 0x8u
#define TCF_TRAIT_ALL    0xFu

#pragma pack(push, 8)
typedef struct TC_FORMAT_DEMAND {
    UINT size;
    UINT categories;
    ULONGLONG spaceDrives;
    UINT rateDrives;
    UINT customVariables;
    UINT intervalMs;
    UINT consumers;
    UINT reserved[2];
} TC_FORMAT_DEMAND;

typedef struct TCF_FRAME_STYLE {
    UINT fields;
    COLORREF foreColor;
    COLORREF shadowColor;
    INT fontSize;
    INT shadowRange;
    UINT traits;
    WCHAR fontFace[TCF_FONT_CCH];
} TCF_FRAME_STYLE;

typedef struct TCF_FRAME_SPAN {
    UINT start;
    UINT length;
    UINT occurrence;
    UINT zone;
    TCF_FRAME_STYLE style;
    WCHAR selector[TCF_SELECTOR_CCH];
} TCF_FRAME_SPAN;

typedef struct TC_FORMAT_FRAME {
    UINT version;
    UINT bytes;
    ULONGLONG sourceRevision;
    UINT textLength;
    UINT spanCount;
    UINT reserved[2];
    WCHAR text[TCF_FRAME_CCH];
    BYTE info[TCF_FRAME_CCH];
    TCF_FRAME_SPAN spans[TCF_FRAME_SPANS];
} TC_FORMAT_FRAME;
#pragma pack(pop)

C_ASSERT(sizeof(TC_FORMAT_DEMAND) == 40);
C_ASSERT(sizeof(TCF_FRAME_STYLE) == 88);
C_ASSERT(sizeof(TCF_FRAME_SPAN) == 168);
C_ASSERT(sizeof(TC_FORMAT_FRAME) == 55328);

/* Native, in-host callback table: never serialize this function pointer.
   The registration owns the immutable table/user until invocations and stop end.
   Callback input/output are borrowed for this call only. No delayed writes.
   Return S_FALSE to decline; initialize successful output with TcfInitFrame. */
typedef struct TC_FORMATTER {
    UINT size;
    TC_FORMAT_DEMAND demand;
    HRESULT (WINAPI *format)(void* user, const TC_FORMAT_SOURCE* source,
        UINT sourceBytes, UINT capacity, TC_FORMAT_FRAME* output);
} TC_FORMATTER;

/* /2 separates UI-owned snapshot lifetime from pure worker execution.
   capture/release are UI callbacks. A successful capture transfers one strong,
   immutable snapshot reference to the host; release receives only that snapshot
   and must remain safe after remove, stop, or another runtime start.
   format runs serially per provider on a worker and cannot call host/UI APIs.
   Replace registration identity when configuration or demand changes. */
#define TC_PLUGIN_FORMATTER2_ID L"tclock.formatter/2"
#pragma pack(push, 8)
typedef struct TC_FORMATTER2 {
    UINT size;
    TC_FORMAT_DEMAND demand;
    HRESULT (WINAPI *capture)(void* user, void** snapshot);
    HRESULT (WINAPI *format)(const void* snapshot, const TC_FORMAT_SOURCE* source,
        UINT sourceBytes, UINT capacity, TC_FORMAT_FRAME* output);
    void (WINAPI *release)(void* snapshot);
} TC_FORMATTER2;
#pragma pack(pop)
#ifdef _WIN64
C_ASSERT(sizeof(TC_FORMATTER2) == 72);
#else
C_ASSERT(sizeof(TC_FORMATTER2) == 64);
#endif

BOOL TcfValidateDemand(const TC_FORMAT_DEMAND* demand);
void TcfInitFrame(TC_FORMAT_FRAME* frame, ULONGLONG sourceRevision);
BOOL TcfValidateFrame(const TC_FORMAT_FRAME* frame, UINT bytes,
    ULONGLONG sourceRevision, UINT capacity);
/* Builder for an exclusively owned initialized frame. Length excludes NUL.
   A NULL selector appends a literal; an empty named slot uses length zero.
   A rejected append leaves the frame unchanged. Final validation is mandatory. */
BOOL TcfAppendFrame(TC_FORMAT_FRAME* frame, UINT capacity, const WCHAR* text,
    UINT length, UINT zone, LPCWSTR selector, const TCF_FRAME_STYLE* style);

#ifdef __cplusplus
}
#endif
#endif
