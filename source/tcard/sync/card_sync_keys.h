#pragma once

// Same-user DPAPI control messages provision a portable key without plaintext files or argv.
int sync_keys(const std::wstring& profile,const std::wstring& token,const std::wstring& from)
{
    if(token.size()!=36||token.find_first_not_of(L"0123456789abcdefABCDEF-")!=std::wstring::npos)throw std::runtime_error("Invalid key request");
    const auto control=profile+L"-control";std::wstring sealed,plain;
    if(!tcard_bridge::Read(control,token+L".request",sealed)||!tcard_bridge::Seal(sealed,plain,true))throw std::runtime_error("Key control unavailable");
    struct Wipe{std::wstring& value;~Wipe(){SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} wipe{plain};
    JsonObject request;if(!JsonObject::TryParse(plain,request))throw std::runtime_error("Invalid key control");
    const auto action=sync_string(request,L"Action");auto words=sync_string(request,L"Words");Wipe wordScope{words};
    bool okay=false;std::wstring failure=L"sync.key_failed";Keys keys;std::wstring material;Wipe materialScope{material};
    try{
        Handle owner(CreateMutexW(nullptr,TRUE,sync_event(profile,L"Owner").c_str()));
        if(!owner.value||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Key update requires a stopped worker");
        struct Release{HANDLE value;~Release(){ReleaseMutex(value);}} release{owner.value};
        if(action==L"log-clear"){
            if(!tcard_log::Clear(profile))throw std::runtime_error("Cannot clear connection log");
        }else if(action==L"cache-clear"){
            if(!tcard_peer::Clear(profile))throw std::runtime_error("Cannot clear endpoint history");
        }else if(action==L"replacement"){
            if(!words.empty())throw std::runtime_error("Replacement credentials must come from its protected acquisition intent");
            material=sync_stage_replacement(profile,sync_string(request,L"Native"),sync_string(request,L"Job"));
        }else{
        if(tcard_activation::Pending(profile)){failure=L"sync.rotation_pending";throw std::runtime_error("Activation requires the native owner");}
        if((action==L"import"||action==L"match"||action==L"merge")&&!tcard_words::Decode(words,keys.value)){failure=L"sync.key_invalid";throw std::runtime_error("Invalid recovery words");}
        if(action!=L"import"&&action!=L"show"&&action!=L"new"&&action!=L"storage"&&action!=L"rotate"&&action!=L"match"&&action!=L"merge")throw std::runtime_error("Unknown key operation");
        if(action==L"storage"&&tcard_storage::Required(profile)&&!tcard_storage::Ready(profile)){
            std::string error;if(!tcard_storage::Resume(profile,error))throw std::runtime_error("Cannot resume private profile encryption");
        }
        const bool creating=!sync_exists(profile,L"sync-profile.json");
        if((action==L"rotate"||action==L"match"||action==L"merge")&&creating)throw std::runtime_error("Rotation cannot provision a missing profile");
        if(!tcard_bridge::Protect(profile))throw std::runtime_error("Private key profile unavailable");sync_profile(profile,creating);
        Keys previous;std::wstring error;
        if(!creating&&!ReadCredentials(sync_storage(profile),previous.value,error))throw std::runtime_error("Existing credentials unavailable");
        if(action==L"show"||action==L"storage"){
            if(!creating)keys.value=previous.value;
            else if(!from.empty()){sync_profile(from,false);if(!ReadCredentials(sync_storage(from),keys.value,error))throw std::runtime_error("Cannot read local provisioning key");}
            else if(!tcard_words::Create(keys.value))throw std::runtime_error("Cannot generate recovery key");
        }else if(action==L"new"&&!tcard_words::Create(keys.value))throw std::runtime_error("Cannot generate recovery key");
        if(action==L"match"){
            material=keys.value.transportKey==previous.value.transportKey?L"same":L"different";
        }else if(action==L"rotate"||(action==L"merge"&&keys.value.transportKey!=previous.value.transportKey)){
            const auto native=sync_string(request,L"Native");tcard_crypto::Keys current,candidate;tcard_crypto::Seed seed{};
            struct SeedScope{tcard_crypto::Seed& value;~SeedScope(){SecureZeroMemory(value.data(),value.size());}} seedScope{seed};
            if(creating||native.empty()||!tcard_storage::Required(profile)||!tcard_storage::Required(native)||
                !tcard_storage::LoadKey(native,current)||!tcard_storage::Matches(profile,current))throw std::runtime_error("Rotation requires matching encrypted owner roots");
            auto existing=tcard_words::Encode(previous.value);Wipe existingScope{existing};tcard_crypto::Keys original;
            if(!tcard_compat::DecodeSeed(existing,seed)||!tcard_crypto::Derive(seed,original)||original.publicKey!=current.publicKey||original.dataKey!=current.dataKey)throw std::runtime_error("Current private credentials differ from owner roots");
            SecureZeroMemory(seed.data(),seed.size());
            if(action==L"rotate"&&!tcard_words::Create(keys.value))throw std::runtime_error("Cannot generate recovery key");
            auto display=tcard_words::Encode(keys.value);Wipe displayScope{display};
            if(!tcard_compat::DecodeSeed(display,seed)||!tcard_crypto::Derive(seed,candidate))throw std::runtime_error("Cannot derive current rotation identity");
            const auto groups=profile+L"\\groups",group=tcard_bridge::Token(),storage=groups+L"\\"+group;
            if(!tcard_bridge::Protect(groups)||!tcard_bridge::Protect(storage)||!PersistCredentials(storage,keys.value,error))throw std::runtime_error("Cannot stage rotation credentials");
            Keys verified;tcard_crypto::Keys checked;
            if(!ReadCredentials(storage,verified.value,error)||verified.value.transportKey!=keys.value.transportKey||verified.value.space!=keys.value.space)throw std::runtime_error("Rotation credentials differ");
            auto restored=tcard_words::Encode(verified.value);Wipe restoredScope{restored};
            SecureZeroMemory(seed.data(),seed.size());
            if(!tcard_compat::DecodeSeed(restored,seed)||!tcard_crypto::Derive(seed,checked)||checked.publicKey!=candidate.publicKey||checked.dataKey!=candidate.dataKey)throw std::runtime_error("Rotation identity differs");
            tcard_activation::Status activation;
            if(!tcard_activation::Stage(native,profile,group,candidate,activation,action==L"merge"?tcard_activation::Intent::Merge:tcard_activation::Intent::Rotate)){failure=L"sync.rotation_pending";throw std::runtime_error("Cannot stage coordinated rotation");}
        }else if(creating){
            const auto storage=sync_storage(profile);if(!tcard_bridge::Protect(storage)||!PersistCredentials(storage,keys.value,error))throw std::runtime_error("Cannot initialize key");sync_mark(profile);
        }else if(keys.value.transportKey!=previous.value.transportKey){
            if(tcard_storage::Required(profile)){failure=L"sync.rotation_pending";throw std::runtime_error("Encrypted key rotation requires coordinated activation");}
            // Never mutate/delete the previous replica or its pending mailboxes. Commit one pointer last.
            const auto groups=profile+L"\\groups",group=tcard_bridge::Token(),storage=groups+L"\\"+group;
            if(!tcard_bridge::Protect(groups)||!tcard_bridge::Protect(storage)||!PersistCredentials(storage,keys.value,error))throw std::runtime_error("Cannot stage updated key");
            Keys verified;if(!ReadCredentials(storage,verified.value,error)||verified.value.transportKey!=keys.value.transportKey)throw std::runtime_error("Staged key verification failed");
            JsonObject active;active.Insert(L"Version",JsonValue::CreateNumberValue(1));active.Insert(L"Group",JsonValue::CreateStringValue(group));
            if(!tcard_bridge::Publish(profile,L"active.json",active.Stringify().c_str()))throw std::runtime_error("Cannot commit updated key");
        }
        if(action==L"storage"){
            auto display=tcard_words::Encode(keys.value);Wipe displayScope{display};tcard_crypto::Seed seed{};tcard_crypto::Keys derived;
            struct SeedScope{tcard_crypto::Seed& value;~SeedScope(){SecureZeroMemory(value.data(),value.size());}} seedScope{seed};
            if(!tcard_compat::DecodeSeed(display,seed)||!tcard_crypto::Derive(seed,derived))throw std::runtime_error("Encrypted storage requires a 21-word key");
            std::string storageError;if(!tcard_storage::Enable(profile,derived,storageError))throw std::runtime_error("Private profile encryption failed");
            // Same-user DPAPI response contains only the public identity and purpose-separated data key.
            const wchar_t hex[]=L"0123456789abcdef";
            auto append=[&](const auto& bytes){for(auto byte:bytes){material+=hex[byte>>4];material+=hex[byte&15];}};
            append(derived.publicKey);append(derived.dataKey);
        }
        }
        okay=true;
    }catch(...){okay=false;}
    JsonObject response;response.Insert(L"Pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));response.Insert(L"Ok",JsonValue::CreateBooleanValue(okay));
    response.Insert(L"Error",JsonValue::CreateStringValue(okay?L"":failure));
    if(okay){auto display=(action==L"storage"||action==L"match"||action==L"cache-clear"||action==L"log-clear"||action==L"replacement")?material:tcard_words::Encode(keys.value);Wipe displayScope{display};response.Insert(L"Words",JsonValue::CreateStringValue(display));response.Insert(L"Space",JsonValue::CreateStringValue(keys.value.space));}
    auto result=std::wstring(response.Stringify().c_str());Wipe resultScope{result};
    if(!tcard_bridge::Seal(result,sealed)||!tcard_bridge::Publish(control,token+L".response",sealed))throw std::runtime_error("Key response unavailable");return 0;
}
