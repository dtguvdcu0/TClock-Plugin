#pragma once
#include "card_sync_ack.h"
#include "card_sync_bootstrap.h"
#include "card_native_root.h"
#include <utility>

// Local owner evidence only. Remote admission still requires authenticated v2 transport and complete originals.
namespace tcard_source_owner {
using namespace tcard_sync;
inline constexpr wchar_t queryName[]=L"source-query.json",requestName[]=L"source-request.json",replyName[]=L"source-owner.json",resultName[]=L"source-result.json";
struct Binding {std::wstring owner,worker,group,node,native;};
struct Snapshot {std::wstring request,target,node,native,challenge;bool ready=false;};
inline void Require(bool okay,const char* error){if(!okay)throw std::runtime_error(error);}
inline bool Process(const std::wstring& value){const auto colon=value.find(L':');return colon!=std::wstring::npos&&colon>0&&colon+1<value.size()&&value.size()<=64&&value.find_first_not_of(L"0123456789:")==std::wstring::npos&&value.find(L':',colon+1)==std::wstring::npos;}
inline void Validate(const Binding& b){Require(Process(b.owner)&&Process(b.worker)&&b.owner!=b.worker&&sync_hex(b.group)&&sync_hex(b.node)&&sync_hex(b.native),"Invalid source owner binding");}
inline JsonObject Record(const wchar_t* kind,const std::wstring& nonce,const Binding& b)
{
    Validate(b);Require(sync_hex(nonce),"Invalid source query nonce");JsonObject r;r.Insert(L"Version",JsonValue::CreateNumberValue(1));
    for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",kind},{L"Request",nonce},{L"Owner",b.owner},{L"Worker",b.worker},{L"Group",b.group},{L"Node",b.node},{L"NativeNode",b.native}})r.Insert(name,JsonValue::CreateStringValue(value));return r;
}
inline void Check(const JsonObject& r,const wchar_t* kind,const std::wstring& nonce,const Binding& b,unsigned count)
{
    Validate(b);Require(r.Size()==count&&r.GetNamedNumber(L"Version")==1&&sync_string(r,L"Kind")==kind&&sync_hex(nonce)&&sync_string(r,L"Request")==nonce&&
        sync_string(r,L"Owner")==b.owner&&sync_string(r,L"Worker")==b.worker&&sync_string(r,L"Group")==b.group&&sync_string(r,L"Node")==b.node&&sync_string(r,L"NativeNode")==b.native,"Source owner association differs");
}
inline void Store(const std::wstring& path,const Binding& b,tcard_crypto::Keys& keys)
{
    Require(tcard_storage::Required(path)&&tcard_storage::Ready(path)&&tcard_storage::LoadKey(path,keys)&&tcard_ack::Identity(keys)==b.group,"Source owner encryption unavailable");
}
inline JsonObject Read(const std::wstring& mailbox,const wchar_t* name,std::wstring& text)
{
    Require(tcard_bridge::Read(mailbox,name,text),"Source owner IPC unreadable");return JsonObject::Parse(text);
}
inline bool Put(const std::wstring& mailbox,const wchar_t* name,const JsonObject& record)
{
    const auto text=std::wstring(record.Stringify());std::wstring retained;
    if(tcard_bridge::Read(mailbox,name,retained)&&retained==text)return true;
    return tcard_bridge::Publish(mailbox,name,text)&&tcard_bridge::Read(mailbox,name,retained)&&retained==text;
}
inline std::wstring Target(const JsonObject& r)
{
    const auto target=sync_string(r,L"Target");Require(!target.empty()&&sync_string(r,L"TargetDigest")==tcard_bridge::Hash(target),"Source owner target differs");selection::Read(target);return target;
}
inline bool Begin(const std::wstring& mailbox,const Binding& b,std::wstring& nonce)
{
    nonce.clear();try{tcard_crypto::Keys keys;Store(mailbox,b,keys);const auto token=tcard_bridge::Hash(tcard_bridge::Token());if(!Put(mailbox,queryName,Record(L"source-query",token,b)))return false;nonce=token;return true;}catch(...){return false;}
}
// Called in a TCard-owned read-only export task; no live bundle or draft is modified.
inline bool Serve(const std::wstring& native,const std::wstring& mailbox,const Binding& b,HANDLE worker,const std::wstring& expected)
{
    try{
        if(WaitForSingleObject(worker,0)!=WAIT_TIMEOUT)return false;tcard_crypto::Keys keys;Store(mailbox,b,keys);
        Require(tcard_storage::Matches(native,keys)&&tcard_bootstrap::Node(native)==b.native,"Source native owner changed");
        std::wstring query,request;const auto q=Read(mailbox,queryName,query),r=Read(mailbox,requestName,request);const auto nonce=sync_string(q,L"Request");
        Require(request==expected,"Source task challenge changed");Check(q,L"source-query",nonce,b,8);Check(r,L"source-request",nonce,b,9);Require(sync_string(r,L"Query")==tcard_bridge::Hash(query),"Source query changed");
        std::wstring retained;
        if(tcard_bridge::Read(mailbox,replyName,retained)){
            const auto old=JsonObject::Parse(retained);if(sync_string(old,L"Request")==nonce){Check(old,L"source-owner",nonce,b,12);Target(old);
                Require(sync_string(old,L"Query")==tcard_bridge::Hash(query)&&sync_string(old,L"Challenge")==tcard_bridge::Hash(request),"Source challenge changed");return true;}
        }else Require(!tcard_native::Exists(mailbox+L"\\"+replyName),"Source owner receipt unavailable");
        std::wstring target;Result result;unsigned version=0;Require(ExportCommittedInventory(native,target,result,version),"Saved source inventory unavailable");
        Require(tcard_storage::Matches(native,keys)&&tcard_bootstrap::Node(native)==b.native&&WaitForSingleObject(worker,0)==WAIT_TIMEOUT,"Source owner changed during capture");
        std::wstring current;Read(mailbox,requestName,current);Require(current==request,"Source challenge superseded");Read(mailbox,queryName,current);Require(current==query,"Source query superseded");
        auto reply=Record(L"source-owner",nonce,b);reply.Insert(L"Query",JsonValue::CreateStringValue(tcard_bridge::Hash(query)));reply.Insert(L"Challenge",JsonValue::CreateStringValue(tcard_bridge::Hash(request)));
        reply.Insert(L"Target",JsonValue::CreateStringValue(target));reply.Insert(L"TargetDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(target)));return Put(mailbox,replyName,reply);
    }catch(...){return false;}
}
inline bool Inspect(const std::wstring& mailbox,const Binding& b,const std::wstring& nonce,Snapshot& state)
{
    state={};try{
        tcard_crypto::Keys keys;Store(mailbox,b,keys);std::wstring query,request,response;const auto q=Read(mailbox,queryName,query),r=Read(mailbox,requestName,request),result=Read(mailbox,resultName,response);
        Check(q,L"source-query",nonce,b,8);Check(r,L"source-request",nonce,b,9);Check(result,L"source-result",nonce,b,14);
        Require(sync_string(r,L"Query")==tcard_bridge::Hash(query)&&sync_string(result,L"Query")==tcard_bridge::Hash(query)&&sync_string(result,L"Challenge")==tcard_bridge::Hash(request),"Source result query differs");
        const auto phase=sync_string(result,L"Phase");Require(phase==L"waiting-owner"||phase==L"waiting-originals"||phase==L"ready","Invalid source result phase");
        state.request=nonce;state.node=b.node;state.native=b.native;state.challenge=tcard_bridge::Hash(request);
        if(phase==L"waiting-owner"){Require(sync_string(result,L"Reply").empty()&&sync_string(result,L"Target").empty()&&sync_string(result,L"TargetDigest")==tcard_bridge::Hash(L""),"Unwitnessed source target");return true;}
        std::wstring owner;const auto reply=Read(mailbox,replyName,owner);Check(reply,L"source-owner",nonce,b,12);state.target=Target(reply);
        Require(sync_string(reply,L"Query")==tcard_bridge::Hash(query)&&sync_string(reply,L"Challenge")==tcard_bridge::Hash(request)&&sync_string(result,L"Reply")==tcard_bridge::Hash(owner)&&Target(result)==state.target,"Source result owner witness differs");
        state.ready=phase==L"ready";return true;
    }catch(...){state={};return false;}
}
// The borrowed parent handle stays owned by the regular worker until its main loop exits.
class Provider {
    std::wstring mailbox_,replica_,owner_,worker_,group_,node_,query_,reply_;HANDLE parent_;ULONGLONG retryAt_=0;
    bool ready_=false;tcard_bridge::Stamp queryStamp_{},replyStamp_{},resultStamp_{};
public:
    Provider(std::wstring mailbox,std::wstring replica,std::wstring owner,std::wstring worker,std::wstring group,std::wstring node,HANDLE parent):mailbox_(std::move(mailbox)),replica_(std::move(replica)),owner_(std::move(owner)),worker_(std::move(worker)),group_(std::move(group)),node_(std::move(node)),parent_(parent){}
    bool Step()
    {
        try{
            if(WaitForSingleObject(parent_,0)!=WAIT_TIMEOUT)return false;tcard_bridge::Stamp stamp,replyStamp,resultStamp;
            if(!tcard_bridge::Fingerprint(mailbox_,queryName,stamp))return true;
            if(ready_&&stamp==queryStamp_&&tcard_bridge::Fingerprint(mailbox_,replyName,replyStamp)&&replyStamp==replyStamp_&&tcard_bridge::Fingerprint(mailbox_,resultName,resultStamp)&&resultStamp==resultStamp_)return true;
            std::wstring query;const auto q=Read(mailbox_,queryName,query);const auto nonce=sync_string(q,L"Request");Binding b{owner_,worker_,group_,node_,sync_string(q,L"NativeNode")};Check(q,L"source-query",nonce,b,8);
            tcard_crypto::Keys keys;Store(mailbox_,b,keys);Require(tcard_storage::Matches(replica_,keys)&&tcard_bootstrap::Node(replica_)==node_,"Source replica changed");
            selection::Inventory applied;bool dependencies=false;std::wstring native;
            if(!tcard_ack::Read(mailbox_,owner_,applied,nullptr,nullptr,nullptr,&dependencies,&native)||!dependencies||native!=b.native)return false;
            const auto hash=tcard_bridge::Hash(query);if(hash!=query_){query_=hash;reply_.clear();ready_=false;retryAt_=0;}
            auto request=Record(L"source-request",nonce,b);request.Insert(L"Query",JsonValue::CreateStringValue(hash));if(!Put(mailbox_,requestName,request))return false;
            auto result=Record(L"source-result",nonce,b);std::wstring owner,target,receipt;bool ready=false;std::wstring phase=L"waiting-owner";
            if(tcard_bridge::Read(mailbox_,replyName,owner)){
                const auto reply=JsonObject::Parse(owner);
                if(sync_string(reply,L"Request")==nonce){Check(reply,L"source-owner",nonce,b,12);Require(sync_string(reply,L"Query")==hash&&sync_string(reply,L"Challenge")==tcard_bridge::Hash(std::wstring(request.Stringify())),"Source owner challenge differs");
                    target=Target(reply);receipt=tcard_bridge::Hash(owner);phase=L"waiting-originals";
                    if(receipt==reply_&&GetTickCount64()<retryAt_)return true;reply_=receipt;retryAt_=GetTickCount64()+1000;
                    std::wstring committed;Result exported;unsigned version=0;
                    ready=ExportCommittedInventory(replica_,committed,exported,version)&&!selection::Missing(selection::Read(target),selection::Read(committed));if(ready)phase=L"ready";
                }
            }else Require(!tcard_native::Exists(mailbox_+L"\\"+replyName),"Source owner proof unavailable");
            for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Query",hash},{L"Challenge",tcard_bridge::Hash(std::wstring(request.Stringify()))},{L"Reply",receipt},{L"Phase",phase},{L"Target",target},{L"TargetDigest",tcard_bridge::Hash(target)}})result.Insert(name,JsonValue::CreateStringValue(value));
            Require(WaitForSingleObject(parent_,0)==WAIT_TIMEOUT,"Source owner exited");if(!Put(mailbox_,resultName,result))return false;
            ready_=ready;queryStamp_=stamp;tcard_bridge::Fingerprint(mailbox_,replyName,replyStamp_);tcard_bridge::Fingerprint(mailbox_,resultName,resultStamp_);return true;
        }catch(...){return false;}
    }
};
}
