#pragma once
#include "card_sync_bootstrap.h"
#include "card_sync_dependency_mail.h"

// Internal preparation only: caller authenticates the target; readiness never acknowledges live application.
namespace tcard_stage {
using namespace tcard_sync;
struct State { std::wstring remote,local; };
struct Progress { size_t remote=0,local=0,fresh=0;bool ready=false; };
inline std::wstring Folder(const std::wstring& owner)
{
    return tcard_bootstrap::Folder(owner)+L"-stage";
}
inline std::wstring Data(const std::wstring& owner){return Folder(owner)+L"\\data";}
inline void Check(const std::wstring& owner)
{
    tcard_chunks::Group(owner);
    // Inherit the one owner nonce allocator; never accept copied nested key/counter controls.
    for(const auto& root:{Folder(owner),Data(owner)}){
        for(const auto* leaf:{L".tcard-vault.key",L".tcard-vault.required",L".tcard-vault.pending"})
            if(sync_exists(root,leaf))throw std::runtime_error("Nested stage encryption controls");
        if(GetFileAttributesW(root.c_str())!=INVALID_FILE_ATTRIBUTES){tcard_asset::File folder(tcard_asset::open_dir(root,false));if(!folder)throw std::runtime_error("Unsafe staging directory");}
    }
}
// Only an empty directory can precede the durable target. Preserve every unknown entry.
inline bool Unstarted(const std::wstring& owner)
{
    Check(owner);const auto folder=Folder(owner);const auto attributes=GetFileAttributesW(folder.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;throw std::runtime_error("Stage directory unavailable");}
    WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW((folder+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return true;throw std::runtime_error("Stage enumeration failed");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{const std::wstring name=item.cFileName;if(name!=L"."&&name!=L"..")return false;}while(FindNextFileW(handle,&item));
    if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Stage enumeration interrupted");return true;
}
inline std::wstring Inventory(const std::wstring& path)
{
    std::wstring header;Result result;unsigned version=0;
    if(!ExportCommittedInventory(path,header,result,version))throw std::runtime_error("Stage committed inventory unavailable");return header;
}
inline selection::Inventory Expected(const State& state)
{
    auto expected=selection::Read(state.remote);selection::Include(expected,selection::Read(state.local));return expected;
}
inline State Read(const std::wstring& owner,bool requireBaseline=true)
{
    Check(owner);std::wstring text;if(!tcard_bridge::Read(Folder(owner),L"target.json",text))throw std::runtime_error("Stage target unavailable");
    const auto record=JsonObject::Parse(text);State state{std::wstring(record.GetNamedObject(L"Remote").Stringify()),std::wstring(record.GetNamedObject(L"Local").Stringify())};
    if(record.GetNamedNumber(L"Version")!=1||sync_string(record,L"Group")!=tcard_chunks::Group(owner)||sync_string(record,L"Owner")!=tcard_bootstrap::Node(owner)||
        sync_string(record,L"RemoteDigest")!=tcard_bridge::Hash(state.remote)||sync_string(record,L"LocalDigest")!=tcard_bridge::Hash(state.local))throw std::runtime_error("Stage target identity differs");
    // A partial full-native baseline must not be seeded or merged as a usable causal snapshot.
    if(requireBaseline&&sync_exists(Folder(owner),L"native.json")){
        std::wstring catalog,sealed;if(!tcard_bridge::Read(Folder(owner),L"native.json",catalog)||!tcard_bridge::Read(Folder(owner),L"native-ready.json",sealed))throw std::runtime_error("Native baseline copy incomplete");
        const auto seal=JsonObject::Parse(sealed);if(seal.GetNamedNumber(L"Version")!=1||sync_string(seal,L"Catalog")!=tcard_bridge::Hash(catalog))throw std::runtime_error("Native baseline seal differs");
    }
    Expected(state);return state;
}
inline void Write(const std::wstring& owner,const State& state)
{
    Expected(state);JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Group",JsonValue::CreateStringValue(tcard_chunks::Group(owner)));
    record.Insert(L"Owner",JsonValue::CreateStringValue(tcard_bootstrap::Node(owner)));
    record.Insert(L"Remote",JsonObject::Parse(state.remote));record.Insert(L"Local",JsonObject::Parse(state.local));
    record.Insert(L"RemoteDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.remote)));record.Insert(L"LocalDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.local)));
    const auto text=std::wstring(record.Stringify());if(tcard_asset::encode(text).size()>tcard_asset::max_file)throw std::runtime_error("Stage metadata budget exceeded");
    if(!tcard_bridge::Publish(Folder(owner),L"target.json",text))throw std::runtime_error("Cannot retain stage target");
    const auto saved=Read(owner);if(saved.local!=state.local||saved.remote!=state.remote)throw std::runtime_error("Stage target verification failed");
}
inline void Begin(const std::wstring& owner,const std::wstring& authenticatedTarget)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Stage owner busy");Check(owner);
    const auto remote=tcard_blobs::Canonical(authenticatedTarget);selection::Read(remote);
    if(!Unstarted(owner)){if(Read(owner,false).remote!=remote)throw std::runtime_error("Another stage target is retained");
        if(GetFileAttributesW(Data(owner).c_str())==INVALID_FILE_ATTRIBUTES&&!tcard_bridge::Protect(Data(owner)))throw std::runtime_error("Cannot resume staged native data");return;}
    const State state{remote,Inventory(owner)};
    if(!tcard_bridge::Protect(Folder(owner)))throw std::runtime_error("Cannot protect snapshot stage");Write(owner,state);
    if(!tcard_bridge::Protect(Data(owner)))throw std::runtime_error("Cannot protect staged native data");
}
inline void Refresh(const std::wstring& owner)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Stage owner busy");auto state=Read(owner);const auto current=Inventory(owner);
    if(selection::Missing(selection::Read(state.local),selection::Read(current)))throw std::runtime_error("Captured local originals unavailable");
    if(current!=state.local){state.local=current;Write(owner,state);}
}
inline Progress Inspect(const std::wstring& owner)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Stage owner busy");const auto state=Read(owner);
    const auto staged=selection::Read(Inventory(Data(owner))),current=selection::Read(Inventory(owner));
    if(selection::Missing(staged,Expected(state))||selection::Missing(selection::Read(state.local),current))throw std::runtime_error("Stage contains unexpected or lost originals");
    Progress result;result.remote=selection::Missing(selection::Read(state.remote),staged);result.local=selection::Missing(selection::Read(state.local),staged);
    result.fresh=selection::Missing(current,staged);result.ready=!result.remote&&!result.local&&!result.fresh;return result;
}
inline bool Apply(const std::wstring& owner,const std::wstring& package,const selection::Inventory& permitted,const std::wstring& objectBase,Result& result)
{
    result={};const auto required=selection::Validate(package,permitted,true);const auto staged=selection::Read(Inventory(Data(owner)));
    if(!selection::Missing(required,staged))return true;
    if(!tcard_bridge::Protect(Data(owner)))throw std::runtime_error("Cannot open staged native data");
    return tcard_objects::Referenced(package)?tcard_blobs::Import(Data(owner),objectBase,package,result):ImportChanges(Data(owner),package,result);
}
inline bool Seed(const std::wstring& owner,Result& result)
{
    result={};try{
        std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Stage owner busy");const auto state=Read(owner);
        const auto staged=selection::Read(Inventory(Data(owner)));if(!selection::Missing(selection::Read(state.local),staged))return true;
        std::wstring packet,delta;unsigned version=0;
        if(!ExportSelection(owner,staged,packet,result,version,true,true,state.local))return false;selection::Unwrap(packet,delta,true);
        if(tcard_objects::Referenced(delta))tcard_blobs::Stage(owner,Folder(owner),delta);
        const auto okay=Apply(owner,delta,selection::Read(state.local),Folder(owner),result);
        if(okay&&!result.pending&&!result.partial)tcard_blobs::Reap(Folder(owner),selection::Read(Inventory(Data(owner))));return okay;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
}
inline bool Receive(const std::wstring& owner,const std::wstring& authenticatedDelta,const std::wstring& objectBase,Result& result,const tcard_dependencies::Context* dependency=nullptr)
{
    result={};try{
        std::unique_ptr<tcard_dependency_mail::Guard> boundary;if(dependency)boundary=std::make_unique<tcard_dependency_mail::Guard>(Data(owner),objectBase);
        std::lock_guard guard(sync_mutex);SyncLock lock(owner);if(!lock)throw std::runtime_error("Stage owner busy");const auto state=Read(owner);
        if(dependency){
            if(tcard_blobs::Canonical(dependency->target)!=state.remote)throw std::runtime_error("Dependency stage target differs");
            const auto manifest=tcard_dependencies::Read(authenticatedDelta,*dependency);const auto required=tcard_dependency_mail::Required(manifest);
            if(selection::Missing(required,selection::Read(state.remote)))throw std::runtime_error("Dependency outside native stage target");
            if(!selection::Missing(required,selection::Read(Inventory(Data(owner)))))return true;
            tcard_dependencies::Pool pool(objectBase,authenticatedDelta,*dependency);PreparedNote note;
            return PrepareDependencyNote(Data(owner),authenticatedDelta,*dependency,pool,note,result)&&ImportDependencyNote(Data(owner),note,*dependency,pool,result);
        }
        return Apply(owner,authenticatedDelta,selection::Read(state.remote),objectBase,result);
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
}
}
