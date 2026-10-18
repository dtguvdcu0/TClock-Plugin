#pragma once
#include "card_sync_bridge.h"
#include "card_sync_selection.h"
#include "card_sync_bootstrap.h"

// Owner application receipts describe durable native materialization, never worker custody.
// Publish/PublishInventory accept verified committed snapshots/headers captured by the TCard owner.
namespace tcard_ack {
inline std::wstring Identity(const tcard_crypto::Keys& keys)
{
    return tcard_asset::hash({keys.publicKey.begin(),keys.publicKey.end()});
}
inline std::wstring Owner(HANDLE process,DWORD pid)
{
    FILETIME created{},exited{},kernel{},user{};
    if(!pid||!GetProcessTimes(process,&created,&exited,&kernel,&user))throw std::runtime_error("Application owner unavailable");
    const ULONGLONG epoch=(static_cast<ULONGLONG>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    return std::to_wstring(pid)+L":"+std::to_wstring(epoch);
}
inline tcard_sync::selection::Inventory Validate(const std::wstring& receipt,const std::wstring& identity,const std::wstring& owner)
{
    using namespace tcard_sync;
    if(tcard_asset::encode(receipt).size()>tcard_asset::max_file)throw std::runtime_error("Application receipt budget exceeded");
    const auto root=JsonObject::Parse(receipt);
    if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"owner-applied"||
        sync_string(root,L"Group")!=identity||owner.empty()||sync_string(root,L"Owner")!=owner)throw std::runtime_error("Application receipt identity differs");
    const auto header=std::wstring(root.GetNamedObject(L"Inventory").Stringify());
    if(sync_string(root,L"Digest")!=tcard_bridge::Hash(header))throw std::runtime_error("Application receipt digest differs");
    return selection::Read(header);
}
inline bool Read(const std::wstring& mailbox,const std::wstring& owner,tcard_sync::selection::Inventory& inventory,bool* batches=nullptr,bool* objects=nullptr,bool* atomic=nullptr,bool* dependencies=nullptr,std::wstring* node=nullptr)
{
    inventory={};if(batches)*batches=false;if(objects)*objects=false;if(atomic)*atomic=false;if(dependencies)*dependencies=false;if(node)node->clear();
    try{
        tcard_crypto::Keys keys;std::wstring receipt;
        if(!tcard_storage::LoadKey(mailbox,keys)||!tcard_bridge::Read(mailbox,L"applied.json",receipt))return false;
        inventory=Validate(receipt,Identity(keys),owner);const auto record=tcard_sync::JsonObject::Parse(receipt);
        const bool dependency=record.GetNamedBoolean(L"Dependencies",false);const auto native=std::wstring(record.GetNamedString(L"DependencyNode",L""));
        if(dependency&&!tcard_sync::sync_hex(native))throw std::runtime_error("Dependency owner node unavailable");
        if(dependencies)*dependencies=dependency;if(node&&dependency)*node=native;if(batches)*batches=tcard_sync::JsonObject::Parse(receipt).GetNamedBoolean(L"Batches",false);if(objects)*objects=tcard_sync::JsonObject::Parse(receipt).GetNamedBoolean(L"Objects",false);if(atomic)*atomic=tcard_sync::JsonObject::Parse(receipt).GetNamedBoolean(L"NativeAtomic",false);return true;
    }catch(...){inventory={};return false;}
}
inline bool PublishInventory(const std::wstring& mailbox,const std::wstring& storage,const std::wstring& header,bool batches=false,bool objects=false,bool atomic=false,bool dependencies=false)
{
    // Unencrypted legacy/headless callers get no receipt, rather than plaintext ACK metadata.
    if(!tcard_storage::Required(mailbox))return true;
    try{
        tcard_crypto::Keys keys,native;
        if(!tcard_storage::LoadKey(mailbox,keys)||!tcard_storage::LoadKey(storage,native)||keys.publicKey!=native.publicKey)return false;
        const auto identity=Identity(keys),owner=Owner(GetCurrentProcess(),GetCurrentProcessId());
        const auto expected=tcard_sync::selection::Read(header);const auto node=dependencies?tcard_bootstrap::Node(storage):L"";std::wstring retained;
        if(tcard_bridge::Read(mailbox,L"applied.json",retained)){
            try{if(Validate(retained,identity,owner)==expected&&tcard_sync::JsonObject::Parse(retained).GetNamedBoolean(L"Batches",false)==batches&&tcard_sync::JsonObject::Parse(retained).GetNamedBoolean(L"Objects",false)==objects&&tcard_sync::JsonObject::Parse(retained).GetNamedBoolean(L"NativeAtomic",false)==atomic&&tcard_sync::JsonObject::Parse(retained).GetNamedBoolean(L"Dependencies",false)==dependencies&&std::wstring(tcard_sync::JsonObject::Parse(retained).GetNamedString(L"DependencyNode",L""))==node)return true;}catch(...){}
        }
        using namespace tcard_sync;
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));
        root.Insert(L"Batches",JsonValue::CreateBooleanValue(batches));root.Insert(L"Objects",JsonValue::CreateBooleanValue(objects));
        root.Insert(L"Dependencies",JsonValue::CreateBooleanValue(dependencies));root.Insert(L"DependencyNode",JsonValue::CreateStringValue(node));
        root.Insert(L"NativeAtomic",JsonValue::CreateBooleanValue(atomic));root.Insert(L"Kind",JsonValue::CreateStringValue(L"owner-applied"));root.Insert(L"Group",JsonValue::CreateStringValue(identity));
        root.Insert(L"Owner",JsonValue::CreateStringValue(owner));
        root.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(header)));root.Insert(L"Inventory",JsonObject::Parse(header));
        return tcard_bridge::Publish(mailbox,L"applied.json",std::wstring(root.Stringify()));
    }catch(...){return false;}
}
inline bool Publish(const std::wstring& mailbox,const std::wstring& storage,const std::wstring& snapshot,bool batches=false,bool objects=false,bool atomic=false,bool dependencies=false)
{
    try{return PublishInventory(mailbox,storage,tcard_sync::selection::Header(snapshot),batches,objects,atomic,dependencies);}catch(...){return false;}
}
}
