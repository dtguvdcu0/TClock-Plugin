#pragma once
// Included inside tcard_sync::session after the authenticated frame helpers.
static JsonObject sync_object_notice(const wchar_t* role,const std::wstring& transfer,const std::wstring& object)
{
    JsonObject value;value.Insert(L"Protocol",JsonValue::CreateNumberValue(2));value.Insert(L"Role",JsonValue::CreateStringValue(role));
    value.Insert(L"Transfer",JsonValue::CreateStringValue(transfer));value.Insert(L"Object",JsonValue::CreateStringValue(object));return value;
}
static void sync_object_check(const JsonObject& value,const wchar_t* role,const std::wstring& transfer,const std::wstring& object)
{
    if(value.GetNamedNumber(L"Protocol")!=2||sync_string(value,L"Role")!=role||sync_string(value,L"Transfer")!=transfer||sync_string(value,L"Object")!=object)
        throw std::runtime_error("Object control identity differs");
}
static void sync_object_send(Tls& tls,Io& io,bool server,const JsonObject& data,const std::wstring& reference)
{
    if(!io.objects||tcard_chunks::Group(io.path)!=tcard_asset::hash({io.secure->publicKey.begin(),io.secure->publicKey.end()}))throw std::runtime_error("Object source capability or authority differs");
    const auto values=tcard_objects::References(reference);const auto transfer=tcard_blobs::Identity(reference);
    io.chunkDeadline=Clock::now()+std::chrono::seconds(60);auto offer=JsonObject::Parse(std::wstring(data.Stringify()));offer.SetNamedValue(L"Role",JsonValue::CreateStringValue(L"object-offer"));
    auto encoded=tcard_asset::encode(std::wstring(offer.Stringify()));sync_chunk_frame(tls,io,server,server?2:1,encoded,true,tcard_asset::max_file);
    if(io.report){io.report->objects=true;io.report->chunked=true;}
    for(const auto& value:values){
        io.chunkDeadline=Clock::now()+std::chrono::seconds(60);const auto slot=tcard_objects::Slot(value);
        const auto request=sync_chunk_control(tls,io,!server,server?61:62,JsonObject{},false);sync_object_check(request,L"object-request",transfer,slot);
        const auto indices=request.GetNamedArray(L"Indices");if(indices.Size()>value.hashes.size())throw std::runtime_error("Object request budget exceeded");
        const auto bytes=indices.Size()?tcard_blobs::Source(io.path,reference,value):tcard_crypto::Bytes{};
        if(io.report)io.report->reusedChunks+=value.hashes.size()-indices.Size();size_t previous=0;
        for(unsigned n=0;n<indices.Size();++n){const auto number=indices.GetAt(n).GetNumber();
            if(!std::isfinite(number)||number<0||number>=value.hashes.size()||floor(number)!=number)throw std::runtime_error("Invalid requested object fragment");
            const auto index=static_cast<size_t>(number);if(n&&index<=previous)throw std::runtime_error("Repeated object fragment request");previous=index;
            tcard_crypto::Bytes fragment;fragment.reserve(68+tcard_chunks::chunkSize);
            for(const auto c:slot)fragment.push_back(static_cast<BYTE>(c));for(unsigned i=0;i<4;++i)fragment.push_back(static_cast<BYTE>(index>>(24-i*8)));
            const auto begin=index*tcard_chunks::chunkSize,end=(std::min)(bytes.size(),begin+tcard_chunks::chunkSize);fragment.insert(fragment.end(),bytes.begin()+begin,bytes.begin()+end);
            sync_chunk_frame(tls,io,server,server?72:71,fragment,true,tcard_chunks::chunkSize+68);
            const auto ack=sync_chunk_control(tls,io,!server,server?81:82,JsonObject{},false);sync_object_check(ack,L"object-stored",transfer,slot);
            if(tcard_chunks::Number(ack,L"Index",value.hashes.size()-1)!=index)throw std::runtime_error("Object stored index differs");
            if(io.report){++io.report->sentChunks;io.report->sentChunkBytes+=end-begin;io.report->sentContentBytes+=end-begin;}
        }
        const auto complete=sync_chunk_control(tls,io,!server,server?91:92,JsonObject{},false);sync_object_check(complete,L"object-complete",transfer,slot);
    }
}
static JsonObject sync_object_receive(Tls& tls,Io& io,bool server,const JsonObject& offer)
{
    if(!io.objects||offer.GetNamedNumber(L"Protocol")!=2||sync_string(offer,L"Phase")!=L"delta")throw std::runtime_error("Unnegotiated object offer");
    std::wstring reference;selection::Unwrap(std::wstring(offer.GetNamedObject(L"Data").Stringify()),reference,true);
    const auto values=tcard_objects::References(reference);const auto group=tcard_asset::hash({io.secure->publicKey.begin(),io.secure->publicKey.end()});
    if(tcard_chunks::Group(io.path)!=group)throw std::runtime_error("Object receiving authority differs");
    tcard_blobs::ReapLocal(io.path);const auto root=tcard_blobs::Open(io.path,reference,true),transfer=tcard_blobs::Identity(reference);
    if(io.report){io.report->objects=true;io.report->chunked=true;}
    for(const auto& value:values){
        io.chunkDeadline=Clock::now()+std::chrono::seconds(60);const auto slot=tcard_objects::Slot(value);tcard_objects::Cache cache(root,value,group);
        const auto missing=cache.Missing();auto request=sync_object_notice(L"object-request",transfer,slot);JsonArray indices;
        for(size_t index:missing)indices.Append(JsonValue::CreateNumberValue(static_cast<double>(index)));request.Insert(L"Indices",indices);
        sync_chunk_control(tls,io,!server,server?61:62,request,true);if(io.report)io.report->reusedChunks+=value.hashes.size()-missing.size();
        for(size_t index:missing){tcard_crypto::Bytes fragment;sync_chunk_frame(tls,io,server,server?72:71,fragment,false,tcard_chunks::chunkSize+68);
            if(fragment.size()<68)throw std::runtime_error("Truncated object fragment");for(unsigned i=0;i<64;++i)if(fragment[i]!=slot[i])throw std::runtime_error("Object fragment identity differs");
            size_t received=0;for(unsigned i=64;i<68;++i)received=(received<<8)|fragment[i];if(received!=index)throw std::runtime_error("Object fragment index differs");
            const tcard_crypto::Bytes body(fragment.begin()+68,fragment.end());cache.Put(index,body);
            if(io.report){++io.report->receivedChunks;io.report->receivedChunkBytes+=body.size();io.report->receivedContentBytes+=body.size();}
            auto ack=sync_object_notice(L"object-stored",transfer,slot);ack.Insert(L"Index",JsonValue::CreateNumberValue(static_cast<double>(index)));
            sync_chunk_control(tls,io,!server,server?81:82,ack,true);
        }
        cache.Assemble();sync_chunk_control(tls,io,!server,server?91:92,sync_object_notice(L"object-complete",transfer,slot),true);
    }
    auto result=JsonObject::Parse(std::wstring(offer.Stringify()));result.SetNamedValue(L"Role",JsonValue::CreateStringValue(L"replica"));return result;
}
