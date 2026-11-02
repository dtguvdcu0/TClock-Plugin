#include "formatter_api.h"
#include <stdint.h>
#include <string.h>
#include <wchar.h>

static BOOL fmt_check_zone(UINT zone)
{
    return zone == TCF_ZONE_TEXT || zone == TCF_ZONE_DATE || zone == TCF_ZONE_WEEKDAY ||
        zone == TCF_ZONE_TIME || zone == TCF_ZONE_ACCENT;
}

static BOOL fmt_check_text(const WCHAR* text, UINT length)
{
    UINT i;
    if (!text && length) return FALSE;
    for (i = 0; i < length; i++) {
        WCHAR ch = text[i];
        if (!ch) return FALSE;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= length || text[i] < 0xDC00 || text[i] > 0xDFFF) return FALSE;
        }
        else if (ch >= 0xDC00 && ch <= 0xDFFF) return FALSE;
    }
    return TRUE;
}

static BOOL fmt_check_name(LPCWSTR name)
{
    UINT i;
    if (!name || !((name[0] >= L'A' && name[0] <= L'Z') ||
        (name[0] >= L'a' && name[0] <= L'z') || name[0] == L'_')) return FALSE;
    for (i = 1; i < TCF_SELECTOR_CCH; i++) {
        WCHAR ch = name[i];
        if (!ch) return TRUE;
        if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'_')) return FALSE;
    }
    return FALSE;
}

static BOOL fmt_check_style(const TCF_FRAME_STYLE* style)
{
    UINT length;
    if ((style->fields & ~TCF_STYLE_ALL) || (style->traits & ~TCF_TRAIT_ALL)) return FALSE;
    if ((style->fields & TCF_STYLE_FORE_COLOR) && (style->foreColor & 0xFF000000u)) return FALSE;
    if ((style->fields & TCF_STYLE_SHADOW_COLOR) && (style->shadowColor & 0xFF000000u)) return FALSE;
    if ((style->fields & TCF_STYLE_FONT_SIZE) && (style->fontSize < 5 || style->fontSize > 256)) return FALSE;
    if ((style->fields & TCF_STYLE_SHADOW_RANGE) && (style->shadowRange < 0 || style->shadowRange > 10)) return FALSE;
    if (style->fields & TCF_STYLE_FONT) {
        for (length = 0; length < TCF_FONT_CCH && style->fontFace[length]; length++) {}
        if (!length || length == TCF_FONT_CCH || !fmt_check_text(style->fontFace, length)) return FALSE;
    }
    return TRUE;
}

BOOL TcfValidateDemand(const TC_FORMAT_DEMAND* demand)
{
    if (!demand || demand->size != sizeof(*demand) || (demand->categories & ~TCF_CATEGORY_ALL) ||
        (demand->spaceDrives & ~0xFFFFFFFFFULL) || (demand->rateDrives & ~0x03FFFFFFu) ||
        demand->intervalMs < TCF_INTERVAL_MIN || demand->intervalMs > TCF_INTERVAL_MAX ||
        !demand->consumers || (demand->consumers & ~TCF_CONSUMERS_ALL) ||
        demand->reserved[0] || demand->reserved[1]) return FALSE;
    if (!!(demand->categories & TCF_CATEGORY_SPACE) != !!demand->spaceDrives ||
        !!(demand->categories & TCF_CATEGORY_RATE) != !!demand->rateDrives ||
        !!(demand->categories & TCF_CATEGORY_CUSTOM) != !!demand->customVariables) return FALSE;
    return TRUE;
}

void TcfInitFrame(TC_FORMAT_FRAME* frame, ULONGLONG sourceRevision)
{
    if (!frame) return;
    ZeroMemory(frame, sizeof(*frame));
    frame->version = TCF_FRAME_VERSION;frame->bytes = sizeof(*frame);
    frame->sourceRevision = sourceRevision;
}

