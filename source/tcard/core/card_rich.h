#pragma once

#include <windows.h>
#include "card_clipboard.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tcard_rich {

// RichEdit uses 5pt = 100twips as a logical 100%; display zoom supplies
// the note base size. Never persist a zoomed or DPI-scaled run size.
enum Mark : unsigned { Bold = 1, Italic = 2, Underline = 4, Strike = 8 };
struct Style {
    int percent = 100;
    unsigned marks = 0;
    COLORREF color = 0;
    bool explicitColor = false;
    bool operator==(const Style&) const = default;
};
struct Run { std::wstring text; Style style; std::wstring image, alt; int width = 0, height = 0; };
struct Paragraph {
    std::vector<Run> runs;
    int list = 0;
    int number = 1;
    bool checkpoint = false;
    std::wstring checkpointStamp;
    int columns = 1;
};
struct Document { std::vector<Paragraph> paragraphs; };

inline std::wstring escape(std::wstring_view text)
{
    std::wstring result;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'"': result += L"&quot;"; break;
        case L'\'': result += L"&#39;"; break;
        default: result += c; break;
        }
    }
    return result;
}
inline std::wstring lower(std::wstring text)
{
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}
inline std::wstring trim(std::wstring text)
{
    const auto first = text.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}
inline std::wstring decode(std::wstring_view text)
{
    return tcard_clip::entities(std::wstring(text));
}
inline std::map<std::wstring, std::wstring> attributes(std::wstring_view text)
{
    std::map<std::wstring, std::wstring> result;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (iswspace(text[i]) || text[i] == L'/')) ++i;
        const size_t start = i;
        while (i < text.size() && !iswspace(text[i]) && text[i] != L'=' && text[i] != L'/') ++i;
        if (i == start) { ++i; continue; }
        const auto key = lower(std::wstring(text.substr(start, i - start)));
        while (i < text.size() && iswspace(text[i])) ++i;
        std::wstring value;
        if (i < text.size() && text[i] == L'=') {
            ++i; while (i < text.size() && iswspace(text[i])) ++i;
            const wchar_t quote = i < text.size() && (text[i] == L'"' || text[i] == L'\'') ? text[i++] : 0;
            const size_t begin = i;
            while (i < text.size() && (quote ? text[i] != quote : !iswspace(text[i]))) ++i;
            value = decode(text.substr(begin, i - begin));
            if (quote && i < text.size()) ++i;
        }
        result[key] = std::move(value);
    }
    return result;
}
inline bool parse_color(const std::wstring& text, COLORREF& color)
{
    static const std::map<std::wstring, COLORREF> named = {
        {L"black",RGB(0,0,0)}, {L"white",RGB(255,255,255)}, {L"red",RGB(255,0,0)},
        {L"blue",RGB(0,0,255)}, {L"green",RGB(0,128,0)}, {L"gray",RGB(128,128,128)}, {L"purple",RGB(128,0,128)}
    };
    auto value = lower(trim(text));
    if (auto it = named.find(value); it != named.end()) { color = it->second; return true; }
    if (value.starts_with(L"rgb(") || value.starts_with(L"rgba(")) {
        const auto open=value.find(L'(');const wchar_t* p=value.c_str()+open+1;int channels[3]{};
        for(int i=0;i<3;++i){
            while(*p==L' '||*p==L'\t'||*p==L',')++p;
            wchar_t* end=nullptr;double n=wcstod(p,&end);
            if(end==p||!std::isfinite(n))return false;
            p=end;if(*p==L'%'){n=n*255/100;++p;}
            channels[i]=std::clamp(static_cast<int>(std::lround(std::clamp(n,0.0,255.0))),0,255);
        }
        color=RGB(channels[0],channels[1],channels[2]);return true;
    }
    if (value.size() == 4 && value[0] == L'#') {
        value = L"#" + value.substr(1,1) + value.substr(1,1) + value.substr(2,1) + value.substr(2,1) + value.substr(3,1) + value.substr(3,1);
    }
    if (value.size() != 7 || value[0] != L'#' || !std::all_of(value.begin()+1, value.end(), [](wchar_t c){return iswxdigit(c) != 0;})) return false;
    const auto number = wcstoul(value.c_str()+1, nullptr, 16);
    color = RGB((number >> 16) & 255, (number >> 8) & 255, number & 255);
    return true;
}
inline std::wstring color_text(COLORREF color)
{
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}
inline void append(Paragraph& paragraph, std::wstring text, const Style& style)
{
    if (text.empty()) return;
    if (!paragraph.runs.empty() && paragraph.runs.back().image.empty() && paragraph.runs.back().style == style) paragraph.runs.back().text += text;
    else paragraph.runs.push_back({std::move(text),style});
}
inline Document plain(std::wstring_view source)
{
    Document result; result.paragraphs.emplace_back();
    for (size_t start = 0, i = 0; i <= source.size(); ++i) {
        if (i != source.size() && source[i] != L'\r' && source[i] != L'\n') continue;
        append(result.paragraphs.back(), std::wstring(source.substr(start, i-start)), {});
        if (i != source.size()) {
            if (source[i] == L'\r' && i + 1 < source.size() && source[i+1] == L'\n') ++i;
            result.paragraphs.emplace_back();
        }
        start = i+1;
    }
    return result;
}
inline std::wstring paragraph_text(const Paragraph& paragraph)
{
    std::wstring result;
    for (const auto& run : paragraph.runs) result += run.text;
    return result;
}
inline bool is_checkpoint(std::wstring_view text)
{
    if (text.size() != 25 || (text.substr(0,3) != L"-- " && text.substr(0,3) != L"\u2500\u2500 ") || text.substr(22) != L" --") return false;
    for (size_t i = 3; i < 22; ++i) {
        const wchar_t expected = i == 7 || i == 10 ? L'/' : i == 13 ? L' ' : i == 16 || i == 19 ? L':' : 0;
        if (expected ? text[i] != expected : !iswdigit(text[i])) return false;
    }
    return true;
}
inline std::wstring checkpoint_date(std::wstring_view stamp)
{
    if(stamp.size()<19)return {};
    std::wstring date(stamp.substr(0,19));date[4]=L'/';date[7]=L'/';date[10]=L' ';
    return is_checkpoint(L"-- "+date+L" --")?date:L"";
}
inline std::wstring checkpoint_stamp(const std::wstring& date)
{
    if(!is_checkpoint(L"-- "+date+L" --"))return {};
    std::wstring stamp=date;stamp[4]=L'-';stamp[7]=L'-';stamp[10]=L'T';
    SYSTEMTIME local{},utc{};
    local.wYear=static_cast<WORD>(wcstoul(date.substr(0,4).c_str(),nullptr,10));
    local.wMonth=static_cast<WORD>(wcstoul(date.substr(5,2).c_str(),nullptr,10));
    local.wDay=static_cast<WORD>(wcstoul(date.substr(8,2).c_str(),nullptr,10));
    local.wHour=static_cast<WORD>(wcstoul(date.substr(11,2).c_str(),nullptr,10));
    local.wMinute=static_cast<WORD>(wcstoul(date.substr(14,2).c_str(),nullptr,10));
    local.wSecond=static_cast<WORD>(wcstoul(date.substr(17,2).c_str(),nullptr,10));
    FILETIME lf{},uf{};
    if(TzSpecificLocalTimeToSystemTime(nullptr,&local,&utc)&&SystemTimeToFileTime(&local,&lf)&&SystemTimeToFileTime(&utc,&uf)){
        ULARGE_INTEGER l{},u{};l.LowPart=lf.dwLowDateTime;l.HighPart=lf.dwHighDateTime;
        u.LowPart=uf.dwLowDateTime;u.HighPart=uf.dwHighDateTime;
        const auto minutes=(static_cast<LONGLONG>(l.QuadPart)-static_cast<LONGLONG>(u.QuadPart))/600000000;
        const auto absolute=minutes<0?-minutes:minutes;wchar_t zone[16]{};
        swprintf_s(zone,L"%c%02lld:%02lld",minutes<0?L'-':L'+',absolute/60,absolute%60);stamp+=zone;
    }
    return stamp;
}
inline Style styled(Style style, const std::wstring& tag, const std::map<std::wstring,std::wstring>& attrs, double base)
{
    const int inheritedPercent=style.percent;
    if (tag == L"b" || tag == L"strong") style.marks |= Bold;
    if (tag == L"i" || tag == L"em") style.marks |= Italic;
    if (tag == L"u") style.marks |= Underline;
    if (tag == L"s" || tag == L"strike" || tag == L"del") style.marks |= Strike;
    if (auto it = attrs.find(L"color"); it != attrs.end()) { COLORREF color{}; if(parse_color(it->second,color)){style.color=color;style.explicitColor=true;} }
    if (auto it = attrs.find(L"style"); it != attrs.end()) {
        size_t start = 0;
        const auto css = lower(it->second);
        while (start < css.size()) {
            const auto end = css.find(L';',start), stop = end == std::wstring::npos ? css.size() : end;
            const auto colon = css.find(L':',start);
            if (colon < stop) {
                const auto key = trim(css.substr(start,colon-start)), value = trim(css.substr(colon+1,stop-colon-1));
                if (key == L"color") { COLORREF color{};if(parse_color(value,color)){style.color=color;style.explicitColor=true;} }
                else if (key == L"font-weight") {
                    if (value == L"bold" || value == L"bolder" || wcstol(value.c_str(),nullptr,10)>=600) style.marks |= Bold;
                    else if(value==L"normal"||value==L"400"||value==L"lighter")style.marks&=~Bold;
                }
                else if (key == L"font-style") {if(value==L"italic"||value==L"oblique")style.marks|=Italic;else if(value==L"normal")style.marks&=~Italic;}
                else if (key == L"text-decoration" || key == L"text-decoration-line") {
                    if (value.find(L"underline") != std::wstring::npos) style.marks |= Underline;
                    if (value.find(L"line-through") != std::wstring::npos) style.marks |= Strike;
                } else if (key == L"font-size") {
                    wchar_t* tail = nullptr; const double number = wcstod(value.c_str(),&tail);
                    if (tail != value.c_str() && std::isfinite(number) && number > 0) {
                        const std::wstring unit(tail);
                        double percent = 100;
                        if (unit == L"%") percent = number * inheritedPercent / 100;
                        else if (unit == L"pt") percent = number * 100 / base;
                        else if (unit == L"px") percent = number * 75 / base;
                        else if (unit == L"em") percent = number * inheritedPercent;
                        else if(unit==L"rem")percent=number*100;
                        else {start=stop+1;continue;}
                        style.percent = static_cast<int>(std::lround(std::clamp(percent,50.0,200.0)));
                    }
                }
            }
            start = stop+1;
        }
    }
    return style;
}
inline std::wstring css_value(const std::map<std::wstring,std::wstring>& attrs,const std::wstring& property)
{
    const auto found=attrs.find(L"style");if(found==attrs.end())return {};
    const auto css=lower(found->second);std::wstring result;
    for(size_t start=0;start<css.size();){
        const auto end=css.find(L';',start),stop=end==std::wstring::npos?css.size():end;
        const auto colon=css.find(L':',start);
        if(colon<stop&&trim(css.substr(start,colon-start))==property)result=trim(css.substr(colon+1,stop-colon-1));
        start=stop+1;
    }
    return result;
}
inline Document parse(std::wstring_view html, double base = 14)
{
    if (html.size() > 4 * 1024 * 1024) return {};
    Document result;result.paragraphs.emplace_back();
    struct Frame { std::wstring tag;Style style;int whitespace;bool canonical;int list;int number;size_t paragraph; };
    std::vector<Frame> frames;
    std::vector<int> lists,counters;
    Style style;int whitespace=0,list=0,number=1;bool canonical=false;
    std::wstring skipped;unsigned skipDepth=0;
    const auto finish=[&](bool force=false){
        if(force||!result.paragraphs.back().runs.empty())result.paragraphs.emplace_back();
    };
    const auto emit=[&](std::wstring text){
        auto& paragraph=result.paragraphs.back();
        if(paragraph.runs.empty()){paragraph.list=list;paragraph.number=number;}
        std::wstring clean;
        wchar_t previous=paragraph.runs.empty()?L'\r':paragraph.runs.back().text.back();
        for(size_t j=0;j<text.size();++j){
            wchar_t c=text[j];
            if(c==L'\r'){if(j+1<text.size()&&text[j+1]==L'\n')++j;c=L'\n';}
            const bool space=c==L' '||c==L'\t'||c==L'\n'||c==L'\f';
            if(canonical){clean+=c;previous=c;continue;}
            if(whitespace==1){if(c==L'\n')c=L'\v';clean+=c;previous=c;continue;}
            if(whitespace==2&&c==L'\n'){clean+=L'\v';previous=L'\v';continue;}
            if(space){if(previous!=L' '&&previous!=L'\r'&&previous!=L'\v'){clean+=L' ';previous=L' ';}}
            else{clean+=c;previous=c;}
        }
        append(paragraph,std::move(clean),style);
    };
    for(size_t i=0;i<html.size();){
        if(html[i]!=L'<'){
            const auto next=html.find(L'<',i),stop=next==std::wstring_view::npos?html.size():next;
            if(skipped.empty())emit(decode(html.substr(i,stop-i)));
            i=stop;continue;
        }
        if(html.substr(i,4)==L"<!--"){
            const auto end=html.find(L"-->",i+4);i=end==std::wstring_view::npos?html.size():end+3;continue;
        }
        size_t end=i+1;wchar_t quote=0;
        for(;end<html.size();++end){
            const wchar_t c=html[end];
            if(quote){if(c==quote)quote=0;}
            else if(c==L'"'||c==L'\'')quote=c;
            else if(c==L'>')break;
        }
        if(end==html.size())break;
        auto raw=trim(std::wstring(html.substr(i+1,end-i-1)));i=end+1;
        const bool closing=!raw.empty()&&raw[0]==L'/';
        if(closing)raw.erase(0,1);
        const auto space=raw.find_first_of(L" \t\r\n/");
        const auto tag=lower(raw.substr(0,space));
        const auto attrs=space==std::wstring::npos?std::map<std::wstring,std::wstring>{}:attributes(std::wstring_view(raw).substr(space));
        if(!skipped.empty()){
            if(tag==skipped){if(closing){if(--skipDepth==0)skipped.clear();}else ++skipDepth;}
            continue;
        }
        if(tag.empty()||tag[0]==L'!')continue;
        const bool unsafe=tag==L"script"||tag==L"style"||tag==L"iframe"||tag==L"object"||tag==L"svg"||tag==L"head"||tag==L"template";
        const bool hidden=attrs.contains(L"hidden")||css_value(attrs,L"display")==L"none"||css_value(attrs,L"visibility")==L"hidden";
        if(!closing&&(unsafe||hidden)){
            const bool single=raw.ends_with(L"/")||tag==L"img"||tag==L"br"||tag==L"hr"||tag==L"input"||tag==L"meta"||tag==L"link";
            if(!single){skipped=tag;skipDepth=1;}continue;
        }
        const bool cell=tag==L"td"||tag==L"th";
        const bool inCell=std::any_of(frames.begin(),frames.end(),[](const Frame& f){return f.tag==L"td"||f.tag==L"th";});
        const bool block=!inCell&&(tag==L"p"||tag==L"div"||tag==L"li"||tag==L"pre"||tag==L"blockquote"||
            tag==L"section"||tag==L"article"||tag==L"header"||tag==L"footer"||tag==L"tr"||tag==L"table"||
            tag==L"dt"||tag==L"dd"||(tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6'));
        if(tag==L"br"&&!closing){if(!attrs.contains(L"data-tcard-empty"))append(result.paragraphs.back(),L"\v",style);continue;}
        if(tag==L"hr"&&!closing){
            finish();
            if(auto marker=attrs.find(L"data-tcard-checkpoint");marker!=attrs.end()){
                const auto date=checkpoint_date(marker->second);
                if(!date.empty()){
                    auto& paragraph=result.paragraphs.back();
                    paragraph.checkpoint=true;paragraph.checkpointStamp=marker->second;
                    append(paragraph,L"\u2500\u2500 "+date+L" --",Style{});
                    finish();
                }
            }
            continue;
        }
        if(tag==L"img"&&!closing){
            if(attrs.contains(L"src")){
                const auto source=attrs.at(L"src");
                const auto alt=attrs.contains(L"alt")?attrs.at(L"alt"):L"Image";
                if(!source.empty()){
                    Run run{L"\xfffc",style,source,alt};
                    auto dimension=[&](const wchar_t* key){
                        auto item=css_value(attrs,key);
                        if(item.empty()&&attrs.contains(key))item=attrs.at(key);
                        if(item.empty()||item.find(L'%')!=item.npos)return 0;
                        wchar_t* tail=nullptr;double number=wcstod(item.c_str(),&tail);
                        if(tail==item.c_str()||!std::isfinite(number)||number<=0)return 0;
                        if(*tail&&std::wstring(tail)!=L"px")return 0;
                        return std::clamp(static_cast<int>((std::min)(8192.0,number)),1,8192);
                    };
                    run.width=dimension(L"width");run.height=dimension(L"height");
                    result.paragraphs.back().runs.push_back(std::move(run));
                }
            }else if(attrs.contains(L"alt"))emit(attrs.at(L"alt"));
            continue;
        }
        if(closing){
            if(tag==L"ul"||tag==L"ol"){if(!lists.empty()){lists.pop_back();counters.pop_back();}}
            size_t n=frames.size();while(n&&frames[n-1].tag!=tag)--n;
            if(!n)continue;
            const auto frame=frames[n-1];
            if(cell)append(result.paragraphs.back(),L"\t",style);
            if(tag==L"tr"&&!result.paragraphs.back().runs.empty()){
                auto& runs=result.paragraphs.back().runs;
                if(!runs.back().text.empty()&&runs.back().text.back()==L'\t')runs.back().text.pop_back();
                if(runs.back().text.empty())runs.pop_back();
                const auto rowText=paragraph_text(result.paragraphs.back());
                result.paragraphs.back().columns=std::clamp(1+static_cast<int>(std::count(rowText.begin(),rowText.end(),L'\t')),1,12);
            }
            if(block)finish((tag==L"p"||tag==L"li"||tag==L"pre")&&frame.paragraph==result.paragraphs.size()-1);
            style=frame.style;whitespace=frame.whitespace;canonical=frame.canonical;list=frame.list;number=frame.number;frames.resize(n-1);
            continue;
        }
        if(tag==L"input"||tag==L"meta"||tag==L"link"||tag==L"wbr"||tag==L"source")continue;
        if(frames.size()>=128)continue;
        if(block)finish();
        frames.push_back({tag,style,whitespace,canonical,list,number,result.paragraphs.size()-1});
        if(attrs.contains(L"data-tcard-rich"))canonical=true;
        if(tag==L"pre")whitespace=1;
        const auto ws=css_value(attrs,L"white-space");
        if(ws==L"pre"||ws==L"pre-wrap"||ws==L"break-spaces")whitespace=1;
        else if(ws==L"normal"||ws==L"nowrap")whitespace=0;
        else if(ws==L"pre-line")whitespace=2;
        if(tag==L"ul"||tag==L"ol"){
            finish();lists.push_back(tag==L"ul"?1:2);int first=1;
            if(attrs.contains(L"start"))first=std::clamp(static_cast<int>(wcstol(attrs.at(L"start").c_str(),nullptr,10)),1,100000);
            counters.push_back(first);
        }
        if(tag==L"li"){
            list=lists.empty()?1:lists.back();
            if(list==2){if(attrs.contains(L"value"))counters.back()=std::clamp(static_cast<int>(wcstol(attrs.at(L"value").c_str(),nullptr,10)),1,100000);number=counters.back()++;}
        }
        if(tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6'){
            style.marks|=Bold;
            if(css_value(attrs,L"font-size").empty())style.percent=tag==L"h1"?150:tag==L"h2"?125:110;
        }
        if(tag==L"th")style.marks|=Bold;
        style=styled(style,tag,attrs,base);
        if(attrs.contains(L"data-tcard-checkpoint"))result.paragraphs.back().checkpoint=true;
    }
    if(result.paragraphs.size()>1&&result.paragraphs.back().runs.empty())result.paragraphs.pop_back();
    for(auto& paragraph:result.paragraphs){
        paragraph.checkpoint=paragraph.checkpoint||is_checkpoint(paragraph_text(paragraph));

    }
    return result;
}
inline std::wstring serialize(const Document& doc, bool external = false, double base = 14, const std::wstring& family = L"Segoe UI", COLORREF ink = RGB(37,37,37))
{
    std::wstring safeFamily=family;
    if(safeFamily.find_first_of(L";\"'\\<>\r\n")!=std::wstring::npos)safeFamily=L"Segoe UI";
    std::wstring result = external ? L"<div style=\"white-space:pre-wrap;font-family:" + escape(safeFamily) + L";font-size:" + std::to_wstring(base) + L"pt\">" : L"<div data-tcard-rich=\"1\">";
    int openList=0;
    for(const auto& paragraph:doc.paragraphs) {
        if(openList!=paragraph.list) {
            if(openList)result+=openList==1?L"</ul>":L"</ol>";
            openList=paragraph.list;
            if(openList){
                const auto css=external?L" style=\"margin:0;padding-left:1.25em\"":L"";
                result+=openList==1?L"<ul"+std::wstring(css)+L">":L"<ol start=\""+std::to_wstring(paragraph.number)+L"\""+css+L">";
            }
        }
        if(paragraph.columns>1){
            result+=L"<table><tr>";
            Document cell;cell.paragraphs.emplace_back();
            auto flush=[&](){result+=L"<td>"+serialize(cell,external,base,family,ink)+L"</td>";cell.paragraphs[0].runs.clear();};
            for(const auto& run:paragraph.runs){
                if(!run.image.empty()){cell.paragraphs[0].runs.push_back(run);continue;}
                size_t start=0;
                for(size_t at=0;at<=run.text.size();++at)if(at==run.text.size()||run.text[at]==L'\t'){
                    append(cell.paragraphs[0],run.text.substr(start,at-start),run.style);
                    if(at<run.text.size())flush();start=at+1;
                }
            }
            flush();result+=L"</tr></table>";continue;
        }
        const auto tag=paragraph.list?L"li":L"p";
        if(paragraph.checkpoint&&!external){
            const auto text=paragraph_text(paragraph);
            if(is_checkpoint(text)){
                const auto date=text.substr(3,19);
                const auto stamp=checkpoint_date(paragraph.checkpointStamp)==date?paragraph.checkpointStamp:checkpoint_stamp(date);
                result+=L"<hr data-tcard-checkpoint=\""+escape(stamp)+L"\" title=\""+escape(date)+L"\">";
                continue;
            }
        }
        if(paragraph.checkpoint&&external){
            const auto text=paragraph_text(paragraph);
            const auto date=is_checkpoint(text)?text.substr(3,19):text;
            result+=L"<p style=\"margin:0.5em 0;border-bottom:1px solid #999\">" L"<span style=\"display:inline-block;width:2em;border-bottom:1px solid #999;vertical-align:middle\"></span> " + escape(date) + L"</p>";
            continue;
        }
        result+=L"<"+std::wstring(tag);
        if(paragraph.list==2)result+=L" value=\""+std::to_wstring(paragraph.number)+L"\"";
        if(paragraph.checkpoint)result+=L" data-tcard-checkpoint=\"1\"";
        if(external)result+=L" style=\"margin:0\"";
        result+=L">";
        for(const auto& run:paragraph.runs){
            if(!run.image.empty()){
                result+=L"<img src=\""+escape(run.image)+L"\" alt=\""+escape(run.alt)+L"\"";
                if(run.width)result+=L" width=\""+std::to_wstring(run.width)+L"\"";
                if(run.height)result+=L" height=\""+std::to_wstring(run.height)+L"\"";
                result+=L" style=\"max-width:100%;height:auto\">";
                continue;
            }
            std::wstring css;
            if(external)css=L"font-family:"+escape(safeFamily)+L";font-size:"+std::to_wstring(base*run.style.percent/100.0)+L"pt;"+
                L"color:"+color_text(run.style.explicitColor?run.style.color:ink)+L";"+
                L"font-weight:"+((run.style.marks&Bold)?L"700;":L"400;")+
                L"font-style:"+((run.style.marks&Italic)?L"italic;":L"normal;");
            else if(run.style.percent!=100)css=L"font-size:"+std::to_wstring(run.style.percent)+L"%;";
            if(!external&&run.style.explicitColor)css+=L"color:"+color_text(run.style.color)+L";";
            if(!css.empty())result+=L"<span style=\""+css+L"\">";
            if(run.style.marks&Bold)result+=L"<b>";
            if(run.style.marks&Italic)result+=L"<i>";
            if(run.style.marks&Underline)result+=L"<u>";
            if(run.style.marks&Strike)result+=L"<s>";
            for(wchar_t c:run.text)result+=c==L'\v'?L"<br>":escape(std::wstring_view(&c,1));
            if(run.style.marks&Strike)result+=L"</s>";
            if(run.style.marks&Underline)result+=L"</u>";
            if(run.style.marks&Italic)result+=L"</i>";
            if(run.style.marks&Bold)result+=L"</b>";
            if(!css.empty())result+=L"</span>";
        }
        if(external&&paragraph.runs.empty())result+=L"<br data-tcard-empty=\"1\">";
        result+=L"</"+std::wstring(tag)+L">";
    }
    if(openList)result+=openList==1?L"</ul>":L"</ol>";
    return result+L"</div>";
}
inline std::string escape_rtf(std::wstring_view text)
{
    std::string result;
    for(const wchar_t c:text){
        if(c==L'\\'||c==L'{'||c==L'}'){result+='\\';result+=static_cast<char>(c);}
        else if(c==L'\v'||c==L'\n')result+="\\line ";
        else if(c==L'\r')continue;
        else if(c==L'\t')result+="\\tab ";
        else if(c>=32&&c<127)result+=static_cast<char>(c);
        else result+="\\u"+std::to_string(static_cast<short>(c))+"?";
    }
    return result;
}
inline std::string serialize_rtf(const Document& doc,double base,const std::wstring& family,COLORREF ink,
    const std::function<std::vector<BYTE>(const std::wstring&)>& imageDib = {})
{
    const auto safeFamily=family.find_first_of(L";\r\n")!=std::wstring::npos?L"Segoe UI":family;
    std::vector<COLORREF> colors{ink};
    for(const auto& p:doc.paragraphs)for(const auto& r:p.runs)
        if(r.style.explicitColor&&std::find(colors.begin(),colors.end(),r.style.color)==colors.end())colors.push_back(r.style.color);
    std::string result="{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fnil "+escape_rtf(safeFamily)+";}}{\\colortbl;";
    for(const auto color:colors)result+="\\red"+std::to_string(GetRValue(color))+"\\green"+std::to_string(GetGValue(color))+"\\blue"+std::to_string(GetBValue(color))+";";
    result+="}\\viewkind4 ";
    bool first=true;
    for(const auto& p:doc.paragraphs){
        if(!first)result+="\\par ";first=false;
        result+="\\pard\\plain\\f0\\fs"+std::to_string(static_cast<int>(std::lround(base*2)))+"\\cf1 ";
        if(p.list){
            const auto indent=std::to_string(static_cast<int>(std::lround(base*25)));
            result+="\\li"+indent+"\\fi-"+indent+"\\tx"+indent+" ";
            result+=p.list==1?"\\u8226?\\tab ":std::to_string(p.number)+".\\tab ";
        }
        for(const auto& r:p.runs){
            if(!r.image.empty()){
                const auto bytes=imageDib?imageDib(r.image):std::vector<BYTE>{};
                if(bytes.size()>=sizeof(BITMAPINFOHEADER)){
                    BITMAPINFOHEADER info{};memcpy(&info,bytes.data(),sizeof(info));
                    LONG width=r.width?r.width:info.biWidth,height=r.height?r.height:info.biHeight;
                    if(r.width&&!r.height&&info.biWidth>0)height=MulDiv(info.biHeight,width,info.biWidth);
                    if(r.height&&!r.width&&info.biHeight>0)width=MulDiv(info.biWidth,height,info.biHeight);
                    result+="{\\pict\\dibitmap0\\picw"+std::to_string(info.biWidth)+"\\pich"+std::to_string(info.biHeight)+
                        "\\picwgoal"+std::to_string(width*15)+"\\pichgoal"+std::to_string(height*15)+" ";
                    constexpr char hex[]="0123456789ABCDEF";
                    for(BYTE byte:bytes){result+=hex[byte>>4];result+=hex[byte&15];}
                    result+="}";
                }else result+=escape_rtf(r.alt);
                continue;
            }
            const auto color=r.style.explicitColor?r.style.color:ink;
            const auto index=std::find(colors.begin(),colors.end(),color)-colors.begin()+1;
            result+="{\\f0\\fs"+std::to_string(static_cast<int>(std::lround(base*r.style.percent/50.0)))+"\\cf"+std::to_string(index);
            result+=(r.style.marks&Bold)?"\\b":"\\b0";
            result+=(r.style.marks&Italic)?"\\i":"\\i0";
            result+=(r.style.marks&Underline)?"\\ul":"\\ul0";
            result+=(r.style.marks&Strike)?"\\strike":"\\strike0";
            result+=" "+escape_rtf(r.text)+"}";
        }
    }
    return result+"}";
}
inline std::wstring text(const Document& doc)
{
    std::wstring result;
    bool first = true;
    for(const auto& paragraph:doc.paragraphs) {
        if(!first)result+=L"\r\n";
        first = false;
        if(paragraph.list)result+=paragraph.list==1?L"- ":std::to_wstring(paragraph.number)+L". ";
        std::wstring value;
        for(const auto& run:paragraph.runs)value+=run.image.empty()?run.text:run.alt;
        for(auto& c:value)if(c==L'\v')c=L'\n';
        result+=value;
    }
    return result;
}
} // namespace tcard_rich
