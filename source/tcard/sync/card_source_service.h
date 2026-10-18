#pragma once
#include "../core/card_source_owner.h"
#include "../core/card_source_witness.h"
#include "../core/card_sync_transport.h"

// Worker-only adapter. Networking requests metadata; only the native owner reads its saved bundles.
namespace tcard_source_service {
using namespace tcard_sync;
class Service final:public PeerSource {
    std::wstring mailbox_,replica_,owner_,worker_,group_,node_;HANDLE parent_;
    std::mutex mutex_;std::wstring active_,receiver_,nonce_;tcard_source_owner::Binding binding_;
    void Alive(){tcard_source_witness::Require(WaitForSingleObject(parent_,0)==WAIT_TIMEOUT,"Saved source owner exited");}
    std::wstring Retained(const std::wstring& request,const std::wstring& receiver)
    {
        const auto folder=tcard_bootstrap::Folder(replica_),leaf=receiver+L".json";if(!tcard_native::Exists(folder+L"\\"+leaf))return L"";
        const auto target=tcard_bootstrap::Read(replica_,replica_,receiver);std::wstring raw;
        tcard_source_witness::Require(tcard_bridge::Read(folder,leaf,raw),"Saved source retained receipt unavailable");
        const auto record=JsonObject::Parse(raw);const auto proof=sync_string(record,L"OwnerSource");
        tcard_source_witness::Require(sync_string(record,L"OwnerDigest")==tcard_bridge::Hash(proof),"Saved source retained proof digest differs");
        tcard_source_witness::Require(tcard_source_witness::Check(proof,request,node_,receiver,group_)==target,"Saved source retained target differs");
        std::wstring committed;Result result;unsigned version=0;
        tcard_source_witness::Require(ExportCommittedInventory(replica_,committed,result,version)&&!selection::Missing(selection::Read(target),selection::Read(committed)),"Saved source retained originals unavailable");
        Alive();return proof;
    }
    bool Complete()
    {
        if(active_.empty())return true;
        if(!Retained(active_,receiver_).empty()){active_.clear();receiver_.clear();nonce_.clear();return true;}
        tcard_source_owner::Snapshot snapshot;
        if(!tcard_source_owner::Inspect(mailbox_,binding_,nonce_,snapshot)||!snapshot.ready)return false;
        std::wstring owner;tcard_source_owner::Read(mailbox_,tcard_source_owner::replyName,owner);
        tcard_source_owner::Snapshot confirmed;std::wstring retained;
        tcard_source_witness::Require(tcard_source_owner::Inspect(mailbox_,binding_,nonce_,confirmed)&&confirmed.ready&&confirmed.target==snapshot.target&&confirmed.challenge==snapshot.challenge&&
            tcard_bridge::Read(mailbox_,tcard_source_owner::replyName,retained)&&retained==owner,"Saved source owner receipt changed before retention");
        JsonObject proof;proof.Insert(L"Version",JsonValue::CreateNumberValue(1));
        for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",L"saved-owner-source"},{L"Request",active_},{L"Source",node_},{L"Receiver",receiver_},{L"Group",group_},
            {L"Owner",owner_},{L"Worker",worker_},{L"NativeNode",binding_.native},{L"Challenge",snapshot.challenge},{L"Receipt",tcard_bridge::Hash(owner)},{L"Target",snapshot.target},{L"TargetDigest",tcard_bridge::Hash(snapshot.target)}})proof.Insert(name,JsonValue::CreateStringValue(value));
        const auto text=std::wstring(proof.Stringify());tcard_source_witness::Check(text,active_,node_,receiver_,group_);
        // Publish the ordinary retention target and its freshness proof in one encrypted durable record.
        std::lock_guard pins(tcard_bootstrap::pinMutex);const auto folder=tcard_bootstrap::Folder(replica_),leaf=receiver_+L".json";SyncLock lock(folder);
        tcard_source_witness::Require(bool(lock)&&!tcard_native::Exists(folder+L"\\"+leaf),"Saved source retention slot busy");
        std::wstring committed;Result result;unsigned version=0;
        tcard_source_witness::Require(ExportCommittedInventory(replica_,committed,result,version)&&!selection::Missing(selection::Read(snapshot.target),selection::Read(committed)),"Saved source originals changed before retention");
        tcard_source_witness::Require(tcard_bridge::Protect(folder)&&tcard_bootstrap::Count(folder)<tcard_bootstrap::maxPins,"Saved source retention budget reached");
        JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));
        for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Group",group_},{L"Source",node_},{L"Receiver",receiver_},{L"Digest",tcard_bridge::Hash(snapshot.target)},{L"OwnerSource",text},{L"OwnerDigest",tcard_bridge::Hash(text)}})record.Insert(name,JsonValue::CreateStringValue(value));
        record.Insert(L"Inventory",JsonObject::Parse(snapshot.target));Alive();
        tcard_source_witness::Require(tcard_source_owner::Put(folder,leaf.c_str(),record)&&Retained(active_,receiver_)==text,"Cannot retain saved source witness");
        active_.clear();receiver_.clear();nonce_.clear();return true;
    }
public:
    Service(std::wstring mailbox,std::wstring replica,std::wstring owner,std::wstring worker,std::wstring group,std::wstring node,HANDLE parent):mailbox_(std::move(mailbox)),replica_(std::move(replica)),owner_(std::move(owner)),worker_(std::move(worker)),group_(std::move(group)),node_(std::move(node)),parent_(parent){}
    std::wstring Capture(const std::wstring& request,const std::wstring& receiver,std::stop_token stop)override
    {
        std::lock_guard guard(mutex_);Alive();tcard_source_witness::Require(!stop.stop_requested()&&sync_hex(request)&&sync_hex(receiver)&&receiver!=node_,"Invalid saved source request");
        const auto retained=Retained(request,receiver);if(!retained.empty())return retained;
        if(!Complete())return L"";const auto completed=Retained(request,receiver);if(!completed.empty())return completed;
        const auto folder=tcard_bootstrap::Folder(replica_);if(tcard_native::Exists(folder))tcard_source_witness::Require(tcard_bootstrap::Count(folder)<tcard_bootstrap::maxPins,"Saved source retention budget reached");
        selection::Inventory applied;bool dependencies=false;std::wstring native;
        if(!tcard_ack::Read(mailbox_,owner_,applied,nullptr,nullptr,nullptr,&dependencies,&native)||!dependencies)return L"";
        binding_={owner_,worker_,group_,node_,native};tcard_source_owner::Validate(binding_);tcard_crypto::Keys keys;tcard_source_owner::Store(mailbox_,binding_,keys);
        tcard_source_witness::Require(tcard_storage::Matches(replica_,keys)&&tcard_bootstrap::Node(replica_)==node_,"Saved source replica association changed");
        const auto nonce=tcard_bridge::Hash(request+L"\n"+receiver+L"\n"+owner_+L"\n"+worker_);
        tcard_source_witness::Require(tcard_source_owner::Put(mailbox_,tcard_source_owner::queryName,tcard_source_owner::Record(L"source-query",nonce,binding_)),"Cannot request saved owner source");
        active_=request;receiver_=receiver;nonce_=nonce;return L"";
    }
};
}
