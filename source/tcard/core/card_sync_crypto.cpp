#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include "card_sync_crypto.h"
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <mutex>
#pragma comment(lib,"bcrypt.lib")

namespace tcard_crypto {
namespace {
constexpr uint8_t salt[]="TCard.Sync.v2.seed";
constexpr size_t prefix=4+65+16+32+1,header=prefix+12+4,tagSize=16;
static_assert(header+tagSize==envelopeOverhead);
struct Secret {
    std::array<uint8_t,32> bytes{};
    ~Secret(){SecureZeroMemory(bytes.data(),bytes.size());}
};
void crypto_initialize()
{
    // Process-lifetime reference, matching the existing transport initialization lifetime.
    // The pinned backend requires DRBG/global mutex initialization before direct RNG use.
    static std::once_flag once;std::call_once(once,[]{if(wolfCrypt_Init()!=0)throw std::runtime_error("Crypto initialization failed");});
}
struct Random {
    WC_RNG value{};
    Random(){crypto_initialize();if(wc_InitRng(&value))throw std::runtime_error("Crypto RNG unavailable");}
    ~Random(){wc_FreeRng(&value);}
};
struct Curve {
    ecc_key value{};
    Curve(){crypto_initialize();if(wc_ecc_init(&value))throw std::runtime_error("Curve unavailable");}
    ~Curve(){wc_ecc_free(&value);}
};
struct Cipher {
    Aes value{};
    explicit Cipher(const Secret& key){
        crypto_initialize();if(wc_AesInit(&value,nullptr,INVALID_DEVID))throw std::runtime_error("Cipher unavailable");
        if(wc_AesGcmSetKey(&value,key.bytes.data(),32)){wc_AesFree(&value);throw std::runtime_error("Cipher key rejected");}
    }
    ~Cipher(){wc_AesFree(&value);}
};
bool crypto_equal(const uint8_t* a,const uint8_t* b,size_t count)
{
    uint8_t different=0;for(size_t i=0;i<count;++i)different|=a[i]^b[i];return different==0;
}
void crypto_clear(Bytes& bytes)
{
    if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());bytes.clear();
}
Bytes crypto_label(const char* text)
{
    return Bytes(text,text+strlen(text)); // ASCII protocol constants, never paths/commands.
}
bool crypto_derive(const uint8_t* input,size_t count,const Bytes& info,Secret& out)
{
    return wc_HKDF(WC_SHA256,input,static_cast<word32>(count),salt,sizeof(salt)-1,
        info.data(),static_cast<word32>(info.size()),out.bytes.data(),32)==0;
}
bool crypto_valid(const Context& context)
{
    return context.kind>=1&&context.kind<=5&&
        std::any_of(context.generation.begin(),context.generation.end(),[](uint8_t b){return b!=0;})&&
        std::any_of(context.object.begin(),context.object.end(),[](uint8_t b){return b!=0;});
}
Bytes crypto_context(const Public& identity,const Context& context)
{
    Bytes bytes{'T','C','E','2'};bytes.insert(bytes.end(),identity.begin(),identity.end());
    bytes.insert(bytes.end(),context.generation.begin(),context.generation.end());
    bytes.insert(bytes.end(),context.object.begin(),context.object.end());bytes.push_back(context.kind);return bytes;
}
bool crypto_object(const Keys& keys,const Bytes& prefixBytes,Secret& objectKey)
{
    auto info=crypto_label("TCard.Sync.v2.object:");info.insert(info.end(),prefixBytes.begin(),prefixBytes.end());
    return crypto_derive(keys.dataKey.data(),32,info,objectKey);
}
bool crypto_digest(const Bytes& transcript,Secret& digest)
{
    if(transcript.empty()||transcript.size()>65536)return false;
    auto bytes=crypto_label("TCard.Sync.v2.proof:");bytes.insert(bytes.end(),transcript.begin(),transcript.end());
    return wc_Sha256Hash(bytes.data(),static_cast<word32>(bytes.size()),digest.bytes.data())==0;
}
}
Keys::~Keys()
{
    SecureZeroMemory(privateKey.data(),privateKey.size());SecureZeroMemory(dataKey.data(),dataKey.size());SecureZeroMemory(channelKey.data(),channelKey.size());
}
bool Derive(const Seed& seed,Keys& keys)
{
    SecureZeroMemory(keys.privateKey.data(),32);SecureZeroMemory(keys.dataKey.data(),32);SecureZeroMemory(keys.channelKey.data(),32);keys.publicKey={};
    try{
        // P-256 subgroup order; rejection sampling avoids biased scalar reduction.
        constexpr std::array<uint8_t,32> order{0xff,0xff,0xff,0xff,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
        Random random;Secret scalar,data,channel;
        for(unsigned attempt=0;attempt<256;++attempt){
            auto info=crypto_label("TCard.Sync.v2.identity:");
            for(int shift=24;shift>=0;shift-=8)info.push_back(static_cast<uint8_t>(attempt>>shift));
            if(!crypto_derive(seed.data(),seed.size(),info,scalar))return false;
            if(std::all_of(scalar.bytes.begin(),scalar.bytes.end(),[](uint8_t b){return b==0;})||
                !std::lexicographical_compare(scalar.bytes.begin(),scalar.bytes.end(),order.begin(),order.end()))continue;
            Curve curve;
            if(wc_ecc_import_private_key_ex(scalar.bytes.data(),32,nullptr,0,&curve.value,ECC_SECP256R1)||
                wc_ecc_set_rng(&curve.value,&random.value)||wc_ecc_make_pub_ex(&curve.value,nullptr,&random.value)||
                wc_ecc_check_key(&curve.value))return false;
            Public publicKey{};word32 length=static_cast<word32>(publicKey.size());
            if(wc_ecc_export_x963(&curve.value,publicKey.data(),&length)||length!=publicKey.size()||
                !crypto_derive(seed.data(),seed.size(),crypto_label("TCard.Sync.v2.data"),data)||
                !crypto_derive(seed.data(),seed.size(),crypto_label("TCard.Sync.v2.transport"),channel))return false;
            keys.privateKey=scalar.bytes;keys.dataKey=data.bytes;keys.channelKey=channel.bytes;keys.publicKey=publicKey;return true;
        }
    }catch(...){}return false;
}
bool Sign(const Keys& keys,const Bytes& transcript,Bytes& signature)
{
    if(&transcript==&signature)return false;signature.clear();
    try{
        Secret digest;if(!crypto_digest(transcript,digest))return false;
        Curve curve;Random random;
        if(wc_ecc_import_private_key_ex(keys.privateKey.data(),32,keys.publicKey.data(),65,&curve.value,ECC_SECP256R1)||
            wc_ecc_check_key(&curve.value))return false;
        Bytes output(80);word32 length=static_cast<word32>(output.size());
        if(wc_ecc_sign_hash(digest.bytes.data(),32,output.data(),&length,&random.value,&curve.value))return false;
        output.resize(length);signature=std::move(output);return true;
    }catch(...){}return false;
}
bool Verify(const Public& identity,const Bytes& transcript,const Bytes& signature)
{
    try{
        if(identity[0]!=4||signature.empty()||signature.size()>80)return false;
        Secret digest;if(!crypto_digest(transcript,digest))return false;Curve curve;
        if(wc_ecc_import_x963_ex(identity.data(),65,&curve.value,ECC_SECP256R1)||wc_ecc_check_key(&curve.value))return false;
        int valid=0;return wc_ecc_verify_hash(signature.data(),static_cast<word32>(signature.size()),digest.bytes.data(),32,&valid,&curve.value)==0&&valid==1;
    }catch(...){}return false;
}
bool Seal(const Keys& keys,const Context& context,const Bytes& plaintext,Bytes& envelope)
{
    if(&plaintext==&envelope)return false;envelope.clear();
    try{
        if(!crypto_valid(context)||keys.publicKey[0]!=4||plaintext.size()>maxObject)return false;
        Bytes bytes=crypto_context(keys.publicKey,context);Secret key;if(!crypto_object(keys,bytes,key))return false;
        bytes.resize(header+plaintext.size()+tagSize);
        if(BCryptGenRandom(nullptr,bytes.data()+prefix,12,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
        for(size_t i=0;i<4;++i)bytes[prefix+12+i]=static_cast<uint8_t>(plaintext.size()>>(24-i*8));
        Cipher cipher(key);uint8_t empty=0;
        if(wc_AesGcmEncrypt(&cipher.value,bytes.data()+header,plaintext.empty()?&empty:plaintext.data(),static_cast<word32>(plaintext.size()),
            bytes.data()+prefix,12,bytes.data()+header+plaintext.size(),tagSize,bytes.data(),header))return false;
        envelope=std::move(bytes);return true;
    }catch(...){}return false;
}
bool Open(const Keys& keys,const Context& expected,const Bytes& envelope,Bytes& plaintext)
{
    if(&envelope==&plaintext)return false;crypto_clear(plaintext);Bytes output;
    try{
        if(!crypto_valid(expected)||keys.publicKey[0]!=4||envelope.size()<header+tagSize||envelope.size()>header+maxObject+tagSize)return false;
        const auto context=crypto_context(keys.publicKey,expected);
        if(!crypto_equal(context.data(),envelope.data(),prefix))return false;
        size_t length=0;for(size_t i=0;i<4;++i)length=(length<<8)|envelope[prefix+12+i];
        if(length>maxObject||envelope.size()!=header+length+tagSize)return false;
        Secret key;if(!crypto_object(keys,context,key))return false;Cipher cipher(key);output.resize(length);uint8_t empty=0;
        if(wc_AesGcmDecrypt(&cipher.value,output.empty()?&empty:output.data(),envelope.data()+header,static_cast<word32>(length),
            envelope.data()+prefix,12,envelope.data()+header+length,tagSize,envelope.data(),header)){crypto_clear(output);return false;}
        plaintext=std::move(output);return true;
    }catch(...){crypto_clear(output);return false;}
}
}
