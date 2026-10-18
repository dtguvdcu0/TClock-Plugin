#include <winsock2.h>
#include <ws2tcpip.h>
#include "card_sync_transport.h"
#include "card_sync_source.h"
#include "card_native_root.h"
#include "card_sync_store.h"
#include "card_sync_protocol.h"
#include "card_sync_selection.h"
#include "card_sync_chunks.h"
#include "card_sync_blobs.h"
#include "card_sync_bootstrap.h"
#include "card_sync_remote.h"
#include "card_sync_prepare.h"
#include "card_sync_dependency_pool.h"
#include "../sync/compat/card_sync_session.h"
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfio.h>
#include <dpapi.h>
#include <iphlpapi.h>
#pragma comment(lib,"iphlpapi.lib")
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"crypt32.lib")

namespace tcard_sync {
namespace session {
using Clock=std::chrono::steady_clock;
constexpr size_t maxWire=tcard_asset::max_file+65536;
constexpr char cipher[]="TLS13-AES128-GCM-SHA256";
struct Socket {
    SOCKET value=INVALID_SOCKET;
    Socket()=default;
    explicit Socket(SOCKET handle):value(handle){}
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
};
struct Context {
    WOLFSSL_CTX* value=nullptr;
    explicit Context(bool server):value(wolfSSL_CTX_new(server?wolfTLSv1_3_server_method():wolfTLSv1_3_client_method())){}
    ~Context(){if(value)wolfSSL_CTX_free(value);}
};
struct Tls {
    WOLFSSL* value=nullptr;
    explicit Tls(WOLFSSL_CTX* context):value(wolfSSL_new(context)){}
    ~Tls(){if(value)wolfSSL_free(value);}
};
struct Io {
    SOCKET socket;
    const Credentials& credentials;
    std::string identity;
    std::stop_token stop;
    Clock::time_point deadline=Clock::now()+std::chrono::seconds(10);
    std::unique_ptr<tcard_crypto::Keys> secure;
    tcard_protocol::Session proof;
    bool sourceOnly=false;bool streamWanted=false,stream=false;unsigned short localPort=0,remotePort=0;
    PeerSource* sourceProvider=nullptr;bool ownerWanted=false,ownerPeer=false;
    std::shared_ptr<std::atomic_bool> cancelled;
    bool selection=false;bool chunks=false;bool objects=false;bool bootstrap=false;bool targets=false;bool custody=false;bool dependencies=false;std::wstring receiver;std::wstring path;SessionReport* report=nullptr;
    tcard_prepare::Budget preparation;
    tcard_dependencies::Context dependencyContext;PreparedNote dependencyNote;std::unique_ptr<tcard_dependencies::Pool> dependencyPool;
    bool dependencyTransfer=false;bool dependencyCommitted=false;unsigned dependencyOffers=0;Clock::time_point dependencyDeadline;
    Clock::time_point chunkDeadline;std::vector<std::wstring> completedChunks;
    unsigned operations=2;
    ~Io(){SecureZeroMemory(proof.binding.data(),proof.binding.size());}
};
static void sync_initialize()
{
    static std::once_flag once;
    std::call_once(once,[]{WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)||wolfSSL_Init()!=WOLFSSL_SUCCESS)
        throw std::runtime_error("TLS runtime unavailable");});
}
static void sync_validate(const Credentials& credentials)
{
    if(!sync_hex(credentials.space)||std::all_of(credentials.transportKey.begin(),credentials.transportKey.end(),[](BYTE b){return b==0;}))
        throw std::runtime_error("Invalid space credentials");
    auto suffix=tcard_words::Suffix(credentials.transportKey.data());
    const bool current=std::equal(suffix.begin(),suffix.begin()+4,credentials.transportKey.begin()+28);
    SecureZeroMemory(suffix.data(),suffix.size());
    if(!current)throw std::runtime_error("Unsupported recovery key format");
    Credentials expected=credentials;tcard_words::Finish(expected);
    SecureZeroMemory(expected.transportKey.data(),expected.transportKey.size());
    if(expected.space!=credentials.space)throw std::runtime_error("Recovery key binding differs");
}
static int sync_receive(WOLFSSL*,char* bytes,int count,void* context)
{
    auto& io=*static_cast<Io*>(context);
    if(io.stop.stop_requested()||(io.cancelled&&io.cancelled->load())||Clock::now()>=io.deadline)return WOLFSSL_CBIO_ERR_GENERAL;
    const auto n=recv(io.socket,bytes,count,0);
    if(n>0)return n;if(n==0)return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WSAGetLastError()==WSAEWOULDBLOCK?WOLFSSL_CBIO_ERR_WANT_READ:WOLFSSL_CBIO_ERR_GENERAL;
}
static int sync_send(WOLFSSL*,char* bytes,int count,void* context)
{
    auto& io=*static_cast<Io*>(context);
    if(io.stop.stop_requested()||(io.cancelled&&io.cancelled->load())||Clock::now()>=io.deadline)return WOLFSSL_CBIO_ERR_GENERAL;
    const auto n=send(io.socket,bytes,count,0);
    if(n>=0)return n;
    return WSAGetLastError()==WSAEWOULDBLOCK?WOLFSSL_CBIO_ERR_WANT_WRITE:WOLFSSL_CBIO_ERR_GENERAL;
}
static unsigned int sync_client(WOLFSSL* ssl,const char*,char* identity,unsigned int capacity,
    unsigned char* key,unsigned int keyCapacity,const char** suite)
{
    const auto& io=*static_cast<Io*>(wolfSSL_GetIOReadCtx(ssl));
    if(capacity<=io.identity.size()||keyCapacity<32)return 0;
    memcpy(identity,io.identity.c_str(),io.identity.size()+1);memcpy(key,io.secure->channelKey.data(),32);*suite=cipher;return 32;
}
static unsigned int sync_server(WOLFSSL* ssl,const char* identity,unsigned char* key,unsigned int capacity,const char** suite)
{
    const auto& io=*static_cast<Io*>(wolfSSL_GetIOReadCtx(ssl));
    if(!identity||io.identity!=identity||capacity<32)return 0;
    memcpy(key,io.secure->channelKey.data(),32);*suite=cipher;return 32;
}
static void sync_configure(Context& context,bool server)
{
    if(!context.value||wolfSSL_CTX_set_cipher_list(context.value,cipher)!=WOLFSSL_SUCCESS)throw std::runtime_error("TLS configuration failed");
    // V2 uses a purpose-derived channel PSK plus fresh private-possession proof.
    // Every session requires private possession and independently encrypted payloads.
    wolfSSL_CTX_set_verify(context.value,WOLFSSL_VERIFY_PEER,nullptr);
    wolfSSL_CTX_SetIORecv(context.value,sync_receive);wolfSSL_CTX_SetIOSend(context.value,sync_send);
    if(server)wolfSSL_CTX_set_psk_server_tls13_callback(context.value,sync_server);
    else wolfSSL_CTX_set_psk_client_tls13_callback(context.value,sync_client);
}
static void sync_nonblocking(SOCKET socket)
{
    u_long on=1;if(ioctlsocket(socket,FIONBIO,&on))throw std::runtime_error("Socket configuration failed");
}
static void sync_wait(Io& io,WOLFSSL* tls,int result)
{
    const auto error=wolfSSL_get_error(tls,result);
    if(error!=WOLFSSL_ERROR_WANT_READ&&error!=WOLFSSL_ERROR_WANT_WRITE)throw std::runtime_error("Authenticated TLS session failed: "+std::to_string(error));
    if(io.stop.stop_requested()||(io.cancelled&&io.cancelled->load())||Clock::now()>=io.deadline)throw std::runtime_error("Session cancelled or timed out");
    fd_set ready;FD_ZERO(&ready);FD_SET(io.socket,&ready);timeval timeout{0,20000};
    if(select(0,error==WOLFSSL_ERROR_WANT_READ?&ready:nullptr,error==WOLFSSL_ERROR_WANT_WRITE?&ready:nullptr,nullptr,&timeout)==SOCKET_ERROR)
        throw std::runtime_error("Socket wait failed");
}
static void sync_handshake(Tls& tls,Io& io,bool server)
{
    if(!tls.value)throw std::runtime_error("TLS allocation failed");
    io.secure=tcard_compat::SessionKeys(io.credentials);
    {const auto address=tcard_protocol::Address(io.secure->publicKey);
        // Fixed ASCII public address on the wire, never a path or executable command.
        io.identity="TCard.Sync.v2.";for(wchar_t c:address)io.identity+=static_cast<char>(c);}

    // The identity accessor needs handshake arrays; release them immediately after checking.
    wolfSSL_KeepArrays(tls.value);
    wolfSSL_SetIOReadCtx(tls.value,&io);wolfSSL_SetIOWriteCtx(tls.value,&io);
    for(;;){const auto n=server?wolfSSL_accept(tls.value):wolfSSL_connect(tls.value);if(n==WOLFSSL_SUCCESS)break;sync_wait(io,tls.value,n);}
    const auto identity=wolfSSL_get_psk_identity(tls.value);
    const bool valid=identity&&io.identity==identity&&!strcmp(wolfSSL_get_version(tls.value),"TLSv1.3");
    if(valid){
        constexpr char label[]="EXPORTER-TCard-Sync-v2";
        if(wolfSSL_export_keying_material(tls.value,io.proof.binding.data(),32,label,sizeof(label)-1,io.secure->publicKey.data(),65,1)!=WOLFSSL_SUCCESS)
            throw std::runtime_error("TLS session binding unavailable");
    }
    wolfSSL_FreeArrays(tls.value);
    if(!valid)throw std::runtime_error("Unexpected TLS identity or version");
}
static void sync_transfer(Tls& tls,Io& io,BYTE* bytes,size_t count,bool writing)
{
    for(size_t at=0;at<count;){
        const auto size=static_cast<int>((std::min)(count-at,size_t(16384)));
        const auto n=writing?wolfSSL_write(tls.value,bytes+at,size):wolfSSL_read(tls.value,bytes+at,size);
        if(n>0){
            // Only positive application data from wolfSSL_read consumes first-offer waiting.
            if(!writing&&io.preparation.waiting)io.preparation.FirstRead(Clock::now(),io.stop.stop_requested(),io.deadline);
            at+=n;
        }else sync_wait(io,tls.value,n);
    }
}
static void sync_blob(Tls& tls,Io& io,unsigned kind,std::vector<BYTE>& bytes,bool writing,size_t limit=maxWire)
{
    if(writing&&bytes.size()>limit)throw std::runtime_error("Frame too large");
    if(writing&&io.preparation.active)io.preparation.Send(Clock::now(),io.stop.stop_requested(),io.deadline);
    constexpr BYTE version=0x32;std::array<BYTE,9> header{0x54,0x43,0x53,version,static_cast<BYTE>(kind)};
    if(writing)for(unsigned i=0;i<4;++i)header[5+i]=static_cast<BYTE>(bytes.size()>>(24-i*8));
    sync_transfer(tls,io,header.data(),header.size(),writing);
    if(!writing){
        if(header[0]!=0x54||header[1]!=0x43||header[2]!=0x53||header[3]!=version||header[4]!=kind)throw std::runtime_error("Unexpected frame role or version");
        size_t count=0;for(unsigned i=0;i<4;++i)count=(count<<8)|header[5+i];
        if(!count||count>limit)throw std::runtime_error("Invalid frame size");bytes.resize(count);
    }
    sync_transfer(tls,io,bytes.data(),bytes.size(),writing);
}
static void sync_frame(Tls& tls,Io& io,unsigned kind,std::wstring& body,bool writing,size_t limit=maxWire)
{
    auto bytes=writing?tcard_asset::encode(body):std::vector<BYTE>{};sync_blob(tls,io,kind,bytes,writing,limit);
    if(!writing){body=tcard_asset::decode(bytes);if(tcard_asset::encode(body)!=bytes)throw std::runtime_error("Invalid frame encoding");}
}
static std::wstring sync_encode(const tcard_crypto::Bytes& bytes)
{
    std::wstring text;for(auto c:bytes){text+=L"0123456789abcdef"[c>>4];text+=L"0123456789abcdef"[c&15];}return text;
}
static tcard_crypto::Bytes sync_decode(const std::wstring& text,size_t minimum,size_t maximum)
{
    if(text.size()%2||text.size()/2<minimum||text.size()/2>maximum||text.find_first_not_of(L"0123456789abcdef")!=std::wstring::npos)throw std::runtime_error("Invalid proof encoding");
    auto digit=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};tcard_crypto::Bytes bytes;
    for(size_t i=0;i<text.size();i+=2)bytes.push_back(static_cast<BYTE>((digit(text[i])<<4)|digit(text[i+1])));return bytes;
}
static void sync_challenge(Tls& tls,Io& io,bool server,bool writing)
{
    auto& nonce=server?io.proof.server:io.proof.client;std::wstring body;
    if(writing){if(!tcard_protocol::Challenge(nonce))throw std::runtime_error("Fresh challenge unavailable");
        JsonObject data;data.Insert(L"Nonce",JsonValue::CreateStringValue(sync_encode({nonce.begin(),nonce.end()})));
        data.Insert(L"Selection",JsonValue::CreateNumberValue(1));data.Insert(L"Operations",JsonValue::CreateNumberValue(3));
        if(io.sourceProvider)data.Insert(L"OwnerSource",JsonValue::CreateNumberValue(1));
        if(io.ownerWanted)data.Insert(L"OwnerSourceRequest",JsonValue::CreateNumberValue(1));
        if(io.streamWanted&&tcard_storage::Required(io.path)&&tcard_storage::Ready(io.path)){data.Insert(L"Persistent",JsonValue::CreateNumberValue(1));data.Insert(L"Port",JsonValue::CreateNumberValue(io.localPort));}
        if(tcard_storage::Required(io.path)&&tcard_storage::Ready(io.path)){data.Insert(L"Chunks",JsonValue::CreateNumberValue(1));data.Insert(L"Objects",JsonValue::CreateNumberValue(1));data.Insert(L"Bootstrap",JsonValue::CreateNumberValue(1));data.Insert(L"BootstrapTarget",JsonValue::CreateNumberValue(1));data.Insert(L"BootstrapCustody",JsonValue::CreateNumberValue(1));if(!io.sourceOnly)data.Insert(L"Dependencies",JsonValue::CreateNumberValue(1));data.Insert(L"Node",JsonValue::CreateStringValue(tcard_bootstrap::Node(io.path)));}body=data.Stringify().c_str();}
    sync_frame(tls,io,server?11:10,body,writing,4096);
    if(!writing){const auto data=JsonObject::Parse(body);const auto bytes=sync_decode(sync_string(data,L"Nonce"),32,32);std::copy(bytes.begin(),bytes.end(),nonce.begin());
        // The later private proof binds the TLS exporter, authenticating this TLS capability exchange.
        io.ownerPeer=data.GetNamedNumber(L"OwnerSource",0)==1;
        // Here server identifies the challenge author, not the local endpoint role.
        if(!server&&data.GetNamedNumber(L"OwnerSourceRequest",0)==1){io.ownerWanted=true;io.sourceOnly=true;}
        io.chunks=data.GetNamedNumber(L"Chunks",0)==1&&tcard_storage::Required(io.path)&&tcard_storage::Ready(io.path);
        io.objects=data.GetNamedNumber(L"Objects",0)==1&&io.chunks;
        io.bootstrap=data.GetNamedNumber(L"Bootstrap",0)==1&&io.objects;
        io.targets=data.GetNamedNumber(L"BootstrapTarget",0)==1&&io.bootstrap;
        io.custody=data.GetNamedNumber(L"BootstrapCustody",0)==1&&io.targets;
        if(io.bootstrap){io.receiver=sync_string(data,L"Node");if(!sync_hex(io.receiver))throw std::runtime_error("Invalid bootstrap peer identity");}
        io.selection=data.GetNamedNumber(L"Selection",0)==1;io.dependencies=!io.sourceOnly&&data.GetNamedNumber(L"Dependencies",0)==1&&io.selection&&io.custody;io.operations=data.GetNamedNumber(L"Operations",2)>=3?3:2;
        io.stream=io.streamWanted&&io.selection&&io.bootstrap&&data.GetNamedNumber(L"Persistent",0)==1;
        if(io.stream){const double port=data.GetNamedNumber(L"Port",0);if(!std::isfinite(port)||port<0||port>65535||floor(port)!=port)throw std::runtime_error("Invalid persistent peer port");io.remotePort=static_cast<unsigned short>(port);}}
}
static void sync_proof(Tls& tls,Io& io,bool server,bool writing)
{
    std::wstring body;const auto& keys=*io.secure;const auto publicKey=sync_encode({keys.publicKey.begin(),keys.publicKey.end()});
    if(writing){tcard_crypto::Bytes signature;if(!tcard_protocol::Prove(keys,io.proof,server,signature))throw std::runtime_error("Private possession proof failed");
        JsonObject data;data.Insert(L"Public",JsonValue::CreateStringValue(publicKey));data.Insert(L"Signature",JsonValue::CreateStringValue(sync_encode(signature)));body=data.Stringify().c_str();}
    sync_frame(tls,io,server?13:12,body,writing,4096);
    if(!writing){const auto data=JsonObject::Parse(body);
        if(sync_string(data,L"Public")!=publicKey||!tcard_protocol::Accept(keys.publicKey,io.proof,server,sync_decode(sync_string(data,L"Signature"),1,80)))
            throw std::runtime_error("Peer private possession rejected");}
}
static void sync_authenticate(Tls& tls,Io& io,bool server)
{
    sync_challenge(tls,io,false,!server);sync_challenge(tls,io,true,server);
    sync_proof(tls,io,false,!server);sync_proof(tls,io,true,server);
    if(io.selection){
        // Private proof must finish within the unchanged unauthenticated admission deadline.
        if(io.stop.stop_requested()||(io.cancelled&&io.cancelled->load())||Clock::now()>=io.deadline)throw std::runtime_error("Session cancelled or timed out");
        io.preparation.Begin(Clock::now(),false);io.deadline=io.preparation.expires;
    }
}
static void sync_check_versions(const std::wstring& package,const Io& io)
{
    const auto notes=JsonObject::Parse(package).GetNamedArray(L"Notes");
    for(unsigned n=0;n<notes.Size();++n){const auto changes=notes.GetObjectAt(n).GetNamedArray(L"Changes");
        for(unsigned i=0;i<changes.Size();++i)if(changes.GetObjectAt(i).GetNamedNumber(L"Version")>io.operations)
            throw std::runtime_error("Peer must be updated to synchronize authored dates");}
}
static void sync_chunk_frame(Tls& tls,Io& io,bool role,unsigned kind,tcard_crypto::Bytes& plain,bool writing,size_t limit)
{
    if(io.stop.stop_requested()||Clock::now()>=io.chunkDeadline)throw std::runtime_error("Chunk transfer cancelled or timed out");
    // Do not rescue expired preparation by resetting a first-offer frame deadline.
    if(writing&&io.preparation.active)io.preparation.Send(Clock::now(),io.stop.stop_requested(),io.deadline);
    io.deadline=Clock::now()+std::chrono::seconds(10);if(io.dependencyTransfer)io.deadline=(std::min)({io.deadline,io.chunkDeadline,io.dependencyDeadline});tcard_crypto::Bytes encrypted;
    if(writing&&!tcard_protocol::Seal(*io.secure,io.proof,role,plain,encrypted))throw std::runtime_error("Chunk encryption failed");
    sync_blob(tls,io,kind,encrypted,writing,limit+tcard_crypto::envelopeOverhead);
    if(!writing&&(!tcard_protocol::Open(*io.secure,io.proof,role,encrypted,plain)||plain.size()>limit))throw std::runtime_error("Chunk authentication failed");
}
static JsonObject sync_chunk_control(Tls& tls,Io& io,bool role,unsigned kind,const JsonObject& value,bool writing)
{
    auto bytes=writing?tcard_asset::encode(std::wstring(value.Stringify())):tcard_crypto::Bytes{};
    sync_chunk_frame(tls,io,role,kind,bytes,writing,16384);
    const auto text=tcard_asset::decode(bytes);if(tcard_asset::encode(text)!=bytes)throw std::runtime_error("Invalid chunk control encoding");return JsonObject::Parse(text);
}
static JsonObject sync_chunk_notice(const wchar_t* role,const tcard_chunks::Manifest& manifest)
{
    JsonObject value;value.Insert(L"Protocol",JsonValue::CreateNumberValue(2));value.Insert(L"Role",JsonValue::CreateStringValue(role));
    value.Insert(L"Hash",JsonValue::CreateStringValue(manifest.hash));return value;
}
static void sync_chunk_validate(const JsonObject& value,const wchar_t* role,const tcard_chunks::Manifest& manifest)
{
    if(value.GetNamedNumber(L"Protocol")!=2||sync_string(value,L"Role")!=role||sync_string(value,L"Hash")!=manifest.hash)throw std::runtime_error("Chunk control identity differs");
}
static void sync_chunk_send(Tls& tls,Io& io,bool server,const JsonObject& data,const std::wstring& package)
{
    if(tcard_chunks::Group(io.path)!=tcard_asset::hash({io.secure->publicKey.begin(),io.secure->publicKey.end()}))throw std::runtime_error("Chunk source group differs");
    io.chunkDeadline=Clock::now()+std::chrono::seconds(60);const auto bytes=tcard_asset::encode(package);const auto manifest=tcard_chunks::Describe(bytes);
    auto offer=tcard_chunks::Encode(manifest);offer.Insert(L"Protocol",JsonValue::CreateNumberValue(2));offer.Insert(L"Role",JsonValue::CreateStringValue(L"chunk-offer"));
    auto metadata=JsonObject::Parse(std::wstring(data.Stringify()));metadata.Remove(L"Data");offer.Insert(L"Metadata",metadata);
    auto encoded=tcard_asset::encode(std::wstring(offer.Stringify()));sync_chunk_frame(tls,io,server,server?2:1,encoded,true,512*1024);
    const auto request=sync_chunk_control(tls,io,!server,server?21:22,JsonObject{},false);sync_chunk_validate(request,L"chunk-request",manifest);
    const auto indices=request.GetNamedArray(L"Indices");if(indices.Size()>manifest.hashes.size())throw std::runtime_error("Chunk request budget exceeded");
    if(io.report){io.report->chunked=true;io.report->reusedChunks+=manifest.hashes.size()-indices.Size();}
    size_t previous=0;
    for(unsigned n=0;n<indices.Size();++n){const auto number=indices.GetAt(n).GetNumber();
        if(!std::isfinite(number)||number<0||number>=manifest.hashes.size()||floor(number)!=number)throw std::runtime_error("Invalid requested chunk");
        const auto index=static_cast<size_t>(number);if(n&&index<=previous)throw std::runtime_error("Repeated or unordered chunk request");previous=index;
        tcard_crypto::Bytes fragment;fragment.reserve(68+tcard_chunks::chunkSize);
        for(const auto c:manifest.hash)fragment.push_back(static_cast<BYTE>(c));for(unsigned i=0;i<4;++i)fragment.push_back(static_cast<BYTE>(index>>(24-i*8)));
        const auto begin=index*tcard_chunks::chunkSize,end=(std::min)(bytes.size(),begin+tcard_chunks::chunkSize);
        fragment.insert(fragment.end(),bytes.begin()+begin,bytes.begin()+end);sync_chunk_frame(tls,io,server,server?32:31,fragment,true,tcard_chunks::chunkSize+68);
        const auto ack=sync_chunk_control(tls,io,!server,server?41:42,JsonObject{},false);sync_chunk_validate(ack,L"chunk-stored",manifest);
        if(tcard_chunks::Number(ack,L"Index",manifest.hashes.size()-1)!=index)throw std::runtime_error("Chunk stored index differs");
        if(io.report){++io.report->sentChunks;io.report->sentChunkBytes+=end-begin;}
    }
    const auto complete=sync_chunk_control(tls,io,!server,server?51:52,JsonObject{},false);sync_chunk_validate(complete,L"chunk-complete",manifest);
}
static JsonObject sync_chunk_receive(Tls& tls,Io& io,bool server,const JsonObject& offer)
{
    if(!io.chunks||offer.GetNamedNumber(L"Protocol")!=2)throw std::runtime_error("Unnegotiated chunk offer");
    io.chunkDeadline=Clock::now()+std::chrono::seconds(60);const auto manifest=tcard_chunks::Read(offer);
    auto metadata=offer.GetNamedObject(L"Metadata");
    if(metadata.GetNamedNumber(L"Protocol")!=2||sync_string(metadata,L"Role")!=L"replica"||metadata.HasKey(L"Data"))throw std::runtime_error("Invalid chunk metadata");
    tcard_chunks::Cache cache(io.path,manifest,tcard_asset::hash({io.secure->publicKey.begin(),io.secure->publicKey.end()}));
    const auto missing=cache.Missing();auto request=sync_chunk_notice(L"chunk-request",manifest);JsonArray indices;
    for(const auto index:missing)indices.Append(JsonValue::CreateNumberValue(static_cast<double>(index)));request.Insert(L"Indices",indices);
    sync_chunk_control(tls,io,!server,server?21:22,request,true);
    if(io.report){io.report->chunked=true;io.report->reusedChunks+=manifest.hashes.size()-missing.size();}
    for(const auto index:missing){tcard_crypto::Bytes fragment;sync_chunk_frame(tls,io,server,server?32:31,fragment,false,tcard_chunks::chunkSize+68);
        if(fragment.size()<68)throw std::runtime_error("Truncated chunk header");
        for(unsigned i=0;i<64;++i)if(fragment[i]!=manifest.hash[i])throw std::runtime_error("Chunk packet identity differs");
        size_t received=0;for(unsigned i=64;i<68;++i)received=(received<<8)|fragment[i];if(received!=index)throw std::runtime_error("Chunk order differs");
        const tcard_crypto::Bytes body(fragment.begin()+68,fragment.end());cache.Put(index,body);
        if(io.report){++io.report->receivedChunks;io.report->receivedChunkBytes+=body.size();}
        auto ack=sync_chunk_notice(L"chunk-stored",manifest);ack.Insert(L"Index",JsonValue::CreateNumberValue(static_cast<double>(index)));
        sync_chunk_control(tls,io,!server,server?41:42,ack,true);
    }
    const auto assembled=cache.Assemble();const auto text=tcard_asset::decode(assembled);
    if(tcard_asset::encode(text)!=assembled)throw std::runtime_error("Invalid assembled chunk encoding");
    metadata.Insert(L"Data",JsonObject::Parse(text));io.completedChunks.push_back(manifest.hash);
    sync_chunk_control(tls,io,!server,server?51:52,sync_chunk_notice(L"chunk-complete",manifest),true);return metadata;
}
static void sync_chunk_commit(Io& io,const Result& committed)
{
    if(committed.pending||committed.partial)return;
    for(const auto& hash:io.completedChunks)tcard_chunks::Remove(io.path,hash);io.completedChunks.clear();
}

