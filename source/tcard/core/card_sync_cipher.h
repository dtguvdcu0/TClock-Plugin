#pragma once
#include "card_sync_crypto.h"
#include <string>

// Worker-owned experimental v2 store. Never pass the live card directory as root.
namespace tcard_cipher {
struct Object {
    std::wstring id,name; // Stable logical ID and encrypted snapshot name, never a disk path.
    uint8_t kind=0;
    tcard_crypto::Bytes bytes;
};
struct Status {
    size_t retained=0,remaining=0;
    bool active=false;
    std::string error;
};
inline constexpr size_t maxEntries=10000;
// A durable journal refuses a second Begin until the first generation is activated.
bool Begin(const std::wstring& root,const tcard_crypto::Seed& candidate,Status& status);
// Snapshot is complete and captured under the source writer lock through activation.
// nullptr resumes the last persisted snapshot; an empty vector intentionally clears heads.
// budget limits old-generation work, never drops accepted snapshot changes.
bool Advance(const std::wstring& root,const std::vector<Object>* snapshot,size_t budget,bool activate,Status& status);
bool Read(const std::wstring& root,const std::wstring& id,uint8_t kind,tcard_crypto::Bytes& bytes);
bool ReadHeads(const std::wstring& root,std::vector<Object>& snapshot);
}
