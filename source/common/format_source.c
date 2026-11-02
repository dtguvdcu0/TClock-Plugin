#include "format_source.h"
#include <float.h>
#include <stdint.h>
#include <string.h>

static BOOL fc_validate_text(const WCHAR* text, UINT capacity)
{
    UINT i;
    for (i = 0; i < capacity; i++) {
        WCHAR ch = text[i];
        if (!ch) return TRUE;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= capacity || text[i] < 0xDC00 || text[i] > 0xDFFF) return FALSE;
        }
        else if (ch >= 0xDC00 && ch <= 0xDFFF) return FALSE;
    }
    return FALSE;
}

static BOOL fc_validate_time(const SYSTEMTIME* time)
{
    FILETIME value;
    return time->wDayOfWeek <= 6 && SystemTimeToFileTime(time, &value);
}

UINT TcfSourceBytes(UINT calendarCount)
{
    if (!calendarCount || calendarCount > TCF_CALENDAR_MAX) return 0;
    return (UINT)offsetof(TC_FORMAT_SOURCE, calendar) + calendarCount * (UINT)sizeof(TC_FORMAT_CALENDAR);
}

BOOL TcfValidateSource(const TC_FORMAT_SOURCE* source, UINT bytes)
{
    UINT i, j;
    if (!source || (uintptr_t)source % 8 || bytes < offsetof(TC_FORMAT_SOURCE, calendar) ||
        bytes > TCF_SOURCE_MAX_BYTES || source->version != TCF_SOURCE_VERSION ||
        source->bytes != bytes || TcfSourceBytes(source->calendarCount) != bytes ||
        !source->revision || source->reserved || (source->options & ~TCF_OPT_ALL) ||
        (source->refreshedCategories & ~TCF_CATEGORY_ALL) ||
        source->sampledProcessors > TCF_PROCESSOR_COUNT ||
        source->sampledProcessors > (UINT)(source->logicalProcessors < 0 ? 0 : source->logicalProcessors) ||
        source->consumer < TCF_CONSUMER_MAIN || source->consumer > TCF_CONSUMER_MENU ||
        !source->megabytesPerGigabyte || source->beat100 < 0 || source->beat100 >= 100000 ||
        source->timeZoneBiasMinutes < -1440 || source->timeZoneBiasMinutes > 1440 ||
        !fc_validate_time(&source->time) || !fc_validate_text(source->format, TCF_FORMAT_CCH)) return FALSE;
    if (source->options & TCF_OPT_UTC) {
        if (!fc_validate_time(&source->utc)) return FALSE;
    }
    else {
        static const SYSTEMTIME empty = {0};
        if (memcmp(&source->utc, &empty, sizeof(empty))) return FALSE;
    }
    for (i = 0; i < TCF_LABEL_COUNT; i++)
        if (!fc_validate_text(source->labels[i], TCF_LABEL_CCH)) return FALSE;
    for (i = 0; i < TCF_CUSTOM_COUNT; i++) {
        if ((!(source->options & TCF_OPT_CUSTOM_READY) && (source->customState[i] || source->custom[i][0])) ||
            (source->customState[i] & ~TCF_CUSTOM_ALL) || !fc_validate_text(source->custom[i], TCF_CUSTOM_CCH) ||
            (!!source->custom[i][0] != !!(source->customState[i] & TCF_CUSTOM_PRESENT))) return FALSE;
    }
    for (i = 0; i < TCF_NETWORK_COUNT; i++) if (!_finite(source->network[i])) return FALSE;
    for (i = 0; i < TCF_SPACE_COUNT; i++)
        if (!_finite(source->diskFree[i]) || !_finite(source->diskTotal[i])) return FALSE;
    for (i = 0; i < TCF_RATE_COUNT; i++)
        if (!_finite(source->diskRead[i]) || !_finite(source->diskWrite[i]) || !_finite(source->diskRate[i])) return FALSE;
    if (memcmp(&source->calendar[0].time, &source->time, sizeof(SYSTEMTIME))) return FALSE;
    for (i = 0; i < source->calendarCount; i++) {
        const TC_FORMAT_CALENDAR* entry = &source->calendar[i];
        const WCHAR* values[] = {entry->shortDate,entry->longDate,entry->timeText,
            entry->shortDay,entry->longDay,entry->shortMonth,entry->longMonth};
        if (entry->reserved || (entry->valid & ~TCF_CAL_ALL) || !fc_validate_time(&entry->time)) return FALSE;
        for (j = 0; j < 7; j++) {
            if (!fc_validate_text(values[j], j < 3 ? 128u : 64u) ||
                (!(entry->valid & (1u << j)) && values[j][0])) return FALSE;
        }
        for (j = 0; j < i; j++)
            if (!memcmp(&entry->time, &source->calendar[j].time, sizeof(SYSTEMTIME))) return FALSE;
    }
    return TRUE;
}