#include "card_sync_object_wire.h"
#include "card_sync_dependency_wire.h"

static void sync_payload(Tls& tls,Io& io,bool server,std::wstring& package,size_t& pending,bool writing,std::wstring* discovery=nullptr,std::wstring* phase=nullptr,std::wstring* application=nullptr,bool control=false)
{
    if(!io.secure)throw std::runtime_error("Encrypted payload requires v2");tcard_crypto::Bytes bytes;
    if(io.preparation.active){
        const auto now=Clock::now();const auto stopped=io.stop.stop_requested();
        if(writing)io.preparation.Check(now,stopped);else io.deadline=io.preparation.Wait(now,stopped);
    }
    if(writing){JsonObject data;data.Insert(L"Protocol",JsonValue::CreateNumberValue(2));data.Insert(L"Role",JsonValue::CreateStringValue(L"replica"));
        data.Insert(L"Pending",JsonValue::CreateNumberValue(static_cast<double>(pending)));data.Insert(L"Data",JsonObject::Parse(package));
        if(phase&&!phase->empty()){
            if(!io.selection||(*phase!=L"inventory"&&*phase!=L"delta"))throw std::runtime_error("Invalid selected transfer phase");
            data.Insert(L"Phase",JsonValue::CreateStringValue(*phase));
        }
        if(discovery&&!discovery->empty()&&discovery->size()<=8192)data.Insert(L"Discovery",JsonObject::Parse(*discovery));
        if(application&&!application->empty()&&tcard_asset::encode(*application).size()<=262144)data.Insert(L"Application",JsonObject::Parse(*application));
        if(control)tcard_source_witness::Control(data);
        auto plain=tcard_asset::encode(std::wstring(data.Stringify()));
        if(plain.size()>tcard_asset::max_file&&data.HasKey(L"Application")){data.Remove(L"Application");plain=tcard_asset::encode(std::wstring(data.Stringify()));}
        if(phase&&*phase==L"delta"&&tcard_remote::DependencyPacket(package)){sync_dependency_send(tls,io,server,data,package);return;}
        if(phase&&*phase==L"delta"&&data.GetNamedObject(L"Data").GetNamedObject(L"Delta").HasKey(L"ObjectVersion")){
            std::wstring reference;selection::Unwrap(package,reference,io.objects);sync_object_send(tls,io,server,data,reference);return;
        }
        if(io.chunks&&phase&&*phase!=L"inventory"&&tcard_asset::encode(package).size()>tcard_chunks::chunkSize){
            if(plain.size()>tcard_asset::max_file)throw std::runtime_error("Replica payload budget exceeded");
            sync_chunk_send(tls,io,server,data,package);return;
        }
        if(!tcard_protocol::Seal(*io.secure,io.proof,server,plain,bytes))throw std::runtime_error("Payload encryption failed");}
    sync_blob(tls,io,server?2:1,bytes,writing,tcard_asset::max_file);
    if(!writing){tcard_crypto::Bytes plain;
        if(!tcard_protocol::Open(*io.secure,io.proof,server,bytes,plain))throw std::runtime_error("Payload authentication failed");
        const auto text=tcard_asset::decode(plain);if(tcard_asset::encode(text)!=plain)throw std::runtime_error("Invalid payload encoding");auto data=JsonObject::Parse(text);
        if(control)tcard_source_witness::Control(data);
        if(sync_string(data,L"Role")==L"chunk-offer")data=sync_chunk_receive(tls,io,server,data);
        else if(sync_string(data,L"Role")==L"object-offer")data=sync_object_receive(tls,io,server,data);
        else if(sync_string(data,L"Role")==L"dependency-offer")data=sync_dependency_receive(tls,io,server,data);
        const double count=data.GetNamedNumber(L"Pending");
        if(data.GetNamedNumber(L"Protocol")!=2||sync_string(data,L"Role")!=L"replica"||count<0||count>10000000||floor(count)!=count)throw std::runtime_error("Invalid encrypted replica payload");
        const auto transfer=std::wstring(data.GetNamedString(L"Phase",L""));
        if(!transfer.empty()&&(!io.selection||(transfer!=L"inventory"&&transfer!=L"delta")||!phase))throw std::runtime_error("Unexpected selected transfer");
        if(phase)*phase=transfer;
        auto verified=std::wstring(data.GetNamedObject(L"Data").Stringify());pending=static_cast<size_t>(count);package=std::move(verified);
        if(application){application->clear();try{if(data.HasKey(L"Application")){const auto value=std::wstring(data.GetNamedObject(L"Application").Stringify());
            if(tcard_asset::encode(value).size()<=262144)*application=value;}}catch(...){}}
        // Invalid optional metadata must not discard successfully authenticated note data.
        if(discovery){discovery->clear();try{if(data.HasKey(L"Discovery")){auto value=std::wstring(data.GetNamedObject(L"Discovery").Stringify());if(value.size()<=8192)*discovery=std::move(value);}}catch(...){}}
    }
}
static std::wstring sync_directory(const std::wstring& path)
{
    auto root=tcard::CardDirectory(path,L"_sync");root.resize(root.size()-6);return root;
}
static void sync_bind(const std::wstring& path,const Credentials& credentials)
{
    const auto directory=sync_directory(path);std::lock_guard guard(sync_mutex);SyncLock lock(directory);
    if(!lock||!sync_prepare(directory,L"session"))throw std::runtime_error("Storage busy or unavailable");
    const auto root=directory+L"\\_sync";JsonObject space;
    if(sync_exists(root,L"space.json")){
        if(!sync_read(root,L"space.json",space)||space.GetNamedNumber(L"Version")!=1||sync_string(space,L"Id")!=credentials.space)
            throw std::runtime_error("Storage belongs to another space");
    }else{
        space.Insert(L"Version",JsonValue::CreateNumberValue(1));space.Insert(L"Id",JsonValue::CreateStringValue(credentials.space));
        if(!sync_write(root,L"space.json",space))throw std::runtime_error("Cannot bind data space");
    }
}

