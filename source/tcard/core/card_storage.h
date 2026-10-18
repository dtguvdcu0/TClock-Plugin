#pragma once
#include "card_sync_crypto.h"
#include <string>

// Native encrypted file boundary; networking and identity provisioning stay in TCardSync.
namespace tcard_storage {
// Required also returns true for an interrupted or locked encrypted store.
bool Required(const std::wstring& path);
bool Ready(const std::wstring& path);
bool Resume(const std::wstring& root,std::string& error);
// Load only the local data key/public identity after checking active controls; no signing key.
bool LoadKey(const std::wstring& root,tcard_crypto::Keys& keys);
bool Matches(const std::wstring& path,const tcard_crypto::Keys& keys);
bool Pack(const std::wstring& path,const tcard_crypto::Bytes& plain,tcard_crypto::Bytes& stored);
bool Unpack(const std::wstring& path,const tcard_crypto::Bytes& stored,tcard_crypto::Bytes& plain);
// Caller owns/quiesces the native store. No automatic key rotation or group switch.
// Durable pending state blocks ordinary reads/writes until verified conversion completes.
bool Enable(const std::wstring& root,const tcard_crypto::Keys& keys,std::string& error);
}
