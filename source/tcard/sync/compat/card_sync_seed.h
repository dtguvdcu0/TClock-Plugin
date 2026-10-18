#pragma once
#include "../../core/card_sync_words.h"
#include "../../core/card_sync_crypto.h"

// Current 21-word provisioning boundary. Credential storage types do not enter the crypto API.
namespace tcard_compat {
inline bool DecodeSeed(const std::wstring& words,tcard_crypto::Seed& seed)
{
    seed={};size_t count=0,at=0;
    while((at=words.find_first_not_of(L" \t\r\n",at))!=std::wstring::npos){
        ++count;at=words.find_first_of(L" \t\r\n",at);if(at==std::wstring::npos)break;
    }
    // The 24-word migration has no implicit truncation or silent seed conversion.
    if(count!=21)return false;tcard_sync::Credentials legacy;
    struct Wipe{tcard_sync::Credentials& keys;~Wipe(){SecureZeroMemory(keys.transportKey.data(),32);}} wipe{legacy};
    if(!tcard_words::Decode(words,legacy))return false;
    std::copy_n(legacy.transportKey.begin(),seed.size(),seed.begin());return true;
}
}
