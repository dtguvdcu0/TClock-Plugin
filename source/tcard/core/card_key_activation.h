#pragma once
#include "card_storage_transition.h"
#include <utility>

// TCard owns Resume on its quiesced UI thread. The worker only stages credentials.
// The independent root barriers survive the engine's earlier complete decision.
namespace tcard_activation {
enum class Intent { Rotate, Merge };
struct Status {
    std::wstring phase,transaction,group,intent;
    bool active=false;
    std::string error;
};
bool Inspect(const std::wstring& profile,Status& status);
// Malformed, missing-key or inaccessible ledgers are pending, never admission.
bool Pending(const std::wstring& profile);
// The credential service has verified that the staged private credentials derive candidate.
bool Stage(const std::wstring& native,const std::wstring& profile,const std::wstring& group,
    const tcard_crypto::Keys& candidate,Status& status,Intent intent=Intent::Rotate);
// Only the quiesced native owner calls this after verified private credential staging.
// The source-only candidate is sealed by tcard_replacement::Build under candidate keys.
bool StageReplacement(const std::wstring& native,const std::wstring& profile,const std::wstring& group,
    const tcard_crypto::Keys& candidate,const std::wstring& replacement,Status& status);
// Only the native reconstruction entry point can construct this in-process proof.
// Storage still verifies its complete generation catalog before and after migration.
class ReplacementProof {
    std::wstring metadata_;
    explicit ReplacementProof(std::wstring metadata):metadata_(std::move(metadata)){}
    friend bool StageReplacement(const std::wstring&,const std::wstring&,const std::wstring&,const tcard_crypto::Keys&,const std::wstring&,Status&);
    friend bool StageVerified(const std::wstring&,const std::wstring&,const std::wstring&,const tcard_crypto::Keys&,const ReplacementProof&,Status&);
public:
    ReplacementProof(const ReplacementProof&)=delete;
};
bool Resume(const std::wstring& native,const std::wstring& profile,Status& status,
    const tcard_transition::Progress& progress={});
}