static void sync_error(std::wstring& error,const std::exception& exception)
{
    const char* text=exception.what();error.assign(text,text+strlen(text));
}
}

bool CreateCredentials(Credentials& credentials,std::wstring& error)
{
    credentials={};error.clear();
    try{
        if(!tcard_words::Create(credentials))throw std::runtime_error("Cannot generate recovery key");
        return true;
    }catch(const std::exception& e){session::sync_error(error,e);SecureZeroMemory(credentials.transportKey.data(),32);return false;}
}
bool PersistCredentials(const std::wstring& path,const Credentials& credentials,std::wstring& error)
{
    error.clear();
    try{
        session::sync_validate(credentials);session::sync_bind(path,credentials);
        DATA_BLOB entropy{static_cast<DWORD>(credentials.space.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(credentials.space.data()))};
        DATA_BLOB plain{32,const_cast<BYTE*>(credentials.transportKey.data())},encrypted{};
        if(!CryptProtectData(&plain,L"TCard local synchronization",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted))throw std::runtime_error("Cannot protect transport key");
        struct Scope{BYTE* bytes;DWORD size;~Scope(){SecureZeroMemory(bytes,size);LocalFree(bytes);}} scope{encrypted.pbData,encrypted.cbData};
        std::wstring hex;for(DWORD i=0;i<encrypted.cbData;++i){const auto b=encrypted.pbData[i];hex+=L"0123456789abcdef"[b>>4];hex+=L"0123456789abcdef"[b&15];}
        JsonObject object;object.Insert(L"Version",JsonValue::CreateNumberValue(1));object.Insert(L"Space",JsonValue::CreateStringValue(credentials.space));object.Insert(L"ProtectedKey",JsonValue::CreateStringValue(hex));
        const auto root=session::sync_directory(path);std::lock_guard guard(sync_mutex);SyncLock lock(root);
        if(!lock||!sync_write(root+L"\\_sync",L"credentials.json",object))throw std::runtime_error("Cannot publish protected credentials");return true;
    }catch(const std::exception& e){session::sync_error(error,e);return false;}catch(...){error=L"Credential protection failed";return false;}
}
bool ReadCredentials(const std::wstring& path,Credentials& credentials,std::wstring& error)
{
    credentials={};error.clear();
    try{
        const auto root=session::sync_directory(path);std::lock_guard guard(sync_mutex);SyncLock lock(root);JsonObject object;
        if(!lock||!sync_read(root+L"\\_sync",L"credentials.json",object)||object.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Protected credentials unavailable");
        credentials.space=sync_string(object,L"Space");JsonObject binding;
        if(!sync_read(root+L"\\_sync",L"space.json",binding)||binding.GetNamedNumber(L"Version")!=1||sync_string(binding,L"Id")!=credentials.space)
            throw std::runtime_error("Credential space binding differs");
        const auto hex=sync_string(object,L"ProtectedKey");
        if(hex.empty()||hex.size()%2||hex.size()>65536)throw std::runtime_error("Invalid protected credentials");
        std::vector<BYTE> bytes;const auto digit=[](wchar_t c)->unsigned{if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'a'&&c<=L'f')return c-L'a'+10;throw std::runtime_error("Invalid protected key encoding");};
        for(size_t i=0;i<hex.size();i+=2)bytes.push_back(static_cast<BYTE>((digit(hex[i])<<4)|digit(hex[i+1])));
        DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()),bytes.data()},plain{};
        DATA_BLOB entropy{static_cast<DWORD>(credentials.space.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(credentials.space.data())};
        if(!CryptUnprotectData(&encrypted,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))throw std::runtime_error("Cannot unprotect transport key");
        struct Scope{BYTE* bytes;DWORD size;~Scope(){SecureZeroMemory(bytes,size);LocalFree(bytes);}} scope{plain.pbData,plain.cbData};
        if(plain.cbData!=32)throw std::runtime_error("Invalid transport key size");memcpy(credentials.transportKey.data(),plain.pbData,32);session::sync_validate(credentials);return true;
    }catch(const std::exception& e){session::sync_error(error,e);SecureZeroMemory(credentials.transportKey.data(),32);return false;}catch(...){error=L"Credential loading failed";SecureZeroMemory(credentials.transportKey.data(),32);return false;}
}
#include "card_sync_source.inc"

