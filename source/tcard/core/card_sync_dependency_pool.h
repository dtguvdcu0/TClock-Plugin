#pragma once
#include "card_sync_dependencies.h"
#include <map>
#include <memory>
#include <mutex>

// Retained dependency custody is bound to independent authenticated context, never an owner ACK.
namespace tcard_dependencies {
struct Recovery {tcard_bridge::Stamp stamp;bool known=false,done=false;size_t next=0;std::wstring last;std::vector<std::wstring> bindings;std::map<std::wstring,std::pair<ULONGLONG,unsigned>> retries;};
class Pool final:public Reader {
    std::wstring storage_,root_,binding_,identity_;
    Manifest manifest_;
    size_t metadata_=0;bool cleanupOnly_=false,recordTarget_=true;
    mutable tcard_bridge::Stamp catalogStamp_;mutable bool catalogKnown_=false;
    std::map<std::wstring,size_t> descriptors_,slots_;
    mutable std::recursive_mutex mutex_;
    static std::wstring Record(const Manifest& manifest,bool target)
    {
        auto value=JsonObject::Parse(Encode(manifest));if(target)value.Insert(L"CapturedTarget",JsonObject::Parse(manifest.context.target));
        return std::wstring(value.Stringify());
    }
    static size_t Metadata(const std::wstring& record){return 2*(tcard_asset::encode(record).size()+tcard_crypto::envelopeOverhead)+8192;}
    const Descriptor& Bound(const Descriptor& descriptor)const
    {
        const auto found=descriptors_.find(Key(descriptor));
        if(found==descriptors_.end())throw std::runtime_error("Unbound dependency custody descriptor");
        const auto& value=manifest_.descriptors[found->second];
        if(std::wstring(EncodeDescriptor(value).Stringify())!=std::wstring(EncodeDescriptor(descriptor).Stringify()))
            throw std::runtime_error("Dependency custody descriptor differs");
        return value;
    }
    void Observe()const
    {
        if(cleanupOnly_)throw std::runtime_error("Retiring dependency custody is not data authority");
        const tcard_chunks::budget::Catalog location{tcard_chunks::budget::Owner(storage_),manifest_.context.group,{}};
        tcard_bridge::Stamp stamp;if(!tcard_bridge::Fingerprint(tcard_chunks::budget::Folder(location),L"budget.json",stamp))throw std::runtime_error("Dependency quota catalog unavailable");
        if(catalogKnown_&&stamp==catalogStamp_)return;
        const auto catalog=tcard_chunks::budget::Load(storage_);CheckCatalog(catalog);const auto relative=tcard_chunks::budget::Relative(catalog.owner,root_);
        for(const auto& entry:catalog.entries)if((entry.path==relative||entry.path.starts_with(relative+L"\\"))&&entry.retiring)
            throw std::runtime_error("Retiring dependency custody is not data authority");
        catalogStamp_=stamp;catalogKnown_=true;
    }
    void Check()const
    {
        Observe();
        if(tcard_chunks::Group(storage_)!=manifest_.context.group)throw std::runtime_error("Dependency custody key differs");
        tcard_asset::File directory(tcard_asset::open_dir(root_,false));if(!directory)throw std::runtime_error("Dependency custody directory unavailable");
        std::wstring text;if(!tcard_bridge::Read(root_,L"binding.json",text)||text!=binding_)throw std::runtime_error("Dependency custody binding unavailable");
    }
    std::wstring Path(const Descriptor& descriptor,bool sealed)const
    {
        return root_+(sealed?L"\\_sealed\\":L"\\_objects\\")+tcard_objects::Slot(descriptor.object);
    }
    std::wstring ObjectRecord(const Descriptor& descriptor)const
    {
        auto value=tcard_objects::Encode(descriptor.object);value.Insert(L"Group",JsonValue::CreateStringValue(manifest_.context.group));return std::wstring(value.Stringify());
    }
    size_t Charge(const Descriptor& descriptor)const
    {
        return tcard_chunks::budget::Charge(descriptor.object.size,descriptor.object.hashes.size(),tcard_asset::encode(ObjectRecord(descriptor)).size());
    }
    void CheckCatalog(const tcard_chunks::budget::Catalog& catalog)const
    {
        const auto relative=tcard_chunks::budget::Relative(catalog.owner,root_);bool metadata=false;std::set<size_t> objects;
        for(const auto& entry:catalog.entries){
            if(entry.path==relative){
                if(entry.identity!=identity_||entry.bytes!=metadata_||entry.provisional||!entry.destination.empty())throw std::runtime_error("Dependency metadata reservation differs");
                if(entry.retiring&&(entry.retireKind!=2||entry.partCount))throw std::runtime_error("Dependency binding retirement differs");
                metadata=true;continue;
            }
            if(!entry.path.starts_with(relative+L"\\"))continue;
            const auto tail=entry.path.substr(relative.size()+1);const bool sealed=tail.starts_with(L"_sealed\\"),provisional=tail.starts_with(L"_objects\\");
            if(!sealed&&!provisional)throw std::runtime_error("Unknown dependency reservation retained");
            const auto slot=tail.substr(sealed?8:9);const auto found=slots_.find(slot);
            if(found==slots_.end()||!objects.insert(found->second).second)throw std::runtime_error("Dependency reservation outside manifest or duplicated");
            const auto& descriptor=manifest_.descriptors[found->second];
            if(entry.identity!=Digest(ObjectRecord(descriptor))||entry.bytes!=Charge(descriptor)||entry.provisional!=provisional||
                (!entry.destination.empty()&&(!provisional||entry.destination!=tcard_chunks::budget::Relative(catalog.owner,Path(descriptor,true)))))
                throw std::runtime_error("Dependency object reservation differs");
            if(entry.retiring&&(entry.retireKind!=1||entry.partCount!=descriptor.object.hashes.size()))throw std::runtime_error("Dependency object retirement differs");
        }
        if(!metadata)throw std::runtime_error("Dependency metadata reservation unavailable");
    }
    static std::vector<std::pair<std::wstring,DWORD>> Entries(const std::wstring& path)
    {
        std::vector<std::pair<std::wstring,DWORD>> entries;if(!tcard_chunks::budget::Exists(path))return entries;
        tcard_asset::File directory(tcard_asset::open_dir(path,false));if(!directory)throw std::runtime_error("Unsafe dependency retirement directory");
        WIN32_FIND_DATAW item{};const auto find=FindFirstFileW((path+L"\\*").c_str(),&item);
        if(find==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return entries;throw std::runtime_error("Dependency retirement enumeration failed");}
        struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};
        do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
            if(entries.size()>=2048||item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Unknown dependency retirement entry");
            entries.emplace_back(name,item.dwFileAttributes);
        }while(FindNextFileW(find,&item));if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Dependency retirement enumeration interrupted");return entries;
    }
    static void Delete(const std::wstring& path)
    {
        tcard_asset::File file(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return;throw std::runtime_error("Dependency retirement file unavailable");}
        BY_HANDLE_FILE_INFORMATION info{};if(!tcard_asset::regular(file.value,false)||!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1)
            throw std::runtime_error("Unsafe dependency retirement file");
        FILE_DISPOSITION_INFO disposition{TRUE};if(!SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition)))throw std::runtime_error("Dependency retirement deletion failed");
    }
    void CheckTree(const tcard_chunks::budget::Catalog& catalog)const
    {
        const auto relative=tcard_chunks::budget::Relative(catalog.owner,root_);
        for(const auto& [name,attributes]:Entries(root_)){
            if(name==L"binding.json"){if(attributes&FILE_ATTRIBUTE_DIRECTORY)throw std::runtime_error("Unsafe dependency binding");continue;}
            if((name!=L"_objects"&&name!=L"_sealed")||!(attributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Unknown dependency custody retained");
            for(const auto& [slot,slotAttributes]:Entries(root_+L"\\"+name)){
                const auto found=slots_.find(slot);if(found==slots_.end()||!(slotAttributes&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Unknown dependency object retained");
                const auto path=relative+L"\\"+name+L"\\"+slot;
                const auto entry=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& value){return value.path==path;});
                if(entry==catalog.entries.end()||!entry->destination.empty())throw std::runtime_error("Unreserved dependency object retained");
                const auto& descriptor=manifest_.descriptors[found->second];
                for(const auto& [leaf,fileAttributes]:Entries(root_+L"\\"+name+L"\\"+slot)){
                    if(fileAttributes&FILE_ATTRIBUTE_DIRECTORY)throw std::runtime_error("Nested dependency object retained");
                    bool known=leaf==L"manifest.json";for(size_t i=0;!known&&i<descriptor.object.hashes.size();++i)known=leaf==std::to_wstring(i)+L".part";
                    if(!known)throw std::runtime_error("Unknown dependency fragment retained");
                }
                if(!entry->retiring){std::wstring record;if(!tcard_bridge::Read(root_+L"\\"+name+L"\\"+slot,L"manifest.json",record)||record!=ObjectRecord(descriptor))throw std::runtime_error("Dependency retirement object binding differs");}
            }
        }
    }
    std::unique_ptr<tcard_objects::Cache> Open(const Descriptor& descriptor,bool create,bool sealedOnly=false)const
    {
        const bool provisional=tcard_chunks::budget::Exists(Path(descriptor,false)),sealed=tcard_chunks::budget::Exists(Path(descriptor,true));
        if(provisional&&sealed)throw std::runtime_error("Duplicate dependency custody retained");
        if(sealed)return std::make_unique<tcard_objects::Cache>(root_,descriptor.object,manifest_.context.group,false,L"_sealed");
        if(sealedOnly)throw std::runtime_error("Dependency object is not sealed");
        if(provisional)return std::make_unique<tcard_objects::Cache>(root_,descriptor.object,manifest_.context.group,false);
        if(!create)throw std::runtime_error("Dependency object unavailable");
        return std::make_unique<tcard_objects::Cache>(root_,descriptor.object,manifest_.context.group,true);
    }
public:
    Pool(const std::wstring& storage,const std::wstring& package,const Context& expected,bool create=true):manifest_(tcard_dependencies::Read(package,expected))
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage));if(!owner)throw std::runtime_error("Dependency custody busy");
        storage_=tcard_chunks::budget::Extended(sync_path_plain(tcard_native::Resolve(storage)));
        if(tcard_chunks::Group(storage_)!=expected.group)throw std::runtime_error("Dependency authenticated group differs");
        identity_=Digest(Encode(manifest_));binding_=Record(manifest_,true);root_=storage_+L"\\_dependency-custody\\"+identity_;
        // Reserve actual binding bytes and an atomic-publication copy, separate from ledger metadata.
        metadata_=Metadata(binding_);
        for(size_t i=0;i<manifest_.descriptors.size();++i){const auto& descriptor=manifest_.descriptors[i];
            if(!descriptors_.emplace(Key(descriptor),i).second||!slots_.emplace(tcard_objects::Slot(descriptor.object),i).second)
                throw std::runtime_error("Duplicate dependency custody identity");}
        const auto retained=tcard_chunks::budget::Load(storage_);const auto relative=tcard_chunks::budget::Relative(retained.owner,root_);
        const auto binding=std::find_if(retained.entries.begin(),retained.entries.end(),[&](const auto& entry){return entry.path==relative;});
        const bool exists=tcard_chunks::budget::Exists(root_);
        if(tcard_chunks::budget::Exists(root_+L"\\binding.json")){std::wstring record;if(!tcard_bridge::Read(root_,L"binding.json",record))throw std::runtime_error("Dependency binding unavailable");
            recordTarget_=JsonObject::Parse(record).HasKey(L"CapturedTarget");binding_=Record(manifest_,recordTarget_);metadata_=Metadata(binding_);
            if(record!=binding_)throw std::runtime_error("Dependency custody stored context differs");
        }else if(binding!=retained.entries.end()&&binding->retiring&&binding->bytes!=metadata_){
            // Retained pre-extension records keep their exact old charge and supplied independent context.
            const auto legacy=Record(manifest_,false);if(binding->bytes!=Metadata(legacy))throw std::runtime_error("Dependency retired metadata charge differs");
            recordTarget_=false;binding_=legacy;metadata_=Metadata(binding_);
        }
        if(binding==retained.entries.end()&&exists)throw std::runtime_error("Unreserved dependency custody retained");
        bool retiring=false;for(const auto& entry:retained.entries)if((entry.path==relative||entry.path.starts_with(relative+L"\\"))&&entry.retiring)retiring=true;
        if(!create||retiring){
            cleanupOnly_=true;
            if(binding==retained.entries.end()){
                if(std::any_of(retained.entries.begin(),retained.entries.end(),[&](const auto& entry){return entry.path.starts_with(relative+L"\\");}))throw std::runtime_error("Dependency binding reservation lost");return;
            }
            CheckCatalog(retained);
            if(!exists&&!binding->retiring)throw std::runtime_error("Dependency custody lost before retirement");
            if(tcard_chunks::budget::Exists(root_+L"\\binding.json")){std::wstring text;if(!tcard_bridge::Read(root_,L"binding.json",text)||text!=binding_)throw std::runtime_error("Dependency cleanup binding differs");}
            else if(!binding->retiring)throw std::runtime_error("Dependency cleanup binding unavailable");
            return;
        }
        if(tcard_asset::encode(binding_).size()>tcard_asset::max_file-65536)throw std::runtime_error("Dependency custody metadata exceeds file budget");
        tcard_chunks::budget::Reserve(storage_,root_,identity_,metadata_,false);
        // Reserve before creating the namespace; retain the parent against substitution while opening custody.
        const auto parent=storage_+L"\\_dependency-custody";
        if(!tcard_bridge::Protect(parent))throw std::runtime_error("Cannot protect dependency namespace");
        tcard_asset::File namespaceGuard(tcard_asset::open_dir(parent,false));if(!namespaceGuard)throw std::runtime_error("Unsafe dependency namespace");
        if(!tcard_bridge::Protect(root_))throw std::runtime_error("Cannot protect dependency custody");
        if(tcard_chunks::budget::Exists(root_+L"\\binding.json")){Check();}
        else if(!tcard_bridge::Publish(root_,L"binding.json",binding_))throw std::runtime_error("Cannot retain dependency binding");
        Check();const auto catalog=tcard_chunks::budget::Load(storage_);CheckCatalog(catalog);
        // Only interrupted promotions need full proof on reopen. Stable objects are verified by Read/Ready.
        for(const auto& entry:catalog.entries)if(entry.path.starts_with(relative+L"\\")&&!entry.destination.empty()){
            const auto slot=entry.path.substr(entry.path.find_last_of(L'\\')+1);Seal(manifest_.descriptors.at(slots_.at(slot)));}
    }
    void Bind(const Manifest& manifest)const override
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
        std::lock_guard guard(mutex_);const auto supplied=tcard_dependencies::Read(Encode(manifest),manifest_.context);
        if(Encode(supplied)!=Encode(manifest_))throw std::runtime_error("Dependency provider manifest differs");Check();
    }
    Bytes Read(const Descriptor& descriptor)override
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
        std::lock_guard guard(mutex_);const auto& value=Bound(descriptor);Check();return Open(value,false,true)->Assemble();
    }
    std::vector<size_t> Missing(const Descriptor& descriptor)
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
        std::lock_guard guard(mutex_);const auto& value=Bound(descriptor);Check();auto cache=Open(value,true);const auto missing=cache->Missing();
        if(tcard_chunks::budget::Exists(Path(value,true))&&!missing.empty())throw std::runtime_error("Sealed dependency evidence damaged");return missing;
    }
    void Put(const Descriptor& descriptor,size_t index,const Bytes& bytes)
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
        std::lock_guard guard(mutex_);const auto& value=Bound(descriptor);
        if(index>=value.object.hashes.size())throw std::runtime_error("Dependency fragment index outside manifest");
        const auto length=(std::min)(tcard_chunks::chunkSize,value.object.size-index*tcard_chunks::chunkSize);
        if(bytes.size()!=length||tcard_asset::hash(bytes)!=value.object.hashes[index])throw std::runtime_error("Dependency fragment differs");
        Observe();
        // Hold a non-create parent handle. Stale providers cannot recreate retired pool paths.
        tcard_asset::File directory(tcard_asset::open_dir(root_,false));if(!directory)throw std::runtime_error("Retired dependency custody unavailable");
        if(tcard_chunks::Group(storage_)!=manifest_.context.group)throw std::runtime_error("Dependency custody key differs");
        if(tcard_chunks::budget::Exists(Path(value,true)))throw std::runtime_error("Sealed dependency custody is immutable");
        // Reserve only the first object opening; later chunks do not rescan or rewrite the quota catalog.
        Open(value,true)->Put(index,bytes);
    }
    void Seal(const Descriptor& descriptor)
    {
        std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
        std::lock_guard guard(mutex_);const auto& value=Bound(descriptor);Check();auto cache=Open(value,true);auto proof=cache->Assemble();
        SecureZeroMemory(proof.data(),proof.size());cache.reset();
        const auto catalog=tcard_chunks::budget::Load(storage_);CheckCatalog(catalog);
        const auto old=tcard_chunks::budget::Relative(catalog.owner,Path(value,false)),next=tcard_chunks::budget::Relative(catalog.owner,Path(value,true));
        const auto provisional=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path==old;});
        if(provisional!=catalog.entries.end()){
            if(!tcard_bridge::Protect(root_+L"\\_sealed"))throw std::runtime_error("Cannot protect sealed dependency custody");
            tcard_chunks::budget::Promote(storage_,Path(value,false),Path(value,true));
        }else{
            const auto sealed=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path==next;});
            if(sealed==catalog.entries.end()||!tcard_chunks::budget::Exists(Path(value,true)))throw std::runtime_error("Sealed dependency reservation unavailable");
        }
    }
    // Invoke only inside WithDependencyCommit: proof is fresh and all stable owners are already held.
    bool Release(const std::wstring& committedStorage,const DependencyCommitProof& proof)
    {
        try{
            if(!proof.Matches(committedStorage,manifest_))throw std::runtime_error("Dependency cleanup proof differs");
            std::lock_guard guard(mutex_);if(tcard_chunks::Group(storage_)!=manifest_.context.group)throw std::runtime_error("Dependency cleanup key differs");
            auto catalog=tcard_chunks::budget::Load(storage_);const auto relative=tcard_chunks::budget::Relative(catalog.owner,root_);
            const auto binding=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path==relative;});
            const bool exists=tcard_chunks::budget::Exists(root_);
            if(binding==catalog.entries.end()){
                if(exists||std::any_of(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path.starts_with(relative+L"\\");}))throw std::runtime_error("Unreserved dependency cleanup retained");
                cleanupOnly_=true;return true;
            }
            if(identity_!=Digest(Encode(manifest_))||binding_!=Record(manifest_,recordTarget_)||relative!=tcard_chunks::budget::Relative(catalog.owner,storage_+L"\\_dependency-custody\\"+identity_))throw std::runtime_error("Dependency cleanup identity differs");
            CheckCatalog(catalog);
            if(!exists){if(!binding->retiring||catalog.entries.end()!=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path.starts_with(relative+L"\\");}))throw std::runtime_error("Dependency cleanup decision missing");
                tcard_chunks::budget::Release(storage_,root_);cleanupOnly_=true;return true;}
            if(tcard_chunks::budget::Exists(root_+L"\\binding.json")){std::wstring text;if(!tcard_bridge::Read(root_,L"binding.json",text)||text!=binding_)throw std::runtime_error("Dependency cleanup binding differs");}
            else if(!binding->retiring)throw std::runtime_error("Dependency cleanup binding unavailable");
            CheckTree(catalog);cleanupOnly_=true;
            for(const auto& entry:catalog.entries)if(entry.path.starts_with(relative+L"\\")){
                const auto slot=entry.path.substr(entry.path.find_last_of(L'\\')+1);const auto& descriptor=manifest_.descriptors.at(slots_.at(slot));
                const auto path=tcard_chunks::budget::Extended(catalog.owner+L"\\"+entry.path);
                CheckCatalog(tcard_chunks::budget::Load(storage_));
                tcard_chunks::budget::Retire(storage_,path,1,descriptor.object.hashes.size(),committedStorage,manifest_,proof);
                for(size_t i=0;i<descriptor.object.hashes.size();++i)Delete(path+L"\\"+std::to_wstring(i)+L".part");Delete(path+L"\\manifest.json");
                if(tcard_chunks::budget::Exists(path)&&!RemoveDirectoryW(path.c_str()))throw std::runtime_error("Dependency object retirement incomplete");
                tcard_chunks::budget::Release(storage_,path);
            }
            for(const auto* lane:{L"_objects",L"_sealed"}){const auto path=root_+L"\\"+lane;if(tcard_chunks::budget::Exists(path)&&!RemoveDirectoryW(path.c_str()))throw std::runtime_error("Dependency retirement lane not empty");}
            CheckCatalog(tcard_chunks::budget::Load(storage_));
            tcard_chunks::budget::Retire(storage_,root_,2,0,committedStorage,manifest_,proof);Delete(root_+L"\\binding.json");
            if(tcard_chunks::budget::Exists(root_)&&!RemoveDirectoryW(root_.c_str()))throw std::runtime_error("Dependency binding retirement incomplete");
            tcard_chunks::budget::Release(storage_,root_);return true;
        }catch(...){return false;}
    }
    // One catalog-bound pool per progress pass. This is duplicate reclamation, never peer admission.
    static void Recover(const std::wstring& storage,const std::wstring& space,const std::wstring& group,const std::wstring& node,Recovery& state,bool progress=false)
    {
        std::unique_lock process(sync_mutex);const auto base=tcard_native::Base(storage);
        const std::set<std::wstring> paths{sync_owner(base),tcard_chunks::budget::Owner(base)};std::vector<std::unique_ptr<SyncLock>> owners;
        for(const auto& path:paths){if(path.empty())throw std::runtime_error("Dependency recovery owner unavailable");auto lock=std::make_unique<SyncLock>(path);
            if(!*lock)throw std::runtime_error("Dependency recovery owner busy");owners.push_back(std::move(lock));}
        const tcard_chunks::budget::Catalog location{tcard_chunks::budget::Owner(base),group,{}};tcard_bridge::Stamp stamp;
        if(!tcard_bridge::Fingerprint(tcard_chunks::budget::Folder(location),L"budget.json",stamp)){
            if(tcard_chunks::budget::Exists(tcard_chunks::budget::Folder(location)+L"\\budget.json"))throw std::runtime_error("Dependency recovery catalog unavailable");
            state={};state.done=true;return;
        }
        const auto now=GetTickCount64();const auto due=std::find_if(state.retries.begin(),state.retries.end(),[&](const auto& entry){return entry.second.first<=now;});
        if(state.known&&stamp==state.stamp&&!progress&&state.done&&due==state.retries.end())return;
        if(space.empty()||!sync_hex(group)||!sync_hex(node)||tcard_chunks::Group(storage)!=group)throw std::runtime_error("Dependency recovery identity differs");
        auto catalog=tcard_chunks::budget::Load(storage);const std::wstring component=L"_dependency-custody\\";
        if(!state.known||!(stamp==state.stamp)||progress){state.bindings.clear();state.done=false;state.stamp=stamp;state.known=true;
            for(const auto& entry:catalog.entries){const auto mark=entry.path.find(component);if(mark==std::wstring::npos)continue;
                const auto tail=entry.path.substr(mark+component.size());if(!sync_hex(tail))continue;
                if(tcard_chunks::budget::RetirementRoot(entry.path,2,0)!=entry.path||entry.identity!=tail||entry.provisional||!entry.destination.empty())throw std::runtime_error("Dependency recovery binding reservation differs");
                state.bindings.push_back(entry.path);}
            std::sort(state.bindings.begin(),state.bindings.end());
            std::erase_if(state.retries,[&](const auto& entry){return !std::binary_search(state.bindings.begin(),state.bindings.end(),entry.first);});
            const auto after=std::upper_bound(state.bindings.begin(),state.bindings.end(),state.last);
            std::rotate(state.bindings.begin(),after,state.bindings.end());state.next=0;
        }
        const auto retry=std::find_if(state.retries.begin(),state.retries.end(),[&](const auto& entry){return entry.second.first<=now;});
        std::wstring relative;
        if(retry!=state.retries.end())relative=retry->first;
        else{if(state.next>=state.bindings.size()){state.done=true;return;}relative=state.bindings[state.next++];
            if(state.retries.contains(relative))return;}
        state.last=relative;
        auto delay=[&]{if(!state.retries.contains(relative)&&state.retries.size()>=2048)throw std::runtime_error("Dependency recovery retry budget exceeded");
            auto& pending=state.retries[relative];pending.first=GetTickCount64()+(pending.second?60000:3000);pending.second=1;};
        try{
        const auto found=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& entry){return entry.path==relative;});
        if(found==catalog.entries.end()){state.retries.erase(relative);return;}const auto entry=*found;const auto root=tcard_chunks::budget::Extended(catalog.owner+L"\\"+relative);
        if(!tcard_chunks::budget::Exists(root+L"\\binding.json")){
            // A completed authenticated retirement has no payload or manifest left to prove again.
            if(!entry.retiring||entry.retireKind!=2||entry.partCount||std::any_of(catalog.entries.begin(),catalog.entries.end(),[&](const auto& row){return row.path.starts_with(relative+L"\\");})){state.retries.erase(relative);return;}
            if(!Entries(root).empty())throw std::runtime_error("Unknown final dependency retirement retained");
            if(tcard_chunks::budget::Exists(root)&&!RemoveDirectoryW(root.c_str()))throw std::runtime_error("Final dependency retirement blocked");
            tcard_chunks::budget::Release(storage,root);state.retries.erase(relative);return;
        }
        std::wstring record;if(!tcard_bridge::Read(root,L"binding.json",record))throw std::runtime_error("Dependency recovery binding unavailable");
        const auto value=JsonObject::Parse(record);
        // Historical records remain usable with caller-supplied context, but cannot self-reconstruct a target.
        if(!value.HasKey(L"CapturedTarget")){state.retries.erase(relative);return;}
        const auto source=sync_string(value,L"Source"),receiver=sync_string(value,L"Receiver");
        bool retiring=entry.retiring;for(const auto& row:catalog.entries)if(row.path.starts_with(relative+L"\\")&&row.retiring)retiring=true;
        if(sync_string(value,L"Space")!=space||sync_string(value,L"Group")!=group||(receiver!=node&&!(retiring&&source==node))){state.retries.erase(relative);return;}
        Context expected{space,group,source,receiver,std::wstring(value.GetNamedObject(L"CapturedTarget").Stringify())};
        const auto manifest=tcard_dependencies::Read(record,expected);const auto identity=Digest(Encode(manifest));
        if(identity!=entry.identity||record!=Record(manifest,true)||entry.bytes!=Metadata(record))throw std::runtime_error("Dependency recovery stored binding differs");
        const auto mark=relative.find(component);auto custody=tcard_chunks::budget::Extended(catalog.owner+L"\\"+relative.substr(0,mark));
        while(custody.size()>3&&custody.back()==L'\\')custody.pop_back();const PreparedNote note{manifest.note,Encode(manifest)};Result result;
        // Existing authenticated context permits only fresh-proof duplicate cleanup, not networking or target ACK.
        if(WithDependencyCommit(storage,custody,note,expected,[&](const DependencyCommitProof& proof){Pool pool(custody,note.package,expected,false);return pool.Release(storage,proof);},result))state.retries.erase(relative);
        else{
            const auto directory=tcard_chunks::budget::Extended(tcard_native::Resolve(storage)),stem=sync_stem(manifest.note);
            const bool absent=!tcard_chunks::budget::Exists(directory+L"\\"+stem+L"\\card.json")&&!tcard_chunks::budget::Exists(directory+L"\\"+stem+L".deleted");
            if(result.error==L"Dependency original is not committed"||absent)state.retries.erase(relative);else delay();
        }
        }catch(...){delay();throw;}
    }
    bool Ready()
    {
        try{
            std::lock_guard native(sync_mutex);SyncLock owner(tcard_chunks::budget::Owner(storage_));if(!owner)throw std::runtime_error("Dependency custody busy");
            std::lock_guard guard(mutex_);Check();const auto catalog=tcard_chunks::budget::Load(storage_);CheckCatalog(catalog);
            // Prove metadata completeness first: partial reception must not repeatedly decode sealed bodies.
            std::vector<std::unique_ptr<tcard_objects::Cache>> sealed;sealed.reserve(manifest_.descriptors.size());
            for(const auto& descriptor:manifest_.descriptors){const auto relative=tcard_chunks::budget::Relative(catalog.owner,Path(descriptor,true));
                const auto entry=std::find_if(catalog.entries.begin(),catalog.entries.end(),[&](const auto& value){return value.path==relative&&!value.provisional&&value.destination.empty();});
                if(entry==catalog.entries.end())return false;sealed.push_back(Open(descriptor,false,true));}
            for(const auto& cache:sealed){auto proof=cache->Assemble();SecureZeroMemory(proof.data(),proof.size());}
            return true;
        }catch(...){return false;}
    }
};
}
