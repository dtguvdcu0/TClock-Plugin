#pragma once
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include "signal_protocol.h"

namespace tcard_signal {
using Bytes=std::vector<unsigned char>;
inline std::wstring Encode(const unsigned char* bytes,size_t count){const wchar_t hex[]=L"0123456789abcdef";std::wstring result;result.reserve(count*2);for(size_t i=0;i<count;++i){result+=hex[bytes[i]>>4];result+=hex[bytes[i]&15];}return result;}
inline Bytes Decode(const std::wstring& value){if(value.size()%2||value.size()>maxMessage||!Hex(value,value.size()))return {};Bytes result;result.reserve(value.size()/2);auto digit=[](wchar_t c){return c<=L'9'?c-L'0':c-L'a'+10;};for(size_t i=0;i<value.size();i+=2)result.push_back(static_cast<unsigned char>((digit(value[i])<<4)|digit(value[i+1])));return result;}
inline Bytes Ascii(const std::wstring& text){if(text.size()>maxMessage)throw std::runtime_error("Signaling identity size exceeded");Bytes result;result.reserve(text.size());for(wchar_t c:text){if(c>127)throw std::runtime_error("Non-ASCII signaling identity");result.push_back(static_cast<unsigned char>(c));}return result;}
inline std::wstring Digest(const Bytes& bytes){if(bytes.size()>maxMessage)throw std::runtime_error("Signaling hash size exceeded");unsigned char hash[32]{};if(wc_Sha256Hash(bytes.data(),static_cast<word32>(bytes.size()),hash)!=0)throw std::runtime_error("Signaling hash failed");return Encode(hash,32);}
inline std::wstring Route(const std::wstring& publicKey){auto key=Decode(publicKey);if(key.size()!=65||key.front()!=4)throw std::runtime_error("Invalid signaling public identity");const char label[]="TCard.Signal.v1.route:";Bytes bytes(label,label+sizeof(label)-1);bytes.insert(bytes.end(),key.begin(),key.end());return Digest(bytes);}
inline Bytes CapsuleProof(const Capsule& value){return Ascii(L"TCard.Signal.v1.capsule:"+value.route+L":"+value.endpoint+L":"+value.ip+L":"+std::to_wstring(value.port)+L":"+std::to_wstring(value.sequence)+L":"+std::to_wstring(value.expires));}
inline std::wstring CapsuleDigest(const Capsule& value){auto body=CapsuleProof(value);const auto suffix=Ascii(L":"+value.publicKey+L":"+value.signature);body.insert(body.end(),suffix.begin(),suffix.end());return Digest(body);}
inline Bytes SubmissionProof(const Submission& value){return Ascii(L"TCard.Signal.v1.register:"+CapsuleDigest(value.capsule)+L":"+value.node+L":"+value.nonce+L":"+value.tlsBinding);}
inline bool Verify(const std::wstring& publicKey,const Bytes& transcript,const std::wstring& signature) noexcept{
    try{auto key=Decode(publicKey),proof=Decode(signature);if(key.size()!=65||key.front()!=4||proof.empty()||proof.size()>80||transcript.empty()||transcript.size()>maxMessage)return false;
        // Reuse the fixed current v2 signing domain without exporting or deriving synchronization keys here.
        const char label[]="TCard.Sync.v2.proof:";Bytes body(label,label+sizeof(label)-1);body.insert(body.end(),transcript.begin(),transcript.end());unsigned char hash[32]{};
        ecc_key curve{};if(wc_Sha256Hash(body.data(),static_cast<word32>(body.size()),hash)!=0||wc_ecc_init(&curve)!=0)return false;
        int valid=0;const bool okay=wc_ecc_import_x963_ex(key.data(),static_cast<word32>(key.size()),&curve,ECC_SECP256R1)==0&&wc_ecc_check_key(&curve)==0&&wc_ecc_verify_hash(proof.data(),static_cast<word32>(proof.size()),hash,32,&valid,&curve)==0&&valid==1;wc_ecc_free(&curve);return okay;
    }catch(...){return false;}
}
inline bool ValidCapsule(const Capsule& value,uint64_t now) noexcept{
    try{return Hex(value.route,64)&&Hex(value.endpoint,32)&&IPv4(value.ip)&&value.port&&Hex(value.publicKey,130)&&value.sequence&&value.sequence<=maxSequence&&value.expires>now&&value.expires-now<=leaseSeconds&&value.route==Route(value.publicKey)&&Verify(value.publicKey,CapsuleProof(value),value.signature);}catch(...){return false;}
}
inline bool ValidSubmission(const Submission& value,const PublicChallenge& expected,uint64_t now) noexcept{
    try{return Hex(expected.node,64)&&Hex(expected.nonce,64)&&Hex(expected.tlsBinding,64)&&value.node==expected.node&&value.nonce==expected.nonce&&value.tlsBinding==expected.tlsBinding&&ValidCapsule(value.capsule,now)&&Verify(value.capsule.publicKey,SubmissionProof(value),value.proof);}catch(...){return false;}
}
}
