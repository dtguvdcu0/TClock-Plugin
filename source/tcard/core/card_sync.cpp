#include "card_sync_units.h"
#include "card_sync_objects.h"
#include "card_sync_dependencies.h"
#include "card_native_root.h"
#include <memory>

namespace tcard_sync {
std::wstring IdentifyChange(const Change& change) { return sync_identify(change); }
bool MergeChanges(const std::vector<Change>& changes,Snapshot& merged,std::wstring& error)
{
    return sync_merge(changes,merged,error);
}
static std::wstring sync_directory(const std::wstring& path)
{
    // Extend concrete file I/O without changing the caller's transport/node identity.
    return tcard_native::Extended(tcard_native::Resolve(path));
}
static void sync_seed(const std::wstring& storagePath,const std::wstring& directory,
    const std::vector<tcard::CardRecord>& cards,bool readonly=false)
{
    for(const auto& card:cards){
        JsonObject manifest;
        if(!sync_read(directory+L"\\"+sync_stem(card.id),L"card.json",manifest))throw std::runtime_error("Unreadable card");
        const bool incomplete=manifest.GetNamedNumber(L"CreatedUtcMs",0)!=static_cast<double>(sync_read_time(card.createdUtc))||
            manifest.GetNamedNumber(L"UpdatedUtcMs",0)!=static_cast<double>(sync_read_time(card.updatedUtc));
        if(incomplete||!manifest.HasKey(L"SyncVersion")||!manifest.HasKey(L"SyncState")||(!manifest.GetNamedObject(L"SyncState").HasKey(L"Display")||!manifest.GetNamedObject(L"SyncState").HasKey(L"Dates"))){
            if(readonly)throw std::runtime_error("Legacy journal requires UI preparation");
            std::vector<tcard::CardRecord> saved{card};
            if(!tcard::SaveCards(storagePath,saved,false))throw std::runtime_error("Cannot seed legacy journal");
        }
    }
}
static bool sync_alias(const std::wstring& a,const std::wstring& b)
{
    const auto left=sync_stem(a),right=sync_stem(b);
    return CompareStringOrdinal(left.c_str(),static_cast<int>(left.size()),right.c_str(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
static std::map<std::wstring,JsonObject> sync_manifests(const std::wstring& directory,bool readonly=false,
    const std::vector<std::wstring>* selected=nullptr)
{
    std::map<std::wstring,JsonObject> found;WIN32_FIND_DATAW entry{};
    const auto handle=FindFirstFileW((directory+L"\\*").c_str(),&entry);
    if(handle==INVALID_HANDLE_VALUE)return found;
    struct Scope{HANDLE handle;~Scope(){FindClose(handle);}} scope{handle};
    do{
        const std::wstring name=entry.cFileName;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT||name.empty()||name[0]==L'_')continue;
        if(selected){
            const bool folder=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            if(!folder&&(name.size()<9||name.substr(name.size()-8)!=L".deleted"))continue;
            const auto stem=folder?name:name.substr(0,name.size()-8);
            if(std::none_of(selected->begin(),selected->end(),[&](const auto& id){return sync_alias(stem,id);}))continue;
        }
        JsonObject manifest;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            if(!tcard_asset::leaf(name)||sync_deleted(directory,name))continue;
            if(!sync_read(directory+L"\\"+name,L"card.json",manifest))continue;
        }else{
            if(name.size()<9||name.substr(name.size()-8)!=L".deleted")continue;
            const auto stem=name.substr(0,name.size()-8);if(!sync_deleted(directory,stem))continue;
            std::vector<BYTE> bytes;if(!tcard_asset::read(directory,name,bytes))throw std::runtime_error("Unreadable deletion");
            if(!JsonObject::TryParse(tcard_asset::decode(bytes),manifest)||!manifest.HasKey(L"SyncVersion")){
                if(readonly)throw std::runtime_error("Legacy journal requires UI preparation");
                const auto id=tcard_asset::decode(bytes);tcard::CardRecord card;card.id=id.empty()?stem:id;
                JsonObject seeded;
                if(!sync_stage(directory,card.id,{},card,{},seeded,true))throw std::runtime_error("Cannot seed deletion");
                seeded.Insert(L"Id",JsonValue::CreateStringValue(card.id));
                if(!sync_write(directory,name,seeded))throw std::runtime_error("Cannot publish deletion");manifest=seeded;
            }
        }
        const auto id=sync_string(manifest,L"Id");if(sync_stem(id)!=(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY?name:name.substr(0,name.size()-8)))
            throw std::runtime_error("Manifest identity mismatch");
        found[id]=manifest;
    }while(FindNextFileW(handle,&entry));
    return found;
}
// Only genuine absence can be requested again. Existing unreadable/corrupt evidence remains fail-closed.
static bool sync_check_image(const std::wstring& root,const std::wstring& name,std::vector<BYTE>& bytes)
{
    if(!tcard_asset::read(root,name,bytes)){
        auto relative=name;std::replace(relative.begin(),relative.end(),L'/',L'\\');
        const auto attributes=GetFileAttributesW((root+L"\\"+relative).c_str());const auto error=GetLastError();
        if(attributes==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND))return false;
        throw std::runtime_error("Unreadable retained image");
    }
    if(tcard_asset::hash(bytes)!=name.substr(7,64))throw std::runtime_error("Invalid retained image hash");return true;
}
static std::wstring sync_capture(const std::wstring& directory,bool readonly,unsigned* maximumVersion,Result& result)
{
    JsonObject envelope;envelope.Insert(L"Version",JsonValue::CreateNumberValue(1));JsonArray notes;
    for(const auto& [id,manifest]:sync_manifests(directory,readonly)){
        JsonObject note;note.Insert(L"Id",JsonValue::CreateStringValue(id));JsonArray operations;
        const auto changes=sync_load(directory,id,sync_ids(manifest));std::set<std::wstring> names;
        for(const auto& change:changes){
            if(maximumVersion){
                // Match sync_pack operation versions; old IDs and canonical bytes remain unchanged.
                const unsigned version=change.before.dates||change.after.dates?3u:change.before.display||change.after.display?2u:1u;
                *maximumVersion=(std::max)(*maximumVersion,version);operations.Append(JsonValue::CreateStringValue(change.id));
            }else operations.Append(sync_pack(change,true));
            for(const auto* state:{&change.before,&change.after}){const auto images=sync_images(*state);names.insert(images.begin(),images.end());}
        }
        JsonArray assets;const auto archive=sync_archive(directory,id);
        const auto visible=sync_deleted(directory,sync_stem(id))?std::set<std::wstring>{}:sync_images(sync_read_state(manifest,archive));
        for(const auto& name:names){
            std::vector<BYTE> bytes;bool ready=sync_check_image(archive,name,bytes);
            if(visible.contains(name)){std::vector<BYTE> native;ready=sync_check_image(directory+L"\\"+sync_stem(id),name,native)&&ready;}
            if(!ready){if(maximumVersion)continue;throw std::runtime_error("Missing archived image");}
            // Committed originals survive image loss; asset membership is fresh complete payload proof.
            if(maximumVersion){assets.Append(JsonValue::CreateStringValue(name));continue;}
            std::wstring hex;hex.reserve(bytes.size()*2);for(BYTE b:bytes){hex+=L"0123456789abcdef"[b>>4];hex+=L"0123456789abcdef"[b&15];}
            JsonObject asset;asset.Insert(L"Name",JsonValue::CreateStringValue(name));asset.Insert(L"Bytes",JsonValue::CreateStringValue(hex));assets.Append(asset);
        }
        note.Insert(L"Changes",operations);note.Insert(L"Assets",assets);notes.Append(note);result.applied+=changes.size();
    }
    envelope.Insert(L"Notes",notes);auto package=std::wstring(envelope.Stringify());
    if(tcard_asset::encode(package).size()>tcard_asset::max_file)throw std::runtime_error("Exchange package budget exceeded");return package;
}
static bool sync_export(const std::wstring& storagePath,std::wstring& package,Result& result,bool readonly,unsigned* maximumVersion=nullptr)
{
    result={};package.clear();if(maximumVersion)*maximumVersion=0;std::lock_guard guard(sync_mutex);
    try{
        SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Storage busy");
        const auto directory=sync_directory(storagePath);
        std::vector<tcard::CardRecord> cards;
        if(readonly){if(!tcard::LoadCardSnapshot(storagePath,cards,false))throw std::runtime_error("Cannot read committed snapshot");}
        else tcard::LoadCards(storagePath,cards);
        sync_seed(storagePath,directory,cards,readonly);
        package=sync_capture(directory,readonly,maximumVersion,result);return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));package.clear();if(maximumVersion)*maximumVersion=0;return false;}
    catch(...){result.error=L"Export failed";package.clear();if(maximumVersion)*maximumVersion=0;return false;}
}

