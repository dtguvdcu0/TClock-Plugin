#ifndef TCLOCK_FORMAT_SOURCE_H
#define TCLOCK_FORMAT_SOURCE_H

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source body only. Runtime /1 tables and the future IPC envelope are separate. */
#define TCF_SOURCE_VERSION 1u
#define TCF_SOURCE_MAX_BYTES (1024u * 1024u)
#define TCF_FORMAT_CCH 4096
#define TCF_CUSTOM_COUNT 32
#define TCF_CUSTOM_CCH 4096
#define TCF_LABEL_CCH 128
#define TCF_PROCESSOR_COUNT 64
#define TCF_NETWORK_COUNT 21
#define TCF_SPACE_COUNT 72
#define TCF_RATE_COUNT 26
#define TCF_CALENDAR_MAX (TCF_FORMAT_CCH / 8 + 1)

#define TCF_OPT_HOUR12        0x0001u
#define TCF_OPT_HOUR_ZERO     0x0002u
#define TCF_OPT_SAFE          0x0004u
#define TCF_OPT_CLOCK2        0x0008u
#define TCF_OPT_TEMPERATURE   0x0010u
#define TCF_OPT_CHARGING      0x0020u
#define TCF_OPT_MUTE          0x0040u
#define TCF_OPT_VPN           0x0080u
#define TCF_OPT_METERED       0x0100u
#define TCF_OPT_CUSTOM_READY  0x0200u
#define TCF_OPT_SUMMER_US     0x0400u
#define TCF_OPT_SUMMER_EUROPE 0x0800u
#define TCF_OPT_GIP_ALIGNED   0x1000u
#define TCF_OPT_UTC           0x2000u
#define TCF_OPT_ALL           0x3FFFu

#define TCF_CATEGORY_CPU        0x0001u
#define TCF_CATEGORY_BATTERY    0x0002u
#define TCF_CATEGORY_MEMORY     0x0004u
#define TCF_CATEGORY_NETWORK    0x0008u
#define TCF_CATEGORY_CONNECTION 0x0010u
#define TCF_CATEGORY_SPACE      0x0020u
#define TCF_CATEGORY_RATE       0x0040u
#define TCF_CATEGORY_VOLUME     0x0080u
#define TCF_CATEGORY_GPU        0x0100u
#define TCF_CATEGORY_TEMP       0x0200u
#define TCF_CATEGORY_CUSTOM     0x0400u
#define TCF_CATEGORY_GIP        0x0800u
#define TCF_CATEGORY_ALL        0x0FFFu

#define TCF_CUSTOM_PRESENT 0x01u
#define TCF_CUSTOM_PATH    0x02u
#define TCF_CUSTOM_SCRIPT  0x04u
#define TCF_CUSTOM_ALL     0x07u

typedef enum TCF_CONSUMER {
    TCF_CONSUMER_MAIN = 1,
    TCF_CONSUMER_TOOLTIP_BODY,
    TCF_CONSUMER_TOOLTIP_TITLE,
    TCF_CONSUMER_LED,
    TCF_CONSUMER_LED_PREVIEW,
    TCF_CONSUMER_MENU
} TCF_CONSUMER;

typedef enum TCF_LABEL {
    TCF_LABEL_DATE_SEPARATOR,
    TCF_LABEL_TIME_SEPARATOR,
    TCF_LABEL_AM,
    TCF_LABEL_PM,
    TCF_LABEL_ERA,
    TCF_LABEL_SSID,
    TCF_LABEL_APN,
    TCF_LABEL_LTE,
    TCF_LABEL_LTE_CHAR,
    TCF_LABEL_MUTE,
    TCF_LABEL_IP_ETHERNET,
    TCF_LABEL_IP_WIFI,
    TCF_LABEL_IP_LTE,
    TCF_LABEL_IP_VPN,
    TCF_LABEL_GIP,
    TCF_LABEL_GIP_ALIGNED,
    TCF_LABEL_NETWORK_MIX,
    TCF_LABEL_COUNT
} TCF_LABEL;

typedef enum TCF_MEMORY {
    TCF_MEMORY_TOTAL_PHYSICAL,
    TCF_MEMORY_AVAILABLE_PHYSICAL,
    TCF_MEMORY_TOTAL_PAGEFILE,
    TCF_MEMORY_AVAILABLE_PAGEFILE,
    TCF_MEMORY_TOTAL_VIRTUAL,
    TCF_MEMORY_AVAILABLE_VIRTUAL,
    TCF_MEMORY_COUNT
} TCF_MEMORY;

