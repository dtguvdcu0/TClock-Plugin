#pragma once
#include "card_sync_crypto.h"
#include <string>

namespace tcard_protocol {
using Nonce=std::array<uint8_t,32>;
struct Session { Nonce client{},server{},binding{}; };
bool Challenge(Nonce& nonce);
std::wstring Address(const tcard_crypto::Public& identity);
// TLS exporter, both challenges, replica scope, version, identity and sender role are signed.
bool Prove(const tcard_crypto::Keys& keys,const Session& session,bool server,tcard_crypto::Bytes& signature);
bool Accept(const tcard_crypto::Public& identity,const Session& session,bool server,const tcard_crypto::Bytes& signature);
// Capability-gated continuations bind purpose and sequence without changing seed/key domains.
bool Continue(const Session& authenticated,uint64_t sequence,bool control,Session& context);
// Legacy pairs remain fresh-session scoped; negotiated streams use sequenced contexts.
bool Seal(const tcard_crypto::Keys& keys,const Session& session,bool server,const tcard_crypto::Bytes& plain,tcard_crypto::Bytes& envelope);
bool Open(const tcard_crypto::Keys& keys,const Session& session,bool server,const tcard_crypto::Bytes& envelope,tcard_crypto::Bytes& plain);
}
