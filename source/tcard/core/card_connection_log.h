#pragma once
#include "card_peer_cache.h"
#include "card_sync_transport.h"
#include <mutex>

// Local diagnostics only: never an endpoint candidate, authorization or delivery receipt.
namespace tcard_log {
using tcard_peer::JsonObject;
using tcard_peer::JsonArray;
using tcard_peer::JsonValue;
inline constexpr wchar_t filename[]=L"connection-log.json";
inline constexpr size_t capacity=256;
struct Event {
    tcard_net::Peer peer;
    uint64_t time=0;
    std::wstring outcome,stage;
    bool inbound=false;
};
inline bool Valid(const std::wstring& outcome,const std::wstring& stage)
{
    return (outcome==L"success"||outcome==L"failure"||outcome==L"disconnect"||outcome==L"unreachable")&&
        (stage==L"policy"||stage==L"connect"||stage==L"authentication"||stage==L"exchange"||stage==L"closed"||stage==L"cancelled"||stage==L"connected"||stage==L"liveness");
}
inline bool Read(const std::wstring& profile,std::vector<Event>& events)
{
    events.clear();try{
        const auto identity=tcard_peer::Identity(profile);if(identity.empty())return false;
        const auto attributes=GetFileAttributesW((profile+L"\\"+filename).c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND;
        std::wstring text;if(!tcard_bridge::Read(profile,filename,text)||text.size()>65536)return false;
        JsonObject root;if(!JsonObject::TryParse(text,root)||root.GetNamedNumber(L"Version",0)!=1||root.GetNamedString(L"Identity",L"")!=identity)return false;
        const auto rows=root.GetNamedArray(L"Events");if(rows.Size()>capacity)return false;std::vector<Event> parsed;
        for(const auto& value:rows){const auto row=value.GetObject();Event event;
            const double stamp=row.GetNamedNumber(L"Time",0);if(!std::isfinite(stamp)||stamp<=0||stamp>9007199254740991.0||std::floor(stamp)!=stamp)return false;
            const auto endpoint=std::wstring(row.GetNamedString(L"Endpoint",L""));std::vector<tcard_net::Peer> peers;
            if(!tcard_net::ParsePeers(endpoint,0,peers)||peers.size()!=1||endpoint!=peers[0].ip+L":"+std::to_wstring(peers[0].port))return false;
            event.peer=peers[0];event.time=static_cast<uint64_t>(stamp);event.outcome=row.GetNamedString(L"Outcome",L"");event.stage=row.GetNamedString(L"Stage",L"");event.inbound=row.GetNamedBoolean(L"Inbound");
            if(!Valid(event.outcome,event.stage))return false;parsed.push_back(std::move(event));
        }events=std::move(parsed);return true;
    }catch(...){events.clear();return false;}
}
inline bool Clear(const std::wstring& profile)
{
    if(tcard_peer::Identity(profile).empty())return false;
    return DeleteFileW((profile+L"\\"+filename).c_str())||GetLastError()==ERROR_FILE_NOT_FOUND;
}
class Log final:public tcard_sync::PeerEvents {
    std::mutex mutex_;
    std::wstring profile_,identity_;
    std::vector<Event> events_;
    bool healthy_=false,dirty_=false;
    bool Publish()
    {
        if(!healthy_||tcard_peer::Identity(profile_)!=identity_)return false;
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Identity",JsonValue::CreateStringValue(identity_));JsonArray rows;
        for(const auto& event:events_){JsonObject row;
            row.Insert(L"Time",JsonValue::CreateNumberValue(static_cast<double>(event.time)));row.Insert(L"Endpoint",JsonValue::CreateStringValue(event.peer.ip+L":"+std::to_wstring(event.peer.port)));
            row.Insert(L"Outcome",JsonValue::CreateStringValue(event.outcome));row.Insert(L"Stage",JsonValue::CreateStringValue(event.stage));row.Insert(L"Inbound",JsonValue::CreateBooleanValue(event.inbound));rows.Append(row);
        }root.Insert(L"Events",rows);if(!tcard_bridge::Publish(profile_,filename,root.Stringify().c_str()))return false;dirty_=false;return true;
    }
public:
    explicit Log(const std::wstring& profile):profile_(profile),identity_(tcard_peer::Identity(profile)){healthy_=!identity_.empty()&&Read(profile_,events_);}
    void Record(const tcard_net::Peer& peer,const wchar_t* outcome,bool inbound,const wchar_t* stage)override
    {
        std::lock_guard lock(mutex_);if(!healthy_||!Valid(outcome,stage))return;
        std::vector<tcard_net::Peer> parsed;if(!tcard_net::ParsePeers(peer.ip+L":"+std::to_wstring(peer.port),0,parsed)||parsed.size()!=1)return;
        // An inbound remote port is ephemeral diagnostic metadata, never a listening endpoint.
        events_.push_back({peer,tcard_peer::Now(),outcome,stage,inbound});
        if(events_.size()>capacity)events_.erase(events_.begin(),events_.begin()+(events_.size()-capacity));
        dirty_=true;Publish();
    }
    void Flush(){std::lock_guard lock(mutex_);if(dirty_)Publish();}
};
}
