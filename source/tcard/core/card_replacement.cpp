#include <winsock2.h>
#include "card_replacement.h"
#include "card_key_activation.h"
#include "card_native_commit.h"
#include <memory>
#include "card_replacement.inc"

namespace tcard_replacement {
namespace {
JsonObject replace_prepare(const std::wstring& native,const std::wstring& profile,const std::wstring& source,const tcard_crypto::Keys& candidate)
{
    const auto prepared=replace_root(source);for(const auto& root:{native,profile,profile+L"-control"})replace_require(!replace_contains(root,prepared)&&!replace_contains(prepared,root),"Replacement candidate overlaps owner roots");
    SyncLock sourceLock(prepared);replace_require(bool(sourceLock),"Replacement candidate busy");tcard_replacement::Candidate sealed;
    replace_require(tcard_storage::Matches(prepared,candidate)&&tcard_replacement::Inspect(prepared,sealed)&&sealed.ready,"Replacement source candidate is not sealed and complete");
    const auto files=tcard_replacement::replace_catalog(prepared);replace_require(tcard_native::Digest(files)==sealed.catalog,"Replacement candidate changed before staging");
    tcard_native::Initialize(native);const auto before=tcard_native::Selected(native),after=tcard_bridge::Hash(tcard_bridge::Token());
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((tcard_native::Control(native)+L"\\*").c_str(),&entry);replace_require(find!=INVALID_HANDLE_VALUE,"Native generation enumeration failed");size_t count=0;
    {struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};do{const std::wstring name=entry.cFileName;if(name==L"."||name==L".."||name==L"active.json")continue;
        replace_require(sync_hex(name)&&(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Unexpected native generation entry");++count;
    }while(FindNextFileW(find,&entry));replace_require(GetLastError()==ERROR_NO_MORE_FILES&&count<16,"Retained native generation budget reached");}
    const auto generation=tcard_native::Control(native)+L"\\"+after,data=tcard_native::Data(native,after);
    replace_require(!tcard_native::Exists(generation)&&tcard_bridge::Protect(generation)&&tcard_bridge::Protect(data),"Cannot protect source-only native generation");
    for(const auto& [relative,item]:files){const auto bytes=tcard_retained::ReadFile(prepared,relative);replace_require(bytes.size()==item.size&&tcard_asset::hash(bytes)==item.hash,"Replacement candidate changed during copy");
        auto folder=data;size_t at=0;for(auto slash=relative.find(L'\\');slash!=std::wstring::npos;slash=relative.find(L'\\',at)){
            folder+=L"\\"+relative.substr(at,slash-at);replace_require(tcard_bridge::Protect(folder),"Replacement generation directory unavailable");at=slash+1;}
        replace_require(tcard_asset::publish(folder,relative.substr(at),bytes,true)&&tcard_retained::ReadFile(data,relative)==bytes,"Replacement generation publication failed");}
    JsonObject scope;scope.Insert(L"Version",JsonValue::CreateNumberValue(1));scope.Insert(L"Epoch",JsonValue::CreateStringValue(after));
    replace_require(sync_write(data,L".tcard-data.scope",scope)&&selection::Read(tcard_stage::Inventory(data))==selection::Read(sealed.target),"Replacement generation target differs");
    const auto copied=tcard_retained::Collect(data,false);JsonObject record;JsonArray rows;
    for(const auto& [relative,item]:copied){JsonObject row;row.Insert(L"Path",JsonValue::CreateStringValue(relative));row.Insert(L"Hash",JsonValue::CreateStringValue(item.hash));row.Insert(L"Size",JsonValue::CreateStringValue(std::to_wstring(item.size)));rows.Append(row);}
    for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Before",before},{L"After",after},{L"SourceNode",sealed.node},
        {L"Target",sealed.target},{L"TargetDigest",tcard_bridge::Hash(sealed.target)},{L"Catalog",tcard_native::Digest(copied)}})record.Insert(name,JsonValue::CreateStringValue(value));
    record.Insert(L"Files",rows);return record;
}
}
}
namespace tcard_activation {
bool StageReplacement(const std::wstring& nativePath,const std::wstring& profilePath,const std::wstring& group,
    const tcard_crypto::Keys& candidate,const std::wstring& replacement,Status& status)
{
    status={};try{
        using namespace tcard_sync;using namespace tcard_replacement;
        const auto native=replace_root(nativePath),profile=replace_root(profilePath),prepared=replace_root(replacement);
        replace_require(native==tcard_native::Owner(native)&&!replace_contains(native,profile)&&!replace_contains(profile,native)&&!Pending(profile),"Replacement owner roots are unavailable");
        const auto mutex=L"Local\\TCardSync.Owner."+tcard_bridge::Hash(profile);tcard_asset::File owner(CreateMutexW(nullptr,TRUE,mutex.c_str()));const auto created=GetLastError();
        replace_require(bool(owner)&&created!=ERROR_ALREADY_EXISTS,"Replacement requires a stopped synchronization worker");
        struct Release{HANDLE handle;~Release(){ReleaseMutex(handle);}} release{owner.value};
        std::lock_guard guard(sync_mutex);std::vector<std::wstring> roots{native,profile,prepared};std::sort(roots.begin(),roots.end());std::vector<std::unique_ptr<SyncLock>> locks;
        for(const auto& path:roots){auto lock=std::make_unique<SyncLock>(path);replace_require(bool(*lock),"Replacement owner is busy");locks.push_back(std::move(lock));}
        tcard_crypto::Keys previous;replace_require(tcard_storage::LoadKey(native,previous)&&tcard_storage::Matches(profile,previous)&&previous.publicKey!=candidate.publicKey,"Replacement requires matching old encrypted roots and a different key");
        for(const auto& root:roots){auto ancestor=root;for(;;){tcard_asset::File directory(tcard_asset::open_dir(tcard_native::Extended(ancestor),false));replace_require(bool(directory),"Unsafe replacement root ancestor");const auto slash=ancestor.find_last_of(L'\\');if(slash<3||slash==std::wstring::npos)break;ancestor.resize(slash);}}
        JsonObject record;record.Insert(L"Native",JsonValue::CreateStringValue(native));record.Insert(L"Profile",JsonValue::CreateStringValue(profile));record.Insert(L"Group",JsonValue::CreateStringValue(group));
        record.Insert(L"Replacement",replace_prepare(native,profile,prepared,candidate));
        ReplacementProof proof(std::wstring(record.Stringify()));return StageVerified(native,profile,group,candidate,proof,status);
    }catch(const std::exception& e){status.error=e.what();return false;}catch(...){status.error="Replacement owner staging failed";return false;}
}
}
