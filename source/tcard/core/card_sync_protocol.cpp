#include <winsock2.h>
#include "card_sync_protocol.h"
#include "card_assets.h"

namespace tcard_protocol {
namespace {
using tcard_crypto::Bytes;
Bytes wire_transcript(const tcard_crypto::Public& identity,const Session& session,bool server)
{
    for(const auto& value:{session.client,session.server,session.binding})
        if(std::all_of(value.begin(),value.end(),[](uint8_t c){return c==0;}))throw std::runtime_error("Missing session freshness");
    // Fixed ASCII protocol domain, never a path or executable command.
    constexpr char label[]="TCard.Sync.v2.replica-session:";Bytes bytes(label,label+sizeof(label)-1);
    bytes.push_back(server?2:1);bytes.insert(bytes.end(),identity.begin(),identity.end());
    bytes.insert(bytes.end(),session.client.begin(),session.client.end());bytes.insert(bytes.end(),session.server.begin(),session.server.end());
    bytes.insert(bytes.end(),session.binding.begin(),session.binding.end());return bytes;
}
std::array<uint8_t,32> wire_hash(const Bytes& bytes)
{
    const auto hex=tcard_asset::hash(bytes);if(hex.size()!=64)throw std::runtime_error("Session digest failed");
    std::array<uint8_t,32> digest{};auto nibble=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};
    for(size_t i=0;i<32;++i)digest[i]=static_cast<uint8_t>((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));return digest;
}
tcard_crypto::Context wire_context(const tcard_crypto::Public& identity,const Session& session,bool server)
{
    const auto digest=wire_hash(wire_transcript(identity,session,server));tcard_crypto::Context context;
    std::copy_n(digest.begin(),16,context.generation.begin());context.kind=5;return context;
}
}
bool Challenge(Nonce& nonce)
{
    nonce={};return BCryptGenRandom(nullptr,nonce.data(),static_cast<ULONG>(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0&&
        std::any_of(nonce.begin(),nonce.end(),[](uint8_t c){return c!=0;});
}
std::wstring Address(const tcard_crypto::Public& identity){return tcard_asset::hash(Bytes(identity.begin(),identity.end()));}
bool Prove(const tcard_crypto::Keys& keys,const Session& session,bool server,Bytes& signature)
{
    signature.clear();try{return tcard_crypto::Sign(keys,wire_transcript(keys.publicKey,session,server),signature);}catch(...){return false;}
}
bool Accept(const tcard_crypto::Public& identity,const Session& session,bool server,const Bytes& signature)
{
    try{return tcard_crypto::Verify(identity,wire_transcript(identity,session,server),signature);}catch(...){return false;}
}
bool Continue(const Session& authenticated,uint64_t sequence,bool control,Session& context)
{
    context={};try{
        if(!sequence||sequence>9007199254740991ULL)return false;
        // Independent message contexts, not new authentication or encryption key derivation.
        constexpr char label[]="TCard.Sync.v2.persistent-context:";Bytes bytes(label,label+sizeof(label)-1);
        for(const auto& value:{authenticated.client,authenticated.server,authenticated.binding}){
            if(std::all_of(value.begin(),value.end(),[](uint8_t c){return c==0;}))return false;
            bytes.insert(bytes.end(),value.begin(),value.end());
        }
        bytes.push_back(control?1:2);for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<uint8_t>(sequence>>(56-i*8)));
        context=authenticated;context.binding=wire_hash(bytes);return true;
    }catch(...){context={};return false;}
}
bool Seal(const tcard_crypto::Keys& keys,const Session& session,bool server,const Bytes& plain,Bytes& envelope)
{
    if(&plain==&envelope)return false;envelope.clear();try{
        if(plain.size()>tcard_crypto::maxObject)return false;auto context=wire_context(keys.publicKey,session,server);
        context.object=wire_hash(plain);return tcard_crypto::Seal(keys,context,plain,envelope);
    }catch(...){return false;}
}
bool Open(const tcard_crypto::Keys& keys,const Session& session,bool server,const Bytes& envelope,Bytes& plain)
{
    if(&envelope==&plain)return false;plain.clear();try{
        if(envelope.size()<tcard_crypto::envelopeOverhead)return false;auto context=wire_context(keys.publicKey,session,server);
        std::copy_n(envelope.begin()+85,32,context.object.begin());Bytes verified;
        if(!tcard_crypto::Open(keys,context,envelope,verified)||wire_hash(verified)!=context.object)return false;
        plain=std::move(verified);return true;
    }catch(...){return false;}
}
}
