#pragma once
#include "card_rich_edit.h"
#include "card_image_codec.h"

namespace tcard_media {
inline constexpr UINT_PTR timer=0x54434d49,subclass=0x54434d50;
inline constexpr const wchar_t* property=L"TCard.Media.Import";
struct Job {
    tcard_codec::Bytes input;tcard_codec::Encoded output;std::wstring path,root,uri;
    CHARRANGE selection{};bool markdown=false,adjust=false,oversized=false,failed=false;
    std::atomic_bool done=false;std::jthread worker;
    ~Job(){worker.request_stop();if(worker.joinable())worker.join();}
};
inline Job* job(HWND editor){return reinterpret_cast<Job*>(GetPropW(editor,property));}
inline bool busy(HWND editor){return job(editor)!=nullptr;}
inline void failed(HWND owner){MessageBoxW(owner,tcard_lang::text(L"message.image_failed",L"The image could not be inserted. Check its format and size.").c_str(),L"TCard",MB_OK|MB_ICONINFORMATION);}
inline void cancel(HWND editor)
{
    KillTimer(editor,timer);auto* value=job(editor);RemovePropW(editor,property);delete value;if(value)EnableWindow(editor,TRUE);
}
inline tcard_codec::Bytes read(const std::wstring& path)
{
    tcard_asset::File file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));LARGE_INTEGER size{};
    if(!file||!tcard_asset::regular(file.value,false)||!GetFileSizeEx(file.value,&size)||size.QuadPart<=0||size.QuadPart>tcard_codec::inputLimit)throw std::runtime_error("Unsafe or oversized import file");
    tcard_codec::Bytes bytes(static_cast<size_t>(size.QuadPart));DWORD count=0;if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size())throw std::runtime_error("Image import read failed");return bytes;
}
inline void work(Job* value)
{
    value->done=false;value->failed=false;value->oversized=false;
    value->worker=std::jthread([value](std::stop_token stop){
        try{
            if(!value->path.empty()&&value->input.empty())value->input=read(value->path);
            if(stop.stop_requested())throw std::runtime_error("Image import cancelled");
            value->output=tcard_codec::prepare(value->input,value->adjust);value->oversized=value->output.oversized;
            if(!value->oversized){
                auto uri=tcard_codec::uri(value->output);if(stop.stop_requested())throw std::runtime_error("Image import cancelled");
                value->uri=value->root.empty()?std::move(uri):tcard_asset::image(value->root,uri);
                if(value->uri.empty())throw std::runtime_error("Image storage failed");
            }
        }catch(...){value->failed=true;}value->done=true;
    });
}
inline bool insert(HWND editor,Job& value)
{
    SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&value.selection));
    if(!value.markdown){auto* rich=tcard_rich_edit::state(editor);if(!rich)return false;
        if(value.root.empty()&&value.uri.size()>3*1024*1024)return false;
        tcard_rich::Document doc;doc.paragraphs.emplace_back();tcard_rich::Run run;run.image=value.uri;run.alt=L"Image";run.text=L"\xfffc";doc.paragraphs[0].runs.push_back(std::move(run));
        return tcard_rich_edit::paste_document(editor,doc);
    }
    const auto text=L"![Image]("+value.uri+L")",before=tcard_rich_edit::raw_text(editor);const auto limit=SendMessageW(editor,EM_GETLIMITTEXT,0,0);
    if(value.selection.cpMin<0||value.selection.cpMax<value.selection.cpMin||static_cast<size_t>(value.selection.cpMax)>before.size()||
       before.size()-(value.selection.cpMax-value.selection.cpMin)+text.size()>static_cast<size_t>(limit))return false;
    SendMessageW(editor,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(text.c_str()));return true;
}
inline LRESULT CALLBACK editor_proc(HWND editor,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR)
{
    if(message==WM_NCDESTROY){cancel(editor);RemoveWindowSubclass(editor,editor_proc,id);return DefSubclassProc(editor,message,w,l);}
    if(message==WM_SETTEXT&&busy(editor))cancel(editor);
    if(message==WM_TIMER&&w==timer){auto* value=job(editor);if(!value||!value->done.load())return 0;
        if(value->worker.joinable())value->worker.join();EnableWindow(editor,TRUE);
        if(value->oversized){if(tcard_rich_edit::image_adjust(editor)){value->adjust=true;EnableWindow(editor,FALSE);try{work(value);}catch(...){cancel(editor);failed(GetAncestor(editor,GA_ROOT));}return 0;}cancel(editor);return 0;}
        KillTimer(editor,timer);RemovePropW(editor,property);std::unique_ptr<Job> complete(value);
        if((GetWindowLongPtrW(editor,GWL_STYLE)&WS_VISIBLE)==0)return 0;
        if(value->failed||!insert(editor,*value))failed(GetAncestor(editor,GA_ROOT));
        if(GetForegroundWindow()==GetAncestor(editor,GA_ROOT))SetFocus(editor);return 0;
    }
    return DefSubclassProc(editor,message,w,l);
}
inline bool begin(HWND editor,bool markdown,tcard_codec::Bytes input={},std::wstring path={})
{
    const auto* rich=tcard_rich_edit::state(editor);
    if(busy(editor)||!IsWindowEnabled(editor)||tcard_edit::composing(editor)||(rich&&rich->pasteJob))return false;
    auto value=std::make_unique<Job>();value->input=std::move(input);value->path=std::move(path);value->markdown=markdown;value->root=rich&&rich->logical?rich->assetRoot:tcard_edit::assets(editor);
    SendMessageW(editor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&value->selection));
    if(!SetPropW(editor,property,value.get()))return false;
    if(!SetTimer(editor,timer,50,nullptr)){RemovePropW(editor,property);return false;}
    EnableWindow(editor,FALSE);try{work(value.get());value.release();return true;}catch(...){RemovePropW(editor,property);KillTimer(editor,timer);EnableWindow(editor,TRUE);return false;}
}
inline tcard_clip::ClipboardData capture(HWND owner)
{
    if(!OpenClipboard(owner))throw std::runtime_error("Clipboard busy");struct Close{~Close(){CloseClipboard();}} close;
    const auto read=[](UINT format,size_t limit){try{return tcard_clip::clipboard_data(format,limit);}catch(...){return tcard_codec::Bytes{};}};
    tcard_clip::ClipboardData data;data.html=read(RegisterClipboardFormatW(L"HTML Format"),tcard_clip::max_source*4);data.png=read(RegisterClipboardFormatW(L"PNG"),tcard_clip::max_image);
    if(data.png.empty())data.dib=read(CF_DIB,tcard_codec::inputLimit-sizeof(BITMAPFILEHEADER));data.text=read(CF_UNICODETEXT,tcard_clip::max_source*sizeof(wchar_t)+sizeof(wchar_t));return data;
}
inline bool clipboard(HWND editor,tcard_clip::ClipboardData data,bool markdown=false)
{
    auto input=data.png.empty()?tcard_codec::bitmap(data.dib):std::move(data.png);if(input.empty())return false;return begin(editor,markdown,std::move(input));
}
inline bool rich_clipboard(HWND editor,tcard_clip::ClipboardData&& data){return clipboard(editor,std::move(data));}
inline void attach(HWND editor){SetWindowSubclass(editor,editor_proc,subclass,0);tcard_rich_edit::image_insert=rich_clipboard;}
}
