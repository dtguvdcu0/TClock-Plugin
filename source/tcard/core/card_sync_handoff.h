#pragma once
#include "card_sync_mail.h"
#include "card_sync_remote.h"

// Custody advances a bounded transfer; only the native owner's applied inventory releases its target.
namespace tcard_handoff {
using namespace tcard_sync;
inline std::wstring Target(const std::wstring& packet)
{
    const auto root=JsonObject::Parse(packet);if(!root.GetNamedBoolean(L"NativeAtomic",false))return {};
    const auto target=std::wstring(root.GetNamedObject(L"Target").Stringify());
    if(sync_string(root,L"TargetDigest")!=tcard_bridge::Hash(target))throw std::runtime_error("Native target digest differs");selection::Read(target);return target;
}
inline selection::Inventory Custody(const std::wstring& text,const std::wstring& group,const std::wstring& owner,const std::wstring& packet,const std::wstring& target,bool& rewind)
{
    const auto root=JsonObject::Parse(text);const auto header=std::wstring(root.GetNamedObject(L"Inventory").Stringify());
    if(root.GetNamedNumber(L"Version")!=1||sync_string(root,L"Kind")!=L"mailbox-custody"||sync_string(root,L"Group")!=group||owner.empty()||
        sync_string(root,L"Owner")!=owner||sync_string(root,L"Packet")!=packet||sync_string(root,L"TargetDigest")!=tcard_bridge::Hash(target)||sync_string(root,L"Digest")!=tcard_bridge::Hash(header))throw std::runtime_error("Native custody identity differs");
    rewind=root.GetNamedBoolean(L"Rewind",false);return selection::Read(header);
}
inline bool Publish(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& packet,const std::wstring& target,const std::wstring& header,bool rewind)
{
    const auto group=tcard_mail::Group(storage,mailbox),owner=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId());selection::Read(header);
    JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-custody"));
    root.Insert(L"Group",JsonValue::CreateStringValue(group));root.Insert(L"Owner",JsonValue::CreateStringValue(owner));root.Insert(L"Packet",JsonValue::CreateStringValue(packet));
    root.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(target)));root.Insert(L"Inventory",JsonObject::Parse(header));root.Insert(L"Digest",JsonValue::CreateStringValue(tcard_bridge::Hash(header)));root.Insert(L"Rewind",JsonValue::CreateBooleanValue(rewind));
    const auto text=std::wstring(root.Stringify());std::wstring old;if(tcard_bridge::Read(mailbox,L"incoming-custody.json",old)&&old==text)return true;
    return tcard_bridge::Publish(mailbox,L"incoming-custody.json",text);
}
struct Sender {
    std::wstring packet,hash,owner;selection::Inventory applied; tcard_bridge::Stamp stamp,custody;bool custodyPresent=false;ULONGLONG audit=0;
    bool Changed(const std::wstring& mailbox,const std::wstring& receiver,const selection::Inventory& native)const
    {
        tcard_bridge::Stamp next,receipt;const bool present=tcard_bridge::Fingerprint(mailbox,L"incoming-custody.json",receipt);
        return packet.empty()||owner!=receiver||applied!=native||!tcard_bridge::Fingerprint(mailbox,L"incoming.json",next)||!(next==stamp)||present!=custodyPresent||(present&&!(receipt==custody))||GetTickCount64()-audit>=60000;
    }
};
inline bool Produce(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& owner,const selection::Inventory& applied,bool changed,Sender& state,Result& result,const std::wstring& remoteTarget=L"",ULONGLONG receiveRevision=0,const tcard_dependencies::Context* dependency=nullptr)
{
    result={};try{
        std::unique_lock<std::recursive_mutex> boundary(tcard_remote::receiveMutex,std::defer_lock);
        if(receiveRevision){boundary.lock();if(receiveRevision!=tcard_remote::revision.load())throw std::runtime_error("Remote receive changed before export");}
        const auto group=tcard_mail::Group(storage,mailbox),slot=sync_hash(L"incoming.json");if(owner.empty())throw std::runtime_error("Native receiver unavailable");
        if(!changed&&!state.Changed(mailbox,owner,applied))return true;
        auto next=state;next.owner=owner;next.applied=applied;next.audit=GetTickCount64();
        std::wstring retained,target,delta;selection::Inventory known=applied,required;bool replay=false;
        if(tcard_bridge::Read(mailbox,L"incoming.json",retained)&&tcard_mail::Batched(retained)){
            target=Target(retained);
            const auto expected=dependency&&JsonObject::Parse(retained).GetNamedNumber(L"BatchVersion")==3?tcard_dependency_mail::Expected(retained,*dependency,false):tcard_dependencies::Context{};
            const bool current=!dependency||JsonObject::Parse(retained).GetNamedNumber(L"BatchVersion")!=3||tcard_dependency_mail::Current(retained,*dependency);
            if(current)required=tcard_mail::Decode(retained,group,delta,true,dependency?&expected:nullptr);
            if(dependency&&current&&JsonObject::Parse(retained).GetNamedNumber(L"BatchVersion")==3&&!selection::Missing(required,applied)){
                const PreparedNote note{tcard_dependencies::Read(delta,expected).note,delta};
                if(!tcard_dependency_mail::Cleanup(storage,mailbox,L"incoming.json",note,expected,result))return false;
            }
            if(!target.empty()){
                if(!selection::Missing(selection::Read(target),applied)){tcard_bootstrap::Finish(storage,mailbox,slot,applied);target.clear();}
                else{
                    if(tcard_bootstrap::Read(storage,mailbox,slot)!=target)throw std::runtime_error("Native retained target differs");
                    std::wstring receipt;bool rewind=false;bool accepted=false;
                    if(current&&tcard_bridge::Read(mailbox,L"incoming-custody.json",receipt))try{auto staged=Custody(receipt,group,owner,tcard_bridge::Hash(retained),target,rewind);
                        if(rewind||!selection::Missing(required,staged)){known=std::move(staged);accepted=true;}}catch(...){}
                    if(accepted&&dependency&&JsonObject::Parse(retained).GetNamedNumber(L"BatchVersion")==3&&!rewind&&!selection::Missing(required,known)){
                        const PreparedNote note{tcard_dependencies::Read(delta,expected).note,delta};
                        if(!tcard_dependency_mail::Cleanup(storage,mailbox,L"incoming.json",note,expected,result))return false;
                    }
                    if(accepted)tcard_blobs::Reap(tcard_mail::Objects(mailbox,L"incoming.json"),known);
                    replay=current&&(!accepted||!selection::Missing(selection::Read(target),known));
                }
            }else if(selection::Missing(required,applied))replay=true;
        }
        if(replay){if(tcard_dependency_mail::Referenced(delta)){
                const auto expected=tcard_dependency_mail::Expected(retained,*dependency,false);tcard_dependency_mail::Stage(storage,mailbox,L"incoming.json",delta,expected);
            }else if(tcard_objects::Referenced(delta))tcard_blobs::Stage(storage,tcard_mail::Objects(mailbox,L"incoming.json",true),delta);next.packet=retained;}
        else{
            std::wstring packet;unsigned version=0;
            bool exported=false;
            if(target.empty()){
                if(!remoteTarget.empty())tcard_bootstrap::Retain(storage,mailbox,slot,remoteTarget);
                exported=tcard_bootstrap::Export(storage,mailbox,slot,applied,packet,result,version,false,true);
                if(sync_exists(tcard_bootstrap::Folder(mailbox),slot+L".json"))target=tcard_bootstrap::Read(storage,mailbox,slot);
            }else exported=ExportSelection(storage,known,packet,result,version,false,true,target,tcard_asset::encode(target).size()+512);
            const bool fallback=!exported;
            if(fallback&&(!dependency||!tcard_dependencies::LegacyFallback(result)))return false;
            if(fallback){
                if(dependency->group!=group||dependency->source!=tcard_bootstrap::Node(storage))throw std::runtime_error("Dependency handoff channel differs");
                if(target.empty()){
                    if(!remoteTarget.empty())target=tcard_blobs::Canonical(remoteTarget);
                    else if(!ExportCommittedInventory(storage,target,result,version))return false;
                    if(!selection::Missing(selection::Read(target),applied)){next.packet=retained;next.hash=tcard_bridge::Hash(retained);state=std::move(next);return true;}
                    tcard_bootstrap::Retain(storage,mailbox,slot,target);
                }
                auto expected=*dependency;expected.target=tcard_blobs::Canonical(target);
                if(!ExportDependencies(storage,known,expected,delta,result,version))return false;
                tcard_dependency_mail::Stage(storage,mailbox,L"incoming.json",delta,expected);
                JsonObject root;root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-batch"));root.Insert(L"BatchVersion",JsonValue::CreateNumberValue(3));root.Insert(L"Group",JsonValue::CreateStringValue(group));
                root.Insert(L"Inventory",JsonObject::Parse(expected.target));root.Insert(L"Delta",JsonObject::Parse(delta));root.Insert(L"NativeAtomic",JsonValue::CreateBooleanValue(true));
                root.Insert(L"Target",JsonObject::Parse(expected.target));root.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(expected.target)));
                packet=root.Stringify();
            }
            if(!fallback){
            // The target is additional authenticated metadata, outside the ordinary selection envelope.
            if(!target.empty()&&tcard_asset::encode(packet).size()+tcard_asset::encode(target).size()+512>tcard_asset::max_file-65536&&
                !ExportSelection(storage,known,packet,result,version,false,true,target,tcard_asset::encode(target).size()+512))return false;
            auto root=JsonObject::Parse(packet);selection::Unwrap(packet,delta,true);const bool objects=tcard_objects::Referenced(delta);
            if(objects)tcard_blobs::Stage(storage,tcard_mail::Objects(mailbox,L"incoming.json",true),delta);
            root.Insert(L"Kind",JsonValue::CreateStringValue(L"mailbox-batch"));root.Insert(L"BatchVersion",JsonValue::CreateNumberValue(objects?2:1));root.Insert(L"Group",JsonValue::CreateStringValue(group));
            if(!target.empty()){root.Insert(L"NativeAtomic",JsonValue::CreateBooleanValue(true));root.Insert(L"Target",JsonObject::Parse(target));root.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(target)));}
            packet=root.Stringify();}
            next.packet=packet;if(tcard_asset::encode(next.packet).size()>tcard_asset::max_file-65536)throw std::runtime_error("Native transfer exceeds packet budget");
            if(receiveRevision&&receiveRevision!=tcard_remote::revision.load())throw std::runtime_error("Remote receive changed during export");
            if(next.packet!=retained&&!tcard_bridge::Publish(mailbox,L"incoming.json",next.packet))throw std::runtime_error("Native transfer publication failed");
        }
        next.hash=tcard_bridge::Hash(next.packet);if(!tcard_bridge::Fingerprint(mailbox,L"incoming.json",next.stamp))throw std::runtime_error("Native transfer disappeared");
        next.custodyPresent=tcard_bridge::Fingerprint(mailbox,L"incoming-custody.json",next.custody);state=std::move(next);return true;
    }catch(const std::exception& e){result.error.assign(e.what(),e.what()+strlen(e.what()));return false;}catch(...){result.error=L"Native transfer failed";return false;}
}
}