static void sync_recover(const std::wstring& path,Result& result)
{
    Result recovered;const bool okay=RetryChanges(path,recovered);
    result.pending=(std::max)(result.pending,recovered.pending);result.applied+=recovered.applied;
    if(!okay){result.partial=true;result.pending=(std::max)(result.pending,size_t{1});if(result.error.empty())result.error=recovered.error;}
}
static void sync_finish(const std::wstring& path,const selection::Inventory& sent,
    const selection::Inventory& peer,SessionReport& report,PeerApplication* application=nullptr,const std::wstring& notice=L"")
{
    std::wstring committed;Result checked;unsigned maximumVersion=0;
    if(!tcard_remote::Inventory(path,committed,checked,maximumVersion))throw std::runtime_error("Cannot verify committed inventory");
    const auto local=selection::Read(committed);auto expected=local;selection::Include(expected,sent);
    report.missingPeer=selection::Missing(expected,peer);report.missingLocal=selection::Missing(peer,local);
    report.peerPending=(std::max)(report.peerPending,report.missingPeer);report.received.pending=(std::max)(report.received.pending,report.missingLocal);
    report.converged=!report.peerPending&&!report.received.pending&&!report.received.partial;report.exchanged=true;
    report.peerApplicationKnown=false;report.peerApplicationPending=0;report.peerNode.clear();
    if(application)try{const auto captured=application->Observe(notice,peer,expected);
        report.peerApplicationKnown=captured.known;report.peerApplicationPending=captured.pending;report.peerNode=captured.node;
    }catch(...){/* Optional application evidence cannot discard authenticated note commits. */}
}
static void sync_measure(const std::wstring& package,SessionReport& report,bool writing,const tcard_dependencies::Context* context=nullptr)
{
    const auto inventory=context?tcard_dependencies::Required(tcard_dependencies::Read(package,*context)):selection::Read(selection::Header(package));const auto bytes=tcard_asset::encode(package).size();
    if(writing){report.sentOperations=inventory.operations.size();report.sentAssets=inventory.assets.size();report.sentContentBytes+=bytes;}
    else{report.receivedOperations=inventory.operations.size();report.receivedAssets=inventory.assets.size();report.receivedContentBytes+=bytes;}
}

