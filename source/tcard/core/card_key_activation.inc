// Included only by card_storage.cpp: guarded publication never exposes a general bypass API.
namespace tcard_activation {
namespace {
using namespace tcard_storage;
using namespace tcard_sync;
constexpr wchar_t pointerName[]=L"key-activation.dpapi";
constexpr wchar_t recordName[]=L"activation.dpapi";
void key_require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::wstring key_canonicalize(const std::wstring& path){const auto result=sync_path_plain(path);key_require(result.size()>3&&result.size()<32000,"Invalid activation root");return result;}
std::wstring key_extend(const std::wstring& path){return tcard_native::Extended(path);}
bool key_exists(const std::wstring& path){return store_exists(key_extend(path));}
bool key_is_guid(const std::wstring& value){GUID guid{};return value.size()==36&&SUCCEEDED(CLSIDFromString((L"{"+value+L"}").c_str(),&guid));}
bool key_contains(const std::wstring& parent,const std::wstring& child){return parent==child||(child.starts_with(parent+L"\\"));}
void key_check_roots(const std::wstring& native,const std::wstring& profile){
    key_require(native==key_canonicalize(tcard_native::Owner(native))&&native==key_canonicalize(tcard_native::Base(native)),"Activation requires the native owner root");
    const auto control=profile+L"-control";key_require(native!=profile&&native!=control&&profile!=control,"Activation roots must be distinct");
    for(const auto& a:{native,profile,control})for(const auto& b:{native,profile,control})if(a!=b)key_require(!key_contains(a,b),"Activation roots overlap");
    for(const auto& root:{native,profile,control}){auto ancestor=root;
        for(;;){tcard_asset::File directory(tcard_asset::open_dir(key_extend(ancestor),false));key_require(bool(directory),"Activation root alias or reparse point");
            const auto slash=ancestor.find_last_of(L'\\');if(slash<3||slash==std::wstring::npos)break;ancestor.resize(slash);}}
    key_require(store_find_root(key_extend(control)).empty(),"Activation control must be outside encrypted roots");
}
JsonObject key_read(const std::wstring& directory,const wchar_t* name){
    std::wstring sealed,plain;key_require(tcard_bridge::Read(key_extend(directory),name,sealed)&&tcard_bridge::Seal(sealed,plain,true),"Activation journal authentication failed");
    struct Clear{std::wstring& value;~Clear(){SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} clear{plain};return JsonObject::Parse(plain);
}
void key_write(const std::wstring& directory,const wchar_t* name,const JsonObject& record){
    auto plain=std::wstring(record.Stringify());std::wstring sealed;const auto protectedRecord=tcard_bridge::Seal(plain,sealed);SecureZeroMemory(plain.data(),plain.size()*sizeof(wchar_t));
    key_require(protectedRecord&&tcard_bridge::Publish(key_extend(directory),name,sealed),"Activation journal publication failed");
}
std::wstring key_encode(const auto& bytes){std::wstring result;for(auto byte:bytes){result+=L"0123456789abcdef"[byte>>4];result+=L"0123456789abcdef"[byte&15];}return result;}
void key_decode(const std::wstring& value,auto& bytes){
    key_require(value.size()==bytes.size()*2&&value.find_first_not_of(L"0123456789abcdef")==std::wstring::npos,"Invalid activation key material");
    auto digit=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};for(size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<uint8_t>((digit(value[2*i])<<4)|digit(value[2*i+1]));
}
void key_check_replace(const JsonObject& record);
void key_check_complete(const JsonObject& record);
JsonObject key_read_record(const std::wstring& profile,Status& status){
    const auto control=profile+L"-control";if(!key_exists(control+L"\\"+pointerName))return nullptr;
    const auto pointer=key_read(control,pointerName);const auto token=sync_string(pointer,L"Token");
    key_require(pointer.GetNamedNumber(L"Version")==1&&key_is_guid(token),"Invalid activation pointer");
    const auto transaction=control+L"\\_key-transactions\\"+token;auto record=key_read(transaction,recordName);
    key_require(record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Profile")==profile&&sync_string(record,L"Transaction")==transaction,"Activation journal binding differs");
    const auto phase=sync_string(record,L"Phase"),group=sync_string(record,L"Group"),previous=sync_string(record,L"Previous");
    key_require((phase==L"pending"||phase==L"complete")&&key_is_guid(group)&&(previous.empty()||key_is_guid(previous))&&group!=previous,"Invalid activation decision");
    status.intent=record.GetNamedString(L"Intent",L"rotate");key_require(status.intent==L"rotate"||status.intent==L"merge"||status.intent==L"replace","Invalid activation intent");
    const auto native=sync_string(record,L"Native");key_require(native==key_canonicalize(native),"Invalid activation native binding");key_check_roots(native,profile);
    tcard_crypto::Keys keys;key_decode(sync_string(record,L"Public"),keys.publicKey);key_decode(sync_string(record,L"Data"),keys.dataKey);key_require(keys.publicKey[0]==4,"Invalid activation public key");
    key_require(sync_hex(sync_string(record,L"Credentials")),"Invalid activation credential binding");
    const auto scope=std::wstring(record.GetNamedString(L"NativeScope",L""));key_require(scope.empty()||sync_hex(scope),"Invalid required native scope");
    if(status.intent==L"replace")key_check_replace(record);
    if(phase==L"complete"&&tcard_storage::Ready(native)&&tcard_storage::Ready(profile))key_check_complete(record);
    status.phase=phase;status.transaction=transaction;status.group=group;return record;
}
JsonObject key_read_marker(const std::wstring& root){
    Bytes bytes;std::wstring plain;key_require(store_read_raw(key_extend(root+L"\\"+activationName),bytes)&&tcard_bridge::Seal(tcard_asset::decode(bytes),plain,true),"Activation root barrier unavailable");return JsonObject::Parse(plain);
}
void key_guard(const std::wstring& root,const std::wstring& transaction){
    if(key_exists(root+L"\\"+activationName)){const auto marker=key_read_marker(root);key_require(marker.GetNamedNumber(L"Version")==1&&sync_string(marker,L"Root")==root&&sync_string(marker,L"Transaction")==transaction,"Another activation owns this root");return;}
    JsonObject marker;marker.Insert(L"Version",JsonValue::CreateNumberValue(1));marker.Insert(L"Root",JsonValue::CreateStringValue(root));marker.Insert(L"Transaction",JsonValue::CreateStringValue(transaction));std::wstring sealed;
    key_require(tcard_bridge::Seal(std::wstring(marker.Stringify()),sealed)&&store_write_raw(key_extend(root+L"\\"+activationName),tcard_asset::encode(sealed)),"Cannot close activation barrier");
}
void key_release(const std::wstring& root,const std::wstring& transaction){
    if(!key_exists(root+L"\\"+activationName))return;key_guard(root,transaction);key_require(DeleteFileW(key_extend(root+L"\\"+activationName).c_str())!=FALSE,"Cannot release activation barrier");
}
Bytes key_open(const std::wstring& root,const std::wstring& relative,State& state){
    Bytes raw,plain;key_require(store_read_raw(key_extend(root+L"\\"+relative),raw)&&store_open(state,raw,plain),"Activation ciphertext verification failed");return plain;
}
std::wstring key_read_group(const std::wstring& profile,State& state){
    if(!key_exists(profile+L"\\active.json"))return {};const auto plain=key_open(profile,L"active.json",state);const auto active=JsonObject::Parse(tcard_asset::decode(plain));const auto group=sync_string(active,L"Group");
    key_require(active.GetNamedNumber(L"Version")==1&&key_is_guid(group),"Invalid selected activation group");return group;
}
#include "card_replacement_activation.inc"

void key_set_status(const JsonObject& record,Status& status){status.phase=sync_string(record,L"Phase");status.transaction=sync_string(record,L"Transaction");status.group=sync_string(record,L"Group");status.intent=record.GetNamedString(L"Intent",L"rotate");}
struct KeyLock {
    std::unique_lock<std::recursive_mutex> guard{sync_mutex};std::vector<std::unique_ptr<SyncLock>> writers;std::vector<std::unique_ptr<Lock>> storage;
    KeyLock(const std::wstring& native,const std::wstring& profile){std::vector<std::wstring> roots{native,profile};std::sort(roots.begin(),roots.end());
        for(const auto& root:roots){auto writer=std::make_unique<SyncLock>(root);key_require(bool(*writer),"Activation writer is busy");writers.push_back(std::move(writer));}
        for(const auto& root:roots)storage.push_back(std::make_unique<Lock>(key_extend(root)));
    }
};
void key_check_engine(const JsonObject& record){
    const auto transaction=sync_string(record,L"Transaction");const auto engine=key_read(transaction+L"\\engine",L"transition.dpapi");
    key_require(engine.GetNamedNumber(L"Version")==1&&sync_string(engine,L"Path")==transaction+L"\\engine"&&sync_string(engine,L"Mode")==L"rotate","Activation engine binding differs");
    const auto roots=engine.GetNamedArray(L"Roots");std::vector<std::wstring> expected{sync_string(record,L"Native"),sync_string(record,L"Profile")};std::sort(expected.begin(),expected.end());
    key_require(roots.Size()==2,"Activation engine root count differs");tcard_crypto::Keys keys;key_decode(sync_string(record,L"Public"),keys.publicKey);key_decode(sync_string(record,L"Data"),keys.dataKey);
    for(unsigned i=0;i<2;++i)key_require(sync_string(roots.GetObjectAt(i),L"Path")==expected[i]&&tcard_storage::Matches(key_extend(transaction+L"\\engine\\root-"+std::to_wstring(i)+L"\\candidate"),keys),"Activation engine identity differs");
}
}
bool Inspect(const std::wstring& path,Status& status){status={};try{const auto record=key_read_record(key_canonicalize(path),status);status.active=record&&status.phase==L"complete";return true;}catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Activation inspection failed";return false;}}
bool Pending(const std::wstring& profile){Status status;return !Inspect(profile,status)||(!status.phase.empty()&&!status.active);}
static bool key_stage(const std::wstring& nativePath,const std::wstring& profilePath,const std::wstring& group,const tcard_crypto::Keys& candidate,Status& status,Intent intent,const JsonObject& replace){
    status={};try{const auto native=key_canonicalize(nativePath),profile=key_canonicalize(profilePath),control=profile+L"-control";
        key_require(intent==Intent::Rotate||intent==Intent::Merge,"Unsupported activation intent");
        key_require(tcard_bridge::Protect(key_extend(control)),"Activation control unavailable");key_check_roots(native,profile);KeyLock locks(native,profile);
        key_require(!Pending(profile)&&key_is_guid(group)&&candidate.publicKey[0]==4,"Another activation requires recovery");
        tcard_crypto::Keys current;key_require(tcard_storage::LoadKey(key_extend(native),current)&&tcard_storage::Matches(key_extend(profile),current)&&current.publicKey!=candidate.publicKey,"Activation requires matching old encrypted roots and a different identity");
        State old;key_require(store_access_key(key_extend(profile),old,false),"Existing profile key unavailable");const auto previous=key_read_group(profile,old);key_require(previous!=group,"Activation namespace must be new");
        auto credentials=key_open(profile,L"groups\\"+group+L"\\_sync\\credentials.json",old);Wipe clear{credentials};
        const auto catalog=control+L"\\_key-transactions",transaction=key_canonicalize(catalog+L"\\"+tcard_bridge::Token());key_require(tcard_bridge::Protect(key_extend(catalog))&&tcard_bridge::Protect(key_extend(transaction)),"Cannot protect activation recovery");
        State nativeKeys;key_require(store_access_key(key_extend(native),nativeKeys,false),"Native activation key unavailable");
        const auto scope=replace?sync_string(replace,L"After"):key_read_scope(native,nativeKeys);
        if(replace){JsonObject proof;proof.Insert(L"NativeScope",JsonValue::CreateStringValue(scope));proof.Insert(L"Replacement",replace);key_check_replace(proof);
            key_require(key_read_native(native,nativeKeys)==sync_string(replace,L"Before"),"Native selection changed before replacement staging");
            tcard_retained::Catalog actual;size_t visited=0;key_scan_generation(native,L"_native-generations\\"+scope+L"\\data",L"",nativeKeys,actual,visited);
            key_require(actual==key_read_catalog(replace),"Replacement generation changed before migration");}
        tcard_transition::Status engine;key_require(tcard_transition::Begin(transaction+L"\\engine",{native,profile},candidate,tcard_transition::Mode::Rotate,engine)&&!engine.active,"Cannot stage activation engine");
        JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Phase",L"pending"},{L"Intent",replace?L"replace":intent==Intent::Merge?L"merge":L"rotate"},{L"NativeScope",scope},{L"Transaction",transaction},{L"Native",native},{L"Profile",profile},{L"Group",group},{L"Previous",previous},{L"Public",key_encode(candidate.publicKey)},{L"Data",key_encode(candidate.dataKey)},{L"Credentials",tcard_asset::hash(credentials)}})record.Insert(name,JsonValue::CreateStringValue(value));
        if(replace)record.Insert(L"Replacement",replace);
        key_write(transaction,recordName,record);JsonObject pointer;pointer.Insert(L"Version",JsonValue::CreateNumberValue(1));pointer.Insert(L"Token",JsonValue::CreateStringValue(transaction.substr(catalog.size()+1)));key_write(control,pointerName,pointer);key_set_status(record,status);return true;
    }catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Activation staging failed";return false;}
}
bool Stage(const std::wstring& native,const std::wstring& profile,const std::wstring& group,const tcard_crypto::Keys& candidate,Status& status,Intent intent){return key_stage(native,profile,group,candidate,status,intent,nullptr);}
bool StageVerified(const std::wstring& native,const std::wstring& profile,const std::wstring& group,const tcard_crypto::Keys& candidate,const ReplacementProof& proof,Status& status){
    status={};try{const auto record=JsonObject::Parse(proof.metadata_);
        key_require(sync_string(record,L"Native")==key_canonicalize(native)&&sync_string(record,L"Profile")==key_canonicalize(profile)&&sync_string(record,L"Group")==group,"Replacement proof owner differs");
        return key_stage(native,profile,group,candidate,status,Intent::Merge,record.GetNamedObject(L"Replacement"));
    }catch(const std::exception& e){status.error=e.what();return false;}catch(...){status.error="Replacement proof failed";return false;}
}
bool Resume(const std::wstring& nativePath,const std::wstring& profilePath,Status& status,const tcard_transition::Progress& progress){
    status={};try{const auto native=key_canonicalize(nativePath),profile=key_canonicalize(profilePath);auto record=key_read_record(profile,status);if(!record)return true;
        key_require(sync_string(record,L"Native")==native,"Activation native owner differs");KeyLock locks(native,profile);record=key_read_record(profile,status);const auto transaction=status.transaction;key_check_engine(record);
        auto notify=[&](const wchar_t* phase,size_t root=0){if(progress)progress(phase,root,0);};
        if(status.phase!=L"complete"){
            key_guard(native,transaction);key_guard(profile,transaction);notify(L"activation-barrier");
            tcard_transition::Status engine;key_require(tcard_transition::Commit(transaction+L"\\engine",engine,progress)&&engine.active,"Activation data migration is incomplete");
        }
        State nativeKeys,profileKeys;tcard_crypto::Keys candidate;key_decode(sync_string(record,L"Public"),candidate.publicKey);key_decode(sync_string(record,L"Data"),candidate.dataKey);
        for(const auto& root:{native,profile}){State& keys=root==native?nativeKeys:profileKeys;
            key_require(!key_exists(root+L"\\"+transitionName)&&!key_exists(root+L"\\"+pendingName)&&key_exists(root+L"\\"+requiredName)&&store_access_key(key_extend(root),keys,false)&&keys.keys.publicKey==candidate.publicKey&&keys.keys.dataKey==candidate.dataKey,"Activation target keys differ");}
        if(status.phase!=L"complete"&&status.intent==L"replace")key_activate_replace(native,record,nativeKeys,progress);
        key_verify_scope(native,nativeKeys,std::wstring(record.GetNamedString(L"NativeScope",L"")));
        const auto selected=key_read_group(profile,profileKeys);
        if(status.phase!=L"complete"){
            auto credentials=key_open(profile,L"groups\\"+status.group+L"\\_sync\\credentials.json",profileKeys);Wipe clear{credentials};key_require(tcard_asset::hash(credentials)==sync_string(record,L"Credentials"),"Staged activation credentials changed");
            key_require(selected==sync_string(record,L"Previous")||selected==status.group,"Selected group changed during activation");
            if(selected!=status.group){JsonObject pointer;pointer.Insert(L"Version",JsonValue::CreateNumberValue(1));pointer.Insert(L"Group",JsonValue::CreateStringValue(status.group));const auto plain=tcard_asset::encode(pointer.Stringify().c_str());Bytes sealed;
                key_require(store_seal(key_extend(profile),profileKeys,plain,sealed)&&store_write_raw(key_extend(profile+L"\\active.json"),sealed)&&key_open(profile,L"active.json",profileKeys)==plain,"Cannot publish verified activation group");}
            notify(L"activation-pointer");record.Insert(L"Phase",JsonValue::CreateStringValue(L"complete"));key_write(transaction,recordName,record);notify(L"activation-decision");
        }else key_require(selected==status.group,"Completed activation group differs");
        key_release(native,transaction);notify(L"activation-release",0);key_release(profile,transaction);notify(L"activation-release",1);
        key_set_status(record,status);status.active=true;return true;
    }catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Activation resume failed";return false;}
}
}
