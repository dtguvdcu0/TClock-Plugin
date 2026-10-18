#pragma once
#include "card_sync_store.h"
#include "card_sync_bridge.h"

// The public root owns encryption controls; only verified native data generations are selected.
namespace tcard_native {
using namespace tcard_sync;
inline std::wstring Base(const std::wstring& path)
{
    return path.size()>5&&_wcsicmp(path.c_str()+path.size()-5,L".json")==0?path.substr(0,path.size()-5):path;
}
inline std::wstring Owner(const std::wstring& path){return sync_owner(Base(path));}
inline std::wstring Extended(const std::wstring& path)
{
    if(path.starts_with(L"\\\\?\\"))return path;
    return path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
}
inline std::wstring Control(const std::wstring& owner){return Extended(Owner(owner))+L"\\_native-generations";}
inline bool Exists(const std::wstring& path)
{
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return true;
    const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;
    throw std::runtime_error("Native generation control unavailable");
}
inline bool Empty(const std::wstring& folder)
{
    tcard_asset::File checked(tcard_asset::open_dir(folder,false));if(!checked)throw std::runtime_error("Unsafe native control directory");
    WIN32_FIND_DATAW item{};const auto handle=FindFirstFileW((folder+L"\\*").c_str(),&item);
    if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return true;throw std::runtime_error("Native control enumeration failed");}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{const std::wstring name=item.cFileName;if(name!=L"."&&name!=L"..")return false;}while(FindNextFileW(handle,&item));
    if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Native control enumeration interrupted");return true;
}
inline void Check(const std::wstring& path)
{
    tcard_asset::File checked(tcard_asset::open_dir(path,false));if(!checked)throw std::runtime_error("Unsafe native generation");
    for(const auto* leaf:{L".tcard-vault.key",L".tcard-vault.required",L".tcard-vault.pending",L".tcard-vault.stage"})
        if(Exists(path+L"\\"+leaf))throw std::runtime_error("Nested native encryption controls");
}
inline std::wstring Selected(const std::wstring& owner)
{
    const auto root=Owner(owner),folder=Control(root);const bool required=Exists(root+L"\\.tcard-native.required");
    if(!Exists(folder)){if(required)throw std::runtime_error("Required native selector missing");return {};}
    Check(folder);
    if(!Exists(folder+L"\\active.json")){
        if(!required&&Empty(folder))return {};throw std::runtime_error("Native selector missing");
    }
    JsonObject record;if(!sync_read(folder,L"active.json",record)||record.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Native selector unreadable");
    const auto generation=sync_string(record,L"Generation");
    if(!generation.empty()&&(!required||!sync_hex(generation)))throw std::runtime_error("Invalid native generation identity");
    if(required){JsonObject marker;if(!sync_read(root,L".tcard-native.required",marker)||marker.GetNamedNumber(L"Version")!=1||sync_string(marker,L"Kind")!=L"native-selector")throw std::runtime_error("Native selector marker differs");
        const auto scope=std::wstring(marker.GetNamedString(L"Scope",L""));
        if(!scope.empty()){JsonObject data;const auto path=folder+L"\\"+generation+L"\\data";
            if(!sync_hex(scope)||generation.empty()||!sync_read(path,L".tcard-data.scope",data)||data.GetNamedNumber(L"Version")!=1||sync_string(data,L"Epoch")!=scope)
                throw std::runtime_error("Native selection belongs to a different replacement scope");}}
    return generation;
}
inline std::wstring Data(const std::wstring& owner,const std::wstring& generation)
{
    if(generation.empty())return Owner(owner);
    if(!sync_hex(generation))throw std::runtime_error("Invalid native generation");return Control(owner)+L"\\"+generation+L"\\data";
}
inline std::wstring Resolve(const std::wstring& path)
{
    // Private candidate/generation paths are concrete; only the stable public root is redirected.
    const auto base=Base(path);if(sync_owner(base)!=sync_path_plain(base))return base;
    const auto generation=Selected(base);if(generation.empty())return base;
    const auto data=Data(base,generation);Check(Control(base)+L"\\"+generation);Check(data);return data;
}
inline void Initialize(const std::wstring& owner)
{
    std::lock_guard guard(sync_mutex);const auto root=Owner(owner);SyncLock lock(root);
    if(!lock||!tcard_storage::Required(root)||!tcard_storage::Ready(root))throw std::runtime_error("Native selector owner unavailable");
    Selected(root);const auto folder=Control(root);
    if(!Exists(folder)&&!tcard_bridge::Protect(folder))throw std::runtime_error("Cannot protect native selector");
    if(!Exists(folder+L"\\active.json")){
        if(!Empty(folder))throw std::runtime_error("Unexpected native control contents");
        JsonObject initial;initial.Insert(L"Version",JsonValue::CreateNumberValue(1));initial.Insert(L"Generation",JsonValue::CreateStringValue(L""));
        if(!sync_write(folder,L"active.json",initial))throw std::runtime_error("Cannot initialize native selector");
    }
    if(!Exists(root+L"\\.tcard-native.required")){
        JsonObject marker;marker.Insert(L"Version",JsonValue::CreateNumberValue(1));marker.Insert(L"Kind",JsonValue::CreateStringValue(L"native-selector"));
        if(!sync_write(root,L".tcard-native.required",marker,true))throw std::runtime_error("Cannot retain native selector requirement");
    }
    Selected(root);
}
}
