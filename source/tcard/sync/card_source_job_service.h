#pragma once

// Included by the separate worker only. This service has no native bundle path.
int sync_source_job(const std::wstring& profile,const std::wstring& request)
{
    using namespace tcard_source_job;
    Require(sync_hex(request),"Invalid acquisition request");const auto launch=Read(profile,request+L".launch");
    const auto job=sync_string(launch,L"Job");Intent intent;ReadIntent(profile,job,intent);Check(launch,L"replace-source-launch",intent,7);
    const auto parentText=sync_string(launch,L"Parent");const auto colon=parentText.find(L':');
    Require(colon!=std::wstring::npos&&parentText.size()<=64,"Invalid acquisition parent");size_t used=0;const auto number=std::stoull(parentText.substr(0,colon),&used);
    Require(number&&number<=MAXDWORD&&used==colon,"Invalid acquisition parent PID");const auto parentPid=static_cast<DWORD>(number);
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentPid));
    Require(parent.value&&WaitForSingleObject(parent.value,0)==WAIT_TIMEOUT&&tcard_ack::Owner(parent.value,parentPid)==parentText&&sync_string(launch,L"Request")==request,"Acquisition parent differs");
    Handle stop(OpenEventW(SYNCHRONIZE,FALSE,Event(profile,request).c_str()));Require(stop.value!=nullptr,"Acquisition stop event unavailable");
    const auto worker=tcard_ack::Owner(GetCurrentProcess(),GetCurrentProcessId());const auto launchDigest=tcard_bridge::Hash(std::wstring(launch.Stringify()));
    auto publish=[&](const wchar_t* phase,const std::wstring& error=L""){
        auto status=Record(L"replace-source-status",intent);for(const auto& [key,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Request",request},{L"Launch",launchDigest},{L"Parent",parentText},{L"Worker",worker},{L"Phase",phase},{L"Error",error}})status.Insert(key,JsonValue::CreateStringValue(value));
        Require(Put(profile,request+L".status",status),"Cannot publish protected acquisition status");
    };
    std::stop_source cancellation;std::jthread watch([&](std::stop_token token){HANDLE waits[]={stop.value,parent.value};while(!token.stop_requested()){
        if(WaitForMultipleObjects(2,waits,FALSE,50)!=WAIT_TIMEOUT){cancellation.request_stop();break;}}});
    try{
        Handle owner(CreateMutexW(nullptr,TRUE,sync_event(profile,L"Owner").c_str()));const auto created=GetLastError();
        Require(owner.value&&created!=ERROR_ALREADY_EXISTS,"Acquisition requires a stopped synchronization worker");struct Release{HANDLE value;~Release(){ReleaseMutex(value);}} release{owner.value};
        Require(!tcard_activation::Pending(profile)&&tcard_storage::Required(profile)&&tcard_storage::Ready(profile)&&tcard_chunks::Group(profile)==intent.previous,"Acquisition previous group differs");
        Keys credentials;Require(tcard_words::Decode(intent.words,credentials.value),"Acquisition requires current 21-word credentials");tcard_crypto::Seed seed{};tcard_crypto::Keys keys;
        struct SeedScope{tcard_crypto::Seed& value;~SeedScope(){SecureZeroMemory(value.data(),value.size());}} seedScope{seed};
        Require(tcard_compat::DecodeSeed(intent.words,seed)&&tcard_crypto::Derive(seed,keys)&&tcard_ack::Identity(keys)!=intent.previous,"Acquisition requires a different current key");
        tcard_net::Policy policy;std::wstring defaults,error;std::vector<tcard_net::Peer> peers;
        Require(DiscoverLan(policy,defaults,error)&&tcard_net::ParseRules(intent.allow,policy.allow)&&tcard_net::ParseRules(intent.deny,policy.deny)&&tcard_net::ParsePeers(intent.endpoint,0,peers)&&peers.size()==1,"Acquisition policy unavailable");
        policy.remoteAdmission=intent.remote;Require(policy.Allows(peers[0].address),"Acquisition endpoint blocked by IP policy");
        if(cancellation.stop_requested()){publish(L"cancelled");return 2;}
        Require(tcard_bridge::Protect(profile+L"-sources"),"Acquisition parent unavailable");const auto root=Root(profile,job);Require(tcard_bridge::Protect(root),"Acquisition replica unavailable");
        std::string reason;
        if(!tcard_storage::Required(root)){
            WIN32_FIND_DATAW entry{};const auto search=FindFirstFileW((root+L"\\*").c_str(),&entry);Require(search!=INVALID_HANDLE_VALUE,"Cannot enumerate acquisition replica");
            bool empty=true;do{if(wcscmp(entry.cFileName,L".")&&wcscmp(entry.cFileName,L".."))empty=false;}while(FindNextFileW(search,&entry));const auto ended=GetLastError();FindClose(search);
            Require(empty&&ended==ERROR_NO_MORE_FILES,"Acquisition replica must start empty");Require(tcard_storage::Enable(root,keys,reason),"Cannot encrypt acquisition replica");
        }else Require((tcard_storage::Ready(root)||tcard_storage::Resume(root,reason))&&tcard_storage::Matches(root,keys),"Acquisition replica key differs");
        auto prepared=Record(L"replace-source-prepared",intent);prepared.Insert(L"Group",JsonValue::CreateStringValue(tcard_ack::Identity(keys)));
        if(tcard_native::Exists(Control(profile)+L"\\"+job+L".prepared"))Require(std::wstring(Read(profile,job+L".prepared").Stringify())==std::wstring(prepared.Stringify()),"Acquisition prepared identity changed");
        else Require(Put(profile,job+L".prepared",prepared),"Cannot retain acquisition key identity");
        publish(L"acquiring");SourceReport report;std::wstring previousError;
        for(;;){
            if(cancellation.stop_requested()){publish(L"cancelled");return 2;}
            Intent current;ReadIntent(profile,job,current);Require(current.digest==intent.digest,"Acquisition intent changed during transfer");
            if(!FetchOwnerSource(root,credentials.value,peers[0],policy,report,cancellation.get_token())){
                if(cancellation.stop_requested()){publish(L"cancelled");return 2;}publish(L"failed",report.error);return 1;
            }
            Require(report.session.sentOperations==0,"Acquisition attempted to upload local intent");
            if(previousError!=report.session.received.error){previousError=report.session.received.error;publish(L"acquiring",previousError);}
            if(report.snapshot.ready){tcard_source_job::Snapshot verified;Require(Inspect(profile,job,verified)&&verified.ready,"Acquisition completion lacks original verification");if(report.session.authenticated)try{sync_complete_source(profile,job,credentials.value,policy,cancellation.get_token());}catch(...){/* Retain completion intent; readiness still requires independently verified originals. */}publish(L"ready");return 0;}
            HANDLE waits[]={stop.value,parent.value};if(WaitForMultipleObjects(2,waits,FALSE,250)!=WAIT_TIMEOUT)cancellation.request_stop();
        }
    }catch(const std::exception& e){const auto* message=e.what();publish(L"failed",std::wstring(message,message+strlen(message)));return 1;}
    catch(...){publish(L"failed",L"Acquisition job failed");return 1;}
}
