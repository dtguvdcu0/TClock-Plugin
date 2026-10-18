#pragma once
#include "card_sync_blobs.h"

// Pin original/asset membership, never worker custody or all-note native activation.
namespace tcard_bootstrap {
using namespace tcard_sync;
inline std::recursive_mutex pinMutex;
constexpr size_t maxPins=8;
inline std::wstring Node(const std::wstring& storage)
{
    tcard_asset::File directory(tcard_asset::open_dir(storage,false));BY_HANDLE_FILE_INFORMATION info{};
    wchar_t computer[MAX_COMPUTERNAME_LENGTH+1]{};DWORD length=MAX_COMPUTERNAME_LENGTH+1;
    if(!directory||!GetFileInformationByHandle(directory.value,&info)||!GetComputerNameW(computer,&length))throw std::runtime_error("Bootstrap source identity unavailable");
    return tcard_bridge::Hash(std::wstring(computer,length)+L":"+sync_path(storage)+L":"+std::to_wstring(info.dwVolumeSerialNumber)+L":"+
        std::to_wstring(info.nFileIndexHigh)+L":"+std::to_wstring(info.nFileIndexLow));
}
inline std::wstring Folder(const std::wstring& base)
{
    auto path=tcard_blobs::Folder(base);const std::wstring suffix=L"\\_object-transfers";path.resize(path.size()-suffix.size());return path+L"\\_bootstrap";
}
inline size_t Count(const std::wstring& folder)
{
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((folder+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return 0;throw std::runtime_error("Bootstrap enumeration failed");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};size_t count=0;
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        if((entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||name.size()!=69||name.substr(64)!=L".json"||!sync_hex(name.substr(0,64)))
            throw std::runtime_error("Unexpected bootstrap entry");++count;
    }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Bootstrap enumeration failed");return count;
}
inline std::wstring Read(const std::wstring& storage,const std::wstring& base,const std::wstring& slot)
{
    if(!sync_hex(slot))throw std::runtime_error("Invalid bootstrap receiver");
    const auto group=tcard_chunks::Group(storage);if(group!=tcard_chunks::Group(base))throw std::runtime_error("Bootstrap authority differs");
    std::wstring saved;if(!tcard_bridge::Read(Folder(base),slot+L".json",saved))throw std::runtime_error("Retained bootstrap unavailable");
    const auto record=JsonObject::Parse(saved);const auto header=std::wstring(record.GetNamedObject(L"Inventory").Stringify());
    if(record.GetNamedNumber(L"Version")!=1||sync_string(record,L"Group")!=group||!sync_hex(sync_string(record,L"Source"))||
        sync_string(record,L"Receiver")!=slot||sync_string(record,L"Digest")!=tcard_bridge::Hash(header))throw std::runtime_error("Retained bootstrap identity differs");
    const auto target=selection::Read(header);
    if(sync_string(record,L"Source")!=Node(storage)){
        // A same-group copied/moved source may resume only if its entire captured history survived.
        std::wstring current;Result result;unsigned version=0;
        if(!ExportCommittedInventory(storage,current,result,version)||selection::Missing(target,selection::Read(current)))
            throw std::runtime_error("Moved bootstrap source lacks captured originals");
    }
    return header;
}
inline void Finish(const std::wstring& storage,const std::wstring& base,const std::wstring& slot,const selection::Inventory& peer)
{
    if(!sync_hex(slot))throw std::runtime_error("Invalid bootstrap receiver");
    const auto folder=Folder(base),leaf=slot+L".json";if(!sync_exists(folder,leaf))return;
    std::lock_guard guard(pinMutex);SyncLock lock(folder);if(!lock)throw std::runtime_error("Bootstrap state busy");
    if(!sync_exists(folder,leaf))return;const auto header=Read(storage,base,slot);
    if(!selection::Missing(selection::Read(header),peer)){
        if(!DeleteFileW((folder+L"\\"+leaf).c_str()))throw std::runtime_error("Committed bootstrap retirement blocked");RemoveDirectoryW(folder.c_str());
    }
}
inline void Retain(const std::wstring& storage,const std::wstring& base,const std::wstring& slot,const std::wstring& authenticatedTarget)
{
    std::lock_guard guard(pinMutex);if(!sync_hex(slot))throw std::runtime_error("Invalid bootstrap receiver");
    const auto group=tcard_chunks::Group(storage);if(group!=tcard_chunks::Group(base))throw std::runtime_error("Bootstrap authority differs");
    const auto header=tcard_blobs::Canonical(authenticatedTarget);const auto target=selection::Read(header);const auto folder=Folder(base),leaf=slot+L".json";
    SyncLock lock(folder);if(!lock)throw std::runtime_error("Bootstrap state busy");
    if(sync_exists(folder,leaf)){if(tcard_blobs::Canonical(Read(storage,base,slot))!=header)throw std::runtime_error("Another native target is retained");return;}
    std::wstring current;Result result;unsigned version=0;
    if(!ExportCommittedInventory(storage,current,result,version)||selection::Missing(target,selection::Read(current)))throw std::runtime_error("Remote target originals remain incomplete");
    if(tcard_asset::encode(header).size()>tcard_asset::max_file-65536||!tcard_bridge::Protect(folder)||Count(folder)>=maxPins)throw std::runtime_error("Retained bootstrap budget reached");
    JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Group",JsonValue::CreateStringValue(group));
    record.Insert(L"Source",JsonValue::CreateStringValue(Node(storage)));record.Insert(L"Receiver",JsonValue::CreateStringValue(slot));
    record.Insert(L"Inventory",JsonObject::Parse(header));record.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(header)));
    if(!tcard_bridge::Publish(folder,leaf,std::wstring(record.Stringify()))||Read(storage,base,slot)!=header)throw std::runtime_error("Cannot retain remote native target");
}
inline bool Export(const std::wstring& storage,const std::wstring& base,const std::wstring& slot,const selection::Inventory& peer,
    std::wstring& packet,Result& result,unsigned& version,bool readonly=false,bool objects=true)
{
    packet.clear();result={};version=0;std::lock_guard guard(pinMutex);
    try{
        if(!sync_hex(slot))throw std::runtime_error("Invalid bootstrap receiver");
        const auto group=tcard_chunks::Group(storage);if(group!=tcard_chunks::Group(base))throw std::runtime_error("Bootstrap authority differs");
        const auto folder=Folder(base),leaf=slot+L".json";SyncLock lock(folder);if(!lock)throw std::runtime_error("Bootstrap state busy");
        if(sync_exists(folder,leaf)){
            const auto header=Read(storage,base,slot);
            if(selection::Missing(selection::Read(header),peer))return ExportSelection(storage,peer,packet,result,version,readonly,objects,header);
            // Only this current authenticated/committed receiver inventory releases the target.
            if(!DeleteFileW((folder+L"\\"+leaf).c_str()))throw std::runtime_error("Committed bootstrap retirement blocked");RemoveDirectoryW(folder.c_str());
        }
        if(!ExportSelection(storage,peer,packet,result,version,readonly,objects))return false;
        std::wstring delta;const auto target=selection::Unwrap(packet,delta,objects);auto covered=peer;selection::Include(covered,selection::Read(selection::Header(delta)));
        if(!selection::Missing(target,covered))return true;
        const auto header=std::wstring(JsonObject::Parse(packet).GetNamedObject(L"Inventory").Stringify());
        if(tcard_asset::encode(header).size()>tcard_asset::max_file-65536)throw std::runtime_error("Bootstrap inventory budget exceeded");
        if(!tcard_bridge::Protect(folder)||Count(folder)>=maxPins)throw std::runtime_error("Retained bootstrap budget reached");
        JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Group",JsonValue::CreateStringValue(group));
        record.Insert(L"Source",JsonValue::CreateStringValue(Node(storage)));record.Insert(L"Receiver",JsonValue::CreateStringValue(slot));
        record.Insert(L"Inventory",JsonObject::Parse(header));record.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(header)));
        if(!tcard_bridge::Publish(folder,leaf,std::wstring(record.Stringify()))||Read(storage,base,slot)!=header)throw std::runtime_error("Cannot retain bootstrap inventory");return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));packet.clear();version=0;return false;}
    catch(...){result.error=L"Bootstrap selection failed";packet.clear();version=0;return false;}
}
}
