#pragma once
#include "card_sync_transport.h"
#include "card_sync_ack.h"
#include <mutex>

namespace tcard_delivery {
using namespace tcard_sync;
using Inventory=selection::Inventory;
inline std::wstring Encode(const Inventory& inventory)
{
    std::map<std::wstring,std::pair<JsonArray,JsonArray>> rows;
    for(const auto& [note,id]:inventory.operations)rows[note].first.Append(JsonValue::CreateStringValue(id));
    for(const auto& [note,id]:inventory.assets)rows[note].second.Append(JsonValue::CreateStringValue(id));
    JsonObject root;JsonArray notes;root.Insert(L"Version",JsonValue::CreateNumberValue(1));
    for(const auto& [id,objects]:rows){JsonObject note;note.Insert(L"Id",JsonValue::CreateStringValue(id));
        note.Insert(L"Changes",objects.first);note.Insert(L"Assets",objects.second);notes.Append(note);}
    root.Insert(L"Notes",notes);const auto text=std::wstring(root.Stringify());selection::Read(text);return text;
}
inline Inventory Difference(const Inventory& required,const Inventory& known)
{
    Inventory result;
    for(const auto& item:required.operations)if(!known.operations.contains(item))result.operations.insert(item);
    for(const auto& item:required.assets)if(!known.assets.contains(item))result.assets.insert(item);
    return result;
}
inline bool Node(const std::wstring& value)
{
    if(value.size()!=36)return false;GUID guid{};const auto text=L"{"+value+L"}";
    return SUCCEEDED(CLSIDFromString(text.c_str(),&guid));
}
// A node identifies a protected same-group replica instance, not independent device trust.
// Bind it to the local directory and machine; copied roots retain data but get a new node.
inline std::wstring Binding(const std::wstring& root)
{
    tcard_asset::File directory(tcard_asset::open_dir(root,false));BY_HANDLE_FILE_INFORMATION info{};
    wchar_t computer[MAX_COMPUTERNAME_LENGTH+1]{};DWORD length=MAX_COMPUTERNAME_LENGTH+1;
    if(!directory||!GetFileInformationByHandle(directory.value,&info)||!GetComputerNameW(computer,&length))
        throw std::runtime_error("Delivery root identity unavailable");
    return tcard_bridge::Hash(std::wstring(computer,length)+L":"+sync_path(root)+L":"+
        std::to_wstring(info.dwVolumeSerialNumber)+L":"+std::to_wstring(info.nFileIndexHigh)+L":"+std::to_wstring(info.nFileIndexLow));
}
class Journal final:public PeerApplication {
    struct Entry {Inventory required,applied;std::wstring owner;bool verified=false;bool operator==(const Entry&)const=default;};
    std::wstring root_,mailbox_,owner_,group_,node_,file_,retained_;
    std::map<std::wstring,Entry> peers_;
    std::mutex mutex_;
    tcard_bridge::Stamp stamp_;ULONGLONG checked_=0;bool valid_=false;Inventory applied_;
    void CheckOwner()
    {
        if(!tcard_storage::Ready(mailbox_)){valid_=false;applied_={};checked_=0;return;}
        tcard_bridge::Stamp next;const auto now=GetTickCount64();
        if(!tcard_bridge::Fingerprint(mailbox_,L"applied.json",next)){valid_=false;applied_={};checked_=0;return;}
        if(!checked_||!(next==stamp_)||now-checked_>=60000){
            valid_=false;applied_={};std::wstring receipt;
            try{if(tcard_bridge::Read(mailbox_,L"applied.json",receipt)){applied_=tcard_ack::Validate(receipt,group_,owner_);valid_=true;}}catch(...){applied_={};}
            stamp_=next;checked_=now;
        }
    }
    bool Save()
    {
        if(!tcard_storage::Ready(root_))return false;
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));
        root.Insert(L"Group",JsonValue::CreateStringValue(group_));root.Insert(L"LocalNode",JsonValue::CreateStringValue(node_));JsonArray peers;
        for(const auto& [node,entry]:peers_){JsonObject peer;peer.Insert(L"Node",JsonValue::CreateStringValue(node));
            peer.Insert(L"Required",JsonObject::Parse(Encode(entry.required)));peer.Insert(L"Applied",JsonObject::Parse(Encode(entry.applied)));
            peer.Insert(L"Owner",JsonValue::CreateStringValue(entry.owner));peer.Insert(L"Verified",JsonValue::CreateBooleanValue(entry.verified));peers.Append(peer);}
        root.Insert(L"Peers",peers);const auto text=std::wstring(root.Stringify());
        if(text==retained_)return true;
        if(!tcard_bridge::Publish(root_,file_,text))return false;retained_=text;return true;
    }
