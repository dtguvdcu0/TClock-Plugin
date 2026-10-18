// Included only by card_replacement.cpp. Candidate receipts never authorize live activation alone.
namespace tcard_replacement {
namespace {
using namespace tcard_sync;
constexpr wchar_t receiptName[]=L".tcard-replacement.json";
void replace_require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
bool replace_contains(const std::wstring& parent,const std::wstring& child){return parent==child||child.starts_with(parent+L"\\");}
std::wstring replace_root(const std::wstring& value){const auto path=sync_path_plain(value);replace_require(path.size()>3&&path==sync_owner(path)&&!path.ends_with(L".json"),"Invalid replacement candidate root");return path;}
tcard_retained::Catalog replace_catalog(const std::wstring& path){auto files=tcard_retained::Collect(path,true);files.erase(receiptName);return files;}
JsonObject replace_read(const std::wstring& path,Candidate& status)
{
    replace_require(tcard_storage::Required(path)&&tcard_storage::Ready(path)&&tcard_native::Selected(path).empty(),"Replacement candidate encryption unavailable");
    std::wstring text;replace_require(tcard_bridge::Read(path,receiptName,text),"Replacement candidate receipt unavailable");const auto record=JsonObject::Parse(text);
    status.path=path;status.node=sync_string(record,L"SourceNode");status.target=sync_string(record,L"Target");status.catalog=sync_string(record,L"Catalog");
    const auto phase=sync_string(record,L"Phase"),source=sync_string(record,L"Source");
    replace_require(record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Kind")==L"source-only"&&sync_string(record,L"Root")==path&&
        sync_string(record,L"Owner")==tcard_bootstrap::Node(path)&&sync_string(record,L"Group")==tcard_chunks::Group(path)&&sync_hex(status.node)&&
        source==replace_root(source)&&!replace_contains(source,path)&&!replace_contains(path,source)&&
        (phase==L"building"||phase==L"sealed")&&sync_string(record,L"TargetDigest")==tcard_bridge::Hash(status.target),"Replacement candidate binding differs");
    const auto wanted=selection::Read(status.target),actual=selection::Read(tcard_stage::Inventory(path));
    replace_require(!selection::Missing(actual,wanted),"Replacement candidate contains unrelated local intent");
    if(phase==L"sealed"){
        replace_require(sync_hex(status.catalog)&&wanted==actual&&tcard_native::Digest(replace_catalog(path))==status.catalog,"Sealed replacement candidate changed");status.ready=true;
    }else replace_require(status.catalog.empty(),"Unsealed replacement catalog was activated");return record;
}
void replace_write(const std::wstring& path,const JsonObject& record)
{
    const auto text=std::wstring(record.Stringify());std::wstring verified;
    replace_require(tcard_bridge::Publish(path,receiptName,text)&&tcard_bridge::Read(path,receiptName,verified)&&verified==text,"Cannot retain replacement candidate receipt");
}
void replace_empty(const std::wstring& path)
{
    WIN32_FIND_DATAW item{};const auto find=FindFirstFileW((tcard_native::Extended(path)+L"\\*").c_str(),&item);replace_require(find!=INVALID_HANDLE_VALUE,"Replacement root enumeration failed");
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};
    do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;
        replace_require(!(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&(name==L".tcard-vault.key"||name==L".tcard-vault.required"),"Replacement requires an empty encrypted candidate");
    }while(FindNextFileW(find,&item));replace_require(GetLastError()==ERROR_NO_MORE_FILES,"Replacement root enumeration interrupted");
}
}
bool Inspect(const std::wstring& candidate,Candidate& status)
{
    status={};try{const auto path=replace_root(candidate);std::lock_guard guard(sync_mutex);SyncLock lock(path);replace_require(bool(lock),"Replacement candidate busy");replace_read(path,status);return true;}
    catch(const std::exception& e){status.ready=false;status.error=e.what();return false;}catch(...){status.ready=false;status.error="Replacement inspection failed";return false;}
}
bool Build(const std::wstring& sourcePath,const std::wstring& candidatePath,const tcard_crypto::Keys& keys,Candidate& status,const tcard_transition::Progress& progress)
{
    status={};try{
        const auto source=replace_root(sourcePath),candidate=replace_root(candidatePath);
        replace_require(!replace_contains(source,candidate)&&!replace_contains(candidate,source),"Replacement roots overlap");
        std::lock_guard guard(sync_mutex);std::vector<std::unique_ptr<SyncLock>> locks;auto roots=std::vector<std::wstring>{source,candidate};std::sort(roots.begin(),roots.end());
        for(const auto& path:roots){auto lock=std::make_unique<SyncLock>(path);replace_require(bool(*lock),"Replacement builder busy");locks.push_back(std::move(lock));}
        replace_require(tcard_storage::Matches(candidate,keys),"Replacement candidate key differs");JsonObject record;
        if(sync_exists(candidate,receiptName)){
            record=replace_read(candidate,status);replace_require(sync_string(record,L"Source")==source,"Replacement source changed");if(status.ready)return true;
        }else{
            replace_empty(candidate);const auto captured=tcard_source::Read(source);const auto snapshot=tcard_source::Inspect(captured);
            replace_require(snapshot.ready&&tcard_storage::Matches(source,keys),"Replacement source is not a complete matching snapshot");
            record=JsonObject{};record.Insert(L"Version",JsonValue::CreateNumberValue(1));
            for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",L"source-only"},{L"Phase",L"building"},{L"Root",candidate},
                {L"Owner",tcard_bootstrap::Node(candidate)},{L"Group",tcard_chunks::Group(candidate)},{L"Source",source},{L"SourceNode",snapshot.node},
                {L"Target",snapshot.target},{L"TargetDigest",tcard_bridge::Hash(snapshot.target)},{L"Catalog",L""}})record.Insert(name,JsonValue::CreateStringValue(value));
            replace_write(candidate,record);replace_read(candidate,status);
        }
        const auto captured=tcard_source::Read(source);const auto snapshot=tcard_source::Inspect(captured);
        replace_require(snapshot.ready&&snapshot.node==status.node&&snapshot.target==status.target&&tcard_storage::Matches(source,keys),"Pinned replacement source changed");
        const auto wanted=selection::Read(status.target);size_t count=0;
        for(;;){
            const auto before=selection::Read(tcard_stage::Inventory(candidate));if(before==wanted)break;
            replace_require(++count<=32768,"Replacement candidate exceeds bounded steps");std::wstring packet,delta;Result result;unsigned version=0;
            replace_require(ExportSelection(source,before,packet,result,version,true,true,status.target),"Cannot export replacement originals");selection::Unwrap(packet,delta,true);
            if(tcard_objects::Referenced(delta)){tcard_blobs::Stage(source,candidate,delta);replace_require(tcard_blobs::Import(candidate,candidate,delta,result),"Cannot import replacement object originals");tcard_blobs::ReapLocal(candidate);}
            else replace_require(ImportChanges(candidate,delta,result),"Cannot import replacement originals");
            const auto after=selection::Read(tcard_stage::Inventory(candidate));
            replace_require(!result.pending&&!result.partial&&!selection::Missing(after,wanted)&&after!=before,"Replacement import did not make a complete causal step");
            if(progress)progress(L"replacement-copy",0,count);
        }
        record.Insert(L"Catalog",JsonValue::CreateStringValue(tcard_native::Digest(replace_catalog(candidate))));record.Insert(L"Phase",JsonValue::CreateStringValue(L"sealed"));
        replace_write(candidate,record);replace_read(candidate,status);if(progress)progress(L"replacement-sealed",0,count);return status.ready;
    }catch(const std::exception& e){status.ready=false;status.error=e.what();return false;}catch(...){status.ready=false;status.error="Replacement construction failed";return false;}
}
}
