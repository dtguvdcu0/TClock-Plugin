#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <functional>
extern "C" {
#include "../third_party/md4c/entity.h"
}

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace tcard_clip {
constexpr size_t max_source = 2 * 1024 * 1024;
constexpr size_t max_image = 16 * 1024 * 1024;
constexpr size_t max_import = 24 * 1024 * 1024;
struct Clip { std::wstring source, url; bool externalImages = false; bool imageOnly = false; std::wstring imageAlt; std::wstring htmlSource, htmlContext; bool richHtml = false; };
using ImageResolver = std::function<std::wstring(const std::wstring&)>;

inline std::wstring lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
inline std::wstring utf8(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n && !s.empty()) throw std::runtime_error("Invalid clipboard UTF-8");
    std::wstring result(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}
inline std::wstring base64(const std::vector<BYTE>& bytes)
{
    DWORD n = 0;
    if (!CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)) return {};
    std::wstring result(n, 0);
    if (!CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &n)) return {};
    result.resize(n);
    return result;
}
inline std::vector<BYTE> image_bytes(const std::wstring& uri)
{
    const size_t comma = uri.find(L',');
    if (comma == std::wstring::npos || comma > 64 || uri.size() > max_image * 2) return {};
    const auto type = lower(uri.substr(0, comma));
    if (type != L"data:image/png;base64" && type != L"data:image/jpeg;base64" &&
        type != L"data:image/gif;base64" && type != L"data:image/bmp;base64") return {};
    DWORD n = 0;
    const auto data = uri.substr(comma + 1);
    if (!CryptStringToBinaryW(data.c_str(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr) || n > max_image) return {};
    std::vector<BYTE> bytes(n);
    if (!CryptStringToBinaryW(data.c_str(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64, bytes.data(), &n, nullptr, nullptr)) return {};
    return bytes;
}
inline std::wstring entities(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'&') { out += text[i]; continue; }
        size_t end = i + 1;
        while (end < text.size() && end - i <= 49 && text[end] != L';' && text[end] != L'&' && !iswspace(text[end])) ++end;
        if (end == text.size() || end - i > 49 || text[end] != L';') { out += L'&'; continue; }
        const auto key = text.substr(i + 1, end - i - 1);
        unsigned long value = 0;
        if (key == L"amp") value = '&'; else if (key == L"lt") value = '<';
        else if (key == L"gt") value = '>'; else if (key == L"quot") value = '"';
        else if (key == L"apos" || key == L"#39") value = '\'';
        else if (key.size() > 1 && key[0] == '#') {
            const bool hex = key.size() > 2 && (key[1] == 'x' || key[1] == 'X');
            wchar_t* tail = nullptr;
            value = wcstoul(key.c_str() + (hex ? 2 : 1), &tail, hex ? 16 : 10);
            if (!tail || *tail) value = 0;
        }
        else {
            char name[52]{}; bool ascii = true;
            for (size_t n = i; n <= end; ++n) { if (text[n] > 127) { ascii = false; break; } name[n - i] = static_cast<char>(text[n]); }
            const auto* named = ascii ? entity_lookup(name, end - i + 1) : nullptr;
            if (named) {
                for (unsigned cp : named->codepoints) {
                    if (!cp) continue;
                    if (cp > 0xffff) { cp -= 0x10000; out += static_cast<wchar_t>(0xd800 + (cp >> 10)); out += static_cast<wchar_t>(0xdc00 + (cp & 1023)); }
                    else out += static_cast<wchar_t>(cp);
                }
                i = end; continue;
            }
        }
        if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { out += L'&'; continue; }
        if (value > 0xffff) { value -= 0x10000; out += static_cast<wchar_t>(0xd800 + (value >> 10)); out += static_cast<wchar_t>(0xdc00 + (value & 1023)); }
        else out += static_cast<wchar_t>(value);
        i = end;
    }
    return out;
}
inline void append_escaped(std::wstring& out, wchar_t c)
{
    if (c == L'&') { out += L"&amp;"; return; }
    if (std::wstring_view(L"\\`*_{}[]<>!|#+-.=~").find(c) != std::wstring_view::npos) out += L'\\';
    out += c;
}
inline std::wstring escape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) append_escaped(out, c);
    return out;
}
inline std::wstring url(const std::wstring& raw, const std::wstring& base)
{
    std::wstring s = raw;
    if (s.empty() || s.size() > 8192) return {};
    for (wchar_t c : s) if (c < 32 || c == 127) return {};
    if (s.find(L':') == std::wstring::npos || s.rfind(L"//", 0) == 0) {
        wchar_t combined[16384]{}; DWORD n = ARRAYSIZE(combined);
        if (base.empty() || FAILED(UrlCombineW(base.c_str(), s.c_str(), combined, &n, 0))) return {};
        s = combined;
    }
    const auto l = lower(s);
    if (l.rfind(L"https://", 0) != 0 && l.rfind(L"http://", 0) != 0 && l.rfind(L"mailto:", 0) != 0) return {};
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L' ') out += L"%20"; else if (c == L'(') out += L"%28"; else if (c == L')') out += L"%29";
        else if (c == L'<') out += L"%3C"; else if (c == L'>') out += L"%3E"; else if (c == L'"') out += L"%22"; else if (c == L'|') out += L"%7C"; else if (c == L'\\') out += L"%5C"; else out += c;
    }
    return out;
}
inline std::wstring markdown_url(const std::wstring& value)
{
    std::wstring out; out.reserve(value.size());
    for (wchar_t c : value) { if (c == L'&') out += L"&amp;"; else out += c; }
    return out;
}
inline std::map<std::wstring, std::wstring> attributes(const std::wstring& tag, size_t at)
{
    std::map<std::wstring, std::wstring> values;
    while (at < tag.size()) {
        while (at < tag.size() && (iswspace(tag[at]) || tag[at] == '/')) ++at;
        const size_t begin = at;
        while (at < tag.size() && !iswspace(tag[at]) && tag[at] != '=' && tag[at] != '/') ++at;
        if (at == begin) break;
        auto key = lower(tag.substr(begin, at - begin));
        while (at < tag.size() && iswspace(tag[at])) ++at;
        std::wstring value;
        if (at < tag.size() && tag[at] == '=') {
            ++at; while (at < tag.size() && iswspace(tag[at])) ++at;
            const wchar_t quote = at < tag.size() && (tag[at] == '\'' || tag[at] == '"') ? tag[at++] : 0;
            const size_t start = at;
            while (at < tag.size() && (quote ? tag[at] != quote : !iswspace(tag[at]))) ++at;
            value = entities(tag.substr(start, at - start));
            if (quote && at < tag.size()) ++at;
        }
        values[key] = value;
    }
    return values;
}
inline bool html_space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f'; }
struct HtmlWriter {
    std::wstring out, prefix;
    bool cellBreak = false;
    void flush() { if (cellBreak) { out += L"<br>"; cellBreak = false; } }
    unsigned breaks = 2;
    size_t lastContent = 0;
    void text(std::wstring_view value) {
        if (!value.empty()) flush();
        for (wchar_t c : value) {
            if (c == L'\r') continue;
            if (c == L'\n') { out += c; ++breaks; }
            else { if (breaks) out += prefix; out += c; breaks = 0; }
        }
        lastContent = out.size();
        if (out.size() > max_source) throw std::runtime_error("Converted clip too large");
    }
    void line(unsigned count = 1) {
        if (out.empty()) return;
        if (!breaks) {
            while (!out.empty() && out.back() == L' ') out.pop_back();
            lastContent = out.size(); out += L'\n'; breaks = 1;
        }
        while (breaks < count) {
            auto blank = prefix;
            while (!blank.empty() && blank.back() == L' ') blank.pop_back();
            out += blank + L'\n'; ++breaks;
        }
    }
    void space() { if (!cellBreak && !breaks && !out.empty() && !html_space(out.back())) text(L" "); }
    void hard() { text(L"  \n"); }
    void escaped(wchar_t c) {
        flush();
        if (breaks) { out += prefix; breaks = 0; }
        append_escaped(out, c); lastContent = out.size();
        if (out.size() > max_source) throw std::runtime_error("Converted clip too large");
    }
    void finish() {
        out.resize((std::min)(out.size(), lastContent));
        while (!out.empty() && html_space(out.back())) out.pop_back();
        const auto start = out.find_first_not_of(L" \r\n\t");
        if (start != std::wstring::npos) out.erase(0, start);
    }
};
inline void normalize_table(HtmlWriter& w, size_t start, const std::wstring& prefix)
{
    struct Row { std::wstring text; unsigned columns = 0; bool separator = false, newline = false; };
    std::vector<Row> rows;
    unsigned columns = 0;
    const size_t trailing = w.out.size() - (std::min)(w.out.size(), w.lastContent);
    for (size_t at = start; at < w.out.size();) {
        const auto end = w.out.find(L'\n', at);
        Row row{w.out.substr(at, end == std::wstring::npos ? end : end - at), 0, false, end != std::wstring::npos};
        if (row.text.size() > prefix.size() && row.text.compare(0, prefix.size(), prefix) == 0 && row.text[prefix.size()] == L'|') {
            unsigned pipes = 0; bool escaped = false; row.separator = true;
            for (size_t i = prefix.size(); i < row.text.size(); ++i) {
                const wchar_t c = row.text[i];
                if (c == L'|' && !escaped) ++pipes;
                if (c != L'|' && c != L'-' && c != L':' && !html_space(c)) row.separator = false;
                escaped = c == L'\\' && !escaped;
            }
            row.columns = pipes > 1 ? pipes - 1 : 0;
            row.separator = row.separator && row.text.find(L'-', prefix.size()) != std::wstring::npos;
            columns = (std::max)(columns, row.columns);
        }
        rows.push_back(std::move(row));
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    if (columns > 64) throw std::runtime_error("HTML table too wide");
    w.out.resize(start);
    for (auto& row : rows) {
        if (row.columns) for (unsigned n = row.columns; n < columns; ++n) row.text += row.separator ? L" --- |" : L"  |";
        w.out += row.text; if (row.newline) w.out += L'\n';
        if (w.out.size() > max_source) throw std::runtime_error("Converted clip too large");
    }
    w.lastContent = w.out.size() - (std::min)(w.out.size(), trailing);
}
inline unsigned html_number(const std::wstring& text, unsigned fallback, unsigned maximum)
{
    if (text.empty()) return fallback;
    unsigned n = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9' || n > maximum / 10) return fallback;
        n = n * 10 + c - L'0'; if (n > maximum) return fallback;
    }
    return n;
}
inline Clip html(const std::wstring& input, const std::wstring& origin = {}, const ImageResolver& resolve = {})
{
    if (input.size() > max_source) throw std::runtime_error("Clipboard too large");
    Clip clip; clip.url = url(origin, {});
    HtmlWriter w; w.out.reserve(input.size());
    struct List { bool ordered; unsigned next; std::wstring base, continuation; bool active = false; };
    struct Span { std::wstring name, open, close; size_t at; bool code = false; };
    std::vector<List> lists;
    std::vector<Span> spans;
    struct Layout { std::wstring name; bool block; int whitespace; };
    std::vector<Layout> layouts;
    std::vector<std::wstring> quotes;
    std::vector<std::pair<size_t, std::wstring>> tables;
    std::wstring skipped, preText;
    unsigned skipDepth = 0;
    bool rawSkip = false, pre = false, row = false, firstRow = true, itemFresh = false;
    unsigned cells = 0, cellSpan = 1;
    size_t cellStart = 0;
    unsigned imageCount = 0;
    bool hasText = false;
    std::vector<std::wstring> aligns;
    auto code = [&]() { return !spans.empty() && spans.back().code; };
    auto open_span = [&](const std::wstring& name, const std::wstring& open, const std::wstring& close, bool isCode = false) {
        if (spans.size() >= 128) throw std::runtime_error("HTML nesting too deep");
        w.flush();
        if (w.breaks) { w.out += w.prefix; w.breaks = 0; }
        spans.push_back({name, open, close, w.out.size(), isCode});
    };
    auto close_span = [&]() {
        auto span = spans.back(); spans.pop_back();
        if (span.at > w.out.size()) return;
        auto text = w.out.substr(span.at);
        if (span.code) {
            size_t longest = 0, run = 0;
            for (wchar_t c : text) { run = c == 0x60 ? run + 1 : 0; longest = (std::max)(longest, run); }
            span.open = span.close = std::wstring(longest + 1, wchar_t(0x60));
            if (!text.empty() && (text.front() == 0x60 || text.back() == 0x60 ||
                (text.front() == L' ' && text.back() == L' ' && text.find_first_not_of(L' ') != std::wstring::npos))) text = L" " + text + L" ";
            if (row) { std::wstring protectedText; for (wchar_t c : text) { if (c == L'|') protectedText += L'\\'; protectedText += c; } text = std::move(protectedText); }
            w.out.replace(span.at, std::wstring::npos, span.open + text + span.close);
        } else {
            std::wstring marked;
            for (size_t begin = 0; begin < text.size();) {
                const auto end = text.find(L"\n\n", begin);
                const auto part = text.substr(begin, end == std::wstring::npos ? end : end - begin);
                const auto first = part.find_first_not_of(L" \t\r\n"), last = part.find_last_not_of(L" \t\r\n");
                marked += first == std::wstring::npos ? part : part.substr(0, first) + span.open + part.substr(first, last - first + 1) + span.close + part.substr(last + 1);
                if (end == std::wstring::npos) break;
                marked += L"\n\n"; begin = end + 2;
            }
            w.out.replace(span.at, std::wstring::npos, marked);
        }
        w.lastContent = w.out.size();
    };
    auto close_named = [&](const std::wstring& name) {
        for (size_t n = spans.size(); n > 0; --n) if (spans[n - 1].name == name) {
            while (spans.size() >= n) close_span();
            break;
        }
    };
    auto boundary = [&](unsigned count = 2) {
        if (row) { if (w.out.size() > cellStart) w.cellBreak = true; }
        else if (!itemFresh) w.line(count);
    };
    auto close_pre = [&]() {
        size_t longest = 0, run = 0;
        for (wchar_t c : preText) { run = c == 0x60 ? run + 1 : 0; longest = (std::max)(longest, run); }
        const std::wstring fence((std::max)(size_t(3), longest + 1), wchar_t(0x60));
        if (row) {
            for (wchar_t c : preText) {
                if (c == L'\n') w.text(L"<br>");
                else if (c != L'\r') w.escaped(c);
            }
        }
        else {
            w.line(2); w.text(fence + L"\n" + preText + (preText.empty() || preText.back() != L'\n' ? L"\n" : L"") + fence); w.line(2);
        }
        preText.clear(); pre = false;
    };
    for (size_t at = 0; at < input.size();) {
        if (w.out.size() > max_source) throw std::runtime_error("Converted clip too large");
        if (input.compare(at, 4, L"<!--") == 0) {
            const auto end = input.find(L"-->", at + 4); at = end == std::wstring::npos ? input.size() : end + 3; continue;
        }
        if (input[at] != L'<') {
            const auto end = input.find(L'<', at);
            if (skipped.empty()) {
                const auto text = entities(input.substr(at, end == std::wstring::npos ? end : end - at));
                if (std::any_of(text.begin(), text.end(), [](wchar_t c) { return !html_space(c); })) hasText = true;
                if (pre) preText += text;
                else for (size_t n = 0; n < text.size(); ++n) {
                    const wchar_t c = text[n];
                    const int whitespace = layouts.empty() ? 0 : layouts.back().whitespace;
                    if (!code() && whitespace && (c == L'\r' || c == L'\n')) {
                        if (c == L'\r' && n + 1 < text.size() && text[n + 1] == L'\n') ++n;
                        if (row) w.text(L"<br>"); else w.hard();
                    }
                    else if (!code() && whitespace == 2 && (c == L' ' || c == L'\t')) {
                        w.text(c == L'\t' ? L"\u00a0\u00a0\u00a0\u00a0" : L"\u00a0");
                    }
                    else if (html_space(c)) w.space();
                    else { if (code()) w.text(std::wstring_view(&c, 1)); else w.escaped(c); itemFresh = false; }
                }
            }
            at = end == std::wstring::npos ? input.size() : end; continue;
        }
        size_t end = at + 1; wchar_t quote = 0;
        for (; end < input.size(); ++end) {
            const wchar_t c = input[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == L'\'' || c == L'"') quote = c;
            else if (c == L'>') break;
        }
        if (end == input.size()) { if (skipped.empty()) { if (pre) preText += entities(input.substr(at)); else w.text(escape(entities(input.substr(at)))); } break; }
        const auto token = input.substr(at + 1, end - at - 1); at = end + 1;
        size_t i = 0; while (i < token.size() && html_space(token[i])) ++i;
        const bool close = i < token.size() && token[i] == L'/'; if (close) ++i;
        const size_t begin = i;
        while (i < token.size() && (iswalnum(token[i]) || token[i] == L'-' || token[i] == L':')) ++i;
        const auto name = lower(token.substr(begin, i - begin));
        if (!skipped.empty()) {
            if (name == skipped) {
                if (close && --skipDepth == 0) skipped.clear();
                else if (!close && !rawSkip) ++skipDepth;
            }
            continue;
        }
        if (name == L"script" || name == L"style" || name == L"iframe" || name == L"object" || name == L"head" || name == L"svg" || name == L"template") {
            if (!close) { skipped = name; skipDepth = 1; rawSkip = name == L"script" || name == L"style" || name == L"iframe"; }
            continue;
        }
        if (pre) { if (name == L"pre" && close) close_pre(); else if (name == L"br") preText += L'\n'; continue; }
        if (code()) { if (name == L"code" && close) close_named(name); else if (name == L"br") w.space(); continue; }
        const auto attrs = close ? std::map<std::wstring, std::wstring>() : attributes(token, i);
        auto attr = [&](const wchar_t* key) { const auto it = attrs.find(key); return it == attrs.end() ? std::wstring() : it->second; };
        auto css = lower(attr(L"style")); css.erase(std::remove_if(css.begin(), css.end(), html_space), css.end()); css = L";" + css + L";";
        auto styled = [&](const wchar_t* value) {
            const auto pos = css.find(L";" + std::wstring(value)); if (pos == std::wstring::npos) return false;
            const auto next = pos + 1 + wcslen(value); return next < css.size() && (css[next] == L';' || css[next] == L'!');
        };
        const bool hidden = attrs.count(L"hidden") || styled(L"display:none") || styled(L"visibility:hidden");
        const bool isVoid = name == L"img" || name == L"br" || name == L"hr" || name == L"input" || name == L"meta" || name == L"link" || name == L"source" || name == L"wbr";
        if (!close && hidden) { if (!isVoid) { skipped = name; skipDepth = 1; rawSkip = false; } continue; }
        if (name == L"pre" && !close) { pre = true; preText.clear(); continue; }
        const bool genericBlock = name == L"p" || name == L"div" || name == L"section" || name == L"article" ||
            name == L"header" || name == L"footer" || name == L"main" || name == L"nav" || name == L"aside" ||
            name == L"figure" || name == L"figcaption" || name == L"address" || name == L"details" || name == L"summary" ||
            name == L"dl" || name == L"dt" || name == L"dd";
        bool block = genericBlock || styled(L"display:block") || styled(L"display:list-item") || styled(L"display:flex") || styled(L"display:grid");
        if (!close && (styled(L"display:inline") || styled(L"display:inline-block"))) block = false;
        if (close) {
            for (size_t n = layouts.size(); n > 0; --n) if (layouts[n - 1].name == name) {
                block = layouts[n - 1].block; layouts.resize(n - 1); break;
            }
        } else if (!isVoid && name != L"code" && !name.empty() && token.find_last_not_of(L" \t\r\n") != token.find_last_of(L'/')) {
            // Optional end tags must not accumulate inherited layout frames across siblings.
            if (name == L"li" || name == L"p" || name == L"dt" || name == L"dd") {
                for (size_t n = layouts.size(); n > 0; --n) {
                    const auto& previous = layouts[n - 1].name;
                    if (previous == name || ((name == L"dt" || name == L"dd") && (previous == L"dt" || previous == L"dd"))) {
                        layouts.resize(n - 1); break;
                    }
                    if ((name == L"li" && (previous == L"ul" || previous == L"ol")) ||
                        ((name == L"dt" || name == L"dd") && previous == L"dl")) break;
                }
            }
            if (layouts.size() >= 128) throw std::runtime_error("HTML nesting too deep");
            int whitespace = layouts.empty() ? 0 : layouts.back().whitespace;
            if (styled(L"white-space:pre-line")) whitespace = 1;
            else if (styled(L"white-space:pre") || styled(L"white-space:pre-wrap") || styled(L"white-space:break-spaces")) whitespace = 2;
            else if (styled(L"white-space:normal") || styled(L"white-space:nowrap")) whitespace = 0;
            layouts.push_back({name, block, whitespace});
        }
        if (block && !close) boundary();
        if (name == L"br") { if (row) w.text(L"<br>"); else w.hard(); }
        else if (genericBlock) { /* Boundaries are handled before/after inline state changes. */ }
        else if (name.size() == 2 && name[0] == L'h' && name[1] >= L'1' && name[1] <= L'6') {
            boundary(); if (!close && !row) w.text(std::wstring(name[1] - L'0', L'#') + L" ");
        }
        else if (name == L"strong" || name == L"b" || name == L"em" || name == L"i" || name == L"del" || name == L"s" || name == L"code" || name == L"span") {
            if (close) close_named(name);
            else {
                std::wstring mark;
                if (name == L"strong" || name == L"b" || styled(L"font-weight:bold") || styled(L"font-weight:600") || styled(L"font-weight:700") || styled(L"font-weight:800") || styled(L"font-weight:900")) mark += L"**";
                if (name == L"em" || name == L"i" || styled(L"font-style:italic")) mark += L"*";
                if (name == L"del" || name == L"s" || styled(L"text-decoration:line-through") || styled(L"text-decoration-line:line-through")) mark += L"~~";
                open_span(name, mark, mark, name == L"code");
            }
        }
        else if (name == L"hr") { boundary(); w.text(row ? L"\u2014" : L"---"); boundary(); }
        else if (name == L"blockquote") {
            boundary();
            if (!row) {
                if (!close) { if (quotes.size() >= 32) throw std::runtime_error("HTML nesting too deep"); quotes.push_back(w.prefix); w.prefix += L"> "; }
                else if (!quotes.empty()) { w.prefix = quotes.back(); quotes.pop_back(); }
            }
        }
        else if (name == L"ul" || name == L"ol") {
            boundary(1);
            if (!close) {
                if (lists.size() >= 32) throw std::runtime_error("HTML nesting too deep");
                lists.push_back({name == L"ol", html_number(attr(L"start"), 1, 999999999), w.prefix, w.prefix, false});
            } else if (!lists.empty()) { w.prefix = lists.back().base; lists.pop_back(); if (!lists.empty() && lists.back().active) w.prefix = lists.back().continuation; }
            if (close && lists.empty() && !row) w.line(2);
        }
        else if (name == L"li") {
            boundary(1);
            if (!close) {
                if (!lists.empty()) w.prefix = lists.back().base;
                std::wstring marker = L"- ";
                if (!lists.empty() && lists.back().ordered) {
                    lists.back().next = html_number(attr(L"value"), lists.back().next, 999999999);
                    marker = std::to_wstring(lists.back().next++) + L". ";
                }
                if (row && marker == L"- ") marker = L"\u2022 ";
                w.text(marker); itemFresh = true;
                if (!lists.empty()) { lists.back().continuation = lists.back().base + std::wstring(marker.size(), L' '); lists.back().active = true; if (!row) w.prefix = lists.back().continuation; }
            } else { itemFresh = false; if (!lists.empty()) { lists.back().active = false; w.prefix = lists.back().base; } }
        }
        else if (name == L"a") {
            if (close) close_named(name);
            else { const auto target = url(attr(L"href"), clip.url); open_span(name, target.empty() ? L"" : L"[", target.empty() ? L"" : L"](" + markdown_url(target) + L")"); }
        }
        else if (name == L"img" && !close) {
            ++imageCount; clip.imageAlt = attr(L"alt");
            const auto alt = escape(attr(L"alt").empty() ? L"Image" : attr(L"alt"));
            const auto src = attr(L"src");
            const auto bytes = image_bytes(src);
            if (!bytes.empty()) w.text(L"![" + alt + L"](" + lower(src.substr(0, src.find(L','))) + L"," + base64(bytes) + L")");
            else {
                const auto target = url(src, clip.url);
                const auto image = !target.empty() && resolve ? resolve(target) : std::wstring{};
                if (!image.empty()) w.text(L"![" + alt + L"](" + image + L")");
                else if (!target.empty()) { w.text(L"[" + alt + L"](" + markdown_url(target) + L")"); clip.externalImages = true; }
                else w.text(alt);
            }
            itemFresh = false;
        }
        else if (name == L"input" && !close && lower(attr(L"type")) == L"checkbox") {
            const bool checked = attrs.count(L"checked") != 0;
            if (itemFresh && !lists.empty()) w.text(checked ? L"[x] " : L"[ ] ");
            else w.text(checked ? L"\u2611 " : L"\u2610 ");
            hasText = true;
        }
        else if (name == L"table") {
            w.line(2);
            if (!close) {
                if (tables.size() >= 32) throw std::runtime_error("HTML nesting too deep");
                tables.push_back({w.out.size(), w.prefix}); firstRow = true;
            } else if (!tables.empty()) {
                normalize_table(w, tables.back().first, tables.back().second); tables.pop_back();
            }
        }
        else if (name == L"tr") {
            if (!close) { w.line(); w.text(L"| "); row = true; cells = 0; aligns.clear(); }
            else {
                w.line(); row = false;
                if (firstRow && cells) { for (const auto& alignment : aligns) w.text(L"| " + alignment + L" "); w.text(L"|"); w.line(); firstRow = false; }
            }
        }
        else if (name == L"td" || name == L"th") {
            if (close) { w.cellBreak = false; for (unsigned n = 0; n < cellSpan; ++n) w.text(L" | "); }
            else {
                w.cellBreak = false; cellStart = w.out.size();
                cellSpan = (std::max)(1u, html_number(attr(L"colspan"), 1, 64)); cells += cellSpan;
                if (cells > 64) throw std::runtime_error("HTML table too wide");
                const auto align = lower(attr(L"align"));
                const auto separator = align == L"center" || styled(L"text-align:center") ? L":---:" : align == L"right" || styled(L"text-align:right") ? L"---:" : align == L"left" || styled(L"text-align:left") ? L":---" : L"---";
                for (unsigned n = 0; n < cellSpan; ++n) aligns.push_back(separator);
            }
        }
        if (block && (close || isVoid)) boundary();
    }
    if (pre) close_pre();
    while (!spans.empty()) close_span();
    w.finish();
    if (!w.out.empty() && !clip.url.empty()) w.out += L"\n\n---\n[Source](" + markdown_url(clip.url) + L")";
    if (w.out.size() > max_source) throw std::runtime_error("Converted clip too large");
    clip.source = std::move(w.out); clip.imageOnly = !hasText && imageCount == 1;
    if (clip.externalImages) clip.htmlSource = input;
    return clip;
}
inline Clip cf_html(const std::string& bytes)
{
    if (bytes.size() > max_source * 4) throw std::runtime_error("Clipboard too large");
    const size_t headerSize = (std::min)({size_t(4096), bytes.size(), bytes.find('<')});
    const std::string_view header(bytes.data(), headerSize);
    auto field = [&](const char* key) {
        for (size_t at = 0; at < header.size();) {
            const auto end = header.find_first_of("\r\n", at);
            auto line = header.substr(at, end == std::string_view::npos ? end : end - at);
            if (line.substr(0, strlen(key)) == key) {
                line.remove_prefix(strlen(key));
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
                return std::string(line);
            }
            if (end == std::string_view::npos) break;
            at = end + 1;
        }
        return std::string();
    };
    auto number = [&](const char* key) -> size_t {
        const auto value = field(key);
        if (value.empty() || value[0] == '-') return std::string::npos;
        size_t n = 0;
        for (char c : value) {
            if (c < '0' || c > '9' || n > bytes.size() / 10) return std::string::npos;
            n = n * 10 + c - '0'; if (n > bytes.size()) return std::string::npos;
        }
        return n;
    };
    const size_t htmlStart = number("StartHTML:"), htmlEnd = number("EndHTML:");
    size_t start = number("StartFragment:"), end = number("EndFragment:");
    if (start == std::string::npos || end == std::string::npos || end < start) {
        auto marker = [&](const char* name, size_t from) -> std::pair<size_t, size_t> {
            const auto at = bytes.find(name, from); if (at == std::string::npos) return {at, at};
            size_t tail = at + strlen(name);
            while (tail < bytes.size() && tail - at < 64 && (bytes[tail] == ' ' || bytes[tail] == '\t')) ++tail;
            return bytes.compare(tail, 3, "-->") == 0 ? std::make_pair(at, tail + 3) : std::make_pair(std::string::npos, std::string::npos);
        };
        const auto first = marker("<!--StartFragment", htmlStart == std::string::npos ? 0 : htmlStart);
        const auto last = marker("<!--EndFragment", first.second == std::string::npos ? 0 : first.second);
        start = first.second; end = last.first;
    }
    if (start == std::string::npos || end == std::string::npos || end < start ||
        (htmlStart != std::string::npos && start < htmlStart) || (htmlEnd != std::string::npos && end > htmlEnd))
        throw std::runtime_error("Invalid HTML clipboard offsets");
    std::wstring origin;
    try { origin = utf8(field("SourceURL:")); } catch (const std::runtime_error&) {}
    auto result = html(utf8(bytes.substr(start, end - start)), origin);
    result.htmlSource = utf8(bytes.substr(start, end - start));
    if (htmlStart != std::string::npos && htmlEnd != std::string::npos && htmlEnd >= htmlStart)
        result.htmlContext = utf8(bytes.substr(htmlStart, htmlEnd - htmlStart));
    return result;
}
inline std::vector<BYTE> clipboard_data(UINT format, size_t limit)
{
    HANDLE h = GetClipboardData(format); if (!h) return {};
    const SIZE_T n = GlobalSize(h); if (!n || n > limit) throw std::runtime_error("Clipboard too large");
    const auto p = static_cast<const BYTE*>(GlobalLock(h)); if (!p) throw std::runtime_error("Clipboard unavailable");
    struct Unlock { HANDLE handle; ~Unlock() { GlobalUnlock(handle); } } unlock{h};
    return std::vector<BYTE>(p, p + n);
}
struct ClipboardData { std::vector<BYTE> html, png, dib, text; };
inline std::wstring bitmap_uri(const ClipboardData& data)
{
    const BYTE signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data.png.size() >= sizeof(signature) && data.png.size() <= max_image &&
        memcmp(data.png.data(), signature, sizeof(signature)) == 0)
        return L"data:image/png;base64," + base64(data.png);
    if (data.dib.size() < sizeof(BITMAPINFOHEADER) || data.dib.size() > max_image - sizeof(BITMAPFILEHEADER)) return {};
    BITMAPINFOHEADER header{}; memcpy(&header, data.dib.data(), sizeof(header));
    if (header.biSize != sizeof(header) || header.biCompression != BI_RGB || header.biPlanes != 1 ||
        (header.biBitCount != 24 && header.biBitCount != 32) || header.biWidth <= 0 || header.biWidth > 8192 ||
        !header.biHeight || header.biHeight < -8192 || header.biHeight > 8192 || header.biClrUsed > 256) return {};
    const size_t offset = sizeof(header) + static_cast<size_t>(header.biClrUsed) * sizeof(RGBQUAD);
    const size_t stride = ((static_cast<size_t>(header.biWidth) * header.biBitCount + 31) / 32) * 4;
    const size_t height = static_cast<size_t>(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    if (offset > data.dib.size() || stride * height > data.dib.size() - offset) return {};
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfSize = static_cast<DWORD>(sizeof(file) + data.dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
    std::vector<BYTE> bytes(sizeof(file)); memcpy(bytes.data(), &file, sizeof(file));
    bytes.insert(bytes.end(), data.dib.begin(), data.dib.end());
    return L"data:image/bmp;base64," + base64(bytes);
}
inline Clip from_clipboard(const ClipboardData& data)
{
    if (!data.html.empty()) {
        try {
            auto clip = cf_html(std::string(data.html.begin(), data.html.end()));
            if (!clip.source.empty()) {
                if (clip.imageOnly && clip.source.find(L"](data:image/") == std::wstring::npos) {
                    const auto image = bitmap_uri(data);
                    if (!image.empty()) {
                        clip.source = L"![" + escape(clip.imageAlt.empty() ? L"Image" : clip.imageAlt) + L"](" + image + L")";
                        if (!clip.url.empty()) clip.source += L"\n\n---\n[Source](" + markdown_url(clip.url) + L")";
                        clip.externalImages = false;
                        clip.htmlSource = L"<img src=\"" + image + L"\" alt=\"Image\">";
                    }
                }
                return clip;
            }
        } catch (const std::runtime_error&) {}
    }
    const auto image = bitmap_uri(data);
    if (!image.empty()) return {L"![Image](" + image + L")", {}, false};
    Clip clip;
    if (data.text.size() > max_source * sizeof(wchar_t) + sizeof(wchar_t)) throw std::runtime_error("Clipboard too large");
    clip.source.reserve(data.text.size() / sizeof(wchar_t));
    for (size_t i = 0; i + 1 < data.text.size(); i += 2) {
        const wchar_t c = static_cast<wchar_t>(data.text[i] | (data.text[i + 1] << 8));
        if (!c) break; clip.source += c;
    }
    if (clip.source.empty()) throw std::runtime_error("Clipboard has no supported content");
    if (clip.source.size() > max_source) throw std::runtime_error("Clipboard too large");
    return clip;
}
inline Clip read(HWND owner)
{
    ClipboardData data;
    {
        if (!OpenClipboard(owner)) throw std::runtime_error("Clipboard busy");
        struct Close { ~Close() { CloseClipboard(); } } close;
        auto capture = [](UINT format, size_t limit) {
            try { return clipboard_data(format, limit); }
            catch (const std::runtime_error&) { return std::vector<BYTE>(); }
        };
        data.html = capture(RegisterClipboardFormatW(L"HTML Format"), max_source * 4);
        data.png = capture(RegisterClipboardFormatW(L"PNG"), max_image);
        data.dib = capture(CF_DIB, max_image);
        data.text = capture(CF_UNICODETEXT, max_source * sizeof(wchar_t) + sizeof(wchar_t));
    }
    // Parse/decode only after releasing the global clipboard lock.
    return from_clipboard(data);
}
}
