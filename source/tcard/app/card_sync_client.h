#pragma once
#include "../core/card_sync_bridge.h"
#include "../core/card_sync_mail.h"
#include "../core/card_source_owner.h"
#include "../core/card_source_job.h"
#include "../core/card_source_candidate.h"
#include "../core/card_native_receive.h"
#include "../core/card_sync_network.h"
#include "../core/card_signal_protection.h"
#include <winrt/Windows.Data.Json.h>
#include <thread>
#include <atomic>

namespace tcard_bridge {
struct Settings {
    bool enabled=false;
    unsigned listenPort=49021,peerPort=0;
    std::wstring joinProfile,peers,whitelist=L"127.0.0.1/32",blacklist;
    bool server=true,hasPolicy=false,sharePeers=false,remoteAdmission=false;
    bool signaling=false,hostSignal=false;
    std::wstring signalNodes,publishedSyncEndpoint,hostSignalPin;
};
inline Settings ReadSettings(const std::wstring& ini)
{
    Settings settings;settings.enabled=GetPrivateProfileIntW(L"TCardSync",L"Enabled",0,ini.c_str())!=0;
    settings.listenPort=GetPrivateProfileIntW(L"TCardSync",L"ListenPort",49021,ini.c_str());
    settings.peerPort=GetPrivateProfileIntW(L"TCardSync",L"PeerPort",0,ini.c_str());
    wchar_t profile[32768]{};GetPrivateProfileStringW(L"TCardSync",L"JoinProfile",L"",profile,32768,ini.c_str());settings.joinProfile=profile;
    // Native synchronization includes inbound listening; retain the legacy field/CLI contract.
    settings.server=true;
    settings.sharePeers=GetPrivateProfileIntW(L"TCardSync",L"SharePeers",0,ini.c_str())!=0;
    auto read=[&](const wchar_t* key,const wchar_t* fallback){wchar_t value[8192]{};GetPrivateProfileStringW(L"TCardSync",key,fallback,value,8192,ini.c_str());return std::wstring(value);};
    settings.remoteAdmission=read(L"RemoteAdmission",L"0")==L"1";
    settings.signaling=read(L"Signaling",L"0")==L"1";settings.hostSignal=read(L"HostSignal",L"0")==L"1";
    settings.signalNodes=read(L"SignalNodes",L"");settings.publishedSyncEndpoint=read(L"PublishedSyncEndpoint",L"");settings.hostSignalPin=read(L"HostSignalPin",L"");
    settings.peers=read(L"Peers",L"");settings.blacklist=read(L"Blacklist",L"");
    settings.whitelist=read(L"Whitelist",L"@unset");settings.hasPolicy=settings.whitelist!=L"@unset";
    if(!settings.hasPolicy)settings.whitelist=L"127.0.0.1/32";
    return settings;
}
inline bool ValidSettings(const Settings& settings)
{
    if(!settings.listenPort||settings.listenPort>65535||settings.peerPort>65535||settings.joinProfile.find_first_of(L"\r\n")!=std::wstring::npos)return false;
    std::vector<tcard_net::Rule> allow,deny;std::vector<tcard_net::Peer> peers;
    if(!tcard_net::ParseRules(settings.whitelist,allow)||!tcard_net::ParseRules(settings.blacklist,deny)||
        !tcard_net::ParsePeers(settings.peers,settings.peerPort?settings.peerPort:settings.listenPort,peers))return false;
    if(!settings.hasPolicy&&settings.peers.empty()&&settings.peerPort==settings.listenPort)return false;
    for(const auto& peer:peers)if((peer.address>>24)==127&&settings.server&&peer.port==settings.listenPort)return false;
    if(settings.signalNodes.size()>8192||settings.publishedSyncEndpoint.size()>32||
        settings.signalNodes.find_first_of(L"\r\n")!=std::wstring::npos||settings.publishedSyncEndpoint.find_first_of(L"\r\n")!=std::wstring::npos)return false;
    if(settings.hostSignalPin.size()>64||settings.hostSignalPin.find_first_of(L"\r\n")!=std::wstring::npos)return false;
    if(settings.hostSignal&&!tcard_sync::sync_hex(settings.hostSignalPin))return false;
    if(settings.enabled&&settings.signaling){std::vector<SignalNode> nodes;tcard_net::Peer published;
        if(!ParseSignalNodes(settings.signalNodes,nodes)||nodes.empty()||!ParsePublishedSyncEndpoint(settings.publishedSyncEndpoint,published))return false;}
    return true;
}
// Edit-only normalization preserves legacy effective ports and provisioning settings.
inline std::wstring EditLines(const std::wstring& value)
{
    std::vector<std::wstring> entries;if(!tcard_net::SplitList(value,entries))return value;std::wstring result;
    for(const auto& item:entries){if(!result.empty())result+=L"\r\n";result+=item;}return result.size()<=4096?result:value;
}
inline std::wstring EditPeers(const Settings& settings)
{
    auto text=settings.peers;
    if(!settings.hasPolicy&&text.empty()&&settings.peerPort)text=L"127.0.0.1";
    std::vector<tcard_net::Peer> peers;if(!tcard_net::ParsePeers(text,settings.peerPort?settings.peerPort:settings.listenPort,peers))return text;
    std::wstring result;for(const auto& peer:peers){if(!result.empty())result+=L"\r\n";result+=peer.ip+L":"+std::to_wstring(peer.port);}return result;
}
inline bool AddPeerRules(Settings& settings)
{
    std::vector<std::wstring> entries,tokens;if(!tcard_net::SplitList(settings.whitelist,entries)||!tcard_net::SplitList(settings.peers,tokens))return false;
    bool changed=false;
    for(const auto& token:tokens){std::vector<tcard_net::Peer> peers;if(!tcard_net::ParsePeers(token,settings.peerPort?settings.peerPort:settings.listenPort,peers)||peers.empty())continue;
        const auto& ip=peers[0].ip;const auto exact=ip+L"/32";
        if(std::find(entries.begin(),entries.end(),ip)==entries.end()&&std::find(entries.begin(),entries.end(),exact)==entries.end()){
            if(entries.size()>=64)return false;entries.push_back(exact);changed=true;
        }
    }
    if(changed){std::wstring text;for(const auto& entry:entries){if(!text.empty())text+=L"\r\n";text+=entry;}
        if(text.size()>4096)return false;settings.whitelist=std::move(text);
    }return true;
}
inline bool SaveSettings(const std::wstring& ini,const Settings& settings)
{
    if(!ValidSettings(settings))return false;
    std::vector<wchar_t> section(65536);const auto length=GetPrivateProfileSectionW(L"TCardSync",section.data(),static_cast<DWORD>(section.size()),ini.c_str());
    if(length>=section.size()-2)return false;std::wstring values;
    for(const wchar_t* item=section.data();*item;item+=wcslen(item)+1){
        if(_wcsnicmp(item,L"Enabled=",8)==0||_wcsnicmp(item,L"ListenPort=",11)==0||_wcsnicmp(item,L"PeerPort=",9)==0||_wcsnicmp(item,L"JoinProfile=",12)==0||
            _wcsnicmp(item,L"Signaling=",10)==0||_wcsnicmp(item,L"HostSignal=",11)==0||_wcsnicmp(item,L"HostSignalPin=",14)==0||_wcsnicmp(item,L"SignalNodes=",12)==0||_wcsnicmp(item,L"PublishedSyncEndpoint=",22)==0||
            _wcsnicmp(item,L"RemoteAdmission=",16)==0||_wcsnicmp(item,L"SharePeers=",11)==0||_wcsnicmp(item,L"ServerMode=",11)==0||_wcsnicmp(item,L"Peers=",6)==0||_wcsnicmp(item,L"Whitelist=",10)==0||_wcsnicmp(item,L"Blacklist=",10)==0)continue;
        values+=item;values.push_back(L'\0');
    }
    auto add=[&](const std::wstring& item){values+=item;values.push_back(L'\0');};
    add(std::wstring(L"Enabled=")+(settings.enabled?L"1":L"0"));add(L"ListenPort="+std::to_wstring(settings.listenPort));
    add(L"PeerPort="+std::to_wstring(settings.peerPort));add(L"JoinProfile="+settings.joinProfile);
    add(L"ServerMode=1");
    add(std::wstring(L"SharePeers=")+(settings.sharePeers?L"1":L"0"));
    add(std::wstring(L"RemoteAdmission=")+(settings.remoteAdmission?L"1":L"0"));
    add(std::wstring(L"Signaling=")+(settings.signaling?L"1":L"0"));add(std::wstring(L"HostSignal=")+(settings.hostSignal?L"1":L"0"));
    add(L"SignalNodes="+settings.signalNodes);add(L"PublishedSyncEndpoint="+settings.publishedSyncEndpoint);add(L"HostSignalPin="+settings.hostSignalPin);
    // Preserve legacy callers that expressed their single loopback peer only by PeerPort.
    add(L"Peers="+(!settings.hasPolicy&&settings.peers.empty()&&settings.peerPort?L"127.0.0.1":tcard_net::Flatten(settings.peers)));
    add(L"Whitelist="+tcard_net::Flatten(settings.whitelist));add(L"Blacklist="+tcard_net::Flatten(settings.blacklist));values.push_back(L'\0');
    if(values.size()>65535)return false;
    return WritePrivateProfileSectionW(L"TCardSync",values.c_str(),ini.c_str())!=FALSE;
}
class Client {
    HANDLE process_=nullptr,control_=nullptr;
    DWORD pid_=0;
    HANDLE acquisition_=nullptr,acquisitionStop_=nullptr;DWORD acquisitionPid_=0;
    std::wstring acquisitionJob_,acquisitionRequest_,acquisitionLaunch_;
    std::wstring directory_,profile_,sent_,received_,check_,storage_;
    struct ExportTask {
        std::atomic_bool done=false;bool okay=false;std::wstring package,hash;tcard_sync::Result result;
        Stamp stamp,appliedStamp;bool batchMode=false;tcard_mail::Producer batch;
        std::jthread worker; // Destroy/join before fields used by the reader are released.
    };
    std::unique_ptr<ExportTask> exportTask_;
    struct SourceTask {std::atomic_bool done=false;bool okay=false;std::wstring hash;std::jthread worker;};
    std::unique_ptr<SourceTask> sourceTask_;std::wstring sourceQuery_;bool ownerSourceCap_=false;
    tcard_mail::Producer batch_;bool batchCap_=false,objectCap_=false,sourceCap_=false,dependencyCap_=false,batchMode_=false;
    std::wstring dependencySpace_,dependencyGroup_,dependencyNode_;
    struct ReceiveTask {
        std::atomic_bool done=false;bool okay=false;std::wstring hash;Stamp stamp;
        std::vector<tcard_sync::PreparedNote> notes;size_t next=0;
        bool batch=false,objects=false,dependencies=false;tcard_mail::Inventory required;tcard_dependencies::Context context;
        std::wstring target,delta;tcard_native::ReceiveResult native;
        bool started=false,failed=false,pending=false,activated=false,proving=false;std::wstring proofGeneration,workerOwner,nativeOwner;ULONGLONG retryAt=0;
        std::jthread worker;
    };
    std::unique_ptr<ReceiveTask> receiveTask_;
    bool atomicWaiting_=false;
    std::vector<std::wstring> refresh_;
    std::wstring mailbox_,readyPhase_;
    Stamp readyStamp_,sentStamp_,receivedStamp_,appliedStamp_;
    ULONGLONG readyAt_=0,receivedAt_=0,retryAt_=0;
    bool receivedValid_=false;
    bool admitted_=false;
    HANDLE changes_=INVALID_HANDLE_VALUE;
    std::wstring watched_;
    ULONGLONG exported_=0;
    bool Changed(const std::wstring& storage)
    {
        if(watched_!=storage||changes_==INVALID_HANDLE_VALUE){
            if(changes_!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(changes_);
            watched_=storage;changes_=FindFirstChangeNotificationW(storage.c_str(),TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_SECURITY);
            return true;
        }
        const auto result=WaitForSingleObject(changes_,0);
        if(result==WAIT_TIMEOUT)return sent_.empty()||GetTickCount64()-exported_>=60000;
        // Rearm before export so a concurrent writer remains visible to the next poll.
        if(result!=WAIT_OBJECT_0||!FindNextChangeNotification(changes_)){
            FindCloseChangeNotification(changes_);changes_=INVALID_HANDLE_VALUE;
        }
        return true;
    }

    bool SendBatches(const std::wstring& storage,bool async)
    {
        if(!batchCap_)return false;
        try{
            const auto owner=tcard_ack::Owner(process_,pid_);tcard_mail::Group(storage,mailbox_);
            const tcard_dependencies::Context channel{dependencySpace_,dependencyGroup_,dependencyCap_?tcard_bootstrap::Node(storage):L"",dependencyNode_,L""};
            const bool changed=Changed(storage);Stamp applied;
            if(!changed&&!batch_.Changed(mailbox_,L"outgoing.json",L"outgoing-commit.json",owner)&&
                Fingerprint(mailbox_,L"applied.json",applied)&&applied==appliedStamp_)return true;
            if(!async){
                tcard_sync::Result result;std::wstring header;unsigned version=0;
                if(!tcard_mail::Produce(storage,mailbox_,L"outgoing.json",L"outgoing-commit.json",owner,false,changed,batch_,result,objectCap_,sourceCap_,dependencyCap_?&channel:nullptr)||
                    !tcard_sync::ExportInventory(storage,header,result,version)||!tcard_ack::PublishInventory(mailbox_,storage,header,true,objectCap_,true,dependencyCap_))return false;
                sent_=batch_.hash;exported_=GetTickCount64();Fingerprint(mailbox_,L"outgoing.json",sentStamp_);Fingerprint(mailbox_,L"applied.json",appliedStamp_);return true;
            }
            exportTask_=std::make_unique<ExportTask>();auto* task=exportTask_.get();task->batch=batch_;task->batchMode=true;const auto mailbox=mailbox_;const auto objects=objectCap_,origins=sourceCap_,dependencies=dependencyCap_;
            task->worker=std::jthread([task,storage,mailbox,owner,changed,objects,origins,dependencies,channel]{
                try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                    std::wstring header;unsigned version=0;
                    task->okay=tcard_mail::Produce(storage,mailbox,L"outgoing.json",L"outgoing-commit.json",owner,true,changed,task->batch,task->result,objects,origins,dependencies?&channel:nullptr)&&
                        tcard_sync::ExportCommittedInventory(storage,header,task->result,version)&&tcard_ack::PublishInventory(mailbox,storage,header,true,objects,true,dependencies)&&
                        Fingerprint(mailbox,L"outgoing.json",task->stamp)&&Fingerprint(mailbox,L"applied.json",task->appliedStamp);
                    task->hash=task->batch.hash;
                }catch(...){task->okay=false;}task->done=true;
            });return false;
        }catch(...){exported_=0;status=L"sync.export_failed";return false;}
    }
    tcard_crypto::Keys storageKeys_;
    tcard_source_owner::Binding SourceBinding()const
    {
        tcard_crypto::Keys keys;if(!ownerSourceCap_||!dependencyCap_||storage_.empty()||!tcard_storage::LoadKey(storage_,keys)||!tcard_storage::Matches(profile_,keys))throw std::runtime_error("Source owner unavailable");
        tcard_source_owner::Binding binding{tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId()),tcard_ack::Owner(process_,pid_),tcard_ack::Identity(keys),dependencyNode_,tcard_bootstrap::Node(storage_)};
        tcard_source_owner::Validate(binding);return binding;
    }
    void ReplySource(const std::wstring& storage)
    {
        if(sourceTask_){if(!sourceTask_->done.load())return;if(sourceTask_->okay)sourceQuery_=sourceTask_->hash;sourceTask_.reset();}
        if(!ownerSourceCap_||!dependencyCap_)return;
        try{
            std::wstring request;if(!Read(mailbox_,tcard_source_owner::requestName,request)||Hash(request)==sourceQuery_)return;
            const auto binding=SourceBinding();const auto pending=tcard_sync::JsonObject::Parse(request);tcard_source_owner::Check(pending,L"source-request",tcard_sync::sync_string(pending,L"Request"),binding,9);auto task=std::make_unique<SourceTask>();task->hash=Hash(request);auto* work=task.get();const auto mailbox=mailbox_;const auto process=process_;
            work->worker=std::jthread([work,storage,mailbox,binding,process,request]{
                try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                    work->okay=tcard_source_owner::Serve(storage,mailbox,binding,process,request);
                }catch(...){work->okay=false;}work->done=true;
            });sourceTask_=std::move(task);
        }catch(...){/* The source query remains pending; ordinary synchronization continues. */}
    }
