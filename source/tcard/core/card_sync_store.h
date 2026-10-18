#pragma once
#include "card_sync_merge.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <mutex>

namespace tcard_sync {
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonValue;
inline std::recursive_mutex sync_mutex;
inline thread_local const std::map<std::wstring,std::vector<Change>>* sync_importing = nullptr;

inline std::wstring sync_stem(std::wstring id)
{
    for (auto& c : id) if (std::wstring(L"\\/:*?\"<>|").find(c)!=std::wstring::npos) c=L'_';
    while (!id.empty() && (id.back()==L'.' || id.back()==L' ')) id.pop_back();
    return id.empty() ? L"card" : id;
}
inline std::wstring sync_path(const std::wstring& directory)
{
    wchar_t full[32768]{};
    const auto count=GetFullPathNameW(directory.c_str(),32768,full,nullptr);
    if(!count||count>=32768)return {};
    std::wstring path=full;for(auto& c:path)c=static_cast<wchar_t>(towlower(c));
    while(path.size()>3&&path.back()==L'\\')path.pop_back();return path;
}
inline std::wstring sync_path_plain(const std::wstring& directory)
{
    auto path=sync_path(directory);
    if(path.rfind(L"\\\\?\\unc\\",0)==0)path=L"\\\\"+path.substr(8);
    else if(path.rfind(L"\\\\?\\",0)==0)path.erase(0,4);
    return path;
}
inline std::wstring sync_owner(const std::wstring& directory)
{
    auto path=sync_path_plain(directory);const auto generation=path.find(L"\\_native-generations\\");
    if(generation!=std::wstring::npos)path.resize(generation);return path;
}
struct SyncLock {
    HANDLE handle=nullptr;
    bool acquired=false;
    explicit SyncLock(const std::wstring& directory) {
        const auto path=sync_owner(directory);if(path.empty())return;
        // Public roots, generation paths and extended aliases serialize with one native owner.
        const auto name=L"Local\\TCard.Store."+sync_hash(path);
        handle=CreateMutexW(nullptr,FALSE,name.c_str());
        if(handle){const auto wait=WaitForSingleObject(handle,5000);acquired=wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED;}
    }
    ~SyncLock(){if(acquired)ReleaseMutex(handle);if(handle)CloseHandle(handle);}
    explicit operator bool() const{return acquired;}
};
inline bool sync_hex(const std::wstring& value)
{
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});
}
inline std::wstring sync_string(const JsonObject& object,const wchar_t* key)
{
    return std::wstring(object.GetNamedString(key));
}
inline uint64_t sync_number(const JsonObject& object,const wchar_t* key)
{
    const auto text=sync_string(object,key);
    size_t end=0; const auto n=std::stoull(text,&end);
    if(end!=text.size()||std::to_wstring(n)!=text)throw std::runtime_error("Invalid integer");
    return n;
}
inline bool sync_exists(const std::wstring& root,const std::wstring& name)
{
    return GetFileAttributesW((root+L"\\"+name).c_str())!=INVALID_FILE_ATTRIBUTES;
}
inline bool sync_read(const std::wstring& root,const std::wstring& name,JsonObject& object)
{
    std::vector<BYTE> bytes;
    if(!tcard_asset::read(root,name,bytes))return false;
    object=JsonObject::Parse(tcard_asset::decode(bytes));return true;
}
inline bool sync_write(const std::wstring& root,const std::wstring& name,const JsonObject& object,bool immutable=false)
{
    const auto bytes=tcard_asset::encode(std::wstring(object.Stringify()));
    if(!tcard_asset::publish(root,name,bytes,immutable))return false;
    std::vector<BYTE> verified;
    return tcard_asset::read(root,name,verified)&&verified==bytes;
}
inline Snapshot sync_snapshot(const tcard::CardRecord& card)
{
    Snapshot state{card.source,card.title.empty()?L"Untitled card":card.title,card.richHtml?2u:card.markdown?1u:0u,false,sync_display(card)};
    const Dates dates{sync_read_time(card.createdUtc),sync_read_time(card.updatedUtc)};
    if(dates.created&&dates.updated)state.dates=dates;return state;
}
inline std::vector<std::wstring> sync_ids(const JsonObject& manifest)
{
    std::vector<std::wstring> ids;
    if(!manifest.HasKey(L"SyncVersion"))return ids;
    if(manifest.GetNamedNumber(L"SyncVersion")!=1)throw std::runtime_error("Unsupported sync version");
    const auto array=manifest.GetNamedArray(L"SyncChanges");
    if(array.Size()>tcard_asset::max_file/66)throw std::runtime_error("Journal index too large");
    for(unsigned i=0;i<array.Size();++i){auto id=std::wstring(array.GetStringAt(i));if(!sync_hex(id))throw std::runtime_error("Invalid change ID");ids.push_back(std::move(id));}
    if(!std::is_sorted(ids.begin(),ids.end())||std::adjacent_find(ids.begin(),ids.end())!=ids.end())throw std::runtime_error("Invalid journal index");
    return ids;
}
inline std::wstring sync_fingerprint(const std::vector<std::wstring>& ids,const std::vector<std::wstring>& heads)
{
    std::wstring joined=L"TCard.Frontier.v1:";joined.reserve((ids.size()+heads.size())*64+32);
    for(const auto& id:ids)joined+=id;joined+=L":";for(const auto& id:heads)joined+=id;return sync_hash(joined);
}
inline void sync_commit(JsonObject& manifest,std::vector<std::wstring> ids,const std::vector<std::wstring>& heads)
{
    std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    JsonArray array;for(const auto& id:ids)array.Append(JsonValue::CreateStringValue(id));
    JsonArray frontier;for(const auto& id:heads)frontier.Append(JsonValue::CreateStringValue(id));
    manifest.Insert(L"SyncVersion",JsonValue::CreateNumberValue(1));manifest.Insert(L"SyncChanges",array);
    manifest.Insert(L"SyncHeads",frontier);manifest.Insert(L"SyncIndexHash",JsonValue::CreateStringValue(sync_fingerprint(ids,heads)));
}
inline void sync_index(JsonObject& manifest,const std::vector<Change>& changes)
{
    std::set<std::wstring> ids,heads;
    for(const auto& c:changes){ids.insert(c.id);heads.insert(c.id);}
    for(const auto& c:changes)for(const auto& parent:c.parents)heads.erase(parent);
    sync_commit(manifest,{ids.begin(),ids.end()},{heads.begin(),heads.end()});
}
inline std::wstring sync_archive(const std::wstring& directory,const std::wstring& note)
{
    return directory+L"\\_sync\\"+sync_stem(note);
}
inline bool sync_prepare(const std::wstring& directory,const std::wstring& note)
{
    tcard_asset::File folder(tcard_asset::open_dir(directory+L"\\_sync",true));if(!folder)return false;
    tcard_asset::File archive(tcard_asset::open_dir(sync_archive(directory,note),true));return static_cast<bool>(archive);
}
inline JsonObject sync_pack_display(const Display& d)
{
    JsonObject value;value.Insert(L"Color",JsonValue::CreateStringValue(d.color));value.Insert(L"FontFamily",JsonValue::CreateStringValue(d.fontFamily));
    value.Insert(L"FontSize",JsonValue::CreateNumberValue(d.fontSize));value.Insert(L"InheritFontFamily",JsonValue::CreateBooleanValue(d.inheritFontFamily));
    value.Insert(L"InheritFontSize",JsonValue::CreateBooleanValue(d.inheritFontSize));value.Insert(L"Live",JsonValue::CreateBooleanValue(d.live));
    return value;
}
inline std::optional<Display> sync_read_display(const JsonObject& state)
{
    if(!state.HasKey(L"Display"))return {};const auto value=state.GetNamedObject(L"Display");Display d;
    d.color=sync_string(value,L"Color");d.fontFamily=sync_string(value,L"FontFamily");d.fontSize=value.GetNamedNumber(L"FontSize");
    d.inheritFontFamily=value.GetNamedBoolean(L"InheritFontFamily");d.inheritFontSize=value.GetNamedBoolean(L"InheritFontSize");d.live=value.GetNamedBoolean(L"Live");
    if(d.color.empty()||d.color.size()>128||d.fontFamily.empty()||d.fontFamily.size()>512||!std::isfinite(d.fontSize)||d.fontSize<8||d.fontSize>48)throw std::runtime_error("Invalid display settings");
    return d;
}
inline std::optional<Dates> sync_read_dates(const JsonObject& object)
{
    if(!object.HasKey(L"Dates"))return {};
    const auto value=object.GetNamedObject(L"Dates");const Dates dates{sync_number(value,L"CreatedUtcMs"),sync_number(value,L"UpdatedUtcMs")};
    if(!dates.created||!dates.updated||dates.created>1000000000000000ULL||dates.updated>1000000000000000ULL)
        throw std::runtime_error("Invalid authored dates");return dates;
}
inline JsonObject sync_pack(const Change& change,bool source)
{
    JsonObject object;
    object.Insert(L"Version",JsonValue::CreateNumberValue(change.before.dates||change.after.dates?3:change.before.display||change.after.display?2:1));
    const auto text=[&](const wchar_t* key,const std::wstring& value){object.Insert(key,JsonValue::CreateStringValue(value));};
    text(L"Id",change.id);text(L"Note",change.note);text(L"Actor",change.actor);
    text(L"Sequence",std::to_wstring(change.sequence));text(L"Time",std::to_wstring(change.time));
    JsonArray parents;for(const auto& id:change.parents)parents.Append(JsonValue::CreateStringValue(id));object.Insert(L"Parents",parents);
    for(bool after:{false,true}){
        const auto& state=after?change.after:change.before;JsonObject value;
        value.Insert(L"Title",JsonValue::CreateStringValue(state.title));
        value.Insert(L"Format",JsonValue::CreateNumberValue(state.format));
        value.Insert(L"Deleted",JsonValue::CreateBooleanValue(state.deleted));
        if(state.display)value.Insert(L"Display",sync_pack_display(*state.display));
        if(state.dates){JsonObject dates;
            dates.Insert(L"CreatedUtcMs",JsonValue::CreateStringValue(std::to_wstring(state.dates->created)));
            dates.Insert(L"UpdatedUtcMs",JsonValue::CreateStringValue(std::to_wstring(state.dates->updated)));value.Insert(L"Dates",dates);}
        const auto extension=state.format==2?L".html":state.format==1?L".md":L".txt";
        value.Insert(L"SourceFile",JsonValue::CreateStringValue(L"note-"+sync_hash(state.source)+extension));
        if(source)value.Insert(L"Source",JsonValue::CreateStringValue(state.source));
        object.Insert(after?L"After":L"Before",value);
    }
    return object;
}
inline Change sync_unpack(const JsonObject& object,const std::wstring& archive,bool source)
{
    const auto version=object.GetNamedNumber(L"Version");if(version!=1&&version!=2&&version!=3)throw std::runtime_error("Unsupported operation version");
    Change c;c.id=sync_string(object,L"Id");c.note=sync_string(object,L"Note");c.actor=sync_string(object,L"Actor");
    if(c.note.empty()||c.note.size()>200||!tcard_asset::leaf(sync_stem(c.note))||sync_stem(c.note)[0]==L'_'||c.actor.size()>100)
        throw std::runtime_error("Invalid operation identity");
    c.sequence=sync_number(object,L"Sequence");c.time=sync_number(object,L"Time");
    if(c.time>1000000000000000ULL)throw std::runtime_error("Invalid operation clock");
    const auto parents=object.GetNamedArray(L"Parents");if(parents.Size()>512)throw std::runtime_error("Too many dependencies");
    for(unsigned i=0;i<parents.Size();++i){const auto id=std::wstring(parents.GetStringAt(i));if(!sync_hex(id))throw std::runtime_error("Invalid dependency");c.parents.push_back(id);}
    if(!std::is_sorted(c.parents.begin(),c.parents.end())||std::adjacent_find(c.parents.begin(),c.parents.end())!=c.parents.end())throw std::runtime_error("Invalid dependency order");
    for(bool after:{false,true}){
        auto& state=after?c.after:c.before;const auto value=object.GetNamedObject(after?L"After":L"Before");
        const auto format=value.GetNamedNumber(L"Format");if(format<0||format>2||floor(format)!=format)throw std::runtime_error("Invalid content format");
        state.format=static_cast<unsigned>(format);state.title=sync_string(value,L"Title");state.deleted=value.GetNamedBoolean(L"Deleted");state.display=sync_read_display(value);state.dates=sync_read_dates(value);
        const auto name=sync_string(value,L"SourceFile");if(!tcard_asset::relative(name,false))throw std::runtime_error("Invalid source reference");
        if(source)state.source=sync_string(value,L"Source");else{
            std::vector<BYTE> bytes;if(!tcard_asset::read(archive,name,bytes))throw std::runtime_error("Missing archived source");state.source=tcard_asset::decode(bytes);
        }
        const auto expected=L"note-"+sync_hash(state.source)+(state.format==2?L".html":state.format==1?L".md":L".txt");
        if(name!=expected||state.source.size()>4*1024*1024||state.title.size()>32768)throw std::runtime_error("Invalid source hash or size");
    }
    if((c.before.dates||c.after.dates?3:c.before.display||c.after.display?2:1)!=version)throw std::runtime_error("Operation display version mismatch");
    if(c.id!=sync_identify(c))throw std::runtime_error("Invalid operation hash");return c;
}
inline bool sync_store(const std::wstring& directory,const Change& c)
{
    if(!sync_prepare(directory,c.note))return false;
    const auto archive=sync_archive(directory,c.note);
    for(const auto* state:{&c.before,&c.after}){
        std::wstring name;if(!tcard_asset::version(archive,state->source,state->format==1,name,state->format==2))return false;
    }
    return sync_write(archive,c.id+L".json",sync_pack(c,false),true);
}
inline std::vector<Change> sync_load(const std::wstring& directory,const std::wstring& note,const std::vector<std::wstring>& ids)
{
    std::vector<Change> changes;const auto archive=sync_archive(directory,note);
    for(const auto& id:ids){JsonObject object;if(!sync_read(archive,id+L".json",object))throw std::runtime_error("Missing operation");auto c=sync_unpack(object,archive,false);if(c.note!=note||c.id!=id)throw std::runtime_error("Wrong journal identity");changes.push_back(std::move(c));}
    return changes;
}
inline std::vector<std::wstring> sync_heads(const std::vector<Change>& changes)
{
    std::set<std::wstring> heads;
    for(const auto& c:changes)heads.insert(c.id);
    for(const auto& c:changes)for(const auto& parent:c.parents)heads.erase(parent);
    return {heads.begin(),heads.end()};
}
struct SyncImages {
    std::set<std::wstring> names;
    static int block(MD_BLOCKTYPE,void*,void*){return 0;}
    static int leave(MD_SPANTYPE,void*,void*){return 0;}
    static int text(MD_TEXTTYPE,const MD_CHAR*,MD_SIZE,void*){return 0;}
    static int enter(MD_SPANTYPE type,void* detail,void* context){
        if(type==MD_SPAN_IMG){const auto& src=static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src;
            const std::wstring name(src.text,src.size);if(tcard_asset::relative(name,true))static_cast<SyncImages*>(context)->names.insert(name);}
        return 0;
    }
};
inline std::set<std::wstring> sync_images(const Snapshot& state)
{
    SyncImages images;
    if(state.format==2){for(const auto& p:tcard_rich::parse(state.source).paragraphs)for(const auto& r:p.runs)
        if(tcard_asset::relative(r.image,true))images.names.insert(r.image);
    }else if(state.format==1){
        MD_PARSER parser{};parser.flags=MD_FLAG_TABLES|MD_FLAG_TASKLISTS|MD_FLAG_STRIKETHROUGH|MD_FLAG_FOOTNOTES|MD_FLAG_ADMONITIONS;
        parser.enter_block=parser.leave_block=SyncImages::block;parser.enter_span=SyncImages::enter;parser.leave_span=SyncImages::leave;parser.text=SyncImages::text;
        if(md_parse(state.source.data(),static_cast<MD_SIZE>(state.source.size()),&parser,&images))throw std::runtime_error("Markdown parse failed");
    }
    return images.names;
}
inline bool sync_archive_images(const std::wstring& directory,const std::wstring& note,const Snapshot& state)
{
    const auto archive=sync_archive(directory,note),bundle=directory+L"\\"+sync_stem(note);
    for(const auto& image:sync_images(state)){
        std::vector<BYTE> bytes;
        if(!tcard_asset::read(archive,image,bytes)){
            if(!tcard_asset::read(bundle,image,bytes))continue; // Preserve existing saves with unresolved image references.
            if(tcard_asset::hash(bytes)!=image.substr(7,64))return false;
            if(!tcard_asset::publish(archive,image,bytes,true))return false;
        }else if(tcard_asset::hash(bytes)!=image.substr(7,64))return false;
    }
    return true;
}
inline std::wstring sync_guid()
{
    GUID guid{};wchar_t text[40]{};
    if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,text,40))throw std::runtime_error("Cannot allocate actor");return text;
}
struct SyncOrigin {
    std::wstring actor;
    uint64_t sequence=0;
};
// Protected by sync_mutex and the storage mutex. Only this process can own a cached origin.
inline std::map<std::wstring,SyncOrigin> sync_origins;
inline void sync_origin(const std::wstring& directory,Change& change,const std::vector<Change>& changes)
{
    const auto root=directory+L"\\_sync",path=sync_path(directory);JsonObject actor;
    if(path.empty())throw std::runtime_error("Invalid actor storage path");
    const bool exists=sync_exists(root,L"actor.json");std::wstring persisted;uint64_t previous=0;
    if(exists){
        if(!sync_read(root,L"actor.json",actor))throw std::runtime_error("Unreadable actor");
        persisted=sync_string(actor,L"Id");previous=sync_number(actor,L"Sequence");
        if(persisted.empty()||persisted.size()>100)throw std::runtime_error("Invalid actor identity");
    }
    auto& origin=sync_origins[path];
    // First edit after launch, a copied root, rollback, or another writer starts a fresh epoch.
    // Keep the v1 wire identity unchanged: a fresh actor GUID represents the new epoch.
    if(origin.actor.empty()||!exists||origin.actor!=persisted||origin.sequence!=previous){
        origin.actor=sync_guid();origin.sequence=0;
    }
    if(origin.sequence==UINT64_MAX)throw std::runtime_error("Actor sequence exhausted");
    change.actor=origin.actor;change.sequence=++origin.sequence;
    actor.Insert(L"Id",JsonValue::CreateStringValue(change.actor));
    FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER ticks{};ticks.LowPart=now.dwLowDateTime;ticks.HighPart=now.dwHighDateTime;change.time=ticks.QuadPart/10000;
    for(const auto& c:changes){if(c.time==UINT64_MAX)throw std::runtime_error("Ordering exhausted");change.time=(std::max)(change.time,c.time+1);}
    if(change.time>1000000000000000ULL)throw std::runtime_error("Ordering clock exhausted");
    actor.Insert(L"Sequence",JsonValue::CreateStringValue(std::to_wstring(change.sequence)));
    if(!sync_write(root,L"actor.json",actor))throw std::runtime_error("Cannot persist actor");
}
inline void sync_set_state(JsonObject& manifest,const Snapshot& state)
{
    Change value;value.after=state;manifest.Insert(L"SyncState",sync_pack(value,false).GetNamedObject(L"After"));
}
inline Snapshot sync_read_state(const JsonObject& manifest,const std::wstring& archive)
{
    const auto state=manifest.GetNamedObject(L"SyncState");Snapshot value;
    const auto format=state.GetNamedNumber(L"Format");if(format<0||format>2||floor(format)!=format)throw std::runtime_error("Invalid state format");
    value.format=static_cast<unsigned>(format);value.title=sync_string(state,L"Title");value.deleted=state.GetNamedBoolean(L"Deleted");value.display=sync_read_display(state);value.dates=sync_read_dates(state);
    const auto name=sync_string(state,L"SourceFile");std::vector<BYTE> bytes;
    if(!tcard_asset::relative(name,false)||!tcard_asset::read(archive,name,bytes)||tcard_asset::hash(bytes)!=name.substr(5,64))throw std::runtime_error("Invalid archived state");
    value.source=tcard_asset::decode(bytes);return value;
}
inline std::vector<Change> sync_headers(const std::wstring& directory,const std::wstring& note,const std::vector<std::wstring>& ids)
{
    std::vector<Change> changes;
    for(const auto& id:ids){JsonObject object;if(!sync_read(sync_archive(directory,note),id+L".json",object))throw std::runtime_error("Missing operation header");
        Change c;c.id=sync_string(object,L"Id");c.note=sync_string(object,L"Note");c.actor=sync_string(object,L"Actor");c.time=sync_number(object,L"Time");
        if(c.id!=id||c.note!=note)throw std::runtime_error("Invalid operation header");
        const auto parents=object.GetNamedArray(L"Parents");for(unsigned i=0;i<parents.Size();++i)c.parents.push_back(std::wstring(parents.GetStringAt(i)));changes.push_back(std::move(c));
    }
    return changes;
}
inline std::vector<Change> sync_frontier(const std::wstring& directory,const std::wstring& note,
    const JsonObject& manifest,const std::vector<std::wstring>& ids)
{
    if(manifest.HasKey(L"SyncHeads")&&manifest.HasKey(L"SyncIndexHash")){
        const auto array=manifest.GetNamedArray(L"SyncHeads");std::vector<std::wstring> heads;
        for(unsigned i=0;i<array.Size();++i)heads.push_back(std::wstring(array.GetStringAt(i)));
        if(sync_string(manifest,L"SyncIndexHash")!=sync_fingerprint(ids,heads))return sync_headers(directory,note,ids);
        for(const auto& id:heads)if(!std::binary_search(ids.begin(),ids.end(),id))throw std::runtime_error("Invalid journal head");
        if((heads.empty()!=ids.empty())||!std::is_sorted(heads.begin(),heads.end())||std::adjacent_find(heads.begin(),heads.end())!=heads.end())
            throw std::runtime_error("Invalid journal frontier");
        return sync_headers(directory,note,heads);
    }
    return sync_headers(directory,note,ids);
}
inline bool sync_stage(const std::wstring& directory,const std::wstring& note,const tcard::CardRecord& previous,
    const tcard::CardRecord& next,const JsonObject& oldManifest,JsonObject& manifest,bool deleted=false)
{
    if(sync_importing){
        const auto found=sync_importing->find(note);if(found==sync_importing->end())return false;
        Snapshot state;std::wstring error;if(!sync_merge(found->second,state,error))return false;
        if(!deleted&&!sync_matches(sync_snapshot(next),state))return false;
        std::wstring name;if(!tcard_asset::version(sync_archive(directory,note),state.source,state.format==1,name,state.format==2))return false;
        sync_index(manifest,found->second);sync_set_state(manifest,state);return true;
    }
    auto ids=sync_ids(oldManifest);
    JsonObject marker;
    const auto markerName=sync_stem(note)+L".deleted";
    if(sync_exists(directory,markerName)){
        std::vector<BYTE> bytes;if(!tcard_asset::read(directory,markerName,bytes))return false;
        if(JsonObject::TryParse(tcard_asset::decode(bytes),marker)&&marker.HasKey(L"SyncVersion")){
            const auto extra=sync_ids(marker);ids.insert(ids.end(),extra.begin(),extra.end());std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
        }
    }
    const auto oldIds=sync_ids(oldManifest);
    const auto markerIds=sync_ids(marker);
    const auto& committed=oldIds==ids?oldManifest:markerIds==ids?marker:oldManifest;
    auto changes=sync_frontier(directory,note,committed,ids);
    if(!sync_prepare(directory,note))return false;
    Snapshot before;
    if(!changes.empty()){
        const auto archive=sync_archive(directory,note);
        if(committed.HasKey(L"SyncState")&&sync_ids(committed)==ids)before=sync_read_state(committed,archive);
        else {std::wstring error;if(!sync_merge(sync_load(directory,note,ids),before,error))return false;}
        // Legacy writers can update ordinary content without retaining sync metadata.
        if(!previous.id.empty()&&!sync_exists(directory,markerName)&&before!=sync_snapshot(previous)){
            Change bridge;bridge.note=note;bridge.before=before;bridge.after=sync_snapshot(previous);bridge.parents=sync_heads(changes);
            sync_origin(directory,bridge,changes);bridge.id=sync_identify(bridge);
            if(!sync_store(directory,bridge)||!sync_archive_images(directory,note,bridge.after))return false;changes.push_back(bridge);before=bridge.after;
        }
    }else if(!previous.id.empty()){
        Change baseline;baseline.note=note;baseline.actor=L"baseline";baseline.after=sync_snapshot(previous);baseline.id=sync_identify(baseline);
        if(!sync_store(directory,baseline)||!sync_archive_images(directory,note,baseline.after))return false;
        changes.push_back(baseline);before=baseline.after;
    }
    auto after=sync_snapshot(next);after.deleted=deleted;
    if(after!=before){
        Change c;c.note=note;c.before=before;c.after=after;c.parents=sync_heads(changes);
        sync_origin(directory,c,changes);c.id=sync_identify(c);
        if(!sync_store(directory,c)||!sync_archive_images(directory,note,c.after))return false;changes.push_back(std::move(c));
    }
    for(const auto& c:changes)ids.push_back(c.id);
    sync_commit(manifest,std::move(ids),sync_heads(changes));sync_set_state(manifest,after);return true;
}
inline bool sync_deleted(const std::wstring& directory,const std::wstring& stem)
{
    if(!sync_exists(directory,stem+L".deleted"))return false;
    // A committed restore can precede marker cleanup. Recover without hiding its readable bundle.
    try{
        JsonObject marker,current;
        if(!sync_read(directory,stem+L".deleted",marker)||!sync_read(directory+L"\\"+stem,L"card.json",current)||
            !marker.HasKey(L"SyncVersion")||!current.HasKey(L"SyncVersion"))return true;
        const auto old=sync_ids(marker),now=sync_ids(current);
        if(old.empty()||now==old||!std::includes(now.begin(),now.end(),old.begin(),old.end()))return true;
        Snapshot state;std::wstring error;
        if(current.HasKey(L"SyncState"))state=sync_read_state(current,sync_archive(directory,sync_string(current,L"Id")));
        else if(!sync_merge(sync_load(directory,sync_string(current,L"Id"),now),state,error))return true;
        if(state.deleted)return true;
        DeleteFileW((directory+L"\\"+stem+L".deleted").c_str());return false;
    }catch(...){return true;}
}
}