#define TCF_CAL_SHORT_DATE  0x01u
#define TCF_CAL_LONG_DATE   0x02u
#define TCF_CAL_TIME        0x04u
#define TCF_CAL_SHORT_DAY   0x08u
#define TCF_CAL_LONG_DAY    0x10u
#define TCF_CAL_SHORT_MONTH 0x20u
#define TCF_CAL_LONG_MONTH  0x40u
#define TCF_CAL_ALL         0x7Fu

/* Fixed-width fields and eight-byte packing are identical in C/C++ and x86/x64. */
#pragma pack(push, 8)
typedef struct TC_FORMAT_CALENDAR {
    SYSTEMTIME time;
    UINT valid;
    UINT reserved;
    WCHAR shortDate[128];
    WCHAR longDate[128];
    WCHAR timeText[128];
    WCHAR shortDay[64];
    WCHAR longDay[64];
    WCHAR shortMonth[64];
    WCHAR longMonth[64];
} TC_FORMAT_CALENDAR;

typedef struct TC_FORMAT_SOURCE {
    UINT version;
    UINT bytes;
    UINT consumer;
    UINT options;
    ULONGLONG revision;
    ULONGLONG sampleTick;
    SYSTEMTIME time;
    /* UTC origin is optional; never infer it across an ambiguous local hour. */
    SYSTEMTIME utc;
    INT beat100;
    INT timeZoneBiasMinutes;
    INT alternativeYear;
    INT logicalProcessors;
    INT physicalCores;
    UINT sampledProcessors;
    INT totalCpu;
    INT clock2Average;
    INT cpuUsage[TCF_PROCESSOR_COUNT];
    INT cpuClock[TCF_PROCESSOR_COUNT];
    INT cpuClock2[TCF_PROCESSOR_COUNT];
    ULONGLONG memory[TCF_MEMORY_COUNT];
    UINT megabytesPerGigabyte;
    /* Categories whose owner attempted a refresh before this capture, not proof
       that hardware collection succeeded. Zero means no freshness assertion. */
    UINT refreshedCategories;
    double network[TCF_NETWORK_COUNT];
    double diskFree[TCF_SPACE_COUNT];
    double diskTotal[TCF_SPACE_COUNT];
    double diskRead[TCF_RATE_COUNT];
    double diskWrite[TCF_RATE_COUNT];
    double diskRate[TCF_RATE_COUNT];
    INT batteryLife;
    INT batteryHours;
    INT batteryMinutes;
    INT batterySeconds;
    INT powerSource;
    INT volume;
    INT gpuUsage;
    INT gpuInstances;
    INT temperature;
    INT connection;
    INT physicalAdapter;
    INT lteProfile;
    INT networkWidth;
    INT ssidWidth;
    UINT customState[TCF_CUSTOM_COUNT];
    UINT calendarCount;
    UINT reserved;
    WCHAR format[TCF_FORMAT_CCH];
    WCHAR labels[TCF_LABEL_COUNT][TCF_LABEL_CCH];
    WCHAR custom[TCF_CUSTOM_COUNT][TCF_CUSTOM_CCH];
    /* Only calendarCount entries are present in bytes; never read the tail. */
    TC_FORMAT_CALENDAR calendar[TCF_CALENDAR_MAX];
} TC_FORMAT_SOURCE;
#pragma pack(pop)

C_ASSERT(sizeof(TC_FORMAT_SOURCE) <= TCF_SOURCE_MAX_BYTES);
C_ASSERT(sizeof(TC_FORMAT_CALENDAR) == 1304);

/* Callers own a readable buffer of bytes; no function retains that buffer. */
UINT TcfSourceBytes(UINT calendarCount);
BOOL TcfValidateSource(const TC_FORMAT_SOURCE* source, UINT bytes);
const TC_FORMAT_CALENDAR* TcfFindCalendar(const TC_FORMAT_SOURCE* source, UINT bytes, const SYSTEMTIME* time);
/* td/tu/te use the captured base, previous display year and local DST state.
   On failure neither output is modified. No global time/locale state is read. */
BOOL TcfShiftTime(const TC_FORMAT_SOURCE* source, const SYSTEMTIME* previous,
    WCHAR mode, int hours, int minutes, BOOL negative, SYSTEMTIME* output, UINT* options);
void TcfFreeSource(TC_FORMAT_SOURCE* source);

#ifdef __cplusplus
}
#endif
#endif
