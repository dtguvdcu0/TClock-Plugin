/*
 * 時報の判定を置き換える、時報プラグインのサンプルです。
 * 本体の設定で有効にすると、本体の時報判定の代わりにこのDLLの計画で鳴ります。
 *   毎正時: 時の数(1〜12)だけ主時報音を鳴らし、時計を点滅させます
 *   15分・30分・45分: 副時報音を1回鳴らします
 * 音の再生と点滅は本体が行い、音のファイルは本体の時報設定のものを使います。
 * DLLはTClock本体(exe)の中で動き、Explorerには読み込まれません。
 *
 * プラグイン画面で「時報の設定ページとして使う」を選ぶと、設定画面の「時報」に
 * 本体のページの代わりにこのDLLのページ(15分ごとの副時報と正時の点滅の切り替え)が
 * 表示されます。設定はこのプラグインのINIに保存され、本体の時報設定は変わりません。
 */
#include "../common/plugin_api.h"
#include "../common/chime_api.h"
#include "../common/page_api.h"
#include "sample_chime.h"
#include <wchar.h>

/* 結果の集計です。この例では数えるだけですが、次の計画に反映することもできます。 */
static UINT sc_outcomes[5];

/* このプラグインの設定と、その保存先(本体から渡されるINIのパス)です。 */
static WCHAR sc_ini[MAX_PATH];
static BOOL sc_quarters = TRUE, sc_blink = TRUE;

static void sc_load(void) {
    sc_quarters = GetPrivateProfileIntW(L"SampleChime", L"Quarters", 1, sc_ini) != 0;
    sc_blink = GetPrivateProfileIntW(L"SampleChime", L"HourBlink", 1, sc_ini) != 0;
}

static ULONGLONG sc_second(const SYSTEMTIME* time) {
    FILETIME file;
    ULARGE_INTEGER value;
    if (!SystemTimeToFileTime(time, &file)) return 0;
    value.LowPart = file.dwLowDateTime;
    value.HighPart = file.dwHighDateTime;
    return value.QuadPart / 10000000u;
}

static BOOL sc_time(ULONGLONG second, SYSTEMTIME* time) {
    ULARGE_INTEGER value;
    FILETIME file;
    value.QuadPart = second * 10000000u;
    file.dwLowDateTime = value.LowPart;
    file.dwHighDateTime = value.HighPart;
    return FileTimeToSystemTime(&file, time);
}

/*
 * 本体から指定された時間帯 [from, from + window) の時報を返します。
 * IDには時刻(秒)を使うので、同じ時報は何度計画しても同じIDになり、
 * 本体は一度始まった時報を二度鳴らしません。
 */
static HRESULT WINAPI sc_plan(void* user, const TC_CHIME_REQUEST* request, TC_CHIME_PLAN* output) {
    ULONGLONG from = sc_second(&request->from), second;
    (void)user;
    if (!from) return E_INVALIDARG;
    /* 次の15分ちょうどから、時間帯の終わりまで15分ごとに並べます。 */
    for (second = (from + 899) / 900 * 900; second < from + request->windowSeconds && output->count < TC_CHIME_MAX_EVENTS; second += 900) {
        TC_CHIME_EVENT* event = &output->events[output->count];
        if (!sc_time(second, &event->time)) return E_FAIL;
        event->id = second;
        if (event->time.wMinute == 0) {
            event->effects = TC_CHIME_PRIMARY | (sc_blink ? TC_CHIME_BLINK : 0);
            event->strikes = (event->time.wHour + 11) % 12 + 1;
        } else if (sc_quarters) {
            event->effects = TC_CHIME_SECONDARY;
            event->strikes = 0;
        } else continue;
        ++output->count;
    }
    return S_OK;
}

/* 受け付けられた時報ごとに、結果が一度だけ届きます。 */
static void WINAPI sc_report(void* user, ULONGLONG id, UINT outcome) {
    (void)user;
    (void)id;
    if (outcome < _countof(sc_outcomes)) ++sc_outcomes[outcome];
}

static const TC_CHIME_PROVIDER sc_provider = { sizeof(TC_CHIME_PROVIDER), sc_plan, sc_report };

/* 設定ページの状態です。ページが閉じられる(WM_NCDESTROY)まで保持します。 */
typedef struct SC_PAGE_STATE { void (WINAPI *changed)(HWND page); BOOL english; } SC_PAGE_STATE;

