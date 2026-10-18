#pragma once
#include "card_sync_ack.h"
#include "card_sync_blobs.h"
#include "card_sync_bootstrap.h"
#include "card_sync_dependency_mail.h"

// One encrypted durable slot per direction. Only the current receiver's committed evidence releases it.
namespace tcard_mail {
using namespace tcard_sync;
using Inventory=selection::Inventory;
inline std::wstring Group(const std::wstring& storage,const std::wstring& mailbox)
{
    tcard_crypto::Keys native,keys;
    struct Wipe{tcard_crypto::Keys& a; tcard_crypto::Keys& b;~Wipe(){SecureZeroMemory(a.dataKey.data(),a.dataKey.size());SecureZeroMemory(b.dataKey.data(),b.dataKey.size());}} wipe{native,keys};
    if(!tcard_storage::Required(storage)||!tcard_storage::Required(mailbox)||!tcard_storage::LoadKey(storage,native)||
        !tcard_storage::LoadKey(mailbox,keys)||native.publicKey!=keys.publicKey)throw std::runtime_error("Mailbox batch keys differ or are unavailable");
    return tcard_ack::Identity(keys);
}
inline bool Batched(const std::wstring& packet)
{
    try{return JsonObject::Parse(packet).GetNamedString(L"Kind",L"")==L"mailbox-batch";}catch(...){return false;}
}
inline std::wstring Objects(const std::wstring& mailbox,const std::wstring& leaf,bool create=false)
{
    if(leaf!=L"incoming.json"&&leaf!=L"outgoing.json")throw std::runtime_error("Unknown mailbox object direction");
    const auto base=mailbox+L"\\"+leaf.substr(0,leaf.size()-5)+L"-objects";
    if(create&&!tcard_bridge::Protect(base))throw std::runtime_error("Cannot protect mailbox objects");return base;
}
inline Inventory Decode(const std::wstring& packet,const std::wstring& group,std::wstring& delta,bool objects=false,const tcard_dependencies::Context* dependency=nullptr)
{
    const auto root=JsonObject::Parse(packet);
    if(root.GetNamedNumber(L"BatchVersion")==3){
        if(!dependency)throw std::runtime_error("Dependency mailbox capability absent");
        return tcard_dependency_mail::Decode(packet,*dependency,delta);
    }
    if((root.GetNamedNumber(L"BatchVersion")!=1&&!(objects&&root.GetNamedNumber(L"BatchVersion")==2))||!Batched(packet)||sync_string(root,L"Group")!=group)throw std::runtime_error("Mailbox batch identity differs");
    const auto advertised=selection::Unwrap(packet,delta,objects);if(tcard_objects::Referenced(delta)&&root.GetNamedNumber(L"BatchVersion")!=2)throw std::runtime_error("Object mailbox capability absent");return selection::Validate(delta,advertised,objects);
}
inline Inventory Receipt(const std::wstring& text,const std::wstring& group,const std::wstring& owner,const std::wstring& packet)
{
    const auto root=JsonObject::Parse(text);
    if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"mailbox-commit"||sync_string(root,L"Group")!=group||
        owner.empty()||sync_string(root,L"Owner")!=owner||sync_string(root,L"Packet")!=packet)throw std::runtime_error("Mailbox receipt identity differs");
    const auto header=std::wstring(root.GetNamedObject(L"Inventory").Stringify());
    if(sync_string(root,L"Digest")!=tcard_bridge::Hash(header))throw std::runtime_error("Mailbox receipt digest differs");return selection::Read(header);
}
inline bool Commit(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const std::wstring& packet,
    const Inventory& required,std::wstring& header)
{
    header.clear();try{
        const auto group=Group(storage,mailbox),owner=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId());Result result;unsigned version=0;
        if(!sync_hex(packet)||!ExportCommittedInventory(storage,header,result,version))return false;
        const auto committed=selection::Read(header);if(selection::Missing(required,committed)){header.clear();return false;}
        std::wstring previous;if(tcard_bridge::Read(mailbox,leaf,previous))try{if(Receipt(previous,group,owner,packet)==committed)return true;}catch(...){}
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-commit"));
        root.Insert(L"Group",JsonValue::CreateStringValue(group));root.Insert(L"Owner",JsonValue::CreateStringValue(owner));root.Insert(L"Packet",JsonValue::CreateStringValue(packet));
        root.Insert(L"Inventory",JsonObject::Parse(header));root.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(header)));
        std::wstring verified;return tcard_bridge::Publish(mailbox,leaf,std::wstring(root.Stringify()))&&tcard_bridge::Read(mailbox,leaf,verified)&&Receipt(verified,group,owner,packet)==committed;
    }catch(...){header.clear();return false;}
}
struct Producer {
    std::wstring packet,header,hash,receiver;Inventory known;bool done=false,ackPresent=false,ackValid=false,cleanupPending=false;
    tcard_bridge::Stamp stamp,ackStamp;ULONGLONG audited=0;
    bool Changed(const std::wstring& mailbox,const std::wstring& leaf,const std::wstring& receipt,const std::wstring& owner)const
    {
        tcard_bridge::Stamp current,ack;const bool present=tcard_bridge::Fingerprint(mailbox,receipt,ack);
        return cleanupPending||packet.empty()||receiver!=owner||!tcard_bridge::Fingerprint(mailbox,leaf,current)||!(current==stamp)||
            present!=ackPresent||(present&&!(ack==ackStamp))||GetTickCount64()-audited>=60000;
    }
};
inline bool Produce(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const std::wstring& receipt,
    const std::wstring& owner,bool readonly,bool changed,Producer& state,Result& result,bool objects=false,bool sourceTargets=false,const tcard_dependencies::Context* dependency=nullptr)
{
    result={};try{
        const auto group=Group(storage,mailbox);if(owner.empty())throw std::runtime_error("Mailbox receiver unavailable");
        if(!changed&&!state.Changed(mailbox,leaf,receipt,owner))return true;
        const auto now=GetTickCount64();auto next=state;tcard_bridge::Stamp stamp,ack;
        if(tcard_bridge::Fingerprint(mailbox,leaf,stamp)){
            if(next.packet.empty()||next.receiver!=owner||!(stamp==next.stamp)||now-next.audited>=60000){
                std::wstring packet;if(!tcard_bridge::Read(mailbox,leaf,packet))throw std::runtime_error("Retained mailbox unavailable");
                if(Batched(packet)){
                    std::wstring delta;
                    if(JsonObject::Parse(packet).GetNamedNumber(L"BatchVersion")==3&&dependency&&!tcard_dependency_mail::Current(packet,*dependency)){
                        next.packet.clear();next.hash.clear();next.done=false;
                    }else{
                    const auto expected=dependency&&JsonObject::Parse(packet).GetNamedNumber(L"BatchVersion")==3?tcard_dependency_mail::Expected(packet,*dependency,leaf==L"outgoing.json"):tcard_dependencies::Context{};
                    Decode(packet,group,delta,objects,dependency?&expected:nullptr);next.packet=std::move(packet);next.hash=tcard_bridge::Hash(next.packet);
                    next.header=std::wstring(JsonObject::Parse(next.packet).GetNamedObject(L"Inventory").Stringify());next.done=false;}
                }else{next.packet.clear();next.hash.clear();next.done=false;}next.stamp=stamp;
            }
        }else{next.packet.clear();next.hash.clear();next.done=false;}
        if(next.receiver!=owner)next.done=false;
        next.receiver=owner;next.ackPresent=tcard_bridge::Fingerprint(mailbox,receipt,ack);next.ackStamp=ack;next.ackValid=false;next.known={};
        if(next.ackPresent&&!next.packet.empty()){
            std::wstring text;if(tcard_bridge::Read(mailbox,receipt,text))try{
                next.known=Receipt(text,group,owner,next.hash);std::wstring delta;
                const auto expected=dependency&&JsonObject::Parse(next.packet).GetNamedNumber(L"BatchVersion")==3?tcard_dependency_mail::Expected(next.packet,*dependency,leaf==L"outgoing.json"):tcard_dependencies::Context{};
                const auto required=Decode(next.packet,group,delta,objects,dependency?&expected:nullptr);
                next.ackValid=selection::Missing(required,next.known)==0;
            }catch(...){}
        }
        if(next.known!=state.known)next.done=false;
        next.audited=now;
        if(!next.packet.empty()&&!next.ackValid){
            std::wstring delta;const auto expected=dependency&&JsonObject::Parse(next.packet).GetNamedNumber(L"BatchVersion")==3?tcard_dependency_mail::Expected(next.packet,*dependency,leaf==L"outgoing.json"):tcard_dependencies::Context{};
            Decode(next.packet,group,delta,objects,dependency?&expected:nullptr);
            if(tcard_dependency_mail::Referenced(delta))tcard_dependency_mail::Stage(storage,mailbox,leaf,delta,expected);
            else if(tcard_objects::Referenced(delta))tcard_blobs::Stage(storage,Objects(mailbox,leaf,true),delta);
            state=std::move(next);return true;
        }
        if(next.ackValid&&dependency&&JsonObject::Parse(next.packet).GetNamedNumber(L"BatchVersion")==3){
            const auto expected=tcard_dependency_mail::Expected(next.packet,*dependency,leaf==L"outgoing.json");std::wstring delta;tcard_dependency_mail::Decode(next.packet,expected,delta);
            next.cleanupPending=true;
            // TCard's synchronous compatibility caller leaves full-byte work to its existing async exporter.
            if(!readonly&&leaf==L"outgoing.json"){state=std::move(next);return true;}
            const PreparedNote note{tcard_dependencies::Read(delta,expected).note,delta};
            if(!tcard_dependency_mail::Cleanup(storage,mailbox,leaf,note,expected,result))return false;next.cleanupPending=false;
        }
        if(next.ackValid)tcard_blobs::Reap(Objects(mailbox,leaf),next.known);
        if(next.done&&!changed){state=std::move(next);return true;}
        std::wstring packet;unsigned version=0;const bool exported=objects?tcard_bootstrap::Export(storage,mailbox,sync_hash(leaf),next.known,packet,result,version,readonly,objects):ExportSelection(storage,next.known,packet,result,version,readonly,objects);
        if(!exported&&(!dependency||!tcard_dependencies::LegacyFallback(result)))return false;
        if(!exported){
            if(!objects||!sourceTargets||leaf!=L"outgoing.json"||dependency->group!=group||dependency->source!=tcard_bootstrap::Node(storage))
                throw std::runtime_error("Dependency producer channel differs");
            const auto slot=sync_hash(leaf);tcard_bootstrap::Finish(storage,mailbox,slot,next.known);
            std::wstring target;unsigned version=0;
            if(sync_exists(tcard_bootstrap::Folder(mailbox),slot+L".json"))target=tcard_bootstrap::Read(storage,mailbox,slot);
            else{if(!ExportCommittedInventory(storage,target,result,version))return false;
                if(!selection::Missing(selection::Read(target),next.known)){next.done=true;state=std::move(next);return true;}
                tcard_bootstrap::Retain(storage,mailbox,slot,target);}
            auto expected=*dependency;expected.target=tcard_blobs::Canonical(target);std::wstring delta;
            if(!ExportDependencies(storage,next.known,expected,delta,result,version))return false;
            tcard_dependency_mail::Stage(storage,mailbox,leaf,delta,expected);
            JsonObject root,boundary;root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-batch"));root.Insert(L"BatchVersion",JsonValue::CreateNumberValue(3));
            root.Insert(L"Group",JsonValue::CreateStringValue(group));root.Insert(L"Inventory",JsonObject::Parse(expected.target));root.Insert(L"Delta",JsonObject::Parse(delta));
            boundary.Insert(L"Version",JsonValue::CreateNumberValue(1));boundary.Insert(L"Direction",JsonValue::CreateStringValue(L"outgoing"));
            boundary.Insert(L"Group",JsonValue::CreateStringValue(group));boundary.Insert(L"Source",JsonValue::CreateStringValue(expected.source));
            boundary.Insert(L"Target",JsonObject::Parse(expected.target));boundary.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(expected.target)));
            boundary.Insert(L"Atomic",JsonValue::CreateBooleanValue(true));root.Insert(L"SourceTarget",boundary);
            const auto packet=std::wstring(root.Stringify());if(tcard_asset::encode(packet).size()>tcard_asset::max_file-65536)throw std::runtime_error("Dependency mailbox exceeds packet budget");
            std::wstring verified;if(!tcard_bridge::Publish(mailbox,leaf,packet)||!tcard_bridge::Read(mailbox,leaf,verified)||verified!=packet)
                throw std::runtime_error("Dependency mailbox publication failed");
            tcard_dependency_mail::Decode(verified,expected,delta);next.packet=std::move(verified);next.header=expected.target;next.hash=tcard_bridge::Hash(next.packet);next.done=false;next.ackValid=false;
            if(!tcard_bridge::Fingerprint(mailbox,leaf,next.stamp))throw std::runtime_error("Dependency mailbox identity unavailable");state=std::move(next);return true;
        }
        std::wstring sourceTarget;bool sourceAtomic=false;
        if(sourceTargets&&objects&&leaf==L"outgoing.json"){
            const auto slot=sync_hash(leaf);sourceAtomic=sync_exists(tcard_bootstrap::Folder(mailbox),slot+L".json");
            sourceTarget=tcard_blobs::Canonical(sourceAtomic?tcard_bootstrap::Read(storage,mailbox,slot):std::wstring(JsonObject::Parse(packet).GetNamedObject(L"Inventory").Stringify()));
            if(tcard_asset::encode(packet).size()+tcard_asset::encode(sourceTarget).size()+1024>tcard_asset::max_file-65536&&
                !ExportSelection(storage,next.known,packet,result,version,readonly,objects,sourceTarget,tcard_asset::encode(sourceTarget).size()+1024))return false;
        }
        auto root=JsonObject::Parse(packet);std::wstring delta;selection::Unwrap(packet,delta,objects);
        next.header=std::wstring(root.GetNamedObject(L"Inventory").Stringify());const auto required=selection::Read(selection::Header(delta));
        if(!next.packet.empty()&&required.operations.empty()&&required.assets.empty()){next.done=true;state=std::move(next);return true;}
        const bool referenced=tcard_objects::Referenced(delta);if(referenced)tcard_blobs::Stage(storage,Objects(mailbox,leaf,true),delta);
        root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-batch"));root.Insert(L"BatchVersion",JsonValue::CreateNumberValue(referenced?2:1));root.Insert(L"Group",JsonValue::CreateStringValue(group));
        if(!sourceTarget.empty()){
            JsonObject boundary;boundary.Insert(L"Version",JsonValue::CreateNumberValue(1));boundary.Insert(L"Direction",JsonValue::CreateStringValue(L"outgoing"));boundary.Insert(L"Group",JsonValue::CreateStringValue(group));
            boundary.Insert(L"Source",JsonValue::CreateStringValue(tcard_bootstrap::Node(storage)));boundary.Insert(L"Target",JsonObject::Parse(sourceTarget));boundary.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(sourceTarget)));boundary.Insert(L"Atomic",JsonValue::CreateBooleanValue(sourceAtomic));root.Insert(L"SourceTarget",boundary);
        }
        packet=root.Stringify();if(tcard_asset::encode(packet).size()>tcard_asset::max_file-65536)throw std::runtime_error("Native source boundary exceeds packet budget");const auto hash=tcard_bridge::Hash(packet);std::wstring verified;
        if(!tcard_bridge::Publish(mailbox,leaf,packet)||!tcard_bridge::Read(mailbox,leaf,verified)||tcard_bridge::Hash(verified)!=hash)throw std::runtime_error("Mailbox batch publication failed");
        Decode(verified,group,delta,objects);next.packet=std::move(verified);next.hash=hash;next.done=false;next.ackValid=false;
        if(!tcard_bridge::Fingerprint(mailbox,leaf,next.stamp))throw std::runtime_error("Mailbox batch identity unavailable");state=std::move(next);return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}catch(...){result.error=L"Mailbox batch failed";return false;}
}
inline bool Budget(const Result& result)
{
    return result.error==L"Source too large"||result.error==L"Exchange package budget exceeded";
}
}