public:
    std::wstring status=L"sync.off";
    ~Client(){if(changes_!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(changes_);Stop();if(control_){WaitForSingleObject(control_,15000);CloseHandle(control_);}}
    bool Available()const{return tcard_bridge::Available(directory_+L"\\TCardSync.exe");}
    bool SignalAvailable()const{return Available()&&tcard_bridge::Available(directory_+L"\\TCardSignal.exe");}
    DWORD Pid()const{return pid_;}
    void SetDirectory(const std::wstring& directory){directory_=directory;profile_=directory+L"\\tcard\\sync";}
    const std::wstring& Profile()const{return profile_;}
    // Explicit product boundary; legacy CLI/test clients remain removable compatibility callers.
    void SetStorage(const std::wstring& storage){storage_=storage;admitted_=false;}
    // The native owner calls this before loading cards, even with Sync OFF or no worker.
    bool Recover(const tcard_transition::Progress& progress={})
    {
        tcard_activation::Status activation;
        if(!tcard_activation::Inspect(profile_,activation)){status=L"sync.rotation_pending";return false;}
        if(activation.phase.empty()||(activation.active&&tcard_storage::Ready(storage_)&&tcard_storage::Ready(profile_)))return true;
        admitted_=false;
        if(storage_.empty()||!Stop()||(control_&&WaitForSingleObject(control_,0)!=WAIT_OBJECT_0)){status=L"sync.rotation_pending";return false;}
        auto normalized=profile_;for(auto& ch:normalized)ch=static_cast<wchar_t>(towlower(ch));
        tcard_asset::File owner(CreateMutexW(nullptr,TRUE,(L"Local\\TCardSync.Owner."+Hash(normalized)).c_str()));
        const auto created=GetLastError();
        if(!owner||created==ERROR_ALREADY_EXISTS){status=L"sync.rotation_pending";return false;}
        struct Release{HANDLE value;~Release(){ReleaseMutex(value);}} release{owner.value};
        if(!tcard_activation::Resume(storage_,profile_,activation,progress)||!activation.active||
            !tcard_storage::LoadKey(storage_,storageKeys_)||!tcard_storage::Matches(profile_,storageKeys_)){
            status=L"sync.rotation_pending";return false;
        }
        sent_.clear();received_.clear();check_.clear();admitted_=true;return true;
    }
    bool Rotate(std::wstring& display,const tcard_transition::Progress& progress={})
    {
        display.clear();
        if(!Encrypted()){status=L"sync.storage_required";return false;}
        std::wstring staged;
        struct Clear{std::wstring& value;~Clear(){SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} clear{staged};
        const auto idle=[&]{if(progress)progress(L"",0,0);};
        if(!Key(L"rotate",L"",staged,L"",L"",idle)||!Recover(progress)||!Key(L"show",L"",display,L"",L"",idle))return false;
        status=L"sync.key_updated";return true;
    }
    // Classification never selects a different namespace or stages old native content.
    bool Match(const std::wstring& words,bool& same)
    {
        same=false;std::wstring result;
        if(!Key(L"match",words,result)||(result!=L"same"&&result!=L"different"))return false;
        same=result==L"same";return true;
    }
    // The native owner has explicitly chosen Merge and quiesced all writers.
    bool Merge(const std::wstring& words,std::wstring& display)
    {
        display.clear();if(!Encrypted()){status=L"sync.storage_required";return false;}
        std::wstring staged;
        struct Clear{std::wstring& value;~Clear(){SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} clear{staged};
        if(!Key(L"merge",words,staged)||!Recover()||!Key(L"show",L"",display))return false;
        status=L"sync.key_updated";return true;
    }
    // Explicit Replace consent and quiesced UI-thread writers are required before calling.
    // PrepareCandidate is a separate resumable local step and never runs during activation.
    bool Replace(const std::wstring& job)
    {
        if(!Encrypted()||!Available()||!Stop())return false;
        try{
            using namespace tcard_source_job;Intent intent;ReadIntent(profile_,job,intent);tcard_crypto::Keys previous,candidate;
            auto verify=[&](tcard_source_job::Snapshot& source,tcard_replacement::Candidate& prepared){
                Intent current;ReadIntent(profile_,job,current);Require(current.digest==intent.digest&&!tcard_activation::Pending(profile_)&&tcard_storage::LoadKey(storage_,previous)&&tcard_storage::Matches(profile_,previous)&&tcard_ack::Identity(previous)==intent.previous,"Replacement previous membership changed");
                Require(Inspect(profile_,job,source)&&source.ready&&tcard_storage::LoadKey(source.path,candidate)&&tcard_ack::Identity(candidate)==source.group,"Replacement source is not verified");
                Require(tcard_replacement::Inspect(CandidateRoot(profile_,job),prepared)&&prepared.ready&&prepared.node==source.node&&prepared.target==source.target&&tcard_storage::Matches(prepared.path,candidate),"Replacement candidate is incomplete or changed");
            };
            tcard_source_job::Snapshot source;tcard_replacement::Candidate prepared;verify(source,prepared);std::wstring group;
            if(!Key(L"replacement",L"",group,L"",job))return false;
            verify(source,prepared);Require(CheckCredentials(profile_,intent,storage_,source)==group,"Replacement credential namespace differs");
            admitted_=false;tcard_activation::Status activation;
            if(!tcard_activation::StageReplacement(storage_,profile_,group,candidate,prepared.path,activation)||!Recover()){status=L"sync.rotation_pending";return false;}
            status=L"sync.key_updated";return true;
        }catch(...){status=L"sync.key_failed";return false;}
    }
    bool Encrypted()const{return GetFileAttributesW((storage_+L".json").c_str())==INVALID_FILE_ATTRIBUTES&&!storage_.empty()&&tcard_storage::Required(storage_)&&tcard_storage::Ready(storage_)&&tcard_storage::Required(profile_)&&tcard_storage::Ready(profile_);}
    bool Prepare(const Settings& settings)
    {
        admitted_=false;
        // Completed stores authenticate each file on access. Initial/resumed migration still verifies the full tree.
        // The separate worker independently derives the current identity and rechecks both roots before networking.
        if(Encrypted()&&tcard_storage::LoadKey(storage_,storageKeys_)&&tcard_storage::Matches(profile_,storageKeys_)){
            admitted_=true;return true;
        }
        std::wstring material;
        struct Wipe{std::wstring& value;~Wipe(){SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} wipe{material};
        if(storage_.empty()||!Key(L"storage",L"",material,settings.joinProfile)||material.size()!=194){status=L"sync.storage_failed";return false;}
        auto digit=[](wchar_t c){return c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'f'?c-L'a'+10:-1;};
        size_t at=0;auto decode=[&](auto& bytes){for(auto& byte:bytes){const int a=digit(material[at++]),b=digit(material[at++]);if(a<0||b<0)return false;byte=static_cast<uint8_t>((a<<4)|b);}return true;};
        std::string error;tcard_crypto::Bytes legacy;
        struct BytesScope{tcard_crypto::Bytes& value;~BytesScope(){SecureZeroMemory(value.data(),value.size());}} legacyScope{legacy};
        const auto oldPath=storage_+L".json";const auto attributes=GetFileAttributesW(oldPath.c_str());
        const bool hasLegacy=attributes!=INVALID_FILE_ATTRIBUTES;
        if(!hasLegacy&&GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND){status=L"sync.storage_failed";return false;}
        if(hasLegacy){
            const auto slash=oldPath.find_last_of(L'\\');
            if(slash==std::wstring::npos||!tcard_asset::read(oldPath.substr(0,slash),oldPath.substr(slash+1),legacy)){status=L"sync.storage_failed";return false;}
            if(!tcard_storage::Required(storage_)){
                tcard_asset::File directory(tcard_asset::open_dir(storage_,true));
                // Retained legacy source is immutable until the encrypted copy has been verified.
                if(!directory||!tcard_asset::publish(storage_,L"cards.json",legacy,true)){status=L"sync.storage_failed";return false;}
            }
        }
        if(!decode(storageKeys_.publicKey)||!decode(storageKeys_.dataKey)||!tcard_storage::Enable(storage_,storageKeys_,error)||!tcard_storage::Matches(profile_,storageKeys_)){
            status=L"sync.storage_failed";return false;
        }
        if(hasLegacy){
            tcard_crypto::Bytes verified,current;BytesScope verifiedScope{verified};BytesScope currentScope{current};const auto slash=oldPath.find_last_of(L'\\');
            if(!tcard_asset::read(storage_,L"cards.json",verified)||verified!=legacy||
                !tcard_asset::read(oldPath.substr(0,slash),oldPath.substr(slash+1),current)||current!=legacy||!DeleteFileW(oldPath.c_str())){
                status=L"sync.storage_failed";return false;
            }
        }
        admitted_=true;return true;
    }
    std::wstring DefaultWhitelist()
    {
        if(!Available())return L"127.0.0.1/32";
        const auto mailbox=directory_+L"\\tcard\\sync-network";DWORD child=0;
        tcard_asset::File process(Launch(directory_+L"\\TCardSync.exe",{L"--network-info",L"--profile",mailbox},child));
        if(!process||WaitForSingleObject(process.value,15000)!=WAIT_OBJECT_0)return L"127.0.0.1/32";
        DWORD result=1;if(!GetExitCodeProcess(process.value,&result)||result)return L"127.0.0.1/32";
        std::wstring text;if(!Read(mailbox,L"defaults.json",text))return L"127.0.0.1/32";
        winrt::Windows::Data::Json::JsonObject data;
        if(!winrt::Windows::Data::Json::JsonObject::TryParse(text,data)||data.GetNamedNumber(L"Pid",0)!=child)return L"127.0.0.1/32";
        return data.GetNamedString(L"Whitelist",L"127.0.0.1/32").c_str();
    }
    bool Key(const wchar_t* action,const std::wstring& words,std::wstring& display,const std::wstring& from=L"",const std::wstring& job=L"",const std::function<void()>& idle={})
    {
        display.clear();if(!Available()||!Stop())return false;
        // A lost profile beside encrypted cards is recovery, never implicit new-group provisioning.
        if(!storage_.empty()&&tcard_storage::Required(storage_)&&!tcard_storage::Required(profile_)&&!tcard_bridge::Available(profile_+L"\\sync-profile.json")){
            status=L"sync.storage_failed";return false;
        }
        if(wcscmp(action,L"import")==0||wcscmp(action,L"new")==0||wcscmp(action,L"rotate")==0||wcscmp(action,L"merge")==0)admitted_=false;
        const auto wait=[&](HANDLE process,DWORD timeout){
            if(!idle)return WaitForSingleObject(process,timeout);
            const auto started=GetTickCount64();
            for(;;){
                idle();const auto elapsed=GetTickCount64()-started;
                const DWORD remaining=elapsed>=timeout?0:timeout-static_cast<DWORD>(elapsed);
                const auto result=WaitForSingleObject(process,(std::min)(remaining,DWORD(25)));
                if(result!=WAIT_TIMEOUT||!remaining)return result;
            }
        };
        if(control_){if(wait(control_,15000)!=WAIT_OBJECT_0){status=L"sync.key_failed";return false;}CloseHandle(control_);control_=nullptr;}
        const auto directory=profile_+L"-control";if(!Protect(directory)){status=L"sync.key_failed";return false;}
        const auto token=Token();winrt::Windows::Data::Json::JsonObject request;
        request.Insert(L"Action",winrt::Windows::Data::Json::JsonValue::CreateStringValue(action));request.Insert(L"Words",winrt::Windows::Data::Json::JsonValue::CreateStringValue(words));
        if(wcscmp(action,L"rotate")==0||wcscmp(action,L"merge")==0||wcscmp(action,L"replacement")==0)request.Insert(L"Native",winrt::Windows::Data::Json::JsonValue::CreateStringValue(storage_));
        if(wcscmp(action,L"replacement")==0)request.Insert(L"Job",winrt::Windows::Data::Json::JsonValue::CreateStringValue(job));
        auto plain=std::wstring(request.Stringify().c_str());std::wstring sealed;
        const bool protectedRequest=Seal(plain,sealed);SecureZeroMemory(plain.data(),plain.size()*sizeof(wchar_t));
        if(!protectedRequest||!Publish(directory,token+L".request",sealed)){status=L"sync.key_failed";return false;}
        std::vector<std::wstring> args{L"--key-service",L"--profile",profile_,L"--request",token};if(!from.empty())args.insert(args.end(),{L"--from-profile",from});
        DWORD child=0;control_=Launch(directory_+L"\\TCardSync.exe",args,child);bool okay=false;
        if(control_&&wait(control_,(wcscmp(action,L"storage")==0||wcscmp(action,L"rotate")==0||wcscmp(action,L"merge")==0||wcscmp(action,L"replacement")==0)?120000:15000)==WAIT_OBJECT_0){
            DWORD code=1;GetExitCodeProcess(control_,&code);CloseHandle(control_);control_=nullptr;
            if(!code&&Read(directory,token+L".response",sealed)&&Seal(sealed,plain,true)){
                winrt::Windows::Data::Json::JsonObject result;
                if(winrt::Windows::Data::Json::JsonObject::TryParse(plain,result)&&result.GetNamedNumber(L"Pid",0)==child){
                    okay=result.GetNamedBoolean(L"Ok",false);if(okay)display=result.GetNamedString(L"Words",L"").c_str();
                    else status=result.GetNamedString(L"Error",L"sync.key_failed").c_str();
                }
                SecureZeroMemory(plain.data(),plain.size()*sizeof(wchar_t));
            }
        }
        if(!control_){DeleteFileW((directory+L"\\"+token+L".request").c_str());DeleteFileW((directory+L"\\"+token+L".response").c_str());}
        if(okay){sent_.clear();received_.clear();check_.clear();status=L"sync.key_updated";}
        else if(status!=L"sync.key_invalid"&&status!=L"sync.rotation_pending")status=L"sync.key_failed";return okay;
    }
    bool Check(const std::wstring& storage)
    {
        if(!Ready()||status==L"sync.failed"||status==L"sync.port_busy"||!Send(storage))return false;
        check_=Token();winrt::Windows::Data::Json::JsonObject request;
        request.Insert(L"Pid",winrt::Windows::Data::Json::JsonValue::CreateNumberValue(pid_));request.Insert(L"Request",winrt::Windows::Data::Json::JsonValue::CreateStringValue(check_));
        request.Insert(L"OutgoingHash",winrt::Windows::Data::Json::JsonValue::CreateStringValue(sent_));return Publish(Mailbox(profile_),L"check-request.json",request.Stringify().c_str());
    }
    bool CheckResult(winrt::Windows::Data::Json::JsonObject& result)
    {
        std::wstring text;return !check_.empty()&&Read(Mailbox(profile_),L"check-result.json",text)&&
            winrt::Windows::Data::Json::JsonObject::TryParse(text,result)&&result.GetNamedNumber(L"Pid",0)==pid_&&result.GetNamedString(L"Request",L"")==check_;
    }
    bool Running()const{return process_&&WaitForSingleObject(process_,0)==WAIT_TIMEOUT;}
    // Called only after an explicit Replace/source choice; never ordinary synchronization.
    bool BeginAcquisition(const std::wstring& words,const std::wstring& endpoint,const Settings& settings,std::wstring& job)
    {
        job.clear();if(!Available()||!Stop())return false;
        if(control_&&WaitForSingleObject(control_,0)!=WAIT_OBJECT_0){status=L"sync.key_busy";return false;}
        try{
            using namespace tcard_source_job;tcard_crypto::Keys previous;
            Require(!storage_.empty()&&tcard_storage::Required(storage_)&&tcard_storage::LoadKey(storage_,previous)&&tcard_storage::Matches(profile_,previous)&&!tcard_activation::Pending(profile_),"Acquisition requires encrypted owner roots");
            Require(tcard_bridge::Protect(Control(profile_)),"Acquisition control unavailable");const auto token=Hash(Token());JsonObject record;
            record.Insert(L"Version",JsonValue::CreateNumberValue(1));record.Insert(L"Remote",JsonValue::CreateBooleanValue(settings.remoteAdmission));
            for(const auto& [key,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",L"replace-source-intent"},{L"Profile",sync_path_plain(profile_)},{L"Job",token},{L"Previous",tcard_ack::Identity(previous)},
                {L"Endpoint",endpoint},{L"Allow",settings.whitelist},{L"Deny",settings.blacklist},{L"Words",words}})record.Insert(key,JsonValue::CreateStringValue(value));
            Require(Put(profile_,token+L".intent",record),"Cannot retain acquisition intent");Intent checked;ReadIntent(profile_,token,checked);
            Require(SelectPending(profile_,checked),"Cannot retain pending acquisition selection");job=token;return ResumeAcquisition(job);
        }catch(...){status=L"sync.key_failed";return false;}
    }
    const std::wstring& Directory()const{return directory_;}
    bool AcquisitionRunning()const{return acquisition_&&WaitForSingleObject(acquisition_,0)==WAIT_TIMEOUT;}
    DWORD AcquisitionPid()const{return acquisitionPid_;}
    bool ResumeAcquisition(const std::wstring& job)
    {
        if(!Available()||!Stop())return false;
        if(control_&&WaitForSingleObject(control_,0)!=WAIT_OBJECT_0){status=L"sync.key_busy";return false;}
        try{
            using namespace tcard_source_job;Intent intent;ReadIntent(profile_,job,intent);tcard_crypto::Keys previous;
            Require(!storage_.empty()&&tcard_storage::LoadKey(storage_,previous)&&tcard_storage::Matches(profile_,previous)&&tcard_ack::Identity(previous)==intent.previous&&!tcard_activation::Pending(profile_),"Acquisition owner changed");
            const auto request=Hash(Token());auto launch=Record(L"replace-source-launch",intent);launch.Insert(L"Request",JsonValue::CreateStringValue(request));
            launch.Insert(L"Parent",JsonValue::CreateStringValue(tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId())));
            Require(Put(profile_,request+L".launch",launch),"Cannot publish acquisition launch");const auto event=Event(profile_,request);
            acquisitionStop_=CreateEventW(nullptr,TRUE,FALSE,event.c_str());const auto created=GetLastError();Require(acquisitionStop_&&created!=ERROR_ALREADY_EXISTS,"Acquisition stop event differs");
            acquisitionRequest_=request;acquisitionJob_=job;acquisitionLaunch_=Hash(std::wstring(launch.Stringify()));
            acquisition_=Launch(directory_+L"\\TCardSync.exe",{L"--source-service",L"--profile",profile_,L"--request",request},acquisitionPid_);
            if(!acquisition_){CloseHandle(acquisitionStop_);acquisitionStop_=nullptr;throw std::runtime_error("Cannot launch acquisition worker");}
            status=L"sync.starting";return true;
        }catch(...){if(!acquisition_&&acquisitionStop_){CloseHandle(acquisitionStop_);acquisitionStop_=nullptr;}status=L"sync.key_failed";return false;}
    }
    bool ReadAcquisition(tcard_source_job::Snapshot& snapshot)
    {
        snapshot={};try{
            using namespace tcard_source_job;Require(acquisition_!=nullptr,"No acquisition child");Intent intent;ReadIntent(profile_,acquisitionJob_,intent);
            const auto result=tcard_source_job::Read(profile_,acquisitionRequest_+L".status");tcard_source_job::Check(result,L"replace-source-status",intent,11);
            Require(sync_string(result,L"Request")==acquisitionRequest_&&sync_string(result,L"Launch")==acquisitionLaunch_&&sync_string(result,L"Parent")==tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId())&&
                sync_string(result,L"Worker")==tcard_ack::Owner(acquisition_,acquisitionPid_),"Acquisition status process differs");
            const auto phase=sync_string(result,L"Phase");Require(phase==L"acquiring"||phase==L"ready"||phase==L"failed"||phase==L"cancelled","Unknown acquisition phase");
            snapshot.job=acquisitionJob_;snapshot.path=Root(profile_,acquisitionJob_);snapshot.phase=phase;snapshot.error=sync_string(result,L"Error");
            if(phase==L"ready"){tcard_source_job::Snapshot verified;Require(Inspect(profile_,acquisitionJob_,verified)&&verified.ready,"Acquisition status lacks complete originals");snapshot=std::move(verified);}
            return true;
        }catch(...){snapshot={};return false;}
    }
    bool StopAcquisition()
    {
        if(acquisition_){
            if(WaitForSingleObject(acquisition_,0)!=WAIT_OBJECT_0){if(!acquisitionStop_||!SetEvent(acquisitionStop_)||WaitForSingleObject(acquisition_,15000)!=WAIT_OBJECT_0){status=L"sync.stopping";return false;}}
            CloseHandle(acquisition_);acquisition_=nullptr;acquisitionPid_=0;
        }
        if(acquisitionStop_){CloseHandle(acquisitionStop_);acquisitionStop_=nullptr;}acquisitionJob_.clear();acquisitionRequest_.clear();acquisitionLaunch_.clear();return true;
    }
    bool Start(const Settings& settings)
    {
        if(acquisition_){if(WaitForSingleObject(acquisition_,0)!=WAIT_OBJECT_0){status=L"sync.key_busy";return false;}if(!StopAcquisition())return false;}
        if(process_&&!Stop())return false;
        if(control_&&WaitForSingleObject(control_,0)!=WAIT_OBJECT_0){status=L"sync.key_failed";return false;}
        if(tcard_activation::Pending(profile_)){status=L"sync.rotation_pending";return false;}
        if(!settings.enabled){status=L"sync.off";return true;}
        if(!Available()){status=L"sync.unavailable";return false;}
        if(!ValidSettings(settings)){status=L"sync.invalid";return false;}
        if(!storage_.empty()&&(!admitted_||!tcard_storage::Matches(storage_,storageKeys_)||!tcard_storage::Matches(profile_,storageKeys_))){status=L"sync.storage_required";return false;}
        std::vector<std::wstring> arguments{L"--ui",L"--profile",profile_,L"--listen-port",std::to_wstring(settings.listenPort),
            L"--port",std::to_wstring(settings.peerPort),L"--parent-pid",std::to_wstring(GetCurrentProcessId())};
        arguments.insert(arguments.end(),{L"--share-peers",settings.sharePeers?L"1":L"0",L"--server",settings.server?L"1":L"0",L"--peers",
            !settings.hasPolicy&&settings.peers.empty()&&settings.peerPort?L"127.0.0.1":tcard_net::Flatten(settings.peers),
            L"--whitelist",tcard_net::Flatten(settings.whitelist),L"--blacklist",tcard_net::Flatten(settings.blacklist)});
        // Omit the new option when OFF so older optional workers retain their existing LAN command line.
        if(settings.remoteAdmission)arguments.insert(arguments.end(),{L"--remote-admission",L"1"});
        // OFF omits all new options, preserving older optional workers and remembered node configuration.
        if(settings.signaling)arguments.insert(arguments.end(),{L"--signaling",L"1",L"--signal-nodes",tcard_net::Flatten(settings.signalNodes),L"--published-sync",settings.publishedSyncEndpoint});
        if(!storage_.empty())arguments.insert(arguments.end(),{L"--storage-root",storage_});
        if(!settings.joinProfile.empty())arguments.insert(arguments.end(),{L"--from-profile",settings.joinProfile});
        process_=Launch(directory_+L"\\TCardSync.exe",arguments,pid_);sent_.clear();received_.clear();
        status=process_?L"sync.starting":L"sync.failed";return process_!=nullptr;
    }
    bool Stop()
    {
        if(!StopAcquisition())return false;
        sourceTask_.reset();sourceQuery_.clear();ownerSourceCap_=false;exportTask_.reset();receiveTask_.reset();atomicWaiting_=false;batch_={};batchMode_=false;batchCap_=false;objectCap_=false;sourceCap_=false;dependencyCap_=false;dependencySpace_.clear();dependencyGroup_.clear();dependencyNode_.clear();refresh_.clear();mailbox_.clear();readyPhase_.clear();readyAt_=0;receivedValid_=false;receivedAt_=0;retryAt_=0;
        if(!process_){status=L"sync.off";return true;}
        if(Running()){
            // Only signal the IPC event owned by this exact child, never another profile's listener.
            const auto eventName=L"Local\\TCardSync.UiStop."+std::to_wstring(pid_);
            for(unsigned attempt=0;attempt<100&&Running();++attempt){
                tcard_asset::File event(OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName.c_str()));
                if(event){SetEvent(event.value);break;}Sleep(20);
            }
            if(WaitForSingleObject(process_,15000)!=WAIT_OBJECT_0){status=L"sync.stopping";return false;}
        }
        CloseHandle(process_);process_=nullptr;pid_=0;status=L"sync.off";return true;
    }
    // Background-only explicit admission. Caller owns this Client exclusively and quiesces UI/native writers.
    // This does not launch Signal or change remembered sync settings; the future launcher must Check before GO.
    bool PrepareSignalProtection(std::wstring& error,std::stop_token stop={})
    {
        error.clear();try{
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
            if(stop.stop_requested()){error=L"Signal protection interrupted";return false;}
            if(storage_.empty()||!Encrypted()){error=L"Signal protection requires admitted native storage";return false;}
            if(control_&&WaitForSingleObject(control_,0)!=WAIT_OBJECT_0){error=L"Signal key service is busy";return false;}
            if(!Stop()){error=L"Signal worker did not stop normally";return false;}
            return tcard_signal_protection::Prepare(storage_,profile_,stop,error);
        }catch(...){error=L"Signal protection preparation failed";return false;}
    }
    bool Ready()
    {
        if(!Running()){if(process_)status=L"sync.failed";return false;}
        if(mailbox_.empty())mailbox_=Mailbox(profile_);
        Stamp stamp;const auto now=GetTickCount64();
        if(!Fingerprint(mailbox_,L"status.json",stamp)){mailbox_.clear();return false;}
        if(readyAt_&&now-readyAt_<60000&&stamp==readyStamp_){status=readyPhase_;return true;}
        std::wstring text;if(!Read(mailbox_,L"status.json",text))return false;
        winrt::Windows::Data::Json::JsonObject object;
        if(!winrt::Windows::Data::Json::JsonObject::TryParse(text,object)||object.GetNamedNumber(L"Pid",0)!=pid_)return false;
        const std::wstring phase=object.GetNamedString(L"Status",L"sync.failed").c_str();
        status=phase==L"sync.ready"||phase==L"sync.pending"||phase==L"sync.peer_failed"||phase==L"sync.port_busy"?phase:L"sync.failed";
        batchCap_=object.GetNamedBoolean(L"MailboxBatches",false);objectCap_=batchCap_&&object.GetNamedBoolean(L"MailboxObjects",false);sourceCap_=objectCap_&&object.GetNamedBoolean(L"MailboxSourceTarget",false);
        dependencyCap_=false;dependencySpace_.clear();dependencyGroup_.clear();dependencyNode_.clear();
        if(sourceCap_&&object.GetNamedBoolean(L"MailboxDependencies",false))try{
            tcard_crypto::Keys keys;if(!tcard_storage::LoadKey(profile_,keys))throw std::runtime_error("Dependency profile key unavailable");
            const auto space=tcard_sync::sync_string(object,L"DependencySpace"),group=tcard_sync::sync_string(object,L"DependencyGroup"),node=tcard_sync::sync_string(object,L"DependencyNode");
            if(!tcard_sync::sync_hex(space)||!tcard_sync::sync_hex(node)||group!=tcard_ack::Identity(keys)||
                (!storage_.empty()&&group!=tcard_ack::Identity(storageKeys_))||!tcard_storage::Matches(mailbox_,keys)||
                tcard_sync::sync_string(object,L"DependencyWorker")!=tcard_ack::Owner(process_,pid_)||
                tcard_sync::sync_string(object,L"DependencyOwner")!=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId()))
                throw std::runtime_error("Dependency owned worker status differs");
            dependencySpace_=space;dependencyGroup_=group;dependencyNode_=node;dependencyCap_=true;
        }catch(...){dependencyCap_=false;}
        ownerSourceCap_=dependencyCap_&&object.GetNamedBoolean(L"MailboxOwnerSource",false);
        readyPhase_=status;readyAt_=now;readyStamp_=stamp;return true;
    }
    bool Send(const std::wstring& storage)
    {
        exportTask_.reset();
        if(!Ready())return false;ReplySource(storage);if(batchMode_)return SendBatches(storage,false);
        std::wstring retained;
        // Notifications avoid rebuilding unchanged history; retain a periodic full integrity audit.
        // Mailbox loss/replacement is checked on every poll, independently of local changes.
        if(!Changed(storage)&&Read(Mailbox(profile_),L"outgoing.json",retained)&&Hash(retained)==sent_)return tcard_ack::Publish(Mailbox(profile_),storage,retained,batchCap_,objectCap_,true,dependencyCap_);
        std::wstring package;tcard_sync::Result result;
        if(!tcard_sync::ExportChanges(storage,package,result)){
            if(batchCap_&&tcard_mail::Budget(result)){batchMode_=true;return SendBatches(storage,false);}
            exported_=0;sent_.clear();status=L"sync.export_failed";return false;}
        exported_=GetTickCount64();const auto hash=Hash(package);
        // A cached publication is not durable delivery: repair a missing or replaced mailbox.
        if(hash==sent_&&Read(Mailbox(profile_),L"outgoing.json",retained)&&Hash(retained)==hash)return tcard_ack::Publish(Mailbox(profile_),storage,package,batchCap_,objectCap_,true,dependencyCap_);
        if(!Publish(Mailbox(profile_),L"outgoing.json",package)){sent_.clear();status=L"sync.failed";return false;}
        if(!tcard_ack::Publish(Mailbox(profile_),storage,package,batchCap_,objectCap_,true,dependencyCap_)){sent_.clear();status=L"sync.failed";return false;}
        sent_=hash;return true;
    }
    // Explicit on-demand local snapshot evidence; this does not authorize remote replacement.
    bool BeginSource(std::wstring& nonce)
    {
        nonce.clear();try{return Ready()&&tcard_source_owner::Begin(mailbox_,SourceBinding(),nonce);}catch(...){return false;}
    }
    bool ReadSource(const std::wstring& nonce,tcard_source_owner::Snapshot& snapshot)
    {
        snapshot={};try{const bool okay=Ready()&&tcard_source_owner::Inspect(mailbox_,SourceBinding(),nonce,snapshot);
            // A verified pending/ready result confirms that the owner receipt was durably published.
            if(okay&&!snapshot.target.empty())sourceQuery_=snapshot.challenge;return okay;
        }catch(...){return false;}
    }
    bool Sending()const{return exportTask_!=nullptr||sourceTask_!=nullptr;}
    bool Receiving()const{return receiveTask_!=nullptr;}
    bool SendAsync(const std::wstring& storage)
    {
        if(!Ready())return false;ReplySource(storage);
        if(exportTask_){
            if(!exportTask_->done.load())return false;
            auto task=std::move(exportTask_);if(task->worker.joinable())task->worker.join();
            if(!task->okay){
                if(task->result.error==L"Legacy journal requires UI preparation")return Send(storage);
                if(batchCap_&&tcard_mail::Budget(task->result)){batchMode_=true;return SendBatches(storage,true);}
                exported_=0;sent_.clear();status=L"sync.export_failed";return false;
            }
            if(task->batchMode)batch_=std::move(task->batch);
            exported_=GetTickCount64();sent_=std::move(task->hash);sentStamp_=task->stamp;appliedStamp_=task->appliedStamp;return true;
        }
        if(atomicWaiting_)return true;
        if(batchMode_)return SendBatches(storage,true);
        Stamp stamp,applied;const bool changed=Changed(storage);
        if(!changed&&!sent_.empty()&&Fingerprint(mailbox_,L"outgoing.json",stamp)&&stamp==sentStamp_&&
            (!tcard_storage::Required(mailbox_)||(Fingerprint(mailbox_,L"applied.json",applied)&&applied==appliedStamp_)))return true;
        exportTask_=std::make_unique<ExportTask>();auto* task=exportTask_.get();const auto mailbox=mailbox_,previous=sent_;const auto batches=batchCap_,objects=objectCap_,dependencies=dependencyCap_;
        try{task->worker=std::jthread([task,storage,mailbox,previous,batches,objects,dependencies]{
            try{
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                task->okay=tcard_sync::ExportSnapshot(storage,task->package,task->result);
                if(task->okay){
                    task->hash=Hash(task->package);std::wstring retained;
                    // Publication and integrity checks are background work; native application stays with the owner.
                    const bool retainedOkay=task->hash==previous&&Read(mailbox,L"outgoing.json",retained)&&Hash(retained)==task->hash;
                    task->okay=(retainedOkay||Publish(mailbox,L"outgoing.json",task->package))&&
                        tcard_ack::Publish(mailbox,storage,task->package,batches,objects,true,dependencies)&&Fingerprint(mailbox,L"outgoing.json",task->stamp)&&
                        (!tcard_storage::Required(mailbox)||Fingerprint(mailbox,L"applied.json",task->appliedStamp));
                }
            }catch(...){task->okay=false;}
            task->done=true;
        });}catch(...){exportTask_.reset();status=L"sync.export_failed";return false;}
        return false;
    }
    bool Receive(const std::wstring& storage,bool blocked,std::vector<tcard::CardRecord>& cards,
        std::vector<tcard::CardRecord>* previous=nullptr)
    {
        if(previous)previous->clear();
        if(!Ready())return false;
        const auto now=GetTickCount64();
        if(receiveTask_&&!receiveTask_->done.load())return false;
        if(blocked&&receiveTask_){if(receiveTask_->okay&&receiveTask_->hash!=received_)status=L"sync.waiting_edit";return false;}
        if(!receiveTask_){
            Stamp stamp;
            const bool freshStamp=Fingerprint(mailbox_,L"incoming.json",stamp)&&
                (!receivedValid_||!(stamp==receivedStamp_)||now-receivedAt_>=60000);
            if(freshStamp&&(receivedValid_||!(stamp==receivedStamp_)||now>=retryAt_)){
                receiveTask_=std::make_unique<ReceiveTask>();auto* task=receiveTask_.get();task->stamp=stamp;const auto mailbox=mailbox_;const auto dependencies=dependencyCap_;
                const tcard_dependencies::Context channel{dependencySpace_,dependencyGroup_,dependencyNode_,dependencies?tcard_bootstrap::Node(storage):L"",L""};
                task->workerOwner=tcard_ack::Owner(process_,pid_);task->nativeOwner=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId());
                try{task->worker=std::jthread([task,mailbox,storage,stamp,dependencies,channel]{
                    try{winrt::init_apartment(winrt::apartment_type::multi_threaded);
                        struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                        std::wstring package;tcard_sync::Result result;
                        task->okay=Read(mailbox,L"incoming.json",package);
                        if(task->okay){task->hash=Hash(package);task->stamp=stamp;
                            task->batch=tcard_mail::Batched(package);
                            if(task->batch){std::wstring delta;task->dependencies=tcard_sync::JsonObject::Parse(package).GetNamedNumber(L"BatchVersion")==3;
                                if(task->dependencies){if(!dependencies)throw std::runtime_error("Dependency native capability absent");task->context=tcard_dependency_mail::Expected(package,channel,false);}
                                task->required=tcard_mail::Decode(package,tcard_mail::Group(storage,mailbox),delta,true,task->dependencies?&task->context:nullptr);task->target=tcard_handoff::Target(package);package=std::move(delta);}
                            task->objects=tcard_objects::Referenced(package);
                            if(!task->target.empty()){if(!task->dependencies)tcard_sync::selection::Validate(package,tcard_sync::selection::Read(task->target),true);task->delta=package;}
                            else if(task->objects){tcard_sync::PreparedNote note;task->okay=tcard_blobs::Prepare(storage,tcard_mail::Objects(mailbox,L"incoming.json"),package,note,result);if(task->okay)task->notes.push_back(std::move(note));}
                            else task->okay=tcard_sync::PrepareChanges(package,task->notes,result);}
                    }catch(...){task->okay=false;}task->done=true;
                });}catch(...){receiveTask_.reset();status=L"sync.apply_failed";}
                return false;
            }
        }
        if(blocked)return false;
        if(receiveTask_&&receiveTask_->proving&&!receiveTask_->okay){
            receiveTask_->proving=false;receiveTask_->okay=true;receiveTask_->retryAt=now+3000;receivedValid_=false;status=L"sync.apply_failed";return false;
        }
        if(receiveTask_&&!receiveTask_->okay){
            receivedStamp_=receiveTask_->stamp;receiveTask_.reset();atomicWaiting_=false;receivedValid_=false;retryAt_=now+3000;
            status=L"sync.apply_failed";return false;
        }
        if((!receiveTask_&&now<retryAt_)||(receiveTask_&&now<receiveTask_->retryAt))return false;
        if(receiveTask_&&!receiveTask_->target.empty()){
            atomicWaiting_=true;if(exportTask_)return false;
            auto* task=receiveTask_.get();
            if(!task->started){
                // Normalize legacy unknown deletion dates before capturing the immutable local baseline.
                std::vector<tcard::TrashRecord> trash;if(!tcard::LoadTrash(storage,trash)){status=L"sync.apply_failed";task->retryAt=now+3000;return false;}
                task->started=true;task->done=false;const auto mailbox=mailbox_;
                task->worker=std::jthread([task,storage,mailbox](std::stop_token stop){
                    try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                        task->native=tcard_native::Receive(storage,task->target,task->delta,tcard_mail::Objects(mailbox,L"incoming.json"),stop,task->dependencies?&task->context:nullptr);task->okay=true;
                    }catch(...){task->okay=false;}task->done=true;
                });return false;
            }
            bool changed=false;try{
                std::wstring header;
                if(task->native.ready||task->native.applied){
                    if(!task->activated){
                        if(task->native.ready)tcard_native::Activate(task->native.source);
                        std::vector<tcard::CardRecord> loaded;if(!tcard::LoadCardSnapshot(storage,loaded))throw std::runtime_error("Activated native snapshot unavailable");
                        if(previous)*previous=cards;cards=std::move(loaded);changed=true;task->activated=true;
                    }
                    if(task->dependencies){
                        if(!task->proving){
                            task->proving=true;task->done=false;const auto mailbox=mailbox_;
                            try{task->worker=std::jthread([task,storage,mailbox](std::stop_token stop){
                                try{winrt::init_apartment(winrt::apartment_type::multi_threaded);struct Apartment{~Apartment(){winrt::uninit_apartment();}} apartment;
                                    if(stop.stop_requested())throw std::runtime_error("Native dependency proof interrupted");
                                    tcard_sync::Result proof;const tcard_sync::PreparedNote note{tcard_dependencies::Read(task->delta,task->context).note,task->delta};
                                    task->okay=tcard_sync::WithDependencyCommit(storage,tcard_dependency_mail::Base(mailbox,L"incoming.json"),note,task->context,[&](const tcard_sync::DependencyCommitProof& proof){
                                        if(stop.stop_requested())return false;tcard_dependencies::Pool pool(tcard_dependency_mail::Base(mailbox,L"incoming.json"),note.package,task->context,false);
                                        if(!pool.Release(storage,proof))return false;task->proofGeneration=tcard_native::Selected(storage);return true;},proof);
                                }catch(...){task->okay=false;}task->done=true;
                            });}catch(...){task->done=true;task->proving=false;task->okay=true;throw;}return changed;
                        }
                        // Only metadata is checked on the UI thread after the bounded background byte proof.
                        if(task->proofGeneration!=tcard_native::Selected(storage)||!dependencyCap_||task->context.space!=dependencySpace_||task->context.group!=dependencyGroup_||
                            task->context.source!=dependencyNode_||task->context.receiver!=tcard_bootstrap::Node(storage)||task->context.group!=tcard_chunks::Group(storage)||
                            task->workerOwner!=tcard_ack::Owner(process_,pid_)||task->nativeOwner!=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId())){
                            task->activated=false;task->proving=false;task->started=false;task->retryAt=now+3000;receivedValid_=false;return changed;
                        }
                    }
                    if(!tcard_mail::Commit(storage,mailbox_,L"incoming-commit.json",task->hash,task->required,header)||
                        !tcard_ack::PublishInventory(mailbox_,storage,header,batchCap_,objectCap_,true,dependencyCap_))throw std::runtime_error("Native application receipt blocked");
                    received_=task->hash;receivedValid_=true;atomicWaiting_=false;exported_=0;
                }else{
                    if(!tcard_handoff::Publish(storage,mailbox_,task->hash,task->target,task->native.header,task->native.rewind))throw std::runtime_error("Native custody publication blocked");
                    receivedValid_=false;retryAt_=now+300;
                }
                receivedStamp_=task->stamp;receivedAt_=now;receiveTask_.reset();return changed;
            }catch(...){status=L"sync.apply_failed";if(!task->activated)task->started=false;task->retryAt=now+3000;return changed;}
        }
        tcard_sync::Result recovered;bool resumed=true;
        // Recover before each new exchange, rather than rescanning retained receipts for every note.
        if(!receiveTask_||!receiveTask_->started){
            resumed=tcard_sync::RetryChanges(storage,recovered);
            retryAt_=now+(resumed&&!recovered.pending?60000:3000);
            if(receiveTask_){receiveTask_->started=true;receiveTask_->failed=!resumed;receiveTask_->pending=recovered.pending!=0;}
        }
        const bool fresh=receiveTask_&&receiveTask_->hash!=received_;
        // Previously verified identical echoes add no operations; recovery still runs first.
        const bool echo=fresh&&!sent_.empty()&&receiveTask_->hash==sent_&&exported_&&
            GetTickCount64()-exported_<60000&&resumed&&!recovered.applied&&!recovered.pending;
        tcard_sync::Result result;bool imported=true;
        auto touched=refresh_;touched.insert(touched.end(),recovered.touched.begin(),recovered.touched.end());
        if(fresh&&!echo&&receiveTask_->next<receiveTask_->notes.size()){
            const auto& note=receiveTask_->notes[receiveTask_->next];
            // One independent native transaction per UI poll, retaining the complete prepared exchange.
            imported=receiveTask_->objects?tcard_blobs::Import(storage,tcard_mail::Objects(mailbox_,L"incoming.json"),note.package,result):tcard_sync::ImportPreparedNote(storage,note,result);
            if(imported||!result.touched.empty())touched.push_back(note.id);
            touched.insert(touched.end(),result.touched.begin(),result.touched.end());
        }
        std::vector<tcard::CardRecord> loaded;
        if(!touched.empty()&&!tcard::LoadCardSubset(storage,touched,loaded)){
            // Keep this note queued so a completed commit can still refresh after a temporary read failure.
            refresh_=std::move(touched);status=L"sync.apply_failed";retryAt_=now+3000;
            if(receiveTask_)receiveTask_->retryAt=retryAt_;return false;
        }
        refresh_.clear();bool changed=false;
        for(const auto& id:touched){
            const auto next=std::find_if(loaded.begin(),loaded.end(),[&](const auto& card){return card.id==id;});
            const auto old=std::find_if(cards.begin(),cards.end(),[&](const auto& card){return card.id==id;});
            if(next==loaded.end()){if(old!=cards.end()){if(previous)previous->push_back(*old);cards.erase(old);changed=true;}}
            else if(old==cards.end()){cards.push_back(*next);changed=true;}
            else{
                if(old->title!=next->title||old->source!=next->source||old->markdown!=next->markdown||
                    old->richHtml!=next->richHtml||tcard_sync::sync_display(*old)!=tcard_sync::sync_display(*next)){
                    if(previous)previous->push_back(*old);changed=true;
                }
                *old=*next;
            }
        }
        if(receiveTask_){
            receiveTask_->failed|=!imported;receiveTask_->pending|=result.pending!=0;
            if(fresh&&!echo&&receiveTask_->next<receiveTask_->notes.size())++receiveTask_->next;
            if(!fresh||echo||receiveTask_->next==receiveTask_->notes.size()){
                receivedStamp_=receiveTask_->stamp;receivedAt_=now;
                receivedValid_=!receiveTask_->failed&&!receiveTask_->pending;
                if(receivedValid_&&receiveTask_->batch){
                    std::wstring header;
                    if(!tcard_mail::Commit(storage,mailbox_,L"incoming-commit.json",receiveTask_->hash,receiveTask_->required,header)||
                        !tcard_ack::PublishInventory(mailbox_,storage,header,batchCap_,objectCap_,true,dependencyCap_)){
                        receivedValid_=false;receiveTask_->retryAt=now+3000;status=L"sync.apply_failed";return changed;
                    }
                }
                if(receivedValid_)received_=receiveTask_->hash;
                else{retryAt_=now+3000;status=receiveTask_->failed?L"sync.apply_failed":L"sync.pending";}
                receiveTask_.reset();
            }
        }
        if(!resumed||!imported)status=L"sync.apply_failed";else if(result.pending||recovered.pending)status=L"sync.pending";
        return changed;
    }
};
}
