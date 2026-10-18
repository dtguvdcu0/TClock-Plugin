#pragma once

#include "card_document.h"
#include "../core/card_rich.h"
#include <tom.h>
#include "card_editor.h"
#include <oleauto.h>
#include <wrl.h>
#include <cstdio>
#include <limits>
#include "../core/card_download.h"
#include <memory>
#include <thread>

#pragma comment(lib, "oleaut32.lib")

namespace tcard_rich_edit {
using Microsoft::WRL::ComPtr;
inline constexpr UINT_PTR kSubclass = 0x54435249;
struct PasteJob {
    tcard_rich::Document doc;
    std::atomic_bool done{false},cancelled{false};
    CHARRANGE selection{};
    std::thread worker;
};
inline constexpr UINT_PTR kPasteTimer=0x54435049;
struct State {
    bool logical = false;
    bool loading = false;
    bool editable = false;
    double base = 14;
    std::wstring family = L"Segoe UI";
    COLORREF paper = RGB(255,255,255);
    COLORREF ink = RGB(37,37,37);
    std::map<DWORD,tcard_rich::Run> images;
    DWORD nextImage=1;
    std::map<std::wstring,std::wstring> checkpoints;
    std::shared_ptr<PasteJob> pasteJob;
    bool pasteFailed=false;
    ~State(){if(pasteJob){pasteJob->cancelled=true;if(pasteJob->worker.joinable())pasteJob->worker.join();}}
};
inline LRESULT CALLBACK editor_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
inline State* state(HWND control)
{
    DWORD_PTR value=0;
    return GetWindowSubclass(control,editor_proc,kSubclass,&value)?reinterpret_cast<State*>(value):nullptr;
}
inline std::wstring raw_text(HWND control)
{
    GETTEXTLENGTHEX length{GTL_PRECISE|GTL_NUMCHARS,1200};
    const auto count=SendMessageW(control,EM_GETTEXTLENGTHEX,reinterpret_cast<WPARAM>(&length),0);
    if(count<0||count>4*1024*1024)return {};
    std::wstring result(static_cast<size_t>(count)+1,L'\0');
    GETTEXTEX get{static_cast<DWORD>(result.size()*sizeof(wchar_t)),GT_RAWTEXT,1200,nullptr,nullptr};
    const auto read=SendMessageW(control,EM_GETTEXTEX,reinterpret_cast<WPARAM>(&get),reinterpret_cast<LPARAM>(result.data()));
    result.resize(read>0?static_cast<size_t>(read):0);
    return result;
}
inline ComPtr<ITextDocument> text_document(HWND control)
{
    ComPtr<IRichEditOle> ole;
    if(!SendMessageW(control,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(ole.GetAddressOf())))return {};
    ComPtr<ITextDocument> document;
    if(ole)ole.As(&document);
    return document;
}
inline int selection_scale(HWND control)
{
    CHARRANGE selected{};SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selected));
    // A detached TOM range preserves the active endpoint and mouse selection anchor.
    if(selected.cpMax>selected.cpMin){
        GETTEXTLENGTHEX length{GTL_PRECISE|GTL_NUMCHARS,1200};
        const LONG end=(std::max)(1L,(std::min)(selected.cpMax,static_cast<LONG>(SendMessageW(control,EM_GETTEXTLENGTHEX,reinterpret_cast<WPARAM>(&length),0))));
        const auto tom=text_document(control);ComPtr<ITextRange> range;ComPtr<ITextFont> font;
        float size=5;
        if(tom&&SUCCEEDED(tom->Range(end-1,end,range.GetAddressOf()))&&range&&
           SUCCEEDED(range->GetFont(font.GetAddressOf()))&&font&&SUCCEEDED(font->GetSize(&size)))
            return std::clamp(static_cast<int>(std::lround(size*20)),50,200);
        return 100;
    }
    CHARFORMAT2W face{};face.cbSize=sizeof(face);
    SendMessageW(control,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face));
    return std::clamp(static_cast<int>(face.yHeight),50,200);
}
inline bool set_zoom(HWND control,double base)
{
    // RichEdit accepts rational zooms; use a close bounded fraction for decimal bases.
    int numerator=1,denominator=1;double error=1e9;
    const double ratio=base/5.0;
    for(int d=1;d<=64;++d){
        const int n=static_cast<int>(std::lround(ratio*d));
        if(n<1||n>64)continue;
        const double difference=std::abs(static_cast<double>(n)/d-ratio);
        if(difference<error){error=difference;numerator=n;denominator=d;}
    }
    return SendMessageW(control,EM_SETZOOM,numerator,denominator)!=0;
}
inline void configure(HWND control,bool logical,double base,const std::wstring& family,COLORREF paper,COLORREF ink,bool editable)
{
    if(logical)if(auto* view=tcard_md::view(control)){view->enabled=false;view->valid=false;}
    auto* value=state(control);
    if(!value){
        value=new(std::nothrow) State;
        if(!value||!SetWindowSubclass(control,editor_proc,kSubclass,reinterpret_cast<DWORD_PTR>(value))){delete value;return;}
    }
    if(logical&&value->logical&&value->family!=family&&!value->loading){
        CHARFORMAT2W face{};face.cbSize=sizeof(face);face.dwMask=CFM_FACE;
        wcsncpy_s(face.szFaceName,family.c_str(),_TRUNCATE);
        SendMessageW(control,EM_STOPGROUPTYPING,0,0);
        SendMessageW(control,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&face));
        SendMessageW(control,EM_STOPGROUPTYPING,0,0);
    }
    value->logical=logical;value->base=base;value->family=family;value->paper=paper;value->ink=ink;value->editable=editable;
    SendMessageW(control,EM_SETBKGNDCOLOR,0,paper);
    SendMessageW(control,EM_SETEVENTMASK,0,SendMessageW(control,EM_GETEVENTMASK,0,0)|ENM_SELCHANGE|ENM_CHANGE);
    if(logical&&editable)RevokeDragDrop(control);
    if(logical)set_zoom(control,base);else SendMessageW(control,EM_SETZOOM,0,0);
}
inline void detach(HWND control)
{
 if(auto* value=state(control)){RemoveWindowSubclass(control,editor_proc,kSubclass);delete value;}
 SendMessageW(control,EM_SETZOOM,0,0);
}
inline void columns(HWND control,const tcard_rich::Paragraph& paragraph)
{
    const auto* value=state(control);RECT area{};GetClientRect(control,&area);
    const double zoom=value&&value->logical?value->base/5.0:1.0;
    PARAFORMAT2 para{};para.cbSize=sizeof(para);para.dwMask=PFM_TABSTOPS;
    para.cTabCount=static_cast<SHORT>(std::clamp(paragraph.columns-1,0,11));
    const LONG width=static_cast<LONG>(MulDiv((std::max)(120L,area.right-24),1440,GetDpiForWindow(control))/zoom);
    for(int i=0;i<para.cTabCount;++i)para.rgxTabs[i]=width*(i+1)/paragraph.columns;
    SendMessageW(control,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));
}
inline bool insert_images(HWND control,const tcard_rich::Document& doc,LONG offset,bool undo)
{
    if(auto* value=state(control))for(const auto& paragraph:doc.paragraphs)
        if(paragraph.checkpoint&&!paragraph.checkpointStamp.empty())
            value->checkpoints[tcard_rich::paragraph_text(paragraph)]=paragraph.checkpointStamp;
    auto* value=state(control);if(!value)return false;
    RECT area{};GetClientRect(control,&area);
    const double zoom=value->logical?value->base/5.0:1.0;
    const LONG available=static_cast<LONG>(MulDiv((std::max)(1L,area.right-12),2540,GetDpiForWindow(control))/zoom);
    for(const auto& paragraph:doc.paragraphs){
        for(const auto& run:paragraph.runs){
            if(!run.image.empty()){
                auto picture=tcard_image::decode(run.image);
                if(picture.dib.empty())return false;
                const double naturalWidth=picture.width,naturalHeight=picture.height;
                double width=run.width?run.width*2540.0/96.0:naturalWidth;
                double height=run.height?run.height*2540.0/96.0:naturalHeight;
                if(run.width&&!run.height)height=naturalHeight*width/naturalWidth;
                if(run.height&&!run.width)width=naturalWidth*height/naturalHeight;
                picture.width=(std::max)(1L,static_cast<LONG>(width/zoom));
                picture.height=(std::max)(1L,static_cast<LONG>(height/zoom));
                const DWORD identity=value->nextImage++;
                value->images.emplace(identity,run);
                if(!tcard_image::place(control,offset,picture,paragraph.columns>1?available/paragraph.columns:available,identity,undo))return false;
            }
            offset+=static_cast<LONG>(run.text.size());
        }
        ++offset;
    }
    return true;
}
inline CHARFORMAT2W format(const tcard_rich::Style& style,const std::wstring& family)
{
    CHARFORMAT2W value{};value.cbSize=sizeof(value);
    value.dwMask=CFM_SIZE|CFM_BOLD|CFM_ITALIC|CFM_UNDERLINE|CFM_STRIKEOUT|CFM_COLOR|CFM_FACE|CFM_HIDDEN|CFM_LINK|CFM_BACKCOLOR;
    value.yHeight=style.percent;
    value.dwEffects=CFE_AUTOBACKCOLOR|(style.explicitColor?0:CFE_AUTOCOLOR);
    if(style.marks&tcard_rich::Bold)value.dwEffects|=CFE_BOLD;
    if(style.marks&tcard_rich::Italic)value.dwEffects|=CFE_ITALIC;
    if(style.marks&tcard_rich::Underline)value.dwEffects|=CFE_UNDERLINE;
    if(style.marks&tcard_rich::Strike)value.dwEffects|=CFE_STRIKEOUT;
    value.crTextColor=style.color;
    wcsncpy_s(value.szFaceName,family.c_str(),_TRUNCATE);
    return value;
}
inline bool load(HWND control,const tcard_rich::Document& doc,double base,const std::wstring& family,COLORREF paper,COLORREF ink,bool editable)
{
    if(doc.paragraphs.empty()||!text_document(control))return false;
    configure(control,true,base,family,paper,ink,editable);
    auto* value=state(control);if(!value)return false;
    if(value->pasteJob){value->pasteJob->cancelled=true;if(value->pasteJob->worker.joinable())value->pasteJob->worker.join();value->pasteJob.reset();KillTimer(control,kPasteTimer);EnableWindow(control,TRUE);}
    value->loading=true;
    struct LoadingScope{State* value;~LoadingScope(){value->loading=false;}} loading{value};
    const bool readOnly=(GetWindowLongPtrW(control,GWL_STYLE)&ES_READONLY)!=0;
    SendMessageW(control,EM_SETREADONLY,FALSE,0);
    std::wstring text;
    for(size_t i=0;i<doc.paragraphs.size();++i){
        if(i)text+=L'\r';
        for(const auto& run:doc.paragraphs[i].runs)text+=run.image.empty()?run.text:L"\u25a1";
    }
    SetWindowTextW(control,L"");
    SendMessageW(control,EM_SETTEXTMODE,TM_RICHTEXT|TM_MULTILEVELUNDO,0);
    auto defaults=format({},family);defaults.crTextColor=ink;defaults.dwEffects&=~CFE_AUTOCOLOR;
    SendMessageW(control,EM_SETCHARFORMAT,0,reinterpret_cast<LPARAM>(&defaults));
    SETTEXTEX set{ST_DEFAULT,1200};
    SendMessageW(control,EM_SETTEXTEX,reinterpret_cast<WPARAM>(&set),reinterpret_cast<LPARAM>(text.c_str()));
    if(raw_text(control)!=text){SendMessageW(control,EM_SETREADONLY,readOnly,0);return false;}
    SendMessageW(control,EM_SETSEL,0,-1);
    auto reset=format({},family);reset.crTextColor=ink;reset.dwEffects&=~CFE_AUTOCOLOR;
    SendMessageW(control,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&reset));
    PARAFORMAT2 baseline{};baseline.cbSize=sizeof(baseline);
    baseline.dwMask=PFM_NUMBERING|PFM_NUMBERINGTAB|PFM_STARTINDENT|PFM_OFFSET|PFM_SPACEAFTER|PFM_ALIGNMENT;
    baseline.wAlignment=PFA_LEFT;
    SendMessageW(control,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&baseline));
    LONG position=0;
    for(const auto& paragraph:doc.paragraphs){
        const LONG begin=position;
        for(const auto& run:paragraph.runs){
            CHARRANGE selection{position,position+static_cast<LONG>(run.text.size())};
            SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
            auto face=format(run.style,family);
            if(!run.style.explicitColor){face.crTextColor=ink;face.dwEffects&=~CFE_AUTOCOLOR;}
            if(!SendMessageW(control,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face)))return false;
            position=selection.cpMax;
        }
        CHARRANGE selection{begin,position};SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
        PARAFORMAT2 para{};para.cbSize=sizeof(para);para.dwMask=PFM_NUMBERING|PFM_NUMBERINGSTART|PFM_NUMBERINGTAB|PFM_STARTINDENT|PFM_OFFSET|PFM_SPACEAFTER;
        para.wNumberingStart=static_cast<WORD>(paragraph.number);
        para.wNumbering=paragraph.list==1?PFN_BULLET:paragraph.list==2?PFN_ARABIC:0;
        if(paragraph.list){para.dxStartIndent=0;para.dxOffset=125;para.wNumberingTab=125;}
        para.dySpaceAfter=25;
        SendMessageW(control,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));
        columns(control,paragraph);
        ++position;
    }
    value->images.clear();
    if(!insert_images(control,doc,0,false)){SendMessageW(control,EM_SETREADONLY,readOnly,0);return false;}
    SendMessageW(control,EM_SETSEL,0,0);
    SendMessageW(control,EM_EMPTYUNDOBUFFER,0,0);
    SendMessageW(control,EM_SETMODIFY,FALSE,0);
    SendMessageW(control,EM_SETREADONLY,readOnly,0);
    value->loading=false;set_zoom(control,base);InvalidateRect(control,nullptr,FALSE);
    return true;
}

