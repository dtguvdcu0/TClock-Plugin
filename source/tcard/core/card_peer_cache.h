#pragma once
#include "card_sync_bridge.h"
#include "card_sync_network.h"
#include "card_storage.h"
#include <cmath>

// Endpoint history is discovery metadata, never an authorization or delivery receipt.
namespace tcard_peer {
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonValue;
inline constexpr uint64_t lifetime=30ULL*24*60*60;
inline constexpr size_t capacity=128;
inline constexpr wchar_t filename[]=L"peer-cache.json";
struct Entry {
    tcard_net::Peer peer;
    std::wstring network;
    uint64_t success=0;
};
inline uint64_t Now()
{
    FILETIME time{};GetSystemTimeAsFileTime(&time);ULARGE_INTEGER ticks{};
    ticks.LowPart=time.dwLowDateTime;ticks.HighPart=time.dwHighDateTime;return ticks.QuadPart/10000000;
}
inline std::wstring Identity(const std::wstring& profile)
{
    try{tcard_crypto::Keys keys;if(!tcard_storage::Required(profile)||!tcard_storage::LoadKey(profile,keys))return {};
        return tcard_asset::hash({keys.publicKey.begin(),keys.publicKey.end()});}catch(...){return {};}
}
inline std::wstring Network(const tcard_net::Policy& policy)
{
    std::vector<std::wstring> networks;
    for(const auto& rule:policy.lan)networks.push_back(tcard_net::FormatAddress(rule.network)+L"/"+tcard_net::FormatAddress(rule.mask));
    std::sort(networks.begin(),networks.end());networks.erase(std::unique(networks.begin(),networks.end()),networks.end());
    std::wstring text;for(const auto& network:networks){if(!text.empty())text+=L";";text+=network;}return text.empty()?L"":tcard_asset::hash(tcard_asset::encode(text));
}
inline bool Read(const std::wstring& profile,const std::wstring& identity,std::vector<Entry>& entries,uint64_t now=Now())
{
    entries.clear();try{
        if(identity.empty()||Identity(profile)!=identity)return false;
        const auto attributes=GetFileAttributesW((profile+L"\\"+filename).c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES)return GetLastError()==ERROR_FILE_NOT_FOUND;
        std::wstring text;if(!tcard_bridge::Read(profile,filename,text)||text.size()>65536)return false;
        JsonObject root;if(!JsonObject::TryParse(text,root)||root.GetNamedNumber(L"Version")!=1||root.GetNamedString(L"Identity")!=identity)return false;
        const auto array=root.GetNamedArray(L"Entries");if(array.Size()>capacity)return false;
        std::vector<Entry> parsed;
        for(const auto& value:array){
            const auto item=value.GetObject();const double stamp=item.GetNamedNumber(L"LastSuccess");
            if(!std::isfinite(stamp)||stamp<=0||stamp>9007199254740991.0||std::floor(stamp)!=stamp)return false;
            const auto endpoint=std::wstring(item.GetNamedString(L"Endpoint"));std::vector<tcard_net::Peer> peers;
            if(!tcard_net::ParsePeers(endpoint,0,peers)||peers.size()!=1||endpoint!=peers[0].ip+L":"+std::to_wstring(peers[0].port))return false;
            const auto network=std::wstring(item.GetNamedString(L"Network"));if(network.size()!=64||network.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos)return false;
            const auto success=static_cast<uint64_t>(stamp);
            // Clock rollback cannot turn a future timestamp into an indefinitely valid entry.
            if(success>now||now-success>=lifetime)continue;
            if(std::any_of(parsed.begin(),parsed.end(),[&](const Entry& e){return e.peer.address==peers[0].address&&e.peer.port==peers[0].port&&e.network==network;}))return false;
            parsed.push_back({peers[0],network,success});
        }
        entries=std::move(parsed);return true;
    }catch(...){entries.clear();return false;}
}
inline bool Clear(const std::wstring& profile)
{
    // Caller must own the stopped worker's profile mutex. Never touch keys, notes or manual peers.
    if(Identity(profile).empty())return false;
    bool cleared=true;
    for(const auto* name:{filename,L"peer-hints.json"})if(!DeleteFileW((profile+L"\\"+name).c_str())&&GetLastError()!=ERROR_FILE_NOT_FOUND)cleared=false;
    return cleared;
}
class Cache {
    std::wstring profile_,identity_,network_;
    std::vector<Entry> entries_;
    uint64_t flushed_=0;
    bool healthy_=false,dirty_=false;
public:
    Cache(const std::wstring& profile,const tcard_net::Policy& policy):profile_(profile),identity_(Identity(profile)),network_(Network(policy))
    {
        healthy_=!network_.empty()&&Read(profile_,identity_,entries_);
        // Rewrite once to physically remove expired entries; malformed files are preserved for explicit clearing.
        dirty_=healthy_&&GetFileAttributesW((profile_+L"\\"+filename).c_str())!=INVALID_FILE_ATTRIBUTES;
    }
    const std::wstring& PublicIdentity()const{return identity_;}
    bool Healthy()const{return healthy_;}
    std::vector<Entry> Entries(uint64_t now=Now())const
    {
        std::vector<Entry> current;
        if(healthy_)for(const auto& entry:entries_)if(entry.network==network_&&entry.success<=now&&now-entry.success<lifetime)current.push_back(entry);
        return current;
    }
    bool Flush(uint64_t now=Now(),bool force=false)
    {
        if(!healthy_)return false;
        const auto count=entries_.size();
        entries_.erase(std::remove_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.success>now||now-e.success>=lifetime;}),entries_.end());
        dirty_=dirty_||entries_.size()!=count;
        if(!dirty_)return true;if(!force&&flushed_&&now>=flushed_&&now-flushed_<60)return true;
        if(Identity(profile_)!=identity_)return false;
        JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Identity",JsonValue::CreateStringValue(identity_));JsonArray array;
        for(const auto& entry:entries_){JsonObject item;
            item.Insert(L"Endpoint",JsonValue::CreateStringValue(entry.peer.ip+L":"+std::to_wstring(entry.peer.port)));
            item.Insert(L"Network",JsonValue::CreateStringValue(entry.network));item.Insert(L"LastSuccess",JsonValue::CreateNumberValue(static_cast<double>(entry.success)));array.Append(item);}
        root.Insert(L"Entries",array);
        if(!tcard_bridge::Publish(profile_,filename,root.Stringify().c_str()))return false;
        dirty_=false;flushed_=now;return true;
    }
    bool Record(const tcard_net::Peer& peer,const std::wstring& identity,uint64_t now=Now())
    {
        if(!healthy_||identity!=identity_||!now)return false;
        std::vector<tcard_net::Peer> parsed;
        if(!tcard_net::ParsePeers(peer.ip+L":"+std::to_wstring(peer.port),0,parsed)||parsed.size()!=1||parsed[0].address!=peer.address)return false;
        auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.peer.address==peer.address&&e.peer.port==peer.port&&e.network==network_;});
        const bool fresh=found==entries_.end();
        if(fresh){
            if(entries_.size()>=capacity){const auto oldest=std::min_element(entries_.begin(),entries_.end(),[](const Entry& a,const Entry& b){return a.success<b.success;});entries_.erase(oldest);}
            entries_.push_back({parsed[0],network_,now});
        }else found->success=now;
        dirty_=true;return Flush(now,fresh);
    }
};
}