BOOL TcfValidateFrame(const TC_FORMAT_FRAME* frame, UINT bytes,
    ULONGLONG sourceRevision, UINT capacity)
{
    UINT i, previousEnd = 0;
    if (!frame || (uintptr_t)frame % 8 || bytes != sizeof(*frame) ||
        !capacity || capacity > TCF_FRAME_CCH || !sourceRevision ||
        frame->version != TCF_FRAME_VERSION || frame->bytes != bytes ||
        frame->sourceRevision != sourceRevision || frame->reserved[0] || frame->reserved[1] ||
        frame->textLength >= capacity || frame->spanCount > TCF_FRAME_SPANS ||
        frame->text[frame->textLength] || frame->info[frame->textLength] ||
        !fmt_check_text(frame->text, frame->textLength)) return FALSE;
    for (i = 0; i < frame->textLength; i++) {
        if (!fmt_check_zone(frame->info[i])) return FALSE;
        if (frame->text[i] >= 0xD800 && frame->text[i] <= 0xDBFF) {
            if (frame->info[i] != frame->info[i + 1]) return FALSE;
            i++;
        }
    }
    for (i = 0; i < frame->spanCount; i++) {
        const TCF_FRAME_SPAN* span = &frame->spans[i];
        UINT j, occurrence = 0, end;
        if (span->start < previousEnd || span->start > frame->textLength ||
            span->length > frame->textLength - span->start ||
            !fmt_check_zone(span->zone) || !fmt_check_name(span->selector) || !fmt_check_style(&span->style)) return FALSE;
        end = span->start + span->length;
        if ((span->start < frame->textLength && frame->text[span->start] >= 0xDC00 && frame->text[span->start] <= 0xDFFF) ||
            (end < frame->textLength && frame->text[end] >= 0xDC00 && frame->text[end] <= 0xDFFF)) return FALSE;
        for (j = 0; j < i; j++) if (!wcscmp(frame->spans[j].selector, span->selector)) occurrence++;
        if (span->occurrence != occurrence) return FALSE;
        previousEnd = end;
    }
    return TRUE;
}

BOOL TcfAppendFrame(TC_FORMAT_FRAME* frame, UINT capacity, const WCHAR* text,
    UINT length, UINT zone, LPCWSTR selector, const TCF_FRAME_STYLE* style)
{
    UINT i, occurrence = 0, start;
    WCHAR name[TCF_SELECTOR_CCH];
    TCF_FRAME_STYLE selectedStyle = {0};
    if (!frame || !capacity || capacity > TCF_FRAME_CCH || frame->version != TCF_FRAME_VERSION ||
        frame->bytes != sizeof(*frame) || !frame->sourceRevision || frame->textLength >= capacity ||
        frame->spanCount > TCF_FRAME_SPANS || frame->text[frame->textLength] ||
        frame->info[frame->textLength] || length >= capacity - frame->textLength ||
        !fmt_check_zone(zone) || !fmt_check_text(text, length) || (style && !selector)) return FALSE;
    if (selector) {
        if (frame->spanCount == TCF_FRAME_SPANS || !fmt_check_name(selector) ||
            (style && !fmt_check_style(style))) return FALSE;
        lstrcpyW(name, selector);
        if (style) selectedStyle = *style;else ZeroMemory(&selectedStyle, sizeof(selectedStyle));
        for (i = 0; i < frame->spanCount; i++)
            if (!wcsncmp(frame->spans[i].selector, name, TCF_SELECTOR_CCH)) occurrence++;
    }
    start = frame->textLength;
    if (length) {
        MoveMemory(frame->text + start, text, (SIZE_T)length * sizeof(WCHAR));
        FillMemory(frame->info + start, length, (BYTE)zone);
    }
    frame->textLength += length;
    frame->text[frame->textLength] = L'\0';frame->info[frame->textLength] = 0;
    if (selector) {
        TCF_FRAME_SPAN* span = &frame->spans[frame->spanCount++];
        ZeroMemory(span, sizeof(*span));span->start = start;span->length = length;
        span->occurrence = occurrence;span->zone = zone;span->style = selectedStyle;
        lstrcpyW(span->selector, name);
    }
    return TRUE;
}