struct ReadStream {
    std::string bytes;
    size_t offset=0;
    static constexpr size_t limit=8*1024*1024;
    static DWORD CALLBACK Write(DWORD_PTR cookie,LPBYTE buffer,LONG count,LONG* written)
    {
        auto& stream=*reinterpret_cast<ReadStream*>(cookie);*written=0;
        if(count<0||static_cast<size_t>(count)>limit-stream.bytes.size())return 1;
        try{stream.bytes.append(reinterpret_cast<const char*>(buffer),count);*written=count;return 0;}catch(...){return 1;}
    }
    static DWORD CALLBACK Read(DWORD_PTR cookie,LPBYTE buffer,LONG count,LONG* read)
    {
        auto& stream=*reinterpret_cast<ReadStream*>(cookie);*read=0;if(count<0)return 1;
        const auto size=(std::min)(static_cast<size_t>(count),stream.bytes.size()-stream.offset);
        memcpy(buffer,stream.bytes.data()+stream.offset,size);stream.offset+=size;*read=static_cast<LONG>(size);return 0;
    }
};
inline bool load_preview(HWND control,const std::wstring& source,double base,const std::wstring& family,COLORREF paper,COLORREF ink)
{
    struct ExactSize {LONG first,last,height;};
    struct Entry {
        std::wstring source,family;std::string rtf;std::vector<ExactSize> precise;
        std::map<std::wstring,std::wstring> checkpoints;
        double base=0;COLORREF paper=0,ink=0;LONG width=0;UINT dpi=0;size_t bytes=0;
    };
    // Bounded per-UI-thread LRU. Nothing is persisted; source, style, width and DPI are exact keys.
    static thread_local std::vector<Entry> cache;
    RECT area{};GetClientRect(control,&area);const auto dpi=GetDpiForWindow(control);
    const auto found=std::find_if(cache.begin(),cache.end(),[&](const Entry& e){return e.source==source&&e.base==base&&e.family==family&&e.paper==paper&&e.ink==ink&&e.width==area.right&&e.dpi==dpi;});
    if(found!=cache.end()){
        std::rotate(cache.begin(),found,found+1);const auto& entry=cache.front();
        configure(control,true,base,family,paper,ink,false);auto* value=state(control);
        if(value&&!value->pasteJob){
            value->loading=true;const bool readonly=(GetWindowLongPtrW(control,GWL_STYLE)&ES_READONLY)!=0;
            SendMessageW(control,EM_SETREADONLY,FALSE,0);SetWindowTextW(control,L"");SendMessageW(control,EM_SETTEXTMODE,TM_RICHTEXT|TM_MULTILEVELUNDO,0);
            ReadStream data{entry.rtf};EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&data),0,ReadStream::Read};
            SendMessageW(control,EM_STREAMIN,SF_RTF,reinterpret_cast<LPARAM>(&stream));
            // RichEdit RTF rounds font sizes to half-points; restore logical percentage precision.
            if(!stream.dwError)for(const auto& size:entry.precise){
                SendMessageW(control,EM_SETSEL,size.first,size.last);CHARFORMAT2W face{};face.cbSize=sizeof(face);face.dwMask=CFM_SIZE;face.yHeight=size.height;
                if(!SendMessageW(control,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face))){stream.dwError=1;break;}
            }
            value->images.clear();value->checkpoints=entry.checkpoints;
            SendMessageW(control,EM_SETSEL,0,0);SendMessageW(control,EM_EMPTYUNDOBUFFER,0,0);SendMessageW(control,EM_SETMODIFY,FALSE,0);
            SendMessageW(control,EM_SETREADONLY,readonly,0);value->loading=false;set_zoom(control,base);
            if(!stream.dwError){InvalidateRect(control,nullptr,FALSE);return true;}
        }
        cache.erase(cache.begin());
    }
    const auto doc=tcard_rich::parse(source);if(!load(control,doc,base,family,paper,ink,false))return false;
    // OLE images require their original object identities; retain the existing image loader.
    for(const auto& paragraph:doc.paragraphs)for(const auto& run:paragraph.runs)if(!run.image.empty())return true;
    if(source.size()>ReadStream::limit/sizeof(wchar_t))return true;
    ReadStream data;EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&data),0,ReadStream::Write};
    SendMessageW(control,EM_STREAMOUT,SF_RTF,reinterpret_cast<LPARAM>(&stream));if(stream.dwError)return true;
    Entry entry;entry.source=source;entry.family=family;entry.rtf=std::move(data.bytes);entry.base=base;entry.paper=paper;entry.ink=ink;entry.width=area.right;entry.dpi=dpi;
    entry.bytes=(source.size()+family.size())*sizeof(wchar_t)+entry.rtf.size();
    for(const auto& paragraph:doc.paragraphs)if(paragraph.checkpoint&&!paragraph.checkpointStamp.empty()){
        auto text=tcard_rich::paragraph_text(paragraph);entry.bytes+=(text.size()+paragraph.checkpointStamp.size())*sizeof(wchar_t);entry.checkpoints.emplace(std::move(text),paragraph.checkpointStamp);}
    LONG position=0;
    for(const auto& paragraph:doc.paragraphs){for(const auto& run:paragraph.runs){
        const LONG end=position+static_cast<LONG>(run.text.size());
        if(end>position&&run.style.percent%10)entry.precise.push_back({position,end,run.style.percent});position=end;
    }++position;}
    entry.bytes+=entry.precise.size()*sizeof(ExactSize);
    if(entry.bytes>ReadStream::limit)return true;
    size_t bytes=entry.bytes;for(const auto& item:cache)bytes+=item.bytes;
    while(!cache.empty()&&(cache.size()>=8||bytes>ReadStream::limit)){bytes-=cache.back().bytes;cache.pop_back();}
    cache.insert(cache.begin(),std::move(entry));return true;
}

