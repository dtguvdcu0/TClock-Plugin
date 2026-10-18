#pragma once
#include "signal_transport.h"
#include <thread>

namespace tcard_signal {
struct Options {
    std::wstring ip=L"0.0.0.0";
    unsigned short port=defaultPort;
    std::vector<Node> peers;
    bool allowLoopback=false; // Component-fixture option only; never read from production configuration.
};
struct Activity {uint64_t admitted=0,limited=0,lookups=0,forwarded=0,coalesced=0,registrations=0;};
class NodeRuntime {
    Options options_;std::wstring identity_;std::shared_ptr<Context> server_,client_;
    Router router_;Admission admission_;Budget forwarding_;LookupCache cache_;Activity activity_;std::mutex mutex_;std::wstring firstFailure_;std::atomic<unsigned> incoming_=0,inflight_=0;
    struct Active {std::shared_ptr<std::atomic<bool>> done;std::jthread thread;};
    void Failure(const std::exception* error)noexcept{try{std::lock_guard guard(mutex_);if(!firstFailure_.empty())return;
        std::wstring message=L"Signal server request refused";if(error){const char* raw=error->what();size_t count=0;while(count<384&&raw[count])++count;
            if(count<384){const std::string text(raw,count),prefix="Signal TLS failed code=";const auto numeric=[](const std::string& value){const auto first=value.starts_with('-')?size_t(1):size_t(0);
                return value.size()>first&&value.size()<=11&&value.find_first_not_of("0123456789",first)==std::string::npos;};
                if(text.starts_with(prefix)){const auto split=text.find(" WSA=",prefix.size());if(split!=std::string::npos&&numeric(text.substr(prefix.size(),split-prefix.size()))&&numeric(text.substr(split+5)))message.assign(text.begin(),text.end());}
                else{constexpr const char* fixed[]={"Signal TLS exporter unavailable","Signal TLS connection unavailable","Signal TLS version or resumption refused",
                    "Signal peer certificate missing","Signal peer certificate size invalid","Signal certificate invalid","Signal certificate expired or not yet valid",
                    "Signal unconfigured mesh certificate refused","Signal connection cancelled or timed out","Signal connection quota exceeded"};
                    for(const auto* value:fixed)if(text==value){message.assign(text.begin(),text.end());break;}}
            }}firstFailure_=std::move(message);
        }catch(...){ /* Failure diagnostics cannot affect socket cleanup or routing admission. */ }}
    bool Endpoint(const Capsule& value)const{
        if(!IPv4(value.ip)||!value.port)return false;const auto first=std::stoul(value.ip.substr(0,value.ip.find(L'.')));
        return first>0&&first<224&&(first!=127||options_.allowLoopback)&&!value.ip.starts_with(L"169.254.");
    }
    bool Known(const std::wstring& id)const{return Hex(id,64)&&std::any_of(options_.peers.begin(),options_.peers.end(),[&](const Node& peer){return peer.id==id;});}
    static uint64_t Tick(){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
    Reply RouteQuery(Query query,std::stop_token stop){
        const auto now=Now();Require(Hex(query.id,64)&&Hex(query.route,64)&&Hex(query.origin,64)&&query.expires>now&&query.expires-now<=requestSeconds&&
            !query.path.empty()&&query.path.size()<=maxHops&&query.path.front()==query.origin,"Signal mesh query invalid");
        std::set<std::wstring> path;for(const auto& hop:query.path)Require(Hex(hop,64)&&path.insert(hop).second,"Signal mesh path invalid");
        const auto count=inflight_.fetch_add(1);if(count>=maxRequests){--inflight_;throw std::runtime_error("Signal request quota exceeded");}
        struct Flight{std::atomic<unsigned>& value;~Flight(){--value;}} flight{inflight_};Reply reply{query.id,query.route,{}};bool fresh=false;
        const auto cacheKey=LookupCache::Key(query);bool owner=false;
        {std::lock_guard guard(mutex_);++activity_.lookups;reply.capsules=router_.Lookup(query.route,now);fresh=router_.Remember(query.id,query.expires,now);
            if(fresh&&query.path.size()<maxHops&&reply.capsules.size()<maxEndpoints){std::vector<Capsule> cached;owner=cache_.Begin(cacheKey,Tick(),cached);
                if(!owner){++activity_.coalesced;std::set<std::wstring> endpoints;for(const auto& capsule:reply.capsules)endpoints.insert(capsule.endpoint);
                    for(const auto& capsule:cached)if(capsule.expires>now&&endpoints.insert(capsule.endpoint).second&&reply.capsules.size()<maxEndpoints)reply.capsules.push_back(capsule);}}}
        if(!owner)return reply;
        // A cancelled/failed owner must release its coalescing slot, including allocation failures.
        struct Cached {NodeRuntime& node;const std::wstring& key;bool complete=false;
            ~Cached(){if(!complete){std::lock_guard guard(node.mutex_);node.cache_.Cancel(key);}}} cached{*this,cacheKey};
        const auto limit=Clock::now()+std::chrono::seconds((std::min)(query.expires-now,requestSeconds));std::set<std::wstring> endpoints;
        for(const auto& capsule:reply.capsules)endpoints.insert(capsule.endpoint);unsigned fanout=0;
        for(const auto& peer:options_.peers){if(stop.stop_requested()||Clock::now()>=limit||reply.capsules.size()>=maxEndpoints)break;
            if(path.contains(peer.id)||peer.id==identity_)continue;if(fanout++>=maxFanout)break;
            {std::lock_guard guard(mutex_);if(!forwarding_.Take(Tick(),24,6))break;++activity_.forwarded;}
            try{Channel channel(peer,stop,client_,limit);Writer hello(Kind::Mesh);channel.Write(hello.data);Writer request(Kind::Query);Put(request,query);channel.Write(request.data);
                Reader input(channel.Read());auto returned=GetReply(input);Require(returned.id==query.id&&returned.route==query.route,"Signal mesh return identity invalid");
                for(const auto& capsule:returned.capsules){Require(capsule.route==query.route&&ValidCapsule(capsule,Now())&&Endpoint(capsule),"Signal mesh return capsule invalid");
                    if(endpoints.insert(capsule.endpoint).second&&reply.capsules.size()<maxEndpoints)reply.capsules.push_back(capsule);}
            }catch(...){if(stop.stop_requested())throw; /* Independent pinned alternatives remain eligible. */}
        }
        {std::lock_guard guard(mutex_);if(!stop.stop_requested())cache_.Finish(cacheKey,reply.capsules,Tick());else cache_.Cancel(cacheKey);cached.complete=true;}
        return reply;
    }
    void Serve(SOCKET socket,std::stop_token stop){Channel channel(socket,stop,server_);const auto peer=channel.Peer();
        // Anonymous workers authenticate their fixed registration with group possession proof.
        // Mesh connections must present a configured node certificate before any routing metadata is read.
        const bool mesh=!peer.empty();if(mesh)Require(Known(peer),"Signal unconfigured mesh certificate refused");Reader first(channel.Read());
        if(mesh){Require(first.kind==Kind::Mesh,"Signal mesh role invalid");first.End();Reader input(channel.Read());Require(input.kind==Kind::Query,"Signal mesh query role invalid");
            auto query=GetQuery(input);input.End();Require(!query.path.empty()&&query.path.back()==peer&&std::find(query.path.begin(),query.path.end(),identity_)==query.path.end(),"Signal mesh source or loop invalid");
            Require(query.path.size()<maxHops,"Signal mesh hop bound exceeded");query.path.push_back(identity_);const auto reply=RouteQuery(std::move(query),stop);Writer out(Kind::Reply);Put(out,reply);channel.Write(out.data);return;}
        if(first.kind==Kind::Client){first.End();PublicChallenge challenge{identity_,RandomId(),L""};challenge.tlsBinding=channel.Binding(identity_,challenge.nonce);const auto expires=Clock::now()+std::chrono::seconds(5);channel.Limit(expires);
            Writer out(Kind::Challenge);out.Text(challenge.node,64);out.Text(challenge.nonce,64);out.Text(challenge.tlsBinding,64);channel.Write(out.data);
            Reader input(channel.Read());Require(input.kind==Kind::Register,"Signal registration role invalid");Submission submission;submission.capsule=GetCapsule(input);submission.node=input.Text(64);submission.nonce=input.Text(64);submission.tlsBinding=input.Text(64);submission.proof=input.Text(160);input.End();
            // Exactly one response is accepted on this connection; the nonce is consumed even on refusal.
            bool accepted=false;if(Clock::now()<expires&&ValidSubmission(submission,challenge,Now())&&Endpoint(submission.capsule)){
                std::lock_guard guard(mutex_);accepted=router_.Register(submission.capsule,Now());if(accepted)++activity_.registrations;}
            challenge.nonce.clear();Writer ack(Kind::Ack);ack.Number(accepted?1:0,1);channel.Write(ack.data);return;}
        Require(first.kind==Kind::Lookup,"Signal client request role invalid");auto query=GetQuery(first);first.End();Require(query.origin.empty()&&query.path.empty(),"Signal client cannot claim mesh authority");
        query.origin=identity_;query.path.push_back(identity_);const auto reply=RouteQuery(std::move(query),stop);Writer out(Kind::Reply);Put(out,reply);channel.Write(out.data);
    }
public:
    // Test-owned component use shares exactly this router/TLS runtime, but is not a product launch bypass.
    // The production executable verifies the AppContainer token and owner GO before constructing it.
    NodeRuntime(const Options& options,const Bytes& certDER,const Bytes& keyPKCS8):options_(options),identity_(CertificateIdentity(certDER)){
        Require((options.ip==L"0.0.0.0"||IPv4(options.ip))&&options.peers.size()<=maxNodes,"Signal node configuration invalid");std::set<std::wstring> unique;
        for(const auto& peer:options_.peers)Require(IPv4(peer.ip)&&peer.port&&Hex(peer.id,64)&&peer.id!=identity_&&unique.insert(peer.id).second,"Signal configured mesh peer invalid");
        server_=std::make_shared<Context>(true,certDER,keyPKCS8);client_=std::make_shared<Context>(false,certDER,keyPKCS8);
    }
    const std::wstring& Identity()const{return identity_;}
    std::wstring FirstFailure(){std::lock_guard guard(mutex_);return firstFailure_;}
    Activity ReadActivity(){std::lock_guard guard(mutex_);return activity_;}
    size_t LeaseCount(){std::lock_guard guard(mutex_);router_.Expire(Now());return router_.LeaseCount();}
    bool Run(std::stop_token stop,const std::function<void(unsigned short)>& onBound,std::wstring& error){error.clear();try{Network();Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));Require(listener.value!=INVALID_SOCKET,"Signal listener unavailable");
            // Exclusive binding never replaces another service or changes firewall policy.
            BOOL exclusive=TRUE;Require(setsockopt(listener.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0,"Signal exclusive listener failed");
            const auto address=Address(options_.ip,options_.port,true);if(bind(listener.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==SOCKET_ERROR)throw std::runtime_error("Signal listener bind failed WSA="+std::to_string(WSAGetLastError()));
            Require(listen(listener.value,12)==0,"Signal listener listen failed");Nonblocking(listener.value);sockaddr_in local{};int size=sizeof(local);Require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&local),&size)==0,"Signal bound endpoint unavailable");if(onBound)onBound(ntohs(local.sin_port));
            std::stop_source ownedStop;std::stop_callback cancelled(stop,[&]{ownedStop.request_stop();});
            const auto ownedToken=ownedStop.get_token();std::vector<Active> active;active.reserve(12);
            struct Cancel{std::stop_source& source;~Cancel(){source.request_stop();}} cancel{ownedStop};
            while(!ownedToken.stop_requested()){
                for(auto it=active.begin();it!=active.end();)if(it->done->load())it=active.erase(it);else ++it;
                fd_set set{};FD_ZERO(&set);FD_SET(listener.value,&set);timeval timeout{0,50000};const auto ready=select(0,&set,nullptr,nullptr,&timeout);if(ready==SOCKET_ERROR)throw std::runtime_error("Signal listener wait failed WSA="+std::to_string(WSAGetLastError()));if(!ready)continue;
                sockaddr_in source{};int sourceSize=sizeof(source);Socket accepted(accept(listener.value,reinterpret_cast<sockaddr*>(&source),&sourceSize));if(accepted.value==INVALID_SOCKET){Require(WSAGetLastError()==WSAEWOULDBLOCK,"Signal listener accept failed");continue;}
                // Budget the actual IPv4 source before handler allocation or TLS. Never use its ephemeral port or a supplied route ID.
                {std::lock_guard guard(mutex_);if(sourceSize!=static_cast<int>(sizeof(source))||source.sin_family!=AF_INET||active.size()>=12||incoming_.load()>=12||!admission_.Take(ntohl(source.sin_addr.s_addr),Tick())){++activity_.limited;continue;}++activity_.admitted;}
                auto done=std::make_shared<std::atomic<bool>>(false);
                auto holder=std::make_unique<Socket>(accepted.value);accepted.value=INVALID_SOCKET;
                // Allocate the entry before a thread can own the socket; no container mutation follows launch.
                active.emplace_back();active.back().done=done;++incoming_;
                try{active.back().thread=std::jthread([this,holder=std::move(holder),ownedToken,done]()mutable{
                    const auto socket=holder->value;holder->value=INVALID_SOCKET;
                    try{Serve(socket,ownedToken);}catch(const std::exception& error){Failure(&error);}catch(...){Failure(nullptr);}--incoming_;done->store(true);
                });}catch(...){--incoming_;active.pop_back();throw;}
            }
            // Listener failure and caller cancellation request the owned token before handler joins.
            ownedStop.request_stop();active.clear();return true;
        }catch(const std::exception& e){error=Error(e);return false;}catch(...){error=L"Signal node runtime failed";return false;}}
};
} // namespace tcard_signal