static void sync_reply(session::Tls& tls,session::Io& io,const std::wstring& path,const Credentials& credentials,
    uint32_t remote,PeerDiscovery* discovery,PeerApplication* application,SessionReport& report)
{
    std::wstring package,phase;Result received;size_t peerPending=0;
    std::wstring introduction,peerApplication;session::sync_payload(tls,io,false,package,peerPending,false,discovery?&introduction:nullptr,&phase,application?&peerApplication:nullptr);
    // Reply preparation begins once, before capture/import/finish/export of the received request.
    if(io.selection){io.preparation.Begin(session::Clock::now(),io.stop.stop_requested());io.deadline=io.preparation.expires;}
    const auto peerCustody=io.custody?tcard_remote::PeerCustody(package):L"";selection::Inventory peerInventory;const bool atomicSource=phase==L"delta"&&io.targets&&JsonObject::Parse(package).GetNamedObject(L"RemoteBootstrap").GetNamedBoolean(L"Atomic");
    if(phase==L"inventory")peerInventory=selection::Read(package);
    else{
        if(phase==L"delta"){std::wstring delta;if(io.dependencyPool)peerInventory=tcard_remote::DependencyUnwrap(package,io.dependencyContext,delta);
            else{if(io.targets)tcard_remote::Capture(path,io.receiver,package);peerInventory=selection::Unwrap(package,delta,io.objects);}package=std::move(delta);}
        if(phase!=L"delta")peerInventory=selection::Read(selection::Header(package));
        sync_measure(package,report,false,io.dependencyPool?&io.dependencyContext:nullptr);
        const bool imported=io.dependencyPool?session::sync_dependency_import(io,received):io.objects&&tcard_objects::Referenced(package)?tcard_blobs::Import(path,path,package,received):ImportChanges(path,package,received);
        if(!imported&&!received.partial)throw std::runtime_error("Cannot commit received exchange");
        if(imported&&!received.partial&&!received.pending&&!atomicSource){if(io.dependencyPool)tcard_remote::AdmitDependency(path,package,io.dependencyContext);else tcard_remote::Admit(path,package);}
        if(!io.dependencyPool&&io.objects&&tcard_objects::Referenced(package))tcard_blobs::ReapLocal(path);
    }
    if(io.bootstrap)tcard_bootstrap::Finish(path,path,io.receiver,peerInventory);
    sync_recover(path,received);if(phase!=L"inventory")session::sync_chunk_commit(io,received);
    std::wstring reply;Result exported;unsigned maximumVersion=0;selection::Inventory advertised;
    if(phase==L"inventory"){
        if(!tcard_remote::WireInventory(path,io.receiver,reply,exported,maximumVersion,io.custody))throw std::runtime_error("Cannot export replica inventory");
        if(maximumVersion>io.operations)throw std::runtime_error("Peer must be updated to synchronize authored dates");
        advertised=selection::Read(reply);
    }else if(phase==L"delta"){
        if(!(io.dependencies?tcard_remote::ExportDependency(path,io.receiver,peerInventory,credentials.space,reply,exported,maximumVersion,peerCustody):io.bootstrap?tcard_remote::Export(path,io.receiver,peerInventory,reply,exported,maximumVersion,io.targets,io.custody,peerCustody):tcard_remote::Selection(path,peerInventory,reply,exported,maximumVersion,io.objects)))throw std::runtime_error("Cannot export selected replica state");
        if(maximumVersion>io.operations)throw std::runtime_error("Peer must be updated to synchronize authored dates");
        std::wstring delta;advertised=tcard_remote::DependencyPacket(reply)?tcard_remote::DependencyUnwrap(reply,session::sync_dependency_context(io,reply,false),delta):selection::Unwrap(reply,delta,io.objects);
    }else{
        if(!tcard_remote::Whole(path,reply,exported))throw std::runtime_error("Cannot export replica state");
        session::sync_check_versions(reply,io);advertised=selection::Read(selection::Header(reply));
    }
    if(phase==L"delta"){
        std::wstring delta;if(tcard_remote::DependencyPacket(reply)){const auto context=session::sync_dependency_context(io,reply,false);tcard_remote::DependencyUnwrap(reply,context,delta);sync_measure(delta,report,true,&context);}
        else{selection::Unwrap(reply,delta,io.objects);sync_measure(delta,report,true);}
    }else if(phase!=L"inventory")sync_measure(reply,report,true);
    std::wstring offer;
    if(discovery&&!introduction.empty())try{offer=discovery->Offer(remote);}catch(...){}
    std::wstring localApplication;if(application)try{application->Observe(peerApplication,peerInventory,advertised);localApplication=application->Offer(advertised);
    }catch(...){/* Preserve note transfer if optional delivery persistence is unavailable. */}
    session::sync_payload(tls,io,true,reply,received.pending,true,&offer,&phase,&localApplication);
    if(discovery&&!introduction.empty())try{discovery->Accept(introduction,remote);}catch(...){}
    report.received=received;report.peerPending=peerPending;report.payloadEncrypted=true;report.selected=!phase.empty();
    sync_finish(path,advertised,peerInventory,report,application,peerApplication);
}

