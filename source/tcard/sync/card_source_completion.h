#pragma once

// Included by the worker only; completion never reads or uploads the old native owner.
bool sync_complete_source(const std::wstring& profile,const std::wstring& job,const Credentials& credentials,const tcard_net::Policy& policy,std::stop_token stop)
{
    using namespace tcard_source_job;Intent intent;ReadIntent(profile,job,intent);tcard_source_job::Snapshot source;
    Require(Inspect(profile,job,source)&&source.ready,"Source completion originals unavailable");
    auto record=Record(L"replace-source-retired",intent);
    for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Group",source.group},{L"Node",source.node},{L"Target",tcard_bridge::Hash(source.target)}})record.Insert(name,JsonValue::CreateStringValue(value));
    if(tcard_native::Exists(Control(profile)+L"\\"+job+L".retired")){
        const auto previous=Read(profile,job+L".retired");Check(previous,L"replace-source-retired",intent,8);
        Require(std::wstring(previous.Stringify())==std::wstring(record.Stringify()),"Source completion association differs");return true;
    }
    std::vector<tcard_net::Peer> peers;Require(tcard_net::ParsePeers(intent.endpoint,0,peers)&&peers.size()==1&&policy.Allows(peers[0].address),"Source completion endpoint blocked");
    SourceReport report;if(!AcknowledgeOwnerSource(source.path,credentials,peers[0],policy,report,stop))return false;
    Intent checked;ReadIntent(profile,job,checked);Require(checked.digest==intent.digest&&report.snapshot.node==source.node&&report.snapshot.target==source.target,"Source completion changed during acknowledgement");
    Require(Put(profile,job+L".retired",record),"Cannot retain source completion association");return true;
}
