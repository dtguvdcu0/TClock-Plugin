// Included only by card_sync_transport.cpp, after the existing replica-round helpers.
struct PeerConnections::Impl {
    struct Slot {Lease lease;tcard_net::Peer peer;bool preferred=false;};
    ConnectionTiming timing;mutable std::mutex mutex;
    std::map<std::wstring,Slot> nodes;std::map<std::wstring,PeerStatus> states;
    std::atomic<uint64_t> requested=0,revision=0;std::atomic<unsigned short> port=0;uint64_t serial=0;
    explicit Impl(ConnectionTiming value):timing(value){
        if(!value.idle||!value.reply||!value.retry||value.maximum<value.retry||!value.dead||!value.legacy)
            throw std::runtime_error("Invalid connection timing");
    }
};
static std::wstring sync_endpoint(const tcard_net::Peer& peer){return peer.ip+L":"+std::to_wstring(peer.port);}
PeerConnections::PeerConnections(ConnectionTiming timing):impl_(std::make_unique<Impl>(timing)){}
PeerConnections::~PeerConnections()=default;
const ConnectionTiming& PeerConnections::Timing()const{return impl_->timing;}
void PeerConnections::SetPort(unsigned short value){impl_->port=value;}
unsigned short PeerConnections::Port()const{return impl_->port.load();}
uint64_t PeerConnections::Request(){return ++impl_->requested;}
uint64_t PeerConnections::Requested()const{return impl_->requested.load();}
void PeerConnections::Changed(){++impl_->revision;}
uint64_t PeerConnections::Revision()const{return impl_->revision.load();}
PeerConnections::Lease PeerConnections::Admit(const std::wstring& local,const std::wstring& remote,const tcard_net::Peer& peer,bool inbound)
{
    if(!sync_hex(local)||!sync_hex(remote)||local==remote)return {};
    std::lock_guard lock(impl_->mutex);const bool preferred=inbound?(local>remote):(local<remote);
    const auto found=impl_->nodes.find(remote);
    if(found!=impl_->nodes.end()){
        if(found->second.preferred||!preferred)return {};
        found->second.lease.cancelled->store(true);
    }else if(impl_->nodes.size()>=64)return {};
    Lease lease{++impl_->serial,std::make_shared<std::atomic_bool>(false)};
    impl_->nodes[remote]={lease,peer,preferred};return lease;
}
void PeerConnections::Release(uint64_t lease)
{
    std::lock_guard lock(impl_->mutex);
    for(auto i=impl_->nodes.begin();i!=impl_->nodes.end();++i)if(i->second.lease.id==lease){
        const auto key=sync_endpoint(i->second.peer);impl_->nodes.erase(i);
        if(const auto state=impl_->states.find(key);state!=impl_->states.end())state->second.live=false;
        break;
    }
}
void PeerConnections::Publish(const tcard_net::Peer& peer,const SessionReport& report,uint64_t checked,bool live,bool dead)
{
    std::lock_guard lock(impl_->mutex);const auto key=sync_endpoint(peer);
    if(!impl_->states.contains(key)&&impl_->states.size()>=128)impl_->states.erase(impl_->states.begin());
    auto& state=impl_->states[key];state.session=report;state.checked=checked;state.live=live;state.dead=dead;state.serial=++impl_->serial;
}
PeerStatus PeerConnections::Read(const tcard_net::Peer& peer)const
{
    std::lock_guard lock(impl_->mutex);const auto found=impl_->states.find(sync_endpoint(peer));return found==impl_->states.end()?PeerStatus{}:found->second;
}
bool PeerConnections::Active(const tcard_net::Peer& peer)const
{
    std::lock_guard lock(impl_->mutex);const auto key=sync_endpoint(peer);
    return std::any_of(impl_->nodes.begin(),impl_->nodes.end(),[&](const auto& row){return sync_endpoint(row.second.peer)==key&&!row.second.lease.cancelled->load();});
}
bool PeerConnections::Healthy()const
{
    std::lock_guard lock(impl_->mutex);
    for(const auto& row:impl_->nodes){if(row.second.lease.cancelled->load())continue;
        const auto found=impl_->states.find(sync_endpoint(row.second.peer));if(found==impl_->states.end())continue;
        const auto& state=found->second;const auto& session=state.session;
        if(state.live&&!state.dead&&session.connected&&session.authenticated&&session.exchanged&&session.payloadEncrypted&&session.protocol==2&&session.error.empty())return true;
    }return false;
}
struct sync_watch {
    std::wstring path,digest;HANDLE handle=INVALID_HANDLE_VALUE;uint64_t revision=0,audited=0;
    explicit sync_watch(std::wstring storage,PeerConnections& connections):path(std::move(storage)),revision(connections.Revision()){Refresh();}
    ~sync_watch(){if(handle!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(handle);}
    void Refresh(){
        std::wstring inventory;Result result;unsigned version=0;
        if(!ExportCommittedInventory(path,inventory,result,version))throw std::runtime_error("Cannot observe committed replica inventory");
        digest=tcard_asset::hash(tcard_asset::encode(inventory));audited=GetTickCount64();
        if(handle==INVALID_HANDLE_VALUE)handle=FindFirstChangeNotificationW((path+L"\\_sync").c_str(),TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE);
    }
    bool Changed(PeerConnections& connections){
        const auto next=connections.Revision();bool changed=next!=revision;revision=next;
        if(handle==INVALID_HANDLE_VALUE){if(GetTickCount64()-audited<1000)return changed;const auto previous=digest;Refresh();return changed||previous!=digest;}
        if(WaitForSingleObject(handle,0)==WAIT_TIMEOUT)return changed;
        if(!FindNextChangeNotification(handle)){FindCloseChangeNotification(handle);handle=INVALID_HANDLE_VALUE;}
        const auto previous=digest;Refresh();return changed||previous!=digest;
    }
};
struct sync_control {
    session::Tls& tls;session::Io& io;tcard_protocol::Session authenticated;
    bool server;uint64_t sent=0,received=0,round=0;
    sync_control(session::Tls& channel,session::Io& state,bool inbound):tls(channel),io(state),authenticated(state.proof),server(inbound){}
    ~sync_control(){SecureZeroMemory(&authenticated,sizeof(authenticated));}
    void Send(const wchar_t* action,uint64_t value=0){
        JsonObject object;object.Insert(L"Protocol",JsonValue::CreateNumberValue(2));object.Insert(L"Role",JsonValue::CreateStringValue(L"persistent"));
        object.Insert(L"Action",JsonValue::CreateStringValue(action));object.Insert(L"Round",JsonValue::CreateNumberValue(static_cast<double>(value)));
        tcard_protocol::Session context;const auto plain=tcard_asset::encode(std::wstring(object.Stringify()));tcard_crypto::Bytes bytes;
        if(!tcard_protocol::Continue(authenticated,sent+1,true,context)||!tcard_protocol::Seal(*io.secure,context,server,plain,bytes))throw std::runtime_error("Cannot encrypt stream control");
        io.deadline=session::Clock::now()+std::chrono::milliseconds(15000);session::sync_blob(tls,io,server?21:20,bytes,true,4096);++sent;
    }
    std::pair<std::wstring,uint64_t> Read(){
        tcard_crypto::Bytes bytes,plain;tcard_protocol::Session context;
        session::sync_blob(tls,io,server?20:21,bytes,false,4096);
        if(!tcard_protocol::Continue(authenticated,received+1,true,context)||!tcard_protocol::Open(*io.secure,context,!server,bytes,plain))throw std::runtime_error("Invalid stream control sequence");
        const auto text=tcard_asset::decode(plain);if(tcard_asset::encode(text)!=plain)throw std::runtime_error("Invalid stream control encoding");
        const auto object=JsonObject::Parse(text);const double value=object.GetNamedNumber(L"Round",-1);
        if(object.GetNamedNumber(L"Protocol",0)!=2||sync_string(object,L"Role")!=L"persistent"||!std::isfinite(value)||value<0||value>9007199254740991.0||floor(value)!=value)
            throw std::runtime_error("Invalid stream control");
        ++received;return {sync_string(object,L"Action"),static_cast<uint64_t>(value)};
    }
    void Begin(uint64_t value){
        if(value!=round+1||!tcard_protocol::Continue(authenticated,value,false,io.proof))throw std::runtime_error("Invalid stream round");
        round=value;io.preparation={};io.preparation.Begin(session::Clock::now(),io.stop.stop_requested());io.deadline=io.preparation.expires;
        io.dependencyPool.reset();io.dependencyContext={};io.dependencyNote={};io.dependencyTransfer=false;io.dependencyCommitted=false;io.dependencyOffers=0;io.completedChunks.clear();
    }
    bool Ready(){
        if(wolfSSL_pending(tls.value)>0)return true;
        fd_set readable;FD_ZERO(&readable);FD_SET(io.socket,&readable);timeval wait{0,20000};
        const int result=select(0,&readable,nullptr,nullptr,&wait);if(result==SOCKET_ERROR)throw std::runtime_error("Stream receive wait failed");return result>0;
    }
};
// One TLS owner per socket; asynchronous notices never interleave with replica payload frames.
static bool sync_stream(session::Tls& tls,session::Io& io,const std::wstring& path,const Credentials& credentials,
    const tcard_net::Peer& endpoint,bool server,std::shared_ptr<PeerConnections> connections,
    PeerDiscovery* discovery,PeerApplication* application,sync_event_scope& logged)
{
    sync_control channel(tls,io,server);const auto local=tcard_bootstrap::Node(path);
    const auto lease=connections->Admit(local,io.receiver,endpoint,server);
    struct Release {std::shared_ptr<PeerConnections> connections;uint64_t lease;~Release(){if(lease)connections->Release(lease);}} release{connections,lease.id};
    if(server){channel.Send(lease.id?L"accept":L"duplicate");if(!lease.id){logged.successful=true;logged.connected=false;return false;}}
    else{io.deadline=session::Clock::now()+std::chrono::seconds(10);const auto welcome=channel.Read();
        if(welcome.first==L"duplicate"||!lease.id){if(welcome.first==L"accept")channel.Send(L"duplicate");logged.successful=true;logged.connected=false;return false;}
        if(welcome.first!=L"accept"||welcome.second)throw std::runtime_error("Invalid stream admission");}
    io.cancelled=lease.cancelled;logged.stream=true;logged.Complete(L"connected");
    sync_watch watch(path,*connections);const auto timing=connections->Timing();
    SessionReport report;report.connected=report.authenticated=true;report.protocol=2;report.publicAddress=tcard_protocol::Address(io.secure->publicKey);
    connections->Publish(endpoint,report,0,true);
    uint64_t last=GetTickCount64(),requested=0,ping=0,awaiting=0,pingDeadline=0,pendingAt=0;unsigned misses=0;bool dirty=!server;
    while(!io.stop.stop_requested()&&!lease.cancelled->load()){
        const auto now=GetTickCount64();const auto force=connections->Requested();
        const bool changed=watch.Changed(*connections);dirty=dirty||changed||force!=requested;
        if(server){
            if(dirty){channel.Send(L"changed");dirty=false;requested=force;}
            if(now-last>=timing.idle+2*timing.reply)throw std::runtime_error("Stream liveness expired");
        }else{
            if(pendingAt&&now>=pendingAt){dirty=true;pendingAt=0;}
            if(awaiting&&now>=pingDeadline){if(++misses>=2)throw std::runtime_error("Two stream probes were unanswered");channel.Send(L"ping",++ping);awaiting=ping;pingDeadline=now+timing.reply;}
            if(!awaiting&&dirty){
                const auto check=force;SessionReport next;next.connected=next.authenticated=true;next.protocol=2;next.publicAddress=report.publicAddress;
                io.report=&next;selection::Inventory header;std::wstring custody;
                for(unsigned phase=0;phase<2;++phase){
                    const auto round=channel.round+1;channel.Send(L"sync",round);
                    for(;;){io.deadline=session::Clock::now()+std::chrono::seconds(10);const auto message=channel.Read();
                        if(message.first==L"changed")continue;
                        if(message.first!=L"ready"||message.second!=round)throw std::runtime_error("Unexpected stream readiness");break;}
                    channel.Begin(round);logged.stage=L"exchange";
                    if(sync_round(tls,io,path,credentials,endpoint.address,phase,header,custody,next,discovery,application))break;
                }
                io.report=nullptr;report=next;connections->Publish(endpoint,report,check,true);requested=check;dirty=false;last=GetTickCount64();
                if(discovery)try{discovery->Exchanged(endpoint,report);}catch(...){}
                if(report.sentOperations||report.receivedOperations||report.sentAssets||report.receivedAssets)sync_notify(logged.events,logged.peer,L"success",false,L"exchange");
                if(!report.converged)pendingAt=last+5000;
                continue;
            }
            if(!awaiting&&now-last>=timing.idle){channel.Send(L"ping",++ping);awaiting=ping;pingDeadline=now+timing.reply;}
        }
        logged.stage=L"liveness";
        if(!channel.Ready())continue;
        io.deadline=session::Clock::now()+std::chrono::milliseconds(timing.reply);const auto message=channel.Read();
        if(server){
            if(message.first==L"duplicate"){logged.successful=true;return false;}
            if(message.first==L"ping"){channel.Send(L"pong",message.second);last=GetTickCount64();continue;}
            if(message.first!=L"sync"||message.second!=channel.round+1)throw std::runtime_error("Unexpected client stream command");
            const auto check=connections->Requested();channel.Send(L"ready",message.second);channel.Begin(message.second);logged.stage=L"exchange";
            SessionReport next;next.connected=next.authenticated=true;next.protocol=2;next.publicAddress=report.publicAddress;io.report=&next;
            sync_reply(tls,io,path,credentials,endpoint.address,discovery,application,next);io.report=nullptr;report=next;
            connections->Publish(endpoint,report,check,true);last=GetTickCount64();
            if(report.sentOperations||report.receivedOperations||report.sentAssets||report.receivedAssets)sync_notify(logged.events,logged.peer,L"success",true,L"exchange");
        }else if(message.first==L"changed"){dirty=true;last=GetTickCount64();}
        else if(message.first==L"pong"){if(message.second==awaiting){awaiting=0;misses=0;last=GetTickCount64();if(discovery)try{discovery->Exchanged(endpoint,report);}catch(...){}}}
        else throw std::runtime_error("Unexpected server stream command");
    }
    logged.successful=true;return true;
}

struct PeerConnector::Impl {
    std::wstring path;Credentials credentials;tcard_net::Peer peer;tcard_net::Policy policy;
    std::shared_ptr<PeerConnections> connections;std::shared_ptr<PeerDiscovery> discovery;
    std::shared_ptr<PeerApplication> application;std::shared_ptr<PeerEvents> events;std::jthread thread;
    Impl(std::wstring storage,const Credentials& keys,tcard_net::Peer endpoint,tcard_net::Policy access,
        std::shared_ptr<PeerConnections> retained,std::shared_ptr<PeerDiscovery> hints,
        std::shared_ptr<PeerApplication> applied,std::shared_ptr<PeerEvents> logged):
        path(std::move(storage)),credentials(keys),peer(std::move(endpoint)),policy(std::move(access)),connections(std::move(retained)),discovery(std::move(hints)),application(std::move(applied)),events(std::move(logged))
    {
        if(!connections)throw std::runtime_error("Missing connection ownership");
        thread=std::jthread([this](std::stop_token stop){Run(stop);});
    }
    void Run(std::stop_token stop){
        try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            ConnectionRetry retry;bool announced=false;uint64_t legacyAt=0,seenRequest=0,seenRevision=0;
            const auto timing=connections->Timing();
            while(!stop.stop_requested()){
                const auto now=GetTickCount64();const auto requested=connections->Requested();const auto revision=connections->Revision();
                if(connections->Active(peer)){retry.Reachable();announced=false;std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
                if(retry.Dead(now,timing)&&!announced){
                    sync_notify(events.get(),peer,L"unreachable",false,L"connect");const auto state=connections->Read(peer);
                    connections->Publish(peer,state.session,state.checked,false,true);announced=true;
                }
                // Explicit checks override the current delay once; edits never reset failure backoff.
                const bool force=requested!=seenRequest;
                if(!force&&((retry.disconnected&&now<retry.next)||(!retry.disconnected&&now<legacyAt&&revision==seenRevision))){
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;
                }
                seenRequest=requested;seenRevision=revision;SessionReport report;bool okay=false,streamed=false,duplicate=false,attempted=false;
                selection::Inventory header;std::wstring custody;
                try{
                    session::sync_initialize();session::sync_validate(credentials);uint32_t addressValue=0;
                    if(!peer.port||!tcard_net::ParseAddress(peer.ip,addressValue)||!policy.Allows(addressValue))throw std::runtime_error("Peer is outside IP policy");
                    for(unsigned phase=0;phase<2;++phase){
                        attempted=true;sync_event_scope logged{events.get(),peer,false};
                        session::Socket socket(WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,0));if(socket.value==INVALID_SOCKET)throw std::runtime_error("Cannot create peer socket");
                        session::sync_nonblocking(socket.value);sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(peer.port);address.sin_addr.s_addr=htonl(addressValue);
                        if(connect(socket.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))&&WSAGetLastError()!=WSAEWOULDBLOCK)throw std::runtime_error("Peer unavailable");
                        const auto deadline=session::Clock::now()+std::chrono::seconds(3);
                        for(;;){
                            if(stop.stop_requested()||session::Clock::now()>=deadline)throw std::runtime_error("Peer connection cancelled or timed out");
                            fd_set writable,exceptional;FD_ZERO(&writable);FD_SET(socket.value,&writable);FD_ZERO(&exceptional);FD_SET(socket.value,&exceptional);timeval wait{0,20000};
                            const int ready=select(0,nullptr,&writable,&exceptional,&wait);if(ready==SOCKET_ERROR)throw std::runtime_error("Peer connection wait failed");if(ready>0)break;
                        }
                        int pending=0,size=sizeof(pending);if(getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&pending),&size)||pending)throw std::runtime_error("Peer connection failed");
                        logged.connected=true;logged.stage=L"authentication";report.connected=true;
                        session::Context context(false);session::sync_configure(context,false);session::Io io{socket.value,credentials,{},stop};io.path=path;io.report=&report;
                        io.streamWanted=true;io.localPort=connections->Port();session::Tls tls(context.value);session::sync_handshake(tls,io,false);session::sync_authenticate(tls,io,false);
                        report.authenticated=true;report.protocol=2;report.publicAddress=tcard_protocol::Address(io.secure->publicKey);session::sync_bind(path,credentials);logged.stage=L"exchange";
                        if(io.stream){
                            streamed=true;
                            try{okay=sync_stream(tls,io,path,credentials,peer,false,connections,discovery.get(),application.get(),logged);duplicate=!okay;}
                            catch(...){logged.successful=stop.stop_requested()||(io.cancelled&&io.cancelled->load());throw;}
                            break;
                        }
                        const bool done=sync_round(tls,io,path,credentials,addressValue,phase,header,custody,report,discovery.get(),application.get());
                        logged.Complete();if(done){okay=true;break;}
                    }
                }catch(const std::exception& error){session::sync_error(report.error,error);}catch(...){report.error=L"Peer connection failed";}
                if(stop.stop_requested())break;
                if(duplicate||connections->Active(peer)){retry.Reachable();announced=false;legacyAt=GetTickCount64()+timing.retry;continue;}
                if(okay&&!streamed){
                    retry.Reachable();announced=false;legacyAt=GetTickCount64()+timing.legacy;
                    connections->Publish(peer,report,requested,false);
                    if(discovery)try{discovery->Exchanged(peer,report);}catch(...){}
                }else{
                    if(streamed){const auto last=connections->Read(peer);if(last.session.exchanged){retry.Reachable();announced=false;}report=last.session;report.error=L"Persistent connection lost";report.connected=report.authenticated=report.exchanged=report.converged=false;}
                    retry.Failed(GetTickCount64(),timing,((peer.address+peer.port)%11)*1000);
                    connections->Publish(peer,report,connections->Requested(),false,retry.Dead(GetTickCount64(),timing));
                    if(!attempted)sync_notify(events.get(),peer,L"failure",false,L"policy");
                }
            }
        }catch(...){SessionReport report;report.error=L"Connection owner failed";connections->Publish(peer,report,connections->Requested(),false);}
    }
    void Stop(){if(thread.joinable()){thread.request_stop();thread.join();}}
    ~Impl(){Stop();SecureZeroMemory(credentials.transportKey.data(),credentials.transportKey.size());}
};
PeerConnector::PeerConnector(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,
    const tcard_net::Policy& policy,std::shared_ptr<PeerConnections> connections,std::shared_ptr<PeerDiscovery> discovery,
    std::shared_ptr<PeerApplication> application,std::shared_ptr<PeerEvents> events):
    impl_(std::make_unique<Impl>(path,credentials,peer,policy,std::move(connections),std::move(discovery),std::move(application),std::move(events))){}
PeerConnector::~PeerConnector()=default;
void PeerConnector::Stop(){impl_->Stop();}