static bool sync_round(session::Tls& tls,session::Io& io,const std::wstring& path,const Credentials& credentials,
    uint32_t addressValue,unsigned round,selection::Inventory& peerHeader,std::wstring& peerCustody,
    SessionReport& report,PeerDiscovery* discovery,PeerApplication* application,sync_source_state* source=nullptr)
{
    if(source)sync_source_bind(*source,io);
    session::sync_bind(path,credentials);std::wstring package;Result exported;unsigned maximumVersion=0;
    std::wstring phase=io.selection?(round==0?L"inventory":L"delta"):L"";const auto requested=phase;
    if(phase==L"inventory"){
        if(!tcard_remote::WireInventory(path,io.receiver,package,exported,maximumVersion,io.custody))throw std::runtime_error("Cannot export local inventory");
        if(maximumVersion>io.operations)throw std::runtime_error("Peer must be updated to synchronize authored dates");
    }else if(phase==L"delta"){
        if(!(io.dependencies?tcard_remote::ExportDependency(path,io.receiver,peerHeader,credentials.space,package,exported,maximumVersion,peerCustody):io.bootstrap?tcard_remote::Export(path,io.receiver,peerHeader,package,exported,maximumVersion,io.targets,io.custody,peerCustody):tcard_remote::Selection(path,peerHeader,package,exported,maximumVersion,io.objects)))throw std::runtime_error("Cannot export selected local state");
        if(maximumVersion>io.operations)throw std::runtime_error("Peer must be updated to synchronize authored dates");
    }else{
        if(!tcard_remote::Whole(path,package,exported))throw std::runtime_error("Cannot export local state");
        session::sync_check_versions(package,io);
    }
    std::wstring selectedDelta;const auto sent=phase==L"delta"?(tcard_remote::DependencyPacket(package)?tcard_remote::DependencyUnwrap(package,session::sync_dependency_context(io,package,false),selectedDelta):selection::Unwrap(package,selectedDelta,io.objects)):
        selection::Read(phase==L"inventory"?package:selection::Header(package));
    std::wstring localApplication,peerApplication;if(application)try{localApplication=application->Offer(sent);}catch(...){/* Optional ACK unavailable. */}
    std::wstring introduction;if(discovery)try{introduction=discovery->Offer(addressValue);}catch(...){}
    if(phase==L"delta"){if(tcard_remote::DependencyPacket(package)){const auto context=session::sync_dependency_context(io,package,false);sync_measure(selectedDelta,report,true,&context);}else sync_measure(selectedDelta,report,true);}
    else if(phase!=L"inventory")sync_measure(package,report,true);
    size_t outgoingPending=0;session::sync_payload(tls,io,false,package,outgoingPending,true,&introduction,&phase,&localApplication);
    // Peer reply preparation is awaited from the successful end of our request transfer.
    if(io.selection){io.preparation.Begin(session::Clock::now(),io.stop.stop_requested());io.deadline=io.preparation.expires;}
    session::sync_payload(tls,io,true,package,report.peerPending,false,discovery?&introduction:nullptr,&phase,application?&peerApplication:nullptr);report.payloadEncrypted=true;
    if(phase!=requested)throw std::runtime_error("Selected phase differs from request");
    if(discovery&&!introduction.empty())try{discovery->Accept(introduction,addressValue);}catch(...){}
    if(phase==L"inventory"){
        peerHeader=selection::Read(package);peerCustody=io.custody?tcard_remote::PeerCustody(package):L"";report.selected=true;
        if(source){
            if(source->target.empty()&&peerHeader==selection::Inventory{}){source->target=tcard_remote::Header({});sync_source_write(*source);}
            else if(!source->target.empty())sync_source_require(!selection::Missing(selection::Read(source->target),peerHeader),"Specified source lost pinned originals");
        }
        // Recheck our committed state after the peer header; concurrent local edits must not be skipped.
        if(io.bootstrap)tcard_bootstrap::Finish(path,path,io.receiver,peerHeader);
        sync_recover(path,report.received);sync_finish(path,sent,peerHeader,report,application,peerApplication);
        if(source?sync_source_inspect(*source).ready:report.converged)return true;
        report.exchanged=false;return false;
    }
    if(source)sync_source_capture(*source,package);
    selection::Inventory committedPeer;const bool atomicSource=phase==L"delta"&&io.targets&&JsonObject::Parse(package).GetNamedObject(L"RemoteBootstrap").GetNamedBoolean(L"Atomic");
    if(phase==L"delta"){std::wstring delta;if(io.dependencyPool)committedPeer=tcard_remote::DependencyUnwrap(package,io.dependencyContext,delta);
        else{if(io.targets)tcard_remote::Capture(path,io.receiver,package);committedPeer=selection::Unwrap(package,delta,io.objects);}package=std::move(delta);report.selected=true;}
    else{committedPeer=selection::Read(selection::Header(package));report.selected=false;}
    if(io.bootstrap)tcard_bootstrap::Finish(path,path,io.receiver,committedPeer);
    sync_measure(package,report,false,io.dependencyPool?&io.dependencyContext:nullptr);
    const bool imported=io.dependencyPool?session::sync_dependency_import(io,report.received):io.objects&&tcard_objects::Referenced(package)?tcard_blobs::Import(path,path,package,report.received):ImportChanges(path,package,report.received);
    if(!imported&&!report.received.partial)throw std::runtime_error("Cannot commit peer state");
    if(imported&&!report.received.partial&&!report.received.pending&&!atomicSource){if(io.dependencyPool)tcard_remote::AdmitDependency(path,package,io.dependencyContext);else tcard_remote::Admit(path,package);}
    if(!io.dependencyPool&&io.objects&&tcard_objects::Referenced(package))tcard_blobs::ReapLocal(path);
    sync_recover(path,report.received);session::sync_chunk_commit(io,report.received);sync_finish(path,sent,committedPeer,report,application,peerApplication);return true;
}

static void sync_notify(PeerEvents* events,const tcard_net::Peer& peer,const wchar_t* outcome,bool inbound,const wchar_t* stage)noexcept
{
    if(events)try{events->Record(peer,outcome,inbound,stage);}catch(...){/* Diagnostic persistence cannot change authentication or note delivery. */}
}
struct sync_event_scope {
    PeerEvents* events=nullptr;
    tcard_net::Peer peer;
    bool inbound=false,connected=false,successful=false,stream=false;
    const wchar_t* stage=L"connect";
    ~sync_event_scope(){
        if(!successful&&!(stream&&connected))sync_notify(events,peer,L"failure",inbound,stage);
        if(connected)sync_notify(events,peer,L"disconnect",inbound,stream&&!successful?stage:L"closed");
    }
    void Complete(const wchar_t* completed=L"exchange"){successful=true;sync_notify(events,peer,L"success",inbound,completed);}
};
#include "card_sync_connections.inc"

#include "card_sync_owner.inc"

