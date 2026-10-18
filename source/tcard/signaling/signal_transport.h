#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/random.h>
#include "signal_identity.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <cstring>
#ifndef KEEP_PEER_CERT
#error TCard signaling requires KEEP_PEER_CERT in the pinned library and generated options.
#endif

namespace tcard_signal {
using Clock=std::chrono::steady_clock;
inline void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline std::wstring RandomId(size_t size=32){Require(size<=32,"Signal random bound exceeded");WC_RNG rng{};Require(wc_InitRng(&rng)==0,"Signal random source unavailable");
    struct Free{WC_RNG* value;~Free(){wc_FreeRng(value);}} free{&rng};unsigned char bytes[32]{};Require(wc_RNG_GenerateBlock(&rng,bytes,static_cast<word32>(size))==0,"Signal random source failed");return Encode(bytes,size);}
inline std::wstring Error(const std::exception& e){const auto* text=e.what();return std::wstring(text,text+strlen(text));}
inline void Ready(){static const bool ready=wolfSSL_Init()==WOLFSSL_SUCCESS;Require(ready,"Signal TLS initialization failed");}
inline void Network(){static const bool ready=[](){WSADATA data{};return WSAStartup(MAKEWORD(2,2),&data)==0;}();Require(ready,"Signal networking unavailable");}
inline std::atomic<unsigned> connections=0;
struct Slot {
    bool held=false;
    Slot(){auto count=connections.load();while(count<maxConnections){if(connections.compare_exchange_weak(count,count+1)){held=true;return;}}throw std::runtime_error("Signal connection quota exceeded");}
    ~Slot(){if(held)--connections;}
    Slot(const Slot&)=delete;
};
struct Socket {
    SOCKET value=INVALID_SOCKET;
    explicit Socket(SOCKET socket=INVALID_SOCKET):value(socket){}
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
    Socket(const Socket&)=delete;
};
struct Io {SOCKET socket;std::stop_token stop;Clock::time_point total,deadline;};
inline void Check(const Io& io){Require(!io.stop.stop_requested()&&Clock::now()<io.deadline&&Clock::now()<io.total,"Signal connection cancelled or timed out");}
inline int Receive(WOLFSSL*,char* data,int size,void* context){auto& io=*static_cast<Io*>(context);if(io.stop.stop_requested()||Clock::now()>=io.deadline)return WOLFSSL_CBIO_ERR_GENERAL;
    const auto result=recv(io.socket,data,size,0);if(result>0)return result;if(!result)return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WSAGetLastError()==WSAEWOULDBLOCK?WOLFSSL_CBIO_ERR_WANT_READ:WOLFSSL_CBIO_ERR_GENERAL;}
inline int Send(WOLFSSL*,char* data,int size,void* context){auto& io=*static_cast<Io*>(context);if(io.stop.stop_requested()||Clock::now()>=io.deadline)return WOLFSSL_CBIO_ERR_GENERAL;
    const auto result=send(io.socket,data,size,0);if(result>0)return result;if(!result)return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WSAGetLastError()==WSAEWOULDBLOCK?WOLFSSL_CBIO_ERR_WANT_WRITE:WOLFSSL_CBIO_ERR_GENERAL;}
inline bool Wait(Io& io,bool writing){Check(io);fd_set read{},write{};FD_ZERO(&read);FD_ZERO(&write);FD_SET(io.socket,writing?&write:&read);timeval timeout{0,50000};
    const auto result=select(0,&read,&write,nullptr,&timeout);if(result==SOCKET_ERROR)throw std::runtime_error("Signal socket wait failed WSA="+std::to_string(WSAGetLastError()));Check(io);return result>0;}
inline void Retry(WOLFSSL* tls,Io& io,int result){const auto code=wolfSSL_get_error(tls,result);if(code==WOLFSSL_ERROR_WANT_READ)Wait(io,false);else if(code==WOLFSSL_ERROR_WANT_WRITE)Wait(io,true);
    else throw std::runtime_error("Signal TLS failed code="+std::to_string(code)+" WSA="+std::to_string(WSAGetLastError()));}
// Manual exact certificate pins are the trust anchor. This exception is not authorization:
// exact DER pin + certificate time + TLS CertificateVerify are required before any application bytes.
inline int PinTrust(int okay,WOLFSSL_X509_STORE_CTX* store){return okay||(store&&(store->error==ASN_NO_SIGNER_E||store->error==ASN_SELF_SIGNED_E));}
struct Context {
    WOLFSSL_CTX* value=nullptr;
    Context(bool server,const Bytes& cert={},const Bytes& key={}){Ready();value=wolfSSL_CTX_new(server?wolfTLSv1_3_server_method():wolfTLSv1_3_client_method());Require(value,"Signal TLS context unavailable");
        try{Require(wolfSSL_CTX_set_cipher_list(value,"TLS13-AES128-GCM-SHA256")==WOLFSSL_SUCCESS,"Signal TLS cipher unavailable");wolfSSL_CTX_set_verify(value,WOLFSSL_VERIFY_PEER,PinTrust);
            wolfSSL_CTX_set_session_cache_mode(value,WOLFSSL_SESS_CACHE_OFF);
#ifdef HAVE_SESSION_TICKET
            Require(wolfSSL_CTX_no_ticket_TLSv13(value)==WOLFSSL_SUCCESS,"Signal TLS ticket disable failed");
#endif
            wolfSSL_CTX_SetIORecv(value,Receive);wolfSSL_CTX_SetIOSend(value,Send);
            if(!cert.empty()||!key.empty()){Require(!cert.empty()&&cert.size()<=16384&&!key.empty()&&key.size()<=16384,"Signal node identity size invalid");
                Require(wolfSSL_CTX_use_certificate_buffer(value,cert.data(),static_cast<long>(cert.size()),WOLFSSL_FILETYPE_ASN1)==WOLFSSL_SUCCESS&&
                    wolfSSL_CTX_use_PrivateKey_buffer(value,key.data(),static_cast<long>(key.size()),WOLFSSL_FILETYPE_ASN1)==WOLFSSL_SUCCESS,"Signal node certificate or key invalid");
                Require(wolfSSL_CTX_check_private_key(value)==WOLFSSL_SUCCESS,"Signal node certificate/key pair mismatch");}
        }catch(...){wolfSSL_CTX_free(value);value=nullptr;throw;}}
    ~Context(){if(value)wolfSSL_CTX_free(value);}
    Context(const Context&)=delete;
};
inline std::shared_ptr<Context> ClientContext(){static auto context=std::make_shared<Context>(false);return context;}
inline std::wstring CertificateIdentity(const Bytes& bytes){Require(!bytes.empty()&&bytes.size()<=16384,"Signal certificate size invalid");
    const auto certificate=CertCreateCertificateContext(X509_ASN_ENCODING,bytes.data(),static_cast<DWORD>(bytes.size()));Require(certificate,"Signal certificate invalid");
    struct Cert{PCCERT_CONTEXT value;~Cert(){CertFreeCertificateContext(value);}} cert{certificate};Require(CertVerifyTimeValidity(nullptr,certificate->pCertInfo)==0,"Signal certificate expired or not yet valid");return Digest(bytes);}
inline std::wstring Certificate(WOLFSSL* tls,bool required){auto* peer=wolfSSL_get_peer_certificate(tls);if(!peer){Require(!required,"Signal peer certificate missing");return {};}
    struct Free{WOLFSSL_X509* value;~Free(){wolfSSL_X509_free(value);}} free{peer};int size=0;const auto* bytes=wolfSSL_X509_get_der(peer,&size);Require(bytes&&size>0&&size<=16384,"Signal peer certificate size invalid");
    return CertificateIdentity(Bytes(bytes,bytes+size));}
inline void Nonblocking(SOCKET socket){u_long value=1;Require(ioctlsocket(socket,FIONBIO,&value)==0,"Signal socket mode failed");}
inline sockaddr_in Address(const std::wstring& ip,unsigned short port,bool listen=false){sockaddr_in value{};value.sin_family=AF_INET;value.sin_port=htons(port);
    Require((listen&&ip==L"0.0.0.0"||IPv4(ip))&&InetPtonW(AF_INET,ip.c_str(),&value.sin_addr)==1,"Signal address invalid");return value;}
class Channel {
    Socket socket_;Slot slot_;std::shared_ptr<Context> context_;Io io_;WOLFSSL* tls_=nullptr;std::wstring peer_;
public:
    Channel(const Node& node,std::stop_token stop,std::shared_ptr<Context> context=ClientContext(),Clock::time_point limit=Clock::now()+std::chrono::seconds(15)):
        socket_(),context_(std::move(context)),io_{INVALID_SOCKET,stop,limit,(std::min)(limit,Clock::now()+std::chrono::seconds(10))}{
        Network();Require(IPv4(node.ip)&&node.port&&Hex(node.id,64),"Signal configured node invalid");socket_.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Require(socket_.value!=INVALID_SOCKET,"Signal socket unavailable");io_.socket=socket_.value;
        Nonblocking(io_.socket);const auto address=Address(node.ip,node.port);if(connect(io_.socket,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==SOCKET_ERROR){
            const auto code=WSAGetLastError();if(code!=WSAEWOULDBLOCK&&code!=WSAEINPROGRESS)throw std::runtime_error("Signal TCP connect failed WSA="+std::to_string(code));while(!Wait(io_,true)){}int error=0;int size=sizeof(error);
            const auto status=getsockopt(io_.socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size);if(status!=0||error)throw std::runtime_error("Signal TCP connect failed WSA="+std::to_string(status?WSAGetLastError():error));}
        try{Open(false);Require(peer_==node.id,"Signal peer certificate pin mismatch");}catch(...){if(tls_){wolfSSL_free(tls_);tls_=nullptr;}throw;}}
    Channel(SOCKET accepted,std::stop_token stop,const std::shared_ptr<Context>& context):socket_(accepted),context_(context),io_{accepted,stop,Clock::now()+std::chrono::seconds(15),Clock::now()+std::chrono::seconds(10)}{Nonblocking(accepted);Open(true);}
    ~Channel(){if(tls_)wolfSSL_free(tls_);}
    Channel(const Channel&)=delete;
    const std::wstring& Peer()const{return peer_;}
    void Limit(Clock::time_point limit){io_.total=(std::min)(io_.total,limit);io_.deadline=io_.total;Check(io_);}
    std::wstring Binding(const std::wstring& node,const std::wstring& nonce){Require(Hex(node,64)&&Hex(nonce,64),"Signal channel challenge invalid");auto context=Decode(node+nonce);unsigned char binding[32]{};
        constexpr char label[]="EXPORTER-TCard-Signal-v1-register";Require(wolfSSL_export_keying_material(tls_,binding,32,label,sizeof(label)-1,context.data(),static_cast<word32>(context.size()),1)==WOLFSSL_SUCCESS,"Signal TLS exporter unavailable");return Encode(binding,32);}
    void Transfer(unsigned char* bytes,size_t size,bool writing){while(size){Check(io_);const auto count=static_cast<int>((std::min)(size,size_t(16384)));const auto n=writing?wolfSSL_write(tls_,bytes,count):wolfSSL_read(tls_,bytes,count);
            if(n>0){bytes+=n;size-=n;}else Retry(tls_,io_,n);}}
    void Write(const Bytes& body){Require(!body.empty()&&body.size()<=maxMessage,"Signal frame size invalid");unsigned char head[4]{};const auto size=static_cast<uint32_t>(body.size());for(unsigned i=0;i<4;++i)head[i]=static_cast<unsigned char>(size>>(24-i*8));Transfer(head,4,true);auto copy=body;Transfer(copy.data(),copy.size(),true);}
    Bytes Read(){unsigned char head[4]{};Transfer(head,4,false);uint32_t size=0;for(auto byte:head)size=(size<<8)|byte;Require(size&&size<=maxMessage,"Signal frame size invalid");Bytes body(size);Transfer(body.data(),body.size(),false);return body;}
private:
    void Open(bool server){tls_=wolfSSL_new(context_->value);Require(tls_,"Signal TLS connection unavailable");try{wolfSSL_KeepArrays(tls_);wolfSSL_SetIOReadCtx(tls_,&io_);wolfSSL_SetIOWriteCtx(tls_,&io_);
            for(;;){Check(io_);const auto n=server?wolfSSL_accept(tls_):wolfSSL_connect(tls_);if(n==WOLFSSL_SUCCESS)break;Retry(tls_,io_,n);}
            Require(!strcmp(wolfSSL_get_version(tls_),"TLSv1.3")&&!wolfSSL_session_reused(tls_),"Signal TLS version or resumption refused");peer_=Certificate(tls_,!server);io_.deadline=io_.total;
        }catch(...){wolfSSL_free(tls_);tls_=nullptr;throw;}}
};
// Finite binary schema avoids recursive/unbounded JSON parsing of unauthenticated wire input.
enum class Kind:unsigned char{Client=1,Challenge=2,Register=3,Lookup=4,Mesh=5,Reply=6,Ack=7,Query=8};
struct Writer {
    Bytes data;
    explicit Writer(Kind kind){data={static_cast<unsigned char>(protocolVersion),static_cast<unsigned char>(kind)};}
    void Number(uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)data.push_back(static_cast<unsigned char>(value>>(8*(size-i-1))));Require(data.size()<=maxMessage,"Signal encoding size exceeded");}
    void Text(const std::wstring& text,size_t maximum){Require(text.size()<=maximum,"Signal field size exceeded");const auto bytes=Ascii(text);Number(bytes.size(),2);data.insert(data.end(),bytes.begin(),bytes.end());Require(data.size()<=maxMessage,"Signal encoding size exceeded");}
};
struct Reader {
    const Bytes data;size_t at=0;Kind kind;
    explicit Reader(Bytes body):data(std::move(body)){Require(data.size()>=2&&data.size()<=maxMessage&&data[0]==protocolVersion,"Signal wire version refused");kind=static_cast<Kind>(data[1]);at=2;}
    uint64_t Number(unsigned size){Require(size<=8&&size<=data.size()-at,"Signal numeric field truncated");uint64_t value=0;for(unsigned i=0;i<size;++i)value=(value<<8)|data[at++];return value;}
    std::wstring Text(size_t maximum){const auto size=Number(2);Require(size<=maximum&&size<=data.size()-at,"Signal field size invalid");std::wstring value;value.reserve(static_cast<size_t>(size));for(size_t i=0;i<size;++i){const auto c=data[at++];Require(c<=127,"Signal ASCII field invalid");value.push_back(c);}return value;}
    void End(){Require(at==data.size(),"Signal trailing fields refused");}
};
inline void Put(Writer& out,const Capsule& value){out.Text(value.route,64);out.Text(value.endpoint,32);out.Text(value.ip,15);out.Number(value.port,2);out.Text(value.publicKey,130);out.Text(value.signature,160);out.Number(value.sequence,8);out.Number(value.expires,8);}
inline Capsule GetCapsule(Reader& in){Capsule value;value.route=in.Text(64);value.endpoint=in.Text(32);value.ip=in.Text(15);value.port=static_cast<unsigned short>(in.Number(2));value.publicKey=in.Text(130);value.signature=in.Text(160);value.sequence=in.Number(8);value.expires=in.Number(8);return value;}
inline void Put(Writer& out,const Query& query){out.Text(query.id,64);out.Text(query.route,64);out.Text(query.origin,64);out.Number(query.expires,8);Require(query.path.size()<=maxHops,"Signal hop bound exceeded");out.Number(query.path.size(),1);for(const auto& hop:query.path)out.Text(hop,64);}
inline Query GetQuery(Reader& in){Query query;query.id=in.Text(64);query.route=in.Text(64);query.origin=in.Text(64);query.expires=in.Number(8);const auto count=in.Number(1);Require(count<=maxHops,"Signal hop bound exceeded");for(size_t i=0;i<count;++i)query.path.push_back(in.Text(64));return query;}
inline void Put(Writer& out,const Reply& reply){out.Text(reply.id,64);out.Text(reply.route,64);Require(reply.capsules.size()<=maxEndpoints,"Signal reply bound exceeded");out.Number(reply.capsules.size(),1);for(const auto& capsule:reply.capsules)Put(out,capsule);}
inline Reply GetReply(Reader& in){Require(in.kind==Kind::Reply,"Signal reply role invalid");Reply reply;reply.id=in.Text(64);reply.route=in.Text(64);const auto count=in.Number(1);Require(count<=maxEndpoints,"Signal reply bound exceeded");for(size_t i=0;i<count;++i)reply.capsules.push_back(GetCapsule(in));in.End();return reply;}
inline bool NodeClient::Register(const Capsule& capsule,const RegistrationSigner& signer,std::wstring& error){error.clear();try{Require(signer&&ValidCapsule(capsule,Now()),"Signal registration capsule invalid");Channel channel(node_,stop_);channel.Write(Writer(Kind::Client).data);
        Reader input(channel.Read());Require(input.kind==Kind::Challenge,"Signal challenge role invalid");PublicChallenge challenge{input.Text(64),input.Text(64),input.Text(64)};input.End();
        Require(challenge.node==node_.id&&Hex(challenge.nonce,64)&&challenge.tlsBinding==channel.Binding(node_.id,challenge.nonce),"Signal challenge channel binding invalid");
        const auto submission=signer(challenge);Require(CapsuleDigest(submission.capsule)==CapsuleDigest(capsule)&&ValidSubmission(submission,challenge,Now()),"Signal fixed registration proof invalid");
        Writer out(Kind::Register);Put(out,submission.capsule);out.Text(submission.node,64);out.Text(submission.nonce,64);out.Text(submission.tlsBinding,64);out.Text(submission.proof,160);channel.Write(out.data);
        Reader ack(channel.Read());Require(ack.kind==Kind::Ack&&ack.Number(1)==1,"Signal registration refused");ack.End();return true;
    }catch(const std::exception& e){error=Error(e);return false;}catch(...){error=L"Signal registration failed";return false;}}
inline bool NodeClient::Lookup(const std::wstring& route,std::vector<Capsule>& results,std::wstring& error){results.clear();error.clear();try{Require(Hex(route,64),"Signal lookup route invalid");
        const Query query{RandomId(),route,L"",Now()+requestSeconds,{}};Channel channel(node_,stop_);Writer out(Kind::Lookup);Put(out,query);channel.Write(out.data);Reader input(channel.Read());auto reply=GetReply(input);
        Require(reply.id==query.id&&reply.route==route,"Signal lookup reply identity invalid");std::set<std::wstring> endpoints;for(const auto& capsule:reply.capsules){Require(capsule.route==route&&ValidCapsule(capsule,Now())&&endpoints.insert(capsule.endpoint).second,"Signal lookup capsule invalid");}results=std::move(reply.capsules);return true;
    }catch(const std::exception& e){error=Error(e);return false;}catch(...){error=L"Signal lookup failed";return false;}}
} // namespace tcard_signal
