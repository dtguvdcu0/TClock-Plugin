#pragma once
#include "card_native_root.h"
#include "card_sync_retained.h"

namespace tcard_native {
inline std::wstring Digest(const tcard_retained::Catalog& files)
{
    JsonArray entries;for(const auto& [path,item]:files){JsonObject row;row.Insert(L"Path",JsonValue::CreateStringValue(path));row.Insert(L"Hash",JsonValue::CreateStringValue(item.hash));row.Insert(L"Size",JsonValue::CreateStringValue(std::to_wstring(item.size)));entries.Append(row);}
    const auto hash=tcard_bridge::Hash(std::wstring(entries.Stringify()));if(!sync_hex(hash))throw std::runtime_error("Native catalog digest unavailable");return hash;
}
inline bool History(const JsonObject& before,const JsonObject& after)
{
    if(sync_string(before,L"Id")!=sync_string(after,L"Id"))return false;
    const auto a=before.GetNamedArray(L"History",JsonArray{}),b=after.GetNamedArray(L"History",JsonArray{});
    if(a.Size()>b.Size())return false;for(unsigned i=0;i<a.Size();++i)if(a.GetAt(i).Stringify()!=b.GetAt(i).Stringify())return false;
    const auto left=before.GetNamedObject(L"Display",JsonObject{}),right=after.GetNamedObject(L"Display",JsonObject{});
    for(const auto* key:{L"WindowWidthDip",L"WindowHeightDip"})if(left.GetNamedNumber(key,0)!=right.GetNamedNumber(key,0))return false;return true;
}
inline void Verify(const std::wstring& source,const tcard_retained::Capture& capture,const std::wstring& candidate,const tcard_retained::Catalog& files)
{
    std::map<std::wstring,std::wstring> relocated;std::map<std::wstring,std::vector<std::wstring>> trash;
    for(const auto& [path,item]:files)if(path.starts_with(L"_trash\\")&&path.ends_with(L"\\card.json")){
        const auto record=JsonObject::Parse(tcard_asset::decode(tcard_retained::ReadFile(candidate,path)));
        trash[sync_stem(sync_string(record,L"Id"))].push_back(path);
    }
    for(const auto& [path,item]:capture.files){
        const auto slash=path.find(L'\\');if(slash==std::wstring::npos||path[0]==L'_'||path.substr(slash)!=L"\\card.json")continue;
        const auto stem=path.substr(0,slash);const auto before=JsonObject::Parse(tcard_asset::decode(tcard_retained::ReadFile(source,path)));
        auto options=trash[stem];if(files.contains(path))options.insert(options.begin(),path);bool found=false;
        for(const auto& option:options){const auto after=JsonObject::Parse(tcard_asset::decode(tcard_retained::ReadFile(candidate,option)));
            if(History(before,after)){relocated[stem]=option.substr(0,option.size()-10);found=true;break;}}
        if(!found)throw std::runtime_error("Native local history or geometry was not retained");
    }
    const auto committed=selection::Read(tcard_stage::Inventory(candidate));std::set<std::wstring> ids;
    for(const auto& [note,id]:committed.operations)ids.insert(id);
    for(const auto& [path,item]:capture.files){
        const auto slash=path.find(L'\\');const auto stem=path.substr(0,slash);
        if(relocated.contains(stem)){
            if(path.substr(slash)==L"\\card.json")continue;
            const auto retained=relocated.at(stem)+path.substr(slash);const auto found=files.find(retained);
            if(found==files.end()||found->second!=item)throw std::runtime_error("Native historical source or image missing");continue;
        }
        if(slash==std::wstring::npos&&path.ends_with(L".deleted"))continue;
        if(path.starts_with(L"_sync\\")&&path.ends_with(L"\\pending.json")){
            const auto before=sync_ids(JsonObject::Parse(tcard_asset::decode(tcard_retained::ReadFile(source,path))));std::set<std::wstring> pending;
            if(files.contains(path)){const auto after=sync_ids(JsonObject::Parse(tcard_asset::decode(tcard_retained::ReadFile(candidate,path))));pending.insert(after.begin(),after.end());}
            for(const auto& id:before)if(!ids.contains(id)&&!pending.contains(id))throw std::runtime_error("Native pending intent disappeared");continue;
        }
        const auto found=files.find(path);if(found==files.end()||found->second!=item)throw std::runtime_error("Native retained file changed unexpectedly");
    }
}
struct Plan {std::wstring before,after,catalog,native,inventory;};
inline Plan ReadPlan(const std::wstring& source)
{
    JsonObject record;if(!sync_read(tcard_stage::Folder(source),L"activation.json",record)||record.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Native activation journal unavailable");
    Plan plan{sync_string(record,L"Before"),sync_string(record,L"After"),sync_string(record,L"Catalog"),sync_string(record,L"Native"),std::wstring(record.GetNamedObject(L"Inventory").Stringify())};
    if((!plan.before.empty()&&!sync_hex(plan.before))||!sync_hex(plan.after)||!sync_hex(plan.catalog)||!sync_hex(plan.native)||plan.before==plan.after)throw std::runtime_error("Invalid native activation journal");
    selection::Read(plan.inventory);return plan;
}
inline void Prepare(const std::wstring& source)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(source));if(!lock)throw std::runtime_error("Native activation owner busy");Initialize(source);
    if(sync_exists(tcard_stage::Folder(source),L"activation.json")){ReadPlan(source);return;}
    if(sync_path_plain(Resolve(Owner(source)))!=sync_path_plain(source)||!tcard_retained::Ready(source))throw std::runtime_error("Native candidate is not current and complete");
    WIN32_FIND_DATAW entry{};const auto search=FindFirstFileW((Control(source)+L"\\*").c_str(),&entry);size_t generations=0;
    if(search==INVALID_HANDLE_VALUE)throw std::runtime_error("Native generation enumeration failed");
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L".."||name==L"active.json")continue;
        if(!sync_hex(name)||!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){FindClose(search);throw std::runtime_error("Unexpected native generation entry");}++generations;
    }while(FindNextFileW(search,&entry));const auto enumerationError=GetLastError();FindClose(search);
    if(enumerationError!=ERROR_NO_MORE_FILES||generations>=16)throw std::runtime_error("Retained native generation budget reached");
    const auto capture=tcard_retained::Read(source);const auto candidate=tcard_stage::Data(source);const auto files=tcard_retained::Collect(candidate,false);Verify(source,capture,candidate,files);
    GUID guid{};wchar_t text[40]{};if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,text,40))throw std::runtime_error("Native generation identity unavailable");
    Plan plan{Selected(source),tcard_bridge::Hash(text),Digest(files),capture.digest,tcard_stage::Inventory(candidate)};
    JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Before",JsonValue::CreateStringValue(plan.before));record.Insert(L"After",JsonValue::CreateStringValue(plan.after));
    record.Insert(L"Catalog",JsonValue::CreateStringValue(plan.catalog));record.Insert(L"Native",JsonValue::CreateStringValue(plan.native));record.Insert(L"Inventory",JsonObject::Parse(plan.inventory));
    if(!sync_write(tcard_stage::Folder(source),L"activation.json",record)||ReadPlan(source).after!=plan.after)throw std::runtime_error("Cannot retain native activation journal");
}
inline bool Activate(const std::wstring& source)
{
    std::lock_guard guard(sync_mutex);const auto owner=Owner(source);SyncLock lock(owner);if(!lock)throw std::runtime_error("Native activation owner busy");
    const auto plan=ReadPlan(source);const auto selected=Selected(owner);
    if(selected==plan.after){if(selection::Missing(selection::Read(plan.inventory),selection::Read(tcard_stage::Inventory(owner))))throw std::runtime_error("Activated native originals unavailable");return true;}
    if(selected!=plan.before)throw std::runtime_error("Native generation changed before activation");
    const auto capture=tcard_retained::Read(source);if(capture.digest!=plan.native)throw std::runtime_error("Native activation source differs");tcard_retained::Current(source,capture);
    const auto staged=tcard_stage::Data(source),destination=Data(owner,plan.after);const bool moved=Exists(destination);
    if(moved&&Exists(staged))throw std::runtime_error("Ambiguous native activation candidates");const auto candidate=moved?destination:staged;
    if(Digest(tcard_retained::Collect(candidate,false))!=plan.catalog||selection::Read(tcard_stage::Inventory(candidate))!=selection::Read(plan.inventory))throw std::runtime_error("Native activation candidate changed");
    if(!moved){const auto generation=Control(owner)+L"\\"+plan.after;
        if(!tcard_bridge::Protect(generation))throw std::runtime_error("Cannot protect native generation");Check(generation);
        if(!MoveFileExW(staged.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot retain complete native generation");}
    Check(destination);JsonObject pointer;pointer.Insert(L"Version",JsonValue::CreateNumberValue(1));pointer.Insert(L"Generation",JsonValue::CreateStringValue(plan.after));
    if(!sync_write(Control(owner),L"active.json",pointer)||Selected(owner)!=plan.after)throw std::runtime_error("Native activation pointer publication blocked");return true;
}
inline void RetireStage(const std::wstring& source)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(source));if(!lock)throw std::runtime_error("Native staging owner busy");
    const auto folder=tcard_stage::Folder(source);if(!Exists(folder))return;
    const auto retained=tcard_bootstrap::Folder(source)+L"-retained";if(!Exists(retained)&&!tcard_bridge::Protect(retained))throw std::runtime_error("Cannot protect retained candidates");
    WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW((retained+L"\\*").c_str(),&item);size_t count=0;
    if(handle!=INVALID_HANDLE_VALUE){do{const std::wstring name=item.cFileName;if(name!=L"."&&name!=L"..")++count;}while(FindNextFileW(handle,&item));const auto error=GetLastError();FindClose(handle);if(error!=ERROR_NO_MORE_FILES)throw std::runtime_error("Candidate enumeration interrupted");}
    else if(GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Candidate enumeration failed");
    if(count>=8)throw std::runtime_error("Retained candidate budget reached");GUID guid{};wchar_t name[40]{};
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,name,40)||!MoveFileExW(folder.c_str(),(retained+L"\\"+name).c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot retain interrupted candidate");
}
}
