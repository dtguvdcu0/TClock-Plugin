#include <winsock2.h>
#include "card_sync_cipher.h"
#include "card_sync_bridge.h"
#include "card_sync_store.h"

namespace tcard_cipher {
namespace {
using tcard_crypto::Bytes;
using tcard_sync::JsonObject;
using tcard_sync::JsonArray;
using tcard_sync::JsonValue;
struct Pointer { std::wstring generation,index,envelope; };
struct Entry { std::wstring id,hash,blob; uint8_t kind=0; };
struct Catalog {
    std::map<std::wstring,Entry> entries;
    std::map<std::wstring,std::wstring> heads;
    std::set<std::wstring> checked;
    bool snapshot=false;
};
struct Journal { Pointer base,next; uint64_t attempts=0; bool completed=false; };
struct Secret { tcard_crypto::Seed seed{}; ~Secret(){SecureZeroMemory(seed.data(),seed.size());} };
constexpr uint64_t emissionLimit=1000000;
void cstore_require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
bool cstore_hex(const std::wstring& value,size_t length)
{
    return value.size()==length&&value.find_first_not_of(L"0123456789abcdef")==std::wstring::npos;
}
std::wstring cstore_key(uint8_t kind,const std::wstring& id)
{
    cstore_require(kind>=1&&kind<=5&&cstore_hex(id,64)&&id!=std::wstring(64,L'0'),"Invalid logical object");
    return std::to_wstring(kind)+L":"+id;
}
bool cstore_equal(const Pointer& a,const Pointer& b){return a.generation==b.generation&&a.index==b.index&&a.envelope==b.envelope;}
JsonObject cstore_pack(const Pointer& pointer)
{
    JsonObject data;data.Insert(L"Generation",JsonValue::CreateStringValue(pointer.generation));
    data.Insert(L"Index",JsonValue::CreateStringValue(pointer.index));data.Insert(L"Envelope",JsonValue::CreateStringValue(pointer.envelope));return data;
}
Pointer cstore_unpack(const JsonObject& data,bool empty,bool staged=false)
{
    Pointer p{std::wstring(data.GetNamedString(L"Generation")),std::wstring(data.GetNamedString(L"Index")),std::wstring(data.GetNamedString(L"Envelope"))};
    if(empty&&p.generation.empty()&&p.index.empty()&&p.envelope.empty())return p;
    cstore_require(cstore_hex(p.generation,32)&&p.generation!=std::wstring(32,L'0'),"Invalid generation");
    cstore_require((staged&&p.index.empty()&&p.envelope.empty())||(cstore_hex(p.index,64)&&cstore_hex(p.envelope,64)),"Invalid catalog pointer");return p;
}
bool cstore_exists(const std::wstring& root,const wchar_t* name)
{
    if(GetFileAttributesW((root+L"\\"+name).c_str())!=INVALID_FILE_ATTRIBUTES)return true;
    cstore_require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"Store control unavailable");return false;
}
JsonObject cstore_control(const std::wstring& root,const wchar_t* name)
{
    JsonObject data;cstore_require(tcard_sync::sync_read(root,name,data)&&data.GetNamedNumber(L"Version")==2,"Invalid store control");return data;
}
Pointer cstore_active(const std::wstring& root)
{
    if(!cstore_exists(root,L"active.json"))return {};
    return cstore_unpack(cstore_control(root,L"active.json"),false);
}
Journal cstore_journal(const std::wstring& root)
{
    const auto data=cstore_control(root,L"journal.json");Journal journal;
    journal.base=cstore_unpack(data.GetNamedObject(L"Base"),true);journal.next=cstore_unpack(data.GetNamedObject(L"Next"),false,true);
    journal.attempts=tcard_sync::sync_number(data,L"Attempts");journal.completed=data.GetNamedBoolean(L"Completed");
    cstore_require(journal.attempts<=emissionLimit&&!cstore_equal(journal.base,journal.next),"Invalid rotation journal");return journal;
}
void cstore_save(const std::wstring& root,const Journal& journal)
{
    JsonObject data;data.Insert(L"Version",JsonValue::CreateNumberValue(2));data.Insert(L"Base",cstore_pack(journal.base));data.Insert(L"Next",cstore_pack(journal.next));
    data.Insert(L"Attempts",JsonValue::CreateStringValue(std::to_wstring(journal.attempts)));data.Insert(L"Completed",JsonValue::CreateBooleanValue(journal.completed));
    cstore_require(tcard_sync::sync_write(root,L"journal.json",data),"Rotation progress publication failed");
}
std::wstring cstore_directory(const std::wstring& root,const Pointer& pointer)
{
    cstore_require(cstore_hex(pointer.generation,32),"Invalid generation directory");return root+L"\\generations\\"+pointer.generation;
}
tcard_crypto::Context cstore_context(const Pointer& pointer,const std::wstring& id,uint8_t kind)
{
    cstore_key(kind,id);tcard_crypto::Context context;context.kind=kind;
    auto decode=[](const std::wstring& text,auto& out){auto digit=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};
        for(size_t i=0;i<out.size();++i)out[i]=static_cast<uint8_t>((digit(text[i*2])<<4)|digit(text[i*2+1]));};
    decode(pointer.generation,context.generation);decode(id,context.object);return context;
}
void cstore_derive(const std::wstring& root,const Pointer& pointer,tcard_crypto::Keys& keys)
{
    Bytes sealed;cstore_require(tcard_asset::read(cstore_directory(root,pointer),L"seed.bin",sealed,65536),"Protected old/candidate seed unavailable");
    auto entropy=tcard_asset::encode(L"TCard.CipherSeed.v2:"+pointer.generation);
    DATA_BLOB input{static_cast<DWORD>(sealed.size()),sealed.data()},domain{static_cast<DWORD>(entropy.size()),entropy.data()},output{};
    cstore_require(CryptUnprotectData(&input,nullptr,&domain,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)!=FALSE,"Protected seed authentication failed");
    struct Free{DATA_BLOB& blob;~Free(){SecureZeroMemory(blob.pbData,blob.cbData);LocalFree(blob.pbData);}} free{output};
    cstore_require(output.cbData==28,"Invalid protected seed length");Secret secret;std::copy_n(output.pbData,28,secret.seed.begin());
    cstore_require(tcard_crypto::Derive(secret.seed,keys),"Stored key derivation failed");
}
void cstore_seed(const std::wstring& root,const Pointer& pointer,const tcard_crypto::Seed& seed,const tcard_crypto::Keys& expected)
{
    auto entropy=tcard_asset::encode(L"TCard.CipherSeed.v2:"+pointer.generation);
    Secret secret;secret.seed=seed;DATA_BLOB input{28,secret.seed.data()},domain{static_cast<DWORD>(entropy.size()),entropy.data()},output{};
    cstore_require(CryptProtectData(&input,L"TCard encrypted generation",&domain,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)!=FALSE,"Candidate seed protection failed");
    struct Free{DATA_BLOB& blob;~Free(){SecureZeroMemory(blob.pbData,blob.cbData);LocalFree(blob.pbData);}} free{output};
    cstore_require(tcard_asset::publish(cstore_directory(root,pointer),L"seed.bin",Bytes(output.pbData,output.pbData+output.cbData),true),"Candidate seed publication failed");
    tcard_crypto::Keys verified;cstore_derive(root,pointer,verified);cstore_require(verified.publicKey==expected.publicKey&&verified.dataKey==expected.dataKey,"Candidate seed verification failed");
}
Bytes cstore_open(const std::wstring& root,const Pointer& pointer,const tcard_crypto::Keys& keys,const Entry& entry)
{
    Bytes envelope,plain;cstore_require(cstore_hex(entry.blob,64)&&tcard_asset::read(cstore_directory(root,pointer),entry.blob+L".bin",envelope)&&
        tcard_asset::hash(envelope)==entry.blob,"Encrypted object missing or damaged");
    cstore_require(tcard_crypto::Open(keys,cstore_context(pointer,entry.id,entry.kind),envelope,plain)&&tcard_asset::hash(plain)==entry.hash,"Encrypted object authentication failed");return plain;
}
Catalog cstore_load(const std::wstring& root,const Pointer& pointer,const tcard_crypto::Keys& keys,const Pointer& base,bool candidate)
{
    Catalog catalog;if(pointer.index.empty())return catalog;
    const auto plain=cstore_open(root,pointer,keys,{pointer.index,pointer.index,pointer.envelope,5});
    const auto data=JsonObject::Parse(tcard_asset::decode(plain));
    cstore_require(tcard_asset::encode(std::wstring(data.Stringify())).size()<=tcard_crypto::maxObject&&data.GetNamedNumber(L"Version")==2,"Invalid encrypted catalog");
    if(candidate)cstore_require(cstore_equal(cstore_unpack(data.GetNamedObject(L"Base"),true),base),"Catalog belongs to another rotation");
    catalog.snapshot=data.GetNamedBoolean(L"Snapshot");const auto entries=data.GetNamedArray(L"Entries");
    cstore_require(entries.Size()<=maxEntries,"Encrypted catalog entry limit");
    for(unsigned i=0;i<entries.Size();++i){const auto item=entries.GetObjectAt(i);const double k=item.GetNamedNumber(L"Kind");
        cstore_require(k>=1&&k<=5&&k==static_cast<int>(k),"Invalid object kind");
        Entry entry{std::wstring(item.GetNamedString(L"Id")),std::wstring(item.GetNamedString(L"Hash")),std::wstring(item.GetNamedString(L"Blob")),static_cast<uint8_t>(k)};
        cstore_require(cstore_hex(entry.hash,64)&&cstore_hex(entry.blob,64)&&catalog.entries.emplace(cstore_key(entry.kind,entry.id),entry).second,"Invalid duplicate encrypted object");}
    const auto heads=data.GetNamedArray(L"Heads");cstore_require(heads.Size()<=maxEntries,"Snapshot name limit");
    for(unsigned i=0;i<heads.Size();++i){const auto item=heads.GetObjectAt(i);const std::wstring name(item.GetNamedString(L"Name")),key(item.GetNamedString(L"Key"));
        cstore_require(!name.empty()&&name.size()<=1024&&catalog.entries.count(key)&&catalog.heads.emplace(name,key).second,"Invalid encrypted snapshot name");}
    const auto checked=data.GetNamedArray(L"Checked");cstore_require(checked.Size()<=maxEntries,"Rotation progress limit");
    for(unsigned i=0;i<checked.Size();++i){const std::wstring key(checked.GetStringAt(i));cstore_require(catalog.entries.count(key)&&catalog.checked.insert(key).second,"Invalid rotation progress");}
    return catalog;
}
Entry cstore_seal(const std::wstring& root,Journal& journal,const tcard_crypto::Keys& keys,const std::wstring& id,uint8_t kind,const Bytes& plain)
{
    cstore_require(plain.size()<=tcard_crypto::maxObject&&journal.attempts<emissionLimit,"Encrypted generation emission/size limit");
    // Reserve before emitting even an orphan. Never reset this persisted generation budget.
    ++journal.attempts;cstore_save(root,journal);Bytes envelope;
    cstore_require(tcard_crypto::Seal(keys,cstore_context(journal.next,id,kind),plain,envelope),"Object encryption failed");
    Entry entry{id,tcard_asset::hash(plain),tcard_asset::hash(envelope),kind};
    cstore_require(tcard_asset::publish(cstore_directory(root,journal.next),entry.blob+L".bin",envelope,true)&&cstore_open(root,journal.next,keys,entry)==plain,"Encrypted object publication verification failed");return entry;
}
void cstore_checkpoint(const std::wstring& root,Journal& journal,const tcard_crypto::Keys& keys,const Catalog& catalog)
{
    JsonObject data;data.Insert(L"Version",JsonValue::CreateNumberValue(2));data.Insert(L"Base",cstore_pack(journal.base));data.Insert(L"Snapshot",JsonValue::CreateBooleanValue(catalog.snapshot));
    JsonArray entries,heads,checked;
    for(const auto& [key,entry]:catalog.entries){(void)key;JsonObject item;item.Insert(L"Id",JsonValue::CreateStringValue(entry.id));item.Insert(L"Hash",JsonValue::CreateStringValue(entry.hash));
        item.Insert(L"Blob",JsonValue::CreateStringValue(entry.blob));item.Insert(L"Kind",JsonValue::CreateNumberValue(entry.kind));entries.Append(item);}
    for(const auto& [name,key]:catalog.heads){JsonObject item;item.Insert(L"Name",JsonValue::CreateStringValue(name));item.Insert(L"Key",JsonValue::CreateStringValue(key));heads.Append(item);}
    for(const auto& key:catalog.checked)checked.Append(JsonValue::CreateStringValue(key));
    data.Insert(L"Entries",entries);data.Insert(L"Heads",heads);data.Insert(L"Checked",checked);
    const auto plain=tcard_asset::encode(std::wstring(data.Stringify()));const auto entry=cstore_seal(root,journal,keys,tcard_asset::hash(plain),5,plain);
    journal.next.index=entry.id;journal.next.envelope=entry.blob;cstore_save(root,journal);
    cstore_load(root,journal.next,keys,journal.base,true);
}
void cstore_verify(const std::wstring& root,const Pointer& pointer,const tcard_crypto::Keys& keys,const Catalog& catalog)
{
    for(const auto& [key,entry]:catalog.entries){(void)key;auto plain=cstore_open(root,pointer,keys,entry);SecureZeroMemory(plain.data(),plain.size());}
}
void cstore_validate(const std::vector<Object>& objects)
{
    cstore_require(objects.size()<=maxEntries,"Snapshot entry limit");size_t total=0;std::set<std::wstring> names;std::map<std::wstring,std::wstring> hashes;
    for(const auto& object:objects){const auto key=cstore_key(object.kind,object.id);cstore_require(!object.name.empty()&&object.name.size()<=1024&&names.insert(object.name).second,"Invalid duplicate snapshot name");
        cstore_require(object.bytes.size()<=tcard_crypto::maxObject,"Snapshot object size limit");total+=object.bytes.size();cstore_require(total<=128*1024*1024,"Snapshot memory limit");
        const auto hash=tcard_asset::hash(object.bytes);const auto found=hashes.find(key);cstore_require(found==hashes.end()||found->second==hash,"Conflicting logical snapshot ID");hashes[key]=hash;}
}
}
bool Begin(const std::wstring& root,const tcard_crypto::Seed& candidate,Status& status)
{
    status={};try{std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(root);cstore_require(static_cast<bool>(lock),"Store lock unavailable");
        const bool existing=cstore_exists(root,L"store.json");
        if(!existing&&GetFileAttributesW(root.c_str())!=INVALID_FILE_ATTRIBUTES){
            tcard_asset::File checked(tcard_asset::open_dir(root,false));cstore_require(static_cast<bool>(checked),"Store root unavailable");
            WIN32_FIND_DATAW item{};HANDLE search=FindFirstFileW((root+L"\\*").c_str(),&item);
            if(search!=INVALID_HANDLE_VALUE){struct Close{HANDLE value;~Close(){FindClose(value);}} close{search};
                do{cstore_require(wcscmp(item.cFileName,L".")==0||wcscmp(item.cFileName,L"..")==0,"Unmarked store root must be empty; never use live cards");}while(FindNextFileW(search,&item));
                cstore_require(GetLastError()==ERROR_NO_MORE_FILES,"Store root enumeration failed");
            }else cstore_require(GetLastError()==ERROR_FILE_NOT_FOUND,"Store root enumeration unavailable");
        }
        cstore_require(tcard_bridge::Protect(root),"Private store unavailable");
        if(existing)cstore_require(std::wstring(cstore_control(root,L"store.json").GetNamedString(L"Role"))==L"TCardSync.Cipher","Unsupported private store role");
        else{JsonObject marker;marker.Insert(L"Version",JsonValue::CreateNumberValue(2));marker.Insert(L"Role",JsonValue::CreateStringValue(L"TCardSync.Cipher"));
            cstore_require(tcard_sync::sync_write(root,L"store.json",marker,true),"Private store marker failed");}
        tcard_asset::File rootGuard(tcard_asset::open_dir(root,false));cstore_require(static_cast<bool>(rootGuard),"Store directory unavailable");
        tcard_crypto::Keys keys;cstore_require(tcard_crypto::Derive(candidate,keys),"Candidate key derivation failed");const auto active=cstore_active(root);
        if(!active.generation.empty()){tcard_crypto::Keys oldKeys;cstore_derive(root,active,oldKeys);const auto old=cstore_load(root,active,oldKeys,{},false);cstore_verify(root,active,oldKeys,old);}
        if(cstore_exists(root,L"journal.json")){const auto previous=cstore_journal(root);cstore_require(previous.completed&&cstore_equal(previous.next,active),"Previous rotation is still pending");
            Bytes old;cstore_require(tcard_asset::read(root,L"journal.json",old)&&tcard_asset::publish(root,L"journal-"+tcard_asset::hash(old)+L".json",old,true),"Completed journal retention failed");}
        auto token=tcard_bridge::Token();token.erase(std::remove(token.begin(),token.end(),L'-'),token.end());for(auto& c:token)c=static_cast<wchar_t>(towlower(c));
        Journal journal;journal.base=active;journal.next.generation=token;
        cstore_require(tcard_bridge::Protect(root+L"\\generations")&&tcard_bridge::Protect(cstore_directory(root,journal.next)),"Generation directory unavailable");
        cstore_seed(root,journal.next,candidate,keys);cstore_save(root,journal);return true;
    }catch(const std::exception& e){status.error=e.what();return false;}catch(...){status.error="Invalid encrypted store";return false;}
}
bool Advance(const std::wstring& root,const std::vector<Object>* snapshot,size_t budget,bool activate,Status& status)
{
    status={};try{std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(root);cstore_require(static_cast<bool>(lock),"Store lock unavailable");
        tcard_asset::File rootGuard(tcard_asset::open_dir(root,false));cstore_require(static_cast<bool>(rootGuard),"Private store unavailable");
        if(snapshot)cstore_validate(*snapshot);cstore_require(budget<=maxEntries,"Rotation work limit");auto journal=cstore_journal(root);const auto active=cstore_active(root);
        tcard_crypto::Keys keys;cstore_derive(root,journal.next,keys);auto next=cstore_load(root,journal.next,keys,journal.base,true);
        if(cstore_equal(active,journal.next)){cstore_verify(root,journal.next,keys,next);journal.completed=true;cstore_save(root,journal);status.active=true;status.retained=next.entries.size();return true;}
        cstore_require(!journal.completed&&cstore_equal(active,journal.base),"Active generation changed during rotation");
        tcard_crypto::Keys oldKeys;Catalog base;
        if(!journal.base.generation.empty()){cstore_derive(root,journal.base,oldKeys);base=cstore_load(root,journal.base,oldKeys,{},false);}
        for(const auto& key:next.checked)cstore_require(base.entries.count(key),"Rotation progress is not in base catalog");
        if(snapshot){next.heads.clear();next.snapshot=true;
            for(const auto& object:*snapshot){const auto key=cstore_key(object.kind,object.id),hash=tcard_asset::hash(object.bytes);const auto found=next.entries.find(key);
                const auto previous=base.entries.find(key);cstore_require(previous==base.entries.end()||previous->second.hash==hash,"Snapshot changed an old logical ID");
                if(found!=next.entries.end())cstore_require(found->second.hash==hash,"Snapshot changed a logical ID");
                else{cstore_require(next.entries.size()<maxEntries,"Retained entry limit");next.entries.emplace(key,cstore_seal(root,journal,keys,object.id,object.kind,object.bytes));}
                next.heads.emplace(object.name,key);}
            // Persist the complete accepted delta before limiting old-generation work.
            cstore_checkpoint(root,journal,keys,next);
        }
        size_t worked=0;
        for(const auto& [key,entry]:base.entries){if(next.checked.count(key)||worked>=budget)continue;
            auto plain=cstore_open(root,journal.base,oldKeys,entry);const auto found=next.entries.find(key);
            if(found!=next.entries.end())cstore_require(found->second.hash==entry.hash,"Rotation object ID conflict");
            else{cstore_require(next.entries.size()<maxEntries,"Retained entry limit");next.entries.emplace(key,cstore_seal(root,journal,keys,entry.id,entry.kind,plain));}
            SecureZeroMemory(plain.data(),plain.size());next.checked.insert(key);++worked;
            // Each checked old object is an independently resumable checkpoint.
            cstore_checkpoint(root,journal,keys,next);
        }
        status.retained=next.entries.size();status.remaining=base.entries.size()-next.checked.size();
        if(!activate||status.remaining)return true;
        if(!next.snapshot)next.heads=base.heads;
        cstore_checkpoint(root,journal,keys,next);
        // Recheck every old and candidate object at commit, including already checked objects.
        if(!journal.base.generation.empty())cstore_verify(root,journal.base,oldKeys,base);cstore_verify(root,journal.next,keys,next);
        auto pointer=cstore_pack(journal.next);pointer.Insert(L"Version",JsonValue::CreateNumberValue(2));
        cstore_require(tcard_sync::sync_write(root,L"active.json",pointer),"Active generation publication failed");
        journal.completed=true;cstore_save(root,journal);status.active=true;return true;
    }catch(const std::exception& e){status.error=e.what();return false;}catch(...){status.error="Invalid encrypted store";return false;}
}
bool Read(const std::wstring& root,const std::wstring& id,uint8_t kind,Bytes& bytes)
{
    bytes.clear();try{std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(root);cstore_require(static_cast<bool>(lock),"Store lock unavailable");
        tcard_asset::File rootGuard(tcard_asset::open_dir(root,false));cstore_require(static_cast<bool>(rootGuard),"Store unavailable");const auto pointer=cstore_active(root);
        tcard_crypto::Keys keys;cstore_derive(root,pointer,keys);const auto catalog=cstore_load(root,pointer,keys,{},false);const auto found=catalog.entries.find(cstore_key(kind,id));
        if(found==catalog.entries.end())return false;bytes=cstore_open(root,pointer,keys,found->second);return true;
    }catch(...){bytes.clear();return false;}
}
bool ReadHeads(const std::wstring& root,std::vector<Object>& snapshot)
{
    snapshot.clear();try{std::lock_guard guard(tcard_sync::sync_mutex);tcard_sync::SyncLock lock(root);cstore_require(static_cast<bool>(lock),"Store lock unavailable");
        tcard_asset::File rootGuard(tcard_asset::open_dir(root,false));cstore_require(static_cast<bool>(rootGuard),"Store unavailable");const auto pointer=cstore_active(root);
        tcard_crypto::Keys keys;cstore_derive(root,pointer,keys);const auto catalog=cstore_load(root,pointer,keys,{},false);std::vector<Object> verified;
        for(const auto& [name,key]:catalog.heads){const auto& entry=catalog.entries.at(key);verified.push_back({entry.id,name,entry.kind,cstore_open(root,pointer,keys,entry)});}
        snapshot=std::move(verified);return true;
    }catch(...){snapshot.clear();return false;}
}
}
