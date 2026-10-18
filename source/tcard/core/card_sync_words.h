#pragma once
#include "card_sync_transport.h"
#include "card_assets.h"
#include "../third_party/mnemonic/english.h"
#include <array>
#include <algorithm>

// Portable words are implemented only by TCardSync and its native tests.
namespace tcard_words {
inline std::array<BYTE,32> Digest(const std::vector<BYTE>& bytes)
{
    const auto hex=tcard_asset::hash(bytes);if(hex.size()!=64)throw std::runtime_error("Word digest failed");
    auto nibble=[](wchar_t c)->BYTE{return static_cast<BYTE>(c<=L'9'?c-L'0':c-L'a'+10);};
    std::array<BYTE,32> result{};for(size_t i=0;i<32;++i)result[i]=static_cast<BYTE>((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));return result;
}
inline void Finish(tcard_sync::Credentials& keys)
{
    // ASCII domain bytes are versioned protocol constants, never paths or commands.
    const char domain[]="TCard.Space.v1:";std::vector<BYTE> material(domain,domain+sizeof(domain)-1);
    material.insert(material.end(),keys.transportKey.begin(),keys.transportKey.end());keys.space=tcard_asset::hash(material);
    SecureZeroMemory(material.data(),material.size());
}
inline std::array<BYTE,32> Suffix(const BYTE* entropy)
{
    const char domain[]="TCard.WordKey.v1:";std::vector<BYTE> material(domain,domain+sizeof(domain)-1);
    material.insert(material.end(),entropy,entropy+28);auto digest=Digest(material);SecureZeroMemory(material.data(),material.size());return digest;
}
inline bool Create(tcard_sync::Credentials& keys)
{
    keys={};if(BCryptGenRandom(nullptr,keys.transportKey.data(),28,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    auto suffix=Suffix(keys.transportKey.data());std::copy_n(suffix.begin(),4,keys.transportKey.begin()+28);
    SecureZeroMemory(suffix.data(),suffix.size());Finish(keys);return true;
}
inline std::wstring Encode(const tcard_sync::Credentials& keys)
{
    auto suffix=Suffix(keys.transportKey.data());const bool compact=std::equal(suffix.begin(),suffix.begin()+4,keys.transportKey.begin()+28);
    SecureZeroMemory(suffix.data(),suffix.size());if(!compact)throw std::runtime_error("Unsupported recovery key format");constexpr size_t bytes=28;
    std::vector<BYTE> entropy(keys.transportKey.begin(),keys.transportKey.begin()+bytes);auto digest=Digest(entropy);
    std::wstring words;const size_t count=(bytes*8+bytes/4)/11;
    for(size_t word=0;word<count;++word){unsigned index=0;
        for(size_t j=0;j<11;++j){const size_t bit=word*11+j;const BYTE byte=bit<bytes*8?entropy[bit/8]:digest[(bit-bytes*8)/8];
            index=(index<<1)|((byte>>(7-bit%8))&1);}
        if(word)words+=L' ';words+=english[index];
    }
    SecureZeroMemory(entropy.data(),entropy.size());SecureZeroMemory(digest.data(),digest.size());return words;
}
inline bool Decode(const std::wstring& text,tcard_sync::Credentials& keys)
{
    keys={};if(text.size()>1024)return false;std::vector<unsigned> indices;size_t at=0;
    while(at<text.size()){
        at=text.find_first_not_of(L" \t\r\n",at);if(at==std::wstring::npos)break;
        const auto end=text.find_first_of(L" \t\r\n",at);auto word=text.substr(at,end==std::wstring::npos?end:end-at);
        for(auto& c:word){if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';if(c<L'a'||c>L'z')return false;}
        const auto found=std::lower_bound(std::begin(english),std::end(english),word,[](const wchar_t* a,const std::wstring& b){return a<b;});
        const bool equal=found!=std::end(english)&&word==*found;
        SecureZeroMemory(word.data(),word.size()*sizeof(wchar_t));if(!equal)return false;
        indices.push_back(static_cast<unsigned>(found-std::begin(english)));if(indices.size()>21)return false;at=end;
    }
    if(indices.size()!=21)return false;constexpr size_t bytes=28;
    std::vector<BYTE> entropy(bytes,0);BYTE checksum=0;
    for(size_t bit=0;bit<indices.size()*11;++bit){const BYTE value=static_cast<BYTE>((indices[bit/11]>>(10-bit%11))&1);
        if(bit<bytes*8)entropy[bit/8]|=static_cast<BYTE>(value<<(7-bit%8));else checksum|=static_cast<BYTE>(value<<(7-(bit-bytes*8)));}
    auto digest=Digest(entropy);constexpr BYTE mask=0xfe;const bool valid=(digest[0]&mask)==checksum;
    if(valid){std::copy(entropy.begin(),entropy.end(),keys.transportKey.begin());
        {auto suffix=Suffix(entropy.data());std::copy_n(suffix.begin(),4,keys.transportKey.begin()+28);SecureZeroMemory(suffix.data(),suffix.size());}}
    SecureZeroMemory(entropy.data(),entropy.size());SecureZeroMemory(digest.data(),digest.size());SecureZeroMemory(indices.data(),indices.size()*sizeof(unsigned));
    if(!valid||std::all_of(keys.transportKey.begin(),keys.transportKey.end(),[](BYTE b){return !b;})){SecureZeroMemory(keys.transportKey.data(),32);return false;}
    Finish(keys);return true;
}
}
