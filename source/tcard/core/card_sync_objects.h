#pragma once
#include "card_sync_chunks.h"

// Private custody of raw source/image objects; never an applied operation or owner receipt.
namespace tcard_objects {
using namespace tcard_sync;
using Bytes=tcard_crypto::Bytes;
constexpr size_t maxObjects=8;
inline std::mutex objectMutex;
struct Manifest {std::wstring name;size_t size=0;std::wstring hash;std::vector<std::wstring> hashes;};
inline Manifest Read(const JsonObject& object)
{
    Manifest value;
    if(object.GetNamedNumber(L"ObjectVersion")!=1||tcard_chunks::Number(object,L"ChunkSize",tcard_chunks::chunkSize)!=tcard_chunks::chunkSize)
        throw std::runtime_error("Unsupported object fragments");
    value.name=sync_string(object,L"Name");value.size=tcard_chunks::Number(object,L"Size",tcard_asset::max_file);value.hash=sync_string(object,L"Hash");
    const bool image=value.name.rfind(L"images/",0)==0;
    if(!tcard_asset::relative(value.name,image)||(!image&&value.name.rfind(L"note-",0)!=0)||value.name.substr(image?7:5,64)!=value.hash||!sync_hex(value.hash))
        throw std::runtime_error("Invalid object identity");
    const auto hashes=object.GetNamedArray(L"Hashes");
    if(hashes.Size()!=(value.size+tcard_chunks::chunkSize-1)/tcard_chunks::chunkSize)throw std::runtime_error("Invalid object fragment count");
    for(const auto& part:hashes){const auto hash=std::wstring(part.GetString());if(!sync_hex(hash))throw std::runtime_error("Invalid object fragment hash");value.hashes.push_back(hash);}
    return value;
}
inline JsonObject Encode(const Manifest& value)
{
    JsonObject object;object.Insert(L"ObjectVersion",JsonValue::CreateNumberValue(1));object.Insert(L"Name",JsonValue::CreateStringValue(value.name));
    object.Insert(L"Size",JsonValue::CreateNumberValue(static_cast<double>(value.size)));object.Insert(L"Hash",JsonValue::CreateStringValue(value.hash));
    object.Insert(L"ChunkSize",JsonValue::CreateNumberValue(tcard_chunks::chunkSize));JsonArray hashes;
    for(const auto& hash:value.hashes)hashes.Append(JsonValue::CreateStringValue(hash));object.Insert(L"Hashes",hashes);Read(object);return object;
}
inline Manifest Describe(const std::wstring& name,const Bytes& bytes)
{
    if(bytes.size()>tcard_asset::max_file)throw std::runtime_error("Object byte budget exceeded");
    Manifest value{name,bytes.size(),tcard_asset::hash(bytes),{}};
    for(size_t at=0;at<bytes.size();at+=tcard_chunks::chunkSize){const auto end=(std::min)(bytes.size(),at+tcard_chunks::chunkSize);
        value.hashes.push_back(tcard_asset::hash(Bytes(bytes.begin()+at,bytes.begin()+end)));}
    return Read(Encode(value));
}
inline bool Referenced(const std::wstring& package)
{
    return JsonObject::Parse(package).HasKey(L"ObjectVersion");
}
inline std::vector<Manifest> References(const std::wstring& package)
{
    if(tcard_asset::encode(package).size()>tcard_asset::max_file-65536)throw std::runtime_error("Object reference budget exceeded");
    const auto root=JsonObject::Parse(package);const auto notes=root.GetNamedArray(L"Notes");
    if(root.GetNamedNumber(L"Version")!=1||root.GetNamedNumber(L"ObjectVersion")!=1||notes.Size()!=1)throw std::runtime_error("Invalid object reference package");
    const auto note=notes.GetObjectAt(0);const auto id=sync_string(note,L"Id");const auto operations=note.GetNamedArray(L"Changes");
    if(!tcard_asset::leaf(sync_stem(id))||sync_stem(id)[0]==L'_'||operations.Size()!=1)throw std::runtime_error("Invalid object reference note");
    const auto operation=operations.GetObjectAt(0);
    if(sync_string(operation,L"Note")!=id||!sync_hex(sync_string(operation,L"Id")))throw std::runtime_error("Invalid referenced operation identity");
    std::map<std::wstring,Manifest> found;size_t total=0;
    auto add=[&](const JsonObject& object,const std::wstring& name){const auto value=Read(object);
        if(value.name!=name)throw std::runtime_error("Object reference name differs");
        if(found.contains(name)){if(std::wstring(Encode(found.at(name)).Stringify())!=std::wstring(Encode(value).Stringify()))throw std::runtime_error("Repeated object descriptor differs");return;}
        if(found.size()>=maxObjects||value.size>maxObjects*tcard_asset::max_file-total)throw std::runtime_error("Reference object budget exceeded");
        total+=value.size;found.emplace(name,value);
    };
    for(const auto* side:{L"Before",L"After"}){const auto state=operation.GetNamedObject(side);const auto name=sync_string(state,L"SourceFile");
        if(state.HasKey(L"Source")||!tcard_asset::relative(name,false))throw std::runtime_error("Invalid referenced source");add(state.GetNamedObject(L"Object"),name);}
    const auto assets=note.GetNamedArray(L"Assets");if(assets.Size()>maxObjects)throw std::runtime_error("Reference image budget exceeded");std::set<std::wstring> names;
    for(const auto& entry:assets){const auto asset=entry.GetObject();const auto name=sync_string(asset,L"Name");
        if(asset.HasKey(L"Bytes")||!tcard_asset::relative(name,true)||!names.insert(name).second)throw std::runtime_error("Invalid referenced image");add(asset.GetNamedObject(L"Object"),name);}
    std::vector<Manifest> result;for(const auto& [name,value]:found)result.push_back(value);return result;
}
inline std::wstring Slot(const Manifest& value){return sync_hash(value.name);}
class Cache {
    std::wstring storage_,root_,group_;Manifest manifest_;
    void Check()const
    {
        if(tcard_chunks::Group(storage_)!=group_)throw std::runtime_error("Object key or group unavailable");
        tcard_asset::File checked(tcard_asset::open_dir(root_,false));if(!checked)throw std::runtime_error("Unsafe object directory");
        std::wstring text;if(!tcard_bridge::Read(root_,L"manifest.json",text))throw std::runtime_error("Object manifest unavailable");
        auto expected=Encode(manifest_);expected.Insert(L"Group",JsonValue::CreateStringValue(group_));
        if(text!=std::wstring(expected.Stringify()))throw std::runtime_error("Object manifest differs");
    }
    size_t Length(size_t index)const
    {
        if(index>=manifest_.hashes.size())throw std::runtime_error("Object fragment index outside manifest");
        return (std::min)(tcard_chunks::chunkSize,manifest_.size-index*tcard_chunks::chunkSize);
    }
    bool Load(size_t index,Bytes& bytes)const
    {
        return tcard_asset::read(root_,std::to_wstring(index)+L".part",bytes,tcard_chunks::chunkSize)&&bytes.size()==Length(index)&&
            tcard_asset::hash(bytes)==manifest_.hashes[index];
    }
public:
    Cache(const std::wstring& storage,const Manifest& manifest,const std::wstring& group,bool create=true,const wchar_t* lane=L"_objects"):storage_(tcard_chunks::budget::Extended(sync_path_plain(storage))),group_(group),manifest_(Read(Encode(manifest)))
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Object quota busy");
        std::lock_guard guard(objectMutex);
        if((wcscmp(lane,L"_objects")&&wcscmp(lane,L"_sealed"))||(create&&wcscmp(lane,L"_objects")))throw std::runtime_error("Invalid object custody lane");
        if(tcard_chunks::Group(storage_)!=group_)throw std::runtime_error("Object group differs");
        const auto folder=storage_+L"\\"+lane;root_=folder+L"\\"+Slot(manifest_);
        if(!create){Check();return;}
        if(!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect object cache");
        if(GetFileAttributesW(root_.c_str())==INVALID_FILE_ATTRIBUTES){
            size_t retained=0;WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW((folder+L"\\*").c_str(),&entry);
            if(find!=INVALID_HANDLE_VALUE){struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};
                do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
                    if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Unsafe object cache entry");
                    if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)++retained;
                }while(FindNextFileW(find,&entry));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Object cache enumeration failed");
            }else if(GetLastError()!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot enumerate object cache");
            // Bound provisional disk custody without expiring or evicting interrupted intent.
            if(retained>=maxObjects)throw std::runtime_error("Retained object budget reached");
        }
        auto reservation=Encode(manifest_);reservation.Insert(L"Group",JsonValue::CreateStringValue(group_));const auto record=std::wstring(reservation.Stringify());
        tcard_chunks::budget::Reserve(storage_,root_,tcard_bridge::Hash(record),tcard_chunks::budget::Charge(manifest_.size,manifest_.hashes.size(),tcard_asset::encode(record).size()));
        if(!tcard_bridge::Protect(root_))throw std::runtime_error("Cannot protect object directory");
        if(!sync_exists(root_,L"manifest.json")){
            auto saved=Encode(manifest_);saved.Insert(L"Group",JsonValue::CreateStringValue(group_));
            if(!tcard_bridge::Publish(root_,L"manifest.json",std::wstring(saved.Stringify())))throw std::runtime_error("Cannot retain object manifest");
        }
        Check();
    }
    std::vector<size_t> Missing()const
    {
        std::lock_guard guard(objectMutex);Check();std::vector<size_t> result;Bytes bytes;
        for(size_t i=0;i<manifest_.hashes.size();++i)if(!Load(i,bytes))result.push_back(i);return result;
    }
    void Put(size_t index,const Bytes& bytes)
    {
        std::lock_guard guard(objectMutex);
        // The non-create directory handle prevents concurrent Remove/Promote and stale path recreation.
        tcard_asset::File directory(tcard_asset::open_dir(root_,false));if(!directory)throw std::runtime_error("Retired object cache unavailable");Check();
        if(bytes.size()!=Length(index)||tcard_asset::hash(bytes)!=manifest_.hashes[index])throw std::runtime_error("Object fragment differs");
        Bytes existing;if(Load(index,existing)&&existing==bytes)return;
        const auto leaf=std::to_wstring(index)+L".part";
        if(!tcard_asset::publish(root_,leaf,bytes)||!Load(index,existing)||existing!=bytes)throw std::runtime_error("Cannot verify object fragment");
    }
    Bytes Assemble()const
    {
        std::lock_guard guard(objectMutex);Check();Bytes result;result.reserve(manifest_.size);Bytes bytes;
        for(size_t i=0;i<manifest_.hashes.size();++i){if(!Load(i,bytes))throw std::runtime_error("Object fragment remains missing");result.insert(result.end(),bytes.begin(),bytes.end());}
        if(result.size()!=manifest_.size||tcard_asset::hash(result)!=manifest_.hash)throw std::runtime_error("Complete object hash differs");return result;
    }
};
inline Bytes Resolve(const std::wstring& storage,const JsonObject& object,const std::wstring& name,size_t& budget)
{
    const auto manifest=Read(object);if(manifest.name!=name||manifest.size>budget)throw std::runtime_error("Object reference differs or exceeds budget");
    budget-=manifest.size;Cache cache(storage,manifest,tcard_chunks::Group(storage),false);return cache.Assemble();
}
}
