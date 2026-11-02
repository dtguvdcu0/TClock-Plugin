/*
 * 時計の文字列を作る、フォーマッタプラグインのサンプルです。
 * 本体の設定で有効にすると、メイン時計とツールチップ(本文/タイトル)の文字列を
 * このDLLが作ります。DLLはTClock本体(exe)の中で動き、Explorerには読み込まれません。
 *
 * 書式文字列の中の次の名前を値に置き換え、それ以外の文字はそのまま出力します。
 *   ${time}    時:分:秒 (24時間)
 *   ${date}    短い日付 (取得できないときは yyyy/mm/dd)
 *   ${cpu}     CPU使用率(%)。SF_HOT_CPU 以上のときは赤色で表示します
 *   ${mem}     物理メモリ使用率(%)
 *   ${battery} バッテリー残量(%)。取得できないときは --
 * 本体が書式に付ける区切り <% %> と引用符 " は出力しません。
 */
#include "../common/plugin_api.h"
#include "../common/formatter_api.h"
#include <stdio.h>
#include <wchar.h>

#define SF_HOT_CPU 80
#define SF_HOT_COLOR RGB(220, 40, 40)

/*
 * 本体に伝える収集要求です。ここに書いた種類の値だけが入力に入ります。
 * 対象はメイン時計とツールチップ本文/タイトル、更新間隔は1秒です。
 */
#define SF_CONSUMERS ((1u << (TCF_CONSUMER_MAIN - 1)) | (1u << (TCF_CONSUMER_TOOLTIP_BODY - 1)) | \
    (1u << (TCF_CONSUMER_TOOLTIP_TITLE - 1)))
#define SF_DEMAND { sizeof(TC_FORMAT_DEMAND), TCF_CATEGORY_CPU | TCF_CATEGORY_MEMORY | TCF_CATEGORY_BATTERY, \
    0, 0, 0, 1000, SF_CONSUMERS, { 0, 0 } }

/*
 * 整形処理は本体の作業スレッドで動くため、変更されない設定だけを渡します。
 * このサンプルの設定は定数なので、static const の値をそのまま渡します。
 */
typedef struct SF_SNAPSHOT { int hotCpu; COLORREF hotColor; } SF_SNAPSHOT;
static const SF_SNAPSHOT sf_settings = { SF_HOT_CPU, SF_HOT_COLOR };

static HRESULT WINAPI sf_capture(void* user, void** snapshot) {
    (void)user;
    *snapshot = (void*)&sf_settings;
    return S_OK;
}

/* static const の値を渡しているので、解放するものはありません。 */
static void WINAPI sf_release(void* snapshot) {
    (void)snapshot;
}

/* name で始まるときだけ、その長さを返します。 */
static UINT sf_match(const WCHAR* text, const WCHAR* name) {
    size_t length = wcslen(name);
    return wcsncmp(text, name, length) == 0 ? (UINT)length : 0;
}

static int sf_percent(ULONGLONG used, ULONGLONG total) {
    return total ? (int)(used * 100 / total) : 0;
}

/*
 * 入力の書式を先頭から読み、置き換えた結果を output に追加します。
 * output は本体が用意した専用の領域で、この呼び出しの間だけ書き込めます。
 */