struct LoopbackListener::Impl {
    std::wstring path;
    Credentials credentials;
    session::Socket socket;
    unsigned short port=0;
    tcard_net::Policy policy;
    std::shared_ptr<PeerDiscovery> discovery;
    std::shared_ptr<PeerApplication> application;
    std::shared_ptr<PeerEvents> events;std::shared_ptr<PeerSource> source;
    std::atomic<size_t> authenticated=0;
    std::shared_ptr<PeerConnections> connections;
    struct Job {std::atomic_bool done=false;std::jthread thread;};
    std::vector<std::unique_ptr<Job>> jobs;std::jthread worker;
    Impl(const std::wstring& storage,const Credentials& keys,unsigned short requested,const tcard_net::Policy& access,bool lan,std::shared_ptr<PeerDiscovery> hints={},std::shared_ptr<PeerApplication> applied={},std::shared_ptr<PeerEvents> logged={},std::shared_ptr<PeerConnections> retained={},std::shared_ptr<PeerSource> provider={}):path(storage),credentials(keys),socket(WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,0)),policy(access),discovery(std::move(hints)),application(std::move(applied)),events(std::move(logged)),source(std::move(provider)),connections(std::move(retained)){
        if(socket.value==INVALID_SOCKET)throw std::runtime_error("Cannot create listener");
        const BOOL exclusive=TRUE;if(setsockopt(socket.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive)))
            throw std::runtime_error("Cannot reserve listener endpoint");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(requested);address.sin_addr.s_addr=htonl(lan?INADDR_ANY:INADDR_LOOPBACK);
        if(bind(socket.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address))||listen(socket.value,4))throw std::runtime_error("Cannot bind configured listener port");
        int size=sizeof(address);if(getsockname(socket.value,reinterpret_cast<sockaddr*>(&address),&size))throw std::runtime_error("Cannot read listener port");
        port=ntohs(address.sin_port);if(connections)connections->SetPort(port);session::sync_nonblocking(socket.value);
        std::promise<void> startup;auto initialized=startup.get_future();
        worker=std::jthread([this,startup=std::move(startup)](std::stop_token stop)mutable{
            bool started=false;try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            startup.set_value();started=true;
            while(!stop.stop_requested()){
                fd_set ready;FD_ZERO(&ready);FD_SET(socket.value,&ready);timeval timeout{0,20000};
                if(select(0,&ready,nullptr,nullptr,&timeout)<=0)continue;
                for(auto i=jobs.begin();i!=jobs.end();)if((*i)->done.load())i=jobs.erase(i);else ++i;
                if(jobs.size()>=64){std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
                sockaddr_in remote{};int remoteSize=sizeof(remote);const auto descriptor=accept(socket.value,reinterpret_cast<sockaddr*>(&remote),&remoteSize);
                if(descriptor==INVALID_SOCKET)continue;
                auto job=std::make_unique<Job>();auto* owned=job.get();
                try{jobs.push_back(std::move(job));owned->thread=std::jthread([this,descriptor,remote,owned](std::stop_token cancel){
                    struct Finish {Job* job;~Finish(){job->done=true;}} finish{owned};
                    Serve(descriptor,remote,cancel);
                });}catch(...){closesocket(descriptor);if(!jobs.empty()&&jobs.back().get()==owned)jobs.pop_back();throw;}

            }
            }catch(...){if(!started)startup.set_exception(std::current_exception());}
        });
        initialized.get();
    }
    void Serve(SOCKET descriptor,const sockaddr_in& remote,std::stop_token stop)
    {
        sync_event_scope logged;session::Socket peer(descriptor);
        logged.events=events.get();logged.peer={tcard_net::FormatAddress(ntohl(remote.sin_addr.s_addr)),ntohs(remote.sin_port),ntohl(remote.sin_addr.s_addr)};
        logged.inbound=true;logged.connected=true;logged.stage=L"policy";
        if(remote.sin_family!=AF_INET||!policy.Allows(ntohl(remote.sin_addr.s_addr)))return;
        try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            logged.stage=L"authentication";session::sync_nonblocking(peer.value);session::Context context(true);session::sync_configure(context,true);
            session::Io io{peer.value,credentials,{},stop};io.path=path;io.streamWanted=connections!=nullptr;io.localPort=port;io.sourceProvider=source.get();
            session::Tls tls(context.value);session::sync_handshake(tls,io,true);session::sync_authenticate(tls,io,true);++authenticated;logged.stage=L"exchange";
            if(io.ownerWanted){sync_owner_reply(tls,io,path);logged.Complete();}
            else if(io.stream){
                const tcard_net::Peer endpoint{logged.peer.ip,io.remotePort,logged.peer.address};
                sync_stream(tls,io,path,credentials,endpoint,true,connections,discovery.get(),application.get(),logged);
            }else{
                SessionReport report;report.connected=report.authenticated=true;report.protocol=2;
                sync_reply(tls,io,path,credentials,logged.peer.address,discovery.get(),application.get(),report);logged.Complete();
            }
        }catch(...){if(logged.stream)logged.successful=stop.stop_requested();/* Private proof and policy still precede every content import. */}
    }
    void Stop(){
        if(worker.joinable()){worker.request_stop();worker.join();}
        for(auto& job:jobs)job->thread.request_stop();jobs.clear();
    }
    ~Impl(){Stop();SecureZeroMemory(credentials.transportKey.data(),32);}
};
LoopbackListener::LoopbackListener(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
LoopbackListener::~LoopbackListener()=default;
unsigned short LoopbackListener::Port()const{return impl_->port;}
size_t LoopbackListener::AuthenticatedCount()const{return impl_->authenticated.load();}
void LoopbackListener::Stop(){impl_->Stop();}
std::unique_ptr<LoopbackListener> ListenLoopback(const std::wstring& path,const Credentials& credentials,unsigned short port,std::wstring& error,std::shared_ptr<PeerApplication> application,std::shared_ptr<PeerEvents> events,std::shared_ptr<PeerConnections> connections,std::shared_ptr<PeerSource> source)
{
    error.clear();try{session::sync_initialize();session::sync_validate(credentials);session::sync_bind(path,credentials);
        return std::unique_ptr<LoopbackListener>(new LoopbackListener(std::make_unique<LoopbackListener::Impl>(path,credentials,port,tcard_net::Policy{{{0x7f000000u,0xff000000u}},{},{{0x7f000000u,0xff000000u}}},false,std::shared_ptr<PeerDiscovery>{},std::move(application),std::move(events),std::move(connections),std::move(source))));
    }catch(const std::exception& e){session::sync_error(error,e);return {};}catch(...){error=L"Listener startup failed";return {};}
}
bool DiscoverLan(tcard_net::Policy& policy,std::wstring& defaultWhitelist,std::wstring& error,std::vector<std::wstring>* addresses)
{
    error.clear();if(addresses)addresses->clear();defaultWhitelist=L"127.0.0.1/32";policy.lan={ {0x7f000000u,0xff000000u} };
    try{
        ULONG bytes=16384;std::vector<BYTE> buffer(bytes);ULONG result=ERROR_BUFFER_OVERFLOW;
        for(unsigned attempt=0;attempt<3&&result==ERROR_BUFFER_OVERFLOW;++attempt){
            buffer.resize(bytes);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER|GAA_FLAG_INCLUDE_GATEWAYS,
                nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&bytes);
        }
        if(result==ERROR_NO_DATA)return true;if(result!=NO_ERROR)throw std::runtime_error("LAN adapter discovery failed");
        unsigned best=MAXDWORD;std::wstring chosen;
        for(auto adapter=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());adapter;adapter=adapter->Next){
            if(adapter->OperStatus!=IfOperStatusUp||(adapter->IfType!=IF_TYPE_ETHERNET_CSMACD&&adapter->IfType!=IF_TYPE_IEEE80211))continue;
            for(auto ip=adapter->FirstUnicastAddress;ip;ip=ip->Next){
                if(!ip->Address.lpSockaddr||ip->Address.lpSockaddr->sa_family!=AF_INET||ip->OnLinkPrefixLength==0||ip->OnLinkPrefixLength>32||ip->DadState!=IpDadStatePreferred)continue;
                const auto address=ntohl(reinterpret_cast<sockaddr_in*>(ip->Address.lpSockaddr)->sin_addr.s_addr);
                if((address>>24)==0||(address>>24)==127||(address>>24)>=224)continue;
                if(addresses)addresses->push_back(tcard_net::FormatAddress(address));
                const auto mask=0xffffffffu<<(32-ip->OnLinkPrefixLength);policy.lan.push_back({address&mask,mask});
                // Seed one actual connected subnet, prefer an adapter with a gateway, then its metric.
                const unsigned rank=(adapter->FirstGatewayAddress?0u:0x80000000u)|(adapter->Ipv4Metric&0x7fffffffu);
                if(chosen.empty()||rank<best){best=rank;chosen=tcard_net::FormatAddress(address&mask)+L"/"+std::to_wstring(ip->OnLinkPrefixLength);}
            }
        }
        if(!chosen.empty())defaultWhitelist+=L";"+chosen;return true;
    }catch(const std::exception& e){session::sync_error(error,e);return false;}catch(...){error=L"LAN discovery failed";return false;}
}
std::unique_ptr<LoopbackListener> ListenLan(const std::wstring& path,const Credentials& credentials,unsigned short port,const tcard_net::Policy& policy,std::wstring& error,std::shared_ptr<PeerDiscovery> discovery,std::shared_ptr<PeerApplication> application,std::shared_ptr<PeerEvents> events,std::shared_ptr<PeerConnections> connections,std::shared_ptr<PeerSource> source)
{
    error.clear();try{
        session::sync_initialize();session::sync_validate(credentials);if(!port)throw std::runtime_error("A fixed listener port is required");session::sync_bind(path,credentials);
        return std::unique_ptr<LoopbackListener>(new LoopbackListener(std::make_unique<LoopbackListener::Impl>(path,credentials,port,policy,true,std::move(discovery),std::move(application),std::move(events),std::move(connections),std::move(source))));
    }catch(const std::exception& e){session::sync_error(error,e);return {};}catch(...){error=L"LAN listener startup failed";return {};}
}
bool ExchangeLoopback(const std::wstring& path,const Credentials& credentials,unsigned short port,SessionReport& report)
{
    const tcard_net::Policy policy{{{0x7f000000u,0xff000000u}},{},{{0x7f000000u,0xff000000u}}};
    return ExchangeLan(path,credentials,{L"127.0.0.1",port,0x7f000001u},policy,report);
}
static bool sync_exchange(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SessionReport& report,std::stop_token stop,PeerDiscovery* discovery,PeerApplication* application,PeerEvents* events,sync_source_state* source)
{
    bool attempted=false;report={};try{
        session::sync_initialize();session::sync_validate(credentials);uint32_t addressValue=0;
        if(!peer.port||!tcard_net::ParseAddress(peer.ip,addressValue)||!policy.Allows(addressValue))throw std::runtime_error("Peer IP is outside the allowed IP policy");
        selection::Inventory peerHeader;std::wstring peerCustody;
        for(unsigned round=0;round<2;++round){
            attempted=true;sync_event_scope logged{events,peer,false};
            session::Socket socket(WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,0));if(socket.value==INVALID_SOCKET)throw std::runtime_error("Cannot create peer socket");session::sync_nonblocking(socket.value);
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(peer.port);address.sin_addr.s_addr=htonl(addressValue);
            const auto connected=connect(socket.value,reinterpret_cast<const sockaddr*>(&address),sizeof(address));
            if(connected&&WSAGetLastError()!=WSAEWOULDBLOCK)throw std::runtime_error("Peer unavailable");
            const auto deadline=session::Clock::now()+std::chrono::seconds(3);
            for(;;){
                if(stop.stop_requested()||session::Clock::now()>=deadline)throw std::runtime_error("Peer connection cancelled or timed out");
                fd_set ready;FD_ZERO(&ready);FD_SET(socket.value,&ready);timeval timeout{0,20000};
                const auto result=select(0,nullptr,&ready,nullptr,&timeout);
                if(result==SOCKET_ERROR)throw std::runtime_error("Peer connection wait failed");if(result>0)break;
            }
            int pending=0,size=sizeof(pending);if(getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&pending),&size)||pending)throw std::runtime_error("Peer connection failed");
            logged.connected=true;logged.stage=L"authentication";report.connected=true;session::Context context(false);session::sync_configure(context,false);session::Io io{socket.value,credentials,{},stop};io.path=path;io.report=&report;io.sourceOnly=source!=nullptr;session::Tls tls(context.value);session::sync_handshake(tls,io,false);session::sync_authenticate(tls,io,false);report.authenticated=true;logged.stage=L"exchange";
            report.protocol=2;report.publicAddress=tcard_protocol::Address(io.secure->publicKey);
            const bool done=sync_round(tls,io,path,credentials,addressValue,round,peerHeader,peerCustody,report,discovery,application,source);
            logged.Complete();if(done)return true;
        }
        throw std::runtime_error("Selected exchange did not complete");
    }catch(const std::exception& e){if(!attempted)sync_notify(events,peer,L"failure",false,L"policy");session::sync_error(report.error,e);return false;}catch(...){if(!attempted)sync_notify(events,peer,L"failure",false,L"policy");report.error=L"Local exchange failed";return false;}
}

