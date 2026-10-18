#pragma once
#include "card_storage.h"
#include <vector>
#include <functional>

// The owner stops its worker and quiesces UI/background writers before Commit.
// Begin/Advance permit normal encrypted edits; Commit recaptures changed files.
// Transaction/recovery directories must be outside every participating root.
namespace tcard_transition {
enum class Mode { Rotate, Decrypt };
struct Status {
    std::wstring phase,recovery;
    size_t total=0,remaining=0;
    bool active=false;
    std::string error;
};
bool Begin(const std::wstring& transaction,const std::vector<std::wstring>& roots,
    const tcard_crypto::Keys& candidate,Mode mode,Status& status);
bool Advance(const std::wstring& transaction,size_t budget,Status& status);
using Progress=std::function<void(const std::wstring&,size_t,size_t)>;
// Progress can report UI status or interrupt a checkpoint; later Commit resumes safely.
bool Commit(const std::wstring& transaction,Status& status,const Progress& progress={});
bool Inspect(const std::wstring& transaction,Status& status);
// Returns the protected journal path named by a valid root barrier; malformed barriers throw.
std::wstring Pending(const std::wstring& root);
}