static HRESULT WINAPI sf_format(const void* state, const TC_FORMAT_SOURCE* source,
    UINT bytes, UINT capacity, TC_FORMAT_FRAME* output) {
    const SF_SNAPSHOT* settings = (const SF_SNAPSHOT*)state;
    const TC_FORMAT_CALENDAR* calendar;
    const WCHAR* cursor;
    TCF_FRAME_STYLE hot = { 0 };

    if (!settings || !TcfValidateSource(source, bytes)) return E_INVALIDARG;
    TcfInitFrame(output, source->revision);
    hot.fields = TCF_STYLE_FORE_COLOR;
    hot.foreColor = settings->hotColor;
    calendar = TcfFindCalendar(source, bytes, &source->time);

    for (cursor = source->format; *cursor;) {
        WCHAR value[64];
        LPCWSTR selector = NULL;
        const TCF_FRAME_STYLE* style = NULL;
        UINT advance, zone = TCF_ZONE_TEXT;

        /* 本体が付けた区切りと引用符は読み飛ばします。 */
        if ((cursor[0] == L'<' && cursor[1] == L'%') || (cursor[0] == L'%' && cursor[1] == L'>')) { cursor += 2; continue; }
        if (cursor[0] == L'"') { ++cursor; continue; }

        if ((advance = sf_match(cursor, L"${time}")) != 0) {
            swprintf_s(value, _countof(value), L"%02u:%02u:%02u", source->time.wHour, source->time.wMinute, source->time.wSecond);
            selector = L"time"; zone = TCF_ZONE_TIME;
        } else if ((advance = sf_match(cursor, L"${date}")) != 0) {
            if (calendar && (calendar->valid & TCF_CAL_SHORT_DATE)) wcscpy_s(value, _countof(value), calendar->shortDate);
            else swprintf_s(value, _countof(value), L"%04u/%02u/%02u", source->time.wYear, source->time.wMonth, source->time.wDay);
            selector = L"date"; zone = TCF_ZONE_DATE;
        } else if ((advance = sf_match(cursor, L"${cpu}")) != 0) {
            swprintf_s(value, _countof(value), L"%d", source->totalCpu);
            selector = L"cpu";
            if (source->totalCpu >= settings->hotCpu) style = &hot;
        } else if ((advance = sf_match(cursor, L"${mem}")) != 0) {
            ULONGLONG total = source->memory[TCF_MEMORY_TOTAL_PHYSICAL];
            ULONGLONG available = source->memory[TCF_MEMORY_AVAILABLE_PHYSICAL];
            swprintf_s(value, _countof(value), L"%d", sf_percent(total > available ? total - available : 0, total));
            selector = L"mem";
        } else if ((advance = sf_match(cursor, L"${battery}")) != 0) {
            if (source->batteryLife >= 0 && source->batteryLife <= 100) swprintf_s(value, _countof(value), L"%d", source->batteryLife);
            else wcscpy_s(value, _countof(value), L"--");
            selector = L"battery";
        } else {
            /* 置き換え対象でない文字は1文字ずつ出力します。サロゲートペアは2単位まとめます。 */
            UINT length = (cursor[0] >= 0xD800 && cursor[0] <= 0xDBFF && cursor[1] >= 0xDC00 && cursor[1] <= 0xDFFF) ? 2 : 1;
            if (!TcfAppendFrame(output, capacity, cursor, length, TCF_ZONE_TEXT, NULL, NULL)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            cursor += length;
            continue;
        }
        if (!TcfAppendFrame(output, capacity, value, (UINT)wcslen(value), zone, selector, style)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        cursor += advance;
    }
    return S_OK;
}

/* 本体へ登録する関数表です。登録している間は変更しないので static const にします。 */
static const TC_FORMATTER2 sf_formatter = { sizeof(TC_FORMATTER2), SF_DEMAND, sf_capture, sf_format, sf_release };

/*
 * プラグインの開始時に、フォーマッタを本体へ登録します。
 * 複数のフォーマッタが有効なときは、priority の大きいものが使われます。
 */
static HRESULT WINAPI sf_start(const TC_PLUGIN_CONTEXT* context) {
    const TC_HOST_FEATURES* features;
    TC_FEATURE_REGISTRATION entry = { sizeof(entry) };
    TC_REGISTRATION token;
    HRESULT result;
    if (!context || context->size < sizeof(*context) || !context->query_host) return E_INVALIDARG;
    result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_FEATURES_ID, (const void**)&features);
    if (FAILED(result)) return result;
    entry.id = L"format";
    entry.interface_id = TC_PLUGIN_FORMATTER2_ID;
    entry.priority = 0;
    entry.table = &sf_formatter;
    entry.user = NULL;
    return features->add(context->plugin, &entry, &token);
}

/* 登録は本体が停止時に取り消します。このサンプルに解放する資源はありません。 */
static void WINAPI sf_stop(void) {
}

static const TC_PLUGIN_RUNTIME sf_runtime = { sizeof(TC_PLUGIN_RUNTIME), sf_start, NULL, sf_stop };

/* IDは .rc の TC_PLUGIN_HEADER_ID と揃えます。 */
static const TC_PLUGIN_INFO sf_info = { sizeof(TC_PLUGIN_INFO), L"sample.formatter", L"Sample clock formatter" };

/* 本体が最初に呼ぶ公開関数です。この名前は sample_formatter.def と揃えます。 */
HRESULT WINAPI TClockGetInterface(UINT abi, LPCWSTR id, const void** out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (abi != TC_PLUGIN_ABI || !id) return E_NOINTERFACE;
    if (wcscmp(id, TC_PLUGIN_INFO_ID) == 0) *out = &sf_info;
    else if (wcscmp(id, TC_PLUGIN_RUNTIME_ID) == 0) *out = &sf_runtime;
    else return E_NOINTERFACE;
    return S_OK;
}