static INT_PTR CALLBACK sc_page_proc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    SC_PAGE_STATE* state = (SC_PAGE_STATE*)GetWindowLongPtrW(page, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        state = (SC_PAGE_STATE*)lParam;
        SetWindowLongPtrW(page, DWLP_USER, (LONG_PTR)state);
        SetDlgItemTextW(page, SC_QUARTERS, state->english ? L"Secondary chime at :15, :30 and :45" : L"15分・30分・45分に副時報音を鳴らす");
        SetDlgItemTextW(page, SC_BLINK, state->english ? L"Blink the clock on the hour" : L"正時に時計を点滅させる");
        SetDlgItemTextW(page, SC_NOTE, state->english ?
            L"SampleChime plans the chimes. Sounds come from the built-in Chime settings, which are kept unchanged and return when this page is no longer selected on the Plugins page." :
            L"時報はSampleChimeが計画します。音は本体の時報設定のものを使います。本体の時報設定はそのまま保持され、プラグイン画面でこのページの選択を外すと戻ります。");
        CheckDlgButton(page, SC_QUARTERS, sc_quarters ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(page, SC_BLINK, sc_blink ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        /* 変更したら本体に伝え、「適用」を押せるようにします。 */
        if (state && HIWORD(wParam) == BN_CLICKED && state->changed) state->changed(page);
        return TRUE;
    case WM_NCDESTROY:
        if (state) HeapFree(GetProcessHeap(), 0, state);
        SetWindowLongPtrW(page, DWLP_USER, 0);
        return FALSE;
    }
    return FALSE;
}

/* 本体の設定画面の中に置く子ウィンドウとしてページを作ります。 */
static HWND WINAPI sc_page_create(void* user, const TC_EXTENSION_CONTEXT* context) {
    SC_PAGE_STATE* state;
    HMODULE module = NULL;
    HWND page;
    (void)user;
    if (!context || context->size < sizeof(*context)) return NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&sc_outcomes, &module)) return NULL;
    state = (SC_PAGE_STATE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) return NULL;
    state->changed = context->changed;
    state->english = PRIMARYLANGID(context->language) != LANG_JAPANESE;
    page = CreateDialogParamW(module, MAKEINTRESOURCEW(SC_PAGE), context->owner, sc_page_proc, (LPARAM)state);
    if (!page) HeapFree(GetProcessHeap(), 0, state);
    return page;
}

/* 「適用」で設定を保存します。保存できなければ失敗を返し、ページの入力はそのまま残します。 */
static HRESULT WINAPI sc_page_apply(void* user, HWND page) {
    BOOL quarters = IsDlgButtonChecked(page, SC_QUARTERS) == BST_CHECKED;
    BOOL blink = IsDlgButtonChecked(page, SC_BLINK) == BST_CHECKED;
    (void)user;
    if (!WritePrivateProfileStringW(L"SampleChime", L"Quarters", quarters ? L"1" : L"0", sc_ini) ||
        !WritePrivateProfileStringW(L"SampleChime", L"HourBlink", blink ? L"1" : L"0", sc_ini))
        return HRESULT_FROM_WIN32(GetLastError());
    sc_quarters = quarters;
    sc_blink = blink;
    return S_OK;
}

static HRESULT WINAPI sc_start(const TC_PLUGIN_CONTEXT* context) {
    const TC_HOST_FEATURES* features;
    const TC_HOST_REGISTRY* registry;
    TC_FEATURE_REGISTRATION entry = { sizeof(entry) };
    TC_PAGE_REGISTRATION page = { sizeof(page) };
    static const TC_PAGE_SUBSTITUTE substitute = { sizeof(TC_PAGE_SUBSTITUTE), TC_PAGE_TARGET_CHIME, L"chime-settings" };
    TC_REGISTRATION token;
    HRESULT result;
    if (!context || context->size < sizeof(*context) || !context->query_host) return E_INVALIDARG;
    wcscpy_s(sc_ini, _countof(sc_ini), context->ini_path ? context->ini_path : L"");
    sc_load();
    result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_FEATURES_ID, (const void**)&features);
    if (FAILED(result)) return result;
    result = context->query_host(context->plugin, TC_PLUGIN_ABI, TC_HOST_REGISTRY_ID, (const void**)&registry);
    if (FAILED(result)) return result;
    entry.id = L"chime";
    entry.interface_id = TC_PLUGIN_CHIME_ID;
    entry.priority = 0;
    entry.table = &sc_provider;
    result = features->add(context->plugin, &entry, &token);
    if (FAILED(result)) return result;
    /* 時報ページを登録し、本体の「時報」の代わりに使えることを伝えます。 */
    page.id = L"chime-settings";
    page.japanese = L"時報 (SampleChime)";
    page.english = L"Chime (SampleChime)";
    page.create = sc_page_create;
    page.apply = sc_page_apply;
    result = registry->page(context->plugin, &page, &token);
    if (FAILED(result)) return result;
    entry.id = L"chime-page";
    entry.interface_id = TC_PLUGIN_PAGE_ID;
    entry.table = &substitute;
    return features->add(context->plugin, &entry, &token);
}

/* 登録は本体が停止時に取り消します。このサンプルに解放する資源はありません。 */
static void WINAPI sc_stop(void) {
}

static const TC_PLUGIN_RUNTIME sc_runtime = { sizeof(TC_PLUGIN_RUNTIME), sc_start, NULL, sc_stop };

/* IDは .rc の TC_PLUGIN_HEADER_ID と揃えます。 */
static const TC_PLUGIN_INFO sc_info = { sizeof(TC_PLUGIN_INFO), L"sample.chime", L"Sample quarter-hour chime" };

/* 本体が最初に呼ぶ公開関数です。この名前は sample_chime.def と揃えます。 */
HRESULT WINAPI TClockGetInterface(UINT abi, LPCWSTR id, const void** out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (abi != TC_PLUGIN_ABI || !id) return E_NOINTERFACE;
    if (wcscmp(id, TC_PLUGIN_INFO_ID) == 0) *out = &sc_info;
    else if (wcscmp(id, TC_PLUGIN_RUNTIME_ID) == 0) *out = &sc_runtime;
    else return E_NOINTERFACE;
    return S_OK;
}
