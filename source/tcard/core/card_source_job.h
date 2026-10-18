#pragma once
#include "card_source_snapshot.h"
#include "card_sync_ack.h"
#include "card_sync_network.h"

// Protected local acquisition control. No sockets, seed decoding or live owner mutation.
namespace tcard_source_job {
using namespace tcard_sync;
inline void Require(bool okay,const char* error){if(!okay)throw std::runtime_error(error);}
inline std::wstring Path(const std::wstring& profile){const auto path=sync_path_plain(profile);Require(!path.empty(),"Invalid acquisition profile");return path;}
inline std::wstring Control(const std::wstring& profile){return Path(profile)+L"-source-control";}
inline std::wstring Root(const std::wstring& profile,const std::wstring& job){Require(sync_hex(job),"Invalid acquisition job");return Path(profile)+L"-sources\\"+job;}
inline std::wstring Event(const std::wstring& profile,const std::wstring& request){Require(sync_hex(request),"Invalid acquisition request");return L"Local\\TCardSync.SourceStop."+tcard_bridge::Hash(Path(profile)+L"\n"+request);}
struct Wipe {std::wstring& text;~Wipe(){SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));}};
inline JsonObject Read(const std::wstring& profile,const std::wstring& leaf,std::wstring* digest=nullptr)
{
    std::wstring sealed,text;Wipe wipe{text};Require(tcard_bridge::Read(Control(profile),leaf,sealed)&&tcard_bridge::Seal(sealed,text,true),"Protected acquisition control unavailable");
    if(digest)*digest=tcard_bridge::Hash(text);return JsonObject::Parse(text);
}
inline bool Put(const std::wstring& profile,const std::wstring& leaf,const JsonObject& value)
{
    auto text=std::wstring(value.Stringify());Wipe wipe{text};std::wstring sealed,verified;
    return tcard_bridge::Seal(text,sealed)&&tcard_bridge::Publish(Control(profile),leaf,sealed)&&tcard_bridge::Read(Control(profile),leaf,verified)&&verified==sealed;
}
struct Intent {
    std::wstring profile,job,previous,endpoint,allow,deny,words,digest;bool remote=false;
    ~Intent(){SecureZeroMemory(words.data(),words.size()*sizeof(wchar_t));}
};
inline void ReadIntent(const std::wstring& profile,const std::wstring& job,Intent& intent)
{
    const auto path=Path(profile);Require(sync_hex(job),"Acquisition path/job differs");
    const auto record=Read(profile,job+L".intent",&intent.digest);
    Require(record.Size()==10&&record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Kind")==L"replace-source-intent"&&sync_string(record,L"Profile")==path&&sync_string(record,L"Job")==job,"Acquisition intent association differs");
    intent.profile=path;intent.job=job;intent.previous=sync_string(record,L"Previous");intent.endpoint=sync_string(record,L"Endpoint");
    intent.allow=sync_string(record,L"Allow");intent.deny=sync_string(record,L"Deny");intent.remote=record.GetNamedBoolean(L"Remote");intent.words=sync_string(record,L"Words");
    std::vector<tcard_net::Peer> peers;std::vector<tcard_net::Rule> allow,deny;
    Require(sync_hex(intent.previous)&&!intent.words.empty()&&intent.words.size()<=1024&&tcard_net::ParsePeers(intent.endpoint,0,peers)&&peers.size()==1&&intent.endpoint==peers[0].ip+L":"+std::to_wstring(peers[0].port)&&
        tcard_net::ParseRules(intent.allow,allow)&&tcard_net::ParseRules(intent.deny,deny)&&tcard_net::IsAllowed(peers[0].address,allow,deny),"Invalid acquisition credentials, endpoint or policy");
}
inline JsonObject Record(const wchar_t* kind,const Intent& intent)
{
    JsonObject record;record.Insert(L"Version",JsonValue::CreateNumberValue(1));
    for(const auto& [key,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",kind},{L"Profile",intent.profile},{L"Job",intent.job},{L"Intent",intent.digest}})record.Insert(key,JsonValue::CreateStringValue(value));return record;
}
inline void Check(const JsonObject& record,const wchar_t* kind,const Intent& intent,unsigned count)
{
    Require(record.Size()==count&&record.GetNamedNumber(L"Version")==1&&sync_string(record,L"Kind")==kind&&sync_string(record,L"Profile")==intent.profile&&sync_string(record,L"Job")==intent.job&&sync_string(record,L"Intent")==intent.digest,"Acquisition record association differs");
}
// A selector is only a protected resume reference, never permission to connect or activate.
inline std::wstring Pending(const std::wstring& profile)
{
    if(!tcard_native::Exists(Control(profile)+L"\\pending.json"))return L"";
    const auto record=Read(profile,L"pending.json");const auto job=sync_string(record,L"Job");Intent intent;ReadIntent(profile,job,intent);
    Check(record,L"replace-source-pending",intent,5);return job;
}
inline bool SelectPending(const std::wstring& profile,const Intent& intent){return Put(profile,L"pending.json",Record(L"replace-source-pending",intent))&&Pending(profile)==intent.job;}
struct Snapshot {std::wstring job,path,phase,node,target,group,error;bool ready=false;};
// Recompute originals offline. A worker's ready flag alone can never authorize replacement.
inline bool Inspect(const std::wstring& profile,const std::wstring& job,Snapshot& snapshot)
{
    snapshot={};try{
        Intent intent;ReadIntent(profile,job,intent);const auto prepared=Read(profile,job+L".prepared");Check(prepared,L"replace-source-prepared",intent,6);
        const auto group=sync_string(prepared,L"Group"),path=Root(profile,job);Require(sync_hex(group)&&group!=intent.previous&&tcard_chunks::Group(path)==group,"Acquisition data-key identity differs");
        SourceSnapshot source;std::wstring error;Require(InspectOwnerSource(path,source,error),"Acquisition saved-owner originals unavailable");
        const auto receipt=tcard_source::Read(path);Require(intent.endpoint==receipt.ip+L":"+std::to_wstring(receipt.port),"Acquisition endpoint differs");
        snapshot={job,path,source.ready?L"ready":L"acquiring",source.node,source.target,group,L"",source.ready};return true;
    }catch(const std::exception& e){const auto* error=e.what();snapshot.error.assign(error,error+strlen(error));return false;}
    catch(...){snapshot.error=L"Acquisition inspection failed";return false;}
}
}
