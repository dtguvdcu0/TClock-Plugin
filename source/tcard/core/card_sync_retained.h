#pragma once
#include "card_sync_stage.h"
#include <stop_token>

// Explicit pre-merge native retention. Never a live activation or owner-applied acknowledgement.
namespace tcard_retained {
using namespace tcard_sync;
struct Entry { std::wstring hash;uint64_t size=0;bool operator==(const Entry&)const=default; };
using Catalog=std::map<std::wstring,Entry>;
struct Capture { Catalog files;std::wstring local,remote,digest; };
struct Cursor { std::wstring owner,after;Capture capture;size_t next=0; };
constexpr size_t maxFiles=10000;
inline bool Path(const std::wstring& path)
{
    size_t start=0,count=0;for(;;){const auto end=path.find(L'\\',start);const auto leaf=path.substr(start,end==std::wstring::npos?end:end-start);
        if(!tcard_asset::leaf(leaf)||++count>32)return false;if(end==std::wstring::npos)return true;start=end+1;}
}
inline bool Excluded(const std::wstring& name)
{
    return name==L".tcard-vault.key"||name==L".tcard-vault.required"||name==L".tcard-vault.pending"||name==L".tcard-vault.stage"||
        name==L"_bootstrap-stage"||name==L"_bootstrap"||name==L"_object-transfers"||name==L"_native-generations"||name==L".tcard-native.required"||name==L"_bootstrap-retained";
}
inline tcard_crypto::Bytes ReadFile(const std::wstring& root,const std::wstring& relative)
{
    if(!Path(relative))throw std::runtime_error("Unsafe native retention path");const auto slash=relative.find_last_of(L'\\');
    const auto directory=tcard_native::Extended(slash==std::wstring::npos?root:root+L"\\"+relative.substr(0,slash)),leaf=relative.substr(slash==std::wstring::npos?0:slash+1);tcard_crypto::Bytes bytes;
    if(!tcard_asset::read(directory,leaf,bytes))throw std::runtime_error("Native retained file unavailable");return bytes;
}
inline void Scan(const std::wstring& root,const std::wstring& relative,Catalog& files,bool source,size_t& visited,std::stop_token stop={})
{
    if(!relative.empty()&&!Path(relative))throw std::runtime_error("Native retention depth exceeded");
    const auto directory=tcard_native::Extended(relative.empty()?root:root+L"\\"+relative);tcard_asset::File guard(tcard_asset::open_dir(directory,false));if(!guard)throw std::runtime_error("Unsafe native retention directory");
    WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW((directory+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return;throw std::runtime_error("Native retention enumeration failed");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{if(stop.stop_requested())throw std::runtime_error("Native catalog interrupted");const std::wstring name=item.cFileName;if(name==L"."||name==L".."||(source&&relative.empty()&&Excluded(name)))continue;
        if(++visited>2*maxFiles||!tcard_asset::leaf(name)||(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Unsafe native retained entry");
        const auto path=relative.empty()?name:relative+L"\\"+name;if(!Path(path))throw std::runtime_error("Invalid native retained path");
        if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){Scan(root,path,files,source,visited,stop);continue;}
        if(files.size()>=maxFiles)throw std::runtime_error("Native retention file budget exceeded");const auto bytes=ReadFile(root,path);const auto hash=tcard_asset::hash(bytes);
        if(!sync_hex(hash)||!files.emplace(path,Entry{hash,bytes.size()}).second)throw std::runtime_error("Invalid native retained object");
    }while(FindNextFileW(handle,&item));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Native retention enumeration interrupted");
}
inline Catalog Collect(const std::wstring& root,bool source,std::stop_token stop={}){if(stop.stop_requested())throw std::runtime_error("Native catalog interrupted");Catalog files;size_t visited=0;Scan(root,L"",files,source,visited,stop);return files;}
inline Capture Read(const std::wstring& owner)
{
    tcard_stage::Check(owner);std::wstring text;if(!tcard_bridge::Read(tcard_stage::Folder(owner),L"native.json",text))throw std::runtime_error("Native capture unavailable");
    const auto record=JsonObject::Parse(text);Capture capture;capture.digest=tcard_bridge::Hash(text);
    capture.local=std::wstring(record.GetNamedObject(L"Local").Stringify());capture.remote=sync_string(record,L"Remote");selection::Read(capture.local);
    if(record.GetNamedNumber(L"Version")!=1||sync_string(record,L"Group")!=tcard_chunks::Group(owner)||sync_string(record,L"Owner")!=tcard_bootstrap::Node(owner)||!sync_hex(capture.remote))throw std::runtime_error("Native capture identity differs");
    std::wstring target;if(!tcard_bridge::Read(tcard_stage::Folder(owner),L"target.json",target)||sync_string(JsonObject::Parse(target),L"RemoteDigest")!=capture.remote)throw std::runtime_error("Native capture target differs");
    const auto entries=record.GetNamedArray(L"Files");if(entries.Size()>maxFiles)throw std::runtime_error("Native capture budget exceeded");
    for(unsigned i=0;i<entries.Size();++i){const auto entry=entries.GetObjectAt(i);const auto path=sync_string(entry,L"Path"),hash=sync_string(entry,L"Hash");const auto size=sync_number(entry,L"Size");
        if(!Path(path)||Excluded(path.substr(0,path.find(L'\\')))||!sync_hex(hash)||size>tcard_asset::max_file||!capture.files.emplace(path,Entry{hash,size}).second)throw std::runtime_error("Invalid native capture entry");}
    return capture;
}
inline void Begin(const std::wstring& owner,std::stop_token stop={})
{
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Native capture owner busy");
    if(sync_exists(tcard_stage::Folder(owner),L"native.json")){Read(owner);return;}
    const auto state=tcard_stage::Read(owner);if(!Collect(tcard_stage::Data(owner),false,stop).empty())throw std::runtime_error("Native capture requires an empty candidate");
    if(selection::Read(state.local)!=selection::Read(tcard_stage::Inventory(owner)))throw std::runtime_error("Native capture local inventory changed");
    const auto files=Collect(owner,true,stop);JsonObject record;JsonArray entries;
    for(const auto& [path,value]:files){JsonObject entry;entry.Insert(L"Path",JsonValue::CreateStringValue(path));entry.Insert(L"Hash",JsonValue::CreateStringValue(value.hash));entry.Insert(L"Size",JsonValue::CreateStringValue(std::to_wstring(value.size)));entries.Append(entry);}
    record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Group",JsonValue::CreateStringValue(tcard_chunks::Group(owner)));record.Insert(L"Owner",JsonValue::CreateStringValue(tcard_bootstrap::Node(owner)));
    record.Insert(L"Local",JsonObject::Parse(state.local));record.Insert(L"Remote",JsonValue::CreateStringValue(tcard_bridge::Hash(state.remote)));record.Insert(L"Files",entries);
    const auto text=std::wstring(record.Stringify());if(tcard_asset::encode(text).size()>tcard_asset::max_file)throw std::runtime_error("Native catalog budget exceeded");
    if(!tcard_bridge::Publish(tcard_stage::Folder(owner),L"native.json",text)||Read(owner).files!=files)throw std::runtime_error("Cannot retain native catalog");
}
inline void Current(const std::wstring& owner,const Capture& capture,std::stop_token stop={})
{
    if(Collect(owner,true,stop)!=capture.files||selection::Read(tcard_stage::Inventory(owner))!=selection::Read(capture.local))throw std::runtime_error("Native source changed; prepare a fresh candidate");
}
inline bool Step(const std::wstring& owner,Cursor& cursor,std::stop_token stop={})
{
    if(stop.stop_requested())throw std::runtime_error("Native copy interrupted");
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Native copy owner busy");tcard_stage::Check(owner);
    if(sync_exists(tcard_stage::Folder(owner),L"native-ready.json")){const auto capture=Read(owner);tcard_stage::Read(owner);Current(owner,capture,stop);return true;}
    // Cache only provisional copy work. Reauthenticate the catalog before sealing, never at idle.
    if(cursor.owner!=owner){auto capture=Read(owner);cursor={};cursor.capture=std::move(capture);cursor.owner=owner;}
    const auto& capture=cursor.capture;
    if(cursor.next>capture.files.size())throw std::runtime_error("Invalid native copy cursor");
    if(cursor.next<capture.files.size()){
        auto entry=cursor.after.empty()?capture.files.begin():capture.files.upper_bound(cursor.after);if(entry==capture.files.end())throw std::runtime_error("Invalid native copy position");const auto& [path,expected]=*entry;const auto bytes=ReadFile(owner,path);
        if(bytes.size()!=expected.size||tcard_asset::hash(bytes)!=expected.hash)throw std::runtime_error("Native source changed during copy");
        auto folder=tcard_stage::Data(owner);size_t start=0;
        for(auto slash=path.find(L'\\');slash!=std::wstring::npos;slash=path.find(L'\\',start)){
            folder+=L"\\"+path.substr(start,slash-start);
            if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES&&!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect native copy directory");
            tcard_asset::File checked(tcard_asset::open_dir(folder,false));if(!checked)throw std::runtime_error("Unsafe native copy directory");start=slash+1;}
        const auto leaf=path.substr(start);
        if(sync_exists(folder,leaf)){if(ReadFile(tcard_stage::Data(owner),path)!=bytes)throw std::runtime_error("Retained native copy differs");}
        else if(!tcard_asset::publish(folder,leaf,bytes,true)||ReadFile(tcard_stage::Data(owner),path)!=bytes)throw std::runtime_error("Native copy publication failed");
        cursor.after=path;++cursor.next;return false;
    }
    if(Read(owner).digest!=capture.digest)throw std::runtime_error("Native catalog changed during copy");
    if(Collect(owner,true,stop)!=capture.files||selection::Read(tcard_stage::Inventory(owner))!=selection::Read(capture.local))throw std::runtime_error("Native source changed during copy");
    if(Collect(tcard_stage::Data(owner),false,stop)!=capture.files||selection::Read(tcard_stage::Inventory(tcard_stage::Data(owner)))!=selection::Read(capture.local))throw std::runtime_error("Native candidate incomplete");
    JsonObject seal;seal.Insert(L"Version",JsonValue::CreateNumberValue(1));seal.Insert(L"Catalog",JsonValue::CreateStringValue(capture.digest));
    if(!tcard_bridge::Publish(tcard_stage::Folder(owner),L"native-ready.json",std::wstring(seal.Stringify())))throw std::runtime_error("Cannot seal native copy");
    tcard_stage::Read(owner);return true;
}
inline bool Ready(const std::wstring& owner)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Native readiness owner busy");const auto capture=Read(owner);
    tcard_stage::Read(owner);Current(owner,capture);
    // A prior seal is not fresh integrity evidence: authenticate retained local-only files too.
    const auto candidate=Collect(tcard_stage::Data(owner),false);
    for(const auto& [path,expected]:capture.files){
        if(!path.starts_with(L"_trash\\"))continue;
        const auto found=candidate.find(path);
        if(found==candidate.end()||found->second!=expected)throw std::runtime_error("Retained local Trash changed after sealing");
    }
    return tcard_stage::Inspect(owner).ready;
}
}
