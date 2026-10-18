#pragma once
#include "card_storage_transition.h"
#include "card_source_snapshot.h"

namespace tcard_replacement {
struct Candidate {std::wstring path,node,target,catalog;bool ready=false;std::string error;};
// Reconstruct only authenticated target originals in a new encrypted root; never seed old local data.
// This local step may resume after interruption and never changes the owner's native/group selection.
bool Build(const std::wstring& source,const std::wstring& candidate,const tcard_crypto::Keys& keys,
    Candidate& status,const tcard_transition::Progress& progress={});
bool Inspect(const std::wstring& candidate,Candidate& status);
}
