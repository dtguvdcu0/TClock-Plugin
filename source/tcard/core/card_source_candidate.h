#pragma once
#include "card_source_job.h"
#include "card_replacement.h"
#include "card_key_activation.h"

namespace tcard_source_job {
inline std::wstring CandidateRoot(const std::wstring& profile,const std::wstring& job){return Root(profile,job)+L"-replacement";}
// Local reconstruction only. A completed candidate never selects native data or membership.
inline bool PrepareCandidate(const std::wstring& profile,const std::wstring& job,tcard_replacement::Candidate& result,const tcard_transition::Progress& progress={})
{
    result={};try{
        Intent intent;ReadIntent(profile,job,intent);Snapshot source;
        Require(!tcard_activation::Pending(profile)&&tcard_chunks::Group(profile)==intent.previous&&Inspect(profile,job,source)&&source.ready,"Replacement acquisition or previous membership unavailable");
        tcard_crypto::Keys keys;Require(tcard_storage::LoadKey(source.path,keys)&&tcard_ack::Identity(keys)==source.group,"Replacement source key unavailable");
        const auto candidate=CandidateRoot(profile,job);Require(tcard_bridge::Protect(candidate),"Replacement candidate unavailable");std::string reason;
        if(!tcard_storage::Required(candidate)){
            WIN32_FIND_DATAW item{};const auto find=FindFirstFileW((tcard_native::Extended(candidate)+L"\\*").c_str(),&item);Require(find!=INVALID_HANDLE_VALUE,"Cannot enumerate replacement candidate");
            struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};bool empty=true;
            do{if(wcscmp(item.cFileName,L".")&&wcscmp(item.cFileName,L".."))empty=false;}while(FindNextFileW(find,&item));
            Require(empty&&GetLastError()==ERROR_NO_MORE_FILES,"Replacement candidate must start empty");Require(tcard_storage::Enable(candidate,keys,reason),"Cannot encrypt replacement candidate");
        }else Require((tcard_storage::Ready(candidate)||tcard_storage::Resume(candidate,reason))&&tcard_storage::Matches(candidate,keys),"Replacement candidate key differs");
        if(!tcard_replacement::Build(source.path,candidate,keys,result,progress))return false;
        Require(result.ready&&result.node==source.node&&result.target==source.target,"Replacement candidate does not match acquired originals");
        return true;
    }catch(const std::exception& e){result.ready=false;result.error=e.what();return false;}catch(...){result.ready=false;result.error="Replacement candidate preparation failed";return false;}
}
// Worker-issued receipt binds an unselected credential namespace; it grants no activation authority.
inline std::wstring CheckCredentials(const std::wstring& profile,const Intent& intent,const std::wstring& native,const Snapshot& source)
{
    const auto record=Read(profile,intent.job+L".credentials");Check(record,L"replace-source-credentials",intent,10);
    const auto group=sync_string(record,L"CredentialGroup");
    GUID parsed{};Require(group.size()==36&&SUCCEEDED(CLSIDFromString((L"{"+group+L"}").c_str(),&parsed))&&sync_string(record,L"Native")==sync_path_plain(native)&&sync_string(record,L"Group")==source.group&&sync_string(record,L"Node")==source.node&&sync_string(record,L"Target")==tcard_bridge::Hash(source.target),"Replacement credential association differs");
    return group;
}
}
