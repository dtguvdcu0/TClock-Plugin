#pragma once
#include "card_sync_bridge.h"
#include "card_sync_store.h"
#include <cmath>
#include <mutex>

namespace tcard_chunks {
using Bytes=tcard_crypto::Bytes;
using namespace tcard_sync;
constexpr size_t chunkSize=256*1024;
constexpr size_t maxTransfers=8;
inline std::mutex cacheMutex;
inline bool Hash(const std::wstring& value)
{
    return value.size()==64&&std::all_of(value.begin(),value.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});
}
inline size_t Number(const JsonObject& object,const wchar_t* name,size_t maximum)
{
    const auto value=object.GetNamedNumber(name);
    if(!std::isfinite(value)||value<0||value>maximum||floor(value)!=value)throw std::runtime_error("Invalid chunk number");
    return static_cast<size_t>(value);
}
struct Manifest {size_t size=0;std::wstring hash;std::vector<std::wstring> hashes;};
inline Manifest Read(const JsonObject& object)
{
    Manifest result;
    if(object.GetNamedNumber(L"Version")!=1||Number(object,L"ChunkSize",chunkSize)!=chunkSize)throw std::runtime_error("Unsupported chunks");
    result.size=Number(object,L"Size",tcard_asset::max_file);result.hash=sync_string(object,L"Hash");
    const auto hashes=object.GetNamedArray(L"Hashes");
    if(result.size<=chunkSize||!Hash(result.hash)||hashes.Size()!=(result.size+chunkSize-1)/chunkSize)throw std::runtime_error("Invalid chunk manifest");
    for(const auto& value:hashes){const auto hash=std::wstring(value.GetString());if(!Hash(hash))throw std::runtime_error("Invalid chunk hash");result.hashes.push_back(hash);}
    return result;
}
inline JsonObject Encode(const Manifest& value)
{
    JsonObject result;result.Insert(L"Version",JsonValue::CreateNumberValue(1));result.Insert(L"ChunkSize",JsonValue::CreateNumberValue(chunkSize));
    result.Insert(L"Size",JsonValue::CreateNumberValue(static_cast<double>(value.size)));result.Insert(L"Hash",JsonValue::CreateStringValue(value.hash));JsonArray hashes;
    for(const auto& hash:value.hashes)hashes.Append(JsonValue::CreateStringValue(hash));result.Insert(L"Hashes",hashes);Read(result);return result;
}
inline Manifest Describe(const Bytes& bytes)
{
    if(bytes.size()<=chunkSize||bytes.size()>tcard_asset::max_file)throw std::runtime_error("Chunk payload budget exceeded");
    Manifest result;result.size=bytes.size();result.hash=tcard_asset::hash(bytes);
    for(size_t at=0;at<bytes.size();at+=chunkSize){const auto end=(std::min)(bytes.size(),at+chunkSize);
        result.hashes.push_back(tcard_asset::hash(Bytes(bytes.begin()+at,bytes.begin()+end)));}return result;
}
inline std::wstring Group(const std::wstring& storage)
{
    tcard_crypto::Keys keys;struct Wipe{tcard_crypto::Keys& keys;~Wipe(){SecureZeroMemory(keys.dataKey.data(),keys.dataKey.size());}} wipe{keys};
    if(!tcard_storage::Required(storage)||!tcard_storage::LoadKey(storage,keys))throw std::runtime_error("Encrypted chunks unavailable");
    return tcard_asset::hash({keys.publicKey.begin(),keys.publicKey.end()});
}
// Shared provisional transfer reservations exclude committed native notes/history/assets.
namespace budget {
constexpr size_t maxBytes=128*1024*1024,metadataBytes=2*1024*1024+65536;
constexpr unsigned retireObject=1,retireBinding=2;
struct Entry {std::wstring path,identity,destination;size_t bytes=0;bool provisional=true,retiring=false;unsigned retireKind=0;size_t partCount=0;};
struct Catalog {std::wstring owner,group;std::vector<Entry> entries;};
struct RestoreMemo {bool profile=false,checked=false;};
inline std::map<std::wstring,RestoreMemo> restored;
inline std::wstring Extended(const std::wstring& path){return path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;}
inline bool Exists(const std::wstring& path)
{
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return true;const auto error=GetLastError();
    if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;throw std::runtime_error("Transfer path state unavailable");
}
inline std::wstring Scope(const std::wstring& storage)
{
    auto path=sync_path_plain(storage);
    for(unsigned depth=0;depth<64&&!path.empty();++depth){
        // Real encryption controls identify the root; user directory names are not namespaces.
        if(Exists(Extended(path)+L"\\.tcard-vault.required")){
            if(!Exists(Extended(path)+L"\\.tcard-vault.key")||!tcard_storage::Ready(Extended(path)))throw std::runtime_error("Transfer encryption root unavailable");return path;
        }
        const auto part=path.find_last_of(L"\\");if(part==std::wstring::npos||part<3)break;path.resize(part);
    }
    throw std::runtime_error("Transfer quota root unavailable");
}
inline std::wstring Owner(const std::wstring& storage){return sync_owner(Scope(storage));}
inline std::wstring Relative(const std::wstring& owner,const std::wstring& path)
{
    const auto absolute=sync_path_plain(path);if(!absolute.starts_with(owner+L"\\"))throw std::runtime_error("Transfer quota path outside owner");return absolute.substr(owner.size()+1);
}
inline std::wstring Folder(const Catalog& value){return Extended(value.owner)+L"\\_transfer";}
inline size_t Charge(size_t size,size_t chunks,size_t metadata)
{
    if(size>tcard_asset::max_file||chunks>64||metadata>65536)throw std::runtime_error("Transfer reservation budget exceeded");
    return size+chunks*(tcard_crypto::envelopeOverhead+4096)+metadata+4096+chunkSize+tcard_crypto::envelopeOverhead+4096;
}
inline std::wstring Encode(const Catalog& value)
{
    JsonObject root;JsonArray entries;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Group",JsonValue::CreateStringValue(value.group));
    for(const auto& entry:value.entries){JsonObject item;item.Insert(L"Path",JsonValue::CreateStringValue(entry.path));item.Insert(L"Identity",JsonValue::CreateStringValue(entry.identity));
        item.Insert(L"Destination",JsonValue::CreateStringValue(entry.destination));item.Insert(L"Bytes",JsonValue::CreateNumberValue(static_cast<double>(entry.bytes)));item.Insert(L"Provisional",JsonValue::CreateBooleanValue(entry.provisional));
        item.Insert(L"Retiring",JsonValue::CreateBooleanValue(entry.retiring));item.Insert(L"RetireKind",JsonValue::CreateNumberValue(entry.retireKind));item.Insert(L"PartCount",JsonValue::CreateNumberValue(static_cast<double>(entry.partCount)));entries.Append(item);}
    root.Insert(L"Entries",entries);const auto text=std::wstring(root.Stringify());if(tcard_asset::encode(text).size()>1024*1024)throw std::runtime_error("Transfer reservation metadata exceeded");return text;
}
// Match the existing bridge::Storage active-group schema, rather than inventing a new GUID contract.
inline bool GroupName(const std::wstring& name)
{
    return name.size()==36&&name.find_first_not_of(L"0123456789abcdefABCDEF-")==std::wstring::npos;
}
inline bool Profile(const std::wstring& owner)
{
    const auto path=Extended(owner);if(!Exists(path+L"\\active.json"))return false;
    std::wstring text;if(!tcard_bridge::Read(path,L"active.json",text))throw std::runtime_error("Transfer profile authority unavailable");
    JsonObject active;if(!JsonObject::TryParse(text,active)||active.GetNamedNumber(L"Version",0)!=1)return false;
    const std::wstring group=active.GetNamedString(L"Group",L"").c_str();
    return GroupName(group);
}
inline std::wstring MemoKey(const Catalog& value){return value.owner+L":"+value.group;}
inline RestoreMemo& Memo(const Catalog& value)
{
    const bool profile=Profile(value.owner);const auto key=MemoKey(value);
    auto [entry,inserted]=restored.try_emplace(key,RestoreMemo{profile,false});
    // Observe changes even when a later catalog check refuses admission; no old state may silently reappear.
    if(!inserted&&entry->second.profile!=profile)entry->second={profile,false};return entry->second;
}
inline bool Managed(const std::wstring& path,bool profile=false)
{
    auto relative=path;bool replica=false;
    if(relative.starts_with(L"groups\\")){
        if(!profile)return false;const auto end=relative.find(L'\\',7);
        if(end==std::wstring::npos||!GroupName(relative.substr(7,end-7)))return false;relative.erase(0,end+1);
    }else if(relative.starts_with(L"replica\\")){replica=true;relative.erase(0,8);}
    if(relative.starts_with(L"ui\\")){
        if(replica)return false;relative.erase(0,3);const auto end=relative.find(L'\\');
        if(end==std::wstring::npos)return false;const auto direction=relative.substr(0,end);
        if(direction!=L"incoming-objects"&&direction!=L"outgoing-objects")return false;relative.erase(0,end+1);
    }
    if(relative.starts_with(L"_native-generations\\")){
        const auto end=relative.find(L'\\',20);
        if(end==std::wstring::npos||!sync_hex(relative.substr(20,end-20))||relative.substr(end,6)!=L"\\data\\")return false;relative.erase(0,end+6);
    }
    return relative.starts_with(L"_transfer\\")||relative.starts_with(L"_objects\\")||relative.starts_with(L"_object-transfers\\")||relative.starts_with(L"_dependency-custody\\");
}
inline std::vector<std::wstring> Children(const std::wstring& path)
{
    std::vector<std::wstring> names;if(!Exists(path))return names;
    tcard_asset::File directory(tcard_asset::open_dir(path,false));if(!directory)throw std::runtime_error("Unsafe transfer namespace directory");
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((path+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return names;throw std::runtime_error("Transfer namespace unavailable");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};size_t count=0;
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        if(++count>2048||entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Transfer namespace exceeds bounds");
        if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Unknown transfer namespace entry retained");names.push_back(name);
    }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Transfer namespace enumeration failed");return names;
}
inline std::set<std::wstring> Scopes(const Catalog& value)
{
    const bool profile=Memo(value).profile;std::set<std::wstring> scopes{L"",L"replica",L"ui\\incoming-objects",L"ui\\outgoing-objects"};
    if(profile)for(const auto& group:Children(Extended(value.owner)+L"\\groups")){
        if(!GroupName(group))throw std::runtime_error("Unknown profile group retained");const auto base=L"groups\\"+group;
        scopes.insert(base);scopes.insert(base+L"\\ui\\incoming-objects");scopes.insert(base+L"\\ui\\outgoing-objects");
        if(scopes.size()>2048)throw std::runtime_error("Transfer scope catalog exceeds bounds");
    }
    const auto bases=scopes;for(const auto& base:bases){const auto prefix=base.empty()?L"":base+L"\\";
        // Enumerate only selected-generation names; never traverse live note/history trees.
        for(const auto& generation:Children(Extended(value.owner)+L"\\"+prefix+L"_native-generations")){
            if(!sync_hex(generation))throw std::runtime_error("Unknown transfer generation retained");scopes.insert(prefix+L"_native-generations\\"+generation+L"\\data");
            if(scopes.size()>2048)throw std::runtime_error("Transfer scope catalog exceeds bounds");
        }
    }
    std::set<std::wstring> absolute;for(const auto& scope:scopes){const auto relative=scope.empty()?L"_transfer\\quota":scope+L"\\_transfer\\quota";
        if(!Managed(relative,profile))throw std::runtime_error("Transfer scope grammar differs");absolute.insert(value.owner+(scope.empty()?L"":L"\\"+scope));}
    return absolute;
}
// Retirement is confined to exact dependency roots; missing files never create new deletion authority.
inline std::wstring RetirementRoot(const std::wstring& path,unsigned kind,size_t count)
{
    const std::wstring component=L"_dependency-custody\\";const auto at=path.find(component);
    if(at==std::wstring::npos||(at&&path[at-1]!=L'\\')||count>64)throw std::runtime_error("Invalid dependency retirement path");
    const auto begin=at+component.size(),end=path.find(L'\\',begin);const auto identity=path.substr(begin,end==std::wstring::npos?end:end-begin);
    if(!sync_hex(identity))throw std::runtime_error("Invalid dependency retirement identity");
    if(kind==retireBinding){if(end!=std::wstring::npos||count)throw std::runtime_error("Invalid dependency binding retirement");}
    else if(kind==retireObject){
        if(end==std::wstring::npos)throw std::runtime_error("Invalid dependency object retirement");const auto tail=path.substr(end+1);
        const auto offset=tail.starts_with(L"_sealed\\")?8:tail.starts_with(L"_objects\\")?9:0;
        if(!offset||!sync_hex(tail.substr(offset)))throw std::runtime_error("Invalid dependency object retirement");
    }else throw std::runtime_error("Invalid dependency retirement kind");
    return path.substr(0,begin)+identity;
}
inline void Check(const Catalog& value)
{
    size_t bytes=metadataBytes,slots=0;std::set<std::wstring> paths;const bool profile=Memo(value).profile;
    if(value.entries.size()>2048)throw std::runtime_error("Transfer reservation catalog exceeded");
    for(const auto& entry:value.entries){if(!sync_hex(entry.identity)||!Managed(entry.path,profile)||entry.path.find(L"..")!=std::wstring::npos||!paths.insert(entry.path).second||entry.bytes>maxBytes-bytes)throw std::runtime_error("Transfer quota identity or bytes exceeded");
        if(entry.retiring){RetirementRoot(entry.path,entry.retireKind,entry.partCount);if(!entry.destination.empty())throw std::runtime_error("Retiring transfer promotion unresolved");}
        else if(entry.retireKind||entry.partCount)throw std::runtime_error("Inactive transfer retirement differs");
        bytes+=entry.bytes;if(entry.provisional&&++slots>maxTransfers)throw std::runtime_error("Combined provisional transfer slots reached");
        if(!entry.destination.empty()&&(!Managed(entry.destination,profile)||entry.path.find(L"\\_objects\\")==std::wstring::npos||entry.destination.find(L"..")!=std::wstring::npos||!entry.destination.starts_with(entry.path.substr(0,entry.path.find(L"\\_objects\\"))+L"\\_sealed\\")))throw std::runtime_error("Transfer promotion path differs");}
}
inline void Save(const Catalog& value)
{
    Check(value);if(!tcard_bridge::Protect(Folder(value))||!tcard_bridge::Publish(Folder(value),L"budget.json",Encode(value)))throw std::runtime_error("Cannot retain transfer reservation");
}
inline Catalog Load(const std::wstring& storage)
{
    Catalog value{Owner(storage),Group(storage),{}};std::wstring text;
    if(sync_exists(Folder(value),L"budget.json")){
        if(!tcard_bridge::Read(Folder(value),L"budget.json",text))throw std::runtime_error("Transfer reservation unavailable");const auto root=JsonObject::Parse(text);
        if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Group")!=value.group)throw std::runtime_error("Transfer reservation group differs");const auto entries=root.GetNamedArray(L"Entries");
        if(entries.Size()>2048)throw std::runtime_error("Transfer reservation catalog exceeded");for(const auto& item:entries){const auto row=item.GetObject();value.entries.push_back({sync_string(row,L"Path"),sync_string(row,L"Identity"),sync_string(row,L"Destination"),Number(row,L"Bytes",maxBytes),row.GetNamedBoolean(L"Provisional"),row.GetNamedBoolean(L"Retiring",false),static_cast<unsigned>(row.HasKey(L"RetireKind")?Number(row,L"RetireKind",retireBinding):0),row.HasKey(L"PartCount")?Number(row,L"PartCount",64):0});}
    }
    Check(value);return value;
}
inline void Walk(const std::wstring& path,std::vector<std::pair<std::wstring,size_t>>& files,size_t& count,unsigned depth=0)
{
    const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES){if(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)return;throw std::runtime_error("Transfer quota tree unavailable");}
    if(depth>12||attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Unsafe transfer quota tree");tcard_asset::File directory(tcard_asset::open_dir(path,false));if(!directory)throw std::runtime_error("Unsafe transfer quota directory");
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((path+L"\\*").c_str(),&entry);if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return;throw std::runtime_error("Cannot inspect transfer quota");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;if(++count>8192||entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Transfer quota inventory exceeds bounds");
        const auto child=path+L"\\"+name;if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)Walk(child,files,count,depth+1);else{if(entry.nFileSizeHigh)throw std::runtime_error("Transfer quota file exceeds bounds");files.emplace_back(child,entry.nFileSizeLow);}
    }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Transfer quota inventory failed");
}
// Cooperating writers reserve before publication. External/new unknown insertion is detected on restart or authority change, never by per-Put scans.
inline void Restore(Catalog& value,const std::wstring&)
{
    auto& memo=Memo(value);if(memo.checked)return;const auto scopes=Scopes(value);
    std::vector<std::pair<std::wstring,size_t>> files;size_t count=0;for(const auto& scope:scopes)for(const auto* component:{L"_transfer",L"_objects",L"_object-transfers",L"_dependency-custody"})Walk(Extended(scope)+L"\\"+component,files,count);
    // Adopt only authenticated legacy manifests; unknown files, temps or abandoned roots remain protected and refuse admission.
    bool changed=false;for(const auto& [path,unused]:files){const auto name=path.substr(path.find_last_of(L"\\")+1);if(name!=L"manifest.json"&&name!=L"reference.json")continue;
        const auto directory=path.substr(0,path.find_last_of(L"\\")),relative=Relative(value.owner,directory);
        if(std::any_of(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==relative||entry.destination==relative;}))continue;
        if(relative.find(L"_dependency-custody")!=std::wstring::npos)throw std::runtime_error("Unregistered dependency custody retained");std::wstring record;if(!tcard_bridge::Read(directory,name,record))throw std::runtime_error("Legacy transfer manifest unavailable");const auto root=JsonObject::Parse(record);
        if(sync_string(root,L"Group")!=value.group)throw std::runtime_error("Legacy transfer group differs");size_t bytes=0;bool provisional=true;
        if(name==L"reference.json"){if(root.GetNamedNumber(L"Version")!=1||!root.HasKey(L"Reference"))throw std::runtime_error("Unknown transfer metadata retained");bytes=tcard_asset::encode(record).size()+8192;provisional=false;}
        else{const auto size=Number(root,L"Size",tcard_asset::max_file);const auto parts=root.GetNamedArray(L"Hashes").Size();if(Number(root,L"ChunkSize",chunkSize)!=chunkSize||parts!=(size+chunkSize-1)/chunkSize)throw std::runtime_error("Legacy transfer descriptor differs");bytes=Charge(size,parts,tcard_asset::encode(record).size());}
        value.entries.push_back({relative,tcard_bridge::Hash(record),L"",bytes,provisional});changed=true;
    }
    std::map<std::wstring,size_t> actual;for(const auto& [path,bytes]:files){const auto relative=Relative(value.owner,path);if(relative==L"_transfer\\budget.json")continue;const auto name=relative.substr(relative.find_last_of(L"\\")+1);
        const auto* owner=static_cast<const Entry*>(nullptr);for(const auto& entry:value.entries)if(relative.starts_with(entry.path+L"\\")||(!entry.destination.empty()&&relative.starts_with(entry.destination+L"\\")))if(!owner||entry.path.size()>owner->path.size())owner=&entry;
        if(!owner||name.ends_with(L".tmp")||(name!=L"manifest.json"&&name!=L"reference.json"&&name!=L"binding.json"&&!name.ends_with(L".part")))throw std::runtime_error("Unknown transfer bytes retained");
        if(owner->retiring){
            // Only authenticated exact filenames survive a deletion interruption; missing manifests are permitted here alone.
            if(relative!=owner->path+L"\\"+name)throw std::runtime_error("Unknown retirement descendant retained");
            if(owner->retireKind==retireBinding){if(name!=L"binding.json")throw std::runtime_error("Unknown retiring binding bytes retained");}
            else if(name!=L"manifest.json"){
                if(!name.ends_with(L".part"))throw std::runtime_error("Unknown retiring object bytes retained");
                const auto digits=name.substr(0,name.size()-5);
                if(digits.empty()||digits.size()>2||digits.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::runtime_error("Unknown retiring fragment retained");
                const auto index=static_cast<size_t>(std::stoul(digits));
                if(name!=std::to_wstring(index)+L".part"||index>=owner->partCount)throw std::runtime_error("Unknown retiring fragment retained");
            }
        }else if(name.ends_with(L".part")){
            const auto directory=path.substr(0,path.find_last_of(L"\\"));std::wstring metadata;
            if(!tcard_bridge::Read(directory,L"manifest.json",metadata))throw std::runtime_error("Transfer fragment manifest unavailable");const auto manifest=JsonObject::Parse(metadata);
            if(sync_string(manifest,L"Group")!=value.group)throw std::runtime_error("Transfer fragment group differs");const auto hashes=manifest.GetNamedArray(L"Hashes");
            const auto indexText=name.substr(0,name.size()-5);if(indexText.empty()||indexText.size()>2||indexText.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::runtime_error("Unknown transfer fragment retained");
            const auto index=static_cast<unsigned>(std::stoul(indexText));if(name!=std::to_wstring(index)+L".part"||index>=hashes.Size())throw std::runtime_error("Unknown transfer fragment retained");
            if(tcard_bridge::Hash(metadata)!=owner->identity)throw std::runtime_error("Transfer fragment descriptor binding differs");
            // Account known corrupt raw bytes; existing Missing/Read/Put performs necessary content verification and repair.
        }
        if(bytes>owner->bytes-actual[owner->path])throw std::runtime_error("Actual transfer quota differs");actual[owner->path]+=bytes;
    }
    Check(value);if(changed)Save(value);memo.checked=true;
}
inline void Reserve(const std::wstring& storage,const std::wstring& path,const std::wstring& identity,size_t bytes,bool provisional=true)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(storage));if(!lock)throw std::runtime_error("Transfer quota busy");auto value=Load(storage);Restore(value,storage);const auto relative=Relative(value.owner,path);
    if(std::any_of(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.retiring&&relative.starts_with(entry.path+L"\\");}))throw std::runtime_error("Retiring dependency ancestor cannot admit custody");
    const auto found=std::find_if(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==relative;});if(found!=value.entries.end()){if(found->retiring)throw std::runtime_error("Retiring dependency custody cannot reactivate");if(found->identity!=identity||found->bytes!=bytes||found->provisional!=provisional)throw std::runtime_error("Transfer reservation binding differs");return;}
    value.entries.push_back({relative,identity,L"",bytes,provisional});Save(value);
}
// Called only while WithDependencyCommit holds both owners. Pool also checks its private binding/catalog exactly.
inline void Retire(const std::wstring& storage,const std::wstring& path,unsigned kind,size_t partCount,
    const std::wstring& committedStorage,const tcard_dependencies::Manifest& manifest,const DependencyCommitProof& proof)
{
    if(!proof.Matches(committedStorage,manifest))throw std::runtime_error("Fresh dependency commit proof required");
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(storage));if(!lock)throw std::runtime_error("Transfer quota busy");
    auto value=Load(storage);const auto relative=Relative(value.owner,path),root=RetirementRoot(relative,kind,partCount);
    const auto found=std::find_if(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==relative;});
    const auto binding=std::find_if(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==root;});
    if(found==value.entries.end()||binding==value.entries.end()||binding->identity!=root.substr(root.find_last_of(L'\\')+1)||binding->provisional||!found->destination.empty())
        throw std::runtime_error("Dependency retirement reservation differs");
    if(found->retiring){if(found->retireKind!=kind||found->partCount!=partCount)throw std::runtime_error("Dependency retirement resume differs");return;}
    if(kind==retireBinding&&std::any_of(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path.starts_with(root+L"\\");}))
        throw std::runtime_error("Dependency child custody remains before binding retirement");
    found->retiring=true;found->retireKind=kind;found->partCount=partCount;Save(value);
}
inline void Release(const std::wstring& storage,const std::wstring& path)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(storage));if(!lock)throw std::runtime_error("Transfer quota busy");auto value=Load(storage);const auto relative=Relative(value.owner,path);
    const auto found=std::find_if(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==relative;});
    if(found!=value.entries.end()&&!found->destination.empty())throw std::runtime_error("Pending transfer promotion must resume before release");
    if(Exists(Extended(value.owner+L"\\"+relative)))throw std::runtime_error("Transfer bytes remain before quota release");
    std::erase_if(value.entries,[&](const auto& entry){return entry.path==relative;});Save(value);
}
inline void Promote(const std::wstring& storage,const std::wstring& from,const std::wstring& to)
{
    std::lock_guard guard(sync_mutex);SyncLock lock(Owner(storage));if(!lock)throw std::runtime_error("Transfer quota busy");auto value=Load(storage);const auto old=Relative(value.owner,from),next=Relative(value.owner,to);
    const auto found=std::find_if(value.entries.begin(),value.entries.end(),[&](const auto& entry){return entry.path==old;});if(found==value.entries.end())throw std::runtime_error("Missing transfer promotion reservation");
    if(found->retiring)throw std::runtime_error("Retiring transfer cannot promote");if(!found->destination.empty()&&found->destination!=next)throw std::runtime_error("Transfer promotion destination differs");found->destination=next;Save(value);
    const bool before=Exists(from),after=Exists(to);if(before&&after)throw std::runtime_error("Duplicate transfer promotion retained");
    if(before){if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Transfer promotion interrupted");}else if(!after)throw std::runtime_error("Transfer promotion missing");
    found->path=next;found->destination.clear();found->provisional=false;Save(value);
}
}
// Staged fragments are private-replica custody. They never publish owner application evidence.
class Cache {
    std::wstring root_,group_;Manifest manifest_;
    size_t Length(size_t index)const
    {
        if(index>=manifest_.hashes.size())throw std::runtime_error("Chunk index outside manifest");
        return (std::min)(chunkSize,manifest_.size-index*chunkSize);
    }
    std::wstring Leaf(size_t index)const{return std::to_wstring(index)+L".part";}
    bool Load(size_t index,Bytes& bytes)const
    {
        return tcard_asset::read(root_,Leaf(index),bytes,chunkSize)&&bytes.size()==Length(index)&&tcard_asset::hash(bytes)==manifest_.hashes[index];
    }
public:
    Cache(const std::wstring& storage,const Manifest& manifest,const std::wstring& group):group_(Group(storage)),manifest_(Read(Encode(manifest)))
    {
        std::lock_guard native(sync_mutex);SyncLock owner(budget::Owner(storage));if(!owner)throw std::runtime_error("Chunk quota busy");
        std::lock_guard guard(cacheMutex);if(group_!=group)throw std::runtime_error("Chunk group differs");
        const auto folder=storage+L"\\_transfer";if(!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect chunk cache");
        root_=folder+L"\\"+manifest_.hash;
        if(GetFileAttributesW(root_.c_str())==INVALID_FILE_ATTRIBUTES){
            size_t retained=0;WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((folder+L"\\*").c_str(),&entry);
            if(find!=INVALID_HANDLE_VALUE){struct Search{HANDLE handle;~Search(){FindClose(handle);}} search{find};
                do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
                    if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Unsafe chunk cache entry");
                    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)++retained;
                }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Chunk cache enumeration failed");
            }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot enumerate chunk cache");
            // Never evict an interrupted transfer by age or to admit newer work.
            if(retained>=maxTransfers)throw std::runtime_error("Retained chunk budget reached");
        }
        auto saved=Encode(manifest_);saved.Insert(L"Group",JsonValue::CreateStringValue(group_));const auto expected=std::wstring(saved.Stringify());
        budget::Reserve(storage,root_,tcard_bridge::Hash(expected),budget::Charge(manifest_.size,manifest_.hashes.size(),tcard_asset::encode(expected).size()));
        if(!tcard_bridge::Protect(root_))throw std::runtime_error("Unsafe chunk directory");
        std::wstring current;
        if(sync_exists(root_,L"manifest.json")){
            if(!tcard_bridge::Read(root_,L"manifest.json",current)||current!=expected)throw std::runtime_error("Retained chunk manifest differs");
        }else if(!tcard_bridge::Publish(root_,L"manifest.json",expected))throw std::runtime_error("Cannot retain chunk manifest");
    }
    std::vector<size_t> Missing()const
    {
        std::lock_guard guard(cacheMutex);if(!tcard_storage::Ready(root_))throw std::runtime_error("Chunk key unavailable");
        std::vector<size_t> result;Bytes bytes;for(size_t i=0;i<manifest_.hashes.size();++i)if(!Load(i,bytes))result.push_back(i);return result;
    }
    void Put(size_t index,const Bytes& bytes)
    {
        std::lock_guard guard(cacheMutex);
        // Hold the existing directory without FILE_SHARE_DELETE; stale retired/moved caches cannot recreate it.
        tcard_asset::File directory(tcard_asset::open_dir(root_,false));if(!directory)throw std::runtime_error("Retired chunk cache unavailable");
        auto descriptor=Encode(manifest_);descriptor.Insert(L"Group",JsonValue::CreateStringValue(group_));std::wstring record;
        if(!tcard_bridge::Read(root_,L"manifest.json",record)||record!=std::wstring(descriptor.Stringify()))throw std::runtime_error("Chunk manifest binding differs");
        if(bytes.size()!=Length(index)||tcard_asset::hash(bytes)!=manifest_.hashes[index])throw std::runtime_error("Chunk bytes differ");
        if(!tcard_storage::Ready(root_))throw std::runtime_error("Chunk key unavailable");Bytes existing;
        if(Load(index,existing)&&existing==bytes)return;
        if(!tcard_asset::publish(root_,Leaf(index),bytes)||!Load(index,existing)||existing!=bytes)throw std::runtime_error("Cannot verify retained chunk");
    }
    Bytes Assemble()const
    {
        std::lock_guard guard(cacheMutex);Bytes result;result.reserve(manifest_.size);Bytes bytes;
        for(size_t i=0;i<manifest_.hashes.size();++i){if(!Load(i,bytes))throw std::runtime_error("Chunk remains missing");result.insert(result.end(),bytes.begin(),bytes.end());}
        if(result.size()!=manifest_.size||tcard_asset::hash(result)!=manifest_.hash)throw std::runtime_error("Complete chunk hash differs");return result;
    }
};
// Call only after the existing native importer durably retained the complete verified payload.
inline void Remove(const std::wstring& storage,const std::wstring& hash)
{
    std::lock_guard native(sync_mutex);SyncLock owner(budget::Owner(storage));if(!owner)return;
    std::lock_guard guard(cacheMutex);if(!Hash(hash))return;const auto root=storage+L"\\_transfer\\"+hash;
    try{std::wstring text;if(!tcard_bridge::Read(root,L"manifest.json",text))return;const auto saved=JsonObject::Parse(text);const auto manifest=Read(saved);
        if(manifest.hash!=hash||sync_string(saved,L"Group")!=Group(storage))return;{tcard_asset::File checked(tcard_asset::open_dir(root,false));if(!checked)return;
        for(size_t i=0;i<manifest.hashes.size();++i){const auto file=root+L"\\"+std::to_wstring(i)+L".part";
            if(!DeleteFileW(file.c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)return;}
        if(!DeleteFileW((root+L"\\manifest.json").c_str()))return;}if(RemoveDirectoryW(root.c_str()))budget::Release(storage,root);
    }catch(...){/* Retain bounded fragments if safe cleanup is unavailable. */}
}
}
