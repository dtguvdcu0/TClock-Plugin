#include <winsock2.h>
#include "card_storage.h"
#include "card_key_activation.h"
#include "card_native_root.h"
#include "card_native_commit.h"
#include <memory>
#include "card_sync_bridge.h"
#include "card_sync_store.h"
#include <algorithm>
#include <array>
#include <cwctype>
#pragma comment(lib, "windowsapp.lib") // Migration catalog uses the existing Windows JSON runtime.

namespace tcard_storage {
namespace {
using tcard_crypto::Bytes;
constexpr wchar_t keyName[]=L".tcard-vault.key";
constexpr wchar_t requiredName[]=L".tcard-vault.required";
constexpr wchar_t pendingName[]=L".tcard-vault.pending";
constexpr wchar_t stageName[]=L".tcard-vault.stage";
constexpr wchar_t transitionName[]=L".tcard-vault.transition";
constexpr wchar_t activationName[]=L".tcard-key.activation";
constexpr uint64_t maxAttempts=1000000;
struct State {
    tcard_crypto::Keys keys;
    std::array<uint8_t,16> generation{};
    uint64_t attempts=0;
};
struct Wipe {
    Bytes& bytes;
    ~Wipe(){if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());}
};
std::wstring store_resolve(const std::wstring& path)
{
    wchar_t buffer[32768]{};const auto count=GetFullPathNameW(path.c_str(),32768,buffer,nullptr);
    if(!count||count>=32768)throw std::runtime_error("Invalid encrypted storage path");
    std::wstring result=buffer;while(result.size()>3&&result.back()==L'\\')result.pop_back();return result;
}
bool store_exists(const std::wstring& path)
{
    const auto attributes=GetFileAttributesW(tcard_native::Extended(path).c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES)return true;
    const auto code=GetLastError();if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)return false;
    throw std::runtime_error("Encrypted storage control unavailable");
}
std::wstring store_find_root(const std::wstring& path)
{
    auto current=store_resolve(path);
    for(;;){
        if(store_exists(current+L"\\"+requiredName)||store_exists(current+L"\\"+keyName)||store_exists(current+L"\\"+pendingName)||store_exists(current+L"\\"+transitionName)||store_exists(current+L"\\"+activationName))return current;
        const auto slash=current.find_last_of(L'\\');if(slash==std::wstring::npos||current.size()<=3)return {};
        current.resize(slash==2?3:slash);
    }
}
struct Lock {
    tcard_asset::File handle;
    explicit Lock(const std::wstring& root):handle([&]{
        auto normalized=store_resolve(root);
        // Extended DOS/UNC paths must share the original writer mutex and nonce counter.
        if(normalized.rfind(L"\\\\?\\UNC\\",0)==0)normalized=L"\\\\"+normalized.substr(8);
        else if(normalized.rfind(L"\\\\?\\",0)==0)normalized.erase(0,4);
        for(auto& ch:normalized)ch=static_cast<wchar_t>(towlower(ch));
        const auto name=L"Local\\TCard.Storage."+tcard_asset::hash(tcard_asset::encode(normalized));
        return CreateMutexW(nullptr,FALSE,name.c_str());
    }()){
        if(!handle.value||handle.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Storage mutex unavailable");
        const auto result=WaitForSingleObject(handle.value,15000);
        if(result!=WAIT_OBJECT_0&&result!=WAIT_ABANDONED)throw std::runtime_error("Storage is busy");
    }
    ~Lock(){ReleaseMutex(handle.value);}
};
bool store_read_raw(const std::wstring& path,Bytes& bytes)
{
    // Extend concrete I/O only; catalog paths, root identities and nonce domains stay unchanged.
    bytes.clear();tcard_asset::File file(CreateFileW(tcard_native::Extended(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file||!tcard_asset::regular(file.value,false))return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.value,&info)||info.nNumberOfLinks!=1||!GetFileSizeEx(file.value,&size)||size.QuadPart<0||size.QuadPart>tcard_asset::max_file)return false;
    bytes.resize(static_cast<size_t>(size.QuadPart));DWORD read=0;
    if(!bytes.empty()&&(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())){bytes.clear();return false;}
    return true;
}
bool store_write_raw(const std::wstring& path,const Bytes& bytes)
{
    const auto slash=path.find_last_of(L'\\');if(slash==std::wstring::npos||bytes.size()>tcard_asset::max_file)return false;
    tcard_asset::File directory(tcard_asset::open_dir(tcard_native::Extended(path.substr(0,slash)),false));if(!directory)return false;
    const auto attrs=GetFileAttributesW(tcard_native::Extended(path).c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES){
        tcard_asset::File existing(CreateFileW(tcard_native::Extended(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if(!existing||!tcard_asset::regular(existing.value,false)||!GetFileInformationByHandle(existing.value,&info)||info.nNumberOfLinks!=1)return false;
    }
    auto root=store_find_root(path);if(root.empty())root=path.substr(0,slash);
    const auto temp=root+L"\\"+stageName;bool okay=false;
    // A single reserved stage is serialized by the root mutex. Discard only its regular one-link orphan.
    if(store_exists(temp)){
        Bytes orphan;Wipe wipe{orphan};if(!store_read_raw(temp,orphan)||!DeleteFileW(tcard_native::Extended(temp).c_str()))return false;
    }
    {
        tcard_asset::File file(CreateFileW(tcard_native::Extended(temp).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!file)return false;DWORD count=0;
        okay=(bytes.empty()||(WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size()))&&FlushFileBuffers(file.value);
    }
    if(okay)okay=MoveFileExW(tcard_native::Extended(temp).c_str(),tcard_native::Extended(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!okay)DeleteFileW(tcard_native::Extended(temp).c_str());return okay;
}
bool store_access_key(const std::wstring& root,State& state,bool writing)
{
    struct CachedKey {
        std::wstring root;Bytes protectedBytes;std::array<BYTE,125> plain{};
        ~CachedKey(){SecureZeroMemory(plain.data(),plain.size());}
    };
    // Compare the actual protected control bytes on every access, never file timestamps.
    // Missing/replaced/locked controls still fail closed; no plaintext cache is persisted.
    static thread_local std::array<CachedKey,4> cache;
    static thread_local size_t next=0;
    const auto decode=[&](const BYTE* plain,size_t size){
        if(size!=125||memcmp(plain,"TCV1",4)!=0)return false;
        std::copy_n(plain+4,65,state.keys.publicKey.begin());std::copy_n(plain+69,32,state.keys.dataKey.begin());
        std::copy_n(plain+101,16,state.generation.begin());state.attempts=0;
        for(unsigned i=0;i<8;++i)state.attempts|=static_cast<uint64_t>(plain[117+i])<<(i*8);
        return state.attempts<=maxAttempts&&state.keys.publicKey[0]==4&&
            std::any_of(state.generation.begin(),state.generation.end(),[](uint8_t v){return v!=0;});
    };
    Bytes bytes;Wipe wipe{bytes};const wchar_t domain[]=L"TCard.NativeStorage.v1";
    DATA_BLOB entropy{sizeof(domain),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(domain))};
    if(writing){
        bytes={'T','C','V','1'};
        bytes.insert(bytes.end(),state.keys.publicKey.begin(),state.keys.publicKey.end());
        bytes.insert(bytes.end(),state.keys.dataKey.begin(),state.keys.dataKey.end());
        bytes.insert(bytes.end(),state.generation.begin(),state.generation.end());
        for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<uint8_t>(state.attempts>>(i*8)));
    }else if(!store_read_raw(root+L"\\"+keyName,bytes))return false;
    if(!writing)for(const auto& entry:cache)if(entry.root==root&&entry.protectedBytes==bytes)return decode(entry.plain.data(),entry.plain.size());
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),bytes.data()},output{};
    const bool okay=(writing?CryptProtectData(&input,L"TCard native storage",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output):
        CryptUnprotectData(&input,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))!=FALSE;
    if(!okay)return false;
    struct Blob{DATA_BLOB data;~Blob(){SecureZeroMemory(data.pbData,data.cbData);LocalFree(data.pbData);}} scope{output};
    if(writing){if(!store_write_raw(root+L"\\"+keyName,Bytes(output.pbData,output.pbData+output.cbData)))return false;}
    else if(!decode(output.pbData,output.cbData))return false;
    auto found=std::find_if(cache.begin(),cache.end(),[&](const auto& entry){return entry.root==root;});
    auto& entry=found==cache.end()?cache[next++%cache.size()]:*found;
    SecureZeroMemory(entry.plain.data(),entry.plain.size());entry.root=root;
    entry.protectedBytes=writing?Bytes(output.pbData,output.pbData+output.cbData):bytes;
    std::copy_n(writing?bytes.data():output.pbData,entry.plain.size(),entry.plain.begin());return true;
}
bool store_is_envelope(const Bytes& bytes){return bytes.size()>=4&&memcmp(bytes.data(),"TCE2",4)==0;}
bool store_hash(const Bytes& bytes,std::array<uint8_t,32>& digest)
{
    const auto hex=tcard_asset::hash(bytes);if(hex.size()!=64)return false;
    auto digit=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};
    for(size_t i=0;i<32;++i)digest[i]=static_cast<uint8_t>((digit(hex[i*2])<<4)|digit(hex[i*2+1]));return true;
}
bool store_open(const State& state,const Bytes& stored,Bytes& plain)
{
    plain.clear();if(stored.size()<tcard_crypto::envelopeOverhead||!store_is_envelope(stored))return false;
    tcard_crypto::Context context;context.generation=state.generation;context.kind=5;
    std::copy_n(stored.begin()+85,32,context.object.begin());
    if(!tcard_crypto::Open(state.keys,context,stored,plain))return false;
    std::array<uint8_t,32> digest{};
    if(!store_hash(plain,digest)||digest!=context.object){if(!plain.empty())SecureZeroMemory(plain.data(),plain.size());plain.clear();return false;}return true;
}
bool store_seal(const std::wstring& root,State& state,const Bytes& plain,Bytes& stored)
{
    stored.clear();if(plain.size()>tcard_crypto::maxObject||state.attempts>=maxAttempts)return false;
    tcard_crypto::Context context;context.generation=state.generation;context.kind=5;
    if(!store_hash(plain,context.object))return false;
    // Reserve every attempt durably, including orphan ciphertext after failed publication.
    ++state.attempts;if(!store_access_key(root,state,true))return false;
    return tcard_crypto::Seal(state.keys,context,plain,stored);
}
void store_collect(const std::wstring& root,const std::wstring& current,std::vector<std::wstring>& files,unsigned depth=0)
{
    if(depth>32||files.size()>10000)throw std::runtime_error("Storage migration exceeds bounds");
    tcard_asset::File directory(tcard_asset::open_dir(tcard_native::Extended(current),false));if(!directory)throw std::runtime_error("Unsafe migration directory");
    WIN32_FIND_DATAW entry{};const auto search=FindFirstFileW(tcard_native::Extended(current+L"\\*").c_str(),&entry);
    if(search==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return;throw std::runtime_error("Cannot enumerate migration source");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} scope{search};
    do{
        const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Migration refuses reparse points");
        if(current==root&&(name==keyName||name==requiredName||name==pendingName||name==transitionName))continue;
        if(current==root&&name==stageName){
            if(!store_exists(root+L"\\"+keyName))throw std::runtime_error("Reserved migration stage needs recovery");
            Bytes orphan;Wipe wipe{orphan};if(!store_read_raw(current+L"\\"+name,orphan))throw std::runtime_error("Unsafe migration stage");
            continue;
        }
        const auto path=current+L"\\"+name;
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)store_collect(root,path,files,depth+1);
        else{files.push_back(path);if(files.size()>10000)throw std::runtime_error("Storage migration exceeds bounds");}
    }while(FindNextFileW(search,&entry));
    if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Migration enumeration failed");
}
}
bool Required(const std::wstring& path)
{
    try{return !store_find_root(path).empty();}catch(...){return true;}
}
bool Ready(const std::wstring& path)
{
    try{const auto root=store_find_root(path);if(root.empty())return true;State state;
        return !store_exists(root+L"\\"+activationName)&&!store_exists(root+L"\\"+transitionName)&&!store_exists(root+L"\\"+pendingName)&&store_exists(root+L"\\"+requiredName)&&store_access_key(root,state,false);
    }catch(...){return false;}
}
bool Pack(const std::wstring& path,const Bytes& plain,Bytes& stored)
{
    if(&plain==&stored)return false;stored.clear();
    try{const auto root=store_find_root(path);if(root.empty()){stored=plain;return true;}Lock lock(root);State state;
        if(store_exists(root+L"\\"+activationName)||store_exists(root+L"\\"+transitionName)||store_exists(root+L"\\"+pendingName)||!store_exists(root+L"\\"+requiredName)||!store_access_key(root,state,false))return false;
        return store_seal(root,state,plain,stored);
    }catch(...){stored.clear();return false;}
}
bool Unpack(const std::wstring& path,const Bytes& stored,Bytes& plain)
{
    if(&stored==&plain)return false;plain.clear();
    try{const auto root=store_find_root(path);if(root.empty()){plain=stored;return true;}State state;
        if(store_exists(root+L"\\"+activationName)||store_exists(root+L"\\"+transitionName)||store_exists(root+L"\\"+pendingName)||!store_exists(root+L"\\"+requiredName)||!store_access_key(root,state,false))return false;
        return store_open(state,stored,plain);
    }catch(...){plain.clear();return false;}
}
bool Enable(const std::wstring& path,const tcard_crypto::Keys& keys,std::string& error)
{
    error.clear();try{
        const auto root=store_resolve(path);
        std::lock_guard nativeGuard(tcard_sync::sync_mutex);tcard_sync::SyncLock nativeLock(root);
        if(!nativeLock)throw std::runtime_error("Native writer lock unavailable");Lock lock(root);
        if(store_exists(root+L"\\"+activationName)||store_exists(root+L"\\"+transitionName))throw std::runtime_error("Coordinated storage transition requires recovery");
        if(!tcard_bridge::Protect(tcard_native::Extended(root)))throw std::runtime_error("Private native storage unavailable");
        const auto existing=store_find_root(root);if(!existing.empty()&&existing!=root)throw std::runtime_error("Nested encrypted roots are not supported");
        State state;const bool hasKey=store_exists(root+L"\\"+keyName);
        if(hasKey){
            if(!store_access_key(root,state,false)||state.keys.publicKey!=keys.publicKey||state.keys.dataKey!=keys.dataKey)
                throw std::runtime_error("Storage key mismatch; explicit rotation is required");
        }else{
            if(store_exists(root+L"\\"+requiredName)||store_exists(root+L"\\"+pendingName))throw std::runtime_error("Required storage key is missing");
            state.keys.publicKey=keys.publicKey;state.keys.dataKey=keys.dataKey;
            if(keys.publicKey[0]!=4||BCryptGenRandom(nullptr,state.generation.data(),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
                throw std::runtime_error("Cannot initialize native storage key");
        }
        const bool pending=store_exists(root+L"\\"+pendingName);
        const bool active=store_exists(root+L"\\"+requiredName)&&!pending;
        std::vector<std::wstring> files;store_collect(root,root,files);std::sort(files.begin(),files.end());
        using namespace winrt::Windows::Data::Json;
        JsonObject catalog;size_t total=0;
        if(pending){
            Bytes raw,plain;Wipe wipe{plain};
            if(!store_read_raw(root+L"\\"+pendingName,raw)||!store_open(state,raw,plain)||
                !JsonObject::TryParse(tcard_asset::decode(plain),catalog)||catalog.Size()!=files.size())
                throw std::runtime_error("Migration source catalog unavailable or inventory changed");
        }
        // Bind every source path and logical hash before any content replacement. Original TCE2 bytes are ordinary input.
        for(const auto& file:files){
            Bytes raw,plain;Wipe rawScope{raw};Wipe wipe{plain};
            if(!store_read_raw(file,raw))throw std::runtime_error("Unreadable native migration source");
            const auto leaf=file.substr(root.size()+1);
            if(active){if(!store_open(state,raw,plain))throw std::runtime_error("Native ciphertext authentication failed");}
            else if(pending){
                if(!catalog.HasKey(leaf))throw std::runtime_error("Migration source inventory changed");
                const auto hash=catalog.GetNamedString(leaf);
                if(tcard_asset::hash(raw)==hash)plain=raw;
                else if(!store_open(state,raw,plain)||tcard_asset::hash(plain)!=hash)throw std::runtime_error("Migration source changed");
            }else{plain=raw;catalog.Insert(leaf,JsonValue::CreateStringValue(tcard_asset::hash(plain)));}
            if(plain.size()>tcard_crypto::maxObject)throw std::runtime_error("Oversized native migration source");
            total+=plain.size();if(total>128*1024*1024)throw std::runtime_error("Native migration exceeds total bound");
        }
        if(active)return true;
        if(!hasKey&&!store_access_key(root,state,true))throw std::runtime_error("Cannot protect native storage key");
        if(!pending){
            Bytes plain=tcard_asset::encode(catalog.Stringify().c_str()),sealed;Wipe wipe{plain};
            if(!store_seal(root,state,plain,sealed)||!store_write_raw(root+L"\\"+pendingName,sealed))throw std::runtime_error("Cannot journal native migration");
        }
        if(!store_exists(root+L"\\"+requiredName)&&!store_write_raw(root+L"\\"+requiredName,Bytes{'1'}))throw std::runtime_error("Cannot require native encryption");
        for(const auto& file:files){
            Bytes raw,plain,sealed,verified;Wipe rawScope{raw};Wipe wipe{plain};Wipe verifiedScope{verified};
            if(!store_read_raw(file,raw))throw std::runtime_error("Native migration source changed");
            const auto hash=catalog.GetNamedString(file.substr(root.size()+1));
            if(tcard_asset::hash(raw)!=hash){
                if(!store_open(state,raw,plain)||tcard_asset::hash(plain)!=hash)throw std::runtime_error("Native migration source changed");
                continue;
            }
            plain=raw;
            if(!store_seal(root,state,plain,sealed)||!store_open(state,sealed,verified)||verified!=plain||!store_write_raw(file,sealed))
                throw std::runtime_error("Native migration publication failed");
        }
        files.clear();store_collect(root,root,files);
        if(files.size()!=catalog.Size())throw std::runtime_error("Final migration inventory changed");
        for(const auto& file:files){Bytes raw,plain;Wipe wipe{plain};const auto leaf=file.substr(root.size()+1);
            if(!catalog.HasKey(leaf)||!store_read_raw(file,raw)||!store_open(state,raw,plain)||tcard_asset::hash(plain)!=catalog.GetNamedString(leaf))
                throw std::runtime_error("Final native migration verification failed");}
        if(store_exists(root+L"\\"+stageName)&&!DeleteFileW(tcard_native::Extended(root+L"\\"+stageName).c_str()))throw std::runtime_error("Cannot clear migration stage");
        if(!DeleteFileW(tcard_native::Extended(root+L"\\"+pendingName).c_str()))throw std::runtime_error("Native migration activation failed");
        return true;
    }catch(const std::exception& failure){error=failure.what();return false;}catch(...){error="Native migration failed";return false;}
}
bool Resume(const std::wstring& path,std::string& error)
{
    try{const auto root=store_resolve(path);State state;
        if(store_exists(root+L"\\"+transitionName)){error="Coordinated storage transition requires recovery";return false;}
        if(!store_access_key(root,state,false)){error="Required storage key is missing";return false;}
        return Enable(root,state.keys,error);
    }catch(...){error="Cannot resume encrypted storage";return false;}
}
bool LoadKey(const std::wstring& path,tcard_crypto::Keys& keys)
{
    SecureZeroMemory(keys.privateKey.data(),keys.privateKey.size());
    SecureZeroMemory(keys.channelKey.data(),keys.channelKey.size());
    SecureZeroMemory(keys.dataKey.data(),keys.dataKey.size());keys.publicKey={};
    try{const auto root=store_find_root(path);State state;
        if(root.empty()||!Ready(root)||!store_access_key(root,state,false))return false;
        keys.publicKey=state.keys.publicKey;keys.dataKey=state.keys.dataKey;return true;
    }catch(...){return false;}
}
bool Matches(const std::wstring& path,const tcard_crypto::Keys& keys)
{
    try{const auto root=store_find_root(path);State state;
        return !root.empty()&&Ready(root)&&store_access_key(root,state,false)&&state.keys.publicKey==keys.publicKey&&state.keys.dataKey==keys.dataKey;
    }catch(...){return false;}
}

}

#include "card_key_activation.inc"