bool ExportSelection(const std::wstring& storagePath,const selection::Inventory& peer,std::wstring& package,Result& result,unsigned& maximumVersion,bool readonly,bool objects,const std::wstring& pinnedHeader,size_t reservedBytes)
{
    result={};package.clear();maximumVersion=0;std::lock_guard guard(sync_mutex);
    try{
        SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Storage busy");
        const auto directory=sync_directory(storagePath);
        {std::vector<tcard::CardRecord> cards;
            if(readonly){if(!tcard::LoadCardSnapshot(storagePath,cards,false))throw std::runtime_error("Cannot read committed snapshot");}
            else tcard::LoadCards(storagePath,cards);sync_seed(storagePath,directory,cards,readonly);}
        Result captured;const auto header=sync_capture(directory,readonly,&maximumVersion,captured);
        if(!pinnedHeader.empty()){
            if(!tcard_storage::Required(directory)||!tcard_storage::Ready(directory)||selection::Missing(selection::Read(pinnedHeader),selection::Read(header)))
                throw std::runtime_error("Pinned originals are no longer available");
        }
        selection::Read(header);const auto& target=pinnedHeader.empty()?header:pinnedHeader;const auto source=JsonObject::Parse(target).GetNamedArray(L"Notes");
        JsonObject envelope,delta;JsonArray selected;delta.Insert(L"Version",JsonValue::CreateNumberValue(1));delta.Insert(L"Notes",selected);
        // Advertise fresh committed membership even while a captured target controls body selection.
        envelope.Insert(L"Inventory",JsonObject::Parse(header));envelope.Insert(L"Delta",delta);
        // Reserve the existing replica wrapper, optional discovery and authenticated frame overhead.
        // Select a committable causal prefix, keeping each operation's missing image dependencies together.
        if(reservedBytes>=tcard_asset::max_file-65536)throw std::runtime_error("Selected metadata exceeds packet budget");
        const size_t budget=tcard_asset::max_file-65536-reservedBytes;
        size_t used=tcard_asset::encode(std::wstring(envelope.Stringify())).size();
        if(used>budget)throw std::runtime_error("Selected inventory exceeds packet budget");
        for(unsigned n=0;n<source.Size();++n){
            const auto note=source.GetObjectAt(n);const auto id=sync_string(note,L"Id");const auto ids=note.GetNamedArray(L"Changes"),names=note.GetNamedArray(L"Assets");
            std::vector<std::wstring> missing;std::vector<std::wstring> images;
            for(unsigned i=0;i<ids.Size();++i){const auto hash=std::wstring(ids.GetStringAt(i));if(!peer.operations.contains({id,hash}))missing.push_back(hash);}
            for(unsigned i=0;i<names.Size();++i){const auto name=std::wstring(names.GetStringAt(i));if(!peer.assets.contains({id,name}))images.push_back(name);}
            if(missing.empty()&&images.empty())continue;
            if(missing.empty()){if(!ids.Size())throw std::runtime_error("Asset inventory has no operation anchor");missing.push_back(std::wstring(ids.GetStringAt(0)));}
            JsonObject item;const auto changes=sync_load(directory,id,missing);size_t count=0,bytes=0;bool partial=false;
            const size_t comma=selected.Size()?1:0;
            if(used+comma>=budget||!sync_select_note(directory,id,changes,images,peer,budget-used-comma,item,count,bytes,partial)){
                if(!selected.Size()){
                    if(!objects)throw std::runtime_error("Selected dependency unit exceeds bounded packet budget");
                    if(!tcard_storage::Required(directory)||!tcard_storage::Ready(directory))throw std::runtime_error("Encrypted object export unavailable");
                    const auto chosen=std::min_element(changes.begin(),changes.end(),[](const auto& a,const auto& b){return std::tie(a.time,a.actor,a.sequence,a.id)<std::tie(b.time,b.actor,b.sequence,b.id);});
                    if(chosen==changes.end())throw std::runtime_error("Missing reference operation");
                    const bool repair=std::all_of(changes.begin(),changes.end(),[&](const auto& c){return peer.operations.contains({id,c.id});});
                    if(!repair)for(const auto& parent:chosen->parents)if(!peer.operations.contains({id,parent}))throw std::runtime_error("Missing reference parent order");
                    auto operation=sync_pack(*chosen,false);const auto archive=sync_archive(directory,id);
                    auto describe=[&](const std::wstring& name){std::vector<BYTE> raw;if(!tcard_asset::read(archive,name,raw))throw std::runtime_error("Missing reference object");return tcard_objects::Encode(tcard_objects::Describe(name,raw));};
                    for(const auto* side:{L"Before",L"After"}){auto state=operation.GetNamedObject(side);state.Insert(L"Object",describe(sync_string(state,L"SourceFile")));}
                    std::set<std::wstring> needed;
                    if(repair)needed.insert(images.begin(),images.end());else for(const auto* state:{&chosen->before,&chosen->after})for(const auto& name:sync_images(*state))if(!peer.assets.contains({id,name}))needed.insert(name);
                    JsonArray operations,assets;operations.Append(operation);
                    for(const auto& name:needed){JsonObject asset;asset.Insert(L"Name",JsonValue::CreateStringValue(name));asset.Insert(L"Object",describe(name));assets.Append(asset);}
                    JsonObject referenced;referenced.Insert(L"Id",JsonValue::CreateStringValue(id));referenced.Insert(L"Changes",operations);referenced.Insert(L"Assets",assets);selected.Append(referenced);
                    delta.Insert(L"ObjectVersion",JsonValue::CreateNumberValue(1));tcard_objects::References(std::wstring(delta.Stringify()));result.applied=1;
                }break;
            }
            used+=bytes+comma;selected.Append(item);result.applied+=count;
            if(partial)break;
        }
        package=envelope.Stringify();if(tcard_asset::encode(package).size()>budget)throw std::runtime_error("Selected packet budget exceeded");
        return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));package.clear();maximumVersion=0;return false;}
    catch(...){result.error=L"Selected export failed";package.clear();maximumVersion=0;return false;}
}
bool ExportChanges(const std::wstring& path,std::wstring& package,Result& result){return sync_export(path,package,result,false);}
bool ExportInventory(const std::wstring& path,std::wstring& header,Result& result,unsigned& maximumVersion){return sync_export(path,header,result,false,&maximumVersion);}
bool ExportSnapshot(const std::wstring& path,std::wstring& package,Result& result){return sync_export(path,package,result,true);}
bool ExportCommittedInventory(const std::wstring& path,std::wstring& header,Result& result,unsigned& maximumVersion){return sync_export(path,header,result,true,&maximumVersion);}

