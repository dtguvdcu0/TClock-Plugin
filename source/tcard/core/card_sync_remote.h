#pragma once
#include "card_sync_bootstrap.h"
#include "card_sync_dependencies.h"
#include <atomic>
#include <cmath>
#include <chrono>

// Authenticated source membership survives replica custody until the native owner applies it.
namespace tcard_remote {
using namespace tcard_sync;
constexpr size_t maxTargets=8;
inline std::recursive_mutex receiveMutex;
inline std::atomic<ULONGLONG> revision=1;
struct Target {std::wstring leaf,source,receiver,header,digest;ULONGLONG sequence=0;};
inline std::wstring Folder(const std::wstring& storage){return tcard_bootstrap::Folder(storage)+L"-receive";}
inline std::wstring Header(const selection::Inventory& inventory)
{
    JsonObject root;JsonArray notes;std::set<std::wstring> names;for(const auto& value:inventory.operations)names.insert(value.first);for(const auto& value:inventory.assets)names.insert(value.first);
    for(const auto& name:names){JsonObject note;JsonArray operations,assets;note.Insert(L"Id",JsonValue::CreateStringValue(name));
        for(const auto& value:inventory.operations)if(value.first==name)operations.Append(JsonValue::CreateStringValue(value.second));
        for(const auto& value:inventory.assets)if(value.first==name)assets.Append(JsonValue::CreateStringValue(value.second));
        note.Insert(L"Changes",operations);note.Insert(L"Assets",assets);notes.Append(note);}
    root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Notes",notes);const auto text=std::wstring(root.Stringify());selection::Read(text);return text;
}
inline std::wstring SourceFolder(const std::wstring& storage){return tcard_bootstrap::Folder(storage)+L"-source";}
// Only remote metadata uses this publication protocol. Content and common asset publication are unchanged.
inline bool PublicationLeaf(const std::wstring& leaf,bool source)
{
    return source?(leaf==L"state.json"||leaf==L"required.json"):
        (leaf.size()==69&&leaf.substr(64)==L".json"&&sync_hex(leaf.substr(0,64)));
}
inline void PublicationInventory(const std::wstring& text)
{
    selection::Read(text);const auto root=JsonObject::Parse(text);if(root.Size()!=2)throw std::runtime_error("Publication inventory is not metadata");
    for(const auto& value:root.GetNamedArray(L"Notes"))if(value.GetObject().Size()!=3)throw std::runtime_error("Publication note is not metadata");
}
inline void PublicationRecord(const std::wstring& storage,const std::wstring& leaf,const std::wstring& text,bool source)
{
    const auto group=tcard_chunks::Group(storage);
    if(source&&leaf==L"required.json"){if(text!=group)throw std::runtime_error("Publication requirement differs");return;}
    const auto root=JsonObject::Parse(text);if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Group")!=group)throw std::runtime_error("Publication group differs");
    if(source){if(root.Size()!=10||!sync_hex(sync_string(root,L"Owner"))||(!sync_string(root,L"Origin").empty()&&!sync_hex(sync_string(root,L"Origin"))))throw std::runtime_error("Publication frontier identity differs");
        for(const auto* name:{L"Safe",L"Native",L"Deferred"}){const auto inventory=std::wstring(root.GetNamedObject(name).Stringify());PublicationInventory(inventory);
            if(sync_string(root,(std::wstring(name)+L"Digest").c_str())!=tcard_bridge::Hash(inventory))throw std::runtime_error("Publication frontier digest differs");}}
    else{const auto inventory=tcard_blobs::Canonical(std::wstring(root.GetNamedObject(L"Target").Stringify())),digest=tcard_bridge::Hash(inventory),origin=sync_string(root,L"Source");const auto sequence=root.GetNamedNumber(L"Sequence");
        if(root.Size()!=9||sync_string(root,L"Kind")!=L"remote-bootstrap"||!sync_hex(origin)||!sync_hex(sync_string(root,L"Receiver"))||sync_string(root,L"TargetDigest")!=digest||
            leaf!=tcard_bridge::Hash(origin+L":"+digest)+L".json"||!std::isfinite(sequence)||sequence<1||sequence>9007199254740991.0||sequence!=std::floor(sequence))throw std::runtime_error("Publication target identity differs");
        root.GetNamedBoolean(L"Atomic");PublicationInventory(inventory);}
}
inline bool PublicationRead(const std::wstring& folder,const std::wstring& physical,const std::wstring& aad,std::wstring& text)
{
    text.clear();tcard_asset::File directory(tcard_asset::open_dir(folder,false));if(!directory)throw std::runtime_error("Unsafe publication directory");tcard_asset::File file(CreateFileW((folder+L"\\"+physical).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file)return false;BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!tcard_asset::regular(file.value,false)||!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1||!GetFileSizeEx(file.value,&size)||size.QuadPart<0||static_cast<ULONGLONG>(size.QuadPart)>tcard_asset::max_file+tcard_crypto::envelopeOverhead)
        throw std::runtime_error("Unsafe publication entry");
    tcard_crypto::Bytes bytes(static_cast<size_t>(size.QuadPart)),plain;DWORD count=0;
    if(!bytes.empty()&&(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size()))throw std::runtime_error("Publication read failed");
    if(!tcard_storage::Unpack(folder+L"\\"+aad,bytes,plain)||plain.size()>tcard_asset::max_file)return false;
    text=tcard_asset::decode(plain);return tcard_asset::encode(text)==plain;
}
inline void PublicationWrite(const std::wstring& folder,const std::wstring& physical,const std::wstring& aad,const std::wstring& text,bool recreate=false)
{
    tcard_asset::File directory(tcard_asset::open_dir(folder,false));if(!directory)throw std::runtime_error("Unsafe publication directory");
    const auto plain=tcard_asset::encode(text);if(plain.size()>tcard_asset::max_file-65536)throw std::runtime_error("Publication metadata budget exceeded");tcard_crypto::Bytes bytes;
    if(!tcard_storage::Pack(folder+L"\\"+aad,plain,bytes))throw std::runtime_error("Publication encryption unavailable");
    tcard_asset::File file(CreateFileW((folder+L"\\"+physical).c_str(),GENERIC_WRITE,0,nullptr,recreate?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));DWORD count=0;BY_HANDLE_FILE_INFORMATION info{};
    if(!file||!tcard_asset::regular(file.value,false)||!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1)throw std::runtime_error("Unsafe publication write entry");
    LARGE_INTEGER beginning{};if(recreate&&(!SetFilePointerEx(file.value,beginning,nullptr,FILE_BEGIN)||!SetEndOfFile(file.value)))throw std::runtime_error("Publication reconstruction blocked");
    // Keep even a partial write: only independent complete intent can authorize its reconstruction.
    if((!bytes.empty()&&(!WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size()))||!FlushFileBuffers(file.value))throw std::runtime_error("Publication flush failed");
}
inline std::wstring PublicationIntent(const std::wstring& storage,const std::wstring& leaf,const std::wstring& record,bool present,const std::wstring& old)
{
    JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Kind",JsonValue::CreateStringValue(L"remote-publication"));root.Insert(L"Group",JsonValue::CreateStringValue(tcard_chunks::Group(storage)));
    root.Insert(L"Owner",JsonValue::CreateStringValue(tcard_bootstrap::Node(storage)));root.Insert(L"Leaf",JsonValue::CreateStringValue(leaf));root.Insert(L"OldExists",JsonValue::CreateBooleanValue(present));root.Insert(L"OldDigest",JsonValue::CreateStringValue(present?tcard_bridge::Hash(old):L""));
    root.Insert(L"Record",JsonValue::CreateStringValue(record));root.Insert(L"RecordDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(record)));const auto text=std::wstring(root.Stringify());
    if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Publication intent budget exceeded");return text;
}
inline JsonObject PublicationProof(const std::wstring& storage,const std::wstring& leaf,const std::wstring& text,bool source)
{
    const auto root=JsonObject::Parse(text);const auto record=sync_string(root,L"Record");const bool present=root.GetNamedBoolean(L"OldExists");const auto old=sync_string(root,L"OldDigest");
    if(root.Size()!=9||root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"remote-publication"||sync_string(root,L"Group")!=tcard_chunks::Group(storage)||sync_string(root,L"Owner")!=tcard_bootstrap::Node(storage)||
        sync_string(root,L"Leaf")!=leaf||sync_string(root,L"RecordDigest")!=tcard_bridge::Hash(record)||(present?!sync_hex(old):!old.empty()))throw std::runtime_error("Publication intent identity differs");
    PublicationRecord(storage,leaf,record,source);return root;
}
inline void PublicationMove(const std::wstring& folder,const std::wstring& from,const std::wstring& to,bool replace=false)
{
    if(!MoveFileExW((folder+L"\\"+from).c_str(),(folder+L"\\"+to).c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0)))throw std::runtime_error("Publication activation blocked");
}
inline void PublicationDelete(const std::wstring& folder,const std::wstring& leaf)
{
    if(sync_exists(folder,leaf)&&!DeleteFileW((folder+L"\\"+leaf).c_str()))throw std::runtime_error("Publication cleanup blocked");
}
inline void PublicationRecover(const std::wstring& storage,const std::wstring& folder,const std::wstring& leaf,bool source)
{
    std::lock_guard guard(receiveMutex);const auto intent=leaf+L".intent",initial=intent+L".pending",pending=leaf+L".pending";std::wstring proof,first;
    const bool hasIntent=sync_exists(folder,intent),hasInitial=sync_exists(folder,initial),hasPending=sync_exists(folder,pending);
    if(!hasIntent&&!hasInitial){if(hasPending)throw std::runtime_error("Publication candidate lacks proof");return;}
    if(hasIntent){if(!PublicationRead(folder,intent,intent,proof))throw std::runtime_error("Publication intent unreadable");PublicationProof(storage,leaf,proof,source);}
    if(hasInitial){if(!PublicationRead(folder,initial,intent,first))throw std::runtime_error("Initial publication intent lacks proof");PublicationProof(storage,leaf,first,source);
        if(hasIntent){if(proof!=first)throw std::runtime_error("Publication intents differ");}else proof=first;}
    const auto root=PublicationProof(storage,leaf,proof,source);const auto record=sync_string(root,L"Record");std::wstring current;
    const bool present=sync_exists(folder,leaf);if(present&&!PublicationRead(folder,leaf,leaf,current))throw std::runtime_error("Publication final unreadable");
    const bool activated=present&&current==record;
    if(!activated&&(present!=root.GetNamedBoolean(L"OldExists")||(present&&tcard_bridge::Hash(current)!=sync_string(root,L"OldDigest"))))throw std::runtime_error("Publication old state changed");
    // Validate old/final before advancing any pending name. No raw-replica fallback or final-corruption repair.
    if(hasInitial&&!hasIntent)PublicationMove(folder,initial,intent);
    std::wstring candidate;bool incomplete=false;if(hasPending){const bool complete=PublicationRead(folder,pending,leaf,candidate);
        if(complete&&candidate!=record)throw std::runtime_error("Publication candidate differs from proof");
        if(!complete){incomplete=true;candidate.clear();}}
    if(!activated){if(candidate.empty())PublicationWrite(folder,pending,leaf,record,incomplete);std::wstring verified;
        if(!PublicationRead(folder,pending,leaf,verified)||verified!=record)throw std::runtime_error("Publication candidate verification failed");PublicationMove(folder,pending,leaf,present);}
    std::wstring verified;if(!PublicationRead(folder,leaf,leaf,verified)||verified!=record)throw std::runtime_error("Publication final verification failed");
    PublicationDelete(folder,pending);PublicationDelete(folder,initial);PublicationDelete(folder,intent);++revision;
}
inline std::vector<std::wstring> PublicationPreflight(const std::wstring& storage,const std::wstring& folder,bool source)
{
    std::lock_guard guard(receiveMutex);WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((folder+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return {};throw std::runtime_error("Publication enumeration failed");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};tcard_asset::File directory(tcard_asset::open_dir(folder,false));if(!directory)throw std::runtime_error("Unsafe publication directory");std::set<std::wstring> finals,transactions,leaves;size_t count=0;
    do{std::wstring leaf=entry.cFileName;if(leaf==L"."||leaf==L"..")continue;
        if((entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||entry.nFileSizeHigh||entry.nFileSizeLow>tcard_asset::max_file+tcard_crypto::envelopeOverhead||++count>(source?8:maxTargets*4))throw std::runtime_error("Unexpected publication entry");
        bool auxiliary=false;for(const auto* suffix:{L".intent.pending",L".intent",L".pending"}){const std::wstring tail=suffix;if(leaf.size()>tail.size()&&leaf.substr(leaf.size()-tail.size())==tail){leaf.resize(leaf.size()-tail.size());auxiliary=true;break;}}
        if(!PublicationLeaf(leaf,source))throw std::runtime_error("Unknown publication entry");leaves.insert(leaf);if(auxiliary)transactions.insert(leaf);else finals.insert(leaf);
    }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Publication enumeration failed");
    if(leaves.size()>(source?2:maxTargets))throw std::runtime_error("Publication target budget exceeded");
    for(const auto& leaf:transactions){PublicationRecover(storage,folder,leaf,source);finals.insert(leaf);}return {finals.begin(),finals.end()};
}
inline void PublicationPublish(const std::wstring& storage,const std::wstring& folder,const std::wstring& leaf,const std::wstring& record,bool source)
{
    std::lock_guard guard(receiveMutex);PublicationRecord(storage,leaf,record,source);PublicationPreflight(storage,folder,source);std::wstring old;const bool present=sync_exists(folder,leaf);
    if(present&&!PublicationRead(folder,leaf,leaf,old))throw std::runtime_error("Publication old record unreadable");if(present&&old==record)return;
    if(!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect publication directory");
    const auto intent=leaf+L".intent",proof=PublicationIntent(storage,leaf,record,present,old);PublicationWrite(folder,intent+L".pending",intent,proof);
    // Intent publishes itself directly; it is never recursively wrapped in another intent.
    PublicationRecover(storage,folder,leaf,source);
}
inline std::vector<Target> Read(const std::wstring& storage,const selection::Inventory* relocation=nullptr)
{
    std::lock_guard guard(receiveMutex);const auto folder=Folder(storage),group=tcard_chunks::Group(storage),receiver=tcard_bootstrap::Node(storage);
    std::vector<Target> targets;std::set<ULONGLONG> sequences;
    for(const auto& leaf:PublicationPreflight(storage,folder,false)){
        std::wstring text;if(!tcard_bridge::Read(folder,leaf,text))throw std::runtime_error("Retained remote target unreadable");
        const auto root=JsonObject::Parse(text);Target target;target.leaf=leaf;target.source=sync_string(root,L"Source");target.receiver=sync_string(root,L"Receiver");target.header=tcard_blobs::Canonical(std::wstring(root.GetNamedObject(L"Target").Stringify()));target.digest=tcard_bridge::Hash(target.header);
        const auto sequence=root.GetNamedNumber(L"Sequence");
        if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"remote-bootstrap"||sync_string(root,L"Group")!=group||!sync_hex(target.receiver)||
            !sync_hex(target.source)||sync_string(root,L"TargetDigest")!=target.digest||leaf!=tcard_bridge::Hash(target.source+L":"+target.digest)+L".json"||
            sequence<1||sequence>9007199254740991.0||sequence!=std::floor(sequence))throw std::runtime_error("Retained remote target identity differs");
        if(target.receiver!=receiver){selection::Inventory current;
            if(!relocation){std::wstring header;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,header,result,version))throw std::runtime_error("Moved receiver inventory unavailable");current=selection::Read(header);relocation=&current;}
            if(selection::Missing(selection::Read(target.header),*relocation))throw std::runtime_error("Moved receiver lacks captured originals");
            // Do not retain the address of this loop-local proof for the next record.
            if(relocation==&current)relocation=nullptr;
        }
        target.sequence=static_cast<ULONGLONG>(sequence);if(!sequences.insert(target.sequence).second)throw std::runtime_error("Remote target ordering differs");selection::Read(target.header);targets.push_back(std::move(target));
    }
    std::sort(targets.begin(),targets.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});return targets;
}
struct Source {std::wstring safe,native,origin,deferred,owner;bool exists=false;};
inline Source ReadSource(const std::wstring& storage)
{
    std::lock_guard guard(receiveMutex);const auto folder=SourceFolder(storage);Source state;std::wstring text;
    // Six deterministic existence checks avoid idle enumeration/decryption of publication auxiliaries.
    bool pending=false;for(const auto* leaf:{L"state.json",L"required.json"})for(const auto* suffix:{L".pending",L".intent",L".intent.pending"})pending|=sync_exists(folder,std::wstring(leaf)+suffix);
    if(pending)PublicationPreflight(storage,folder,true);
    if(!tcard_storage::Required(storage)){if(sync_exists(folder,L"state.json")||sync_exists(folder,L"required.json"))throw std::runtime_error("Source frontier encryption unavailable");return state;}
    if(!sync_exists(folder,L"state.json")){
        if(sync_exists(folder,L"required.json")||!Read(storage).empty())throw std::runtime_error("Required source frontier unavailable");return state;
    }
    if(!tcard_bridge::Read(folder,L"state.json",text))throw std::runtime_error("Source frontier unreadable");const auto root=JsonObject::Parse(text);
    state.safe=std::wstring(root.GetNamedObject(L"Safe").Stringify());state.native=std::wstring(root.GetNamedObject(L"Native").Stringify());state.origin=sync_string(root,L"Origin");state.owner=sync_string(root,L"Owner");state.deferred=root.HasKey(L"Deferred")?std::wstring(root.GetNamedObject(L"Deferred").Stringify()):Header({});
    if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Group")!=tcard_chunks::Group(storage)||!sync_hex(state.owner)||
        sync_string(root,L"SafeDigest")!=tcard_bridge::Hash(state.safe)||sync_string(root,L"NativeDigest")!=tcard_bridge::Hash(state.native)||(!state.origin.empty()&&!sync_hex(state.origin)))
        throw std::runtime_error("Source frontier identity differs");
    if(root.HasKey(L"Deferred")&&sync_string(root,L"DeferredDigest")!=tcard_bridge::Hash(state.deferred))throw std::runtime_error("Deferred frontier digest differs");
    selection::Read(state.safe);selection::Read(state.native);selection::Read(state.deferred);state.exists=true;
    if(sync_exists(folder,L"required.json")){std::wstring marker;if(!tcard_bridge::Read(folder,L"required.json",marker)||marker!=tcard_chunks::Group(storage))throw std::runtime_error("Source frontier requirement differs");}
    if(state.owner!=tcard_bootstrap::Node(storage)){
        std::wstring header;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,header,result,version))throw std::runtime_error("Moved source inventory unavailable");const auto actual=selection::Read(header);
        auto required=selection::Read(state.safe);selection::Include(required,selection::Read(state.native));selection::Include(required,selection::Read(state.deferred));
        if(selection::Missing(required,actual))throw std::runtime_error("Moved source lacks retained originals");Read(storage,&actual);
    }
    return state;
}
inline void WriteSource(const std::wstring& storage,Source& state)
{
    std::lock_guard admission(receiveMutex);const auto folder=SourceFolder(storage),group=tcard_chunks::Group(storage),owner=tcard_bootstrap::Node(storage);
    if(!state.owner.empty()&&state.owner!=owner){
        std::lock_guard guard(sync_mutex);SyncLock lock(storage);if(!lock)throw std::runtime_error("Moved source rebind busy");
        std::wstring current;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,current,result,version))throw std::runtime_error("Moved source rebind inventory unavailable");const auto actual=selection::Read(current);
        auto required=selection::Read(state.safe);selection::Include(required,selection::Read(state.native));selection::Include(required,selection::Read(state.deferred));
        if(selection::Missing(required,actual))throw std::runtime_error("Moved source rebind lacks retained originals");
        // Rebind receivers first and the frontier owner last. Mixed bindings remain resumable only with the same full proof.
        for(const auto& target:Read(storage,&actual)){if(target.receiver==owner)continue;std::wstring text;if(!tcard_bridge::Read(Folder(storage),target.leaf,text))throw std::runtime_error("Moved receiver record unavailable");auto record=JsonObject::Parse(text);record.Insert(L"Receiver",JsonValue::CreateStringValue(owner));const auto next=std::wstring(record.Stringify());const auto before=record.GetNamedNumber(L"Sequence");
            PublicationPublish(storage,Folder(storage),target.leaf,next,false);std::wstring verified;if(!tcard_bridge::Read(Folder(storage),target.leaf,verified)||verified!=next||JsonObject::Parse(verified).GetNamedNumber(L"Sequence")!=before)throw std::runtime_error("Moved receiver rebind verification failed");}
    }
    JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Group",JsonValue::CreateStringValue(group));root.Insert(L"Owner",JsonValue::CreateStringValue(owner));
    root.Insert(L"Safe",JsonObject::Parse(state.safe));root.Insert(L"Native",JsonObject::Parse(state.native));root.Insert(L"Origin",JsonValue::CreateStringValue(state.origin));root.Insert(L"SafeDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.safe)));root.Insert(L"NativeDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.native)));
    if(state.deferred.empty())state.deferred=Header({});root.Insert(L"Deferred",JsonObject::Parse(state.deferred));root.Insert(L"DeferredDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.deferred)));
    const auto text=std::wstring(root.Stringify());if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Source frontier budget exceeded");
    if(!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect source frontier");std::wstring old;
    const bool present=sync_exists(folder,L"state.json");const bool readable=tcard_bridge::Read(folder,L"state.json",old);if(present&&!readable)throw std::runtime_error("Source frontier changed during publication");
    if(!readable||old!=text)PublicationPublish(storage,folder,L"state.json",text,true);
    if(!sync_exists(folder,L"required.json"))PublicationPublish(storage,folder,L"required.json",group,true);
    const auto verified=ReadSource(storage);if(verified.safe!=state.safe||verified.native!=state.native||verified.origin!=state.origin||verified.deferred!=state.deferred)throw std::runtime_error("Source frontier verification failed");state.exists=true;state.owner=owner;
}
inline Source EnsureSource(const std::wstring& storage,const std::wstring& committed)
{
    auto state=ReadSource(storage);if(!state.exists){state.safe=Header(selection::Read(committed));state.native=Header({});WriteSource(storage,state);}else if(state.owner!=tcard_bootstrap::Node(storage))WriteSource(storage,state);return state;
}
inline void Close(const std::wstring& storage,selection::Inventory& safe,selection::Inventory& deferred)
{
    // Committed ordinary work may still depend on an unpublished source prefix. Keep it durable until its parents are admitted.
    using Operation=std::pair<std::wstring,std::wstring>;std::map<Operation,size_t> waiting;std::map<Operation,std::vector<Operation>> children;std::vector<Operation> ready;
    for(auto at=deferred.operations.begin();at!=deferred.operations.end();){if(safe.operations.contains(*at))at=deferred.operations.erase(at);else ++at;}
    for(const auto& operation:deferred.operations){JsonObject record;
        if(!sync_read(sync_archive(storage,operation.first),operation.second+L".json",record)||sync_string(record,L"Id")!=operation.second||sync_string(record,L"Note")!=operation.first)throw std::runtime_error("Deferred operation unavailable");
        const auto parents=record.GetNamedArray(L"Parents");size_t missing=0;for(unsigned i=0;i<parents.Size();++i){const Operation parent{operation.first,std::wstring(parents.GetStringAt(i))};
            if(!safe.operations.contains(parent)){++missing;children[parent].push_back(operation);}}
        waiting[operation]=missing;if(!missing)ready.push_back(operation);
    }
    for(size_t index=0;index<ready.size();++index){const auto operation=ready[index];safe.operations.insert(operation);deferred.operations.erase(operation);
        for(const auto& child:children[operation]){auto& count=waiting.at(child);if(count&&!--count)ready.push_back(child);}}
    std::set<std::wstring> blocked;for(const auto& operation:deferred.operations)blocked.insert(operation.first);
    for(auto at=deferred.assets.begin();at!=deferred.assets.end();){if(!blocked.contains(at->first)){safe.assets.insert(*at);at=deferred.assets.erase(at);}else ++at;}
}
inline std::wstring Promote(const std::wstring& storage,const std::wstring& committed,Source& state)
{
    if(!state.exists)return committed;const auto actual=selection::Read(committed);auto safe=selection::Read(state.safe);
    if(!std::includes(actual.operations.begin(),actual.operations.end(),safe.operations.begin(),safe.operations.end()))throw std::runtime_error("Admitted originals no longer available");
    const auto local=selection::Read(state.native);if(!selection::Missing(local,actual)){selection::Include(safe,local);state.native=Header({});state.origin.clear();}
    std::set<std::wstring> blockedSources;for(const auto& target:Read(storage)){if(blockedSources.contains(target.source))continue;const auto expected=selection::Read(target.header);
        if(selection::Missing(expected,actual))blockedSources.insert(target.source);else selection::Include(safe,expected);}
    auto deferred=selection::Read(state.deferred);if(!std::includes(actual.operations.begin(),actual.operations.end(),deferred.operations.begin(),deferred.operations.end()))throw std::runtime_error("Deferred originals no longer available");Close(storage,safe,deferred);
    state.safe=Header(safe);state.deferred=Header(deferred);WriteSource(storage,state);
    // Retain durable admitted boundaries; withhold physically absent assets only in the fresh advertisement.
    for(auto at=safe.assets.begin();at!=safe.assets.end();)if(!actual.assets.contains(*at))at=safe.assets.erase(at);else ++at;
    return Header(safe);
}
inline bool Inventory(const std::wstring& storage,std::wstring& header,Result& result,unsigned& version)
{
    std::lock_guard guard(receiveMutex);
    if(!ExportInventory(storage,header,result,version))return false;
    try{if(tcard_storage::Required(storage)){auto state=ReadSource(storage);header=Promote(storage,header,state);}return true;}
    catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));header.clear();return false;}catch(...){result.error=L"Source admission failed";header.clear();return false;}
}
// Ephemeral NEW-capability offer budget, never expiry of retained custody or offline intent.
struct DependencyWindow {
    using Clock=std::chrono::steady_clock;Clock::time_point expires{};bool begun=false;
    void Begin(Clock::time_point now,bool stopped)
    {
        if(begun||stopped)throw std::runtime_error("Dependency offer cannot restart");
        begun=true;expires=now+std::chrono::seconds(tcard_objects::maxObjects*60);
    }
    void Check(Clock::time_point now,bool stopped)const
    {
        if(!begun||stopped||now>=expires)throw std::runtime_error("Dependency offer cancelled or timed out");
    }
    Clock::time_point Limit(Clock::time_point now,std::chrono::seconds budget,bool stopped)const
    {
        Check(now,stopped);return (std::min)(expires,now+budget);
    }
};
// Dependency manifests carry logical requirements, not a legacy inline/reference delta or an ACK.
inline bool DependencyPacket(const std::wstring& packet)
{
    const auto root=JsonObject::Parse(packet);return root.HasKey(L"Delta")&&root.GetNamedObject(L"Delta").HasKey(L"DependencyVersion");
}
inline selection::Inventory DependencyUnwrap(const std::wstring& packet,const tcard_dependencies::Context& context,std::wstring& delta)
{
    const auto root=JsonObject::Parse(packet);const auto advertised=selection::Read(std::wstring(root.GetNamedObject(L"Inventory").Stringify()));
    delta=std::wstring(root.GetNamedObject(L"Delta").Stringify());const auto manifest=tcard_dependencies::Read(delta,context);const auto required=tcard_dependencies::Required(manifest);
    if(selection::Missing(required,advertised))throw std::runtime_error("Unadvertised dependency original or image");return advertised;
}
inline void DependencyBoundary(const std::wstring& storage,const std::wstring& source,const std::wstring& target,const tcard_dependencies::Context& context)
{
    tcard_dependencies::CheckContext(context);
    if(context.source!=source||context.receiver!=tcard_bootstrap::Node(storage)||context.group!=tcard_chunks::Group(storage)||context.target!=target)
        throw std::runtime_error("Independent dependency boundary differs");
}
inline void Admit(const std::wstring& storage,const std::wstring& delta,const selection::Inventory* membership=nullptr)
{
    std::lock_guard guard(receiveMutex);auto state=ReadSource(storage);if(!state.exists)return;
    const auto required=membership?*membership:selection::Read(selection::Header(delta));std::wstring current;Result result;unsigned version=0;
    if(!ExportCommittedInventory(storage,current,result,version)||selection::Missing(required,selection::Read(current)))throw std::runtime_error("Ordinary package admission remains incomplete");
    auto deferred=selection::Read(state.deferred);selection::Include(deferred,required);state.deferred=Header(deferred);Promote(storage,current,state);
}
inline void AdmitDependency(const std::wstring& storage,const std::wstring& package,const tcard_dependencies::Context& context)
{
    const auto manifest=tcard_dependencies::Read(package,context);const auto required=tcard_dependencies::Required(manifest);Admit(storage,package,&required);
}
inline bool Native(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& packet,const tcard_dependencies::Context* context=nullptr)
{
    std::lock_guard guard(receiveMutex);const auto root=JsonObject::Parse(packet);if(!root.HasKey(L"SourceTarget"))return false;const auto boundary=root.GetNamedObject(L"SourceTarget");
    const auto target=tcard_blobs::Canonical(std::wstring(boundary.GetNamedObject(L"Target").Stringify())),origin=sync_string(boundary,L"Source"),group=tcard_chunks::Group(storage);
    if(boundary.GetNamedNumber(L"Version")!=1||sync_string(boundary,L"Group")!=group||group!=tcard_chunks::Group(mailbox)||!sync_hex(origin)||sync_string(boundary,L"Direction")!=L"outgoing"||sync_string(boundary,L"TargetDigest")!=tcard_bridge::Hash(target))throw std::runtime_error("Native source boundary differs");
    if(context)DependencyBoundary(storage,origin,target,*context);
    std::wstring delta;const auto advertised=context?DependencyUnwrap(packet,*context,delta):selection::Unwrap(packet,delta,true);const auto expected=selection::Read(target);if(selection::Missing(expected,advertised))throw std::runtime_error("Native source target exceeds advertisement");if(!context)selection::Validate(delta,expected,true);
    if(!boundary.GetNamedBoolean(L"Atomic"))return false;
    std::wstring committed;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,committed,result,version))throw std::runtime_error("Native source baseline unavailable");auto state=EnsureSource(storage,committed);Promote(storage,committed,state);
    if(!selection::Read(state.native).operations.empty()||!selection::Read(state.native).assets.empty()){
        if(state.native!=target||state.origin!=origin)throw std::runtime_error("Another native source target is retained");return true;
    }
    if(selection::Missing(expected,selection::Read(committed))){state.native=target;state.origin=origin;WriteSource(storage,state);}return true;
}
inline bool NativeDependency(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& packet,const tcard_dependencies::Context& context)
{
    if(!JsonObject::Parse(packet).HasKey(L"SourceTarget"))throw std::runtime_error("Dependency native target absent");return Native(storage,mailbox,packet,&context);
}
inline bool Selection(const std::wstring& storage,const selection::Inventory& peer,std::wstring& packet,Result& result,unsigned& version,bool objects,const std::wstring& target=L"",size_t reserved=0)
{
    packet.clear();std::lock_guard guard(receiveMutex);if(!tcard_storage::Required(storage))return ExportSelection(storage,peer,packet,result,version,false,objects);
    std::wstring admitted;if(!Inventory(storage,admitted,result,version))return false;
    if(!ExportSelection(storage,peer,packet,result,version,false,objects,target.empty()?admitted:target,reserved))return false;
    auto root=JsonObject::Parse(packet);root.Insert(L"Inventory",JsonObject::Parse(admitted));packet=root.Stringify();return true;
}
inline bool Whole(const std::wstring& storage,std::wstring& package,Result& result)
{
    try{std::lock_guard guard(receiveMutex);if(!ReadSource(storage).exists)return ExportChanges(storage,package,result);
        unsigned version=0;std::wstring selected;if(!Selection(storage,{},selected,result,version,false))return false;selection::Unwrap(selected,package);return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));package.clear();return false;}catch(...){result.error=L"Admitted full export failed";package.clear();return false;}
}
// Destination-bound private progress is not a public inventory, source completion or native owner ACK.
inline std::wstring Custody(const std::wstring& storage,const std::wstring& source)
{
    std::lock_guard guard(receiveMutex);if(!sync_hex(source))throw std::runtime_error("Invalid custody destination");
    const auto retained=Read(storage);if(std::none_of(retained.begin(),retained.end(),[&](const auto& value){return value.source==source;}))return {};
    std::wstring committed;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,committed,result,version))throw std::runtime_error("Private custody inventory unavailable");const auto actual=selection::Read(committed);
    JsonObject root;JsonArray targets;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Kind",JsonValue::CreateStringValue(L"bootstrap-custody"));
    root.Insert(L"Group",JsonValue::CreateStringValue(tcard_chunks::Group(storage)));root.Insert(L"Source",JsonValue::CreateStringValue(source));root.Insert(L"Receiver",JsonValue::CreateStringValue(tcard_bootstrap::Node(storage)));
    for(const auto& target:retained){if(target.source!=source)continue;const auto expected=selection::Read(target.header);selection::Inventory covered;
        for(const auto& value:expected.operations)if(actual.operations.contains(value))covered.operations.insert(value);
        for(const auto& value:expected.assets)if(actual.assets.contains(value))covered.assets.insert(value);
        JsonObject item;item.Insert(L"TargetDigest",JsonValue::CreateStringValue(target.digest));item.Insert(L"Inventory",JsonObject::Parse(Header(covered)));targets.Append(item);
    }
    root.Insert(L"Targets",targets);const auto text=std::wstring(root.Stringify());if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Private custody budget exceeded");return text;
}
inline selection::Inventory ReadCustody(const std::wstring& storage,const std::wstring& receiver,const std::wstring& target,const std::wstring& text)
{
    // Caller supplies only metadata from this fresh authenticated receiver and its active retained wire target.
    if(text.empty())return {};if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Private custody input budget exceeded");const auto root=JsonObject::Parse(text);
    if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"bootstrap-custody"||
        sync_string(root,L"Group")!=tcard_chunks::Group(storage)||sync_string(root,L"Source")!=tcard_bootstrap::Node(storage)||!sync_hex(receiver)||sync_string(root,L"Receiver")!=receiver)
        throw std::runtime_error("Authenticated custody identity differs");
    const auto expected=selection::Read(target);const auto digest=tcard_bridge::Hash(target);const auto targets=root.GetNamedArray(L"Targets");if(targets.Size()>maxTargets)throw std::runtime_error("Private custody target budget exceeded");
    selection::Inventory covered;std::set<std::wstring> seen;
    for(const auto& value:targets){const auto item=value.GetObject();const auto offered=sync_string(item,L"TargetDigest");if(!sync_hex(offered)||!seen.insert(offered).second)throw std::runtime_error("Private custody target identity differs");
        if(offered!=digest)continue;covered=selection::Read(std::wstring(item.GetNamedObject(L"Inventory").Stringify()));if(selection::Missing(covered,expected))throw std::runtime_error("Private custody exceeds captured target");}
    return covered;
}
inline std::wstring PeerCustody(const std::wstring& packet)
{
    const auto root=JsonObject::Parse(packet);return root.HasKey(L"BootstrapCustody")?std::wstring(root.GetNamedObject(L"BootstrapCustody").Stringify()):L"";
}
inline bool WireInventory(const std::wstring& storage,const std::wstring& receiver,std::wstring& header,Result& result,unsigned& version,bool custody)
{
    std::lock_guard guard(receiveMutex);if(!Inventory(storage,header,result,version))return false;if(!custody)return true;
    try{const auto receipt=Custody(storage,receiver);if(!receipt.empty()){auto root=JsonObject::Parse(header);root.Insert(L"BootstrapCustody",JsonObject::Parse(receipt));header=root.Stringify();}
        if(tcard_asset::encode(header).size()>tcard_asset::max_file-65536)throw std::runtime_error("Wire inventory custody budget exceeded");return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));header.clear();return false;}catch(...){result.error=L"Wire custody export failed";header.clear();return false;}
}
inline bool Export(const std::wstring& storage,const std::wstring& receiver,const selection::Inventory& peer,std::wstring& packet,Result& result,unsigned& version,bool metadata,bool custody=false,const std::wstring& receipt=L"")
{
    packet.clear();std::lock_guard admission(receiveMutex);std::lock_guard guard(tcard_bootstrap::pinMutex);
    try{
        tcard_bootstrap::Finish(storage,storage,receiver,peer);const auto folder=tcard_bootstrap::Folder(storage),leaf=receiver+L".json";
        std::wstring admitted;if(!Inventory(storage,admitted,result,version))return false;
        const auto target=tcard_blobs::Canonical(sync_exists(folder,leaf)?tcard_bootstrap::Read(storage,storage,receiver):admitted);
        if(selection::Missing(selection::Read(target),selection::Read(admitted)))throw std::runtime_error("Pinned source exceeds admitted frontier");
        // Private receipt progress is scoped to this active target. It never retires pins or changes public Safe.
        auto known=peer;if(custody&&metadata&&sync_exists(folder,leaf))selection::Include(known,ReadCustody(storage,receiver,target,receipt));
        if(!Selection(storage,known,packet,result,version,true,target))return false;
        std::wstring selected;auto covered=known;selection::Unwrap(packet,selected,true);selection::Include(covered,selection::Read(selection::Header(selected)));
        if(selection::Missing(selection::Read(target),covered)&&!sync_exists(folder,leaf))tcard_bootstrap::Retain(storage,storage,receiver,target);
        const bool atomic=sync_exists(folder,leaf);if(!metadata)return true;auto root=JsonObject::Parse(packet);const auto localCustody=custody?Custody(storage,receiver):L"";
        const auto reserved=tcard_asset::encode(target).size()+tcard_asset::encode(localCustody).size()+(custody?2048:1024);
        if(tcard_asset::encode(packet).size()+reserved>tcard_asset::max_file-65536&&
            !Selection(storage,known,packet,result,version,true,target,reserved))return false;
        root=JsonObject::Parse(packet);JsonObject boundary;boundary.Insert(L"Version",JsonValue::CreateNumberValue(1));boundary.Insert(L"Kind",JsonValue::CreateStringValue(L"remote-bootstrap"));
        boundary.Insert(L"Source",JsonValue::CreateStringValue(tcard_bootstrap::Node(storage)));boundary.Insert(L"Receiver",JsonValue::CreateStringValue(receiver));boundary.Insert(L"Group",JsonValue::CreateStringValue(tcard_chunks::Group(storage)));
        boundary.Insert(L"Target",JsonObject::Parse(target));boundary.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(target)));boundary.Insert(L"Atomic",JsonValue::CreateBooleanValue(atomic));root.Insert(L"RemoteBootstrap",boundary);
        if(!localCustody.empty())root.Insert(L"BootstrapCustody",JsonObject::Parse(localCustody));
        packet=root.Stringify();if(tcard_asset::encode(packet).size()>tcard_asset::max_file-65536)throw std::runtime_error("Remote boundary exceeds packet budget");return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));packet.clear();return false;}catch(...){result.error=L"Remote boundary export failed";packet.clear();return false;}
}
inline bool ExportDependency(const std::wstring& storage,const std::wstring& receiver,const selection::Inventory& peer,const std::wstring& space,std::wstring& packet,Result& result,unsigned& version,const std::wstring& receipt=L"")
{
    std::lock_guard admission(receiveMutex);std::lock_guard guard(tcard_bootstrap::pinMutex);
    // Keep ordinary/no-op/old object selection unchanged; only the indivisible object-count refusal selects this capability.
    if(Export(storage,receiver,peer,packet,result,version,true,true,receipt))return true;
    if(!tcard_dependencies::LegacyFallback(result))return false;
    try{
        std::wstring admitted;if(!Inventory(storage,admitted,result,version))return false;const auto maximum=version;
        const auto folder=tcard_bootstrap::Folder(storage),leaf=receiver+L".json";const auto target=tcard_blobs::Canonical(sync_exists(folder,leaf)?tcard_bootstrap::Read(storage,storage,receiver):admitted);
        if(selection::Missing(selection::Read(target),selection::Read(admitted)))throw std::runtime_error("Dependency pin exceeds Safe frontier");
        auto known=peer;if(sync_exists(folder,leaf))selection::Include(known,ReadCustody(storage,receiver,target,receipt));
        const tcard_dependencies::Context context{space,tcard_chunks::Group(storage),tcard_bootstrap::Node(storage),receiver,target};std::wstring manifest;unsigned selectedVersion=0;
        if(!ExportDependencies(storage,known,context,manifest,result,selectedVersion))return false;version=(std::max)(maximum,selectedVersion);
        const auto parsed=tcard_dependencies::Read(manifest,context);auto covered=known;selection::Include(covered,tcard_dependencies::Required(parsed));
        JsonObject root,boundary;root.Insert(L"Inventory",JsonObject::Parse(admitted));root.Insert(L"Delta",JsonObject::Parse(manifest));
        boundary.Insert(L"Version",JsonValue::CreateNumberValue(1));boundary.Insert(L"Kind",JsonValue::CreateStringValue(L"remote-bootstrap"));boundary.Insert(L"Source",JsonValue::CreateStringValue(context.source));
        boundary.Insert(L"Receiver",JsonValue::CreateStringValue(receiver));boundary.Insert(L"Group",JsonValue::CreateStringValue(context.group));boundary.Insert(L"Target",JsonObject::Parse(target));boundary.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(target)));
        const bool retain=sync_exists(folder,leaf)||selection::Missing(selection::Read(target),covered);boundary.Insert(L"Atomic",JsonValue::CreateBooleanValue(retain));root.Insert(L"RemoteBootstrap",boundary);
        const auto custody=Custody(storage,receiver);if(!custody.empty())root.Insert(L"BootstrapCustody",JsonObject::Parse(custody));packet=root.Stringify();
        if(tcard_asset::encode(packet).size()>tcard_asset::max_file-65536)throw std::runtime_error("Dependency selected metadata budget exceeded");
        if(retain&&!sync_exists(folder,leaf))tcard_bootstrap::Retain(storage,storage,receiver,target);return true;
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));packet.clear();return false;}catch(...){result.error=L"Dependency selected export failed";packet.clear();return false;}
}
inline void Capture(const std::wstring& storage,const std::wstring& source,const std::wstring& packet,const tcard_dependencies::Context* context=nullptr)
{
    // This entry point is called only after v2 authentication and payload decryption, before import.
    std::lock_guard guard(receiveMutex);const auto root=JsonObject::Parse(packet);const auto boundary=root.GetNamedObject(L"RemoteBootstrap");
    const auto header=tcard_blobs::Canonical(std::wstring(boundary.GetNamedObject(L"Target").Stringify())),digest=tcard_bridge::Hash(header),group=tcard_chunks::Group(storage);
    if(boundary.GetNamedNumber(L"Version")!=1||sync_string(boundary,L"Kind")!=L"remote-bootstrap"||!sync_hex(source)||sync_string(boundary,L"Source")!=source||
        sync_string(boundary,L"Receiver")!=tcard_bootstrap::Node(storage)||sync_string(boundary,L"Group")!=group||sync_string(boundary,L"TargetDigest")!=digest)
        throw std::runtime_error("Authenticated remote target identity differs");
    if(context)DependencyBoundary(storage,source,header,*context);
    const auto target=selection::Read(header);std::wstring delta;const auto advertised=context?DependencyUnwrap(packet,*context,delta):selection::Unwrap(packet,delta,true);
    if(selection::Missing(target,advertised))throw std::runtime_error("Remote target exceeds advertised originals");if(!context)selection::Validate(delta,target,true);
    const auto folder=Folder(storage),leaf=tcard_bridge::Hash(source+L":"+digest)+L".json";auto targets=Read(storage);ULONGLONG sequence=0;
    // A complete unrelated source can progress while an earlier source remains offline.
    auto sourceState=ReadSource(storage);
    if(!boundary.GetNamedBoolean(L"Atomic")&&targets.empty()&&(!sourceState.exists||selection::Read(sourceState.native)==selection::Inventory{}))return;
    for(const auto& retained:targets){if(retained.leaf==leaf){if(retained.header!=header)throw std::runtime_error("Remote target digest collision");return;}sequence=(std::max)(sequence,retained.sequence);}
    if(targets.size()>=maxTargets||sequence>=9007199254740991ULL)throw std::runtime_error("Retained remote target budget reached");
    if(tcard_asset::encode(header).size()>tcard_asset::max_file-65536||!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect remote target boundary");
    std::wstring committed;Result committedResult;unsigned committedVersion=0;
    if(!ExportCommittedInventory(storage,committed,committedResult,committedVersion))throw std::runtime_error("Remote admission baseline unavailable");EnsureSource(storage,committed);
    JsonObject record;for(const auto* name:{L"Version",L"Kind",L"Source",L"Receiver",L"Group",L"Target",L"TargetDigest",L"Atomic"})record.Insert(name,boundary.GetNamedValue(name));record.Insert(L"Sequence",JsonValue::CreateNumberValue(static_cast<double>(sequence+1)));
    PublicationPublish(storage,folder,leaf,std::wstring(record.Stringify()),false);
    const auto verified=Read(storage);if(std::none_of(verified.begin(),verified.end(),[&](const auto& value){return value.leaf==leaf&&value.header==header;}))throw std::runtime_error("Remote target verification failed");++revision;
}
inline void CaptureDependency(const std::wstring& storage,const std::wstring& source,const std::wstring& packet,const tcard_dependencies::Context& context)
{
    Capture(storage,source,packet,&context);
}
struct Inbox {
    std::vector<Target> targets;ULONGLONG observed=0,audit=0;
    bool Refresh(const std::wstring& storage)
    {
        std::lock_guard guard(receiveMutex);const auto now=GetTickCount64(),current=revision.load();if(observed==current&&audit&&now-audit<60000)return false;
        targets=Read(storage);observed=current;audit=now;return true;
    }
    bool Retire(const std::wstring& storage,const selection::Inventory& applied)
    {
        std::lock_guard guard(receiveMutex);Refresh(storage);bool changed=false;
        bool completed=false;for(const auto& target:targets)completed|=!selection::Missing(selection::Read(target.header),applied);
        selection::Inventory committed;
        if(completed){std::wstring header;Result result;unsigned version=0;if(!ExportCommittedInventory(storage,header,result,version))throw std::runtime_error("Retiring source inventory unavailable");
            committed=selection::Read(header);auto state=ReadSource(storage);Promote(storage,header,state);}
        std::set<std::wstring> blockedSources;for(auto at=targets.begin();at!=targets.end();){if(blockedSources.contains(at->source)||selection::Missing(selection::Read(at->header),applied)||selection::Missing(selection::Read(at->header),committed)){blockedSources.insert(at->source);++at;continue;}
            if(!DeleteFileW((Folder(storage)+L"\\"+at->leaf).c_str()))throw std::runtime_error("Native-applied remote target retirement blocked");at=targets.erase(at);changed=true;}
        if(changed){++revision;observed=revision.load();RemoveDirectoryW(Folder(storage).c_str());}return changed;
    }
    std::wstring Ready(const selection::Inventory& committed)const
    {
        std::set<std::wstring> blocked;
        for(const auto& target:targets){if(!blocked.insert(target.source).second)continue;if(!selection::Missing(selection::Read(target.header),committed))return target.header;}return {};
    }
};
}
