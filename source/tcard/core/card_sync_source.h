#pragma once
#include "card_sync_transport.h"
#include "card_source_snapshot.h"

namespace tcard_sync {
// A fixed replacement source. Readiness is recomputed from authenticated committed originals.
struct SourceReport {SessionReport session;SourceSnapshot snapshot;std::wstring error;};
// This replica must start empty and remain isolated from live owner/profile edits.
// Production callers run FetchSource only in TCardSync, after explicit replacement consent.
bool FetchSource(const std::wstring& replica,const Credentials& credentials,
    const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop={});
// Strict product acquisition: a fresh authenticated remote saved-owner witness precedes any original fetch.
bool FetchOwnerSource(const std::wstring& replica,const Credentials& credentials,
    const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop={});
// Explicit completion only: freshly authenticate the fixed source and advertise verified custody.
// No data operation is uploaded; a completed offline fetch still never opens a socket implicitly.
bool AcknowledgeOwnerSource(const std::wstring& replica,const Credentials& credentials,
    const tcard_net::Peer& peer,const tcard_net::Policy& policy,SourceReport& report,std::stop_token stop={});
// Offline InspectSource/InspectOwnerSource are shared with the native owner through card_source_snapshot.h.
}