// Metadata capture and per-object reads are separate; this path never constructs an all-image map.
bool ExportDependencies(const std::wstring& storagePath,const selection::Inventory& peer,const tcard_dependencies::Context& context,std::wstring& package,Result& result,unsigned& maximumVersion)
{
    package.clear();result={};maximumVersion=0;std::lock_guard guard(sync_mutex);
    try{
        tcard_dependencies::CheckContext(context);SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Dependency source busy");
        const auto directory=sync_directory(storagePath);if(tcard_chunks::Group(directory)!=context.group)throw std::runtime_error("Dependency source group differs");
        const auto target=selection::Read(context.target);std::map<std::wstring,std::vector<std::wstring>> originals;
        for(const auto& value:target.operations)originals[value.first].push_back(value.second);
        for(const auto& [id,ids]:originals){
            std::vector<std::wstring> missingIds;std::set<std::wstring> missingImages;
            for(const auto& original:ids)if(!peer.operations.contains({id,original}))missingIds.push_back(original);
            for(const auto& image:target.assets)if(image.first==id&&!peer.assets.contains(image))missingImages.insert(image.second);
            // Completed notes and acknowledged originals do not reload their source histories.
            if(missingIds.empty()&&missingImages.empty())continue;
            auto changes=missingIds.empty()?std::vector<Change>{}:sync_load(directory,id,missingIds);
            std::set<std::wstring> coveredImages;for(const auto& change:changes)for(const auto* state:{&change.before,&change.after}){const auto names=sync_images(*state);coveredImages.insert(names.begin(),names.end());}
            const bool repairOlder=std::any_of(missingImages.begin(),missingImages.end(),[&](const auto& name){return !coveredImages.contains(name);});
            if(repairOlder)for(const auto& original:ids){if(!peer.operations.contains({id,original}))continue;
                auto one=sync_load(directory,id,{original});if(one.size()!=1)throw std::runtime_error("Captured repair original unavailable");bool needed=false;
                for(const auto* state:{&one.front().before,&one.front().after})for(const auto& name:sync_images(*state))needed|=missingImages.contains(name);
                if(needed){changes.push_back(std::move(one.front()));break;}
            }
            std::sort(changes.begin(),changes.end(),[](const auto& left,const auto& right){return std::tie(left.time,left.actor,left.sequence,left.id)<std::tie(right.time,right.actor,right.sequence,right.id);});
            for(const auto& change:changes){bool missing=!peer.operations.contains({id,change.id});std::set<std::wstring> images;
                for(const auto* side:{&change.before,&change.after}){const auto names=sync_images(*side);images.insert(names.begin(),names.end());}
                const bool repair=!missing&&std::any_of(images.begin(),images.end(),[&](const auto& name){return !peer.assets.contains({id,name});});
                if(!missing&&!repair)continue;if(missing&&std::any_of(change.parents.begin(),change.parents.end(),[&](const auto& parent){return !peer.operations.contains({id,parent});}))continue;
                const auto original=sync_pack(change,false);tcard_dependencies::Manifest manifest{context,id,change.id,tcard_dependencies::Digest(std::wstring(original.Stringify())),std::wstring(original.Stringify()),{}};
                std::set<std::wstring> descriptors;const auto archive=sync_archive(directory,id);
                auto add=[&](unsigned kind,const std::wstring& name){const auto key=std::to_wstring(kind)+L":"+name;if(!descriptors.insert(key).second)return;
                    tcard_crypto::Bytes bytes;if(!tcard_asset::read(archive,name,bytes))throw std::runtime_error("Captured dependency object unavailable");
                    manifest.descriptors.push_back({kind,tcard_objects::Describe(name,bytes)});};
                for(const auto* side:{L"Before",L"After"})add(1,sync_string(original.GetNamedObject(side),L"SourceFile"));for(const auto& name:images)add(2,name);
                package=tcard_dependencies::Encode(manifest);tcard_dependencies::Read(package,context);maximumVersion=static_cast<unsigned>(original.GetNamedNumber(L"Version"));result.applied=1;return true;
            }
        }
        throw std::runtime_error("No captured dependency original ready for peer");
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));package.clear();maximumVersion=0;return false;}
    catch(...){result.error=L"Dependency export failed";package.clear();maximumVersion=0;return false;}
}
static Change sync_dependency_change(const tcard_dependencies::Manifest& manifest,tcard_dependencies::Reader& reader)
{
    reader.Bind(manifest);auto original=JsonObject::Parse(manifest.original);std::set<std::wstring> sources;
    for(const auto* side:{L"Before",L"After"}){auto state=original.GetNamedObject(side);const auto name=sync_string(state,L"SourceFile");sources.insert(name);
        const auto& descriptor=tcard_dependencies::Find(manifest,1,name);const auto bytes=tcard_dependencies::ReadVerified(reader,descriptor);
        state.Insert(L"Source",JsonValue::CreateStringValue(tcard_asset::decode(bytes)));}
    const auto change=sync_unpack(original,L"",true);if(change.note!=manifest.note||change.id!=manifest.operation)throw std::runtime_error("Dependency logical original differs");
    std::set<std::wstring> images;for(const auto* state:{&change.before,&change.after}){const auto names=sync_images(*state);images.insert(names.begin(),names.end());}
    if(manifest.descriptors.size()!=sources.size()+images.size())throw std::runtime_error("Extra or missing dependency descriptor");
    for(const auto& descriptor:manifest.descriptors){const auto& names=descriptor.kind==1?sources:images;if(!names.contains(descriptor.object.name))throw std::runtime_error("Dependency descriptor differs from original");}
    for(const auto& name:images)tcard_dependencies::Find(manifest,2,name);return change;
}
static Change sync_verify_dependencies(const tcard_dependencies::Manifest& manifest,tcard_dependencies::Reader& reader)
{
    const auto change=sync_dependency_change(manifest,reader);
    // Complete proof precedes durable original intent; publication rereads each asset to reject TOCTOU.
    for(const auto& descriptor:manifest.descriptors)if(descriptor.kind==2){const auto bytes=tcard_dependencies::ReadVerified(reader,descriptor);}
    return change;
}
bool PrepareDependencyNote(const std::wstring& storagePath,const std::wstring& package,const tcard_dependencies::Context& context,tcard_dependencies::Reader& reader,PreparedNote& note,Result& result)
{
    note={};result={};std::lock_guard guard(sync_mutex);try{SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Dependency receiver busy");
        const auto directory=sync_directory(storagePath);if(tcard_chunks::Group(directory)!=context.group)throw std::runtime_error("Dependency receiver group differs");
        const auto manifest=tcard_dependencies::Read(package,context);sync_verify_dependencies(manifest,reader);note={manifest.note,package};return true;
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}catch(...){result.error=L"Dependency preparation failed";return false;}
}
static void sync_parse(const std::wstring& package,
    std::map<std::wstring,std::vector<Change>>& incoming,
    std::map<std::wstring,std::map<std::wstring,std::vector<BYTE>>>& images,
    std::vector<PreparedNote>* prepared=nullptr,const std::wstring* objectStorage=nullptr)
{
    if(tcard_asset::encode(package).size()>tcard_asset::max_file)throw std::runtime_error("Exchange package too large");
    const auto envelope=JsonObject::Parse(package);if(envelope.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Unsupported exchange version");
    const auto notes=envelope.GetNamedArray(L"Notes");if(notes.Size()>1024)throw std::runtime_error("Too many notes");
    if(objectStorage){
        if(envelope.GetNamedNumber(L"ObjectVersion",0)!=1||notes.Size()!=1||!tcard_storage::Required(*objectStorage)||!tcard_storage::Ready(*objectStorage))
            throw std::runtime_error("Encrypted single-note object references unavailable");
    }else if(envelope.HasKey(L"ObjectVersion"))throw std::runtime_error("Object references require explicit admission");
    if(envelope.HasKey(L"DependencyVersion"))throw std::runtime_error("Dependency references require explicit admission");
    size_t objectBudget=tcard_objects::maxObjects*tcard_asset::max_file;
    // Validate the complete exchange before any visible publication.
    for(unsigned n=0;n<notes.Size();++n){
        const auto note=notes.GetObjectAt(n);const auto id=sync_string(note,L"Id");
        if(!tcard_asset::leaf(sync_stem(id))||sync_stem(id)[0]==L'_'||incoming.contains(id))throw std::runtime_error("Invalid note ID");
        for(const auto& [other,unused]:incoming)if(sync_alias(id,other))throw std::runtime_error("Colliding note IDs");
        auto& changes=incoming[id];const auto ops=note.GetNamedArray(L"Changes");if(ops.Size()>10000)throw std::runtime_error("Too many changes");
        if(objectStorage&&ops.Size()!=1)throw std::runtime_error("Object note requires one original operation");
        for(unsigned i=0;i<ops.Size();++i){
            auto operation=ops.GetObjectAt(i);
            if(objectStorage){
                // Hydrate a private JSON copy only. Logical operation hashing remains unchanged.
                operation=JsonObject::Parse(std::wstring(operation.Stringify()));
                for(const auto* side:{L"Before",L"After"}){
                    auto state=operation.GetNamedObject(side);if(state.HasKey(L"Source"))throw std::runtime_error("Mixed inline and referenced source");
                    const auto name=sync_string(state,L"SourceFile");if(!tcard_asset::relative(name,false))throw std::runtime_error("Invalid source object reference");
                    const auto bytes=tcard_objects::Resolve(*objectStorage,state.GetNamedObject(L"Object"),name,objectBudget);
                    state.Insert(L"Source",JsonValue::CreateStringValue(tcard_asset::decode(bytes)));
                }
            }
            auto c=sync_unpack(operation,L"",true);if(c.note!=id)throw std::runtime_error("Cross-note operation");changes.push_back(std::move(c));
        }
        const auto assets=note.GetNamedArray(L"Assets");if(assets.Size()>1024)throw std::runtime_error("Too many assets");
        for(unsigned i=0;i<assets.Size();++i){
            const auto asset=assets.GetObjectAt(i);const auto name=sync_string(asset,L"Name");
            if(objectStorage){
                if(!tcard_asset::relative(name,true)||asset.HasKey(L"Bytes")||images[id].contains(name))throw std::runtime_error("Invalid referenced asset");
                images[id][name]=tcard_objects::Resolve(*objectStorage,asset.GetNamedObject(L"Object"),name,objectBudget);continue;
            }
            const auto hex=sync_string(asset,L"Bytes");
            if(!tcard_asset::relative(name,true)||hex.size()%2||hex.size()>tcard_asset::max_file*2)throw std::runtime_error("Invalid asset");
            std::vector<BYTE> bytes;bytes.reserve(hex.size()/2);
            const auto nibble=[](wchar_t c)->unsigned{if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;throw std::runtime_error("Invalid asset encoding");};
            for(size_t at=0;at<hex.size();at+=2)bytes.push_back(static_cast<BYTE>((nibble(hex[at])<<4)|nibble(hex[at+1])));
            if(tcard_asset::hash(bytes)!=name.substr(7,64))throw std::runtime_error("Invalid asset hash");images[id][name]=std::move(bytes);
        }
        if(prepared){
            JsonArray batch;batch.Append(note);JsonObject part;
            part.Insert(L"Version",JsonValue::CreateNumberValue(1));part.Insert(L"Notes",batch);
            if(objectStorage)part.Insert(L"ObjectVersion",JsonValue::CreateNumberValue(1));
            prepared->push_back({id,std::wstring(part.Stringify())});
        }
    }
}
bool PrepareChanges(const std::wstring& package,std::vector<PreparedNote>& notes,Result& result)
{
    notes.clear();result={};
    try{
        std::map<std::wstring,std::vector<Change>> incoming;
        std::map<std::wstring,std::map<std::wstring,std::vector<BYTE>>> images;
        std::vector<PreparedNote> validated;sync_parse(package,incoming,images,&validated);
        notes=std::move(validated);return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
    catch(...){result.error=L"Invalid incoming exchange";return false;}
}

bool PrepareObjectNote(const std::wstring& storagePath,const std::wstring& package,PreparedNote& note,Result& result,const std::wstring& objectRoot)
{
    note={};result={};try{
        std::map<std::wstring,std::vector<Change>> incoming;
        std::map<std::wstring,std::map<std::wstring,std::vector<BYTE>>> images;std::vector<PreparedNote> validated;
        std::wstring directory;{std::lock_guard guard(sync_mutex);SyncLock lock(tcard_native::Base(storagePath));
            if(!lock)throw std::runtime_error("Storage busy");directory=sync_directory(storagePath);}
        const auto objects=objectRoot.empty()?directory:objectRoot;
        if(tcard_chunks::Group(directory)!=tcard_chunks::Group(objects))throw std::runtime_error("Reference storage groups differ");
        tcard_objects::References(package);sync_parse(package,incoming,images,&validated,&objects);
        if(validated.size()!=1)throw std::runtime_error("Invalid prepared object note");note=std::move(validated.front());return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
    catch(...){result.error=L"Invalid object note";return false;}
}
static bool sync_import(const std::wstring& storagePath,const std::wstring& package,Result& result,const std::wstring* expected=nullptr,const std::wstring* objects=nullptr,const tcard_dependencies::Manifest* dependencies=nullptr,tcard_dependencies::Reader* reader=nullptr)
{
    result={};std::lock_guard guard(sync_mutex);
    try{
        std::map<std::wstring,std::vector<Change>> incoming;
        std::map<std::wstring,std::map<std::wstring,std::vector<BYTE>>> images;
        SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Storage busy");
        const auto directory=sync_directory(storagePath);
        if(objects){if(tcard_chunks::Group(directory)!=tcard_chunks::Group(*objects))throw std::runtime_error("Reference storage groups differ");tcard_objects::References(package);}
        if(dependencies){if(!reader||tcard_chunks::Group(directory)!=dependencies->context.group)throw std::runtime_error("Dependency import authority differs");
            incoming[dependencies->note].push_back(sync_verify_dependencies(*dependencies,*reader));
        }else sync_parse(package,incoming,images,nullptr,objects);
        tcard_asset::File root(tcard_asset::open_dir(directory,true));if(!root)throw std::runtime_error("Invalid storage root");
        std::vector<std::wstring> selected;
        for(const auto& [id,unused]:incoming)selected.push_back(id);
        bool scoped=expected!=nullptr;
        if(scoped&&(incoming.size()!=1||selected.front()!=*expected))throw std::runtime_error("Prepared note identity differs");
        // Historical flat copies do not disable the targeted path after a current bundle has committed.
        if(scoped)for(const auto& id:selected){
            if(sync_exists(directory+L"\\"+sync_stem(id),L"card.json"))continue;
            for(const auto* suffix:{L".json",L".txt"})
                if(GetFileAttributesW((directory+L"\\"+sync_stem(id)+suffix).c_str())!=INVALID_FILE_ATTRIBUTES)scoped=false;
        }
        std::vector<tcard::CardRecord> cards;
        if(scoped){if(!tcard::LoadCardSubset(storagePath,selected,cards))throw std::runtime_error("Cannot read current note");}
        else tcard::LoadCards(storagePath,cards);
        auto manifests=sync_manifests(directory,false,scoped?&selected:nullptr);
        for(const auto& [id,unused]:incoming)for(const auto& [other,manifest]:manifests)
            if(id!=other&&sync_alias(id,other))throw std::runtime_error("Existing note ID collision");
        // Seal unsent legacy content before combining it with a received history.
        sync_seed(storagePath,directory,cards);
        manifests=sync_manifests(directory,false,scoped?&selected:nullptr);
        bool complete=true;
        for(auto& [id,additions]:incoming){
            if(additions.empty())continue;
            try{
            if(!sync_prepare(directory,id))throw std::runtime_error("Cannot prepare journal");
            const auto archive=sync_archive(directory,id);
            std::map<std::wstring,Change> unionSet;
            std::vector<std::wstring> committed;
            if(manifests.contains(id)){committed=sync_ids(manifests.at(id));for(auto& c:sync_load(directory,id,committed))unionSet.emplace(c.id,std::move(c));}
            JsonObject pending;
            if(sync_exists(archive,L"pending.json")){
                if(!sync_read(archive,L"pending.json",pending))throw std::runtime_error("Unreadable pending journal");
                for(auto& c:sync_load(directory,id,sync_ids(pending)))unionSet.emplace(c.id,std::move(c));
            }
            for(const auto& c:additions){if(!sync_store(directory,c))throw std::runtime_error("Cannot stage operation");unionSet.emplace(c.id,c);}
            std::vector<Change> changes;for(const auto& [key,c]:unionSet)changes.push_back(c);
            std::vector<std::wstring> ids;for(const auto& c:changes)ids.push_back(c.id);
            // Retain original IDs before assets/materialization: a later failure can resume without the sender.
            if(ids!=committed){JsonObject waiting;sync_index(waiting,changes);
                if(!sync_write(archive,L"pending.json",waiting))throw std::runtime_error("Cannot retain received intent");}
            for(const auto& [name,bytes]:images[id])if(!tcard_asset::publish(archive,name,bytes,true))throw std::runtime_error("Cannot stage image");
            if(dependencies)for(const auto& descriptor:dependencies->descriptors)if(descriptor.kind==2){
                const auto bytes=tcard_dependencies::ReadVerified(*reader,descriptor);
                if(!tcard_asset::publish(archive,descriptor.object.name,bytes,true))throw std::runtime_error("Cannot stage verified dependency image");
            }
            if(ids==committed){
                // Repair current image files without replaying history over later edits or rewriting metadata.
                const auto visible=sync_deleted(directory,sync_stem(id))?std::set<std::wstring>{}:sync_images(sync_read_state(manifests.at(id),archive));
                const auto bundle=directory+L"\\"+sync_stem(id);bool touched=false;
                auto restore=[&](const std::wstring& name){if(!visible.contains(name))return;std::vector<BYTE> bytes;
                    auto relative=name;std::replace(relative.begin(),relative.end(),L'/',L'\\');const bool missing=!sync_exists(bundle,relative);
                    if(!tcard_asset::read(archive,name,bytes)||!tcard_asset::publish(bundle,name,bytes,true))throw std::runtime_error("Cannot repair native image");touched|=missing;};
                for(const auto& [name,bytes]:images[id])restore(name);
                if(dependencies)for(const auto& descriptor:dependencies->descriptors)if(descriptor.kind==2)restore(descriptor.object.name);
                if(touched)result.touched.push_back(id);
                if(pending.HasKey(L"SyncVersion")&&!sync_ids(pending).empty()){
                    JsonObject done;sync_index(done,{});if(!sync_write(archive,L"pending.json",done))throw std::runtime_error("Cannot complete committed receipt");}
                result.applied+=additions.size();continue;
            }
            Snapshot state;std::wstring error;
            if(!sync_merge(changes,state,error)){
                result.pending+=changes.size();result.error=error;continue;
            }
            bool assetsReady=true;
            for(const auto& c:changes)for(const auto* value:{&c.before,&c.after})for(const auto& name:sync_images(*value)){
                std::vector<BYTE> bytes;if(!tcard_asset::read(archive,name,bytes)||tcard_asset::hash(bytes)!=name.substr(7,64))assetsReady=false;
            }
            if(!assetsReady){result.pending+=changes.size();result.error=L"Missing image dependency";continue;}
            const auto old=std::find_if(cards.begin(),cards.end(),[&](const auto& card){return card.id==id;});
            tcard::CardRecord card;if(old!=cards.end())card=*old;
            else if(!state.deleted&&!tcard::RetainCardHistory(storagePath,id,card))throw std::runtime_error("Cannot retain local restored history");
            card.id=id;card.source=state.source;card.title=state.title;card.markdown=state.format==1;card.richHtml=state.format==2;
            if(state.display)sync_display(card,*state.display);
            if(state.dates){card.createdUtc=sync_make_time(state.dates->created);card.updatedUtc=sync_make_time(state.dates->updated);}
            std::map<std::wstring,std::vector<Change>> overrideMap{{id,changes}};
            struct ImportScope{const std::map<std::wstring,std::vector<Change>>* previous;explicit ImportScope(const std::map<std::wstring,std::vector<Change>>& value):previous(sync_importing){sync_importing=&value;}~ImportScope(){sync_importing=previous;}} scope(overrideMap);
            result.touched.push_back(id);
            if(state.deleted){if(!tcard::DeleteCard(storagePath,id))throw std::runtime_error("Cannot commit deletion");}
            else{
                const auto bundle=directory+L"\\"+sync_stem(id);tcard_asset::File folder(tcard_asset::open_dir(bundle,true));if(!folder)throw std::runtime_error("Invalid bundle");
                for(const auto& name:sync_images(state)){std::vector<BYTE> bytes;if(!tcard_asset::read(archive,name,bytes)||!tcard_asset::publish(bundle,name,bytes,true))throw std::runtime_error("Cannot publish image");}
                std::vector<tcard::CardRecord> batch{card};if(!tcard::SaveCards(storagePath,batch))throw std::runtime_error("Cannot commit received state");
            }
            JsonObject done;sync_index(done,{});if(!sync_write(archive,L"pending.json",done))throw std::runtime_error("Cannot complete pending index");
            result.applied+=additions.size();
            }catch(const std::exception& e){complete=false;result.partial=true;result.pending+=additions.size();result.error.assign(e.what(),e.what()+strlen(e.what()));}
            catch(...){complete=false;result.partial=true;result.pending+=additions.size();result.error=L"Note commit failed";}
        }
        return complete;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
    catch(...){result.error=L"Import failed";return false;}
}

bool ImportChanges(const std::wstring& storagePath,const std::wstring& package,Result& result)
{
    return sync_import(storagePath,package,result);
}
bool ImportPreparedNote(const std::wstring& storagePath,const PreparedNote& note,Result& result)
{
    return sync_import(storagePath,note.package,result,&note.id);
}

bool ImportObjectNote(const std::wstring& storagePath,const PreparedNote& note,Result& result,const std::wstring& objectRoot)
{
    std::lock_guard guard(sync_mutex);try{SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Storage busy");
        const auto objects=objectRoot.empty()?sync_directory(storagePath):objectRoot;
        return sync_import(storagePath,note.package,result,&note.id,&objects);
    }catch(...){result={};result.error=L"Object note storage unavailable";return false;}
}
bool ImportDependencyNote(const std::wstring& storagePath,const PreparedNote& note,const tcard_dependencies::Context& context,tcard_dependencies::Reader& reader,Result& result)
{
    result={};try{const auto manifest=tcard_dependencies::Read(note.package,context);
        if(note.id!=manifest.note)throw std::runtime_error("Prepared dependency identity differs");reader.Bind(manifest);
        return sync_import(storagePath,note.package,result,&note.id,nullptr,&manifest,&reader);
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}catch(...){result.error=L"Dependency import failed";return false;}
}
// Only the internal committed-copy wrapper may invoke duplicate custody cleanup while all owners remain locked.
struct sync_dependency_locks {
    std::unique_lock<std::recursive_mutex> process{sync_mutex};
    std::vector<std::unique_ptr<SyncLock>> owners;
    sync_dependency_locks(const std::wstring& storage,const std::wstring& custody=L"")
    {
        const auto base=tcard_native::Base(storage);std::set<std::wstring> paths{sync_owner(base),tcard_chunks::budget::Owner(base)};
        if(!custody.empty())paths.insert(tcard_chunks::budget::Owner(custody));
        for(const auto& path:paths){if(path.empty())throw std::runtime_error("Dependency owner unavailable");
            auto lock=std::make_unique<SyncLock>(path);if(!*lock)throw std::runtime_error("Dependency owner busy");owners.push_back(std::move(lock));}
    }
};
static tcard_dependencies::Manifest sync_dependency_commit(const std::wstring& storage,const PreparedNote& note,const tcard_dependencies::Context& context)
{
    const auto manifest=tcard_dependencies::Read(note.package,context);if(note.id!=manifest.note)throw std::runtime_error("Prepared dependency identity differs");
    const auto directory=tcard_native::Extended(sync_directory(storage));if(tcard_chunks::Group(directory)!=context.group)throw std::runtime_error("Committed dependency key differs");
    const auto stem=sync_stem(manifest.note);JsonObject committed;
    if(sync_deleted(directory,stem)){
        if(!sync_read(directory,stem+L".deleted",committed))throw std::runtime_error("Committed dependency deletion unavailable");
    }else if(!sync_read(directory+L"\\"+stem,L"card.json",committed))throw std::runtime_error("Committed dependency note unavailable");
    if(sync_string(committed,L"Id")!=manifest.note)throw std::runtime_error("Committed dependency note identity differs");
    const auto ids=sync_ids(committed);if(std::find(ids.begin(),ids.end(),manifest.operation)==ids.end())throw std::runtime_error("Dependency original is not committed");
    JsonObject original;if(!sync_read(sync_archive(directory,manifest.note),manifest.operation+L".json",original)||
        tcard_dependencies::Digest(std::wstring(original.Stringify()))!=manifest.originalHash)throw std::runtime_error("Committed dependency original differs");
    // The source archive, never externally supplied Pool/Result/Required, proves every immutable original/source/asset byte.
    tcard_dependencies::SourceReader reader(directory,note.package,context);sync_verify_dependencies(manifest,reader);return manifest;
}
bool DependencyCommitProof::Matches(const std::wstring& storage,const tcard_dependencies::Manifest& manifest)const noexcept
{
    if(!valid_||!active_)return false;
    try{return owner_==sync_owner(tcard_native::Base(storage))&&vault_==tcard_chunks::budget::Owner(tcard_native::Base(storage))&&
        directory_==sync_path_plain(sync_directory(storage))&&group_==manifest.context.group&&tcard_chunks::Group(tcard_native::Extended(directory_))==group_&&
        manifest_==tcard_dependencies::Digest(tcard_dependencies::Encode(manifest));}catch(...){return false;}
}
bool VerifyDependencyCommit(const std::wstring& storage,const PreparedNote& note,const tcard_dependencies::Context& context,DependencyCommitProof& proof,Result& result)
{
    result={};proof.Clear();try{sync_dependency_locks locks(storage);const auto manifest=sync_dependency_commit(storage,note,context);
        proof.owner_=sync_owner(tcard_native::Base(storage));proof.vault_=tcard_chunks::budget::Owner(tcard_native::Base(storage));
        proof.directory_=sync_path_plain(sync_directory(storage));proof.group_=context.group;proof.manifest_=tcard_dependencies::Digest(tcard_dependencies::Encode(manifest));proof.valid_=true;
        // active remains false: a stored standalone proof never authorizes later cleanup.
        return true;
    }catch(const std::exception& error){proof.Clear();result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}catch(...){proof.Clear();result.error=L"Committed dependency verification failed";return false;}
}
bool WithDependencyCommit(const std::wstring& storage,const std::wstring& custodyRoot,const PreparedNote& note,const tcard_dependencies::Context& context,const std::function<bool(const DependencyCommitProof&)>& cleanup,Result& result)
{
    result={};try{if(custodyRoot.empty()||!cleanup)throw std::runtime_error("Dependency cleanup boundary unavailable");
        sync_dependency_locks locks(storage,custodyRoot);const auto manifest=sync_dependency_commit(storage,note,context);
        if(tcard_chunks::Group(custodyRoot)!=context.group)throw std::runtime_error("Dependency custody key differs");
        DependencyCommitProof proof;proof.owner_=sync_owner(tcard_native::Base(storage));proof.vault_=tcard_chunks::budget::Owner(tcard_native::Base(storage));
        proof.directory_=sync_path_plain(sync_directory(storage));proof.group_=context.group;proof.manifest_=tcard_dependencies::Digest(tcard_dependencies::Encode(manifest));proof.valid_=proof.active_=true;
        struct End{bool& active;~End(){active=false;}} end{proof.active_};
        if(!cleanup(proof)){result.error=L"Committed duplicate cleanup remains incomplete";return false;}return true;
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}catch(...){result.error=L"Committed duplicate cleanup failed";return false;}
}
bool WithSignalProtection(const std::wstring& nativeRoot,const std::wstring& profileRoot,const std::function<bool()>& protect,Result& result)
{
    result={};try{
        if(nativeRoot.empty()||profileRoot.empty()||!protect)throw std::runtime_error("Signal protection boundary unavailable");
        std::unique_lock<std::recursive_mutex> process(sync_mutex);
        const auto native=sync_path_plain(tcard_native::Base(nativeRoot)),profile=sync_path_plain(profileRoot),control=profile+L"-control";
        if(native.empty()||profile.empty())throw std::runtime_error("Signal protection roots unavailable");
        const auto paths=[&]{return std::set<std::wstring>{sync_owner(native),tcard_chunks::budget::Owner(native),
            sync_owner(profile),tcard_chunks::budget::Owner(profile),sync_owner(control)};};
        const auto expected=paths();std::vector<std::unique_ptr<SyncLock>> owners;
        for(const auto& path:expected){if(path.empty())throw std::runtime_error("Signal protection owner unavailable");
            auto lock=std::make_unique<SyncLock>(path);if(!*lock)throw std::runtime_error("Signal protection owner busy");owners.push_back(std::move(lock));}
        // Noncreate handles refuse reparse/aliases and prevent stable roots being renamed during the callback.
        std::set<std::wstring> directories=expected;directories.insert(native);directories.insert(profile);
        const auto flags=GetFileAttributesW(tcard_native::Extended(control).c_str());
        if(flags==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)throw std::runtime_error("Signal control path unavailable");
            directories.erase(sync_owner(control));const auto slash=control.find_last_of(L'\\');
            if(slash==std::wstring::npos)throw std::runtime_error("Signal control parent unavailable");directories.insert(control.substr(0,slash));}
        std::vector<std::unique_ptr<tcard_asset::File>> roots;
        for(const auto& path:directories){auto root=std::make_unique<tcard_asset::File>(tcard_asset::open_dir(tcard_native::Extended(path),false));
            if(!*root)throw std::runtime_error("Signal stable root unavailable");roots.push_back(std::move(root));}
        if(paths()!=expected)throw std::runtime_error("Signal protection owner changed");
        if(!protect()){result.error=L"Signal protection remains incomplete";return false;}return true;
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}
    catch(...){result.error=L"Signal protection boundary failed";return false;}
}
bool RetryChanges(const std::wstring& storagePath,Result& result)
{
    result={};std::lock_guard guard(sync_mutex);
    try{
        SyncLock lock(tcard_native::Base(storagePath));if(!lock)throw std::runtime_error("Storage busy");
        const auto directory=sync_directory(storagePath),root=directory+L"\\_sync";
        WIN32_FIND_DATAW entry{};const auto handle=FindFirstFileW((root+L"\\*").c_str(),&entry);
        if(handle==INVALID_HANDLE_VALUE){const auto error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return true;throw std::runtime_error("Cannot inspect retained receipts");}
        struct Scope{HANDLE handle;~Scope(){FindClose(handle);}} scope{handle};
        size_t notes=0;bool okay=true;
        do{
            const std::wstring stem=entry.cFileName;
            if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||stem==L"."||stem==L"..")continue;
            const auto archive=root+L"\\"+stem;
            const auto attributes=GetFileAttributesW((archive+L"\\pending.json").c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();
                if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)continue;throw std::runtime_error("Cannot inspect pending index");}
            if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Invalid pending directory");
            size_t retained=0;
            try{
                JsonObject pending;if(!sync_read(archive,L"pending.json",pending))throw std::runtime_error("Unreadable pending index");
                const auto ids=sync_ids(pending);if(ids.empty())continue;retained=ids.size();
                if(++notes>1024||ids.size()>10000)throw std::runtime_error("Retained receipt budget exceeded");
                JsonObject first;if(!sync_read(archive,ids.front()+L".json",first))throw std::runtime_error("Missing retained operation");
                const auto id=sync_string(first,L"Note");if(sync_stem(id)!=stem)throw std::runtime_error("Pending note identity differs");
                const auto changes=sync_load(directory,id,ids);JsonArray operations,assets;std::set<std::wstring> names;
                size_t budget=tcard_asset::max_file-4096;
                for(const auto& c:changes){
                    auto object=sync_pack(c,true);const auto bytes=tcard_asset::encode(std::wstring(object.Stringify())).size();
                    if(bytes>budget)throw std::runtime_error("Retained exchange budget exceeded");budget-=bytes;operations.Append(object);
                    for(const auto* state:{&c.before,&c.after}){const auto images=sync_images(*state);names.insert(images.begin(),images.end());}
                }
                for(const auto& name:names){
                    std::vector<BYTE> bytes;
                    // Missing/corrupt assets stay pending; never fabricate or overwrite their evidence.
                    if(!tcard_asset::read(archive,name,bytes)||tcard_asset::hash(bytes)!=name.substr(7,64))continue;
                    if(bytes.size()>budget/2||budget-bytes.size()*2<256)throw std::runtime_error("Retained asset budget exceeded");
                    std::wstring hex;hex.reserve(bytes.size()*2);for(BYTE b:bytes){hex+=L"0123456789abcdef"[b>>4];hex+=L"0123456789abcdef"[b&15];}
                    JsonObject asset;asset.Insert(L"Name",JsonValue::CreateStringValue(name));asset.Insert(L"Bytes",JsonValue::CreateStringValue(hex));
                    budget-=bytes.size()*2+256;assets.Append(asset);
                }
                JsonObject note;note.Insert(L"Id",JsonValue::CreateStringValue(id));note.Insert(L"Changes",operations);note.Insert(L"Assets",assets);
                JsonArray batch;batch.Append(note);JsonObject envelope;envelope.Insert(L"Version",JsonValue::CreateNumberValue(1));envelope.Insert(L"Notes",batch);
                // Current native bundles need only their own read/merge; absent targets retain legacy preparation.
                const bool current=sync_exists(directory+L"\\"+sync_stem(id),L"card.json");
                const PreparedNote prepared{id,std::wstring(envelope.Stringify())};
                Result step;const bool imported=current?ImportPreparedNote(storagePath,prepared,step):
                    ImportChanges(storagePath,prepared.package,step);
                result.applied+=step.applied;result.pending+=step.pending;
                result.touched.insert(result.touched.end(),step.touched.begin(),step.touched.end());
                if(!imported){okay=false;if(!step.pending)result.pending+=retained;result.error=step.error;}
                else if(!step.error.empty())result.error=step.error;
            }catch(const std::exception& e){okay=false;result.pending+=retained;result.error.assign(e.what(),e.what()+strlen(e.what()));}
            catch(...){okay=false;result.pending+=retained;result.error=L"Retained receipt failed";}
        }while(FindNextFileW(handle,&entry));
        if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Pending enumeration failed");return okay;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}
    catch(...){result.error=L"Retry failed";return false;}
}
}