const TC_FORMAT_CALENDAR* TcfFindCalendar(const TC_FORMAT_SOURCE* source, UINT bytes, const SYSTEMTIME* time)
{
    UINT i;
    if (!source || !time || bytes < offsetof(TC_FORMAT_SOURCE, calendar) || bytes > TCF_SOURCE_MAX_BYTES ||
        TcfSourceBytes(source->calendarCount) != bytes) return NULL;
    for (i = 0; i < source->calendarCount; i++)
        if (!memcmp(time, &source->calendar[i].time, sizeof(SYSTEMTIME))) return &source->calendar[i];
    return NULL;
}

static BOOL fc_shift_ticks(ULONGLONG original, LONGLONG delta, ULONGLONG* result)
{
    if ((delta < 0 && original < (ULONGLONG)-delta) ||
        (delta >= 0 && original > ~(ULONGLONG)0 - (ULONGLONG)delta)) return FALSE;
    *result = delta < 0 ? original - (ULONGLONG)-delta : original + (ULONGLONG)delta;
    return TRUE;
}

static BOOL fc_convert_time(ULONGLONG ticks, SYSTEMTIME* time)
{
    ULARGE_INTEGER value;
    FILETIME file;
    value.QuadPart = ticks;
    file.dwHighDateTime = value.HighPart; file.dwLowDateTime = value.LowPart;
    return FileTimeToSystemTime(&file, time);
}

BOOL TcfShiftTime(const TC_FORMAT_SOURCE* source, const SYSTEMTIME* previous,
    WCHAR mode, int hours, int minutes, BOOL negative, SYSTEMTIME* output, UINT* options)
{
    FILETIME file;
    ULARGE_INTEGER value;
    ULONGLONG ticks;
    LONGLONG delta;
    SYSTEMTIME shifted, utc;
    UINT flags;
    int day;
    BOOL summer;
    if (!source || !previous || !output || !options || (mode != L'd' && mode != L'u' && mode != L'e') ||
        hours < 0 || hours > 23 || minutes < 0 || minutes > 59 ||
        source->timeZoneBiasMinutes < -1440 || source->timeZoneBiasMinutes > 1440 ||
        !SystemTimeToFileTime(&source->time, &file)) return FALSE;
    flags = *options;
    if (mode == L'u' && previous->wYear > 2023) hours--;
    delta = ((LONGLONG)hours * 60 + minutes) * 600000000LL;
    if (negative) delta = -delta;
    value.HighPart = file.dwHighDateTime; value.LowPart = file.dwLowDateTime;
    if (!fc_shift_ticks(value.QuadPart, delta, &ticks) || !fc_convert_time(ticks, &shifted)) return FALSE;
    if (mode == L'u' && previous->wYear <= 2023) {
        day = shifted.wDayOfWeek ? shifted.wDayOfWeek : 7;
        summer = (shifted.wMonth > 3 && shifted.wMonth < 11) ||
            (shifted.wMonth == 3 && (shifted.wDay - day) > 7) ||
            (shifted.wMonth == 3 && shifted.wDay > 7 && shifted.wDay <= 14 && !shifted.wDayOfWeek && shifted.wHour >= 2) ||
            (shifted.wMonth == 11 && shifted.wDay <= shifted.wDayOfWeek) ||
            (shifted.wMonth == 11 && shifted.wDay <= 7 && !shifted.wDayOfWeek && !shifted.wHour);
        flags = summer ? flags | TCF_OPT_SUMMER_US : flags & ~TCF_OPT_SUMMER_US;
        if (summer && (!fc_shift_ticks(ticks, 36000000000LL, &ticks) || !fc_convert_time(ticks, &shifted))) return FALSE;
    }
    if (mode == L'e') {
        ULONGLONG utcTicks;
        if (!fc_shift_ticks(value.QuadPart, (LONGLONG)source->timeZoneBiasMinutes * 600000000LL, &utcTicks) ||
            !fc_convert_time(utcTicks, &utc)) return FALSE;
        day = utc.wDayOfWeek ? utc.wDayOfWeek : 7;
        summer = (utc.wMonth > 3 && utc.wMonth < 10) ||
            (utc.wMonth == 3 && (utc.wDay + 7 - day) > 31) ||
            (utc.wMonth == 3 && utc.wDay >= 24 && day == 7 && utc.wHour >= 1) ||
            (utc.wMonth == 10 && (utc.wDay + 7 - utc.wDayOfWeek) <= 31) ||
            (utc.wMonth == 10 && utc.wDay >= 24 && day == 7 && !utc.wHour);
        flags = summer ? flags | TCF_OPT_SUMMER_EUROPE : flags & ~TCF_OPT_SUMMER_EUROPE;
        if (summer && (!fc_shift_ticks(ticks, 36000000000LL, &ticks) || !fc_convert_time(ticks, &shifted))) return FALSE;
    }
    *output = shifted; *options = flags;
    return TRUE;
}

void TcfFreeSource(TC_FORMAT_SOURCE* source)
{
    if (source) HeapFree(GetProcessHeap(), 0, source);
}
