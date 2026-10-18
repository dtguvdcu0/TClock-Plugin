#include "tcalendar_host.h"
#include "bridge_contract.h"

#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ActivScp.h>

namespace tcalendar {

namespace {

enum class HolidayRuleKind {
    Limited,
    Fixed,
    MondayFixed,
    SpringEquinox,
    AutumnEquinox,
};

struct HolidayRule {
    int start_year;
    int end_year; // 0 means open-ended
    const wchar_t* spec;
    const wchar_t* name;
    HolidayRuleKind kind;
};

struct HolidayItem {
    std::wstring date;
    std::wstring name;
    std::wstring kind;
};

struct HolidaySubscriptionSourceStatus {
    std::wstring path;
    std::wstring resolved_path;
    bool exists = false;
    bool loaded = false;
    int record_count = 0;
    int applied_count = 0;
    std::wstring error;
};

struct HolidayProviderCatalogItem {
    std::wstring id;
    std::wstring name;
    std::wstring source_type;
    bool ready = false;
};

struct HolidayManifestRule {
    std::wstring section;
    std::wstring name;
    std::wstring kind;
    std::wstring scope;
    int start_year = 0;
    int end_year = 0;
    std::wstring date_spec;
    int month = 0;
    int nth = 0;
    std::wstring weekday;
};

struct HolidayManifestPostRule {
    std::wstring section;
    std::wstring kind;
    int start_year = 0;
};

struct HolidayProviderManifest {
    std::wstring provider_id;
    std::wstring display_name;
    std::wstring description;
    std::wstring source;
    int range_start = 0;
    int range_end = 0;
    std::vector<HolidayManifestRule> rules;
    std::vector<HolidayManifestPostRule> postrules;
};

constexpr const wchar_t* kJpPublicHolidayProviderId = L"jp-public-holiday";

// Legacy builtin reference rules kept only for future verification against provider output.
// Do not route production holiday loading through this table.
constexpr HolidayRule kJpPublicHolidayRules[] = {
    {1959, 1959, L"1959/4/10", L"明仁親王の結婚の儀", HolidayRuleKind::Limited},
    {1989, 1989, L"1989/2/24", L"大喪の礼", HolidayRuleKind::Limited},
    {1990, 1990, L"1990/11/12", L"即位礼正殿の儀", HolidayRuleKind::Limited},
    {1993, 1993, L"1993/6/9", L"徳仁親王の結婚の儀", HolidayRuleKind::Limited},
    {2019, 2019, L"2019/5/1", L"即位の日", HolidayRuleKind::Limited},
    {2019, 2019, L"2019/10/22", L"即位礼正殿の儀", HolidayRuleKind::Limited},
    {2020, 2020, L"2020/7/23", L"海の日", HolidayRuleKind::Limited},
    {2020, 2020, L"2020/7/24", L"スポーツの日", HolidayRuleKind::Limited},
    {2020, 2020, L"2020/8/10", L"山の日", HolidayRuleKind::Limited},
    {2021, 2021, L"2021/7/22", L"海の日", HolidayRuleKind::Limited},
    {2021, 2021, L"2021/7/23", L"スポーツの日", HolidayRuleKind::Limited},
    {2021, 2021, L"2021/8/8", L"山の日", HolidayRuleKind::Limited},
    {2000, 0, L"1月第2", L"成人の日", HolidayRuleKind::MondayFixed},
    {2003, 0, L"7月第3", L"海の日", HolidayRuleKind::MondayFixed},
    {2003, 0, L"9月第3", L"敬老の日", HolidayRuleKind::MondayFixed},
    {2020, 0, L"10月第2", L"スポーツの日", HolidayRuleKind::MondayFixed},
    {1949, 1999, L"1/15", L"成人の日", HolidayRuleKind::Fixed},
    {1949, 1988, L"4/29", L"天皇誕生日", HolidayRuleKind::Fixed},
    {1989, 2006, L"4/29", L"みどりの日", HolidayRuleKind::Fixed},
    {1996, 2002, L"7/20", L"海の日", HolidayRuleKind::Fixed},
    {1966, 2002, L"9/15", L"敬老の日", HolidayRuleKind::Fixed},
    {1966, 1999, L"10/10", L"体育の日", HolidayRuleKind::Fixed},
    {1989, 2018, L"12/23", L"天皇誕生日", HolidayRuleKind::Fixed},
    {1949, 0, L"1/1", L"元日", HolidayRuleKind::Fixed},
    {1967, 0, L"2/11", L"建国記念の日", HolidayRuleKind::Fixed},
    {2020, 0, L"2/23", L"天皇誕生日", HolidayRuleKind::Fixed},
    {1949, 0, L"3/20(or21)", L"春分の日", HolidayRuleKind::SpringEquinox},
    {2007, 0, L"4/29", L"昭和の日", HolidayRuleKind::Fixed},
    {1948, 0, L"5/3", L"憲法記念日", HolidayRuleKind::Fixed},
    {2007, 0, L"5/4", L"みどりの日", HolidayRuleKind::Fixed},
    {1948, 0, L"5/5", L"こどもの日", HolidayRuleKind::Fixed},
    {2016, 0, L"8/11", L"山の日", HolidayRuleKind::Fixed},
    {1948, 0, L"9/23(or24)", L"秋分の日", HolidayRuleKind::AutumnEquinox},
    {1948, 0, L"11/3", L"文化の日", HolidayRuleKind::Fixed},
    {1948, 0, L"11/23", L"勤労感謝の日", HolidayRuleKind::Fixed},
};

using CreateCoreWebView2EnvironmentWithOptionsFn = HRESULT (STDAPICALLTYPE*)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    void* environmentOptions,
    void* environment_created_handler);

bool ParseDateKey(const std::wstring& value, std::tm& out_tm);

std::wstring EscapeJsonString(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
            case 0x5C:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(static_cast<wchar_t>(0x5C));
                break;
            case 0x22:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(static_cast<wchar_t>(0x22));
                break;
            case 0x08:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(L'b');
                break;
            case 0x0C:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(L'f');
                break;
            case 0x0A:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(L'n');
                break;
            case 0x0D:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(L'r');
                break;
            case 0x09:
                out.push_back(static_cast<wchar_t>(0x5C));
                out.push_back(L't');
                break;
            default:
                if (ch >= 0 && ch < 0x20) {
                    wchar_t buf[7] = {0};
                    swprintf(buf, 7, L"%lc%04X", static_cast<wchar_t>(0x5C), static_cast<unsigned int>(ch));
                    out += buf;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

bool ParseTimeToMinutes(const std::wstring& value, int& out_minutes) {
    if (value.empty()) return false;
    if (value.size() != 5 || value[2] != L':') return false;
    if (value[0] < L'0' || value[0] > L'9' || value[1] < L'0' || value[1] > L'9' ||
        value[3] < L'0' || value[3] > L'9' || value[4] < L'0' || value[4] > L'9') {
        return false;
    }
    const int hh = (value[0] - L'0') * 10 + (value[1] - L'0');
    const int mm = (value[3] - L'0') * 10 + (value[4] - L'0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    out_minutes = hh * 60 + mm;
    return true;
}

bool ValidateTaskTimes(const std::wstring& start_time, const std::wstring& end_time, std::wstring& out_message) {
    out_message.clear();
    if (start_time.empty() && end_time.empty()) return true;
    if (!start_time.empty() && end_time.empty()) {
        int dummy = 0;
        if (!ParseTimeToMinutes(start_time, dummy)) {
            out_message = L"Invalid startTime format";
            return false;
        }
        return true;
    }
    if (start_time.empty() && !end_time.empty()) {
        out_message = L"endTime requires startTime";
        return false;
    }

    int start_minutes = 0;
    int end_minutes = 0;
    if (!ParseTimeToMinutes(start_time, start_minutes)) {
        out_message = L"Invalid startTime format";
        return false;
    }
    if (!ParseTimeToMinutes(end_time, end_minutes)) {
        out_message = L"Invalid endTime format";
        return false;
    }
    if (end_minutes < start_minutes) {
        out_message = L"endTime must be after startTime";
        return false;
    }
    return true;
}

std::wstring BuildViewConfigJson(const HostConfig& config) {
    return std::wstring(L"{\"defaultViewMode\":\"") + EscapeJsonString(config.default_view_mode) +
        L"\",\"defaultRangePresetDays\":" + std::to_wstring(config.default_range_preset_days) +
        L",\"defaultCustomRangeDays\":" + std::to_wstring(config.default_custom_range_days) +
        L",\"defaultUseCustomRange\":" + (config.default_use_custom_range ? L"true" : L"false") +
        L",\"uiFontFamily\":\"" + EscapeJsonString(config.ui_font_family) +
        L"\",\"uiBaseFontSize\":" + std::to_wstring(config.ui_base_font_size) +
        L",\"uiCalendarDateFontSize\":" + std::to_wstring(config.ui_calendar_date_font_size) +
        L",\"uiTaskFontSize\":" + std::to_wstring(config.ui_task_font_size) +
        L",\"uiPanelRightWidth\":" + std::to_wstring(config.ui_panel_right_width) +
        L",\"uiCalendarHeight\":" + std::to_wstring(config.ui_calendar_height) +
        L",\"uiShowTaskPanel\":" + (config.ui_show_task_panel ? L"true" : L"false") +
        L",\"tclockAlertEnabled\":" + (config.tclock_alert_enabled ? L"true" : L"false") +
        L",\"alertSoundEnabled\":" + (config.alert_sound_enabled ? L"true" : L"false") +
        L",\"alertSoundPath\":\"" + EscapeJsonString(config.alert_sound_path) +
        L"\",\"holidaySubscriptionFiles\":\"" + EscapeJsonString(config.holiday_subscription_files) +
        L"\",\"holidaySubscriptionCatalog\":\"" + EscapeJsonString(config.holiday_subscription_catalog) + L"\"}";
}

std::wstring BuildTaskArrayJson(const std::vector<TaskItem>& tasks) {
    std::wstring data = L"[";
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i) data += L",";
        data += std::wstring(L"{\"id\":\"") + EscapeJsonString(tasks[i].id) + L"\",\"date\":\"" + EscapeJsonString(tasks[i].date) +
                L"\",\"title\":\"" + EscapeJsonString(tasks[i].title) +
                L"\",\"detail\":\"" + EscapeJsonString(tasks[i].detail) +
                L"\",\"startTime\":\"" + EscapeJsonString(tasks[i].start_time) +
                L"\",\"endTime\":\"" + EscapeJsonString(tasks[i].end_time) +
                L"\",\"done\":" + (tasks[i].done ? L"true" : L"false") +
                L",\"alertEnabled\":" + (tasks[i].alert_enabled ? L"true" : L"false") + L"}";
    }
    data += L"]";
    return data;
}

std::wstring BuildHolidayArrayJson(const std::vector<HolidayItem>& items) {
    std::wstring data = L"[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) data += L",";
        data += std::wstring(L"{\"date\":\"") + EscapeJsonString(items[i].date) +
            L"\",\"name\":\"" + EscapeJsonString(items[i].name) +
            L"\",\"kind\":\"" + EscapeJsonString(items[i].kind) + L"\"}";
    }
    data += L"]";
    return data;
}

std::wstring BuildHolidayItemJson(const HolidayItem& item) {
    return std::wstring(L"{\"date\":\"") + EscapeJsonString(item.date) +
        L"\",\"name\":\"" + EscapeJsonString(item.name) +
        L"\",\"kind\":\"" + EscapeJsonString(item.kind) + L"\"}";
}

std::wstring BuildHolidaySubscriptionStatusArrayJson(const std::vector<HolidaySubscriptionSourceStatus>& items) {
    std::wstring data = L"[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) data += L",";
        data += std::wstring(L"{\"path\":\"") + EscapeJsonString(items[i].path) +
            L"\",\"resolvedPath\":\"" + EscapeJsonString(items[i].resolved_path) +
            L"\",\"exists\":" + (items[i].exists ? L"true" : L"false") +
            L",\"loaded\":" + (items[i].loaded ? L"true" : L"false") +
            L",\"recordCount\":" + std::to_wstring(items[i].record_count) +
            L",\"appliedCount\":" + std::to_wstring(items[i].applied_count) +
            L",\"error\":\"" + EscapeJsonString(items[i].error) + L"\"}";
    }
    data += L"]";
    return data;
}

