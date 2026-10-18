#pragma once
#include "tcard_core.h"
#include <cstdint>
#include <optional>
#include <bit>
#include <cmath>
#include <algorithm>
#include <functional>

namespace tcard_dependencies { struct Context; struct Manifest; class Reader; }

namespace tcard_sync {

// Versioned journal; old v1 IDs remain readable. These APIs do not start network services.
struct Display {
    std::wstring color=L"#FFF5A8",fontFamily=L"Segoe UI";
    double fontSize=14;
    bool inheritFontFamily=true,inheritFontSize=true,live=true;
    bool operator==(const Display&) const = default;
};
inline Display sync_display(const tcard::CardRecord& card)
{
    Display value{card.color.empty()?L"#FFF5A8":card.color,card.fontFamily.empty()?L"Segoe UI":card.fontFamily,
        std::clamp(card.fontSize,8.0,48.0),card.inheritFontFamily,card.inheritFontSize,card.live};
    return value;
}
inline void sync_display(tcard::CardRecord& card,const Display& value)
{
    card.color=value.color;card.fontFamily=value.fontFamily;card.fontSize=value.fontSize;
    card.inheritFontFamily=value.inheritFontFamily;card.inheritFontSize=value.inheritFontSize;card.live=value.live;
}
struct Dates {
    uint64_t created=0,updated=0; // UTC FILETIME milliseconds; distinct from causal operation Time.
    bool operator==(const Dates&) const=default;
};
inline uint64_t sync_read_time(FILETIME value)
{
    ULARGE_INTEGER ticks{};ticks.LowPart=value.dwLowDateTime;ticks.HighPart=value.dwHighDateTime;return ticks.QuadPart/10000;
}
inline FILETIME sync_make_time(uint64_t milliseconds)
{
    ULARGE_INTEGER ticks{};ticks.QuadPart=milliseconds*10000;return {ticks.LowPart,ticks.HighPart};
}
struct Snapshot {
    std::wstring source, title;
    unsigned format = 0; // 0: Plain, 1: Markdown, 2: RichHtml.
    bool deleted = false;
    std::optional<Display> display; // Absent in legacy operations: preserve the receiver's appearance.
    std::optional<Dates> dates; // Version 3 authenticates authored dates; old IDs retain their canonical bytes.
    bool operator==(const Snapshot&) const = default;
};
struct Change {
    std::wstring id, note, actor;
    uint64_t sequence = 0, time = 0;
    std::vector<std::wstring> parents;
    Snapshot before, after;
};
struct Result {
    size_t applied = 0, pending = 0;
    bool partial = false; // Valid package; one or more independent note commits failed.
    std::wstring error;
    std::vector<std::wstring> touched; // Native publication attempted; refresh even after a later failure.
};

std::wstring IdentifyChange(const Change& change);
bool MergeChanges(const std::vector<Change>& changes, Snapshot& merged, std::wstring& error);
bool ExportChanges(const std::wstring& storagePath, std::wstring& package, Result& result);
// Committed IDs/assets only; still validates all retained content. No durable cache.
// maximumVersion preserves the authenticated peer capability check without serializing bodies.
bool ExportInventory(const std::wstring& storagePath, std::wstring& header, Result& result, unsigned& maximumVersion);
namespace selection { struct Inventory; }
// Fresh verified header with missing causal operation/image units; committed inventories advance bounded batches.
// An optional captured header restricts body membership, never the fresh advertised inventory.
bool ExportSelection(const std::wstring& storagePath, const selection::Inventory& peer, std::wstring& package, Result& result, unsigned& maximumVersion, bool readonly = false, bool objects = false, const std::wstring& pinnedHeader = L"", size_t reservedBytes = 0);
// Read-only committed inventory for owner receipts and bounded mailbox progress.
bool ExportCommittedInventory(const std::wstring& storagePath, std::wstring& header, Result& result, unsigned& maximumVersion);
// Read-only committed export for a TCard-owned background reader; refuses legacy seeding.
bool ExportSnapshot(const std::wstring& storagePath, std::wstring& package, Result& result);
struct PreparedNote {
    std::wstring id, package;
};
// Validate the complete authenticated exchange before exposing independent note-sized work.
// This read-only preparation may run in a background apartment; imports stay with the owner.
bool PrepareChanges(const std::wstring& package, std::vector<PreparedNote>& notes, Result& result);
// The caller supplies authenticated data. This API validates content, not peer identity.
bool ImportChanges(const std::wstring& storagePath, const std::wstring& package, Result& result);
// Owner-thread, note-sized application. Revalidates content and current target storage.
// Existing flat local bundles fall back to the full migration-aware importer.
bool ImportPreparedNote(const std::wstring& storagePath, const PreparedNote& note, Result& result);
// Explicit encrypted-object path: one original operation, bounded raw source/image references.
// Staged objects are custody only. Preparation is read-only; application stays on the owner thread.
// Caller authenticates the reference manifest and explicitly admits the object capability.
// objectRoot may be a protected same-group immutable mailbox/transfer directory.
bool PrepareObjectNote(const std::wstring& storagePath, const std::wstring& package, PreparedNote& note, Result& result, const std::wstring& objectRoot = L"");
bool ImportObjectNote(const std::wstring& storagePath, const PreparedNote& note, Result& result, const std::wstring& objectRoot = L"");
// Optional dependency capability; expected context comes from the authenticated transport/owner boundary.
// The immutable canonical captured header is Context.target; candidate storage may have another path/node.
// Original operation IDs and the old import/export signatures remain unchanged.
bool ExportDependencies(const std::wstring& storagePath, const selection::Inventory& peer, const tcard_dependencies::Context& context, std::wstring& package, Result& result, unsigned& maximumVersion);
bool PrepareDependencyNote(const std::wstring& storagePath, const std::wstring& package, const tcard_dependencies::Context& context, tcard_dependencies::Reader& reader, PreparedNote& note, Result& result);
bool ImportDependencyNote(const std::wstring& storagePath, const PreparedNote& note, const tcard_dependencies::Context& context, tcard_dependencies::Reader& reader, Result& result);
// Internal committed-copy proof, never an owner ACK. Tokens are noncopyable and live only in the guarded callback.
class DependencyCommitProof {
    std::wstring owner_,vault_,directory_,manifest_,group_;
    bool valid_=false,active_=false;
    void Clear()noexcept{owner_.clear();vault_.clear();directory_.clear();manifest_.clear();group_.clear();valid_=active_=false;}
    friend bool VerifyDependencyCommit(const std::wstring&,const PreparedNote&,const tcard_dependencies::Context&,DependencyCommitProof&,Result&);
    friend bool WithDependencyCommit(const std::wstring&,const std::wstring&,const PreparedNote&,const tcard_dependencies::Context&,const std::function<bool(const DependencyCommitProof&)>&,Result&);
public:
    DependencyCommitProof()=default;
    DependencyCommitProof(const DependencyCommitProof&)=delete;
    DependencyCommitProof& operator=(const DependencyCommitProof&)=delete;
    DependencyCommitProof(DependencyCommitProof&&)=delete;
    DependencyCommitProof& operator=(DependencyCommitProof&&)=delete;
    bool Matches(const std::wstring& committedStorage,const tcard_dependencies::Manifest& manifest)const noexcept;
};
// Standalone verification does not activate a token for cleanup. Reader authority is the immutable replica, not supplied custody.
bool VerifyDependencyCommit(const std::wstring& storage,const PreparedNote& note,const tcard_dependencies::Context& context,DependencyCommitProof& proof,Result& result);
// Call before any Pool mutex/other owner lock. The narrow internal callback may delete only its matching duplicate custody.
// Acquires sync_mutex then canonical sorted vault/native/custody owners before resolving a generation; callback stays inside those locks.
bool WithDependencyCommit(const std::wstring& storage,const std::wstring& custodyRoot,const PreparedNote& note,const tcard_dependencies::Context& context,const std::function<bool(const DependencyCommitProof&)>& cleanup,Result& result);
// Narrow internal Signal protection boundary; caller quiesces its UI/worker and passes admitted native/profile roots.
// Acquires process mutex then sorted stable native/key/profile/control owners before any callback mutation.
// The callback does not grant Signal admission or replace fresh protected-root/identity verification.
bool WithSignalProtection(const std::wstring& nativeRoot,const std::wstring& profileRoot,const std::function<bool()>& protect,Result& result);
// Revalidate and retry retained authenticated receipts; call on the storage owner thread.
bool RetryChanges(const std::wstring& storagePath, Result& result);

}
