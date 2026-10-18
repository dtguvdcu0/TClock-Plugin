#include <winsock2.h>
#include "card_storage_transition.h"
#include "card_sync_store.h"
#include "card_sync_selection.h"
#include "card_sync_bridge.h"
#include <memory>
#include <cstring>

namespace tcard_transition {
namespace {
using tcard_crypto::Bytes;
using namespace tcard_sync;
constexpr wchar_t barrier[]=L".tcard-vault.transition";
constexpr wchar_t journalName[]=L"transition.dpapi";
constexpr wchar_t keyName[]=L".tcard-vault.key";
constexpr wchar_t requiredName[]=L".tcard-vault.required";
constexpr size_t maxFiles=10000,maxRoots=8;
thread_local const Progress* observer=nullptr;
thread_local std::wstring workspace;
struct Workspace {std::wstring previous;explicit Workspace(const std::wstring& value):previous(workspace){workspace=value;}~Workspace(){workspace=previous;}};
struct SourceChanged:std::runtime_error {SourceChanged():std::runtime_error("Source changed during staging"){} };
void transition_progress(const wchar_t* phase,size_t root,size_t file=0){if(observer&&*observer)(*observer)(phase,root,file);}
struct Wipe {Bytes& bytes;~Wipe(){if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());}};
void transition_require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
std::wstring transition_path(const std::wstring& path){const auto full=sync_path_plain(path);transition_require(!full.empty()&&full.size()>3,"Invalid transition path");return full;}
std::wstring transition_extended(const std::wstring& path){
    if(path.starts_with(L"\\\\?\\"))return path;
    return path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
}
bool transition_exists(const std::wstring& path){const auto value=GetFileAttributesW(transition_extended(path).c_str());if(value!=INVALID_FILE_ATTRIBUTES)return true;const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;throw std::runtime_error("Transition path unavailable");}
bool transition_inside(const std::wstring& parent,const std::wstring& child){return parent==child||(child.size()>parent.size()&&child.starts_with(parent)&&child[parent.size()]==L'\\');}
Bytes transition_raw(const std::wstring& path){
    tcard_asset::File file(CreateFileW(transition_extended(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    transition_require(file&&tcard_asset::regular(file.value,false)&&GetFileInformationByHandle(file.value,&info)&&info.nNumberOfLinks==1&&GetFileSizeEx(file.value,&length)&&length.QuadPart>=0&&length.QuadPart<=tcard_asset::max_file+tcard_crypto::envelopeOverhead,"Unsafe or unavailable transition file");
    Bytes bytes(static_cast<size_t>(length.QuadPart));DWORD read=0;transition_require(bytes.empty()||(ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size()),"Transition file read failed");return bytes;
}
void transition_directories(const std::wstring& root,const std::wstring& relative){
    size_t at=0;while((at=relative.find(L'\\',at))!=std::wstring::npos){const auto part=root+L"\\"+relative.substr(0,at);tcard_asset::File opened(tcard_asset::open_dir(transition_extended(part),true));transition_require(static_cast<bool>(opened),"Unsafe transition subdirectory");++at;}
}

void transition_publish(const std::wstring& root,const std::wstring& relative,const Bytes& bytes,const std::wstring& temp,bool notify){
    transition_directories(root,relative);const auto slash=relative.find_last_of(L'\\');const auto directory=slash==std::wstring::npos?root:root+L"\\"+relative.substr(0,slash);const auto target=root+L"\\"+relative,stage=directory+L"\\"+temp;
    if(transition_exists(stage))transition_require(transition_raw(stage)==bytes,"Owned publication stage is corrupt; preserve it for recovery");
    else{tcard_asset::File file(CreateFileW(transition_extended(stage).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));DWORD count=0;
        transition_require(file&&(bytes.empty()||(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()))&&FlushFileBuffers(file.value),"Transition stage write failed");}
    if(notify){const bool live=!transition_inside(workspace,transition_path(target));transition_progress(live?(target.ends_with(barrier)?L"barrier-publication":L"live-publication"):L"publication",0);}
    transition_require(MoveFileExW(transition_extended(stage).c_str(),transition_extended(target).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Transition publication blocked");
    transition_require(transition_raw(target)==bytes,"Transition publication verification failed");
}
void transition_write(const std::wstring& root,const std::wstring& relative,const Bytes& bytes){
    transition_require(!workspace.empty(),"Publication needs an owned transition workspace");const auto target=transition_path(root+L"\\"+relative),hash=tcard_asset::hash(bytes),leaf=tcard_asset::hash(tcard_asset::encode(target))+L".dpapi";
    const auto temp=L".tcard-transition-"+tcard_asset::hash(tcard_asset::encode(workspace+L"|"+target+L"|"+hash));
    const auto publications=workspace+L"\\publications";transition_require(tcard_bridge::Protect(transition_extended(publications)),"Cannot protect publication intents");
    JsonObject intent;intent.Insert(L"Version",JsonValue::CreateNumberValue(1));intent.Insert(L"Owner",JsonValue::CreateStringValue(workspace));intent.Insert(L"Target",JsonValue::CreateStringValue(target));intent.Insert(L"Hash",JsonValue::CreateStringValue(hash));intent.Insert(L"Temp",JsonValue::CreateStringValue(temp));std::wstring sealed;
    transition_require(tcard_bridge::Seal(std::wstring(intent.Stringify()),sealed),"Cannot protect publication intent");const auto protectedBytes=tcard_asset::encode(sealed);
    transition_publish(publications,leaf,protectedBytes,L".intent-"+tcard_asset::hash(protectedBytes),false);
    transition_publish(root,relative,bytes,temp,true);
    transition_require(DeleteFileW(transition_extended(publications+L"\\"+leaf).c_str())!=FALSE,"Completed publication intent cleanup failed");
}
void transition_recover(const std::wstring& transaction,const JsonObject& record){
    const auto phase=sync_string(record,L"Phase");const bool closed=phase==L"prepared"||phase==L"committing"||phase==L"committed";
    for(const auto& value:record.GetNamedArray(L"Roots")){const auto path=sync_string(value.GetObject(),L"Path");if(closed||transition_exists(path+L"\\"+barrier))transition_require(Pending(path)==transaction,"Recovery requires all owned transition barriers");}
    const auto folder=transaction+L"\\publications";if(!transition_exists(folder))return;tcard_asset::File checked(tcard_asset::open_dir(transition_extended(folder),false));transition_require(static_cast<bool>(checked),"Unsafe publication intent directory");WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW(transition_extended(folder+L"\\*.dpapi").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){transition_require(GetLastError()==ERROR_FILE_NOT_FOUND,"Publication intent enumeration failed");return;}struct Search{HANDLE handle;~Search(){FindClose(handle);}} search{handle};size_t count=0;
    do{transition_require(++count<=128&&!(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Unsafe or excessive publication intents");const std::wstring leaf=item.cFileName;std::wstring plain;transition_require(tcard_bridge::Seal(tcard_asset::decode(transition_raw(folder+L"\\"+leaf)),plain,true),"Publication intent authentication failed");const auto intent=JsonObject::Parse(plain);
        const auto target=sync_string(intent,L"Target"),hash=sync_string(intent,L"Hash"),temp=sync_string(intent,L"Temp");transition_require(intent.GetNamedNumber(L"Version")==1&&sync_string(intent,L"Owner")==transaction&&target==transition_path(target)&&sync_hex(hash)&&leaf==tcard_asset::hash(tcard_asset::encode(target))+L".dpapi"&&temp==L".tcard-transition-"+tcard_asset::hash(tcard_asset::encode(transaction+L"|"+target+L"|"+hash)),"Invalid publication intent identity");
        bool owned=transition_inside(transaction,target);for(const auto& value:record.GetNamedArray(L"Roots")){const auto root=sync_string(value.GetObject(),L"Path");if(!transition_inside(root,target))continue;owned=true;
            if(target!=root+L"\\"+barrier)transition_require(Pending(root)==transaction,"Live publication lacks its owned closed barrier");}
        transition_require(owned,"Publication intent escapes transaction roots");const auto slash=target.find_last_of(L'\\');const auto stage=target.substr(0,slash+1)+temp;
        if(transition_exists(stage)){const auto bytes=transition_raw(stage);transition_require(tcard_asset::hash(bytes)==hash,"Owned publication stage changed");transition_publish(target.substr(0,slash),target.substr(slash+1),bytes,temp,false);}
        else transition_require(transition_exists(target)&&tcard_asset::hash(transition_raw(target))==hash,"Publication intent has no verified output");
        transition_require(DeleteFileW(transition_extended(folder+L"\\"+leaf).c_str())!=FALSE,"Recovered publication intent cleanup failed");
    }while(FindNextFileW(handle,&item));transition_require(GetLastError()==ERROR_NO_MORE_FILES,"Publication intent enumeration interrupted");
}
void transition_collect(const std::wstring& root,const std::wstring& current,std::vector<std::wstring>& result,unsigned depth=0){
    transition_require(depth<=32&&result.size()<=maxFiles,"Transition inventory exceeds bounds");tcard_asset::File directory(tcard_asset::open_dir(transition_extended(current),false));transition_require(static_cast<bool>(directory),"Unsafe transition root");WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW(transition_extended(current+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){transition_require(GetLastError()==ERROR_FILE_NOT_FOUND,"Transition enumeration failed");return;}struct Search{HANDLE handle;~Search(){FindClose(handle);}} search{handle};
    do{const std::wstring name=item.cFileName;if(name==L"."||name==L"..")continue;transition_require(!(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Transition refuses reparse points");
        if(current==root&&(name==keyName||name==requiredName||name==barrier||name==L".tcard-key.activation"))continue;
        transition_require(name!=L".tcard-vault.pending"&&name!=L".tcard-vault.stage"&&name.find(L".tcard-transition-")!=0,"Another storage migration or unfinished publication requires recovery");
        const auto path=current+L"\\"+name;if(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)transition_collect(root,path,result,depth+1);else{result.push_back(path.substr(root.size()+1));transition_require(result.size()<=maxFiles,"Transition inventory exceeds bounds");}
    }while(FindNextFileW(handle,&item));transition_require(GetLastError()==ERROR_NO_MORE_FILES,"Transition enumeration interrupted");
}
std::vector<std::wstring> transition_files(const std::wstring& root){std::vector<std::wstring> files;transition_collect(root,root,files);std::sort(files.begin(),files.end());return files;}
struct Vault {tcard_crypto::Keys keys;std::array<uint8_t,16> generation{};};
void transition_key(const Bytes& sealed,Vault& vault){
    const wchar_t domain[]=L"TCard.NativeStorage.v1";DATA_BLOB entropy{sizeof(domain),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(domain))},input{static_cast<DWORD>(sealed.size()),const_cast<BYTE*>(sealed.data())},output{};
    transition_require(CryptUnprotectData(&input,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)!=FALSE,"Protected transition key unavailable");struct Blob{DATA_BLOB value;~Blob(){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}} blob{output};
    transition_require(output.cbData==125&&memcmp(output.pbData,"TCV1",4)==0&&output.pbData[4]==4,"Invalid native transition key");uint64_t attempts=0;for(unsigned i=0;i<8;++i)attempts|=static_cast<uint64_t>(output.pbData[117+i])<<(i*8);transition_require(attempts<=1000000,"Invalid native nonce allocator");std::copy_n(output.pbData+4,65,vault.keys.publicKey.begin());std::copy_n(output.pbData+69,32,vault.keys.dataKey.begin());std::copy_n(output.pbData+101,16,vault.generation.begin());
    transition_require(std::any_of(vault.generation.begin(),vault.generation.end(),[](BYTE value){return value!=0;}),"Invalid native transition generation");
}
Bytes transition_open(const Vault& vault,const Bytes& stored){
    transition_require(stored.size()>=tcard_crypto::envelopeOverhead&&memcmp(stored.data(),"TCE2",4)==0,"Transition source is not authenticated ciphertext");tcard_crypto::Context context;context.kind=5;context.generation=vault.generation;std::copy_n(stored.begin()+85,32,context.object.begin());Bytes plain;
    transition_require(tcard_crypto::Open(vault.keys,context,stored,plain)&&tcard_asset::hash(plain)==[&]{const wchar_t hex[]=L"0123456789abcdef";std::wstring hash;for(BYTE value:context.object){hash+=hex[value>>4];hash+=hex[value&15];}return hash;}(),"Transition ciphertext authentication failed");return plain;
}
void transition_native(const std::wstring& root,const Vault& keys,const std::vector<std::wstring>& files){
    std::map<std::wstring,std::wstring> ancestry;std::set<std::wstring> activated;
    for(const auto& relative:files){if(!relative.ends_with(L"\\activation.json")||relative.find(L"_bootstrap-stage\\")==std::wstring::npos)continue;
        auto plain=transition_open(keys,transition_raw(root+L"\\"+relative));Wipe wipe{plain};const auto record=JsonObject::Parse(tcard_asset::decode(plain));const auto before=sync_string(record,L"Before"),after=sync_string(record,L"After");
        transition_require(record.GetNamedNumber(L"Version")==1&&(before.empty()||sync_hex(before))&&sync_hex(after)&&before!=after&&sync_hex(sync_string(record,L"Catalog"))&&sync_hex(sync_string(record,L"Native")),"Native activation journal is invalid");
        selection::Read(std::wstring(record.GetNamedObject(L"Inventory").Stringify()));transition_require(!ancestry.contains(after)||ancestry.at(after)==before,"Native activation ancestry differs");ancestry[after]=before;activated.insert(after);
    }
    if(activated.empty())return;const auto pointer=root+L"\\_native-generations\\active.json";transition_require(transition_exists(root+L"\\.tcard-native.required")&&transition_exists(pointer),"Native activation requires its authenticated selector");auto plain=transition_open(keys,transition_raw(pointer));Wipe wipe{plain};const auto selected=JsonObject::Parse(tcard_asset::decode(plain));transition_require(selected.GetNamedNumber(L"Version")==1,"Native selector version differs");auto generation=sync_string(selected,L"Generation");std::set<std::wstring> complete;
    for(unsigned i=0;!generation.empty()&&i<=16;++i){transition_require(sync_hex(generation)&&complete.insert(generation).second,"Native activation ancestry cycles");const auto path=root+L"\\_native-generations\\"+generation+L"\\data";tcard_asset::File checked(tcard_asset::open_dir(transition_extended(path),false));transition_require(static_cast<bool>(checked),"Completed native generation is missing");const auto found=ancestry.find(generation);if(found==ancestry.end())break;generation=found->second;}
    for(const auto& generationId:activated)transition_require(complete.contains(generationId),"Uncommitted native activation requires recovery before key rotation");
}
std::wstring transition_area(const std::wstring& transaction,unsigned index,const wchar_t* kind){return transaction+L"\\root-"+std::to_wstring(index)+L"\\"+kind;}
void transition_save(const std::wstring& transaction,const JsonObject& record){std::wstring sealed;transition_require(tcard_bridge::Seal(std::wstring(record.Stringify()),sealed),"Cannot protect transition journal");transition_write(transaction,journalName,tcard_asset::encode(sealed));}
JsonObject transition_read(const std::wstring& transaction){
    const auto raw=transition_raw(transaction+L"\\"+journalName);std::wstring plain;transition_require(tcard_bridge::Seal(tcard_asset::decode(raw),plain,true),"Transition journal authentication failed");struct WipeText{std::wstring& text;~WipeText(){SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));}} wipe{plain};auto record=JsonObject::Parse(plain);
    transition_require(record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Mode")==L"rotate"&&sync_string(record,L"Path")==transaction,"Invalid transition journal");const auto roots=record.GetNamedArray(L"Roots");transition_require(roots.Size()>0&&roots.Size()<=maxRoots,"Invalid transition root count");std::wstring previous;
    for(unsigned i=0;i<roots.Size();++i){const auto root=sync_string(roots.GetObjectAt(i),L"Path");transition_require(root==transition_path(root)&&(previous.empty()||previous<root)&&!transition_inside(root,transaction)&&!transition_inside(transaction,root),"Unsafe transition journal root");previous=root;}return record;
}
void transition_status(const JsonObject& record,Status& status){status.phase=sync_string(record,L"Phase");status.active=status.phase==L"complete";status.recovery=sync_string(record,L"Path");status.total=0;status.remaining=0;for(const auto& value:record.GetNamedArray(L"Roots")){const auto root=value.GetObject();for(const auto& file:root.GetNamedArray(L"Files")){++status.total;if(!file.GetObject().GetNamedBoolean(L"Staged",false))++status.remaining;}}}
struct Locks {
    std::unique_lock<std::recursive_mutex> guard{sync_mutex};std::vector<std::unique_ptr<SyncLock>> writers;std::vector<HANDLE> storage;
    explicit Locks(const JsonArray& roots){
        try{for(const auto& value:roots){const auto root=sync_string(value.GetObject(),L"Path");auto lock=std::make_unique<SyncLock>(root);transition_require(static_cast<bool>(*lock),"Transition writer is busy");writers.push_back(std::move(lock));}
            for(const auto& value:roots){const auto root=sync_string(value.GetObject(),L"Path");const auto name=L"Local\\TCard.Storage."+tcard_asset::hash(tcard_asset::encode(root));const auto handle=CreateMutexW(nullptr,FALSE,name.c_str());transition_require(handle!=nullptr,"Transition storage mutex unavailable");const auto wait=WaitForSingleObject(handle,15000);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED){CloseHandle(handle);throw std::runtime_error("Transition storage is busy");}storage.push_back(handle);}
        }catch(...){for(HANDLE handle:storage){ReleaseMutex(handle);CloseHandle(handle);}storage.clear();throw;}
    }
    ~Locks(){for(auto i=storage.rbegin();i!=storage.rend();++i){ReleaseMutex(*i);CloseHandle(*i);}}
};
void transition_match(const Vault& source,const Vault& expected){transition_require(source.keys.publicKey==expected.keys.publicKey&&source.keys.dataKey==expected.keys.dataKey&&source.generation==expected.generation,"Source key changed during transition");}
void transition_refresh(const std::wstring& transaction,JsonObject& record){
    auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){auto root=roots.GetObjectAt(i);const auto path=sync_string(root,L"Path");transition_require(transition_raw(path+L"\\"+requiredName)==Bytes{'1'},"Required storage control differs");Vault current,expected;transition_key(transition_raw(path+L"\\"+keyName),current);transition_key(transition_raw(transition_area(transaction,i,L"recovery")+L"\\"+keyName),expected);transition_match(current,expected);
        auto previous=root.GetNamedArray(L"Files");std::map<std::wstring,JsonObject> known;for(const auto& value:previous)known.emplace(sync_string(value.GetObject(),L"Path"),value.GetObject());JsonArray files;
        const auto currentFiles=transition_files(path);transition_native(path,current,currentFiles);for(const auto& relative:currentFiles){const auto raw=transition_raw(path+L"\\"+relative);const auto hash=tcard_asset::hash(raw);auto found=known.find(relative);if(found!=known.end()&&sync_string(found->second,L"Source")==hash)files.Append(found->second);else{auto plain=transition_open(current,raw);Wipe wipe{plain};JsonObject row;row.Insert(L"Path",JsonValue::CreateStringValue(relative));row.Insert(L"Source",JsonValue::CreateStringValue(hash));row.Insert(L"Logical",JsonValue::CreateStringValue(tcard_asset::hash(plain)));row.Insert(L"Staged",JsonValue::CreateBooleanValue(false));files.Append(row);}}
        root.Insert(L"Files",files);roots.SetAt(i,root);
    }size_t count=0;for(const auto& value:roots)count+=value.GetObject().GetNamedArray(L"Files").Size();transition_require(count<=maxFiles,"Combined transition inventory exceeds bounds");record.Insert(L"Roots",roots);transition_save(transaction,record);
}
void transition_stage(const std::wstring& transaction,JsonObject& record,size_t budget){
    auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){auto root=roots.GetObjectAt(i);const auto source=sync_string(root,L"Path"),candidate=transition_area(transaction,i,L"candidate");Vault keys;transition_key(transition_raw(transition_area(transaction,i,L"recovery")+L"\\"+keyName),keys);auto files=root.GetNamedArray(L"Files");
        for(unsigned j=0;j<files.Size()&&budget;++j){auto file=files.GetObjectAt(j);if(file.GetNamedBoolean(L"Staged",false))continue;const auto relative=sync_string(file,L"Path");const auto raw=transition_raw(source+L"\\"+relative);if(tcard_asset::hash(raw)!=sync_string(file,L"Source"))throw SourceChanged{};auto plain=transition_open(keys,raw);Wipe wipe{plain};transition_require(tcard_asset::hash(plain)==sync_string(file,L"Logical"),"Source logical hash differs");Bytes stored;
            transition_require(tcard_storage::Pack(transition_extended(candidate+L"\\"+relative),plain,stored),"Candidate nonce/key allocation failed");transition_write(candidate,relative,stored);Vault verify;transition_key(transition_raw(candidate+L"\\"+keyName),verify);auto checked=transition_open(verify,transition_raw(candidate+L"\\"+relative));Wipe checkedScope{checked};transition_require(checked==plain,"Candidate verification failed");file.Insert(L"Candidate",JsonValue::CreateStringValue(tcard_asset::hash(stored)));file.Insert(L"Staged",JsonValue::CreateBooleanValue(true));files.SetAt(j,file);root.Insert(L"Files",files);roots.SetAt(i,root);record.Insert(L"Roots",roots);transition_save(transaction,record);--budget;
        }
    }
}
void transition_barriers(const std::wstring& transaction,JsonObject& record){
    const auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){const auto root=sync_string(roots.GetObjectAt(i),L"Path");if(transition_exists(root+L"\\"+barrier)){transition_require(Pending(root)==transaction,"Another transition owns this root");continue;}JsonObject marker;marker.Insert(L"Version",JsonValue::CreateNumberValue(1));marker.Insert(L"Transaction",JsonValue::CreateStringValue(transaction));marker.Insert(L"Root",JsonValue::CreateStringValue(root));std::wstring sealed;transition_require(tcard_bridge::Seal(std::wstring(marker.Stringify()),sealed),"Transition barrier protection failed");transition_write(root,barrier,tcard_asset::encode(sealed));transition_progress(L"barrier",i); }
    record.Insert(L"Phase",JsonValue::CreateStringValue(L"prepared"));transition_save(transaction,record);
}
void transition_recovery(const std::wstring& transaction,JsonObject& record){
    const auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){const auto root=roots.GetObjectAt(i);const auto source=sync_string(root,L"Path"),recovery=transition_area(transaction,i,L"recovery");for(const auto& value:root.GetNamedArray(L"Files")){const auto row=value.GetObject();const auto relative=sync_string(row,L"Path");const auto raw=transition_raw(source+L"\\"+relative);transition_require(tcard_asset::hash(raw)==sync_string(row,L"Source"),"Source changed before recovery capture");transition_write(recovery,relative,raw);transition_progress(L"recovery",i);}transition_write(recovery,keyName,transition_raw(source+L"\\"+keyName));transition_write(recovery,requiredName,transition_raw(source+L"\\"+requiredName));}
    record.Insert(L"Phase",JsonValue::CreateStringValue(L"committing"));transition_save(transaction,record);
}
void transition_activate(const std::wstring& transaction,JsonObject& record){
    const auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){const auto root=roots.GetObjectAt(i);const auto path=sync_string(root,L"Path"),candidate=transition_area(transaction,i,L"candidate"),recovery=transition_area(transaction,i,L"recovery");Vault oldKeys,newKeys;transition_key(transition_raw(recovery+L"\\"+keyName),oldKeys);const auto nextKey=transition_raw(candidate+L"\\"+keyName);transition_key(nextKey,newKeys);
        for(const auto& value:root.GetNamedArray(L"Files")){const auto row=value.GetObject();const auto relative=sync_string(row,L"Path");const auto old=transition_raw(recovery+L"\\"+relative),next=transition_raw(candidate+L"\\"+relative);transition_require(tcard_asset::hash(old)==sync_string(row,L"Source")&&tcard_asset::hash(next)==sync_string(row,L"Candidate"),"Retained or staged ciphertext changed");auto before=transition_open(oldKeys,old),after=transition_open(newKeys,next);Wipe beforeScope{before};Wipe afterScope{after};transition_require(before==after&&tcard_asset::hash(after)==sync_string(row,L"Logical"),"Retained logical content differs");const auto current=transition_raw(path+L"\\"+relative);transition_require(current==old||current==next,"Active data changed during closed transition");if(current!=next)transition_write(path,relative,next);transition_progress(L"file",i);}
        const auto currentKey=transition_raw(path+L"\\"+keyName),oldKey=transition_raw(recovery+L"\\"+keyName);transition_require(currentKey==oldKey||currentKey==nextKey,"Active key changed during closed transition");if(currentKey!=nextKey)transition_write(path,keyName,nextKey);transition_progress(L"key",i);
        for(const auto& value:root.GetNamedArray(L"Files")){const auto row=value.GetObject();auto plain=transition_open(newKeys,transition_raw(path+L"\\"+sync_string(row,L"Path")));Wipe wipe{plain};transition_require(tcard_asset::hash(plain)==sync_string(row,L"Logical"),"Activated generation verification failed");}
    }record.Insert(L"Phase",JsonValue::CreateStringValue(L"committed"));transition_save(transaction,record);transition_progress(L"committed",0);
}

void transition_owners(const std::wstring& transaction,const JsonObject& record){
    for(const auto& value:record.GetNamedArray(L"Roots")){const auto path=sync_string(value.GetObject(),L"Path");transition_require(Pending(path)==transaction,"Closed transition barrier is missing or belongs to another transaction");transition_require(transition_raw(path+L"\\"+requiredName)==Bytes{'1'},"Closed transition required control differs");}
}
void transition_cleanup(const std::wstring& transaction,const JsonObject& record){
    // The complete decision predates cleanup. A released root may contain later valid edits.
    const auto roots=record.GetNamedArray(L"Roots");for(unsigned i=0;i<roots.Size();++i){const auto path=sync_string(roots.GetObjectAt(i),L"Path");if(!transition_exists(path+L"\\"+barrier))continue;
        transition_require(Pending(path)==transaction,"Cleanup barrier belongs to another transaction");Vault keys,expected;transition_key(transition_raw(path+L"\\"+keyName),keys);transition_key(transition_raw(transition_area(transaction,i,L"candidate")+L"\\"+keyName),expected);transition_match(keys,expected);
        transition_require(transition_raw(path+L"\\"+requiredName)==Bytes{'1'}&&DeleteFileW(transition_extended(path+L"\\"+barrier).c_str()),"Transition barrier release failed");transition_progress(L"release",i);}
}
void transition_finish(const std::wstring& transaction,JsonObject& record){
    transition_owners(transaction,record);const auto roots=record.GetNamedArray(L"Roots");
    for(unsigned i=0;i<roots.Size();++i){const auto root=roots.GetObjectAt(i);const auto path=sync_string(root,L"Path");Vault keys,expected;transition_key(transition_raw(path+L"\\"+keyName),keys);transition_key(transition_raw(transition_area(transaction,i,L"candidate")+L"\\"+keyName),expected);transition_match(keys,expected);for(const auto& value:root.GetNamedArray(L"Files")){const auto row=value.GetObject();auto plain=transition_open(keys,transition_raw(path+L"\\"+sync_string(row,L"Path")));Wipe wipe{plain};transition_require(tcard_asset::hash(plain)==sync_string(row,L"Logical"),"Final transition verification failed");}}
    record.Insert(L"Phase",JsonValue::CreateStringValue(L"complete"));transition_save(transaction,record);transition_progress(L"decision",0);transition_cleanup(transaction,record);
}

}
std::wstring Pending(const std::wstring& path){const auto root=transition_path(path);if(!transition_exists(root+L"\\"+barrier))return {};const auto bytes=transition_raw(root+L"\\"+barrier);std::wstring plain;transition_require(tcard_bridge::Seal(tcard_asset::decode(bytes),plain,true),"Transition barrier authentication failed");const auto record=JsonObject::Parse(plain);transition_require(record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Root")==root,"Transition barrier identity differs");const auto transaction=sync_string(record,L"Transaction");transition_require(transaction==transition_path(transaction)&&!transition_inside(root,transaction)&&!transition_inside(transaction,root),"Unsafe transition barrier path");return transaction;}
bool Begin(const std::wstring& path,const std::vector<std::wstring>& paths,const tcard_crypto::Keys& keys,Mode mode,Status& status){
    status={};try{transition_require(mode==Mode::Rotate,"Plaintext transition is not implemented");const auto transaction=transition_path(path);Workspace context(transaction);transition_require(!transition_exists(transaction),"Transition directory must be new");transition_require(!paths.empty()&&paths.size()<=maxRoots&&keys.publicKey[0]==4,"Invalid transition request");std::vector<std::wstring> ordered;for(const auto& root:paths)ordered.push_back(transition_path(root));std::sort(ordered.begin(),ordered.end());transition_require(std::adjacent_find(ordered.begin(),ordered.end())==ordered.end(),"Repeated transition root");
        JsonArray roots;for(const auto& root:ordered){transition_require(!transition_inside(root,transaction)&&!transition_inside(transaction,root),"Transaction must be outside encrypted roots");for(const auto& other:ordered)if(other!=root)transition_require(!transition_inside(root,other),"Nested transition roots are unsupported");transition_require(tcard_storage::Required(transition_extended(root))&&tcard_storage::Ready(transition_extended(root)),"Transition requires active encrypted storage");JsonObject row;row.Insert(L"Path",JsonValue::CreateStringValue(root));row.Insert(L"Files",JsonArray{});roots.Append(row);}Locks locks(roots);bool same=true;for(const auto& root:ordered)same=same&&tcard_storage::Matches(transition_extended(root),keys);if(same){status.phase=L"noop";status.active=true;return true;}transition_require(tcard_bridge::Protect(transition_extended(transaction)),"Cannot protect transition directory");
        JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Path",JsonValue::CreateStringValue(transaction));record.Insert(L"Mode",JsonValue::CreateStringValue(L"rotate"));record.Insert(L"Phase",JsonValue::CreateStringValue(L"staging"));record.Insert(L"Roots",roots);
        for(unsigned i=0;i<roots.Size();++i){const auto source=ordered[i],area=transaction+L"\\root-"+std::to_wstring(i),candidate=transition_area(transaction,i,L"candidate"),recovery=transition_area(transaction,i,L"recovery");transition_require(tcard_bridge::Protect(transition_extended(area))&&tcard_bridge::Protect(transition_extended(candidate))&&tcard_bridge::Protect(transition_extended(recovery)),"Cannot protect retained transition roots");transition_write(recovery,keyName,transition_raw(source+L"\\"+keyName));transition_write(recovery,requiredName,transition_raw(source+L"\\"+requiredName));std::string error;transition_require(tcard_storage::Enable(transition_extended(candidate),keys,error),"Cannot initialize candidate encrypted generation");}
        transition_save(transaction,record);transition_refresh(transaction,record);transition_status(record,status);return true;
    }catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Storage transition failed";return false;}
}
bool Advance(const std::wstring& path,size_t budget,Status& status){status={};try{transition_require(budget>0&&budget<=maxFiles,"Invalid transition work budget");const auto transaction=transition_path(path);Workspace context(transaction);auto record=transition_read(transaction);Locks locks(record.GetNamedArray(L"Roots"));transition_recover(transaction,record);record=transition_read(transaction);transition_require(sync_string(record,L"Phase")==L"staging","Transition is already closed for commit");try{transition_stage(transaction,record,budget);}catch(const SourceChanged&){transition_refresh(transaction,record);}transition_status(record,status);return true;}catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Storage transition failed";return false;}}
bool Commit(const std::wstring& path,Status& status,const Progress& progress){status={};struct Observe{const Progress* previous;explicit Observe(const Progress& value):previous(observer){observer=&value;}~Observe(){observer=previous;}} observe{progress};try{const auto transaction=transition_path(path);Workspace context(transaction);auto record=transition_read(transaction);Locks locks(record.GetNamedArray(L"Roots"));transition_recover(transaction,record);record=transition_read(transaction);auto phase=sync_string(record,L"Phase");if(phase==L"complete"){transition_cleanup(transaction,record);transition_status(record,status);return true;}if(phase==L"staging"||phase==L"prepared"){transition_refresh(transaction,record);transition_stage(transaction,record,maxFiles*maxRoots);transition_barriers(transaction,record);transition_recovery(transaction,record);phase=L"committing";}if(phase==L"committing"){transition_owners(transaction,record);transition_activate(transaction,record);phase=L"committed";}transition_require(phase==L"committed","Unknown transition phase");transition_finish(transaction,record);transition_status(record,status);return true;}catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Storage transition failed";return false;}}
bool Inspect(const std::wstring& path,Status& status){status={};try{transition_status(transition_read(transition_path(path)),status);return true;}catch(const std::exception& error){status.error=error.what();return false;}catch(...){status.error="Storage transition failed";return false;}}
}