bool ExchangeLan(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SessionReport& report,std::stop_token stop,PeerDiscovery* discovery,PeerApplication* application,PeerEvents* events)
{
    return sync_exchange(path,credentials,peer,policy,report,stop,discovery,application,events,nullptr);
}
static bool sync_fetch_source(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop,bool owner)
{
    report={};try{
        session::sync_validate(credentials);uint32_t address=0;
        sync_source_require(peer.port&&tcard_net::ParseAddress(peer.ip,address)&&policy.Allows(address),"Specified source is outside IP policy");
        SyncLock lock(path);sync_source_require(bool(lock),"Source replica busy");
        sync_source_require(tcard_storage::Required(path)&&tcard_storage::Ready(path)&&sync_owner(path)==sync_path_plain(path),"Source requires a separate encrypted root");
        tcard_crypto::Seed seed{};std::copy_n(credentials.transportKey.begin(),seed.size(),seed.begin());tcard_crypto::Keys keys;
        const auto derived=tcard_crypto::Derive(seed,keys);SecureZeroMemory(seed.data(),seed.size());
        sync_source_require(derived&&tcard_storage::Matches(path,keys),"Source credentials differ from encryption identity");
        const auto folder=sync_source_folder(path);sync_source_state source;
        if(!tcard_native::Exists(folder)){
            sync_source_empty(path);std::wstring inventory;Result result;unsigned version=0;
            sync_source_require(ExportCommittedInventory(path,inventory,result,version)&&selection::Read(inventory)==selection::Inventory{},"Source fetch cannot upload existing local state");
            sync_source_require(tcard_bridge::Protect(folder),"Cannot protect source receipt");
            source={sync_path_plain(path),L"",L"",peer.ip,peer.port};if(owner)source.ownerRequest=tcard_bridge::Hash(tcard_bridge::Token());sync_source_write(source);
        }else source=sync_source_read(path);
        sync_source_require(owner==!source.ownerRequest.empty(),"Source acquisition mode differs from its receipt");
        sync_source_require(source.ip==peer.ip&&source.port==peer.port,"Specified source endpoint changed");
        report.snapshot=sync_source_inspect(source);if(report.snapshot.ready)return true;
        if(owner){
            if(!sync_owner_request(source,credentials,peer,policy,report.session,stop)){report.error=report.session.error;return false;}
            report.snapshot=sync_source_inspect(source);if(source.ownerProof.empty()||report.snapshot.ready)return true;
        }
        if(!sync_exchange(path,credentials,peer,policy,report.session,stop,nullptr,nullptr,nullptr,&source)){report.error=report.session.error;report.snapshot=sync_source_inspect(source);return false;}
        report.snapshot=sync_source_inspect(source);return true;
    }catch(const std::exception& e){session::sync_error(report.error,e);return false;}catch(...){report.error=L"Specified source fetch failed";return false;}
}
bool FetchSource(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop)
{return sync_fetch_source(path,credentials,peer,policy,report,stop,false);}
bool FetchOwnerSource(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop)
{return sync_fetch_source(path,credentials,peer,policy,report,stop,true);}
bool AcknowledgeOwnerSource(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop)
{
    report={};try{
        session::sync_validate(credentials);uint32_t address=0;
        sync_source_require(peer.port&&tcard_net::ParseAddress(peer.ip,address)&&policy.Allows(address),"Source completion is outside IP policy");
        SyncLock lock(path);sync_source_require(bool(lock)&&tcard_storage::Required(path)&&tcard_storage::Ready(path)&&sync_owner(path)==sync_path_plain(path),"Source completion replica unavailable");
        tcard_crypto::Seed seed{};std::copy_n(credentials.transportKey.begin(),seed.size(),seed.begin());tcard_crypto::Keys keys;
        const auto derived=tcard_crypto::Derive(seed,keys);SecureZeroMemory(seed.data(),seed.size());
        sync_source_require(derived&&tcard_storage::Matches(path,keys),"Source completion credentials differ");
        auto source=sync_source_read(path);std::wstring error;
        sync_source_require(!source.ownerRequest.empty()&&!source.ownerProof.empty()&&source.ip==peer.ip&&source.port==peer.port&&InspectOwnerSource(path,report.snapshot,error)&&report.snapshot.ready,"Source completion requires verified complete owner originals");
        // Unlike FetchOwnerSource's completed offline fast path, this explicitly requested round
        // sends only the committed inventory. The source's existing Finish retires covered pins.
        if(!sync_exchange(path,credentials,peer,policy,report.session,stop,nullptr,nullptr,nullptr,&source)){report.error=report.session.error;return false;}
        sync_source_require(report.session.authenticated&&report.session.payloadEncrypted&&report.session.sentOperations==0&&InspectOwnerSource(path,report.snapshot,error)&&report.snapshot.ready,"Source completion lacks authenticated custody");return true;
    }catch(const std::exception& e){session::sync_error(report.error,e);return false;}catch(...){report.error=L"Source completion failed";return false;}
}
}

