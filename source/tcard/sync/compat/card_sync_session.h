#pragma once
#include "card_sync_seed.h"
#include "../../core/card_sync_protocol.h"

// Current 21-word credentials require authenticated v2 sessions; no legacy fallback.
namespace tcard_compat {
inline std::unique_ptr<tcard_crypto::Keys> SessionKeys(const tcard_sync::Credentials& credentials)
{
    auto suffix=tcard_words::Suffix(credentials.transportKey.data());const bool compact=std::equal(suffix.begin(),suffix.begin()+4,credentials.transportKey.begin()+28);
    SecureZeroMemory(suffix.data(),suffix.size());if(!compact)throw std::runtime_error("Unsupported recovery key format");
    tcard_sync::Credentials expected=credentials;tcard_words::Finish(expected);
    struct Wipe{tcard_sync::Credentials& value;~Wipe(){SecureZeroMemory(value.transportKey.data(),32);}} wipe{expected};
    if(expected.space!=credentials.space)throw std::runtime_error("Compact credential binding differs");
    tcard_crypto::Seed seed{};std::copy_n(credentials.transportKey.begin(),28,seed.begin());
    auto keys=std::make_unique<tcard_crypto::Keys>();const bool okay=tcard_crypto::Derive(seed,*keys);SecureZeroMemory(seed.data(),seed.size());
    if(!okay)throw std::runtime_error("Session key derivation failed");return keys;
}
}