inline tcard_rich::Document document(HWND control,bool selectionOnly=false,bool* succeeded=nullptr)
{
    if(succeeded)*succeeded=false;
    tcard_rich::Document result;
    const auto text=raw_text(control);auto* value=state(control);
    const auto tom=text_document(control);if(!tom)return result;
    CHARRANGE selected{0,static_cast<LONG>(text.size())};
    if(selectionOnly){SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selected));if(selected.cpMin==selected.cpMax)return result;}
    selected.cpMin=(std::max)(0L,selected.cpMin);selected.cpMax=(std::min)(static_cast<LONG>(text.size()),selected.cpMax);
    int number=0;
    if(selectionOnly)for(size_t begin=0;begin<static_cast<size_t>(selected.cpMin);){
        const auto end=text.find(L'\r',begin);
        if(end==std::wstring::npos||end>=static_cast<size_t>(selected.cpMin))break;
        ComPtr<ITextRange> range;ComPtr<ITextPara> para;LONG type=tomListNone,start=1;
        if(FAILED(tom->Range(static_cast<LONG>(begin),static_cast<LONG>(begin),range.GetAddressOf()))||!range)return {};
        range->GetPara(para.GetAddressOf());if(!para)return {};
        para->GetListType(&type);para->GetListStart(&start);
        if(type!=tomListNone&&type!=tomListBullet)number=number?number+1:(std::max)(1L,start);else number=0;
        begin=end+1;
    }
    std::map<LONG,tcard_rich::Run> images;
    if(value&&!value->images.empty()){
        ComPtr<IRichEditOle> ole;
        SendMessageW(control,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(ole.GetAddressOf()));
        if(ole)for(LONG i=0;i<ole->GetObjectCount();++i){
            REOBJECT object{};object.cbStruct=sizeof(object);
            if(SUCCEEDED(ole->GetObject(i,&object,REO_GETOBJ_NO_INTERFACES))){
                const auto found=value->images.find(object.dwUser);
                if(found!=value->images.end())images.emplace(object.cp,found->second);
            }
        }
    }
    LONG position=selected.cpMin;
    while(position<=selected.cpMax){
        const auto found=text.find(L'\r',static_cast<size_t>(position));
        const LONG end=(std::min)(selected.cpMax,found==std::wstring::npos?static_cast<LONG>(text.size()):static_cast<LONG>(found));
        tcard_rich::Paragraph paragraph;
        ComPtr<ITextRange> paragraphRange;
        if(FAILED(tom->Range(position,position,paragraphRange.GetAddressOf())))return {};
        if(paragraphRange){
            ComPtr<ITextPara> para;paragraphRange->GetPara(para.GetAddressOf());
            if(para){
                LONG type=0,start=1,tabs=0;para->GetListType(&type);para->GetListStart(&start);
                para->GetTabCount(&tabs);paragraph.columns=std::clamp(static_cast<int>(tabs)+1,1,12);
                paragraph.list=type==tomListBullet?1:type!=tomListNone?2:0;
                if(paragraph.list==2){number=number?number+1:(std::max)(1L,start);paragraph.number=number;}else number=0;
            }
        }
        while(position<end){
            if(auto image=images.find(position);image!=images.end()){paragraph.runs.push_back(image->second);++position;continue;}
            ComPtr<ITextRange> range;tom->Range(position,position+1,range.GetAddressOf());if(!range)return {};
            LONG ignored=0,next=position+1;range->Expand(tomCharFormat,&ignored);range->GetEnd(&next);
            next=(std::max)(position+1,(std::min)(next,end));
            if(auto image=images.upper_bound(position);image!=images.end())next=(std::min)(next,image->first);
            range->SetRange(position,next);
            ComPtr<ITextFont> font;range->GetFont(font.GetAddressOf());
            tcard_rich::Style style;
            if(!font)return {};
            if(font){
                float size=5;LONG bold=0,italic=0,underline=0,strike=0,color=tomAutoColor;
                if(FAILED(font->GetSize(&size)))return {};font->GetBold(&bold);font->GetItalic(&italic);font->GetUnderline(&underline);font->GetStrikeThrough(&strike);font->GetForeColor(&color);
                const double base=value?value->base:14;
                style.percent=static_cast<int>(std::lround((value&&value->logical)?size*20:size*100/base));
                if(bold==tomTrue)style.marks|=tcard_rich::Bold;
                if(italic==tomTrue)style.marks|=tcard_rich::Italic;
                if(underline!=tomNone&&underline!=tomUndefined)style.marks|=tcard_rich::Underline;
                if(strike==tomTrue)style.marks|=tcard_rich::Strike;
                style.color=static_cast<COLORREF>(color);
                style.explicitColor=color!=tomAutoColor&&(!value||style.color!=value->ink);
            }
            tcard_rich::append(paragraph,text.substr(position,next-position),style);
            position=next;
        }
        const auto content=tcard_rich::paragraph_text(paragraph);
        if(paragraph.columns>1)paragraph.columns=std::clamp(1+static_cast<int>(std::count(content.begin(),content.end(),L'\t')),1,12);
        paragraph.checkpoint=tcard_rich::is_checkpoint(content);
        if(paragraph.checkpoint){
            paragraph.list=0;
            if(value){
                auto& stamp=value->checkpoints[content];
                if(stamp.empty())stamp=tcard_rich::checkpoint_stamp(content.substr(3,19));
                paragraph.checkpointStamp=stamp;
            }
        }
        result.paragraphs.push_back(std::move(paragraph));
        if(end>=selected.cpMax)break;
        position=end+1;
    }
    if(succeeded)*succeeded=true;
    return result;
}
inline void changed(HWND control)
{
    if(auto* value=state(control);value&&!value->loading){
        SendMessageW(control,EM_SETMODIFY,TRUE,0);
        SendMessageW(GetParent(control),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(control),EN_CHANGE),reinterpret_cast<LPARAM>(control));
    }
}
inline bool apply(HWND control,unsigned mark,int percent=0,const COLORREF* color=nullptr)
{
    auto* value=state(control);if(!value||!value->logical||tcard_edit::composing(control))return false;
    CHARRANGE selection{};SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    if(selection.cpMin==selection.cpMax)return false;
    CHARFORMAT2W face{};face.cbSize=sizeof(face);
    if(percent){face.dwMask=CFM_SIZE;face.yHeight=std::clamp(percent,50,200);}
    else if(color){face.dwMask=CFM_COLOR;face.crTextColor=*color;}
    else{
        DWORD mask=mark==tcard_rich::Bold?CFM_BOLD:mark==tcard_rich::Italic?CFM_ITALIC:mark==tcard_rich::Underline?CFM_UNDERLINE:CFM_STRIKEOUT;
        DWORD effect=mark==tcard_rich::Bold?CFE_BOLD:mark==tcard_rich::Italic?CFE_ITALIC:mark==tcard_rich::Underline?CFE_UNDERLINE:CFE_STRIKEOUT;
        SendMessageW(control,EM_GETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face));
        const bool active=(face.dwMask&mask)&&(face.dwEffects&effect);
        face.dwMask=mask;face.dwEffects=active?0:effect;
    }
    SendMessageW(control,EM_STOPGROUPTYPING,0,0);
    const bool result=SendMessageW(control,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face))!=0;
    SendMessageW(control,EM_STOPGROUPTYPING,0,0);if(result)changed(control);SetFocus(control);return result;
}
inline void bullet(HWND control)
{
    CHARRANGE selection{};SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));if(selection.cpMin==selection.cpMax)return;
    PARAFORMAT2 para{};para.cbSize=sizeof(para);SendMessageW(control,EM_GETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));
    const bool active=para.wNumbering==PFN_BULLET;
    para.dwMask=PFM_NUMBERING|PFM_NUMBERINGTAB|PFM_STARTINDENT|PFM_OFFSET;para.wNumbering=active?0:PFN_BULLET;para.dxStartIndent=0;para.dxOffset=active?0:125;para.wNumberingTab=active?0:125;
    SendMessageW(control,EM_STOPGROUPTYPING,0,0);SendMessageW(control,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));SendMessageW(control,EM_STOPGROUPTYPING,0,0);changed(control);SetFocus(control);
}
inline std::string utf8(std::wstring_view text)
{
    if(text.empty())return {};
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!count)return {};
    std::string result(count,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);return result;
}
inline std::string clipboard_html(const std::wstring& fragment)
{
    const auto body=utf8(fragment);
    const std::string prefix="<html><body><!--StartFragment-->",suffix="<!--EndFragment--></body></html>";
    const std::string templateHeader="Version:1.0\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    const size_t start=templateHeader.size(),first=start+prefix.size(),last=first+body.size(),end=last+suffix.size();
    char header[256]{};
    sprintf_s(header,"Version:1.0\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n",start,end,first,last);
    return std::string(header)+prefix+body+suffix;
}
inline bool copy(HWND control,bool all=false,bool plainOnly=false,const std::wstring& title=L"")
{
    auto* value=state(control);if(!value)return false;
    bool ok=false;auto doc=document(control,!all,&ok);if(!ok||doc.paragraphs.empty())return false;
    if(all&&!title.empty()){tcard_rich::Paragraph heading;tcard_rich::Style style;style.marks=tcard_rich::Bold;tcard_rich::append(heading,title,style);doc.paragraphs.insert(doc.paragraphs.begin(),std::move(heading));}
    auto text=tcard_rich::text(doc);
    std::wstring normalized;for(size_t i=0;i<text.size();++i){if(text[i]==L'\n'&&(i==0||text[i-1]!=L'\r'))normalized+=L'\r';normalized+=text[i];}
    const auto html=clipboard_html(tcard_rich::serialize(doc,true,value->base,value->family,value->ink));
    const auto rtf=tcard_rich::serialize_rtf(doc,value->base,value->family,value->ink,[](const std::wstring& uri){return tcard_image::decode(uri).dib;});
    const UINT htmlId=plainOnly?0:RegisterClipboardFormatW(L"HTML Format");
    const UINT rtfId=plainOnly?0:RegisterClipboardFormatW(L"Rich Text Format");
    if(!plainOnly&&(!htmlId||!rtfId))return false;
    struct Item { UINT format; HGLOBAL memory; };
    std::vector<Item> items;
    const auto prepare=[&](UINT format,const void* bytes,size_t count){
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,count);if(!memory)return false;
        auto* pointer=GlobalLock(memory);if(!pointer){GlobalFree(memory);return false;}
        memcpy(pointer,bytes,count);GlobalUnlock(memory);items.push_back({format,memory});return true;
    };
    const auto release=[&]{for(const auto& item:items)if(item.memory)GlobalFree(item.memory);};
    if(!prepare(CF_UNICODETEXT,normalized.c_str(),(normalized.size()+1)*sizeof(wchar_t))||
       (!plainOnly&&(!prepare(htmlId,html.c_str(),html.size()+1)||!prepare(rtfId,rtf.c_str(),rtf.size()+1)))){release();return false;}
    if(!OpenClipboard(control)){release();return false;}
    bool result=EmptyClipboard()!=FALSE;
    if(result)for(auto& item:items){
        if(SetClipboardData(item.format,item.memory))item.memory=nullptr;else result=false;
    }
    CloseClipboard();release();return result;
}
inline bool copy_text(HWND control,const std::wstring& text)
{
    if(text.empty()||text.size()>4*1024*1024)return false;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));if(!memory)return false;
    auto* output=GlobalLock(memory);if(!output){GlobalFree(memory);return false;}
    memcpy(output,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
    if(!OpenClipboard(control)){GlobalFree(memory);return false;}
    bool ok=false;
    if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory)){memory=nullptr;ok=true;}
    CloseClipboard();if(memory)GlobalFree(memory);return ok;
}
inline std::wstring html_fragment(const std::string& payload,std::wstring* origin=nullptr)
{
    const size_t headerEnd=(std::min)(payload.find('<'),size_t(4096));
    const auto field=[&](const char* key)->std::string{
        const auto at=payload.find(key);if(at==std::string::npos||at>=headerEnd)return {};
        const auto first=at+strlen(key),last=payload.find_first_of("\r\n",first);
        const auto raw=payload.substr(first,last-first);const auto begin=raw.find_first_not_of(" \t");
        return begin==std::string::npos?std::string{}:raw.substr(begin,raw.find_last_not_of(" \t")-begin+1);
    };
    const auto number=[&](const char* key)->size_t{
        const auto raw=field(key);if(raw.empty())return std::string::npos;size_t n=0;
        for(char c:raw){if(c<'0'||c>'9'||n>payload.size()/10)return std::string::npos;n=n*10+c-'0';}
        return n<=payload.size()?n:std::string::npos;
    };
    auto first=number("StartFragment:"),last=number("EndFragment:");
    if(first==std::string::npos||last==std::string::npos||last<first){
        const auto start=payload.find("<!--StartFragment"),finish=payload.find("<!--EndFragment");
        first=start==std::string::npos?std::string::npos:payload.find("-->",start);
        if(first!=std::string::npos)first+=3;last=finish;
    }
    if(first==std::string::npos||last==std::string::npos||last<first)return {};
    try{
        if(origin)*origin=tcard_clip::utf8(field("SourceURL:"));
        return tcard_clip::utf8(payload.substr(first,last-first));
    }catch(...){return {};}
}
inline bool paste_document(HWND control,const tcard_rich::Document& doc)
{
    auto* value=state(control);if(!value||!value->editable)return false;
    CHARRANGE original{};SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&original));
    std::wstring inserted;
    for(size_t i=0;i<doc.paragraphs.size();++i){
        if(i)inserted+=L'\r';
        for(const auto& run:doc.paragraphs[i].runs)inserted+=run.image.empty()?run.text:L"\u25a1";
    }
    const auto before=raw_text(control);
    original.cpMin=std::clamp(original.cpMin,0L,static_cast<LONG>(before.size()));
    original.cpMax=std::clamp(original.cpMax,original.cpMin,static_cast<LONG>(before.size()));
    SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&original));
    const auto limit=SendMessageW(control,EM_GETLIMITTEXT,0,0);
    if(inserted.empty()||before.size()-(original.cpMax-original.cpMin)+inserted.size()>static_cast<size_t>(limit))return false;
    const auto tom=text_document(control);if(!tom)return false;
    tom->BeginEditCollection();
    SendMessageW(control,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(inserted.c_str()));
    if(raw_text(control).size()!=before.size()-(original.cpMax-original.cpMin)+inserted.size()){
        tom->EndEditCollection();SendMessageW(control,EM_UNDO,0,0);return false;
    }
    LONG position=original.cpMin;
    for(const auto& paragraph:doc.paragraphs){
        const LONG start=position;
        for(const auto& run:paragraph.runs){
            CHARRANGE selected{position,position+static_cast<LONG>(run.text.size())};SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selected));
            auto face=format(run.style,value->family);
            if(!run.style.explicitColor){face.dwEffects&=~CFE_AUTOCOLOR;face.crTextColor=value->ink;}
            SendMessageW(control,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face));position=selected.cpMax;
        }
        {
            CHARRANGE selected{start,position};SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selected));
            PARAFORMAT2 para{};para.cbSize=sizeof(para);para.dwMask=PFM_NUMBERING|PFM_NUMBERINGSTART|PFM_NUMBERINGTAB|PFM_STARTINDENT|PFM_OFFSET;para.wNumbering=paragraph.list==1?PFN_BULLET:paragraph.list==2?PFN_ARABIC:0;para.wNumberingStart=static_cast<WORD>(paragraph.number);para.dxStartIndent=0;para.dxOffset=paragraph.list?125:0;para.wNumberingTab=paragraph.list?125:0;SendMessageW(control,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));
        }
        columns(control,paragraph);
        ++position;
    }
    if(!insert_images(control,doc,original.cpMin,true)){
        tom->EndEditCollection();SendMessageW(control,EM_UNDO,0,0);return false;
    }
    CHARRANGE caret{original.cpMin+static_cast<LONG>(inserted.size()),original.cpMin+static_cast<LONG>(inserted.size())};
    SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&caret));
    if(tom)tom->EndEditCollection();changed(control);return true;
}
inline bool paste(HWND control,bool plainOnly=false)
{
    auto* value=state(control);if(!value||!value->logical||!value->editable||value->pasteJob)return false;
    std::wstring html,text,origin;
    if(!OpenClipboard(control))return false;
    const UINT htmlId=RegisterClipboardFormatW(L"HTML Format");
    if(!plainOnly&&htmlId)if(auto data=GetClipboardData(htmlId)){
        const auto size=GlobalSize(data);if(size&&size<=16*1024*1024)if(const auto* p=static_cast<const char*>(GlobalLock(data))){
            html=html_fragment(std::string(p,size),&origin);GlobalUnlock(data);
        }
    }
    if(auto data=GetClipboardData(CF_UNICODETEXT)){
        const auto size=GlobalSize(data);if(size&&size<=8*1024*1024)if(const auto* p=static_cast<const wchar_t*>(GlobalLock(data))){
            const size_t capacity=size/sizeof(wchar_t);size_t count=0;while(count<capacity&&p[count])++count;
            if(count<capacity)text.assign(p,count);GlobalUnlock(data);
        }
    }
    CloseClipboard();
    if(html.empty()&&text.empty())return false;
    auto doc=!html.empty()?tcard_rich::parse(html,value->base):tcard_rich::plain(text);
    bool images=false;
    for(const auto& paragraph:doc.paragraphs)for(const auto& run:paragraph.runs)images|=!run.image.empty();
    if(!images&&!html.empty()&&tcard_rich::text(doc).empty())doc=tcard_rich::plain(text);
    if(doc.paragraphs.empty())return false;
    if(!images)return paste_document(control,doc);
    auto job=std::make_shared<PasteJob>();job->doc=std::move(doc);
    SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&job->selection));
    const auto used=tcard_rich::serialize(document(control)).size();
    const size_t budget=used<3*1024*1024?3*1024*1024-used:0;
    if(!SetTimer(control,kPasteTimer,50,nullptr))return false;
    value->pasteFailed=false;value->pasteJob=job;EnableWindow(control,FALSE);
    try{
        job->worker=std::thread([job,origin,budget]{
            try{
                const auto deadline=GetTickCount64()+tcard_download::batch_ms;
                size_t remaining=budget;unsigned requests=0;std::map<std::wstring,std::wstring> cache;
                for(auto& paragraph:job->doc.paragraphs)for(auto& run:paragraph.runs){
                    if(run.image.empty())continue;
                    const auto address=run.image.starts_with(L"data:image/")?run.image:tcard_clip::url(run.image,origin);
                    std::wstring embedded;
                    if(!job->cancelled&&requests<12&&remaining){
                        auto found=cache.find(address);
                        if(found==cache.end()){
                            ++requests;
                            auto uri=address.starts_with(L"data:image/")?address:tcard_download::fetch(address,job->cancelled,deadline);
                            if(uri.size()>remaining||tcard_image::decode(uri).dib.empty())uri.clear();
                            found=cache.emplace(address,std::move(uri)).first;
                        }
                        if(found->second.size()<=remaining)embedded=found->second;
                    }
                    if(embedded.empty()){
                        run.text=run.alt.empty()?L"Image":run.alt;
                        if(!address.empty()&&!address.starts_with(L"data:"))run.text+=L" ("+address+L")";
                        run.image.clear();
                    }else{remaining-=embedded.size();run.image=std::move(embedded);}
                }
            }catch(...){job->cancelled=true;}
            job->done=true;
        });
    }catch(...){value->pasteJob.reset();KillTimer(control,kPasteTimer);EnableWindow(control,TRUE);return false;}
    return true;
}
inline void paint_checkpoints(HWND control)
{
    const auto* value=state(control);if(!value)return;
    const auto text=raw_text(control);if(text.size()>1024*1024)return;
    RECT client{};GetClientRect(control,&client);
    HDC dc=GetDC(control);if(!dc)return;
    const int dpi=static_cast<int>(GetDpiForWindow(control));
    HPEN pen=CreatePen(PS_SOLID,1,RGB(150,150,145));const auto old=SelectObject(dc,pen);
    for(size_t start=0;start<text.size();){
        const auto next=text.find(L'\r',start),end=next==std::wstring::npos?text.size():next;
        if(tcard_rich::is_checkpoint(std::wstring_view(text).substr(start,end-start))){
            POINTL left{},prefixEnd{},right{};
            SendMessageW(control,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&left),start);
            SendMessageW(control,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&prefixEnd),start+2);
            SendMessageW(control,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&right),end-2);
            const int half=MulDiv(static_cast<int>(value->base*96/72),dpi,192);
            const int y=left.y+half;
            if(y>=0&&y<client.bottom&&prefixEnd.y==left.y&&prefixEnd.x>left.x){
                RECT erase{left.x,left.y,prefixEnd.x,left.y+half*2+4};
                HBRUSH brush=CreateSolidBrush(value->paper);FillRect(dc,&erase,brush);DeleteObject(brush);
                MoveToEx(dc,left.x,y,nullptr);LineTo(dc,prefixEnd.x,y);
            }
            if(y>=0&&y<client.bottom&&right.x<client.right-4){
                RECT erase{right.x,right.y,client.right,right.y+half*2+4};HBRUSH brush=CreateSolidBrush(value->paper);FillRect(dc,&erase,brush);DeleteObject(brush);
                MoveToEx(dc,right.x,y,nullptr);LineTo(dc,client.right-4,y);
            }
        }
        if(next==std::wstring::npos)break;start=next+1;
    }
    SelectObject(dc,old);DeleteObject(pen);ReleaseDC(control,dc);
}
inline LRESULT CALLBACK editor_proc(HWND control,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data)
{
    auto* value=reinterpret_cast<State*>(data);
    if(message==WM_NCDESTROY){RemoveWindowSubclass(control,editor_proc,id);delete value;return DefSubclassProc(control,message,w,l);}
    if(message==WM_TIMER&&w==kPasteTimer&&value->pasteJob){
        if(value->pasteJob->done){
            auto job=std::move(value->pasteJob);if(job->worker.joinable())job->worker.join();KillTimer(control,kPasteTimer);EnableWindow(control,TRUE);
            value->pasteFailed=job->cancelled;
            if(!job->cancelled){
                SendMessageW(control,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&job->selection));
                value->pasteFailed=!paste_document(control,job->doc);
            }
            const HWND root=GetAncestor(control,GA_ROOT);
            if(GetForegroundWindow()==root&&(!GetFocus()||GetFocus()==root))SetFocus(control);
        }
        return 0;
    }
    if(!value->loading&&!tcard_edit::composing(control)){
        if(message==WM_COPY){copy(control);return 0;}
        if(message==WM_CUT&&value->logical&&value->editable){if(copy(control))SendMessageW(control,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L""));return 0;}
        if((message==WM_PASTE||message==EM_PASTESPECIAL)&&value->logical){paste(control,(GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}
        if(message==WM_CHAR&&value->logical&&(GetKeyState(VK_CONTROL)&0x8000)&&(w==2||w==9||w==21)&&!(GetKeyState(VK_MENU)&0x8000)){
            apply(control,w==2?tcard_rich::Bold:w==9?tcard_rich::Italic:tcard_rich::Underline);return 0;
        }
    }
    const auto result=DefSubclassProc(control,message,w,l);
    if(message==WM_PAINT||message==WM_VSCROLL||message==WM_MOUSEWHEEL||message==WM_SIZE)paint_checkpoints(control);
    return result;
}
} // namespace tcard_rich_edit
