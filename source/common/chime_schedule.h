#pragma once
/* Shared by the editor preview and runtime. Sunday is bit 0. */
#define CHM_VALIDATE (WM_APP + 130)
static __inline int chm_target_hour(int hour, int offset)
{
    return (hour + (offset < 0 ? 1 : 0)) % 24;
}
static __inline int chm_matches(int hour, int second, int offset, int start, int end, int secondary)
{
    if ((3600 + second - offset) % 3600 != 0) return 0;
    if (!secondary) {
        int target = chm_target_hour(hour, offset);
        return target >= start && target <= end;
    }
    /* Preserve the legacy secondary boundary, including negative offsets. */
    if (offset < 0)
        return ((hour == 23 && start == 0) || start - hour - 1 <= 0) && end - hour > 0;
    return start - hour <= 0 && (end - hour >= 0 || (hour == 0 && end == 24));
}
static __inline int chm_allows_day(int mask, int day)
{
    return day >= 0 && day < 7 && (mask & (1 << day)) != 0;
}
