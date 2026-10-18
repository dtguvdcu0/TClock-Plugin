#pragma once
// Included inside tcard_sync::session after authenticated chunk framing; all helpers share the fresh Io proof.
struct sync_dependency_scope {
    Io& io;tcard_remote::DependencyWindow window;
    explicit sync_dependency_scope(Io& value):io(value)
    {
        // One actual request and one actual reply at most; retries create a new Io/session.
        if(io.dependencyTransfer||io.dependencyOffers>=2)throw std::runtime_error("Too many dependency offers");
        window.Begin(Clock::now(),io.stop.stop_requested());++io.dependencyOffers;io.dependencyDeadline=window.expires;io.dependencyTransfer=true;
    }
    ~sync_dependency_scope(){io.dependencyTransfer=false;}
    void Check()const{window.Check(Clock::now(),io.stop.stop_requested());}
    void Object(){io.chunkDeadline=window.Limit(Clock::now(),std::chrono::seconds(60),io.stop.stop_requested());}
};
static tcard_dependencies::Context sync_dependency_context(const Io& io,const std::wstring& packet,bool receiving)
{
    if(!io.dependencies||!io.secure||!io.selection||!io.objects||!io.targets||!io.custody)throw std::runtime_error("Unnegotiated dependencies");
    const auto boundary=JsonObject::Parse(packet).GetNamedObject(L"RemoteBootstrap");const auto local=tcard_bootstrap::Node(io.path),group=tcard_asset::hash({io.secure->publicKey.begin(),io.secure->publicKey.end()});
    const auto target=tcard_blobs::Canonical(std::wstring(boundary.GetNamedObject(L"Target").Stringify()));
    if(boundary.GetNamedNumber(L"Version")!=1||sync_string(boundary,L"Kind")!=L"remote-bootstrap"||sync_string(boundary,L"Source")!=(receiving?io.receiver:local)||
        sync_string(boundary,L"Receiver")!=(receiving?local:io.receiver)||sync_string(boundary,L"Group")!=group||group!=tcard_chunks::Group(io.path)||sync_string(boundary,L"TargetDigest")!=tcard_bridge::Hash(target))
        throw std::runtime_error("Authenticated dependency context differs");
    const tcard_dependencies::Context context{io.credentials.space,group,receiving?io.receiver:local,receiving?local:io.receiver,target};std::wstring manifest;
    tcard_remote::DependencyUnwrap(packet,context,manifest);const auto parsed=tcard_dependencies::Read(manifest,context);
    if(JsonObject::Parse(parsed.original).GetNamedNumber(L"Version")>io.operations)throw std::runtime_error("Peer must be updated to synchronize authored dates");return context;
}
static JsonObject sync_dependency_notice(const wchar_t* role,const std::wstring& transfer,const std::wstring& object)
{
    JsonObject value;value.Insert(L"Protocol",JsonValue::CreateNumberValue(2));value.Insert(L"Role",JsonValue::CreateStringValue(role));value.Insert(L"Transfer",JsonValue::CreateStringValue(transfer));value.Insert(L"Object",JsonValue::CreateStringValue(object));return value;
}
static void sync_dependency_check(const JsonObject& value,const wchar_t* role,const std::wstring& transfer,const std::wstring& object)
{
    if(value.GetNamedNumber(L"Protocol")!=2||sync_string(value,L"Role")!=role||sync_string(value,L"Transfer")!=transfer||sync_string(value,L"Object")!=object)throw std::runtime_error("Dependency control binding differs");
}
static void sync_dependency_send(Tls& tls,Io& io,bool server,const JsonObject& data,const std::wstring& packet)
{
    sync_dependency_scope scope(io);const auto context=sync_dependency_context(io,packet,false);const auto package=std::wstring(JsonObject::Parse(packet).GetNamedObject(L"Delta").Stringify());const auto manifest=tcard_dependencies::Read(package,context);
    tcard_dependencies::SourceReader reader(io.path,package,context);const auto transfer=tcard_dependencies::Digest(tcard_dependencies::Encode(manifest));
    scope.Check();scope.Object();auto offer=JsonObject::Parse(std::wstring(data.Stringify()));offer.SetNamedValue(L"Role",JsonValue::CreateStringValue(L"dependency-offer"));
    auto encoded=tcard_asset::encode(std::wstring(offer.Stringify()));sync_chunk_frame(tls,io,server,server?2:1,encoded,true,tcard_asset::max_file);if(io.report){io.report->objects=true;io.report->chunked=true;}
    for(const auto& descriptor:manifest.descriptors){
        scope.Object();const auto slot=tcard_objects::Slot(descriptor.object);if(!sync_hex(slot))throw std::runtime_error("Invalid dependency object slot");
        const auto request=sync_chunk_control(tls,io,!server,server?101:102,JsonObject{},false);sync_dependency_check(request,L"dependency-request",transfer,slot);const auto indices=request.GetNamedArray(L"Indices");
        if(indices.Size()>descriptor.object.hashes.size())throw std::runtime_error("Dependency request budget exceeded");
        scope.Check();const auto bytes=indices.Size()?tcard_dependencies::ReadVerified(reader,descriptor):tcard_crypto::Bytes{};scope.Check();if(io.report)io.report->reusedChunks+=descriptor.object.hashes.size()-indices.Size();size_t previous=0;
        for(unsigned n=0;n<indices.Size();++n){const auto number=indices.GetAt(n).GetNumber();
            if(!std::isfinite(number)||number<0||number>=descriptor.object.hashes.size()||std::floor(number)!=number)throw std::runtime_error("Invalid dependency fragment index");const auto index=static_cast<size_t>(number);
            if(n&&index<=previous)throw std::runtime_error("Repeated dependency fragment request");previous=index;tcard_crypto::Bytes fragment;fragment.reserve(68+tcard_chunks::chunkSize);
            for(const auto c:slot)fragment.push_back(static_cast<BYTE>(c));for(unsigned i=0;i<4;++i)fragment.push_back(static_cast<BYTE>(index>>(24-i*8)));
            const auto begin=index*tcard_chunks::chunkSize,end=(std::min)(bytes.size(),begin+tcard_chunks::chunkSize);fragment.insert(fragment.end(),bytes.begin()+begin,bytes.begin()+end);
            sync_chunk_frame(tls,io,server,server?112:111,fragment,true,tcard_chunks::chunkSize+68);const auto stored=sync_chunk_control(tls,io,!server,server?121:122,JsonObject{},false);
            sync_dependency_check(stored,L"dependency-stored",transfer,slot);if(tcard_chunks::Number(stored,L"Index",descriptor.object.hashes.size()-1)!=index)throw std::runtime_error("Dependency stored fragment differs");
            if(io.report){++io.report->sentChunks;io.report->sentChunkBytes+=end-begin;io.report->sentContentBytes+=end-begin;}
        }
        const auto complete=sync_chunk_control(tls,io,!server,server?131:132,JsonObject{},false);sync_dependency_check(complete,L"dependency-complete",transfer,slot);
    }
    scope.Check();
}
static JsonObject sync_dependency_receive(Tls& tls,Io& io,bool server,const JsonObject& offer)
{
    sync_dependency_scope scope(io);if(offer.GetNamedNumber(L"Protocol")!=2||sync_string(offer,L"Phase")!=L"delta")throw std::runtime_error("Invalid dependency offer phase");const auto packet=std::wstring(offer.GetNamedObject(L"Data").Stringify());
    const auto context=sync_dependency_context(io,packet,true);const auto package=std::wstring(JsonObject::Parse(packet).GetNamedObject(L"Delta").Stringify());const auto manifest=tcard_dependencies::Read(package,context);const auto transfer=tcard_dependencies::Digest(tcard_dependencies::Encode(manifest));
    // Capture authenticated full membership before any received original can enter the replica.
    scope.Check();tcard_remote::CaptureDependency(io.path,io.receiver,packet,context);scope.Check();
    PreparedNote prepared{manifest.note,package};Result proofResult;bool cleanupEntered=false;
    std::unique_ptr<tcard_dependencies::Pool> pool;
    // One full-byte proof and its cleanup share the same locks. Only pre-callback failure may receive anew.
    const bool committed=WithDependencyCommit(io.path,io.path,prepared,context,[&](const DependencyCommitProof& proof){
        cleanupEntered=true;scope.Check();pool=std::make_unique<tcard_dependencies::Pool>(io.path,package,context,false);return pool->Release(io.path,proof);
    },proofResult);scope.Check();
    if(!committed){
        if(cleanupEntered)throw std::runtime_error("Committed dependency cleanup failed");
        pool=std::make_unique<tcard_dependencies::Pool>(io.path,package,context);
    }
    scope.Check();
    if(io.report){io.report->objects=true;io.report->chunked=true;}
    for(const auto& descriptor:manifest.descriptors){
        scope.Object();const auto slot=tcard_objects::Slot(descriptor.object);if(!sync_hex(slot))throw std::runtime_error("Invalid dependency object slot");scope.Check();const auto missing=committed?std::vector<size_t>{}:pool->Missing(descriptor);scope.Check();
        auto request=sync_dependency_notice(L"dependency-request",transfer,slot);JsonArray indices;for(size_t index:missing)indices.Append(JsonValue::CreateNumberValue(static_cast<double>(index)));request.Insert(L"Indices",indices);
        sync_chunk_control(tls,io,!server,server?101:102,request,true);if(io.report)io.report->reusedChunks+=descriptor.object.hashes.size()-missing.size();
        for(size_t index:missing){tcard_crypto::Bytes fragment;sync_chunk_frame(tls,io,server,server?112:111,fragment,false,tcard_chunks::chunkSize+68);
            if(fragment.size()<68)throw std::runtime_error("Truncated dependency fragment");for(unsigned i=0;i<64;++i)if(fragment[i]!=slot[i])throw std::runtime_error("Dependency fragment object differs");size_t received=0;
            for(unsigned i=64;i<68;++i)received=(received<<8)|fragment[i];if(received!=index)throw std::runtime_error("Dependency fragment order differs");scope.Check();pool->Put(descriptor,index,tcard_crypto::Bytes(fragment.begin()+68,fragment.end()));scope.Check();
            if(io.report){++io.report->receivedChunks;io.report->receivedChunkBytes+=fragment.size()-68;io.report->receivedContentBytes+=fragment.size()-68;}
            auto stored=sync_dependency_notice(L"dependency-stored",transfer,slot);stored.Insert(L"Index",JsonValue::CreateNumberValue(static_cast<double>(index)));sync_chunk_control(tls,io,!server,server?121:122,stored,true);
        }
        scope.Check();if(!committed)pool->Seal(descriptor);scope.Check();sync_chunk_control(tls,io,!server,server?131:132,sync_dependency_notice(L"dependency-complete",transfer,slot),true);
    }
    scope.Check();Result result;if(!committed&&!PrepareDependencyNote(io.path,package,context,*pool,prepared,result))throw std::runtime_error("Dependency original preparation failed");scope.Check();
    io.dependencyContext=context;io.dependencyCommitted=committed;io.dependencyNote=std::move(prepared);io.dependencyPool=std::move(pool);auto value=JsonObject::Parse(std::wstring(offer.Stringify()));value.SetNamedValue(L"Role",JsonValue::CreateStringValue(L"replica"));return value;
}
static bool sync_dependency_import(Io& io,Result& result)
{
    if(!io.dependencyPool||io.dependencyNote.package.empty())throw std::runtime_error("Dependency provider receipt absent");
    result={};const bool imported=io.dependencyCommitted||ImportDependencyNote(io.path,io.dependencyNote,io.dependencyContext,*io.dependencyPool,result);
    if(!imported||result.partial||result.pending)return imported;
    // Only an independently verified immutable committed copy may replace duplicate custody.
    // Captured target, producer pin, and native owner delivery intent remain until actual owner ACK.
    Result cleanup;if(!WithDependencyCommit(io.path,io.path,io.dependencyNote,io.dependencyContext,
        [&](const DependencyCommitProof& proof){return io.dependencyPool->Release(io.path,proof);},cleanup)){
        result.error=cleanup.error;result.partial=false;return false;
    }
    return true;
}
