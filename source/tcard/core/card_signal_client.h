#pragma once
#include "card_peer_discovery.h"
#include "card_sync_words.h"
#include "card_native_root.h"
#include "../signaling/signal_transport.h"
#include <bcrypt.h>
#include <limits>

// Worker-only adapter. UI settings include only the pure public bridge parsers.
namespace tcard_signal_worker {
using namespace tcard_sync;
struct Config {bool enabled=false;std::vector<tcard_bridge::SignalNode> nodes;tcard_net::Peer published;};
struct NodeStamp {std::wstring endpoint,pin;uint64_t success=0;};
// Registration/backoff remains independent of this optional lookup cadence.
struct LookupSchedule {
    uint64_t last=0;bool attempted=false;
    bool Due(uint64_t tick,bool direct)const{return !attempted||tick<last||tick-last>=(direct?300000ull:30000ull);}
    void Complete(uint64_t tick){last=tick;attempted=true;}
    void Wake(){attempted=false;}
};
class Client {
    std::wstring profile_,identity_,publicKey_,route_,endpoint_,saved_;
    Config config_;tcard_net::Policy policy_;tcard_crypto::Keys signing_;
    std::vector<tcard_bridge::SignalNode> active_;
    std::vector<NodeStamp> history_;
    struct Retry {ULONGLONG next=0;unsigned failures=0;LookupSchedule lookup;};
    std::map<std::wstring,Retry> retries_;
    struct Seen {uint64_t sequence=0,expires=0;std::wstring digest;};
    std::map<std::wstring,Seen> seen_;
    uint64_t sequence_=0,registeredUntil_=0,lastExchange_=0;ULONGLONG flushed_=0,nextFlush_=0;bool healthy_=true,dirty_=false,publish_=false,direct_=false;
    static std::wstring Endpoint(const tcard_net::Peer& peer){return peer.ip+L":"+std::to_wstring(peer.port);}
    static void Require(bool okay,const char* error){if(!okay)throw std::runtime_error(error);}
    static std::wstring NodeKey(const tcard_bridge::SignalNode& node){return Endpoint(node.peer)+L"@"+node.pin;}
    void Failover(size_t slot,ULONGLONG tick)
    {
        for(const auto& node:config_.nodes){if(!policy_.Allows(node.peer.address))continue;const auto key=NodeKey(node);
            if(std::any_of(active_.begin(),active_.end(),[&](const auto& current){return NodeKey(current)==key;}))continue;
            const auto retry=retries_.find(key);if(retry!=retries_.end()&&retry->second.next>tick)continue;
            active_[slot]=node;return;
        }
    }
    void Load()
    {
        try{
            const auto path=profile_+L"\\signal-node-cache.json";const auto attributes=GetFileAttributesW(path.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES){Require(GetLastError()==ERROR_FILE_NOT_FOUND,"Signaling node cache unavailable");return;}
            tcard_bridge::Stamp stamp;Require(tcard_bridge::Fingerprint(profile_,L"signal-node-cache.json",stamp)&&!stamp.sizeHigh&&stamp.sizeLow<=65536+tcard_crypto::envelopeOverhead,"Signaling node cache size exceeded");
            std::wstring text;Require(tcard_bridge::Read(profile_,L"signal-node-cache.json",text)&&text.size()<=65536,"Signaling node cache unreadable");
            const auto record=JsonObject::Parse(text);Require(record.Size()==3&&record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Identity")==identity_,"Signaling node cache identity differs");
            const auto rows=record.GetNamedArray(L"Entries");Require(rows.Size()<=tcard_signal::maxNodes,"Signaling node cache count exceeded");
            std::set<std::wstring> unique;
            for(const auto& row:rows){const auto item=row.GetObject();NodeStamp node{sync_string(item,L"Endpoint"),sync_string(item,L"Pin"),0};
                const auto success=item.GetNamedNumber(L"LastSuccess");std::vector<tcard_net::Peer> parsed;
                Require(item.Size()==3&&std::isfinite(success)&&success>0&&success<=9007199254740991.0&&success==std::floor(success)&&
                    tcard_signal::Hex(node.pin,64)&&tcard_net::ParsePeers(node.endpoint,0,parsed)&&parsed.size()==1&&Endpoint(parsed[0])==node.endpoint&&unique.insert(node.endpoint+L"@"+node.pin).second,"Signaling node cache record differs");
                node.success=static_cast<uint64_t>(success);history_.push_back(std::move(node));
            }
            saved_=std::wstring(record.Stringify());
        }catch(...){healthy_=false;history_.clear();} // Corrupt optional metadata never substitutes for configured pins.
    }
    void Record(const tcard_bridge::SignalNode& node,uint64_t now)
    {
        if(!healthy_)return;const auto endpoint=Endpoint(node.peer);
        auto item=std::find_if(history_.begin(),history_.end(),[&](const NodeStamp& value){return value.endpoint==endpoint&&value.pin==node.pin;});
        if(item==history_.end()){
            if(history_.size()>=tcard_signal::maxNodes){auto oldest=std::min_element(history_.begin(),history_.end(),[](const auto& a,const auto& b){return a.success<b.success;});history_.erase(oldest);}
            history_.push_back({endpoint,node.pin,now});
        }else item->success=now;dirty_=true;
    }
    tcard_signal::Capsule Capsule(uint64_t now)
    {
        Require(sequence_<tcard_signal::maxSequence&&now<=UINT64_MAX-tcard_signal::leaseSeconds,"Signaling capsule counter exhausted");
        tcard_signal::Capsule value;value.route=route_;value.endpoint=endpoint_;value.ip=config_.published.ip;value.port=config_.published.port;
        value.publicKey=publicKey_;value.sequence=++sequence_;value.expires=now+tcard_signal::leaseSeconds;
        tcard_crypto::Bytes proof;Require(tcard_crypto::Sign(signing_,tcard_signal::CapsuleProof(value),proof),"Signaling capsule signing failed");
        value.signature=tcard_signal::Encode(proof.data(),proof.size());return value;
    }
    tcard_signal::Submission RegisterProof(const tcard_signal::Capsule& capsule,const std::wstring& pin,const tcard_signal::PublicChallenge& challenge)
    {
        // Only the verified live node channel supplies this context. There is no arbitrary transcript entry point.
        Require(challenge.node==pin&&tcard_signal::Hex(challenge.nonce,64)&&tcard_signal::Hex(challenge.tlsBinding,64)&&
            capsule.publicKey==publicKey_&&capsule.route==route_&&capsule.endpoint==endpoint_,"Signaling registration context differs");
        tcard_signal::Submission value{capsule,challenge.node,challenge.nonce,challenge.tlsBinding,{}};tcard_crypto::Bytes proof;
        Require(tcard_crypto::Sign(signing_,tcard_signal::SubmissionProof(value),proof),"Signaling registration signing failed");
        value.proof=tcard_signal::Encode(proof.data(),proof.size());return value;
    }
    bool Admit(const tcard_signal::Capsule& value,tcard_peer::Discovery& discovery,uint64_t wireNow)
    {
        if(value.endpoint==endpoint_||value.publicKey!=publicKey_||value.route!=route_||!tcard_signal::ValidCapsule(value,wireNow))return false;
        std::vector<tcard_net::Peer> peers;Require(tcard_net::ParsePeers(value.ip+L":"+std::to_wstring(value.port),0,peers)&&peers.size()==1,"Signaling candidate endpoint differs");
        const auto digest=tcard_signal::CapsuleDigest(value);const auto found=seen_.find(value.endpoint);
        if(found!=seen_.end()&&(value.sequence<found->second.sequence||(value.sequence==found->second.sequence&&digest!=found->second.digest)))return false;
        if(found==seen_.end()&&seen_.size()>=tcard_signal::maxEndpoints)return false;
        if(!discovery.AcceptSignaled(peers[0],tcard_peer::Now(),value.expires-wireNow))return false;
        seen_[value.endpoint]={value.sequence,value.expires,digest};return true;
    }
public:
    Client(const std::wstring& profile,const Credentials& credentials,const Config& config,const tcard_net::Policy& policy,bool listening):profile_(tcard_native::Extended(sync_path_plain(profile))),config_(config),policy_(policy)
    {
        Require(config_.enabled&&!config_.nodes.empty()&&config_.nodes.size()<=tcard_signal::maxNodes,"Signaling node configuration absent");
        auto suffix=tcard_words::Suffix(credentials.transportKey.data());const bool current=std::equal(suffix.begin(),suffix.begin()+4,credentials.transportKey.begin()+28);SecureZeroMemory(suffix.data(),suffix.size());
        Require(current,"Signaling requires current recovery credentials");Credentials bound=credentials;
        struct BoundScope{Credentials& value;~BoundScope(){SecureZeroMemory(value.transportKey.data(),value.transportKey.size());}} boundScope{bound};
        tcard_words::Finish(bound);const bool same=bound.space==credentials.space;SecureZeroMemory(bound.transportKey.data(),bound.transportKey.size());
        Require(same,"Signaling recovery key association differs");tcard_crypto::Seed seed{};
        struct SeedScope{tcard_crypto::Seed& value;~SeedScope(){SecureZeroMemory(value.data(),value.size());}} seedScope{seed};
        std::copy_n(credentials.transportKey.begin(),seed.size(),seed.begin());
        const bool derived=tcard_crypto::Derive(seed,signing_);SecureZeroMemory(seed.data(),seed.size());Require(derived,"Signaling identity derivation failed");
        publicKey_=tcard_signal::Encode(signing_.publicKey.data(),signing_.publicKey.size());route_=tcard_signal::Route(publicKey_);identity_=tcard_asset::hash({signing_.publicKey.begin(),signing_.publicKey.end()});
        Require(identity_==tcard_peer::Identity(profile_),"Signaling worker profile identity differs");
        std::array<BYTE,16> nonce{};Require(BCryptGenRandom(nullptr,nonce.data(),static_cast<ULONG>(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Signaling endpoint identity unavailable");endpoint_=tcard_signal::Encode(nonce.data(),nonce.size());
        publish_=listening&&config_.published.port!=0;Load();const auto now=tcard_peer::Now();
        const auto success=[&](const auto& node){uint64_t latest=0;for(const auto& stamp:history_)if(stamp.endpoint==Endpoint(node.peer)&&stamp.pin==node.pin&&stamp.success<=now&&now-stamp.success<tcard_signal::cacheSeconds)latest=stamp.success;return latest;};
        std::stable_sort(config_.nodes.begin(),config_.nodes.end(),[&](const auto& a,const auto& b){return success(a)>success(b);});
        for(const auto& node:config_.nodes)if(policy_.Allows(node.peer.address)&&active_.size()<tcard_signal::maxFanout)active_.push_back(node);
    }
    ~Client(){Flush(true);}
    bool Flush(bool force=false) noexcept
    {
        try{
            if(!healthy_||!dirty_)return healthy_;const auto tick=GetTickCount64();if(!force&&((nextFlush_&&tick<nextFlush_)||(flushed_&&tick-flushed_<600000)))return true;
            nextFlush_=tick+60000; // Failed publication retries are independently bounded, including the first attempt.
            const auto now=tcard_peer::Now();history_.erase(std::remove_if(history_.begin(),history_.end(),[&](const NodeStamp& node){return node.success>now||now-node.success>=tcard_signal::cacheSeconds;}),history_.end());
            JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Identity",JsonValue::CreateStringValue(identity_));JsonArray rows;
            for(const auto& node:history_){JsonObject item;item.Insert(L"Endpoint",JsonValue::CreateStringValue(node.endpoint));item.Insert(L"Pin",JsonValue::CreateStringValue(node.pin));item.Insert(L"LastSuccess",JsonValue::CreateNumberValue(static_cast<double>(node.success)));rows.Append(item);}
            record.Insert(L"Entries",rows);const auto text=std::wstring(record.Stringify());
            if(text!=saved_&&(identity_!=tcard_peer::Identity(profile_)||!tcard_bridge::Publish(profile_,L"signal-node-cache.json",text)))return false;
            saved_=text;dirty_=false;flushed_=tick;nextFlush_=0;return true;
        }catch(...){return false;}
    }
    bool Step(tcard_peer::Discovery& discovery,std::stop_token stop,bool& registered,size_t& accepted,bool direct=false)
    {
        accepted=0;const auto tick=GetTickCount64(),wireNow=tcard_signal::Now();registered=registeredUntil_>wireNow;
        for(auto item=seen_.begin();item!=seen_.end();)if(item->second.expires<=wireNow)item=seen_.erase(item);else ++item;
        if(direct_&&!direct)for(auto& row:retries_){row.second.lookup.Wake();if(!row.second.failures)row.second.next=0;}
        direct_=direct;bool exchanged=false,quiet=false;
        // Successful slots stay sticky; backoff never causes a sweep through all configured nodes.
        for(size_t slot=0;slot<active_.size();++slot){const auto node=active_[slot];
            if(stop.stop_requested())break;
            if(!policy_.Allows(node.peer.address))continue;const auto key=Endpoint(node.peer)+L"@"+node.pin;auto& retry=retries_[key];
            // An intentionally quiet lookup-only client retains its last successful status until the next due lookup.
            if(direct&&!publish_&&!retry.failures&&retry.lookup.attempted&&!retry.lookup.Due(tick,true))quiet=true;
            if(retry.next>tick)continue;
            const bool lookup=retry.lookup.Due(tick,direct);if(!publish_&&!lookup)continue;
            tcard_signal::Node remote;remote.ip=node.peer.ip;remote.port=node.peer.port;remote.id=node.pin;tcard_signal::NodeClient channel(remote,stop);std::wstring error;bool okay=false;
            try{
            if(publish_){const auto capsule=Capsule(tcard_signal::Now());
                const bool sent=channel.Register(capsule,[&](const auto& challenge){return RegisterProof(capsule,node.pin,challenge);},error);registered=registered||sent;if(sent)registeredUntil_=(std::max)(registeredUntil_,capsule.expires);okay=sent;}
            if(stop.stop_requested())break;std::vector<tcard_signal::Capsule> results;
            if(lookup){retry.lookup.Complete(GetTickCount64());if(channel.Lookup(route_,results,error)){
                Require(results.size()<=tcard_signal::maxEndpoints,"Signaling lookup count exceeded");okay=true;
                for(const auto& value:results)if(!stop.stop_requested()&&Admit(value,discovery,tcard_signal::Now()))++accepted;
            }}
            }catch(...){okay=false;} // Optional node failures back off without stopping direct synchronization.
            if(okay){Record(node,tcard_peer::Now());exchanged=true;lastExchange_=tcard_signal::Now();retry.failures=0;}else retry.failures=(std::min)(retry.failures+1,7u);
            const auto delay=okay?30ULL:(std::min)(30ULL<<retry.failures,1800ULL);retry.next=GetTickCount64()+delay*1000+(node.peer.address+node.peer.port)%11000;
            if(!okay&&!stop.stop_requested())Failover(slot,GetTickCount64());
        }
        Flush();return quiet||exchanged||(lastExchange_&&wireNow>=lastExchange_&&wireNow-lastExchange_<tcard_signal::leaseSeconds);
    }
};
}