std::wstring BuildHolidayProviderCatalogJson(const std::vector<HolidayProviderCatalogItem>& items) {
    std::wstring data = L"[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) data += L",";
        data += std::wstring(L"{\"id\":\"") + EscapeJsonString(items[i].id) +
            L"\",\"name\":\"" + EscapeJsonString(items[i].name) +
            L"\",\"sourceType\":\"" + EscapeJsonString(items[i].source_type) +
            L"\",\"ready\":" + (items[i].ready ? L"true" : L"false") + L"}";
    }
    data += L"]";
    return data;
}

bool DecodeUtf8Text(const std::string& text, std::wstring& out_value) {
    out_value.clear();
    if (text.empty()) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return false;
    out_value.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                               out_value.data(), count) > 0;
}

bool ReadUtf8TextFile(const std::filesystem::path& path, std::wstring& out_value) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return DecodeUtf8Text(bytes, out_value);
}

bool DecodeIniByteText(const std::string& text, std::wstring& out_value) {
    out_value.clear();
    out_value.reserve(text.size());
    for (unsigned char ch : text) {
        out_value.push_back(static_cast<wchar_t>(ch));
    }
    return true;
}

bool ReadIniTextFile(const std::filesystem::path& path, std::wstring& out_value) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t wchar_count = (bytes.size() - 2) / sizeof(wchar_t);
        out_value.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), wchar_count);
        return true;
    }
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
        return DecodeUtf8Text(bytes, out_value);
    }
    if (DecodeUtf8Text(bytes, out_value)) {
        return true;
    }
    return DecodeIniByteText(bytes, out_value);
}

bool WriteIniTextFileUtf16(const std::filesystem::path& path, const std::wstring& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    static constexpr unsigned char kUtf16LeBom[] = {0xFF, 0xFE};
    output.write(reinterpret_cast<const char*>(kUtf16LeBom), sizeof(kUtf16LeBom));
    if (!text.empty()) {
        output.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    }
    return !!output;
}

std::vector<std::wstring> SplitString(const std::wstring& value, wchar_t delimiter) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t pos = value.find(delimiter, start);
        const size_t end = (pos == std::wstring::npos) ? value.size() : pos;
        out.push_back(value.substr(start, end - start));
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    return out;
}

std::wstring TrimWhitespace(const std::wstring& value);

bool ParseYearList(const std::wstring& value, std::vector<int>& out_years, std::wstring& out_error) {
    out_years.clear();
    out_error.clear();
    for (const std::wstring& raw : SplitString(value, L',')) {
        const std::wstring token = TrimWhitespace(raw);
        if (token.empty()) continue;
        for (wchar_t ch : token) {
            if (!iswdigit(ch)) {
                out_error = L"Invalid year list";
                return false;
            }
        }
        const int year = _wtoi(token.c_str());
        if (year < 1851 || year > 2150) {
            out_error = L"Year out of supported range";
            return false;
        }
        out_years.push_back(year);
    }
    if (out_years.empty()) {
        out_error = L"Missing yearsCsv";
        return false;
    }
    return true;
}

std::wstring TrimWhitespace(const std::wstring& value) {
    size_t start = 0;
    while (start < value.size() && iswspace(value[start])) ++start;
    size_t end = value.size();
    while (end > start && iswspace(value[end - 1])) --end;
    return value.substr(start, end - start);
}

bool ParseIntValue(const std::wstring& value, int& out_value) {
    const std::wstring trimmed = TrimWhitespace(value);
    if (trimmed.empty()) return false;
    wchar_t* end_ptr = nullptr;
    const long parsed = wcstol(trimmed.c_str(), &end_ptr, 10);
    if (end_ptr == trimmed.c_str() || (end_ptr && *end_ptr != L'\0')) return false;
    out_value = static_cast<int>(parsed);
    return true;
}

