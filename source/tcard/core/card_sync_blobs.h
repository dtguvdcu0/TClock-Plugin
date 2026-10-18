#pragma once
#include "card_sync_selection.h"

// Bounded immutable reference transfers. Only verified committed inventories authorize retirement.
namespace tcard_blobs {
using namespace tcard_sync;
inline std::recursive_mutex transferMutex;
constexpr size_t maxTransfers=8;
inline std::wstring Canonical(const std::wstring& value){return std::wstring(JsonObject::Parse(value).Stringify());}
inline std::wstring Directory(const std::wstring& storage){auto path=tcard::CardDirectory(storage,L"_sync");path.resize(path.size()-6);return path;}
inline std::wstring Identity(const std::wstring& package){return tcard_bridge::Hash(Canonical(package));}
inline std::wstring Folder(const std::wstring& base)
{
    wchar_t absolute[32768]{};const auto count=GetFullPathNameW(base.c_str(),32768,absolute,nullptr);
    if(!count||count>=32768)throw std::runtime_error("Invalid object transfer path");std::wstring path=absolute;
    if(path.rfind(L"\\\\?\\",0)!=0){
        if(path.rfind(L"\\\\",0)==0)path=L"\\\\?\\UNC\\"+path.substr(2);
        else path=L"\\\\?\\"+path;
    }
    return path+L"\\_object-transfers";
}
inline std::wstring Root(const std::wstring& base,const std::wstring& package){return Folder(base)+L"\\"+Identity(package);}
inline std::vector<std::wstring> Slots(const std::wstring& base)
{
    std::vector<std::wstring> result;const auto folder=Folder(base);
    if(GetFileAttributesW(folder.c_str())==INVALID_FILE_ATTRIBUTES)return result;
    tcard_asset::File checked(tcard_asset::open_dir(folder,false));if(!checked)throw std::runtime_error("Unsafe object transfer directory");
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((folder+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return result;throw std::runtime_error("Cannot enumerate object transfers");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Unsafe object transfer entry");
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){if(!sync_hex(name)||result.size()>=maxTransfers)throw std::runtime_error("Object transfer budget or identity differs");result.push_back(name);}
    }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Object transfer enumeration failed");return result;
}
inline std::wstring Open(const std::wstring& base,const std::wstring& package,bool create=false)
{
    std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(base));if(!owner)throw std::runtime_error("Object transfer quota busy");
    std::lock_guard guard(transferMutex);tcard_objects::References(package);const auto group=tcard_chunks::Group(base),root=Root(base,package);
    auto record=JsonObject{};record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Group",JsonValue::CreateStringValue(group));record.Insert(L"Reference",JsonObject::Parse(package));
    const auto expected=std::wstring(record.Stringify());
    if(create){tcard_chunks::budget::Reserve(base,root,tcard_bridge::Hash(expected),tcard_asset::encode(expected).size()+8192,false);
        if(!tcard_bridge::Protect(Folder(base)))throw std::runtime_error("Cannot protect object transfers");
        if(GetFileAttributesW(root.c_str())==INVALID_FILE_ATTRIBUTES&&Slots(base).size()>=maxTransfers)throw std::runtime_error("Retained object transfer budget reached");
        if(!tcard_bridge::Protect(root))throw std::runtime_error("Cannot protect object transfer");
        if(!sync_exists(root,L"reference.json")&&!tcard_bridge::Publish(root,L"reference.json",expected))throw std::runtime_error("Cannot retain object reference");
    }
    tcard_asset::File checked(tcard_asset::open_dir(root,false));std::wstring saved;
    if(!checked||!tcard_bridge::Read(root,L"reference.json",saved)||saved!=expected)throw std::runtime_error("Retained object reference differs");return root;
}
inline selection::Inventory Required(const std::wstring& package)
{
    tcard_objects::References(package);return selection::Read(selection::Header(package));
}
inline bool Committed(const std::wstring& storage,const std::wstring& package)
{
    std::wstring header;Result result;unsigned version=0;
    return ExportCommittedInventory(storage,header,result,version)&&!selection::Missing(Required(package),selection::Read(header));
}
inline bool Remove(const std::wstring& base,const std::wstring& package,const selection::Inventory& committed)
{
    std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(base));if(!owner)return false;
    std::lock_guard guard(transferMutex);
    if(selection::Missing(Required(package),committed))return false;const auto root=Open(base,package),objects=root+L"\\_objects";
    const auto erase=[](const std::wstring& path){return DeleteFileW(path.c_str())||GetLastError()==ERROR_FILE_NOT_FOUND;};
    const auto remove=[](const std::wstring& path){return RemoveDirectoryW(path.c_str())||GetLastError()==ERROR_PATH_NOT_FOUND||GetLastError()==ERROR_FILE_NOT_FOUND;};
    for(const auto& value:tcard_objects::References(package)){
        const auto directory=objects+L"\\"+tcard_objects::Slot(value);
        if(GetFileAttributesW(directory.c_str())==INVALID_FILE_ATTRIBUTES)continue;
        {tcard_asset::File checked(tcard_asset::open_dir(directory,false));if(!checked)return false;
            if(sync_exists(directory,L"manifest.json")){tcard_objects::Cache cache(root,value,tcard_chunks::Group(base),false);}
            for(size_t i=0;i<value.hashes.size();++i)if(!erase(directory+L"\\"+std::to_wstring(i)+L".part"))return false;
            if(!erase(directory+L"\\manifest.json"))return false;
        }
        if(!remove(directory))return false;tcard_chunks::budget::Release(base,directory);
    }
    if(!remove(objects)||!erase(root+L"\\reference.json")||!remove(root))return false;tcard_chunks::budget::Release(base,root);return true;
}
inline void Reap(const std::wstring& base,const selection::Inventory& committed)
{
    std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(base));if(!owner)return;
    std::lock_guard guard(transferMutex);
    for(const auto& slot:Slots(base))try{
        std::wstring saved;if(!tcard_bridge::Read(Folder(base)+L"\\"+slot,L"reference.json",saved))continue;
        const auto record=JsonObject::Parse(saved);const auto package=std::wstring(record.GetNamedObject(L"Reference").Stringify());
        if(record.GetNamedNumber(L"Version")!=1||sync_string(record,L"Group")!=tcard_chunks::Group(base)||Identity(package)!=slot)continue;
        if(!selection::Missing(Required(package),committed))Remove(base,package,committed);
    }catch(...){/* Preserve custody if proof or safe cleanup is unavailable. */}
    RemoveDirectoryW(Folder(base).c_str());
}
inline void ReapLocal(const std::wstring& storage)
{
    if(GetFileAttributesW(Folder(storage).c_str())==INVALID_FILE_ATTRIBUTES)return;
    std::wstring header;Result result;unsigned version=0;if(ExportCommittedInventory(storage,header,result,version))Reap(storage,selection::Read(header));
}
inline tcard_crypto::Bytes Source(const std::wstring& storage,const std::wstring& package,const tcard_objects::Manifest& value)
{
    const auto note=JsonObject::Parse(package).GetNamedArray(L"Notes").GetObjectAt(0);tcard_crypto::Bytes bytes;
    if(!tcard_asset::read(sync_archive(Directory(storage),sync_string(note,L"Id")),value.name,bytes)||
        std::wstring(tcard_objects::Encode(tcard_objects::Describe(value.name,bytes)).Stringify())!=std::wstring(tcard_objects::Encode(value).Stringify()))
        throw std::runtime_error("Original reference object unavailable");return bytes;
}
inline void Stage(const std::wstring& storage,const std::wstring& base,const std::wstring& package)
{
    if(tcard_chunks::Group(storage)!=tcard_chunks::Group(base))throw std::runtime_error("Staged object authority differs");
    const auto root=Open(base,package,true),group=tcard_chunks::Group(base);
    for(const auto& value:tcard_objects::References(package)){tcard_objects::Cache cache(root,value,group);const auto missing=cache.Missing();
        if(!missing.empty()){const auto bytes=Source(storage,package,value);
            for(size_t index:missing){const auto begin=index*tcard_chunks::chunkSize,end=(std::min)(bytes.size(),begin+tcard_chunks::chunkSize);cache.Put(index,{bytes.begin()+begin,bytes.begin()+end});}}
        cache.Assemble();
    }
}
inline bool Prepare(const std::wstring& storage,const std::wstring& base,const std::wstring& package,PreparedNote& note,Result& result)
{
    note={};result={};try{
        if(tcard_chunks::Group(storage)!=tcard_chunks::Group(base))throw std::runtime_error("Prepared object authority differs");
        try{const auto root=Open(base,package);if(PrepareObjectNote(storage,package,note,result,root)){note.package=package;return true;}}catch(...){}
        // A committed packet may outlive its fragments after a lost receipt or receiver restart.
        if(Committed(storage,package)){note={sync_string(JsonObject::Parse(package).GetNamedArray(L"Notes").GetObjectAt(0),L"Id"),package};result={};return true;}
        if(result.error.empty())result.error=L"Reference objects remain unavailable";return false;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}catch(...){result.error=L"Object preparation failed";return false;}
}
inline bool Import(const std::wstring& storage,const std::wstring& base,const std::wstring& package,Result& result)
{
    result={};try{
        if(tcard_chunks::Group(storage)!=tcard_chunks::Group(base))throw std::runtime_error("Imported object authority differs");
        const PreparedNote note{sync_string(JsonObject::Parse(package).GetNamedArray(L"Notes").GetObjectAt(0),L"Id"),package};
        try{const auto root=Open(base,package);if(ImportObjectNote(storage,note,result,root))return true;}catch(...){}
        if(Committed(storage,package)){result={};return true;}
        if(result.error.empty())result.error=L"Reference objects remain unavailable";return false;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}catch(...){result.error=L"Object import failed";return false;}
}
}
