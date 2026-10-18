#pragma once
#include "card_peer_cache.h"
#include "card_sync_transport.h"
#include <mutex>
#include <map>
#include <functional>

namespace tcard_peer {
// The worker owns this shared object until both listener and exchange threads stop.
class Discovery final:public tcard_sync::PeerDiscovery {
    struct Hint {tcard_net::Peer peer;uint64_t first=0,expires=0;bool signaled=false;};
    struct Retry {uint64_t next=0;unsigned failures=0;};
    std::mutex mutex_;
    Cache cache_;
    std::wstring profile_,network_;
    tcard_net::Policy policy_;
    std::vector<std::wstring> local_;
    unsigned short port_;
    bool enabled_,signaling_=false,healthy_=true;
    std::function<void()> changed_;
    std::vector<Hint> hints_;
    std::map<std::wstring,uint64_t> leases_; // At most one live lease per bounded retained first receipt; never persisted.
    std::map<std::wstring,Retry> retries_;
    static std::wstring Endpoint(const tcard_net::Peer& peer){return peer.ip+L":"+std::to_wstring(peer.port);}
    static bool Number(const JsonObject& item,const wchar_t* key,uint64_t& value)
    {
        const double number=item.GetNamedNumber(key,0);
        if(!std::isfinite(number)||number<=0||number>9007199254740991.0||std::floor(number)!=number)return false;
        value=static_cast<uint64_t>(number);return true;
    }
    static bool Parse(const JsonObject& item,tcard_net::Peer& peer)
    {
        std::vector<tcard_net::Peer> parsed;const std::wstring text(item.GetNamedString(L"Endpoint",L""));
        if(!tcard_net::ParsePeers(text,0,parsed)||parsed.size()!=1||Endpoint(parsed[0])!=text)return false;
        peer=parsed[0];return true;
    }
    bool Allowed(const tcard_net::Peer& peer)const
    {
        if(!policy_.Allows(peer.address))return false;
        if(peer.port==port_&&((peer.address>>24)==127||std::find(local_.begin(),local_.end(),peer.ip)!=local_.end()))return false;
        // Exclude subnet network/broadcast addresses before any automatic socket attempt.
        for(const auto& rule:policy_.lan)if((peer.address&rule.mask)==rule.network&&rule.mask<0xfffffffeu&&
            (peer.address==rule.network||peer.address==(rule.network|~rule.mask)))return false;
        return true;
    }
    bool Shareable(const tcard_net::Peer& peer,uint32_t remote)const
    {
        if(!Allowed(peer)||!policy_.Allows(remote))return false;
        if((peer.address>>24)==127||(remote>>24)==127)return (peer.address>>24)==127&&(remote>>24)==127;
        for(const auto& rule:policy_.lan)if((peer.address&rule.mask)==rule.network&&(remote&rule.mask)==rule.network)return true;
        return false;
    }
    bool Save()
    {
        if(!healthy_||Identity(profile_)!=cache_.PublicIdentity())return false;
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Identity",JsonValue::CreateStringValue(cache_.PublicIdentity()));
        root.Insert(L"Network",JsonValue::CreateStringValue(network_));JsonArray rows;
        for(const auto& hint:hints_){JsonObject item;item.Insert(L"Endpoint",JsonValue::CreateStringValue(Endpoint(hint.peer)));
            item.Insert(L"First",JsonValue::CreateNumberValue(static_cast<double>(hint.first)));item.Insert(L"Expires",JsonValue::CreateNumberValue(static_cast<double>(hint.expires)));
            if(hint.signaled)item.Insert(L"Signaled",JsonValue::CreateBooleanValue(true));rows.Append(item);}
        root.Insert(L"Entries",rows);return tcard_bridge::Publish(profile_,L"peer-hints.json",root.Stringify().c_str());
    }
    void Load()
    {
        try{
            if(!cache_.Healthy()){healthy_=false;return;}
            if(GetFileAttributesW((profile_+L"\\peer-hints.json").c_str())==INVALID_FILE_ATTRIBUTES){healthy_=GetLastError()==ERROR_FILE_NOT_FOUND;return;}
            std::wstring text;if(!tcard_bridge::Read(profile_,L"peer-hints.json",text)||text.size()>65536)throw std::runtime_error("Invalid hints");
            const auto root=JsonObject::Parse(text);
            if(root.GetNamedNumber(L"Version")!=1||root.GetNamedString(L"Identity")!=cache_.PublicIdentity()||root.GetNamedString(L"Network")!=network_)throw std::runtime_error("Hint context changed");
            const auto rows=root.GetNamedArray(L"Entries");if(rows.Size()>capacity)throw std::runtime_error("Hint capacity exceeded");
            for(const auto& value:rows){const auto item=value.GetObject();Hint hint;
                if(!Parse(item,hint.peer)||!Number(item,L"First",hint.first)||!Number(item,L"Expires",hint.expires)||hint.expires>hint.first+lifetime||
                    std::any_of(hints_.begin(),hints_.end(),[&](const Hint& h){return Endpoint(h.peer)==Endpoint(hint.peer);}))throw std::runtime_error("Invalid hint receipt");
                hint.signaled=item.GetNamedBoolean(L"Signaled",false);hints_.push_back(hint);}
        }catch(...){healthy_=false;hints_.clear();}
    }
public:
    Discovery(const std::wstring& profile,const tcard_net::Policy& policy,bool enabled,unsigned short port=0,std::vector<std::wstring> local={},bool signaling=false,std::function<void()> changed={}):
        cache_(profile,policy),profile_(profile),network_(Network(policy)),policy_(policy),local_(std::move(local)),port_(port),enabled_(enabled),signaling_(signaling),changed_(std::move(changed)){if(enabled_||signaling_)Load();}
    std::wstring Offer(uint32_t remote)override
    {
        std::lock_guard lock(mutex_);if(!enabled_||!healthy_)return {};
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));JsonArray rows;
        auto entries=cache_.Entries();std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return a.success>b.success;});
        for(const auto& entry:entries)if(Shareable(entry.peer,remote)){
            JsonObject item;item.Insert(L"Endpoint",JsonValue::CreateStringValue(Endpoint(entry.peer)));
            item.Insert(L"Expires",JsonValue::CreateNumberValue(static_cast<double>(entry.success+lifetime)));rows.Append(item);if(rows.Size()==32)break;}
        root.Insert(L"Entries",rows);return std::wstring(root.Stringify());
    }
    void Accept(const std::wstring& offer,uint32_t remote)override{AcceptAt(offer,remote,Now());}
    void AcceptAt(const std::wstring& offer,uint32_t remote,uint64_t now)
    {
        std::lock_guard lock(mutex_);if(!enabled_||!healthy_||offer.size()>8192||!policy_.Allows(remote))return;
        try{
            const auto root=JsonObject::Parse(offer);if(root.GetNamedNumber(L"Version")!=1)return;
            const auto rows=root.GetNamedArray(L"Entries");if(rows.Size()>32)return;
            auto updated=hints_;const auto verified=cache_.Entries(now);
            for(const auto& value:rows){const auto item=value.GetObject();Hint hint;hint.first=now;
                if(!Parse(item,hint.peer)||!Number(item,L"Expires",hint.expires)||hint.expires<=now||hint.expires>now+lifetime||!Shareable(hint.peer,remote))continue;
                if(std::any_of(verified.begin(),verified.end(),[&](const Entry& e){return Endpoint(e.peer)==Endpoint(hint.peer);}))continue;
                // Keep bounded expired receipt tombstones: repetition cannot renew first receipt, even after restart.
                if(std::any_of(updated.begin(),updated.end(),[&](const Hint& h){return Endpoint(h.peer)==Endpoint(hint.peer);}))continue;
                if(updated.size()<capacity)updated.push_back(hint);
            }
            if(updated.size()!=hints_.size()){const auto previous=hints_;hints_=std::move(updated);if(!Save())hints_=previous;}
        }catch(...){/* Optional malformed introductions do not affect note synchronization. */}
    }
    // Caller already verified the fixed capsule against this worker's current public identity.
    // Lease seconds are converted from Unix wire time; first receipts use local FILETIME cache time.
    bool AcceptSignaled(const tcard_net::Peer& peer,uint64_t now,uint64_t leaseSeconds)
    {
        std::lock_guard lock(mutex_);
        if(!signaling_||!healthy_||!leaseSeconds||leaseSeconds>90||now>UINT64_MAX-lifetime||!Allowed(peer))return false;
        const auto endpoint=Endpoint(peer);const auto verified=cache_.Entries(now);
        if(std::any_of(verified.begin(),verified.end(),[&](const Entry& entry){return Endpoint(entry.peer)==endpoint;}))return true;
        auto found=std::find_if(hints_.begin(),hints_.end(),[&](const Hint& hint){return Endpoint(hint.peer)==endpoint;});
        if(found!=hints_.end()){
            if(found->first>now||now-found->first>=lifetime)return false;
            leases_[endpoint]=(std::min)(found->first+lifetime,now+leaseSeconds);
            return true; // Keep the old receipt and LAN expiry; signed online admission is independent.
        }
        if(hints_.size()>=capacity)return false;
        hints_.push_back({peer,now,now+lifetime,true});
        // Failed first-receipt publication leaves neither a new hint nor a live lease admitted.
        if(!Save()){hints_.pop_back();return false;}
        leases_[endpoint]=now+leaseSeconds;return true;
    }
    bool Retained(const tcard_net::Peer& peer,uint64_t now=Now())
    {
        std::lock_guard lock(mutex_);if((!enabled_&&!signaling_)||!healthy_||!Allowed(peer))return false;
        const auto endpoint=Endpoint(peer);
        for(const auto& entry:cache_.Entries(now))if(Endpoint(entry.peer)==endpoint)return true;
        for(const auto& hint:hints_)if(Endpoint(hint.peer)==endpoint&&hint.first<=now&&now-hint.first<lifetime){
            const auto lease=leases_.find(endpoint);
            if((enabled_&&!hint.signaled&&hint.expires>now)||(signaling_&&lease!=leases_.end()&&lease->second>now))return true;
        }
        return false;
    }
    std::vector<tcard_net::Peer> Candidates(const std::vector<tcard_net::Peer>& manual,uint64_t now=Now())
    {
        std::lock_guard lock(mutex_);std::vector<tcard_net::Peer> result;if((!enabled_&&!signaling_)||!healthy_)return result;
        auto append=[&](const tcard_net::Peer& peer){
            if(!Allowed(peer)||std::any_of(manual.begin(),manual.end(),[&](const auto& p){return Endpoint(p)==Endpoint(peer);})||
                std::any_of(result.begin(),result.end(),[&](const auto& p){return Endpoint(p)==Endpoint(peer);}))return;
            const auto retry=retries_.find(Endpoint(peer));if(retry!=retries_.end()&&retry->second.next>now)return;
            if(result.size()<2)result.push_back(peer);
        };
        for(const auto& entry:cache_.Entries(now))append(entry.peer);
        for(const auto& hint:hints_)if(hint.first<=now&&now-hint.first<lifetime){
            const auto lease=leases_.find(Endpoint(hint.peer));const bool signedLive=signaling_&&lease!=leases_.end()&&lease->second>now;
            const bool lanLive=enabled_&&!hint.signaled&&hint.expires>now;
            if(lanLive||signedLive)append(hint.peer);
        }
        return result;
    }
    void Exchanged(const tcard_net::Peer& peer,const tcard_sync::SessionReport& report)override{Complete(peer,report);}
    void Complete(const tcard_net::Peer& peer,const tcard_sync::SessionReport& report,uint64_t now=Now())
    {
        std::lock_guard lock(mutex_);const bool success=report.authenticated&&report.exchanged&&report.protocol==2&&report.payloadEncrypted;
        if(success){
            const auto previous=cache_.Entries(now);
            const bool known=std::any_of(previous.begin(),previous.end(),[&](const Entry& entry){return Endpoint(entry.peer)==Endpoint(peer);});
            cache_.Record(peer,report.publicAddress,now);
            // Notify retained sessions only for new verified membership, never for timestamp renewal or unverified hints.
            if(enabled_&&changed_&&!known){const auto current=cache_.Entries(now);
                if(std::any_of(current.begin(),current.end(),[&](const Entry& entry){return Endpoint(entry.peer)==Endpoint(peer);}))changed_();}
        }
        if(!enabled_&&!signaling_)return;
        // Bounded retry state, one exchange at a time; failures back off to 30 minutes.
        const auto endpoint=Endpoint(peer);if(!retries_.contains(endpoint)&&retries_.size()>=capacity*2)retries_.erase(retries_.begin());
        auto& retry=retries_[endpoint];retry.failures=success?0:(std::min)(retry.failures+1,7u);
        const uint64_t delay=success?30:(std::min)(30ULL<<retry.failures,1800ULL);
        retry.next=now+delay+(peer.address+peer.port)%11;
    }
    void Flush(){std::lock_guard lock(mutex_);cache_.Flush();}
    ~Discovery(){try{cache_.Flush(Now(),true);}catch(...){}}
};
}
