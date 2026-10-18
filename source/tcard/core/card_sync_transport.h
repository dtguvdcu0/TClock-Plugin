#pragma once
#include "card_sync.h"
#include "card_sync_selection.h"
#include "card_sync_network.h"
#include <array>
#include <memory>
#include <stop_token>
#include <atomic>
#include "card_sync_schedule.h"

namespace tcard_sync {
struct Credentials {
    std::wstring space;
    std::array<BYTE,32> transportKey{};
};
struct SessionReport;
// Optional worker-owned fresh saved-state provider, called only after private-possession authentication.
struct PeerSource {
    virtual ~PeerSource()=default;
    virtual std::wstring Capture(const std::wstring& request,const std::wstring& receiver,std::stop_token stop)=0;
};
// Optional discovery travels only inside authenticated encrypted replica payloads.
struct PeerDiscovery {
    virtual ~PeerDiscovery()=default;
    virtual std::wstring Offer(uint32_t remote)=0;
    virtual void Accept(const std::wstring& offer,uint32_t remote)=0;
    virtual void Exchanged(const tcard_net::Peer&,const SessionReport&){}
};
// Optional local diagnostic observer; it never supplies authentication or peer candidates.
struct PeerEvents {
    virtual ~PeerEvents()=default;
    virtual void Record(const tcard_net::Peer& peer,const wchar_t* outcome,bool inbound,const wchar_t* stage)=0;
};
struct ApplicationReport {
    bool known=false;size_t pending=0;std::wstring node;
};
// Optional evidence is protected by the existing fresh authenticated v2 payload.
struct PeerApplication {
    virtual ~PeerApplication()=default;
    virtual std::wstring Offer(const selection::Inventory& advertised)=0;
    virtual ApplicationReport Observe(const std::wstring& notice,const selection::Inventory& advertised,const selection::Inventory& required)=0;
};
struct SessionReport {
    Result received;
    size_t peerPending=0;
    bool connected=false,authenticated=false,exchanged=false;
    unsigned protocol=0;
    bool payloadEncrypted=false;
    bool converged=false;
    bool peerApplicationKnown=false;size_t peerApplicationPending=0;std::wstring peerNode;
    size_t missingLocal=0,missingPeer=0;
    bool objects=false; // Negotiated raw-object custody; committed/owner evidence remains separate.
    bool chunked=false;size_t sentChunks=0,receivedChunks=0,reusedChunks=0,sentChunkBytes=0,receivedChunkBytes=0;
    bool selected=false; // Fresh authenticated inventory selection; unchanged replicas need no content phase.
    size_t sentOperations=0,receivedOperations=0,sentAssets=0,receivedAssets=0;
    size_t sentContentBytes=0,receivedContentBytes=0; // UTF-8 delta/package bytes, excluding headers/TLS overhead.
    std::wstring publicAddress;
    std::wstring error;
};
struct PeerStatus { SessionReport session;bool live=false,dead=false;uint64_t checked=0,serial=0; };
// Shared bounded socket ownership; this object never accesses live native bundles.
class PeerConnections {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    struct Lease {uint64_t id=0;std::shared_ptr<std::atomic_bool> cancelled;};
    explicit PeerConnections(ConnectionTiming timing={});~PeerConnections();
    const ConnectionTiming& Timing()const;
    void SetPort(unsigned short port);unsigned short Port()const;
    uint64_t Request();uint64_t Requested()const;
    void Changed();uint64_t Revision()const;
    Lease Admit(const std::wstring& local,const std::wstring& remote,const tcard_net::Peer& peer,bool inbound);
    void Release(uint64_t lease);
    void Publish(const tcard_net::Peer& peer,const SessionReport& report,uint64_t checked,bool live,bool dead=false);
    PeerStatus Read(const tcard_net::Peer& peer)const;
    bool Active(const tcard_net::Peer& peer)const;
    bool Healthy()const;
};
class PeerConnector {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    PeerConnector(const std::wstring& path,const Credentials& credentials,const tcard_net::Peer& peer,
        const tcard_net::Policy& policy,std::shared_ptr<PeerConnections> connections,
        std::shared_ptr<PeerDiscovery> discovery={},std::shared_ptr<PeerApplication> application={},std::shared_ptr<PeerEvents> events={});
    ~PeerConnector();void Stop();
};
bool CreateCredentials(Credentials& credentials,std::wstring& error);
bool PersistCredentials(const std::wstring& storagePath,const Credentials& credentials,std::wstring& error);
bool ReadCredentials(const std::wstring& storagePath,Credentials& credentials,std::wstring& error);

// Explicit opt-in IPv4 loopback service. Construction does not change live UI settings.
class LoopbackListener {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit LoopbackListener(std::unique_ptr<Impl> impl);
    friend std::unique_ptr<LoopbackListener> ListenLan(const std::wstring&,const Credentials&,unsigned short,const tcard_net::Policy&,std::wstring&,std::shared_ptr<PeerDiscovery>,std::shared_ptr<PeerApplication>,std::shared_ptr<PeerEvents>,std::shared_ptr<PeerConnections>,std::shared_ptr<PeerSource>);
    friend std::unique_ptr<LoopbackListener> ListenLoopback(const std::wstring&,const Credentials&,unsigned short,std::wstring&,std::shared_ptr<PeerApplication>,std::shared_ptr<PeerEvents>,std::shared_ptr<PeerConnections>,std::shared_ptr<PeerSource>);
public:
    ~LoopbackListener();
    unsigned short Port() const;
    size_t AuthenticatedCount() const;
    void Stop();
};
std::unique_ptr<LoopbackListener> ListenLoopback(const std::wstring& storagePath,
    const Credentials& credentials,unsigned short port,std::wstring& error,std::shared_ptr<PeerApplication> application={},std::shared_ptr<PeerEvents> events={},std::shared_ptr<PeerConnections> connections={},std::shared_ptr<PeerSource> source={});
// Addresses require explicit allow; off-link additionally requires remote admission. Deny always wins.
bool DiscoverLan(tcard_net::Policy& policy,std::wstring& defaultWhitelist,std::wstring& error,std::vector<std::wstring>* addresses=nullptr);
std::unique_ptr<LoopbackListener> ListenLan(const std::wstring& storagePath,
    const Credentials& credentials,unsigned short port,const tcard_net::Policy& policy,std::wstring& error,std::shared_ptr<PeerDiscovery> discovery={},std::shared_ptr<PeerApplication> application={},std::shared_ptr<PeerEvents> events={},std::shared_ptr<PeerConnections> connections={},std::shared_ptr<PeerSource> source={});
bool ExchangeLan(const std::wstring& storagePath,const Credentials& credentials,
    const tcard_net::Peer& peer,const tcard_net::Policy& policy,SessionReport& report,std::stop_token stop={},PeerDiscovery* discovery=nullptr,PeerApplication* application=nullptr,PeerEvents* events=nullptr);
bool ExchangeLoopback(const std::wstring& storagePath,const Credentials& credentials,
    unsigned short port,SessionReport& report);
}
