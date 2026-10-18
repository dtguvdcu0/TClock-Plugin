// Included only by card_sync_transport.cpp; owner controls use existing v2 private proof and payload encryption.
static void sync_owner_reply(session::Tls& tls,session::Io& io,const std::wstring& path)
{
    sync_source_require(io.sourceProvider&&io.selection&&io.targets&&io.objects&&!io.dependencies,"Saved owner source is unsupported");
    std::wstring packet;size_t pending=0;session::sync_payload(tls,io,false,packet,pending,false,nullptr,nullptr,nullptr,true);
    const auto query=JsonObject::Parse(packet);const auto request=sync_string(query,L"Request"),source=tcard_bootstrap::Node(path),group=tcard_chunks::Group(path);
    sync_source_require(query.Size()==6&&query.GetNamedNumber(L"Version")==1&&sync_string(query,L"Kind")==L"owner-source-query"&&sync_hex(request)&&
        sync_string(query,L"Source")==source&&sync_string(query,L"Receiver")==io.receiver&&sync_string(query,L"Group")==group,"Saved owner remote request differs");
    const auto proof=io.sourceProvider->Capture(request,io.receiver,io.stop);
    if(!proof.empty())tcard_source_witness::Check(proof,request,source,io.receiver,group);
    JsonObject result;result.Insert(L"Version",JsonValue::CreateNumberValue(1));
    for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",L"owner-source-result"},{L"Request",request},{L"Source",source},{L"Receiver",io.receiver},{L"Group",group},{L"Proof",proof}})result.Insert(name,JsonValue::CreateStringValue(value));
    packet=result.Stringify();session::sync_payload(tls,io,true,packet,pending,true,nullptr,nullptr,nullptr,true);
}
static bool sync_owner_request(sync_source_state& source,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SessionReport& report,std::stop_token stop)
{
    report={};try{
        session::sync_initialize();session::sync_validate(credentials);uint32_t addressValue=0;
        sync_source_require(peer.port&&tcard_net::ParseAddress(peer.ip,addressValue)&&policy.Allows(addressValue),"Saved source IP is outside policy");
        session::Socket socket(WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,0));sync_source_require(socket.value!=INVALID_SOCKET,"Cannot create saved source socket");session::sync_nonblocking(socket.value);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(peer.port);address.sin_addr.s_addr=htonl(addressValue);
        const auto connected=connect(socket.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address));
        sync_source_require(!connected||WSAGetLastError()==WSAEWOULDBLOCK,"Saved source unavailable");const auto deadline=session::Clock::now()+std::chrono::seconds(3);
        for(;;){
            sync_source_require(!stop.stop_requested()&&session::Clock::now()<deadline,"Saved source connection cancelled or timed out");
            fd_set ready;FD_ZERO(&ready);FD_SET(socket.value,&ready);timeval timeout{0,20000};const auto selected=select(0,nullptr,&ready,nullptr,&timeout);
            sync_source_require(selected!=SOCKET_ERROR,"Saved source connection wait failed");if(selected>0)break;
        }
        int error=0,size=sizeof(error);sync_source_require(!getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)&&!error,"Saved source connection failed");
        report.connected=true;session::Context context(false);session::sync_configure(context,false);session::Io io{socket.value,credentials,{},stop};
        io.path=source.path;io.report=&report;io.sourceOnly=true;io.ownerWanted=true;session::Tls tls(context.value);session::sync_handshake(tls,io,false);session::sync_authenticate(tls,io,false);
        report.authenticated=true;report.protocol=2;report.publicAddress=tcard_protocol::Address(io.secure->publicKey);sync_source_bind(source,io);
        sync_source_require(io.ownerPeer,"Saved owner source is unsupported");
        const auto receiver=tcard_bootstrap::Node(source.path),group=tcard_chunks::Group(source.path);JsonObject query;query.Insert(L"Version",JsonValue::CreateNumberValue(1));
        for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",L"owner-source-query"},{L"Request",source.ownerRequest},{L"Source",source.node},{L"Receiver",receiver},{L"Group",group}})query.Insert(name,JsonValue::CreateStringValue(value));
        std::wstring packet(query.Stringify());size_t pending=0;session::sync_payload(tls,io,false,packet,pending,true,nullptr,nullptr,nullptr,true);session::sync_payload(tls,io,true,packet,pending,false,nullptr,nullptr,nullptr,true);report.payloadEncrypted=true;
        const auto result=JsonObject::Parse(packet);sync_source_require(result.Size()==7&&result.GetNamedNumber(L"Version")==1&&sync_string(result,L"Kind")==L"owner-source-result"&&
            sync_string(result,L"Request")==source.ownerRequest&&sync_string(result,L"Source")==source.node&&sync_string(result,L"Receiver")==receiver&&sync_string(result,L"Group")==group,"Saved source response differs");
        const auto proof=sync_string(result,L"Proof");
        if(proof.empty()){sync_source_require(source.ownerProof.empty()&&source.target.empty(),"Pinned saved source witness unavailable");return true;}
        const auto target=tcard_source_witness::Check(proof,source.ownerRequest,source.node,receiver,group);
        if(!source.ownerProof.empty())sync_source_require(source.ownerProof==proof&&source.target==target,"Pinned saved source witness changed");
        else{sync_source_require(source.target.empty(),"Saved source target lacks initial proof");source.ownerProof=proof;source.target=target;sync_source_write(source);}
        return true;
    }catch(const std::exception& e){session::sync_error(report.error,e);return false;}catch(...){report.error=L"Saved source request failed";return false;}
}
