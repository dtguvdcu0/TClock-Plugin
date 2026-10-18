#pragma once
#include "card_native_root.h"
#include "card_sync_bootstrap.h"
#include "card_source_witness.h"
#include <cmath>

namespace tcard_sync {
struct SourceSnapshot {std::wstring node,target;bool ready=false;};
}
// Local receipt verification has no socket, TLS, credential-service or worker dependency.
namespace tcard_source {
using namespace tcard_sync;
struct State {std::wstring path,node,target,ip;unsigned short port=0;std::wstring ownerRequest,ownerProof;};
inline void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline std::wstring Folder(const std::wstring& path){return path+L"\\_source-fetch";}
inline State Read(const std::wstring& path)
{
    Require(tcard_storage::Required(path)&&tcard_storage::Ready(path)&&sync_owner(path)==sync_path_plain(path),"Source replica encryption unavailable");
    std::wstring text;Require(tcard_bridge::Read(Folder(path),L"state.json",text),"Source receipt unavailable");
    const auto root=JsonObject::Parse(text);const auto rawPort=root.GetNamedNumber(L"Port");
    State state{sync_path_plain(path),sync_string(root,L"Source"),sync_string(root,L"Target"),sync_string(root,L"IP")};
    uint32_t address=0;
    Require(root.GetNamedNumber(L"Version")==1&&(sync_string(root,L"Kind")==L"source-fetch"||sync_string(root,L"Kind")==L"owner-source-fetch")&&
        sync_string(root,L"Root")==state.path&&sync_string(root,L"Owner")==tcard_bootstrap::Node(path)&&
        sync_string(root,L"Group")==tcard_chunks::Group(path)&&tcard_net::ParseAddress(state.ip,address)&&
        std::isfinite(rawPort)&&rawPort>=1&&rawPort<=65535&&floor(rawPort)==rawPort&&
        (state.node.empty()||sync_hex(state.node))&&sync_string(root,L"Digest")==tcard_bridge::Hash(state.target),"Source receipt binding differs");
    state.port=static_cast<unsigned short>(rawPort);
    if(sync_string(root,L"Kind")==L"owner-source-fetch"){
        state.ownerRequest=sync_string(root,L"OwnerRequest");state.ownerProof=sync_string(root,L"OwnerProof");Require(sync_hex(state.ownerRequest)&&sync_string(root,L"OwnerDigest")==tcard_bridge::Hash(state.ownerProof),"Saved owner source request/proof unavailable");
        if(!state.ownerProof.empty())Require(tcard_source_witness::Check(state.ownerProof,state.ownerRequest,state.node,tcard_bootstrap::Node(path),tcard_chunks::Group(path))==state.target,"Saved owner source receipt differs");
        else Require(state.target.empty(),"Saved owner source target lacks proof");
    }else Require(!root.HasKey(L"OwnerRequest")&&!root.HasKey(L"OwnerProof")&&!root.HasKey(L"OwnerDigest"),"Legacy source receipt cannot carry owner evidence");
    if(!state.target.empty()){Require(!state.node.empty(),"Source receipt lacks authentication");selection::Read(state.target);}
    return state;
}
inline SourceSnapshot Inspect(const State& state)
{
    if(!state.ownerRequest.empty()&&!state.target.empty())Require(!state.ownerProof.empty()&&tcard_source_witness::Check(state.ownerProof,state.ownerRequest,state.node,tcard_bootstrap::Node(state.path),tcard_chunks::Group(state.path))==state.target,"Saved source target lacks verified owner proof");
    std::wstring committed;Result result;unsigned version=0;
    Require(ExportCommittedInventory(state.path,committed,result,version),"Source committed originals unavailable");
    const auto actual=selection::Read(committed),wanted=state.target.empty()?selection::Inventory{}:selection::Read(state.target);
    Require(!selection::Missing(actual,wanted),"Source replica contains unrelated local intent");
    return {state.node,state.target,!state.target.empty()&&!selection::Missing(wanted,actual)};
}
}
namespace tcard_sync {
inline bool InspectOwnerSource(const std::wstring& path,SourceSnapshot& snapshot,std::wstring& error)
{
    snapshot={};error.clear();try{SyncLock lock(path);tcard_source::Require(bool(lock),"Source replica busy");const auto state=tcard_source::Read(path);
        tcard_source::Require(!state.ownerRequest.empty(),"Source receipt has no saved owner request");snapshot=tcard_source::Inspect(state);return true;
    }catch(const std::exception& e){const auto* message=e.what();error.assign(message,message+strlen(message));return false;}
    catch(...){error=L"Saved source receipt verification failed";return false;}
}
inline bool InspectSource(const std::wstring& path,SourceSnapshot& snapshot,std::wstring& error)
{
    snapshot={};error.clear();try{SyncLock lock(path);tcard_source::Require(bool(lock),"Source replica busy");snapshot=tcard_source::Inspect(tcard_source::Read(path));return true;}
    catch(const std::exception& e){const auto* message=e.what();error.assign(message,message+strlen(message));return false;}
    catch(...){error=L"Source receipt verification failed";return false;}
}
}
