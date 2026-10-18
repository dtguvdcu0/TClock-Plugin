#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <vector>

// Public connection metadata only: no note, seed, storage or data-key APIs.
// Signaling schema v1 is independent of retired sync wire v1; synchronization remains 21-word/v2.
namespace tcard_signal {
inline constexpr unsigned protocolVersion=1,defaultPort=42378,maxHops=4,maxFanout=3,maxConnections=16;
inline constexpr size_t maxMessage=65536,maxNodes=32,maxRoutes=128,maxEndpoints=8,maxLeases=512,maxRequests=128,maxSeen=512;
inline constexpr uint64_t leaseSeconds=90,requestSeconds=15,cacheSeconds=30ull*86400,maxSequence=9007199254740991ull;
inline uint64_t Now(){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
inline bool Hex(const std::wstring& value,size_t count){return value.size()==count&&value.find_first_not_of(L"0123456789abcdef")==std::wstring::npos;}
inline bool IPv4(const std::wstring& ip)
{
    if(ip.empty()||ip.size()>15)return false;size_t at=0;
    for(unsigned part=0;part<4;++part){const auto end=ip.find(L'.',at),last=end==std::wstring::npos?ip.size():end;const auto text=ip.substr(at,last-at);
        if(text.empty()||text.size()>3||text.find_first_not_of(L"0123456789")!=std::wstring::npos||(text.size()>1&&text[0]==L'0'))return false;
        unsigned value=0;for(auto c:text)value=value*10+c-L'0';if(value>255||(part<3&&end==std::wstring::npos)||(part==3&&end!=std::wstring::npos))return false;at=last+1;
    }return ip!=L"0.0.0.0"&&ip!=L"255.255.255.255";
}
struct Node {std::wstring ip,name,id;unsigned short port=0;uint64_t first=0,success=0,expires=0;}; // id is the configured exact leaf DER SHA-256 pin.
struct Capsule {std::wstring route,endpoint,ip,publicKey,signature;unsigned short port=0;uint64_t sequence=0,expires=0;};
struct PublicChallenge {std::wstring node,nonce,tlsBinding;}; // Constructed only from the verified live channel.
struct Submission {Capsule capsule;std::wstring node,nonce,tlsBinding,proof;};
struct Query {std::wstring id,route,origin;uint64_t expires=0;std::vector<std::wstring> path;};
struct Reply {std::wstring id,route;std::vector<Capsule> capsules;};
bool ValidCapsule(const Capsule& value,uint64_t now) noexcept;
// Callback remains local to the credential-owning worker and signs only this fixed registration schema.
using RegistrationSigner=std::function<Submission(const PublicChallenge&)>;
class NodeClient {
    Node node_;std::stop_token stop_;
public:
    explicit NodeClient(const Node& node,std::stop_token stop={}):node_(node),stop_(stop){}
    bool Register(const Capsule& capsule,const RegistrationSigner& signer,std::wstring& error);
    bool Lookup(const std::wstring& route,std::vector<Capsule>& results,std::wstring& error);
};
// Monotonic token buckets are local resource budgets, never group authorization.
class Budget {
    uint64_t tokens_=0,stamp_=0;bool started_=false;
public:
    bool Take(uint64_t tick,unsigned burst,unsigned perSecond){
        if(!started_){tokens_=uint64_t(burst)*1000;stamp_=tick;started_=true;}
        if(tick>stamp_){const auto elapsed=(std::min)(tick-stamp_,uint64_t(burst)*1000);
            tokens_=(std::min)(uint64_t(burst)*1000,tokens_+elapsed*perSecond);stamp_=tick;}
        if(tokens_<1000)return false;tokens_-=1000;return true;
    }
};
class Admission {
    struct Source {Budget budget;uint64_t last=0;};
    Budget global_;std::map<uint32_t,Source> sources_;
public:
    static constexpr size_t capacity=512;
    bool Take(uint32_t source,uint64_t tick){
        for(auto it=sources_.begin();it!=sources_.end();)if(tick>=it->second.last&&tick-it->second.last>=120000)it=sources_.erase(it);else ++it;
        auto found=sources_.find(source);
        if(found==sources_.end()){if(sources_.size()>=capacity)return false;found=sources_.emplace(source,Source{}).first;}
        found->second.last=tick;
        // Source refusal does not spend another source's process-wide allowance.
        return found->second.budget.Take(tick,32,2)&&global_.Take(tick,96,12);
    }
    size_t Size()const{return sources_.size();}
};
class LookupCache {
    struct Entry {bool busy=true;uint64_t until=0;std::vector<Capsule> capsules;};
    std::map<std::wstring,Entry> entries_;
public:
    static std::wstring Key(const Query& query){
        auto path=query.path;std::sort(path.begin(),path.end());std::wstring key=query.route;
        for(const auto& hop:path)key+=L":"+hop;return key;
    }
    bool Begin(const std::wstring& key,uint64_t tick,std::vector<Capsule>& cached){
        cached.clear();
        for(auto it=entries_.begin();it!=entries_.end();)if(!it->second.busy&&it->second.until<=tick)it=entries_.erase(it);else ++it;
        const auto found=entries_.find(key);
        if(found!=entries_.end()){cached=found->second.capsules;return false;}
        if(entries_.size()>=maxRequests)return false;
        entries_.emplace(key,Entry{});return true;
    }
    void Finish(const std::wstring& key,const std::vector<Capsule>& capsules,uint64_t tick){
        const auto found=entries_.find(key);if(found==entries_.end())return;
        found->second.capsules=capsules;found->second.until=tick+(capsules.empty()?1000:5000);found->second.busy=false;
    }
    void Cancel(const std::wstring& key){entries_.erase(key);}
    size_t Size()const{return entries_.size();}
};
class Router {
    struct Lease {Capsule capsule;uint64_t first=0;};
    std::map<std::wstring,std::map<std::wstring,Lease>> routes_;
    std::map<std::wstring,uint64_t> seen_;
public:
    void Expire(uint64_t now){
        for(auto i=routes_.begin();i!=routes_.end();){for(auto j=i->second.begin();j!=i->second.end();)if(j->second.capsule.expires<=now)j=i->second.erase(j);else ++j;if(i->second.empty())i=routes_.erase(i);else ++i;}
        for(auto i=seen_.begin();i!=seen_.end();)if(i->second<=now)i=seen_.erase(i);else ++i;
    }
    size_t LeaseCount()const{size_t count=0;for(const auto& route:routes_)count+=route.second.size();return count;}
    bool Register(const Capsule& capsule,uint64_t now){
        Expire(now);if(!ValidCapsule(capsule,now))return false;auto route=routes_.find(capsule.route);
        if(route==routes_.end()&&routes_.size()>=maxRoutes)return false;
        if(route!=routes_.end()){auto found=route->second.find(capsule.endpoint);if(found!=route->second.end()){
            if(capsule.sequence<=found->second.capsule.sequence)return false;found->second.capsule=capsule;return true; // Repetition never renews first receipt.
        }if(route->second.size()>=maxEndpoints)return false;}
        if(LeaseCount()>=maxLeases)return false;routes_[capsule.route][capsule.endpoint]={capsule,now};return true;
    }
    std::vector<Capsule> Lookup(const std::wstring& route,uint64_t now){
        Expire(now);std::vector<Capsule> result;auto found=routes_.find(route);if(found!=routes_.end())for(const auto& entry:found->second)result.push_back(entry.second.capsule);return result;
    }
    bool Remember(const std::wstring& id,uint64_t expires,uint64_t now){
        Expire(now);if(!Hex(id,64)||expires<=now||expires-now>requestSeconds||seen_.contains(id)||seen_.size()>=maxSeen)return false;seen_.emplace(id,expires);return true;
    }
    size_t RouteCount()const{return routes_.size();}
    size_t RequestCount()const{return seen_.size();}
};
}