std::wstring ToLowerInvariant(const std::wstring& value) {
    std::wstring out = value;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

std::wstring EscapeScriptString(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
            case L'\\':
                out += L"\\\\";
                break;
            case L'\'':
                out += L"\\'";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case 0x2028:
                out += L"\\u2028";
                break;
            case 0x2029:
                out += L"\\u2029";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::wstring UnescapeProviderField(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    bool escaped = false;
    for (wchar_t ch : value) {
        if (!escaped) {
            if (ch == L'\\') {
                escaped = true;
            } else {
                out.push_back(ch);
            }
            continue;
        }
        switch (ch) {
            case L'\\':
                out.push_back(L'\\');
                break;
            case L't':
                out.push_back(L'\t');
                break;
            case L'r':
                out.push_back(L'\r');
                break;
            case L'n':
                out.push_back(L'\n');
                break;
            default:
                out.push_back(ch);
                break;
        }
        escaped = false;
    }
    if (escaped) out.push_back(L'\\');
    return out;
}

bool ParseDelimitedHolidayRows(const std::wstring& text, std::vector<HolidayItem>& out_items, std::wstring& out_error) {
    out_items.clear();
    out_error.clear();
    for (const std::wstring& raw_line : SplitString(text, L'\n')) {
        std::wstring line = raw_line;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty()) continue;
        const std::vector<std::wstring> parts = SplitString(line, L'\t');
        if (parts.size() != 3) {
            out_error = L"Invalid script row format";
            return false;
        }
        HolidayItem item{};
        item.date = UnescapeProviderField(parts[0]);
        item.name = UnescapeProviderField(parts[1]);
        item.kind = UnescapeProviderField(parts[2]);
        std::tm tm_value{};
        if (!ParseDateKey(item.date, tm_value) || item.name.empty() || item.kind.empty()) {
            out_error = L"Invalid script row data";
            return false;
        }
        out_items.push_back(item);
    }
    return true;
}

std::wstring BuildHolidayManifestJson(const HolidayProviderManifest& manifest) {
    std::wstring data = std::wstring(L"{\"providerId\":\"") + EscapeJsonString(manifest.provider_id) +
        L"\",\"displayName\":\"" + EscapeJsonString(manifest.display_name) +
        L"\",\"description\":\"" + EscapeJsonString(manifest.description) +
        L"\",\"source\":\"" + EscapeJsonString(manifest.source) +
        L"\",\"rangeStart\":" + std::to_wstring(manifest.range_start) +
        L",\"rangeEnd\":" + std::to_wstring(manifest.range_end) +
        L",\"rules\":[";
    for (size_t i = 0; i < manifest.rules.size(); ++i) {
        const HolidayManifestRule& rule = manifest.rules[i];
        if (i) data += L",";
        data += std::wstring(L"{\"section\":\"") + EscapeJsonString(rule.section) +
            L"\",\"name\":\"" + EscapeJsonString(rule.name) +
            L"\",\"kind\":\"" + EscapeJsonString(rule.kind) +
            L"\",\"scope\":\"" + EscapeJsonString(rule.scope) +
            L"\",\"startYear\":" + std::to_wstring(rule.start_year) +
            L",\"endYear\":" + std::to_wstring(rule.end_year) +
            L",\"dateSpec\":\"" + EscapeJsonString(rule.date_spec) +
            L"\",\"month\":" + std::to_wstring(rule.month) +
            L",\"nth\":" + std::to_wstring(rule.nth) +
            L",\"weekday\":\"" + EscapeJsonString(rule.weekday) + L"\"}";
    }
    data += L"],\"postrules\":[";
    for (size_t i = 0; i < manifest.postrules.size(); ++i) {
        const HolidayManifestPostRule& rule = manifest.postrules[i];
        if (i) data += L",";
        data += std::wstring(L"{\"section\":\"") + EscapeJsonString(rule.section) +
            L"\",\"kind\":\"" + EscapeJsonString(rule.kind) +
            L"\",\"startYear\":" + std::to_wstring(rule.start_year) + L"}";
    }
    data += L"]}";
    return data;
}

class JScriptSite final : public IActiveScriptSite {
public:
    JScriptSite() = default;

    const std::wstring& last_error() const {
        return last_error_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == IID_IUnknown || riid == IID_IActiveScriptSite) {
            *ppvObject = static_cast<IActiveScriptSite*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef(void) override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release(void) override {
        const ULONG value = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (value == 0) {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE GetLCID(LCID* plcid) override {
        if (plcid == nullptr) return E_POINTER;
        *plcid = LOCALE_USER_DEFAULT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetItemInfo(LPCOLESTR, DWORD, IUnknown**, ITypeInfo**) override {
        return TYPE_E_ELEMENTNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDocVersionString(BSTR* pbstrVersion) override {
        if (pbstrVersion == nullptr) return E_POINTER;
        *pbstrVersion = SysAllocString(L"TCalendarHolidayProvider");
        return (*pbstrVersion != nullptr) ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE OnScriptTerminate(const VARIANT*, const EXCEPINFO*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStateChange(SCRIPTSTATE) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnScriptError(IActiveScriptError* error) override {
        if (error == nullptr) {
            last_error_ = L"Script execution failed";
            return S_OK;
        }
        EXCEPINFO info{};
        if (SUCCEEDED(error->GetExceptionInfo(&info))) {
            if (info.bstrDescription && info.bstrDescription[0] != 0) {
                last_error_ = info.bstrDescription;
            } else if (info.bstrSource && info.bstrSource[0] != 0) {
                last_error_ = info.bstrSource;
            } else {
                last_error_ = L"Script execution failed";
            }
            if (info.bstrSource) SysFreeString(info.bstrSource);
            if (info.bstrDescription) SysFreeString(info.bstrDescription);
            if (info.bstrHelpFile) SysFreeString(info.bstrHelpFile);
        } else {
            last_error_ = L"Script execution failed";
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEnterScript(void) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnLeaveScript(void) override {
        return S_OK;
    }

private:
    ~JScriptSite() = default;
    volatile LONG ref_count_ = 1;
    std::wstring last_error_;
};

bool ParseDateKey(const std::wstring& value, std::tm& out_tm) {
    if (value.size() != 10 || value[4] != L'-' || value[7] != L'-') return false;
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (value[i] < L'0' || value[i] > L'9') return false;
    }

    const int year = (value[0] - L'0') * 1000 + (value[1] - L'0') * 100 + (value[2] - L'0') * 10 + (value[3] - L'0');
    const int month = (value[5] - L'0') * 10 + (value[6] - L'0');
    const int day = (value[8] - L'0') * 10 + (value[9] - L'0');
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    out_tm = {};
    out_tm.tm_year = year - 1900;
    out_tm.tm_mon = month - 1;
    out_tm.tm_mday = day;
    out_tm.tm_hour = 12;
    out_tm.tm_isdst = -1;
    return true;
}

std::wstring FormatDateKey(const std::tm& value) {
    wchar_t buf[16] = {0};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04d-%02d-%02d",
                 value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
    return buf;
}

bool AddDaysToDateKey(const std::wstring& value, int days, std::wstring& out_value) {
    std::tm tm_value{};
    if (!ParseDateKey(value, tm_value)) return false;

    time_t t = mktime(&tm_value);
    if (t == static_cast<time_t>(-1)) return false;
    t += static_cast<time_t>(days) * 24 * 60 * 60;

    std::tm out_tm{};
    localtime_s(&out_tm, &t);
    out_value = FormatDateKey(out_tm);
    return true;
}

bool ParseMonthDaySpec(const std::wstring& value, int& out_month, int& out_day) {
    const size_t slash = value.find(L'/');
    if (slash == std::wstring::npos) return false;
    const std::wstring lhs = value.substr(0, slash);
    size_t rhs_end = slash + 1;
    while (rhs_end < value.size() && value[rhs_end] >= L'0' && value[rhs_end] <= L'9') {
        ++rhs_end;
    }
    if (lhs.empty() || rhs_end == slash + 1) return false;
    out_month = _wtoi(lhs.c_str());
    out_day = _wtoi(value.substr(slash + 1, rhs_end - slash - 1).c_str());
    return out_month >= 1 && out_month <= 12 && out_day >= 1 && out_day <= 31;
}

bool ParseNthMondaySpec(const std::wstring& value, int& out_month, int& out_nth) {
    const size_t month_pos = value.find(L"月第");
    if (month_pos == std::wstring::npos) return false;
    out_month = _wtoi(value.substr(0, month_pos).c_str());
    out_nth = _wtoi(value.substr(month_pos + 2).c_str());
    return out_month >= 1 && out_month <= 12 && out_nth >= 1 && out_nth <= 5;
}

std::wstring MakeDateKey(int year, int month, int day) {
    wchar_t buf[16] = {0};
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%04d-%02d-%02d", year, month, day);
    return buf;
}

int VbaInt(double value) {
    return static_cast<int>(std::floor(value));
}

int GetSpringEquinoxDay(int year) {
    if (year > 1850 && year < 1900) {
        return VbaInt(20.8431 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 1899 && year < 1980) {
        return VbaInt(20.8357 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 1979 && year < 2100) {
        return VbaInt(20.8431 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 2099 && year < 2151) {
        return VbaInt(21.851 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    return 20;
}

int GetAutumnEquinoxDay(int year) {
    if (year > 1850 && year < 1900) {
        return VbaInt(22.2588 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 1899 && year < 1980) {
        return VbaInt(23.2588 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 1979 && year < 2100) {
        return VbaInt(23.2488 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    if (year > 2099 && year < 2151) {
        return VbaInt(24.2488 + 0.242194 * (year - 1980)) - VbaInt((year - 1980) / 4.0);
    }
    return 23;
}

std::wstring GetNthMondayDateKey(int year, int month, int nth) {
    std::tm tm_value{};
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = 1;
    tm_value.tm_hour = 12;
    tm_value.tm_isdst = -1;
    const time_t t = mktime(&tm_value);
    if (t == static_cast<time_t>(-1)) return L"";
    std::tm local_tm{};
    localtime_s(&local_tm, &t);
    int first_weekday = local_tm.tm_wday + 1; // Sunday=1
    const int offset = (2 + 7 - first_weekday) % 7;
    return MakeDateKey(year, month, 1 + offset + 7 * (nth - 1));
}

std::map<std::wstring, std::wstring> BuildBaseHolidayMap(int year) {
    std::unordered_set<std::wstring> seen_names;
    std::map<std::wstring, std::wstring> by_date;
    for (const auto& rule : kJpPublicHolidayRules) {
        const int end_year = (rule.end_year == 0) ? year : rule.end_year;
        if (year < rule.start_year || year > end_year) continue;
        const std::wstring name = rule.name;
        if (seen_names.find(name) != seen_names.end()) continue;

        std::wstring date_key;
        if (rule.kind == HolidayRuleKind::Limited) {
            int month = 0;
            int day = 0;
            const std::wstring spec = rule.spec;
            const size_t slash1 = spec.find(L'/');
            const size_t slash2 = (slash1 == std::wstring::npos) ? std::wstring::npos : spec.find(L'/', slash1 + 1);
            if (slash1 == std::wstring::npos || slash2 == std::wstring::npos) continue;
            month = _wtoi(spec.substr(slash1 + 1, slash2 - slash1 - 1).c_str());
            day = _wtoi(spec.substr(slash2 + 1).c_str());
            if (month < 1 || month > 12 || day < 1 || day > 31) continue;
            date_key = MakeDateKey(year, month, day);
        } else if (rule.kind == HolidayRuleKind::Fixed) {
            int month = 0;
            int day = 0;
            if (!ParseMonthDaySpec(rule.spec, month, day)) continue;
            date_key = MakeDateKey(year, month, day);
        } else if (rule.kind == HolidayRuleKind::MondayFixed) {
            int month = 0;
            int nth = 0;
            if (!ParseNthMondaySpec(rule.spec, month, nth)) continue;
            date_key = GetNthMondayDateKey(year, month, nth);
        } else if (rule.kind == HolidayRuleKind::SpringEquinox) {
            date_key = MakeDateKey(year, 3, GetSpringEquinoxDay(year));
        } else if (rule.kind == HolidayRuleKind::AutumnEquinox) {
            date_key = MakeDateKey(year, 9, GetAutumnEquinoxDay(year));
        }

        if (!date_key.empty()) {
            by_date.emplace(date_key, name);
            seen_names.insert(name);
        }
    }
    return by_date;
}

bool HasBaseHoliday(const std::map<std::wstring, std::wstring>& base_holidays, const std::wstring& date_key) {
    return base_holidays.find(date_key) != base_holidays.end();
}

std::wstring GetBaseHolidayName(const std::map<std::wstring, std::wstring>& base_holidays, const std::wstring& date_key) {
    const auto it = base_holidays.find(date_key);
    return (it == base_holidays.end()) ? L"" : it->second;
}

bool IsSundayDateKey(const std::wstring& date_key) {
    std::tm tm_value{};
    if (!ParseDateKey(date_key, tm_value)) return false;
    const time_t t = mktime(&tm_value);
    if (t == static_cast<time_t>(-1)) return false;
    std::tm out_tm{};
    localtime_s(&out_tm, &t);
    return out_tm.tm_wday == 0;
}

bool ParseHolidayManifestIni(const std::wstring& text, HolidayProviderManifest& out_manifest, std::wstring& out_error) {
    out_manifest = {};
    out_error.clear();
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> sections;
    std::vector<std::wstring> section_order;
    std::wstring current_section;
    for (const std::wstring& raw_line : SplitString(text, L'\n')) {
        std::wstring line = raw_line;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        line = TrimWhitespace(line);
        if (line.empty()) continue;
        if (line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            current_section = TrimWhitespace(line.substr(1, line.size() - 2));
            if (!current_section.empty() && sections.find(current_section) == sections.end()) {
                section_order.push_back(current_section);
            }
            continue;
        }
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || current_section.empty()) continue;
        const std::wstring key = TrimWhitespace(line.substr(0, eq));
        const std::wstring value = TrimWhitespace(line.substr(eq + 1));
        sections[current_section][key] = value;
    }

    const auto provider_it = sections.find(L"provider");
    if (provider_it == sections.end()) {
        out_error = L"Manifest missing [provider] section";
        return false;
    }
    const auto& provider = provider_it->second;
    auto get_provider_value = [&](const wchar_t* key) -> std::wstring {
        const auto it = provider.find(key);
        return (it == provider.end()) ? L"" : it->second;
    };
    out_manifest.provider_id = get_provider_value(L"id");
    out_manifest.display_name = get_provider_value(L"name");
    out_manifest.description = get_provider_value(L"description");
    out_manifest.source = get_provider_value(L"source");
    if (out_manifest.provider_id.empty()) {
        out_error = L"Manifest missing provider id";
        return false;
    }
    if (!ParseIntValue(get_provider_value(L"range_start"), out_manifest.range_start)) out_manifest.range_start = 0;
    if (!ParseIntValue(get_provider_value(L"range_end"), out_manifest.range_end)) out_manifest.range_end = 0;

    for (const std::wstring& section_name : section_order) {
        const auto section_it = sections.find(section_name);
        if (section_it == sections.end()) continue;
        const auto& values = section_it->second;
        if (section_name.rfind(L"rule_", 0) == 0) {
            HolidayManifestRule rule{};
            rule.section = section_name;
            auto get_value = [&](const wchar_t* key) -> std::wstring {
                const auto it = values.find(key);
                return (it == values.end()) ? L"" : it->second;
            };
            rule.name = get_value(L"name");
            rule.kind = get_value(L"kind");
            rule.scope = ToLowerInvariant(get_value(L"scope"));
            if (rule.name.empty() || rule.kind.empty()) {
                out_error = L"Manifest rule missing name or kind";
                return false;
            }
            if (!ParseIntValue(get_value(L"start"), rule.start_year)) rule.start_year = 0;
            if (!ParseIntValue(get_value(L"end"), rule.end_year)) rule.end_year = 0;
            rule.date_spec = get_value(L"date");
            (void)ParseIntValue(get_value(L"month"), rule.month);
            (void)ParseIntValue(get_value(L"nth"), rule.nth);
            rule.weekday = ToLowerInvariant(get_value(L"weekday"));
            out_manifest.rules.push_back(rule);
        } else if (section_name.rfind(L"postrule_", 0) == 0) {
            HolidayManifestPostRule rule{};
            rule.section = section_name;
            const auto kind_it = values.find(L"kind");
            if (kind_it == values.end() || kind_it->second.empty()) {
                out_error = L"Manifest postrule missing kind";
                return false;
            }
            rule.kind = kind_it->second;
            const auto start_it = values.find(L"start");
            if (!ParseIntValue(start_it == values.end() ? L"" : start_it->second, rule.start_year)) {
                rule.start_year = 0;
            }
            out_manifest.postrules.push_back(rule);
        }
    }
    return true;
}

bool LoadHolidayProviderManifest(const std::filesystem::path& manifest_path,
                                 HolidayProviderManifest& out_manifest,
                                 std::wstring& out_error) {
    std::wstring text;
    if (!ReadUtf8TextFile(manifest_path, text)) {
        out_error = L"Failed to read UTF-8 manifest";
        return false;
    }
    return ParseHolidayManifestIni(text, out_manifest, out_error);
}

std::vector<HolidayProviderCatalogItem> EnumerateHolidayProviderCatalog(const std::wstring& ini_file_path) {
    std::vector<HolidayProviderCatalogItem> out;
    std::filesystem::path base_dir = std::filesystem::current_path();
    if (!ini_file_path.empty()) {
        base_dir = std::filesystem::path(ini_file_path).parent_path();
    }
    const std::filesystem::path providers_dir = (base_dir / L"tcalendar" / L"providers").lexically_normal();
    std::error_code ec;
    if (!std::filesystem::exists(providers_dir, ec) || !std::filesystem::is_directory(providers_dir, ec)) {
        return out;
    }

    std::map<std::wstring, HolidayProviderCatalogItem> by_id;
    for (const auto& entry : std::filesystem::directory_iterator(providers_dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::filesystem::path path = entry.path();
        const std::wstring extension = ToLowerInvariant(path.extension().wstring());
        if (extension == L".ini") {
            HolidayProviderManifest manifest{};
            std::wstring error;
            const bool manifest_ok = LoadHolidayProviderManifest(path, manifest, error);
            HolidayProviderCatalogItem item{};
            item.id = path.stem().wstring();
            item.name = manifest_ok && !manifest.display_name.empty() ? manifest.display_name : item.id;
            item.source_type = L"ini+js";
            std::filesystem::path script_path = path.parent_path() / path.stem();
            script_path.replace_extension(L".js");
            item.ready = manifest_ok && std::filesystem::exists(script_path);
            by_id[item.id] = item;
            continue;
        }
        if (extension == L".txt") {
            const std::wstring id = path.stem().wstring();
            if (by_id.find(id) != by_id.end()) {
                continue;
            }
            HolidayProviderCatalogItem item{};
            item.id = std::wstring(L"tcalendar/providers/") + path.filename().wstring();
            item.name = id;
            item.source_type = L"txt";
            item.ready = true;
            by_id[item.id] = item;
        }
    }

    for (const auto& entry : by_id) {
        out.push_back(entry.second);
    }
    return out;
}

std::filesystem::path ResolveSubscriptionPath(const std::filesystem::path& base_dir, const std::wstring& raw_path) {
    const std::wstring trimmed = TrimWhitespace(raw_path);
    if (trimmed.empty()) return {};
    std::filesystem::path path(trimmed);
    if (path.is_relative()) {
        path = base_dir / path;
    }
    return path.lexically_normal();
}

std::filesystem::path ResolveProviderAliasPath(const std::filesystem::path& base_dir, const std::wstring& alias_id) {
    if (_wcsicmp(alias_id.c_str(), kJpPublicHolidayProviderId) == 0) {
        const std::filesystem::path ini_path =
            (base_dir / L"tcalendar" / L"providers" / L"jp-public-holiday.ini").lexically_normal();
        if (std::filesystem::exists(ini_path)) {
            return ini_path;
        }
        return {};
    }
    return {};
}

std::wstring GetHolidayProviderWrapperScript() {
    return
        L"function __tcalendar_parseManifest(jsonText){return eval('(' + jsonText + ')');}\n"
        L"function __tcalendar_escapeField(value){\n"
        L"  var s = value == null ? '' : String(value);\n"
        L"  s = s.replace(/\\\\/g,'\\\\\\\\');\n"
        L"  s = s.replace(/\\t/g,'\\\\t');\n"
        L"  s = s.replace(/\\r/g,'\\\\r');\n"
        L"  s = s.replace(/\\n/g,'\\\\n');\n"
        L"  return s;\n"
        L"}\n"
        L"function __tcalendar_eval_holidays(year, manifestJson){\n"
        L"  var manifest = __tcalendar_parseManifest(manifestJson);\n"
        L"  var rows = getHolidays(year, manifest);\n"
        L"  if (rows == null || typeof rows.length === 'undefined') throw new Error('getHolidays must return an array');\n"
        L"  var out = [];\n"
        L"  for (var i = 0; i < rows.length; ++i){\n"
        L"    var row = rows[i] || {};\n"
        L"    out.push(__tcalendar_escapeField(row.date) + '\\t' + __tcalendar_escapeField(row.name) + '\\t' + __tcalendar_escapeField(row.kind));\n"
        L"  }\n"
        L"  return out.join('\\n');\n"
        L"}\n";
}

bool EvaluateJScriptHolidayProvider(const std::filesystem::path& script_path,
                                    const HolidayProviderManifest& manifest,
                                    int year,
                                    std::vector<HolidayItem>& out_items,
                                    std::wstring& out_error) {
    out_items.clear();
    out_error.clear();

    std::wstring script_text;
    if (!ReadUtf8TextFile(script_path, script_text)) {
        out_error = L"Failed to read UTF-8 script";
        return false;
    }

    CLSID clsid{};
    HRESULT hr = CLSIDFromProgID(L"JScript", &clsid);
    if (FAILED(hr)) {
        out_error = L"JScript engine not available";
        return false;
    }

    IActiveScript* script = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IActiveScript, reinterpret_cast<void**>(&script));
    if (FAILED(hr) || script == nullptr) {
        out_error = L"Failed to create JScript engine";
        return false;
    }

    IActiveScriptParse* parser = nullptr;
    JScriptSite* site = new JScriptSite();
    if (site == nullptr) {
        script->Release();
        out_error = L"Out of memory";
        return false;
    }

    bool ok = false;
    do {
        hr = script->SetScriptSite(site);
        if (FAILED(hr)) {
            out_error = L"Failed to attach script site";
            break;
        }
        hr = script->QueryInterface(IID_IActiveScriptParse, reinterpret_cast<void**>(&parser));
        if (FAILED(hr) || parser == nullptr) {
            out_error = L"Failed to acquire script parser";
            break;
        }
        hr = parser->InitNew();
        if (FAILED(hr)) {
            out_error = L"Failed to initialize script parser";
            break;
        }
        hr = parser->ParseScriptText(GetHolidayProviderWrapperScript().c_str(), nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr);
        if (FAILED(hr)) {
            out_error = site->last_error().empty() ? L"Failed to load provider wrapper script" : site->last_error();
            break;
        }
        hr = parser->ParseScriptText(script_text.c_str(), nullptr, nullptr, script_path.filename().c_str(), 0, 0, 0, nullptr, nullptr);
        if (FAILED(hr)) {
            out_error = site->last_error().empty() ? L"Failed to load provider script" : site->last_error();
            break;
        }
        hr = script->SetScriptState(SCRIPTSTATE_STARTED);
        if (FAILED(hr)) {
            out_error = L"Failed to start script engine";
            break;
        }

        const std::wstring manifest_json = BuildHolidayManifestJson(manifest);
        const std::wstring expression =
            std::wstring(L"__tcalendar_eval_holidays(") + std::to_wstring(year) + L",'" + EscapeScriptString(manifest_json) + L"')";
        VARIANT result;
        VariantInit(&result);
        hr = parser->ParseScriptText(expression.c_str(), nullptr, nullptr, L"<eval>", 0, 0, SCRIPTTEXT_ISEXPRESSION, &result, nullptr);
        if (FAILED(hr)) {
            VariantClear(&result);
            out_error = site->last_error().empty() ? L"Provider script execution failed" : site->last_error();
            break;
        }
        hr = VariantChangeType(&result, &result, 0, VT_BSTR);
        if (FAILED(hr) || result.vt != VT_BSTR) {
            VariantClear(&result);
            out_error = L"Provider script returned invalid result";
            break;
        }
        const std::wstring rows_text = result.bstrVal ? result.bstrVal : L"";
        VariantClear(&result);
        if (!ParseDelimitedHolidayRows(rows_text, out_items, out_error)) {
            break;
        }
        ok = true;
    } while (false);

    if (parser) {
        parser->Release();
    }
    script->Close();
    script->Release();
    site->Release();
    return ok;
}

bool CanonicalizeTCalendarIniImpl(const std::wstring& ini_file_path) {
    if (ini_file_path.empty()) {
        return true;
    }

    std::wstring text;
    if (!ReadIniTextFile(std::filesystem::path(ini_file_path), text)) {
        return false;
    }

    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L'\n', start);
        const size_t line_end = (end == std::wstring::npos) ? text.size() : end;
        std::wstring line = text.substr(start, line_end - start);
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }

    std::vector<std::wstring> output;
    output.reserve(lines.size() + 8);
    std::wstring current_section;
    size_t first_tcalendar_insert_index = std::wstring::npos;
    bool saw_tcalendar = false;
    std::vector<std::wstring> key_order;
    std::unordered_map<std::wstring, std::wstring> merged_values;

    auto canonical_key = [](const std::wstring& value) {
        std::wstring key = TrimWhitespace(value);
        for (wchar_t& ch : key) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return key;
    };

    for (const std::wstring& raw_line : lines) {
        const std::wstring trimmed = TrimWhitespace(raw_line);
        if (trimmed.size() >= 2 && trimmed.front() == L'[' && trimmed.back() == L']') {
            current_section = TrimWhitespace(trimmed.substr(1, trimmed.size() - 2));
            if (_wcsicmp(current_section.c_str(), L"TCalendar") == 0) {
                saw_tcalendar = true;
                if (first_tcalendar_insert_index == std::wstring::npos) {
                    first_tcalendar_insert_index = output.size();
                }
                continue;
            }
            output.push_back(raw_line);
            continue;
        }

        if (_wcsicmp(current_section.c_str(), L"TCalendar") == 0) {
            if (trimmed.empty() || trimmed.front() == L';' || trimmed.front() == L'#') {
                continue;
            }
            const size_t eq = raw_line.find(L'=');
            if (eq == std::wstring::npos) {
                continue;
            }
            const std::wstring raw_key = TrimWhitespace(raw_line.substr(0, eq));
            const std::wstring raw_value = raw_line.substr(eq + 1);
            const std::wstring key = canonical_key(raw_key);
            if (merged_values.find(key) == merged_values.end()) {
                key_order.push_back(key);
            }
            merged_values[key] = raw_key + L"=" + raw_value;
            continue;
        }

        output.push_back(raw_line);
    }

    if (!saw_tcalendar) {
        return true;
    }

    std::vector<std::wstring> merged_section;
    merged_section.reserve(key_order.size() + 1);
    merged_section.push_back(L"[TCalendar]");
    for (const std::wstring& key : key_order) {
        const auto it = merged_values.find(key);
        if (it != merged_values.end()) {
            merged_section.push_back(it->second);
        }
    }

    const size_t insert_at = (first_tcalendar_insert_index == std::wstring::npos) ? output.size() : first_tcalendar_insert_index;
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(insert_at), merged_section.begin(), merged_section.end());

    std::wstring normalized;
    for (size_t i = 0; i < output.size(); ++i) {
        normalized += output[i];
        if (i + 1 < output.size()) {
            normalized += L"\r\n";
        }
    }

    return WriteIniTextFileUtf16(std::filesystem::path(ini_file_path), normalized);
}


void MergeHolidaySubscriptionFile(std::map<std::wstring, HolidayItem>& items,
                                  const std::filesystem::path& path,
                                  const std::wstring& date_from,
                                  const std::wstring& date_to,
                                  HolidaySubscriptionSourceStatus* out_status) {
    std::wstring text;
    if (out_status) {
        out_status->resolved_path = path.wstring();
        out_status->exists = std::filesystem::exists(path);
    }
    if (!ReadUtf8TextFile(path, text)) {
        if (out_status) {
            out_status->error = out_status->exists ? L"Failed to read UTF-8 file" : L"File not found";
        }
        return;
    }
    if (out_status) {
        out_status->loaded = true;
    }
    for (std::wstring line : SplitString(text, L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = TrimWhitespace(line);
        if (line.empty()) continue;
        if (line[0] == L'#' || line[0] == L';') continue;

        const auto parts = SplitString(line, L'|');
        if (parts.size() < 2) continue;
        const std::wstring date_key = TrimWhitespace(parts[0]);
        const std::wstring name = TrimWhitespace(parts[1]);
        const std::wstring kind = (parts.size() >= 3) ? TrimWhitespace(parts[2]) : L"subscribed";
        std::tm tm_value{};
        if (!ParseDateKey(date_key, tm_value) || name.empty()) continue;
        if (out_status) {
            ++out_status->record_count;
        }
        if (date_key < date_from || date_key > date_to) continue;
        items[date_key] = HolidayItem{date_key, name, kind.empty() ? L"subscribed" : kind};
        if (out_status) {
            ++out_status->applied_count;
        }
    }
}

void MergeHolidaySubscriptionManifest(std::map<std::wstring, HolidayItem>& items,
                                      const std::filesystem::path& path,
                                      const std::wstring& date_from,
                                      const std::wstring& date_to,
                                      HolidaySubscriptionSourceStatus* out_status) {
    if (out_status) {
        out_status->resolved_path = path.wstring();
        out_status->exists = std::filesystem::exists(path);
    }
    HolidayProviderManifest manifest{};
    std::wstring error;
    if (!LoadHolidayProviderManifest(path, manifest, error)) {
        if (out_status) out_status->error = error.empty() ? L"Manifest parse failed" : error;
        return;
    }

    std::filesystem::path script_path = path.parent_path() / path.stem();
    script_path.replace_extension(L".js");
    if (out_status) {
        out_status->loaded = true;
    }

    std::tm from_tm{};
    std::tm to_tm{};
    if (!ParseDateKey(date_from, from_tm) || !ParseDateKey(date_to, to_tm)) {
        if (out_status) out_status->error = L"Invalid date range";
        return;
    }
    const int from_year = from_tm.tm_year + 1900;
    const int to_year = to_tm.tm_year + 1900;
    for (int year = from_year; year <= to_year; ++year) {
        std::vector<HolidayItem> year_items;
        if (!EvaluateJScriptHolidayProvider(script_path, manifest, year, year_items, error)) {
            if (out_status) out_status->error = error.empty() ? L"Script execution failed" : error;
            return;
        }
        for (const HolidayItem& item : year_items) {
            if (item.date < date_from || item.date > date_to) continue;
            items[item.date] = item;
            if (out_status) {
                ++out_status->record_count;
                ++out_status->applied_count;
            }
        }
    }
}

bool IsCitizenHoliday(const std::map<std::wstring, std::wstring>& base_holidays, const std::wstring& date_key) {
    std::tm tm_value{};
    if (!ParseDateKey(date_key, tm_value)) return false;
    const int year = tm_value.tm_year + 1900;
    if (year < 1948) return false;
    if (IsSundayDateKey(date_key)) return false;
    if (HasBaseHoliday(base_holidays, date_key)) return false;

    std::wstring prev_key;
    std::wstring next_key;
    if (!AddDaysToDateKey(date_key, -1, prev_key) || !AddDaysToDateKey(date_key, 1, next_key)) return false;
    return HasBaseHoliday(base_holidays, prev_key) && HasBaseHoliday(base_holidays, next_key);
}

bool IsHolidayWithoutSubstitute(const std::map<std::wstring, std::wstring>& base_holidays, const std::wstring& date_key) {
    return HasBaseHoliday(base_holidays, date_key) || IsCitizenHoliday(base_holidays, date_key);
}

std::wstring GetSubstituteHolidayName(const std::map<std::wstring, std::wstring>& base_holidays, const std::wstring& date_key) {
    std::tm tm_value{};
    if (!ParseDateKey(date_key, tm_value)) return L"";
    const int year = tm_value.tm_year + 1900;
    if (year < 1973) return L"";
    if (HasBaseHoliday(base_holidays, date_key)) return L"";

    std::wstring scan_key;
    if (!AddDaysToDateKey(date_key, -1, scan_key)) return L"";
    while (IsHolidayWithoutSubstitute(base_holidays, scan_key)) {
        const std::wstring holiday_name = GetBaseHolidayName(base_holidays, scan_key);
        if (!holiday_name.empty() && IsSundayDateKey(scan_key)) {
            return holiday_name + L"振替";
        }
        std::wstring prev_key;
        if (!AddDaysToDateKey(scan_key, -1, prev_key)) break;
        scan_key = prev_key;
    }
    return L"";
}

// Legacy verification path for comparing the retired builtin implementation with the active provider runtime.
// Keep this callable for debug/validation only, not as a production source of truth.
void MergeBuiltinJpPublicHolidayProvider(std::map<std::wstring, HolidayItem>& items,
                                         const std::wstring& date_from,
                                         const std::wstring& date_to,
                                         HolidaySubscriptionSourceStatus* out_status) {
    std::tm from_tm{};
    std::tm to_tm{};
    if (!ParseDateKey(date_from, from_tm) || !ParseDateKey(date_to, to_tm)) {
        if (out_status) {
            out_status->error = L"Invalid date range";
        }
        return;
    }
    const int from_year = from_tm.tm_year + 1900;
    const int to_year = to_tm.tm_year + 1900;
    if (from_year > to_year) {
        if (out_status) {
            out_status->error = L"Invalid date range";
        }
        return;
    }

    if (out_status) {
        out_status->path = kJpPublicHolidayProviderId;
        out_status->resolved_path = std::wstring(L"builtin:") + kJpPublicHolidayProviderId;
        out_status->exists = true;
        out_status->loaded = true;
    }
    for (int year = from_year; year <= to_year; ++year) {
        const auto base_holidays = BuildBaseHolidayMap(year);
        std::wstring date_key = MakeDateKey(year, 1, 1);
        while (true) {
            if (date_key >= date_from && date_key <= date_to) {
                const std::wstring base_name = GetBaseHolidayName(base_holidays, date_key);
                if (!base_name.empty()) {
                    items[date_key] = HolidayItem{date_key, base_name, L"public"};
                    if (out_status) {
                        ++out_status->record_count;
                        ++out_status->applied_count;
                    }
                } else if (IsCitizenHoliday(base_holidays, date_key)) {
                    items[date_key] = HolidayItem{date_key, L"国民の休日", L"citizen"};
                    if (out_status) {
                        ++out_status->record_count;
                        ++out_status->applied_count;
                    }
                } else {
                    const std::wstring substitute_name = GetSubstituteHolidayName(base_holidays, date_key);
                    if (!substitute_name.empty()) {
                        items[date_key] = HolidayItem{date_key, substitute_name, L"substitute"};
                        if (out_status) {
                            ++out_status->record_count;
                            ++out_status->applied_count;
                        }
                    }
                }
            }
            if (date_key >= MakeDateKey(year, 12, 31)) break;
            std::wstring next_key;
            if (!AddDaysToDateKey(date_key, 1, next_key)) break;
            date_key = next_key;
        }
    }
}

void MergeHolidaySubscriptionSources(std::map<std::wstring, HolidayItem>& items,
                                     const std::wstring& subscription_files,
                                     const std::wstring& date_from,
                                     const std::wstring& date_to,
                                     const std::wstring& ini_file_path,
                                     std::vector<HolidaySubscriptionSourceStatus>* out_statuses) {
    if (subscription_files.empty()) return;
    std::filesystem::path base_dir = std::filesystem::current_path();
    if (!ini_file_path.empty()) {
        base_dir = std::filesystem::path(ini_file_path).parent_path();
    }
    const auto merge_source = [&](const std::filesystem::path& path, HolidaySubscriptionSourceStatus* status) {
        const std::wstring extension = ToLowerInvariant(path.extension().wstring());
        if (extension == L".ini") {
            MergeHolidaySubscriptionManifest(items, path, date_from, date_to, status);
        } else {
            MergeHolidaySubscriptionFile(items, path, date_from, date_to, status);
        }
    };
    for (const auto& raw_entry : SplitString(subscription_files, L'|')) {
        const std::wstring entry = TrimWhitespace(raw_entry);
        if (entry.empty()) continue;

        HolidaySubscriptionSourceStatus status{};
        status.path = entry;
        if (_wcsicmp(entry.c_str(), kJpPublicHolidayProviderId) == 0) {
            const std::filesystem::path path = ResolveProviderAliasPath(base_dir, entry);
            if (path.empty()) continue;
            merge_source(path, &status);
        } else {
            const std::filesystem::path path = ResolveSubscriptionPath(base_dir, entry);
            if (path.empty()) continue;
            merge_source(path, &status);
        }
        if (out_statuses) {
            out_statuses->push_back(status);
        }
    }
}

std::vector<HolidayItem> GetSubscribedHolidaysInRange(const std::wstring& date_from,
                                                      const std::wstring& date_to,
                                                      const std::wstring& subscription_files,
                                                      const std::wstring& ini_file_path,
                                                      std::vector<HolidaySubscriptionSourceStatus>* out_statuses = nullptr) {
    std::map<std::wstring, HolidayItem> items;
    MergeHolidaySubscriptionSources(items, subscription_files, date_from, date_to, ini_file_path, out_statuses);

    std::vector<HolidayItem> out;
    out.reserve(items.size());
    for (const auto& entry : items) {
        out.push_back(entry.second);
    }
    return out;
}

bool CompareJpHolidayProviders(const std::wstring& years_csv,
                               const std::wstring& ini_file_path,
                               std::wstring& out_json,
                               std::wstring& out_error) {
    out_json.clear();
    out_error.clear();

    std::vector<int> years;
    if (!ParseYearList(years_csv, years, out_error)) {
        return false;
    }

    std::filesystem::path base_dir = std::filesystem::current_path();
    if (!ini_file_path.empty()) {
        base_dir = std::filesystem::path(ini_file_path).parent_path();
    }
    const std::filesystem::path manifest_path = ResolveProviderAliasPath(base_dir, kJpPublicHolidayProviderId);
    if (manifest_path.empty()) {
        out_error = L"jp-public-holiday provider not found";
        return false;
    }
    if (ToLowerInvariant(manifest_path.extension().wstring()) != L".ini") {
        out_error = L"jp-public-holiday is not resolved to an ini manifest";
        return false;
    }

    std::wstring year_summaries_json = L"[";
    std::wstring years_json = L"[";
    int total_mismatch_count = 0;
    for (size_t i = 0; i < years.size(); ++i) {
        const int year = years[i];
        wchar_t date_from_buf[16] = {0};
        wchar_t date_to_buf[16] = {0};
        _snwprintf_s(date_from_buf, _countof(date_from_buf), _TRUNCATE, L"%04d-01-01", year);
        _snwprintf_s(date_to_buf, _countof(date_to_buf), _TRUNCATE, L"%04d-12-31", year);
        const std::wstring date_from = date_from_buf;
        const std::wstring date_to = date_to_buf;

        std::map<std::wstring, HolidayItem> builtin_items;
        std::map<std::wstring, HolidayItem> manifest_items;
        HolidaySubscriptionSourceStatus manifest_status{};
        MergeBuiltinJpPublicHolidayProvider(builtin_items, date_from, date_to, nullptr);
        MergeHolidaySubscriptionManifest(manifest_items, manifest_path, date_from, date_to, &manifest_status);
        if (!manifest_status.error.empty()) {
            out_error = manifest_status.error;
            return false;
        }

        std::vector<HolidayItem> missing_in_builtin;
        std::vector<HolidayItem> missing_in_manifest;
        std::vector<std::pair<HolidayItem, HolidayItem>> different;

        std::map<std::wstring, bool> seen_dates;
        for (const auto& entry : builtin_items) {
            seen_dates[entry.first] = true;
        }
        for (const auto& entry : manifest_items) {
            seen_dates[entry.first] = true;
        }

        for (const auto& entry : seen_dates) {
            const auto builtin_it = builtin_items.find(entry.first);
            const auto manifest_it = manifest_items.find(entry.first);
            if (builtin_it == builtin_items.end()) {
                missing_in_builtin.push_back(manifest_it->second);
                continue;
            }
            if (manifest_it == manifest_items.end()) {
                missing_in_manifest.push_back(builtin_it->second);
                continue;
            }
            if (builtin_it->second.name != manifest_it->second.name ||
                builtin_it->second.kind != manifest_it->second.kind) {
                different.push_back(std::make_pair(builtin_it->second, manifest_it->second));
            }
        }

        const int year_mismatch_count =
            static_cast<int>(missing_in_builtin.size() + missing_in_manifest.size() + different.size());
        total_mismatch_count += year_mismatch_count;

        if (i) year_summaries_json += L",";
        year_summaries_json += std::wstring(L"{\"year\":") + std::to_wstring(year) +
            L",\"builtinCount\":" + std::to_wstring(builtin_items.size()) +
            L",\"manifestCount\":" + std::to_wstring(manifest_items.size()) +
            L",\"mismatchCount\":" + std::to_wstring(year_mismatch_count) + L"}";

        if (i) years_json += L",";
        years_json += std::wstring(L"{\"year\":") + std::to_wstring(year) +
            L",\"builtinCount\":" + std::to_wstring(builtin_items.size()) +
            L",\"manifestCount\":" + std::to_wstring(manifest_items.size()) +
            L",\"mismatchCount\":" + std::to_wstring(year_mismatch_count) +
            L",\"missingInBuiltin\":[";
        for (size_t j = 0; j < missing_in_builtin.size(); ++j) {
            if (j) years_json += L",";
            years_json += BuildHolidayItemJson(missing_in_builtin[j]);
        }
        years_json += L"],\"missingInManifest\":[";
        for (size_t j = 0; j < missing_in_manifest.size(); ++j) {
            if (j) years_json += L",";
            years_json += BuildHolidayItemJson(missing_in_manifest[j]);
        }
        years_json += L"],\"different\":[";
        for (size_t j = 0; j < different.size(); ++j) {
            if (j) years_json += L",";
            years_json += std::wstring(L"{\"date\":\"") + EscapeJsonString(different[j].first.date) +
                L"\",\"builtin\":" + BuildHolidayItemJson(different[j].first) +
                L",\"manifest\":" + BuildHolidayItemJson(different[j].second) + L"}";
        }
        years_json += L"]}";
    }
    year_summaries_json += L"]";
    years_json += L"]";

    out_json = std::wstring(L"{\"providerId\":\"") + kJpPublicHolidayProviderId +
        L"\",\"manifestPath\":\"" + EscapeJsonString(manifest_path.wstring()) +
        L"\",\"totalMismatchCount\":" + std::to_wstring(total_mismatch_count) +
        L",\"yearSummaries\":" + year_summaries_json +
        L",\"years\":" + years_json + L"}";
    return true;
}

 } // namespace

bool CanonicalizeTCalendarIni(const std::wstring& ini_file_path) {
    return CanonicalizeTCalendarIniImpl(ini_file_path);
}

TCalendarHost::TCalendarHost(const HostConfig& config) : config_(config) {}

TCalendarHost::~TCalendarHost() {
    Shutdown();
}

void TCalendarHost::SetHostWindow(HWND hwnd) {
    host_window_ = hwnd;
}

bool TCalendarHost::Initialize() {
    if (initialized_) return true;

    last_initialize_error_.clear();

    if (!config_.default_template_path.empty() &&
        !std::filesystem::exists(config_.default_template_path)) {
        last_initialize_error_ = L"default template not found: " + config_.default_template_path;
        return false;
    }

    if (config_.enable_webview2_bootstrap && !InitializeWebView2Bootstrap()) {
        last_initialize_error_ = L"WebView2 bootstrap failed (WebView2Loader.dll not available)";
        return false;
    }

    if (config_.storage_db_path.empty()) {
        last_initialize_error_ = L"storage_db_path is empty";
        return false;
    }

    if (!store_.Initialize(config_.storage_db_path)) {
        last_initialize_error_ = L"task store initialize failed: " + store_.GetLastError();
        return false;
    }
    store_.SetTestForceWriteFailure(config_.test_force_storage_write_failure);

    // Phase 1 implementation target:
    // 1) Create WebView2 environment/controller.
    // 2) Load template from default/user path.
    // 3) Register bridge callback.
    initialized_ = true;
    return true;
}

void TCalendarHost::Shutdown() {
    if (!initialized_) return;

    // Phase 1 implementation target:
    // release WebView2 resources and flush pending state.
    store_.Shutdown();
    ShutdownWebView2Bootstrap();
    initialized_ = false;
}

bool TCalendarHost::InitializeWebView2Bootstrap() {
    if (webview2_bootstrap_ready_) return true;

    if (webview2_loader_module_) {
        FreeLibrary(static_cast<HMODULE>(webview2_loader_module_));
        webview2_loader_module_ = nullptr;
    }

    HMODULE module = LoadLibraryW(L"WebView2Loader.dll");
    if (!module) {
        return false;
    }

    const FARPROC proc = GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions");
    if (!proc) {
        FreeLibrary(module);
        return false;
    }

    const auto create_env = reinterpret_cast<CreateCoreWebView2EnvironmentWithOptionsFn>(proc);
    if (!create_env) {
        FreeLibrary(module);
        return false;
    }

    webview2_loader_module_ = module;
    webview2_bootstrap_ready_ = true;
    return true;
}

void TCalendarHost::ShutdownWebView2Bootstrap() {
    if (webview2_loader_module_) {
        FreeLibrary(static_cast<HMODULE>(webview2_loader_module_));
        webview2_loader_module_ = nullptr;
    }
    webview2_bootstrap_ready_ = false;
}

bool TCalendarHost::IsWebView2BootstrapReady() const {
    return webview2_bootstrap_ready_;
}

const std::wstring& TCalendarHost::GetLastInitializeError() const {
    return last_initialize_error_;
}

std::wstring TCalendarHost::BuildResponse(bool ok, const wchar_t* code, const wchar_t* message,
                                          const std::wstring& request_id, const wchar_t* data_json) {
    const std::wstring code_safe = EscapeJsonString(code ? code : L"");
    const std::wstring message_safe = EscapeJsonString(message ? message : L"");
    const std::wstring request_safe = EscapeJsonString(request_id);
    return std::wstring(L"{\"ok\":") + (ok ? L"true" : L"false") +
           L",\"code\":\"" + code_safe +
           L"\",\"message\":\"" + message_safe +
           L"\",\"requestId\":\"" + request_safe +
           L"\",\"data\":" + (data_json ? data_json : L"null") + L"}";
}

bool TCalendarHost::HandleWebMessage(const std::wstring& request_json, std::wstring& response_json) {
    if (!initialized_) return false;

    BridgeRequest req;
    std::wstring err_code;
    std::wstring err_message;
    if (!ValidateBridgeRequest(request_json, req, err_code, err_message)) {
        response_json = BuildResponse(false, err_code.c_str(), err_message.c_str(), L"", L"null");
        return true;
    }

    if (req.api_version != L"1.0") {
        response_json = BuildResponse(false, L"UNSUPPORTED_API_VERSION", L"Unsupported apiVersion", req.request_id, L"null");
        return true;
    }

    if (req.method == L"system.getVersion") {
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"version\":\"1.0\"}");
        return true;
    }

    if (req.method == L"system.getViewConfig") {
        const std::wstring data = BuildViewConfigJson(config_);
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"system.appReady") {
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"accepted\":true}");
        return true;
    }

    if (req.method == L"system.closeWindow") {
        if (!host_window_) {
            response_json = BuildResponse(false, L"HOST_WINDOW_UNAVAILABLE", L"Host window is unavailable", req.request_id, L"null");
            return true;
        }
        PostMessageW(host_window_, WM_CLOSE, 0, 0);
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"accepted\":true}");
        return true;
    }

    const JsonObject* params = nullptr;
    if (!GetObjectField(req.root, L"params", params)) {
        response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing or invalid params object", req.request_id, L"null");
        return true;
    }

    if (req.method == L"system.getStartupState") {
        std::wstring selected_date;
        std::wstring month_from;
        std::wstring month_to;
        if (!GetStringField(*params, L"selectedDate", selected_date) ||
            !GetStringField(*params, L"monthFrom", month_from) ||
            !GetStringField(*params, L"monthTo", month_to)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing selectedDate/monthFrom/monthTo in params", req.request_id, L"null");
            return true;
        }

        int task_days = config_.default_use_custom_range
            ? config_.default_custom_range_days
            : config_.default_range_preset_days;
        if (task_days < 1) task_days = 1;
        if (task_days > 365) task_days = 365;

        std::wstring task_to;
        if (!AddDaysToDateKey(selected_date, task_days - 1, task_to)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid selectedDate in params", req.request_id, L"null");
            return true;
        }

        const auto month_tasks = store_.GetTasksInRange(month_from, month_to);
        const auto task_items = store_.GetTasksInRange(selected_date, task_to);
        const auto month_holidays = GetSubscribedHolidaysInRange(month_from, month_to, config_.holiday_subscription_files, config_.ini_file_path);
        const auto task_holidays = GetSubscribedHolidaysInRange(selected_date, task_to, config_.holiday_subscription_files, config_.ini_file_path);
        const std::wstring data =
            std::wstring(L"{\"config\":") + BuildViewConfigJson(config_) +
            L",\"monthRange\":{\"dateFrom\":\"" + EscapeJsonString(month_from) +
            L"\",\"dateTo\":\"" + EscapeJsonString(month_to) +
            L"\"},\"monthItems\":" + BuildTaskArrayJson(month_tasks) +
            L",\"monthHolidayItems\":" + BuildHolidayArrayJson(month_holidays) +
            L",\"taskRange\":{\"dateFrom\":\"" + EscapeJsonString(selected_date) +
            L"\",\"dateTo\":\"" + EscapeJsonString(task_to) +
            L"\",\"days\":" + std::to_wstring(task_days) +
            L"},\"taskItems\":" + BuildTaskArrayJson(task_items) +
            L",\"taskHolidayItems\":" + BuildHolidayArrayJson(task_holidays) +
            L"}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"system.setViewConfig") {
        std::wstring view_mode;
        std::wstring range_preset;
        std::wstring custom_days_raw;
        std::wstring ui_font_family;
        std::wstring ui_base_font_size_raw;
        std::wstring ui_calendar_date_font_size_raw;
        std::wstring ui_task_font_size_raw;
        std::wstring ui_panel_right_width_raw;
        std::wstring ui_calendar_height_raw;
        std::wstring ui_show_task_panel_raw;
        std::wstring tclock_alert_enabled_raw;
        std::wstring alert_sound_enabled_raw;
        std::wstring alert_sound_path_raw;
        std::wstring holiday_subscription_files_raw;
        std::wstring holiday_subscription_catalog_raw;
        if (!GetStringField(*params, L"defaultViewMode", view_mode) ||
            !GetStringField(*params, L"rangePreset", range_preset)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing defaultViewMode/rangePreset in params", req.request_id, L"null");
            return true;
        }

        if (view_mode != L"list" && view_mode != L"timeline") {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid defaultViewMode", req.request_id, L"null");
            return true;
        }

        bool use_custom_range = false;
        int preset_days = 1;
        if (range_preset == L"custom") {
            use_custom_range = true;
        } else {
            const int candidate = _wtoi(range_preset.c_str());
            if (candidate != 1 && candidate != 7 && candidate != 14 && candidate != 30) {
                response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid rangePreset", req.request_id, L"null");
                return true;
            }
            preset_days = candidate;
        }

        int custom_days = config_.default_custom_range_days;
        if (GetStringField(*params, L"customRangeDays", custom_days_raw)) {
            const int parsed_custom_days = _wtoi(custom_days_raw.c_str());
            if (parsed_custom_days >= 1 && parsed_custom_days <= 365) {
                custom_days = parsed_custom_days;
            }
        }

        config_.default_view_mode = view_mode;
        config_.default_range_preset_days = preset_days;
        config_.default_use_custom_range = use_custom_range;
        config_.default_custom_range_days = custom_days;

        if (GetStringField(*params, L"uiFontFamily", ui_font_family) && !ui_font_family.empty()) {
            config_.ui_font_family = ui_font_family;
        }
        if (GetStringField(*params, L"uiBaseFontSize", ui_base_font_size_raw)) {
            int n = _wtoi(ui_base_font_size_raw.c_str());
            if (n < 9) n = 9;
            if (n > 28) n = 28;
            if (n > 0) config_.ui_base_font_size = n;
        }
        if (GetStringField(*params, L"uiCalendarDateFontSize", ui_calendar_date_font_size_raw)) {
            int n = _wtoi(ui_calendar_date_font_size_raw.c_str());
            if (n < 9) n = 9;
            if (n > 28) n = 28;
            if (n > 0) config_.ui_calendar_date_font_size = n;
        }
        if (GetStringField(*params, L"uiTaskFontSize", ui_task_font_size_raw)) {
            int n = _wtoi(ui_task_font_size_raw.c_str());
            if (n < 9) n = 9;
            if (n > 28) n = 28;
            if (n > 0) config_.ui_task_font_size = n;
        }
        if (GetStringField(*params, L"uiPanelRightWidth", ui_panel_right_width_raw)) {
            int n = _wtoi(ui_panel_right_width_raw.c_str());
            if (n < 320) n = 320;
            if (n > 1600) n = 1600;
            if (n > 0) config_.ui_panel_right_width = n;
        }
        if (GetStringField(*params, L"uiCalendarHeight", ui_calendar_height_raw)) {
            int n = _wtoi(ui_calendar_height_raw.c_str());
            if (n < 280) n = 280;
            if (n > 1200) n = 1200;
            if (n > 0) config_.ui_calendar_height = n;
        }
        if (GetStringField(*params, L"uiShowTaskPanel", ui_show_task_panel_raw)) {
            config_.ui_show_task_panel = (ui_show_task_panel_raw != L"0" && ui_show_task_panel_raw != L"false");
        }
        if (GetStringField(*params, L"tclockAlertEnabled", tclock_alert_enabled_raw)) {
            config_.tclock_alert_enabled = (tclock_alert_enabled_raw != L"0" && tclock_alert_enabled_raw != L"false");
        }
        if (GetStringField(*params, L"alertSoundEnabled", alert_sound_enabled_raw)) {
            config_.alert_sound_enabled = (alert_sound_enabled_raw != L"0" && alert_sound_enabled_raw != L"false");
        }
        if (GetStringField(*params, L"alertSoundPath", alert_sound_path_raw)) {
            config_.alert_sound_path = alert_sound_path_raw.empty()
                ? L"C:\\Windows\\Media\\notify.wav"
                : alert_sound_path_raw;
        }
        if (GetStringField(*params, L"holidaySubscriptionFiles", holiday_subscription_files_raw)) {
            config_.holiday_subscription_files = holiday_subscription_files_raw;
        }
        if (GetStringField(*params, L"holidaySubscriptionCatalog", holiday_subscription_catalog_raw)) {
            config_.holiday_subscription_catalog = holiday_subscription_catalog_raw;
        }

        if (!config_.ini_file_path.empty()) {
            wchar_t preset_buf[16] = {0};
            wchar_t custom_buf[16] = {0};
            wchar_t base_font_buf[16] = {0};
            wchar_t calendar_font_buf[16] = {0};
            wchar_t task_font_buf[16] = {0};
            wchar_t panel_right_buf[16] = {0};
            wchar_t calendar_height_buf[16] = {0};
            _snwprintf_s(preset_buf, _countof(preset_buf), _TRUNCATE, L"%d", config_.default_range_preset_days);
            _snwprintf_s(custom_buf, _countof(custom_buf), _TRUNCATE, L"%d", config_.default_custom_range_days);
            _snwprintf_s(base_font_buf, _countof(base_font_buf), _TRUNCATE, L"%d", config_.ui_base_font_size);
            _snwprintf_s(calendar_font_buf, _countof(calendar_font_buf), _TRUNCATE, L"%d", config_.ui_calendar_date_font_size);
            _snwprintf_s(task_font_buf, _countof(task_font_buf), _TRUNCATE, L"%d", config_.ui_task_font_size);
            _snwprintf_s(panel_right_buf, _countof(panel_right_buf), _TRUNCATE, L"%d", config_.ui_panel_right_width);
            _snwprintf_s(calendar_height_buf, _countof(calendar_height_buf), _TRUNCATE, L"%d", config_.ui_calendar_height);

            const wchar_t* use_custom = config_.default_use_custom_range ? L"1" : L"0";
            const wchar_t* show_task_panel = config_.ui_show_task_panel ? L"1" : L"0";
            const wchar_t* alert_sound_enabled = config_.alert_sound_enabled ? L"1" : L"0";
            bool write_ok = true;
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"DefaultViewMode", config_.default_view_mode.c_str(), config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"DefaultRangePresetDays", preset_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"DefaultCustomRangeDays", custom_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"DefaultUseCustomRange", use_custom, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiFontFamily", config_.ui_font_family.c_str(), config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiBaseFontSize", base_font_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiCalendarDateFontSize", calendar_font_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiTaskFontSize", task_font_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiPanelRightWidth", panel_right_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiCalendarHeight", calendar_height_buf, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"UiShowTaskPanel", show_task_panel, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"AlertSoundEnabled", alert_sound_enabled, config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"AlertSoundPath", config_.alert_sound_path.c_str(), config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionFiles", config_.holiday_subscription_files.c_str(), config_.ini_file_path.c_str()) != FALSE);
            write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"HolidaySubscriptionCatalog", config_.holiday_subscription_catalog.c_str(), config_.ini_file_path.c_str()) != FALSE);
            if (write_ok) {
                write_ok = CanonicalizeTCalendarIni(config_.ini_file_path);
            }
            if (!config_.tclock_ini_file_path.empty()) {
                const wchar_t* tclock_alert = config_.tclock_alert_enabled ? L"1" : L"0";
                // Persist alert startup toggle to tclock-win11.ini [TCalendar].
                write_ok = write_ok && (WritePrivateProfileStringW(L"TCalendar", L"Alart", tclock_alert, config_.tclock_ini_file_path.c_str()) != FALSE);
            }
            if (!write_ok) {
                response_json = BuildResponse(false, L"STORAGE_ERROR", L"Failed to write view config to ini", req.request_id, L"null");
                return true;
            }
        }

        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"saved\":true}");
        return true;
    }

    if (req.method == L"task.create") {
        std::wstring date;
        std::wstring title;
        std::wstring detail;
        std::wstring start_time;
        std::wstring end_time;
        bool alert_enabled = false;
        TaskItem t{};
        if (!GetStringField(*params, L"date", date) ||
            !GetStringField(*params, L"title", title)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing date/title in params", req.request_id, L"null");
            return true;
        }
        GetStringField(*params, L"detail", detail);
        GetStringField(*params, L"startTime", start_time);
        GetStringField(*params, L"endTime", end_time);
        GetBoolField(*params, L"alertEnabled", alert_enabled);
        std::wstring time_error;
        if (!ValidateTaskTimes(start_time, end_time, time_error)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", time_error.c_str(), req.request_id, L"null");
            return true;
        }
        if (!store_.CreateTask(date, title, detail, start_time, end_time, alert_enabled, t)) {
            const std::wstring store_error = store_.GetLastError();
            if (store_error == L"invalid task payload") {
                response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid task payload", req.request_id, L"null");
            } else {
                const std::wstring message = store_error.empty() ? L"Task store error" : store_error;
                response_json = BuildResponse(false, L"STORAGE_ERROR", message.c_str(), req.request_id, L"null");
            }
            return true;
        }
        std::wstring data = std::wstring(L"{\"id\":\"") + EscapeJsonString(t.id) +
                            L"\",\"date\":\"" + EscapeJsonString(t.date) +
                            L"\",\"title\":\"" + EscapeJsonString(t.title) +
                            L"\",\"detail\":\"" + EscapeJsonString(t.detail) +
                            L"\",\"startTime\":\"" + EscapeJsonString(t.start_time) +
                            L"\",\"endTime\":\"" + EscapeJsonString(t.end_time) +
                            L"\",\"done\":false" +
                            L",\"alertEnabled\":" + (t.alert_enabled ? L"true" : L"false") + L"}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"task.toggleDone") {
        std::wstring id;
        bool done = false;
        if (!GetStringField(*params, L"id", id) ||
            !GetBoolField(*params, L"done", done)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing id/done in params", req.request_id, L"null");
            return true;
        }
        if (!store_.ToggleDone(id, done)) {
            const std::wstring store_error = store_.GetLastError();
            if (store_error == L"task not found") {
                response_json = BuildResponse(false, L"NOT_FOUND", L"Task not found", req.request_id, L"null");
            } else {
                const std::wstring message = store_error.empty() ? L"Task store error" : store_error;
                response_json = BuildResponse(false, L"STORAGE_ERROR", message.c_str(), req.request_id, L"null");
            }
            return true;
        }
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"id\":\"updated\"}");
        return true;
    }

    if (req.method == L"task.delete") {
        std::wstring id;
        if (!GetStringField(*params, L"id", id)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing id in params", req.request_id, L"null");
            return true;
        }
        if (!store_.DeleteTask(id)) {
            const std::wstring store_error = store_.GetLastError();
            if (store_error == L"task not found") {
                response_json = BuildResponse(false, L"NOT_FOUND", L"Task not found", req.request_id, L"null");
            } else {
                const std::wstring message = store_error.empty() ? L"Task store error" : store_error;
                response_json = BuildResponse(false, L"STORAGE_ERROR", message.c_str(), req.request_id, L"null");
            }
            return true;
        }
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"id\":\"deleted\"}");
        return true;
    }

    if (req.method == L"task.update") {
        std::wstring id;
        std::wstring title;
        std::wstring detail;
        std::wstring start_time;
        std::wstring end_time;
        bool alert_enabled = false;
        if (!GetStringField(*params, L"id", id) ||
            !GetStringField(*params, L"title", title)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing id/title in params", req.request_id, L"null");
            return true;
        }
        GetStringField(*params, L"detail", detail);
        GetStringField(*params, L"startTime", start_time);
        GetStringField(*params, L"endTime", end_time);
        GetBoolField(*params, L"alertEnabled", alert_enabled);
        std::wstring time_error;
        if (!ValidateTaskTimes(start_time, end_time, time_error)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", time_error.c_str(), req.request_id, L"null");
            return true;
        }
        if (!store_.UpdateTask(id, title, detail, start_time, end_time, alert_enabled)) {
            const std::wstring store_error = store_.GetLastError();
            if (store_error == L"task not found") {
                response_json = BuildResponse(false, L"NOT_FOUND", L"Task not found", req.request_id, L"null");
            } else if (store_error == L"invalid task payload") {
                response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid task payload", req.request_id, L"null");
            } else {
                const std::wstring message = store_error.empty() ? L"Task store error" : store_error;
                response_json = BuildResponse(false, L"STORAGE_ERROR", message.c_str(), req.request_id, L"null");
            }
            return true;
        }
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"id\":\"updated\"}");
        return true;
    }

    if (req.method == L"task.updateTitle") {
        std::wstring id;
        std::wstring title;
        if (!GetStringField(*params, L"id", id) ||
            !GetStringField(*params, L"title", title)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing id/title in params", req.request_id, L"null");
            return true;
        }
        if (!store_.UpdateTitle(id, title)) {
            const std::wstring store_error = store_.GetLastError();
            if (store_error == L"task not found") {
                response_json = BuildResponse(false, L"NOT_FOUND", L"Task not found", req.request_id, L"null");
            } else if (store_error == L"invalid task payload") {
                response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Invalid task payload", req.request_id, L"null");
            } else {
                const std::wstring message = store_error.empty() ? L"Task store error" : store_error;
                response_json = BuildResponse(false, L"STORAGE_ERROR", message.c_str(), req.request_id, L"null");
            }
            return true;
        }
        response_json = BuildResponse(true, L"OK", L"", req.request_id, L"{\"id\":\"updated\"}");
        return true;
    }

    if (req.method == L"calendar.getDayTasks") {
        std::wstring date;
        if (!GetStringField(*params, L"date", date)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing date in params", req.request_id, L"null");
            return true;
        }
        const auto tasks = store_.GetDayTasks(date);
        std::wstring data = L"{\"items\":[";
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (i) data += L",";
            data += std::wstring(L"{\"id\":\"") + EscapeJsonString(tasks[i].id) + L"\",\"date\":\"" + EscapeJsonString(tasks[i].date) +
                    L"\",\"title\":\"" + EscapeJsonString(tasks[i].title) +
                    L"\",\"detail\":\"" + EscapeJsonString(tasks[i].detail) +
                    L"\",\"startTime\":\"" + EscapeJsonString(tasks[i].start_time) +
                    L"\",\"endTime\":\"" + EscapeJsonString(tasks[i].end_time) +
                    L"\",\"done\":" + (tasks[i].done ? L"true" : L"false") +
                    L",\"alertEnabled\":" + (tasks[i].alert_enabled ? L"true" : L"false") + L"}";
        }
        data += L"]}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"calendar.getRangeTasks") {
        std::wstring date_from;
        std::wstring date_to;
        if (!GetStringField(*params, L"dateFrom", date_from) ||
            !GetStringField(*params, L"dateTo", date_to)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing dateFrom/dateTo in params", req.request_id, L"null");
            return true;
        }
        const auto tasks = store_.GetTasksInRange(date_from, date_to);
        std::wstring data = L"{\"items\":[";
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (i) data += L",";
            data += std::wstring(L"{\"id\":\"") + EscapeJsonString(tasks[i].id) + L"\",\"date\":\"" + EscapeJsonString(tasks[i].date) +
                    L"\",\"title\":\"" + EscapeJsonString(tasks[i].title) +
                    L"\",\"detail\":\"" + EscapeJsonString(tasks[i].detail) +
                    L"\",\"startTime\":\"" + EscapeJsonString(tasks[i].start_time) +
                    L"\",\"endTime\":\"" + EscapeJsonString(tasks[i].end_time) +
                    L"\",\"done\":" + (tasks[i].done ? L"true" : L"false") +
                    L",\"alertEnabled\":" + (tasks[i].alert_enabled ? L"true" : L"false") + L"}";
        }
        data += L"]}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"calendar.getRangeHolidays") {
        std::wstring date_from;
        std::wstring date_to;
        if (!GetStringField(*params, L"dateFrom", date_from) ||
            !GetStringField(*params, L"dateTo", date_to)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing dateFrom/dateTo in params", req.request_id, L"null");
            return true;
        }
        const auto items = GetSubscribedHolidaysInRange(date_from, date_to, config_.holiday_subscription_files, config_.ini_file_path);
        const std::wstring data = std::wstring(L"{\"providerId\":\"subscription-set\",\"items\":") +
            BuildHolidayArrayJson(items) + L"}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"system.getHolidaySubscriptionStatus") {
        std::wstring holiday_subscription_files = config_.holiday_subscription_files;
        (void)GetStringField(*params, L"holidaySubscriptionFiles", holiday_subscription_files);
        const std::wstring sample_from = L"1873-01-01";
        const std::wstring sample_to = L"2150-12-31";
        std::vector<HolidaySubscriptionSourceStatus> statuses;
        (void)GetSubscribedHolidaysInRange(sample_from, sample_to, holiday_subscription_files, config_.ini_file_path, &statuses);
        const std::wstring data = std::wstring(L"{\"files\":") + BuildHolidaySubscriptionStatusArrayJson(statuses) + L"}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"system.getHolidayProviderCatalog") {
        const auto items = EnumerateHolidayProviderCatalog(config_.ini_file_path);
        const std::wstring data = std::wstring(L"{\"items\":") + BuildHolidayProviderCatalogJson(items) + L"}";
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    if (req.method == L"debug.compareJpHolidayProviders") {
        std::wstring years_csv;
        if (!GetStringField(*params, L"yearsCsv", years_csv)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", L"Missing yearsCsv in params", req.request_id, L"null");
            return true;
        }
        std::wstring data;
        std::wstring compare_error;
        if (!CompareJpHolidayProviders(years_csv, config_.ini_file_path, data, compare_error)) {
            response_json = BuildResponse(false, L"VALIDATION_ERROR", compare_error.c_str(), req.request_id, L"null");
            return true;
        }
        response_json = BuildResponse(true, L"OK", L"", req.request_id, data.c_str());
        return true;
    }

    response_json = BuildResponse(false, L"NOT_IMPLEMENTED", L"Method not implemented", req.request_id, L"null");
    return true;
}

} // namespace tcalendar
