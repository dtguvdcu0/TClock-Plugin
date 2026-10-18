#include <winsock2.h>
#include "card_sync_capture.h"
#include "../../core/card_sync_store.h"

namespace tcard_compat {
namespace {
void capture_require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
void capture_scan(const std::wstring& source,const std::wstring& relative,unsigned mode,std::vector<tcard_cipher::Object>& objects,size_t& total)
{
    const auto directory=relative.empty()?source:source+L"\\"+relative;tcard_asset::File guard(tcard_asset::open_dir(directory,false));
    capture_require(static_cast<bool>(guard),"Native capture directory unavailable");
    WIN32_FIND_DATAW item{};HANDLE handle=FindFirstFileW((directory+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){capture_require(GetLastError()==ERROR_FILE_NOT_FOUND,"Native capture enumeration failed");return;}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
        const auto path=relative.empty()?name:relative+L"\\"+name;
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){unsigned child=99;
            if(mode==0){if(name==L"_sync")child=2;else if(name==L"_trash")child=3;else if(name[0]!=L'_')child=1;}
            else if(mode==2)child=4;else if(mode==3)child=1;else if((mode==1||mode==4)&&name==L"images")child=5;
            if(child!=99){capture_require(!(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Native capture refuses reparse directories");capture_scan(source,path,child,objects,total);}continue;
        }
        uint8_t kind=0;std::wstring id;
        const auto dot=name.find_last_of(L'.');const auto stem=dot==std::wstring::npos?name:name.substr(0,dot);
        if(mode==0&&name.ends_with(L".deleted"))kind=4;
        else if((mode==1||mode==4)&&name==L"card.json")kind=5;
        else if(mode==4&&name==L"pending.json")kind=5;
        else if(mode==4&&name.ends_with(L".json")&&tcard_sync::sync_hex(stem)){kind=2;id=stem;}
        else if((mode==1||mode==4)&&(name==L"note.txt"||name==L"note.md"||name==L"note.html"||
            (name.starts_with(L"note-")&&(name.ends_with(L".txt")||name.ends_with(L".md")||name.ends_with(L".html")))))kind=1;
        else if(mode==5&&tcard_sync::sync_hex(stem))kind=3;
        if(!kind)continue;capture_require(!(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Native capture refuses reparse files");
        tcard_crypto::Bytes bytes;capture_require(tcard_asset::read(directory,name,bytes,tcard_crypto::maxObject),"Native retained object unreadable");
        total+=bytes.size();capture_require(total<=128*1024*1024&&objects.size()<tcard_cipher::maxEntries,"Native capture size limit");
        const auto hash=tcard_asset::hash(bytes);
        if(kind==3)capture_require(stem==hash,"Native image hash mismatch");
        if(kind==1&&name.starts_with(L"note-"))capture_require(stem.substr(5)==tcard_asset::hash(tcard_asset::encode(tcard_asset::decode(bytes))),"Native source hash mismatch");
        if(kind==2){const auto operation=tcard_sync::JsonObject::Parse(tcard_asset::decode(bytes));
            capture_require((operation.GetNamedNumber(L"Version")==1||operation.GetNamedNumber(L"Version")==2||operation.GetNamedNumber(L"Version")==3)&&std::wstring(operation.GetNamedString(L"Id"))==id&&
                tcard_sync::sync_stem(std::wstring(operation.GetNamedString(L"Note")))==relative.substr(relative.find_last_of(L'\\')+1),"Native operation identity mismatch");
            const auto before=std::wstring(operation.GetNamedObject(L"Before").GetNamedString(L"SourceFile"));
            const auto after=std::wstring(operation.GetNamedObject(L"After").GetNamedString(L"SourceFile"));
            capture_require(tcard_asset::relative(before,false)&&tcard_asset::relative(after,false),"Invalid retained source reference");
            // Missing prerequisites remain opaque retained intent; never fabricate or apply them.
            if(tcard_sync::sync_exists(directory,before)&&tcard_sync::sync_exists(directory,after))tcard_sync::sync_unpack(operation,directory,false);
        }
        if(id.empty())id=tcard_asset::hash(tcard_asset::encode(L"TCard.NativeCapture.v2:"+std::to_wstring(kind)+L":"+path+L":"+hash));
        objects.push_back({id,path,kind,std::move(bytes)});
    }while(FindNextFileW(handle,&item));
    capture_require(GetLastError()==ERROR_NO_MORE_FILES,"Native capture enumeration interrupted");
}
bool capture_collect(const std::wstring& source,std::vector<tcard_cipher::Object>& objects,std::string& error)
{
    objects.clear();error.clear();try{size_t total=0;capture_scan(source,L"",0,objects,total);
        std::sort(objects.begin(),objects.end(),[](const auto& a,const auto& b){return a.name<b.name;});return true;
    }catch(const std::exception& e){objects.clear();error=e.what();return false;}catch(...){objects.clear();error="Invalid native capture";return false;}
}
}
bool Capture(const std::wstring& source,std::vector<tcard_cipher::Object>& objects,std::string& error)
{
    std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(source);
    if(!lock){objects.clear();error="Native writer lock unavailable";return false;}return capture_collect(source,objects,error);
}
bool AdvanceJournal(const std::wstring& root,const std::wstring& source,size_t budget,bool activate,tcard_cipher::Status& status)
{
    status={};const auto target=tcard_sync::sync_path(root),native=tcard_sync::sync_path(source);
    if(target.empty()||native.empty()||target==native||target.starts_with(native+L"\\")||native.starts_with(target+L"\\")){
        status.error="Native and encrypted roots must be disjoint";return false;}
    // Lock order is native source then v2 store; v2 never acquires a source lock.
    std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(source);
    if(!lock){status.error="Native writer lock unavailable";return false;}
    std::vector<tcard_cipher::Object> objects;if(!capture_collect(source,objects,status.error))return false;
    return tcard_cipher::Advance(root,&objects,budget,activate,status);
}
}
