#pragma once

// Included by the separate key service. Stage credentials only; the native owner activates.
std::wstring sync_stage_replacement(const std::wstring& profile,const std::wstring& native,const std::wstring& job)
{
    sync_profile(profile,false);
    using namespace tcard_source_job;Intent intent;ReadIntent(profile,job,intent);tcard_source_job::Snapshot source;tcard_crypto::Keys current,candidate;
    Require(!native.empty()&&!tcard_activation::Pending(profile)&&tcard_storage::LoadKey(native,current)&&tcard_storage::Matches(profile,current)&&tcard_ack::Identity(current)==intent.previous&&Inspect(profile,job,source)&&source.ready,"Replacement requires unchanged encrypted owner roots and complete acquired originals");
    Keys previous,imported;std::wstring error;Require(ReadCredentials(sync_storage(profile),previous.value,error)&&tcard_words::Decode(intent.words,imported.value),"Replacement credentials unavailable");
    tcard_crypto::Seed seed{};struct SeedScope{tcard_crypto::Seed& value;~SeedScope(){SecureZeroMemory(value.data(),value.size());}} seedScope{seed};
    auto old=tcard_words::Encode(previous.value);tcard_source_job::Wipe oldScope{old};tcard_crypto::Keys checked;
    Require(tcard_compat::DecodeSeed(old,seed)&&tcard_crypto::Derive(seed,checked)&&checked.publicKey==current.publicKey&&checked.dataKey==current.dataKey,"Previous private credentials differ from owner roots");
    SecureZeroMemory(seed.data(),seed.size());
    Require(tcard_compat::DecodeSeed(intent.words,seed)&&tcard_crypto::Derive(seed,candidate)&&tcard_ack::Identity(candidate)==source.group&&candidate.publicKey!=current.publicKey&&tcard_storage::Matches(source.path,candidate),"Imported replacement identity differs from acquired originals");
    std::wstring group;
    if(tcard_native::Exists(Control(profile)+L"\\"+job+L".credentials"))group=CheckCredentials(profile,intent,native,source);
    else{
        group=tcard_bridge::Token();const auto groups=profile+L"\\groups",storage=groups+L"\\"+group;
        Require(tcard_bridge::Protect(groups)&&tcard_bridge::Protect(storage)&&PersistCredentials(storage,imported.value,error),"Cannot stage replacement credentials");
    }
    Keys verified;Require(ReadCredentials(profile+L"\\groups\\"+group,verified.value,error)&&verified.value.transportKey==imported.value.transportKey&&verified.value.space==imported.value.space,"Staged replacement credentials differ");
    auto restored=tcard_words::Encode(verified.value);tcard_source_job::Wipe restoredScope{restored};SecureZeroMemory(seed.data(),seed.size());
    Require(tcard_compat::DecodeSeed(restored,seed)&&tcard_crypto::Derive(seed,checked)&&checked.publicKey==candidate.publicKey&&checked.dataKey==candidate.dataKey,"Staged replacement data identity differs");
    if(!tcard_native::Exists(Control(profile)+L"\\"+job+L".credentials")){
        auto receipt=Record(L"replace-source-credentials",intent);
        for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Native",sync_path_plain(native)},{L"CredentialGroup",group},{L"Group",source.group},{L"Node",source.node},{L"Target",tcard_bridge::Hash(source.target)}})receipt.Insert(name,JsonValue::CreateStringValue(value));
        Require(Put(profile,job+L".credentials",receipt),"Cannot retain replacement credential association");
    }
    Require(CheckCredentials(profile,intent,native,source)==group,"Replacement credential readback differs");return group;
}
