#include <winsock2.h>
#include "../core/card_sync_transport.h"
#include "../core/card_key_activation.h"
#include "../core/card_sync_store.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include "../core/card_sync_bridge.h"
#include "../core/card_sync_ack.h"
#include "../core/card_source_owner.h"
#include "../core/card_source_job.h"
#include "../core/card_source_candidate.h"
#include "../core/card_sync_source.h"
#include "card_source_service.h"
#include "../core/card_sync_delivery.h"
#include "../core/card_sync_mail.h"
#include "../core/card_sync_handoff.h"
#include "../core/card_sync_words.h"
#include "../core/card_peer_discovery.h"
#include "../core/card_connection_log.h"
#include "../core/card_signal_client.h"
#include "compat/card_sync_seed.h"
#include "compat/card_sync_capture.h"

namespace {
using namespace tcard_sync;
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE handle):value(handle){}
    ~Handle(){if(value)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
struct Keys {
    Credentials value;
    ~Keys(){SecureZeroMemory(value.transportKey.data(),value.transportKey.size());}
};
std::wstring sync_absolute(const std::wstring& path)
{
    wchar_t absolute[32768]{};const auto n=GetFullPathNameW(path.c_str(),32768,absolute,nullptr);
    if(!n||n>=32768)throw std::runtime_error("Invalid path");std::wstring result=absolute;
    while(result.size()>3&&result.back()==L'\\')result.pop_back();return result;
}
std::wstring sync_event(const std::wstring& profile,const wchar_t* kind)
{
    auto normalized=profile;for(auto& c:normalized)c=static_cast<wchar_t>(towlower(c));
    return std::wstring(L"Local\\TCardSync.")+kind+L"."+sync_hash(normalized);
}
std::wstring sync_storage(const std::wstring& profile)
{
    return tcard_bridge::Storage(profile);
}
void sync_profile(const std::wstring& profile,bool create)
{
    tcard_asset::File root(tcard_asset::open_dir(profile,create));if(!root)throw std::runtime_error("Profile directory unavailable");
    JsonObject marker;
    if(sync_exists(profile,L"sync-profile.json")){
        if(create)throw std::runtime_error("Profile already initialized");
        if(!sync_read(profile,L"sync-profile.json",marker)||marker.GetNamedNumber(L"Version")!=1||sync_string(marker,L"Role")!=L"TCardSync.Replica")
            throw std::runtime_error("Unsupported sync profile");return;
    }
    if(!create)throw std::runtime_error("Initialize or join a dedicated sync profile first");
    WIN32_FIND_DATAW entry{};HANDLE search=FindFirstFileW((profile+L"\\*").c_str(),&entry);
    if(search!=INVALID_HANDLE_VALUE){
        bool empty=true;do{if(wcscmp(entry.cFileName,L".")&&wcscmp(entry.cFileName,L"..")){empty=false;break;}}while(FindNextFileW(search,&entry));FindClose(search);
        if(!empty)throw std::runtime_error("New sync profile must be empty; do not use a TCard data directory");
    }
}
void sync_mark(const std::wstring& profile)
{
    JsonObject marker;marker.Insert(L"Version",JsonValue::CreateNumberValue(1));marker.Insert(L"Role",JsonValue::CreateStringValue(L"TCardSync.Replica"));
    if(!sync_write(profile,L"sync-profile.json",marker))throw std::runtime_error("Cannot commit sync profile");
}
void sync_read_package(const std::wstring& path,std::wstring& package)
{
    const auto at=path.find_last_of(L'\\');if(at==std::wstring::npos)throw std::runtime_error("Invalid package path");
    std::vector<BYTE> bytes;if(!tcard_asset::read(path.substr(0,at),path.substr(at+1),bytes))throw std::runtime_error("Cannot read exchange file");
    package=tcard_asset::decode(bytes);if(tcard_asset::encode(package)!=bytes)throw std::runtime_error("Invalid exchange UTF-8");
}
void sync_write_package(const std::wstring& path,const std::wstring& package)
{
    const auto at=path.find_last_of(L'\\');if(at==std::wstring::npos||!tcard_asset::publish(path.substr(0,at),path.substr(at+1),tcard_asset::encode(package),false))
        throw std::runtime_error("Cannot publish exchange file");
}
void sync_report(const Result& result)
{
    JsonObject report;report.Insert(L"Applied",JsonValue::CreateNumberValue(static_cast<double>(result.applied)));
    report.Insert(L"Pending",JsonValue::CreateNumberValue(static_cast<double>(result.pending)));
    std::wcout<<report.Stringify().c_str()<<L"\n";
}
#include "card_replacement_service.h"
#include "card_sync_keys.h"
#include "card_source_completion.h"
#include "card_source_job_service.h"

int sync_worker(const std::wstring& profile,const Credentials& keys,unsigned short listenPort,DWORD parentPid,bool server,const std::vector<tcard_net::Peer>& peers,const tcard_net::Policy& policy,bool lan,bool sharing,const std::vector<std::wstring>& localAddresses,const std::wstring& dependencyGroup,const tcard_signal_worker::Config& signaling)
{
    if(!parentPid||!listenPort)throw std::runtime_error("Invalid UI worker configuration");
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));if(!parent.value)throw std::runtime_error("UI owner unavailable");
    const auto applicationOwner=tcard_ack::Owner(parent.value,parentPid),workerOwner=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId());
    const auto dependencyNode=dependencyGroup.empty()?L"":tcard_bootstrap::Node(sync_storage(profile));
    if(!dependencyGroup.empty()&&dependencyGroup!=tcard_chunks::Group(sync_storage(profile)))throw std::runtime_error("Dependency worker key association differs");
    Handle owner(CreateMutexW(nullptr,TRUE,sync_event(profile,L"Owner").c_str()));
    if(!owner.value||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Profile already has a worker");
    struct Release{HANDLE value;~Release(){ReleaseMutex(value);}} release{owner.value};
    const auto mailbox=tcard_bridge::Mailbox(profile);if(!tcard_bridge::Protect(mailbox))throw std::runtime_error("Private UI mailbox unavailable");
    Handle stop(CreateEventW(nullptr,TRUE,FALSE,(L"Local\\TCardSync.UiStop."+std::to_wstring(GetCurrentProcessId())).c_str()));
    Handle normalStop(CreateEventW(nullptr,FALSE,FALSE,sync_event(profile,L"Stop").c_str()));
    if(!stop.value||!normalStop.value)throw std::runtime_error("Cannot create UI stop events");
    auto connections=std::make_shared<PeerConnections>();
    auto discovery=std::make_shared<tcard_peer::Discovery>(profile,policy,sharing,server?listenPort:static_cast<unsigned short>(0),localAddresses,signaling.enabled,[connections]{connections->Changed();});
    std::shared_ptr<tcard_delivery::Journal> application;
    try{application=std::make_shared<tcard_delivery::Journal>(sync_storage(profile),mailbox,applicationOwner);}catch(...){/* ACK failure must preserve ordinary encrypted synchronization. */}
    auto events=std::make_shared<tcard_log::Log>(profile);
    std::shared_ptr<PeerSource> source;
    if(!dependencyGroup.empty())source=std::make_shared<tcard_source_service::Service>(mailbox,sync_storage(profile),applicationOwner,workerOwner,dependencyGroup,dependencyNode,parent.value);
    std::wstring error;std::unique_ptr<LoopbackListener> listener;
    if(server)listener=lan?ListenLan(sync_storage(profile),keys,listenPort,policy,error,discovery,application,events,connections,source):ListenLoopback(sync_storage(profile),keys,listenPort,error,application,events,connections,source);
    HANDLE waits[]={stop.value,normalStop.value,parent.value};std::wstring accepted,observed,published;
    std::wstring reported;size_t reportedPending=0,ownerPending=0,reportedOwnerPending=0;
    bool ownerApplied=false,reportedOwnerApplied=false;ULONGLONG reportedAt=0; tcard_bridge::Stamp reportedStamp;
    std::atomic<unsigned> signalPhase=signaling.enabled?1u:0u;std::atomic<bool> signalRegistered=false;
    unsigned reportedSignalPhase=0;bool reportedSignalRegistered=false;
    auto report=[&](const wchar_t* phase,size_t pending){
        const auto now=GetTickCount64();const auto signalState=signalPhase.load();const auto registered=signalRegistered.load();tcard_bridge::Stamp stamp;
        if(reportedSignalPhase==signalState&&reportedSignalRegistered==registered&&reported==phase&&reportedPending==pending&&reportedOwnerPending==ownerPending&&reportedOwnerApplied==ownerApplied&&
            reportedAt&&now-reportedAt<60000&&tcard_bridge::Fingerprint(mailbox,L"status.json",stamp)&&stamp==reportedStamp)return true;
        JsonObject status;status.Insert(L"Pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));
        status.Insert(L"Port",JsonValue::CreateNumberValue(listener?listener->Port():0));status.Insert(L"Status",JsonValue::CreateStringValue(phase));
        status.Insert(L"Pending",JsonValue::CreateNumberValue(static_cast<double>(pending)));
        status.Insert(L"MailboxBatches",JsonValue::CreateBooleanValue(tcard_storage::Required(mailbox)&&tcard_storage::Required(sync_storage(profile))));
        status.Insert(L"MailboxObjects",JsonValue::CreateBooleanValue(tcard_storage::Required(mailbox)&&tcard_storage::Required(sync_storage(profile))));
        status.Insert(L"MailboxSourceTarget",JsonValue::CreateBooleanValue(tcard_storage::Required(mailbox)&&tcard_storage::Required(sync_storage(profile))));
        status.Insert(L"MailboxDependencies",JsonValue::CreateBooleanValue(!dependencyGroup.empty()));
        status.Insert(L"MailboxOwnerSource",JsonValue::CreateBooleanValue(!dependencyGroup.empty()));
        status.Insert(L"DependencySpace",JsonValue::CreateStringValue(dependencyGroup.empty()?L"":keys.space));
        status.Insert(L"DependencyGroup",JsonValue::CreateStringValue(dependencyGroup));status.Insert(L"DependencyNode",JsonValue::CreateStringValue(dependencyNode));
        status.Insert(L"DependencyWorker",JsonValue::CreateStringValue(workerOwner));status.Insert(L"DependencyOwner",JsonValue::CreateStringValue(applicationOwner));
        status.Insert(L"SignalingEnabled",JsonValue::CreateBooleanValue(signaling.enabled));status.Insert(L"SignalingReady",JsonValue::CreateBooleanValue(signalState==2));
        status.Insert(L"SignalingRegistered",JsonValue::CreateBooleanValue(registered));status.Insert(L"SignalingPhase",JsonValue::CreateNumberValue(signalState));
        status.Insert(L"OwnerApplied",JsonValue::CreateBooleanValue(ownerApplied));
        status.Insert(L"OwnerPending",JsonValue::CreateNumberValue(static_cast<double>(ownerPending)));
        // A transient external reader can deny rename; retain the worker and retry on the next poll.
        if(!tcard_bridge::Publish(mailbox,L"status.json",status.Stringify().c_str()))return false;
        reported=phase;reportedPending=pending;reportedOwnerPending=ownerPending;reportedOwnerApplied=ownerApplied;reportedAt=now;reportedSignalPhase=signalState;reportedSignalRegistered=registered;tcard_bridge::Fingerprint(mailbox,L"status.json",reportedStamp);return true;
    };
    if(server&&!listener){
        for(;;){
            report(error==L"Cannot bind configured listener port"?L"sync.port_busy":L"sync.failed",0);
            const auto wake=WaitForMultipleObjects(3,waits,FALSE,300);
            if(wake==WAIT_FAILED)throw std::runtime_error("UI owner wait failed");if(wake!=WAIT_TIMEOUT)return 1;
        }
    }
    // Only an enabled, selected new-group worker retries a retained source completion.
    // Current policy and fresh authentication apply; OFF/no-worker startup runs no thread.
    std::jthread sourceCompletion([&](std::stop_token cancel){
        try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            while(!cancel.stop_requested()){
                try{const auto job=tcard_source_job::Pending(profile);if(!job.empty()&&!tcard_native::Exists(tcard_source_job::Control(profile)+L"\\"+job+L".retired")){
                    tcard_source_job::Snapshot source;if(tcard_source_job::Inspect(profile,job,source)&&source.ready&&source.group==tcard_chunks::Group(profile))sync_complete_source(profile,job,keys,policy,cancel);
                }}catch(...){/* Preserve protected pending evidence; no network fallback or key repair. */}
                for(unsigned i=0;i<600&&!cancel.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }catch(...){/* Completion is optional to local editing and ordinary synchronization. */}
    });
    // Node TLS has its own owned thread so broker outage cannot stall ordinary manual synchronization.
    std::jthread signalingThread;
    if(signaling.enabled)signalingThread=std::jthread([&](std::stop_token cancel){
        try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            tcard_signal_worker::Client client(profile,keys,signaling,policy,listener!=nullptr);
            while(!cancel.stop_requested()){bool registered=false;size_t accepted=0;bool okay=false;
                try{okay=client.Step(*discovery,cancel,registered,accepted,connections->Healthy());}catch(...){/* Keep optional discovery failure separate from local edits and sync delivery. */}
                signalRegistered=registered;signalPhase=okay?2u:3u;
                for(unsigned i=0;i<20&&!cancel.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }catch(...){signalPhase=3;signalRegistered=false;}
    });
    std::atomic<bool> peerFailed=false;std::atomic<size_t> peerPending=0;
    std::mutex checkMutex;std::wstring requested,finished;
    std::jthread exchanges;
    exchanges=std::jthread([&](std::stop_token cancel){
        try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            struct Link {tcard_net::Peer peer;bool manual=false;std::unique_ptr<PeerConnector> connector;};
            std::map<std::wstring,Link> links;uint64_t ticket=0;ULONGLONG checkAt=0,candidatesAt=0;std::wstring active;
            const auto add=[&](const tcard_net::Peer& peer,bool manual){
                const auto id=peer.ip+L":"+std::to_wstring(peer.port);if(links.contains(id)||links.size()>=64)return;
                auto connector=std::make_unique<PeerConnector>(sync_storage(profile),keys,peer,policy,connections,discovery,application,events);
                links.emplace(id,Link{peer,manual,std::move(connector)});
            };
            for(const auto& peer:peers)add(peer,true);
            while(!cancel.stop_requested()){
                std::wstring token;{std::lock_guard lock(checkMutex);if(requested!=finished)token=requested;}
                if(GetTickCount64()>=candidatesAt){
                    for(auto i=links.begin();i!=links.end();)if(!i->second.manual&&!discovery->Retained(i->second.peer)){i->second.connector->Stop();i=links.erase(i);}else ++i;
                    std::vector<tcard_net::Peer> known;for(const auto& row:links)known.push_back(row.second.peer);
                    for(const auto& peer:discovery->Candidates(known))add(peer,false);candidatesAt=GetTickCount64()+1000;
                }
                if(!token.empty()&&token!=active){active=token;ticket=connections->Request();checkAt=GetTickCount64();}
                bool failed=false,checkedAll=true;size_t pending=0;
                for(const auto& row:links){const auto state=connections->Read(row.second.peer);
                    if(row.second.manual&&!state.session.error.empty()&&!state.live)failed=true;
                    pending+=state.session.received.pending+state.session.peerPending;
                    if(state.checked<ticket)checkedAll=false;
                }
                if(!token.empty()&&!cancel.stop_requested()&&(checkedAll||GetTickCount64()-checkAt>=120000)){
                    JsonArray checked;
                    for(const auto& row:links){const auto& peer=row.second.peer;const auto state=connections->Read(peer);const auto& session=state.session;
                        JsonObject item;item.Insert(L"Peer",JsonValue::CreateStringValue(peer.ip+L":"+std::to_wstring(peer.port)));
                        item.Insert(L"Connected",JsonValue::CreateBooleanValue(session.connected));item.Insert(L"Authenticated",JsonValue::CreateBooleanValue(session.authenticated));
                        item.Insert(L"Exchanged",JsonValue::CreateBooleanValue(session.exchanged));item.Insert(L"Pending",JsonValue::CreateNumberValue(static_cast<double>(session.received.pending+session.peerPending)));
                        item.Insert(L"Protocol",JsonValue::CreateNumberValue(session.protocol));item.Insert(L"PayloadEncrypted",JsonValue::CreateBooleanValue(session.payloadEncrypted));
                        item.Insert(L"PublicAddress",JsonValue::CreateStringValue(session.publicAddress));
                        item.Insert(L"Selected",JsonValue::CreateBooleanValue(session.selected));
                        item.Insert(L"SentOperations",JsonValue::CreateNumberValue(static_cast<double>(session.sentOperations)));
                        item.Insert(L"ReceivedOperations",JsonValue::CreateNumberValue(static_cast<double>(session.receivedOperations)));
                        item.Insert(L"SentAssets",JsonValue::CreateNumberValue(static_cast<double>(session.sentAssets)));
                        item.Insert(L"ReceivedAssets",JsonValue::CreateNumberValue(static_cast<double>(session.receivedAssets)));
                        item.Insert(L"SentContentBytes",JsonValue::CreateNumberValue(static_cast<double>(session.sentContentBytes)));
                        item.Insert(L"ReceivedContentBytes",JsonValue::CreateNumberValue(static_cast<double>(session.receivedContentBytes)));
                        item.Insert(L"Chunked",JsonValue::CreateBooleanValue(session.chunked));
                        item.Insert(L"SentChunks",JsonValue::CreateNumberValue(static_cast<double>(session.sentChunks)));
                        item.Insert(L"ReceivedChunks",JsonValue::CreateNumberValue(static_cast<double>(session.receivedChunks)));
                        item.Insert(L"ReusedChunks",JsonValue::CreateNumberValue(static_cast<double>(session.reusedChunks)));
                        item.Insert(L"SentChunkBytes",JsonValue::CreateNumberValue(static_cast<double>(session.sentChunkBytes)));
                        item.Insert(L"ReceivedChunkBytes",JsonValue::CreateNumberValue(static_cast<double>(session.receivedChunkBytes)));
                        item.Insert(L"Converged",JsonValue::CreateBooleanValue(session.converged));
                        item.Insert(L"PeerNode",JsonValue::CreateStringValue(session.peerNode));
                        item.Insert(L"PeerApplicationKnown",JsonValue::CreateBooleanValue(session.peerApplicationKnown));
                        item.Insert(L"PeerApplicationPending",JsonValue::CreateNumberValue(static_cast<double>(session.peerApplicationPending)));
                        item.Insert(L"MissingLocal",JsonValue::CreateNumberValue(static_cast<double>(session.missingLocal)));
                        item.Insert(L"MissingPeer",JsonValue::CreateNumberValue(static_cast<double>(session.missingPeer)));
                        item.Insert(L"Error",JsonValue::CreateStringValue(session.error));item.Insert(L"Persistent",JsonValue::CreateBooleanValue(state.live));item.Insert(L"Dead",JsonValue::CreateBooleanValue(state.dead));checked.Append(item);
                    }
                    JsonObject result;result.Insert(L"Pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));result.Insert(L"Request",JsonValue::CreateStringValue(token));
                    result.Insert(L"Space",JsonValue::CreateStringValue(keys.space));result.Insert(L"Listening",JsonValue::CreateBooleanValue(listener!=nullptr));
                    result.Insert(L"AuthenticatedSessions",JsonValue::CreateNumberValue(listener?static_cast<double>(listener->AuthenticatedCount()):0));result.Insert(L"Peers",checked);
                    if(tcard_bridge::Publish(mailbox,L"check-result.json",result.Stringify().c_str())){std::lock_guard lock(checkMutex);finished=token;}
                }
                try{events->Flush();discovery->Flush();}catch(...){/* Optional metadata retries without changing committed content. */}
                peerFailed=failed;peerPending=pending;
                for(unsigned i=0;i<5&&!cancel.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            for(auto& row:links)row.second.connector->Stop();

        }catch(...){peerFailed=true;}
    });
    const auto replica=sync_storage(profile);
    struct Watch {HANDLE value=INVALID_HANDLE_VALUE;~Watch(){if(value!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(value);}} watch;
    ULONGLONG auditedAt=0;std::wstring cachedIncoming,cachedHash;bool exportNeeded=true;
    tcard_bridge::Stamp incomingStamp,outgoingStamp;ULONGLONG incomingAuditAt=0,outgoingAt=0;std::wstring cachedOutgoing,outgoingHash;
    selection::Inventory expectedOwner,appliedOwner,outgoingRequired; tcard_bridge::Stamp appliedStamp;ULONGLONG appliedAt=0;
    bool ownerBatches=false,ownerObjects=false,ownerAtomic=false,ownerDependencies=false,incomingBatches=false;std::wstring ownerNode;tcard_mail::Producer incomingBatch;tcard_handoff::Sender atomicSender;tcard_remote::Inbox remoteInbox;
    tcard_bridge::Stamp committedOutgoingStamp;ULONGLONG committedOutgoingAt=0;
    tcard_dependencies::Recovery dependencyRecovery;bool dependencyProgress=true;
    tcard_source_owner::Provider sourceOwner(mailbox,replica,applicationOwner,workerOwner,dependencyGroup,dependencyNode,parent.value);
    report(L"sync.ready",0);ULONGLONG retryAt=0,purgeAt=GetTickCount64()+60000;Result recovered;bool resumed=true;
    for(;;){
        const auto wake=WaitForMultipleObjects(3,waits,FALSE,300);
        if(wake==WAIT_FAILED)throw std::runtime_error("UI owner wait failed");
        if(wake!=WAIT_TIMEOUT)break;
        if(!dependencyGroup.empty())sourceOwner.Step();
        if(GetTickCount64()>=retryAt){resumed=RetryChanges(replica,recovered);retryAt=GetTickCount64()+(resumed&&!recovered.pending?60000:3000);if(recovered.applied){exportNeeded=true;dependencyProgress=true;}}
        if(!dependencyGroup.empty())try{
            tcard_dependencies::Pool::Recover(replica,keys.space,dependencyGroup,dependencyNode,dependencyRecovery,dependencyProgress);dependencyProgress=false;
        }catch(...){dependencyProgress=false;/* Preserve unresolved custody; normal transfer admission still validates the quota. */}
        // Maintain only this worker's dedicated replica. Never touch live editor bundles here.
        if(GetTickCount64()>=purgeAt){
            const bool purged=tcard::PurgeTrash(sync_storage(profile));purgeAt=GetTickCount64()+(purged?3600000:60000);
        }
        const wchar_t* phase=!resumed?L"sync.failed":peerFailed?L"sync.peer_failed":L"sync.ready";
        size_t pending=peerPending+recovered.pending;std::wstring readOutgoing;
        tcard_bridge::Stamp nextOutgoing;const auto pollAt=GetTickCount64();
        const bool present=tcard_bridge::Fingerprint(mailbox,L"outgoing.json",nextOutgoing);
        if(present&&(!outgoingAt||!(nextOutgoing==outgoingStamp)||pollAt-outgoingAt>=60000)){
            if(tcard_bridge::Read(mailbox,L"outgoing.json",readOutgoing)){cachedOutgoing=std::move(readOutgoing);outgoingHash=tcard_bridge::Hash(cachedOutgoing);outgoingAt=pollAt;outgoingStamp=nextOutgoing;}
            else{outgoingAt=0;cachedOutgoing.clear();outgoingHash.clear();}
        }
        if(present&&outgoingAt){
            const auto& outgoing=cachedOutgoing;const auto& hash=outgoingHash;
            tcard_bridge::Stamp commitStamp;const bool batchPacket=tcard_mail::Batched(outgoing);
            const bool repair=batchPacket&&(!tcard_bridge::Fingerprint(mailbox,L"outgoing-commit.json",commitStamp)||
                !(commitStamp==committedOutgoingStamp)||pollAt-committedOutgoingAt>=60000);
            if(hash!=accepted||repair){Result result;
                std::wstring delta=outgoing;selection::Inventory required;const bool batch=tcard_mail::Batched(outgoing);bool sourceAtomic=false,dependency=false;tcard_dependencies::Context expected;
                if(batch)try{
                    dependency=JsonObject::Parse(outgoing).GetNamedNumber(L"BatchVersion")==3;
                    if(dependency){
                        selection::Inventory native;bool admitted=false;std::wstring nativeNode;
                        if(dependencyGroup.empty()||!tcard_ack::Read(mailbox,applicationOwner,native,nullptr,nullptr,nullptr,&admitted,&nativeNode)||!admitted)
                            throw std::runtime_error("Dependency native owner capability absent");
                        expected=tcard_dependency_mail::Expected(outgoing,{keys.space,dependencyGroup,nativeNode,dependencyNode,L""},true);
                        required=tcard_mail::Decode(outgoing,tcard_mail::Group(replica,mailbox),delta,true,&expected);sourceAtomic=tcard_remote::NativeDependency(replica,mailbox,outgoing,expected);
                    }else{required=tcard_mail::Decode(outgoing,tcard_mail::Group(replica,mailbox),delta,true);sourceAtomic=tcard_remote::Native(replica,mailbox,outgoing);}
                    selection::Include(outgoingRequired,selection::Read(std::wstring(JsonObject::Parse(outgoing).GetNamedObject(L"Inventory").Stringify())));
                }catch(...){report(L"sync.failed",pending);continue;}
                if(!(dependency?tcard_dependency_mail::Import(replica,mailbox,L"outgoing.json",{tcard_dependencies::Read(delta,expected).note,delta},expected,result):tcard_objects::Referenced(delta)?tcard_blobs::Import(replica,tcard_mail::Objects(mailbox,L"outgoing.json"),delta,result):ImportChanges(replica,delta,result))&&!result.partial){report(L"sync.failed",0);continue;}
                if(!result.pending&&!result.partial&&!sourceAtomic)try{if(dependency)tcard_remote::AdmitDependency(replica,delta,expected);else tcard_remote::Admit(replica,delta);}catch(...){report(L"sync.failed",pending);continue;}
                pending+=result.pending;observed=hash;
                if(!result.pending&&!result.partial){
                    if(dependency){Result proof;const PreparedNote note{tcard_dependencies::Read(delta,expected).note,delta};
                        if(!tcard_dependency_mail::Cleanup(replica,mailbox,L"outgoing.json",note,expected,proof)){report(L"sync.failed",pending);continue;}}
                    std::wstring header;if(batch&&!tcard_mail::Commit(replica,mailbox,L"outgoing-commit.json",hash,required,header)){report(L"sync.failed",pending);continue;}
                    if(accepted!=hash){dependencyProgress=true;connections->Changed();}accepted=hash;if(batch){committedOutgoingAt=GetTickCount64();tcard_bridge::Fingerprint(mailbox,L"outgoing-commit.json",committedOutgoingStamp);}
                }exportNeeded=true;
            }
        }
        std::wstring check;
        if(tcard_bridge::Read(mailbox,L"check-request.json",check)){
            JsonObject request;
            if(JsonObject::TryParse(check,request)&&request.GetNamedNumber(L"Pid",0)==GetCurrentProcessId()&&sync_string(request,L"OutgoingHash")==observed){
                const auto token=sync_string(request,L"Request");if(token.size()==36){std::lock_guard lock(checkMutex);requested=token;}
            }
        }
        // Journal notifications include peer commits, local outgoing imports and restore/delete operations.
        // Watch only the journal: nested IPC/status publications must not invalidate native data caches.
        if(watch.value==INVALID_HANDLE_VALUE){
            watch.value=FindFirstChangeNotificationW((replica+L"\\_sync").c_str(),TRUE,FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE);
            exportNeeded=true;
        }else if(WaitForSingleObject(watch.value,0)!=WAIT_TIMEOUT){
            exportNeeded=true;dependencyProgress=true;connections->Changed();if(!FindNextChangeNotification(watch.value)){FindCloseChangeNotification(watch.value);watch.value=INVALID_HANDLE_VALUE;}
        }
        const auto now=GetTickCount64();
        // Only the owner may publish application evidence; private-replica receipt is insufficient.
        tcard_bridge::Stamp applied;
        if(!tcard_bridge::Fingerprint(mailbox,L"applied.json",applied)){ownerApplied=false;ownerBatches=false;ownerObjects=false;ownerAtomic=false;ownerDependencies=false;ownerNode.clear();appliedOwner={};appliedAt=0;}
        else if(!appliedAt||!(applied==appliedStamp)||now-appliedAt>=60000){
            ownerApplied=tcard_ack::Read(mailbox,applicationOwner,appliedOwner,&ownerBatches,&ownerObjects,&ownerAtomic,&ownerDependencies,&ownerNode);if(appliedAt&&!(applied==appliedStamp))connections->Changed();appliedStamp=applied;appliedAt=ownerApplied?now:0;
            // Retain newer owner intent even while an older outgoing slot is pinned.
            if(ownerApplied)selection::Include(outgoingRequired,appliedOwner);
        }
        // Receive boundaries exist before network imports, so a partial remote replica never becomes a native snapshot.
        // Hold admission through the final mailbox publication; a new target cannot race an unguarded snapshot.
        std::lock_guard boundary(tcard_remote::receiveMutex);
        bool remoteChanged=false;std::wstring remoteTarget;ULONGLONG receiveRevision=0;
        try{
            remoteChanged=remoteInbox.Refresh(replica);if(ownerApplied)remoteChanged=remoteInbox.Retire(replica,appliedOwner)||remoteChanged;
            receiveRevision=remoteInbox.observed;if(remoteChanged)exportNeeded=true;
            if(!remoteInbox.targets.empty()){
                if(!ownerApplied||!ownerBatches||!ownerObjects||!ownerAtomic){report(L"sync.pending",pending+remoteInbox.targets.size());continue;}
                Result result;unsigned version=0;std::wstring committed;
                if(exportNeeded||now-auditedAt>=60000){if(!ExportCommittedInventory(replica,committed,result,version)){report(L"sync.failed",pending);continue;}expectedOwner=selection::Read(committed);auditedAt=now;}
                remoteTarget=remoteInbox.Ready(expectedOwner);incomingBatches=true;
                if(remoteTarget.empty()){exportNeeded=false;ownerPending=remoteInbox.targets.size();report(L"sync.pending",pending);continue;}
            }
        }catch(...){report(L"sync.failed",pending);continue;}
        if(incomingBatches){
            const tcard_dependencies::Context dependencyChannel{keys.space,dependencyGroup,dependencyNode,ownerNode,L""};
            Result result;const bool changed=exportNeeded||now-auditedAt>=60000;
            if(changed){
                std::wstring header;unsigned version=0;
                if(!ExportInventory(replica,header,result,version)){report(L"sync.failed",pending);continue;}
                expectedOwner=selection::Read(header);
            }
            if(changed||(ownerAtomic&&ownerObjects?atomicSender.Changed(mailbox,applicationOwner,appliedOwner):incomingBatch.Changed(mailbox,L"incoming.json",L"incoming-commit.json",applicationOwner))){
                if(!ownerApplied||!ownerBatches||!(ownerAtomic&&ownerObjects?tcard_handoff::Produce(replica,mailbox,applicationOwner,appliedOwner,changed,atomicSender,result,remoteTarget,receiveRevision,ownerDependencies&&!dependencyGroup.empty()?&dependencyChannel:nullptr):tcard_mail::Produce(replica,mailbox,L"incoming.json",L"incoming-commit.json",applicationOwner,false,changed,incomingBatch,result,ownerObjects))){
                    report(L"sync.failed",pending);continue;
                }
                cachedHash=ownerAtomic&&ownerObjects?atomicSender.hash:incomingBatch.hash;auditedAt=now;exportNeeded=false;
            }
        }else{
            if(exportNeeded||now-auditedAt>=60000){
                Result result;std::wstring incoming;
                if(!ExportChanges(replica,incoming,result)){
                    if(ownerApplied&&ownerBatches&&tcard_mail::Budget(result)){
                        incomingBatches=true;exportNeeded=true;continue;
                    }report(L"sync.failed",pending);continue;
                }
                expectedOwner=selection::Read(selection::Header(incoming));
                cachedHash=tcard_bridge::Hash(incoming);cachedIncoming=std::move(incoming);auditedAt=now;exportNeeded=false;
            }
            if(receiveRevision!=tcard_remote::revision.load()){exportNeeded=true;continue;}
            tcard_bridge::Stamp stamp;
            bool retained=tcard_bridge::Fingerprint(mailbox,L"incoming.json",stamp)&&stamp==incomingStamp;
            if(retained&&now-incomingAuditAt>=60000){std::wstring verified;retained=tcard_bridge::Read(mailbox,L"incoming.json",verified)&&tcard_bridge::Hash(verified)==cachedHash;incomingAuditAt=now;}
            if(cachedHash!=published||!retained){
                if(receiveRevision!=tcard_remote::revision.load()){exportNeeded=true;continue;}
                if(!tcard_bridge::Publish(mailbox,L"incoming.json",cachedIncoming)){report(L"sync.failed",pending);continue;}
                published=cachedHash;incomingAuditAt=now;tcard_bridge::Fingerprint(mailbox,L"incoming.json",incomingStamp);
            }
        }
        ownerPending=selection::Missing(expectedOwner,appliedOwner);pending+=selection::Missing(outgoingRequired,expectedOwner);
        if(pending&&wcscmp(phase,L"sync.ready")==0)phase=L"sync.pending";
        report(phase,pending);
    }
    sourceCompletion.request_stop();if(signalingThread.joinable())signalingThread.request_stop();if(exchanges.joinable())exchanges.request_stop();
    sourceCompletion.join();if(signalingThread.joinable())signalingThread.join();if(exchanges.joinable())exchanges.join();if(listener)listener->Stop();events->Flush();return 0;
}
int sync_run(int argc,wchar_t** argv)
{
    if(argc==1){std::wcout<<L"TCardSync --init|--join|--listen|--exchange|--import|--export|--stop|--ui|--network-info|--key-service|--source-service|--cipher-begin|--cipher-step|--cipher-activate --profile PATH [--port NUMBER] [--package PATH] [--from-profile PATH]\n";return 0;}
    const std::wstring command=argv[1];
    if(command!=L"--init"&&command!=L"--join"&&command!=L"--listen"&&command!=L"--exchange"&&command!=L"--import"&&command!=L"--export"&&command!=L"--stop"&&command!=L"--ui"&&command!=L"--network-info"&&command!=L"--key-service"&&command!=L"--source-service"&&command!=L"--cipher-begin"&&command!=L"--cipher-step"&&command!=L"--cipher-activate")
        throw std::runtime_error("Unknown sync command");
    std::map<std::wstring,std::wstring> options;
    for(int i=2;i<argc;i+=2){
        if(i+1>=argc)throw std::runtime_error("Missing option value");const std::wstring key=argv[i];
        if(key!=L"--profile"&&key!=L"--port"&&key!=L"--package"&&key!=L"--from-profile"&&key!=L"--listen-port"&&key!=L"--parent-pid"&&key!=L"--server"&&key!=L"--peers"&&key!=L"--whitelist"&&key!=L"--blacklist"&&key!=L"--request"&&key!=L"--storage-root"&&key!=L"--share-peers"&&key!=L"--remote-admission"&&key!=L"--signaling"&&key!=L"--signal-nodes"&&key!=L"--published-sync")throw std::runtime_error("Unknown sync option");
        if(!options.emplace(key,argv[i+1]).second)throw std::runtime_error("Duplicate sync option");
    }
    if(!options.contains(L"--profile")||options.at(L"--profile").empty())throw std::runtime_error("Dedicated profile path is required");
    const auto profile=sync_absolute(options.at(L"--profile"));
    if(command!=L"--key-service"&&command!=L"--stop"&&tcard_activation::Pending(profile))throw std::runtime_error("Key activation requires the local owner to resume");
    const auto storage=command==L"--key-service"||command==L"--source-service"||command==L"--stop"?L"":sync_storage(profile);
    if(command==L"--source-service"){
        if(options.size()!=2||!options.contains(L"--request"))throw std::runtime_error("Acquisition requires only profile and protected request");
        return sync_source_job(profile,options.at(L"--request"));
    }
    if(command==L"--cipher-begin"||command==L"--cipher-step"||command==L"--cipher-activate"){
        if(options.size()!=(options.contains(L"--from-profile")?2u:1u)||(options.contains(L"--from-profile")&&command!=L"--cipher-begin"))
            throw std::runtime_error("Cipher commands require only a dedicated profile and optional begin donor");
        sync_profile(profile,false);tcard_cipher::Status status;const auto cipher=profile+L"\\cipher-v2";
        if(command==L"--cipher-begin"){
            const auto donor=options.contains(L"--from-profile")?sync_absolute(options.at(L"--from-profile")):profile;sync_profile(donor,false);
            Keys donorKeys;std::wstring error;if(!ReadCredentials(sync_storage(donor),donorKeys.value,error))throw std::runtime_error("Candidate profile credentials unavailable");
            auto words=tcard_words::Encode(donorKeys.value);tcard_crypto::Seed seed{};
            struct Wipe{std::wstring& words;tcard_crypto::Seed& seed;~Wipe(){SecureZeroMemory(words.data(),words.size()*sizeof(wchar_t));SecureZeroMemory(seed.data(),seed.size());}} wipe{words,seed};
            if(!tcard_compat::DecodeSeed(words,seed))throw std::runtime_error("V2 requires explicit 21-word provisioning; 24-word conversion is not implicit");
            if(!tcard_cipher::Begin(cipher,seed,status))throw std::runtime_error(status.error);
        }
        if(!tcard_compat::AdvanceJournal(cipher,storage,command==L"--cipher-begin"?0u:16u,command==L"--cipher-activate",status))throw std::runtime_error(status.error);
        JsonObject report;report.Insert(L"Retained",JsonValue::CreateNumberValue(static_cast<double>(status.retained)));
        report.Insert(L"Remaining",JsonValue::CreateNumberValue(static_cast<double>(status.remaining)));report.Insert(L"Active",JsonValue::CreateBooleanValue(status.active));
        std::wcout<<report.Stringify().c_str()<<L"\n";return status.remaining?2:0;
    }
    if(command==L"--key-service"){
        if(!options.contains(L"--request")||options.size()>(options.contains(L"--from-profile")?3u:2u))throw std::runtime_error("Invalid key-service options");
        return sync_keys(profile,options.at(L"--request"),options.contains(L"--from-profile")?sync_absolute(options.at(L"--from-profile")):L"");
    }
    if(options.contains(L"--request"))throw std::runtime_error("Key request requires key-service");
    tcard_signal_worker::Config signaling;
    const bool signalOptions=options.contains(L"--signaling")||options.contains(L"--signal-nodes")||options.contains(L"--published-sync");
    if(signalOptions){
        if(command!=L"--ui"||!options.contains(L"--signaling"))throw std::runtime_error("Signaling options require UI worker mode");
        const auto& enabled=options.at(L"--signaling");if(enabled!=L"0"&&enabled!=L"1")throw std::runtime_error("Invalid signaling option");signaling.enabled=enabled==L"1";
        if(signaling.enabled){
            if(!options.contains(L"--signal-nodes")||!tcard_bridge::ParseSignalNodes(options.at(L"--signal-nodes"),signaling.nodes)||signaling.nodes.empty())throw std::runtime_error("Pinned signaling nodes required");
            if(options.contains(L"--published-sync")&&!tcard_bridge::ParsePublishedSyncEndpoint(options.at(L"--published-sync"),signaling.published))throw std::runtime_error("Invalid published synchronization endpoint");
        }else if(options.contains(L"--signal-nodes")||options.contains(L"--published-sync"))throw std::runtime_error("Inactive signaling options");
    }
    if(options.contains(L"--port")&&command!=L"--listen"&&command!=L"--exchange"&&command!=L"--ui")throw std::runtime_error("Port is only valid for listen/exchange");
    if(options.contains(L"--package")&&command!=L"--import"&&command!=L"--export")throw std::runtime_error("Package is only valid for import/export");
    if(options.contains(L"--from-profile")&&command!=L"--join"&&command!=L"--ui")throw std::runtime_error("Source profile is only valid for join");
    if((command==L"--import"||command==L"--export")&&!options.contains(L"--package"))throw std::runtime_error("Exchange file is required");
    if(command==L"--join"&&!options.contains(L"--from-profile"))throw std::runtime_error("Provisioning profile is required");
    if(command!=L"--ui"&&(options.contains(L"--listen-port")||options.contains(L"--parent-pid")))throw std::runtime_error("UI owner options require --ui");
    const bool lan=options.contains(L"--remote-admission")||options.contains(L"--share-peers")||options.contains(L"--server")||options.contains(L"--peers")||options.contains(L"--whitelist")||options.contains(L"--blacklist");
    if(lan&&command!=L"--ui")throw std::runtime_error("Network policy options require --ui");
    if(command==L"--network-info"){
        if(options.size()!=1)throw std::runtime_error("Network discovery requires only --profile output directory");
        if(!tcard_bridge::Protect(profile))throw std::runtime_error("Discovery output unavailable");
        tcard_net::Policy policy;std::wstring seed,error;if(!DiscoverLan(policy,seed,error))throw std::runtime_error("Cannot discover LAN subnet");
        JsonObject data;data.Insert(L"Pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));data.Insert(L"Whitelist",JsonValue::CreateStringValue(seed));
        if(!tcard_bridge::Publish(profile,L"defaults.json",data.Stringify().c_str()))throw std::runtime_error("Cannot publish LAN discovery");return 0;
    }
    unsigned short port=0;
    if(options.contains(L"--port")){
        const auto& text=options.at(L"--port");size_t end=0;const auto n=std::stoul(text,&end);
        if(n>65535||end!=text.size()||std::to_wstring(n)!=text)throw std::runtime_error("Invalid port");port=static_cast<unsigned short>(n);
    }
    if(command==L"--exchange"&&!port)throw std::runtime_error("Peer port is required");
    std::wstring error;
    unsigned short listenPort=0;DWORD parentPid=0;
    tcard_net::Policy policy{{{0x7f000000u,0xff000000u}},{},{{0x7f000000u,0xff000000u}}};std::vector<tcard_net::Peer> peers;bool server=true,sharing=false;std::vector<std::wstring> localAddresses;
    if(command==L"--ui"){
        if(!options.contains(L"--listen-port")||!options.contains(L"--parent-pid"))throw std::runtime_error("UI listener and owner are required");
        auto number=[&](const wchar_t* key,uint64_t maximum){const auto& text=options.at(key);size_t end=0;const auto n=std::stoull(text,&end);
            if(!n||n>maximum||end!=text.size()||std::to_wstring(n)!=text)throw std::runtime_error("Invalid UI numeric option");return n;};
        listenPort=static_cast<unsigned short>(number(L"--listen-port",65535));parentPid=static_cast<DWORD>(number(L"--parent-pid",MAXDWORD));
        if(lan){
            if(!options.contains(L"--server")||!options.contains(L"--peers")||!options.contains(L"--whitelist")||!options.contains(L"--blacklist")||
                (options.at(L"--server")!=L"0"&&options.at(L"--server")!=L"1"))throw std::runtime_error("Incomplete network policy");
            server=options.at(L"--server")==L"1";std::wstring seed;
            if(options.contains(L"--remote-admission")){const auto& value=options.at(L"--remote-admission");
                if(value!=L"0"&&value!=L"1")throw std::runtime_error("Invalid remote admission option");policy.remoteAdmission=value==L"1";}
            if(options.contains(L"--share-peers")){const auto& value=options.at(L"--share-peers");if(value!=L"0"&&value!=L"1")throw std::runtime_error("Invalid discovery option");sharing=value==L"1";}
            if(!DiscoverLan(policy,seed,error,&localAddresses)||!tcard_net::ParseRules(options.at(L"--whitelist"),policy.allow)||!tcard_net::ParseRules(options.at(L"--blacklist"),policy.deny)||
                !tcard_net::ParsePeers(options.at(L"--peers"),port?port:listenPort,peers))throw std::runtime_error("Invalid IP policy");
            for(const auto& peer:peers)if(server&&peer.port==listenPort&&
                ((peer.address>>24)==127||std::find(localAddresses.begin(),localAddresses.end(),peer.ip)!=localAddresses.end()))
                throw std::runtime_error("A target cannot be this worker's own listener");
        }else if(port==listenPort)throw std::runtime_error("A target cannot be this worker's own listener");
        else if(port)peers.push_back({L"127.0.0.1",port,0x7f000001u});
    }
    if(command==L"--ui"&&!tcard_bridge::Protect(profile))throw std::runtime_error("Private profile unavailable");
    const bool creating=command==L"--init"||command==L"--join"||(command==L"--ui"&&!sync_exists(profile,L"sync-profile.json"));
    sync_profile(profile,creating);
    Keys keys;
    if(creating){
        if(command==L"--init"||(command==L"--ui"&&!options.contains(L"--from-profile"))){
            if(command==L"--ui"?!tcard_words::Create(keys.value):!CreateCredentials(keys.value,error))throw std::runtime_error("Cannot create synchronization credentials");
        }else{
            const auto from=sync_absolute(options.at(L"--from-profile"));sync_profile(from,false);
            if(!ReadCredentials(sync_storage(from),keys.value,error))throw std::runtime_error("Cannot provision local profile credentials");
        }
        tcard_asset::File replica(tcard_asset::open_dir(storage,true));if(!replica)throw std::runtime_error("Cannot create private replica directory");
        if(!PersistCredentials(storage,keys.value,error))throw std::runtime_error("Cannot persist synchronization credentials");sync_mark(profile);if(command!=L"--ui")return 0;
    }
    if(command==L"--stop"){
        Handle event(OpenEventW(EVENT_MODIFY_STATE,FALSE,sync_event(profile,L"Stop").c_str()));
        if(!event.value)return 2;if(!SetEvent(event.value))throw std::runtime_error("Cannot request normal worker shutdown");return 0;
    }
    if(!ReadCredentials(storage,keys.value,error))throw std::runtime_error("Protected synchronization credentials unavailable");
    std::wstring dependencyGroup;
    if(options.contains(L"--storage-root")||command==L"--ui"){
        if(command!=L"--ui")throw std::runtime_error("Storage owner requires UI mode");
        auto words=tcard_words::Encode(keys.value);tcard_crypto::Seed seed{};tcard_crypto::Keys derived;
        struct Wipe{std::wstring& words;tcard_crypto::Seed& seed;~Wipe(){SecureZeroMemory(words.data(),words.size()*sizeof(wchar_t));SecureZeroMemory(seed.data(),seed.size());}} wipe{words,seed};
        Keys validated;
        if(!tcard_words::Decode(words,validated.value)||validated.value.space!=keys.value.space||!tcard_compat::DecodeSeed(words,seed)||!tcard_crypto::Derive(seed,derived))
            throw std::runtime_error("Dependency credential identity differs");
        if(options.contains(L"--storage-root")&&(!tcard_storage::Matches(profile,derived)||!tcard_storage::Matches(sync_absolute(options.at(L"--storage-root")),derived)))
            throw std::runtime_error("Encrypted storage admission failed");
        if(tcard_storage::Matches(storage,derived)&&tcard_storage::Matches(profile,derived))dependencyGroup=tcard_ack::Identity(derived);
    }
    if(command==L"--ui")return sync_worker(profile,keys.value,listenPort,parentPid,server,peers,policy,lan,sharing,localAddresses,dependencyGroup,signaling);
    if(command==L"--listen"){
        Handle owner(CreateMutexW(nullptr,TRUE,sync_event(profile,L"Owner").c_str()));
        if(!owner.value||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Sync worker already running or unavailable");
        struct Release{HANDLE value;~Release(){ReleaseMutex(value);}} release{owner.value};
        Handle stop(CreateEventW(nullptr,FALSE,FALSE,sync_event(profile,L"Stop").c_str()));if(!stop.value)throw std::runtime_error("Cannot create stop event");
        auto listener=ListenLoopback(storage,keys.value,port,error);if(!listener)throw std::runtime_error("Cannot start loopback worker");
        JsonObject ready;ready.Insert(L"Port",JsonValue::CreateNumberValue(listener->Port()));ready.Insert(L"Pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));
        std::wcout<<ready.Stringify().c_str()<<L"\n"<<std::flush;
        if(WaitForSingleObject(stop.value,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("Worker wait failed");
        listener->Stop();return 0;
    }
    if(command==L"--exchange"){
        SessionReport report;if(!ExchangeLoopback(storage,keys.value,port,report))throw std::runtime_error("Authenticated peer exchange failed");
        sync_report(report.received);return report.received.pending||report.peerPending?2:0;
    }
    const auto file=sync_absolute(options.at(L"--package"));std::wstring package;Result result;
    if(command==L"--import"){
        sync_read_package(file,package);if(!ImportChanges(storage,package,result))throw std::runtime_error("Cannot import exchange file");
    }else{
        if(!ExportChanges(storage,package,result))throw std::runtime_error("Cannot export replica");sync_write_package(file,package);
    }
    sync_report(result);return result.pending?2:0;
}
}
int wmain(int argc,wchar_t** argv)
{
    try{
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
        return sync_run(argc,argv);
    }catch(const std::exception& e){std::cerr<<"TCardSync: "<<e.what()<<"\n";return 1;}
    catch(...){std::cerr<<"TCardSync: operation failed\n";return 1;}
}
