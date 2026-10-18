// Included only by card_sync_transport.cpp; receipts originate at fresh v2 transfer boundaries.
namespace {
using sync_source_state=tcard_source::State;
void sync_source_require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::wstring sync_source_folder(const std::wstring& path){return path+L"\\_source-fetch";}
sync_source_state sync_source_read(const std::wstring& path){return tcard_source::Read(path);}
void sync_source_write(const sync_source_state& state)
{
    JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));root.Insert(L"Port",JsonValue::CreateNumberValue(state.port));
    for(const auto& [name,value]:std::vector<std::pair<std::wstring,std::wstring>>{{L"Kind",state.ownerRequest.empty()?L"source-fetch":L"owner-source-fetch"},{L"Root",state.path},
        {L"Owner",tcard_bootstrap::Node(state.path)},{L"Group",tcard_chunks::Group(state.path)},{L"IP",state.ip},{L"Source",state.node},
        {L"Target",state.target},{L"Digest",tcard_bridge::Hash(state.target)}})root.Insert(name,JsonValue::CreateStringValue(value));
    if(!state.ownerRequest.empty()){root.Insert(L"OwnerRequest",JsonValue::CreateStringValue(state.ownerRequest));root.Insert(L"OwnerProof",JsonValue::CreateStringValue(state.ownerProof));root.Insert(L"OwnerDigest",JsonValue::CreateStringValue(tcard_bridge::Hash(state.ownerProof)));}
    const auto text=std::wstring(root.Stringify());std::wstring verified;
    sync_source_require(tcard_bridge::Publish(sync_source_folder(state.path),L"state.json",text)&&
        tcard_bridge::Read(sync_source_folder(state.path),L"state.json",verified)&&verified==text,"Cannot retain authenticated source receipt");
}
void sync_source_empty(const std::wstring& path)
{
    tcard_asset::File checked(tcard_asset::open_dir(path,false));sync_source_require(bool(checked),"Unsafe source replica root");
    WIN32_FIND_DATAW entry{};const auto handle=FindFirstFileW((path+L"\\*").c_str(),&entry);
    sync_source_require(handle!=INVALID_HANDLE_VALUE,"Source replica enumeration failed");
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{handle};
    do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        sync_source_require(!(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
            (name==L".tcard-vault.key"||name==L".tcard-vault.required"),"Source fetch requires an empty dedicated encrypted replica");
    }while(FindNextFileW(handle,&entry));sync_source_require(GetLastError()==ERROR_NO_MORE_FILES,"Source replica enumeration interrupted");
}
SourceSnapshot sync_source_inspect(const sync_source_state& state){return tcard_source::Inspect(state);}
void sync_source_bind(sync_source_state& state,const session::Io& io)
{
    sync_source_require(io.selection&&io.targets&&io.objects&&!io.dependencies&&sync_hex(io.receiver),"Source requires authenticated target-capable v2 transport");
    if(state.node.empty()){state.node=io.receiver;sync_source_write(state);}
    else sync_source_require(state.node==io.receiver,"Specified source identity changed");
}
void sync_source_capture(sync_source_state& state,const std::wstring& package)
{
    const auto root=JsonObject::Parse(package),boundary=root.GetNamedObject(L"RemoteBootstrap");
    const auto header=tcard_blobs::Canonical(std::wstring(boundary.GetNamedObject(L"Target").Stringify()));
    const auto wanted=selection::Read(header);std::wstring delta;const auto envelope=selection::Unwrap(package,delta,true);
    sync_source_require(boundary.GetNamedNumber(L"Version")==1&&sync_string(boundary,L"Kind")==L"remote-bootstrap"&&
        sync_string(boundary,L"Source")==state.node&&sync_string(boundary,L"Receiver")==tcard_bootstrap::Node(state.path)&&
        sync_string(boundary,L"Group")==tcard_chunks::Group(state.path)&&sync_string(boundary,L"TargetDigest")==tcard_bridge::Hash(header)&&
        !selection::Missing(wanted,envelope),"Source target boundary differs");
    selection::Validate(delta,wanted,true);
    if(state.target.empty()){state.target=header;sync_source_write(state);}
    else sync_source_require(selection::Read(state.target)==wanted,"Pinned source target changed");
}
}

