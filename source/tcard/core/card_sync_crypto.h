#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

// Current v2 primitives shared by encrypted storage and authenticated synchronization.
namespace tcard_crypto {
using Bytes=std::vector<uint8_t>;
using Seed=std::array<uint8_t,28>;
using Public=std::array<uint8_t,65>;
struct Keys {
    std::array<uint8_t,32> privateKey{},dataKey{},channelKey{};
    Public publicKey{};
    Keys()=default;
    ~Keys();
    Keys(const Keys&)=delete;
    Keys& operator=(const Keys&)=delete;
};
struct Context {
    std::array<uint8_t,16> generation{};
    std::array<uint8_t,32> object{};
    // 1 source, 2 history, 3 asset, 4 deletion, 5 metadata.
    uint8_t kind=0;
};
inline constexpr std::size_t envelopeOverhead=150;
inline constexpr std::size_t maxObject=16*1024*1024-envelopeOverhead;
bool Derive(const Seed& seed,Keys& keys);
// Caller must supply a fresh, role/session/peer-bound transcript; no replay ledger here.
bool Sign(const Keys& keys,const Bytes& transcript,Bytes& signature);
bool Verify(const Public& identity,const Bytes& transcript,const Bytes& signature);
// Random nonces are generated internally. Open releases plaintext only after authentication.
bool Seal(const Keys& keys,const Context& context,const Bytes& plaintext,Bytes& envelope);
bool Open(const Keys& keys,const Context& expected,const Bytes& envelope,Bytes& plaintext);
}