public:
    Journal(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& owner):root_(storage+L"\\_delivery"),mailbox_(mailbox),owner_(owner)
    {
        tcard_crypto::Keys keys;
        struct Wipe{tcard_crypto::Keys& keys;~Wipe(){SecureZeroMemory(keys.dataKey.data(),keys.dataKey.size());}} wipe{keys};
        if(owner.empty()||!tcard_storage::Required(storage)||!tcard_storage::LoadKey(storage,keys)||!tcard_bridge::Protect(root_))
            throw std::runtime_error("Encrypted delivery journal unavailable");
        group_=tcard_ack::Identity(keys);const auto binding=Binding(storage);JsonObject identity;std::wstring old;
        if(sync_exists(root_,L"node.json")){
            if(!tcard_bridge::Read(root_,L"node.json",old)||!JsonObject::TryParse(old,identity)||identity.GetNamedNumber(L"Version")!=1||
                sync_string(identity,L"Group")!=group_||!Node(sync_string(identity,L"Node")))throw std::runtime_error("Invalid delivery node");
        }
        if(!identity.HasKey(L"Node")||sync_string(identity,L"Binding")!=binding){
            node_=tcard_bridge::Token();identity=JsonObject{};identity.Insert(L"Version",JsonValue::CreateNumberValue(1));
            identity.Insert(L"Group",JsonValue::CreateStringValue(group_));identity.Insert(L"Node",JsonValue::CreateStringValue(node_));
            identity.Insert(L"Binding",JsonValue::CreateStringValue(binding));
            if(!tcard_bridge::Publish(root_,L"node.json",std::wstring(identity.Stringify())))throw std::runtime_error("Cannot retain delivery node");
        }else node_=sync_string(identity,L"Node");
        // A cloned node starts a separate ledger; never delete the original offline intent.
        file_=L"peers-"+node_+L".json";
        if(sync_exists(root_,file_)){
            if(!tcard_bridge::Read(root_,file_,retained_))throw std::runtime_error("Unreadable delivery ledger");
            const auto saved=JsonObject::Parse(retained_);
            if(saved.GetNamedNumber(L"Version")!=1||sync_string(saved,L"Group")!=group_||sync_string(saved,L"LocalNode")!=node_)
                throw std::runtime_error("Delivery ledger identity differs");
            const auto peers=saved.GetNamedArray(L"Peers");if(peers.Size()>128)throw std::runtime_error("Delivery peer budget exceeded");
            for(const auto& item:peers){const auto peer=item.GetObject();const auto node=sync_string(peer,L"Node");
                if(!Node(node)||node==node_||peers_.contains(node))throw std::runtime_error("Invalid delivery peer identity");
                Entry entry;entry.required=selection::Read(std::wstring(peer.GetNamedObject(L"Required").Stringify()));
                entry.applied=selection::Read(std::wstring(peer.GetNamedObject(L"Applied").Stringify()));
                entry.owner=sync_string(peer,L"Owner");entry.verified=peer.GetNamedBoolean(L"Verified",false);peers_.emplace(node,std::move(entry));}
        }
    }
    std::wstring Offer(const Inventory& inventory)override
    {
        std::lock_guard guard(mutex_);CheckOwner();JsonObject notice;notice.Insert(L"Version",JsonValue::CreateNumberValue(1));
        notice.Insert(L"Group",JsonValue::CreateStringValue(group_));notice.Insert(L"Node",JsonValue::CreateStringValue(node_));
        notice.Insert(L"Valid",JsonValue::CreateBooleanValue(valid_));
        if(valid_){notice.Insert(L"Owner",JsonValue::CreateStringValue(owner_));notice.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(Encode(inventory))));
            // The common complete ACK carries only a digest. Partial ACKs list missing IDs, never bodies.
            const auto missing=Difference(inventory,applied_);if(!missing.operations.empty()||!missing.assets.empty())notice.Insert(L"Missing",JsonObject::Parse(Encode(missing)));}
        const auto text=std::wstring(notice.Stringify());if(tcard_asset::encode(text).size()>262144)throw std::runtime_error("Application metadata budget exceeded");return text;
    }
    ApplicationReport Observe(const std::wstring& text,const Inventory& advertised,const Inventory& required)override
    {
        std::lock_guard guard(mutex_);ApplicationReport report;
        if(text.empty()||tcard_asset::encode(text).size()>262144)return report;
        const auto notice=JsonObject::Parse(text);const auto node=sync_string(notice,L"Node");
        if(notice.GetNamedNumber(L"Version")!=1||sync_string(notice,L"Group")!=group_||!Node(node)||node==node_)return report;
        Entry entry;const auto found=peers_.find(node);if(found!=peers_.end())entry=found->second;
        else if(peers_.size()>=128)return report;
        selection::Include(entry.required,required);entry.verified=false;entry.owner.clear();entry.applied={};
        if(notice.GetNamedBoolean(L"Valid",false)){
            const auto owner=sync_string(notice,L"Owner");
            if(owner.empty()||owner.size()>100||sync_string(notice,L"Digest")!=tcard_bridge::Hash(Encode(advertised)))return report;
            Inventory missing;if(notice.HasKey(L"Missing"))missing=selection::Read(std::wstring(notice.GetNamedObject(L"Missing").Stringify()));
            if(selection::Missing(missing,advertised))return report;
            entry.applied=Difference(advertised,missing);entry.owner=owner;entry.verified=true;
        }
        const bool existed=found!=peers_.end();
        // Do not serialize every peer ledger when the authenticated logical capture is unchanged.
        if(existed&&entry==found->second){
            if(!tcard_storage::Ready(root_))return report;
            report.node=node;report.known=entry.verified;report.pending=selection::Missing(entry.required,entry.applied);return report;
        }
        Entry previous;if(existed)previous=found->second;peers_[node]=entry;
        bool saved=false;try{saved=Save();}catch(...){}
        if(!saved){if(existed)peers_[node]=previous;else peers_.erase(node);return report;}
        report.node=node;report.known=entry.verified;report.pending=selection::Missing(entry.required,entry.applied);return report;
    }
    ApplicationReport State(const std::wstring& node)
    {
        std::lock_guard guard(mutex_);ApplicationReport report;const auto found=peers_.find(node);if(found==peers_.end())return report;
        report.node=node;report.known=found->second.verified;report.pending=selection::Missing(found->second.required,found->second.applied);return report;
    }
    const std::wstring& Id()const{return node_;}
};
}
