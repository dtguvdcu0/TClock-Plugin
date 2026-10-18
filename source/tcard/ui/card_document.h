#pragma once
#include "card_images.h"
#include "../core/card_assets.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <string_view>
#ifndef MD4C_USE_UTF16
#define MD4C_USE_UTF16
#endif
#include "../third_party/md4c/md4c.h"
extern "C" {
#include "../third_party/md4c/entity.h"
}
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace tcard_md {
// RTF is presentation only. Never recover editable Markdown from this document.
inline std::string rtf_text(std::wstring_view text)
{
    std::string out;
    for (wchar_t c : text) {
        if (c == L'\\' || c == L'{' || c == L'}') { out += '\\'; out += static_cast<char>(c); }
        else if (c == L'\n') out += "\\line ";
        else if (c == L'\r') {}
        else if (c == L'\t') out += "\\tab ";
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else out += "\\u" + std::to_string(static_cast<short>(c)) + "?";
    }
    return out;
}
inline void codepoint(std::wstring& out, unsigned value)
{
    if (!value || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) value = 0xfffd;
    if (value < 0x10000) out += static_cast<wchar_t>(value);
    else { value -= 0x10000; out += static_cast<wchar_t>(0xd800 + (value >> 10)); out += static_cast<wchar_t>(0xdc00 + (value & 1023)); }
}
inline std::wstring entity(std::wstring_view value)
{
    std::wstring result;
    if (value.size() > 3 && value[1] == L'#') {
        const bool hex = value[2] == L'x' || value[2] == L'X'; unsigned number = 0;
        for (size_t i = hex ? 3 : 2; i + 1 < value.size(); ++i) {
            const wchar_t c = value[i];
            const unsigned digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : 99;
            if (digit >= (hex ? 16u : 10u) || number > 0x10ffff) return L"\ufffd";
            number = number * (hex ? 16 : 10) + digit;
        }
        codepoint(result, number);
    } else {
        std::string name;
        for (wchar_t c : value) { if (c > 127) return std::wstring(value); name += static_cast<char>(c); }
        const auto* found = entity_lookup(name.data(), name.size());
        if (!found) return std::wstring(value);
        codepoint(result, found->codepoints[0]); if (found->codepoints[1]) codepoint(result, found->codepoints[1]);
    }
    return result;
}
inline std::wstring attribute(const MD_ATTRIBUTE& value)
{
    std::wstring out;
    for (size_t i = 0; value.size && value.substr_offsets[i] < value.size; ++i) {
        const auto start = value.substr_offsets[i], end = value.substr_offsets[i + 1];
        const std::wstring_view part(value.text + start, end - start);
        out += value.substr_types[i] == MD_TEXT_ENTITY ? entity(part) : value.substr_types[i] == MD_TEXT_NULLCHAR ? L"\ufffd" : std::wstring(part);
    }
    return out;
}
inline std::wstring safe_link(const std::wstring& link)
{
    if (link.empty() || link.size() > 8192) return {};
    for (wchar_t c : link) if (c < 32 || c == L'"' || c == L'\\') return {};
    if (link[0] == L'#') return link;
    return tcard_clip::url(link, L"");
}
struct List { bool ordered = false; unsigned next = 1; bool tight = true; };
struct Anchor { std::wstring id, token; LONG position = -1; };
inline std::wstring slug(const std::wstring& text)
{
    std::wstring out;
    for (wchar_t c : text) {
        if (iswspace(c)) out += L'-';
        else if (iswalnum(c) || c == L'-' || c == L'_' || c >= 128) out += static_cast<wchar_t>(towlower(c));
    }
    return out;
}
struct Document {
    std::string rtf;
    std::vector<std::wstring> images;
    std::vector<int> imageWidths;
    std::wstring imageMarker = L"\uf8ffTCARD_IMAGE\uf8ff";
    std::wstring anchorMarker = L"\uf8fdTCARD_ANCHOR\uf8fd";
    size_t headingAnchor = 0;
    std::vector<Anchor> anchors;
    std::wstring heading, htmlBlock;
    std::vector<std::wstring> htmlStyles;
    bool inHeading = false, inHtml = false;
    int htmlDepth = 0;
    int body = 28, width = 3600, quote = 0, imageDepth = 0;
    unsigned columns = 0;
    bool itemStart = false, cell = false, imageLinked = false, lineOpen = false;
    std::vector<List> lists;
    std::vector<bool> links;
    void paragraph(bool tight = false) {
        if (lineOpen && !cell) rtf += "\\par ";
        lineOpen = true;
        rtf += "\\pard\\plain\\f0\\fs" + std::to_string(body) + "\\cf0";
        if (cell) rtf += "\\intbl";
        rtf += "\\li" + std::to_string(quote * 240 + static_cast<int>(lists.size()) * 360);
        rtf += tight ? "\\sa0 " : "\\sa120 ";
    }
    void link(const std::wstring& target) {
        const auto safe = safe_link(target); links.push_back(!safe.empty());
        rtf += safe.empty() ? "{" : "{\\field{\\*\\fldinst HYPERLINK \"" + rtf_text(safe) + "\"}{\\fldrslt\\ul\\cf1 ";
    }
    void end_link() { rtf += links.back() ? "}}" : "}"; links.pop_back(); }
    void anchor(const std::wstring& id) {
        const auto token = anchorMarker + std::to_wstring(anchors.size()) + L'\uf8fe';
        anchors.push_back({id, token}); rtf += rtf_text(token);
    }
    void close_html() { while (!htmlStyles.empty()) { rtf += "}"; htmlStyles.pop_back(); } }
    int parse(const std::wstring& source) {
        MD_PARSER parser{};
        parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_FOOTNOTES | MD_FLAG_ADMONITIONS;
        parser.enter_block = enter_block; parser.leave_block = leave_block;
        parser.enter_span = enter_span; parser.leave_span = leave_span; parser.text = text;
        return md_parse(source.data(), static_cast<MD_SIZE>(source.size()), &parser, this);
    }
    static int enter_block(MD_BLOCKTYPE type, void* detail, void* data) {
        auto& d = *static_cast<Document*>(data);
        if (d.lists.size() > 128 || d.quote > 128 || d.rtf.size() > 32 * 1024 * 1024) return 1;
        switch (type) {
        case MD_BLOCK_QUOTE: ++d.quote; break;
        case MD_BLOCK_UL: d.lists.push_back({false, 1, static_cast<MD_BLOCK_UL_DETAIL*>(detail)->is_tight != 0}); break;
        case MD_BLOCK_OL: { auto* p = static_cast<MD_BLOCK_OL_DETAIL*>(detail); d.lists.push_back({true, p->start, p->is_tight != 0}); break; }
        case MD_BLOCK_LI: {
            d.paragraph(d.lists.back().tight);
            d.rtf += "\\fi-300\\tx" + std::to_string(d.quote * 240 + static_cast<int>(d.lists.size()) * 360) + " ";
            const auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
            const std::wstring marker = li->is_task ? (li->task_mark == L' ' ? L"\u2610" : L"\u2611") : d.lists.back().ordered ? std::to_wstring(d.lists.back().next++) + L"." : L"\u2022";
            d.rtf += rtf_text(marker) + "\\tab "; d.itemStart = true; break;
        }
        case MD_BLOCK_P: if (!d.itemStart) d.paragraph(!d.lists.empty() && d.lists.back().tight); d.itemStart = false; break;
        case MD_BLOCK_H: {
            d.heading.clear(); d.inHeading = true;
            d.paragraph(); const int scale[] = {160, 140, 125, 115, 108, 100};
            d.rtf += "{\\b\\fs" + std::to_string(d.body * scale[static_cast<MD_BLOCK_H_DETAIL*>(detail)->level - 1] / 100) + " ";
            d.headingAnchor = d.anchors.size(); d.anchor(L""); break;
        }
        case MD_BLOCK_CODE: d.paragraph(); d.rtf += "{\\f1\\highlight2\\cf4 "; break;
        case MD_BLOCK_HTML: d.inHtml = true; d.htmlBlock.clear(); break;
        case MD_BLOCK_HR: d.paragraph(); d.rtf += "\\brdrb\\brdrs\\brdrw10\\brdrcf3 \\par "; d.lineOpen = false; break;
        case MD_BLOCK_TABLE: d.columns = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail)->col_count; if (!d.columns || d.columns > 64) return 1; break;
        case MD_BLOCK_TR:
            d.rtf += "\\trowd\\trgaph60\\trleft0";
            for (unsigned i = 1; i <= d.columns; ++i)
                d.rtf += "\\clvertalt\\clbrdrt\\brdrs\\brdrw8\\brdrcf3\\clbrdrl\\brdrs\\brdrw8\\brdrcf3\\clbrdrb\\brdrs\\brdrw8\\brdrcf3\\clbrdrr\\brdrs\\brdrw8\\brdrcf3\\cellx" + std::to_string(d.width * i / d.columns);
            d.rtf += " "; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: {
            d.cell = true; d.paragraph(true); const auto align = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
            d.rtf += align == MD_ALIGN_CENTER ? "\\qc " : align == MD_ALIGN_RIGHT ? "\\qr " : "\\ql ";
            if (type == MD_BLOCK_TH) d.rtf += "\\b "; break;
        }
        case MD_BLOCK_FOOTNOTE_DEF_SECTION: d.paragraph(); d.rtf += "\\brdrt\\brdrs\\brdrw8\\brdrcf3 \\par "; break;
        case MD_BLOCK_FOOTNOTE_DEF: {
            const auto id = std::to_wstring(static_cast<MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail)->id);
            d.paragraph(); d.anchor(L"fn-" + id); d.rtf += rtf_text(L"[" + id + L"] "); d.itemStart = true; break;
        }
        case MD_BLOCK_ADMONITION: d.paragraph(); d.rtf += "{\\b " + rtf_text(attribute(static_cast<MD_BLOCK_ADMONITION_DETAIL*>(detail)->type)) + "}\\par "; ++d.quote; break;
        default: break;
        }
        return 0;
    }
    static int leave_block(MD_BLOCKTYPE type, void*, void* data) {
        auto& d = *static_cast<Document*>(data);
        switch (type) {
        case MD_BLOCK_QUOTE: case MD_BLOCK_ADMONITION: --d.quote; break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: d.lists.pop_back(); break;
        case MD_BLOCK_H: {
            const auto base = slug(d.heading); auto id = base; unsigned suffix = 0;
            while (std::any_of(d.anchors.begin(), d.anchors.end(), [&](const Anchor& a) { return &a != &d.anchors[d.headingAnchor] && a.id == id; })) id = base + L"-" + std::to_wstring(++suffix);
            d.anchors[d.headingAnchor].id = id; d.inHeading = false; d.close_html(); d.rtf += "}\\par "; d.lineOpen = false; break;
        }
        case MD_BLOCK_CODE: d.rtf += "}\\par "; d.lineOpen = false; break;
        case MD_BLOCK_P: d.close_html(); d.rtf += "\\par "; d.lineOpen = false; break;
        case MD_BLOCK_HTML: {
            d.inHtml = false; const auto html = std::move(d.htmlBlock);
            if (d.htmlDepth >= 8) { d.paragraph(); d.rtf += rtf_text(html) + "\\par "; d.lineOpen = false; break; }
            ++d.htmlDepth;
            try { const auto converted = tcard_clip::html(html); if (d.parse(converted.source)) return 1; }
            catch (const std::exception&) { return 1; }
            --d.htmlDepth; break;
        }
        case MD_BLOCK_LI: d.close_html(); if (d.lineOpen) d.rtf += "\\par "; d.itemStart = false; d.lineOpen = false; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD: d.rtf += "\\cell "; d.cell = false; d.lineOpen = false; break;
        case MD_BLOCK_TR: d.rtf += "\\row "; break;
        case MD_BLOCK_TABLE: d.columns = 0; d.paragraph(true); d.lineOpen = false; break;
        default: break;
        }
        return 0;
    }
    static int enter_span(MD_SPANTYPE type, void* detail, void* data) {
        auto& d = *static_cast<Document*>(data);
        if (d.imageDepth) { if (type == MD_SPAN_IMG) ++d.imageDepth; return 0; }
        switch (type) {
        case MD_SPAN_EM: d.rtf += "{\\i "; break;
        case MD_SPAN_STRONG: d.rtf += "{\\b "; break;
        case MD_SPAN_DEL: d.rtf += "{\\strike "; break;
        case MD_SPAN_CODE: d.rtf += "{\\f1\\highlight2\\cf4 "; break;
        case MD_SPAN_A: d.link(attribute(static_cast<MD_SPAN_A_DETAIL*>(detail)->href)); break;
        case MD_SPAN_IMG: {
            const auto uri = attribute(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
            d.imageLinked = !((uri.rfind(L"data:image/", 0) == 0 || tcard_asset::relative(uri, true)) && d.images.size() < 32);
            if (!d.imageLinked) {
                d.images.push_back(uri);
                d.imageWidths.push_back((std::max)(120, d.width / static_cast<int>(d.cell && d.columns ? d.columns : 1) - 240));
                d.rtf += rtf_text(d.imageMarker);
            } else d.link(uri);
            d.imageDepth = 1; d.rtf += "{"; break;
        }
        case MD_SPAN_FOOTNOTE_REF: {
            const auto id = std::to_wstring(static_cast<MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail)->id);
            d.link(L"#fn-" + id); d.rtf += "{\\super " + rtf_text(L"[" + id + L"]"); break;
        }
        default: d.rtf += "{"; break;
        }
        return 0;
    }
    static int leave_span(MD_SPANTYPE type, void*, void* data) {
        auto& d = *static_cast<Document*>(data);
        if (d.imageDepth) {
            if (type == MD_SPAN_IMG && --d.imageDepth == 0) { d.rtf += "}"; if (d.imageLinked) d.end_link(); }
            return 0;
        }
        if (type == MD_SPAN_A) d.end_link();
        else if (type == MD_SPAN_FOOTNOTE_REF) { d.rtf += "}"; d.end_link(); }
        else d.rtf += "}";
        return 0;
    }
    static int text(MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* data) {
        auto& d = *static_cast<Document*>(data);
        if (d.rtf.size() > 32 * 1024 * 1024) return 1;
        if (type == MD_TEXT_HTML) {
            const std::wstring raw(value, size);
            if (d.inHtml) { d.htmlBlock += raw; return 0; }
            const auto tag = tcard_clip::lower(raw);
            if (tag == L"<br>" || tag == L"<br/>" || tag == L"<br />") { d.rtf += "\\line "; return 0; }
            const std::pair<const wchar_t*, const char*> allowed[] = {{L"b","\\b "},{L"strong","\\b "},{L"i","\\i "},{L"em","\\i "},{L"u","\\ul "},{L"s","\\strike "},{L"del","\\strike "},{L"sub","\\sub "},{L"sup","\\super "},{L"code","\\f1 "}};
            for (const auto& item : allowed) {
                if (tag == L"<" + std::wstring(item.first) + L">") { d.rtf += "{"; d.rtf += item.second; d.htmlStyles.push_back(item.first); return 0; }
                if (tag == L"</" + std::wstring(item.first) + L">" && !d.htmlStyles.empty() && d.htmlStyles.back() == item.first) { d.rtf += "}"; d.htmlStyles.pop_back(); return 0; }
            }
            // Unsupported inline HTML remains inert and visible, never executed.
            d.rtf += rtf_text(raw); return 0;
        }
        if (d.inHeading) d.heading += type == MD_TEXT_ENTITY ? entity(std::wstring_view(value, size)) : std::wstring(value, size);
        if (type == MD_TEXT_BR) d.rtf += "\\line ";
        else if (type == MD_TEXT_SOFTBR) d.rtf += " ";
        else if (type == MD_TEXT_NULLCHAR) d.rtf += "\\u-3?";
        else if (type == MD_TEXT_ENTITY) d.rtf += rtf_text(entity(std::wstring_view(value, size)));
        else d.rtf += rtf_text(std::wstring_view(value, size));
        return 0;
    }
};
inline COLORREF code_background(COLORREF paper)
{
    const int brightness = GetRValue(paper) * 299 + GetGValue(paper) * 587 + GetBValue(paper) * 114;
    const int mix = brightness >= 128000 ? 0 : 255;
    return RGB((GetRValue(paper) * 90 + mix * 10 + 50) / 100,
        (GetGValue(paper) * 90 + mix * 10 + 50) / 100,
        (GetBValue(paper) * 90 + mix * 10 + 50) / 100);
}
inline std::string rtf_color(COLORREF color)
{
    return "\\red" + std::to_string(GetRValue(color)) + "\\green" + std::to_string(GetGValue(color)) + "\\blue" + std::to_string(GetBValue(color)) + ";";
}
inline Document document(const std::wstring& source, float size, const std::wstring& family, int width, COLORREF paper = RGB(255,255,255))
{
    Document d; d.body = (std::max)(16, (std::min)(96, static_cast<int>(size * 2 + 0.5f))); d.width = (std::max)(120, width);
    while (source.find(d.imageMarker) != std::wstring::npos) d.imageMarker += L'\uf8ff';
    while (source.find(d.anchorMarker) != std::wstring::npos) d.anchorMarker += L'\uf8fd';
    const auto code = code_background(paper);
    const auto ink = GetRValue(code) * 299 + GetGValue(code) * 587 + GetBValue(code) * 114 >= 128000 ? RGB(24,24,24) : RGB(245,245,245);
    d.rtf = "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0 " + rtf_text(family.empty() ? L"Segoe UI" : family) + ";}{\\f1 Consolas;}}{\\colortbl;\\red30\\green85\\blue170;" + rtf_color(code) + "\\red140\\green140\\blue140;" + rtf_color(ink) + "}\\viewkind4\\fs" + std::to_string(d.body) + " ";
    if (source.size() > tcard_clip::max_source || d.parse(source) != 0) {
        d.images.clear(); d.anchors.clear(); d.rtf = "{\\rtf1\\ansi\\uc1 " + rtf_text(source);
    }
    d.rtf += "}"; return d;
}
inline std::wstring control_text(HWND control)
{
    GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE, 1200};
    const auto count = SendMessageW(control, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
    std::wstring value(static_cast<size_t>((std::max)(LRESULT(0), count)) + 1, L'\0');
    GETTEXTEX get{static_cast<DWORD>(value.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
    const auto copied = SendMessageW(control, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(value.data()));
    value.resize(static_cast<size_t>((std::max)(LRESULT(0), copied))); return value;
}
struct View { std::wstring source, family, assetRoot; std::vector<Anchor> anchors; std::map<std::wstring, tcard_image::Picture> pictures; float size = 0; bool enabled = false, busy = false, valid = false; int width = 0; COLORREF paper = RGB(255,255,255); };
inline constexpr UINT_PTR view_id = 0x54434d44;
inline View* view(HWND control);
inline void render(HWND control, const std::wstring& source, bool enabled, float size, const std::wstring& family, const std::wstring& assetRoot = {}, COLORREF paper = RGB(255,255,255));
inline LRESULT CALLBACK view_proc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    auto* state = reinterpret_cast<View*>(data);
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(control, view_proc, id); delete state; return DefSubclassProc(control, message, wp, lp); }
    if (message == WM_SETTEXT && !state->busy) state->valid = false;
    const auto result = DefSubclassProc(control, message, wp, lp);
    if (message == WM_SIZE && state->valid && state->enabled && !state->busy) render(control, state->source, true, state->size, state->family, state->assetRoot, state->paper);
    return result;
}
inline View* view(HWND control)
{
    DWORD_PTR value = 0;
    return GetWindowSubclass(control, view_proc, view_id, &value) ? reinterpret_cast<View*>(value) : nullptr;
}
struct Stream { const std::string* text; size_t at = 0; };
inline DWORD CALLBACK stream_in(DWORD_PTR cookie, LPBYTE bytes, LONG capacity, LONG* count)
{
    auto& s = *reinterpret_cast<Stream*>(cookie);
    *count = static_cast<LONG>((std::min)(static_cast<size_t>(capacity), s.text->size() - s.at));
    memcpy(bytes, s.text->data() + s.at, *count); s.at += *count; return 0;
}
inline void render(HWND control, const std::wstring& source, bool enabled, float size, const std::wstring& family, const std::wstring& assetRoot, COLORREF paper)
{
    if (!control) return;
    auto* state = view(control);
    if (!state) { state = new View; if (!SetWindowSubclass(control, view_proc, view_id, reinterpret_cast<DWORD_PTR>(state))) { delete state; return; } }
    if (state->busy) return;
    RECT rect{}; GetClientRect(control, &rect);
    HDC dc = GetDC(control); const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96; if (dc) ReleaseDC(control, dc);
    const int width = MulDiv((std::max)(8L, rect.right - 24), 1440, dpi);
    if (state->valid && state->source == source && state->assetRoot == assetRoot && state->enabled == enabled && state->size == size && state->family == family && state->paper == paper && (!enabled || state->width == width)) return;
    if (state->source != source || state->assetRoot != assetRoot) state->pictures.clear();
    state->assetRoot = assetRoot; state->paper = paper;
    state->source = source; state->family = family; state->enabled = enabled; state->size = size; state->width = width; state->busy = true;
    CHARRANGE selection{}; POINT scroll{};
    SendMessageW(control, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection)); SendMessageW(control, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
    const bool readonly = (GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) != 0;
    // WM_SETREDRAW(TRUE) would reveal a reader hidden during source editing.
    const bool redraw = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
    SendMessageW(control, EM_SETREADONLY, FALSE, 0);
    if (redraw) SendMessageW(control, WM_SETREDRAW, FALSE, 0);
    if (enabled) {
        const auto doc = document(source, size, family, width, paper);
        state->anchors = doc.anchors;
        Stream data{&doc.rtf}; EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&data), 0, stream_in};
        SendMessageW(control, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
        if (stream.dwError) SetWindowTextW(control, source.c_str());
        else {
            size_t imageIndex = 0, placedBytes = 0;
            for (const auto& uri : doc.images) {
                const LONG available = MulDiv(doc.imageWidths[imageIndex++], 2540, 1440);
                const auto text = control_text(control); const auto at = text.find(doc.imageMarker);
                if (at == std::wstring::npos) break;
                SendMessageW(control, EM_SETSEL, at, at + doc.imageMarker.size());
                SendMessageW(control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"!"));
                auto found = state->pictures.find(uri);
                if (found == state->pictures.end()) {
                    const auto imageData = tcard_asset::relative(uri, true) ? tcard_asset::load_image(state->assetRoot, uri) : uri;
                    auto picture = tcard_image::decode(imageData);
                    if (picture.dib.size() > 64 * 1024 * 1024 - placedBytes) continue;
                    size_t cached = 0; for (const auto& entry : state->pictures) cached += entry.second.dib.size();
                    if (cached + picture.dib.size() <= 16 * 1024 * 1024) found = state->pictures.emplace(uri, std::move(picture)).first;
                    else if (!picture.dib.empty()) { tcard_image::place(control, at, picture, available); placedBytes += picture.dib.size(); }
                }
                if (found != state->pictures.end() && !found->second.dib.empty() && found->second.dib.size() <= 64 * 1024 * 1024 - placedBytes) {
                    tcard_image::place(control, at, found->second, available); placedBytes += found->second.dib.size();
                }
            }
            for (auto& anchor : state->anchors) {
                const auto text = control_text(control); const auto at = text.find(anchor.token);
                if (at == std::wstring::npos) continue;
                SendMessageW(control, EM_SETSEL, at, at + anchor.token.size());
                SendMessageW(control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
                anchor.position = static_cast<LONG>(at);
            }
        }
    } else {
        state->anchors.clear();
        SetWindowTextW(control, source.c_str()); SendMessageW(control, EM_SETSEL, 0, -1);
        CHARFORMAT2W face{}; face.cbSize = sizeof(face); face.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_HIDDEN | CFM_UNDERLINE | CFM_COLOR | CFM_BACKCOLOR | CFM_LINK;
        face.dwEffects = CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR; face.yHeight = static_cast<LONG>(size * 20);
        wcsncpy_s(face.szFaceName, family.empty() ? L"Segoe UI" : family.c_str(), _TRUNCATE);
        SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&face));
        PARAFORMAT2 para{}; para.cbSize = sizeof(para); para.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_ALIGNMENT | PFM_SPACEAFTER; para.wAlignment = PFA_LEFT;
        SendMessageW(control, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&para));
    }
    SendMessageW(control, EM_SETEVENTMASK, 0, SendMessageW(control, EM_GETEVENTMASK, 0, 0) | ENM_LINK);
    SendMessageW(control, EM_SETREADONLY, readonly, 0); SendMessageW(control, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(control, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll)); SendMessageW(control, EM_EMPTYUNDOBUFFER, 0, 0);
    if (redraw) { SendMessageW(control, WM_SETREDRAW, TRUE, 0); InvalidateRect(control, nullptr, TRUE); }
    state->valid = true; state->busy = false;
}
inline bool matches(HWND control, const std::wstring& source)
{
    const auto* state = view(control); return state && state->valid && state->source == source;
}
inline LRESULT notify(LPARAM parameter)
{
    const auto* event = reinterpret_cast<const ENLINK*>(parameter);
    if (!event || event->nmhdr.code != EN_LINK || event->msg != WM_LBUTTONUP) return 0;
    const auto length = event->chrg.cpMax - event->chrg.cpMin; if (length <= 0 || length > 16384) return 1;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0'); TEXTRANGEW range{event->chrg, text.data()};
    text.resize(static_cast<size_t>(SendMessageW(event->nmhdr.hwndFrom, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&range))));
    if (text.rfind(L"HYPERLINK \"", 0) == 0) { const auto end = text.find(L'"', 11); if (end != std::wstring::npos) text = text.substr(11, end - 11); }
    const auto link = safe_link(text);
    if (!link.empty() && link[0] == L'#') {
        if (const auto* state = view(event->nmhdr.hwndFrom)) {
            for (const auto& anchor : state->anchors) if (anchor.id == link.substr(1)) {
                if (anchor.position >= 0) { SendMessageW(event->nmhdr.hwndFrom, EM_SETSEL, anchor.position, anchor.position); SendMessageW(event->nmhdr.hwndFrom, EM_SCROLLCARET, 0, 0); }
                break;
            }
        }
    } else if (!link.empty()) ShellExecuteW(event->nmhdr.hwndFrom, L"open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 1;
}
}
