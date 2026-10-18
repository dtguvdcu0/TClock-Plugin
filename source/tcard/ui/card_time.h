#pragma once
#include <windows.h>

namespace tcard_ui {
// Convert only for display; persisted and synchronized timestamps remain UTC.
inline bool convert_time(const FILETIME& stamp,SYSTEMTIME& local,const DYNAMIC_TIME_ZONE_INFORMATION* zone=nullptr)
{
    local={};SYSTEMTIME utc{};
    return FileTimeToSystemTime(&stamp,&utc)&&SystemTimeToTzSpecificLocalTimeEx(zone,&utc,&local);
}
}
