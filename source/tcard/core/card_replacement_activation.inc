// Included inside tcard_activation's private namespace after the authenticated record helpers.
tcard_retained::Catalog key_read_catalog(const JsonObject& record)
{
    const auto rows=record.GetNamedArray(L"Files");key_require(rows.Size()<=tcard_retained::maxFiles,"Replacement catalog budget exceeded");tcard_retained::Catalog files;
    for(const auto& row:rows){const auto item=row.GetObject();const auto path=sync_string(item,L"Path"),hash=sync_string(item,L"Hash");const auto size=sync_number(item,L"Size");
        key_require(tcard_retained::Path(path)&&sync_hex(hash)&&size<=tcard_asset::max_file&&files.emplace(path,tcard_retained::Entry{hash,size}).second,"Invalid replacement catalog");}
    key_require(tcard_native::Digest(files)==sync_string(record,L"Catalog"),"Replacement catalog identity differs");return files;
}
void key_check_replace(const JsonObject& record)
{
    const auto value=record.GetNamedObject(L"Replacement");const auto before=sync_string(value,L"Before"),after=sync_string(value,L"After"),target=sync_string(value,L"Target");
    key_require((before.empty()||sync_hex(before))&&sync_hex(after)&&after!=before&&sync_hex(sync_string(value,L"SourceNode"))&&
        sync_string(value,L"TargetDigest")==tcard_bridge::Hash(target)&&record.GetNamedString(L"NativeScope",L"")==after,"Replacement activation binding differs");
    selection::Read(target);key_read_catalog(value);
}
JsonObject key_open_json(const std::wstring& root,const std::wstring& relative,State& keys){return JsonObject::Parse(tcard_asset::decode(key_open(root,relative,keys)));}
std::wstring key_read_native(const std::wstring& root,State& keys)
{
    const auto marker=key_open_json(root,L".tcard-native.required",keys),pointer=key_open_json(root,L"_native-generations\\active.json",keys);
    const auto selected=sync_string(pointer,L"Generation");key_require(marker.GetNamedNumber(L"Version")==1&&sync_string(marker,L"Kind")==L"native-selector"&&
        pointer.GetNamedNumber(L"Version")==1&&(selected.empty()||sync_hex(selected)),"Replacement native selector unavailable");return selected;
}
std::wstring key_read_scope(const std::wstring& root,State& keys)
{
    if(!key_exists(root+L"\\.tcard-native.required"))return {};
    const auto marker=key_open_json(root,L".tcard-native.required",keys);const auto scope=std::wstring(marker.GetNamedString(L"Scope",L""));
    key_require(marker.GetNamedNumber(L"Version")==1&&sync_string(marker,L"Kind")==L"native-selector"&&(scope.empty()||sync_hex(scope)),"Native scope marker invalid");return scope;
}
void key_verify_scope(const std::wstring& root,State& keys,const std::wstring& expected)
{
    if(expected.empty())return;key_require(key_read_scope(root,keys)==expected,"Required native replacement scope differs");const auto selected=key_read_native(root,keys);
    key_require(!selected.empty(),"Scoped native selection cannot return to the old public tree");
    const auto data=key_open_json(root,L"_native-generations\\"+selected+L"\\data\\.tcard-data.scope",keys);
    key_require(data.GetNamedNumber(L"Version")==1&&sync_string(data,L"Epoch")==expected,"Selected native replacement scope differs");
}
void key_check_complete(const JsonObject& record)
{
    const auto native=sync_string(record,L"Native"),profile=sync_string(record,L"Profile");State n,p;tcard_crypto::Keys expected;
    key_decode(sync_string(record,L"Public"),expected.publicKey);key_decode(sync_string(record,L"Data"),expected.dataKey);
    key_require(store_access_key(key_extend(native),n,false)&&store_access_key(key_extend(profile),p,false)&&n.keys.publicKey==expected.publicKey&&n.keys.dataKey==expected.dataKey&&p.keys.publicKey==expected.publicKey&&p.keys.dataKey==expected.dataKey,"Completed activation key binding differs");
    key_require(key_read_group(profile,p)==sync_string(record,L"Group"),"Completed activation group differs");
    key_verify_scope(native,n,std::wstring(record.GetNamedString(L"NativeScope",L"")));
}
void key_publish_json(const std::wstring& root,const std::wstring& relative,const JsonObject& record,State& keys)
{
    const auto plain=tcard_asset::encode(std::wstring(record.Stringify()));Bytes sealed;
    key_require(store_seal(key_extend(root),keys,plain,sealed)&&store_write_raw(key_extend(root+L"\\"+relative),sealed)&&key_open(root,relative,keys)==plain,"Cannot publish verified replacement selector");
}

void key_scan_generation(const std::wstring& root,const std::wstring& base,const std::wstring& relative,State& keys,tcard_retained::Catalog& files,size_t& visited)
{
    key_require(relative.empty()||tcard_retained::Path(relative),"Replacement generation depth exceeded");const auto prefix=base+(relative.empty()?L"":L"\\"+relative);
    tcard_asset::File directory(tcard_asset::open_dir(key_extend(root+L"\\"+prefix),false));key_require(bool(directory),"Unsafe replacement generation directory");
    WIN32_FIND_DATAW entry{};const auto find=FindFirstFileW(key_extend(root+L"\\"+prefix+L"\\*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE){key_require(GetLastError()==ERROR_FILE_NOT_FOUND,"Replacement generation enumeration failed");return;}
    struct Search{HANDLE value;~Search(){FindClose(value);}} search{find};do{const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        key_require(++visited<=2*tcard_retained::maxFiles&&tcard_asset::leaf(name)&&!(entry.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"Unsafe replacement generation entry");
        const auto path=relative.empty()?name:relative+L"\\"+name;key_require(tcard_retained::Path(path),"Invalid replacement generation path");
        if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)key_scan_generation(root,base,path,keys,files,visited);
        else{const auto bytes=key_open(root,base+L"\\"+path,keys);key_require(files.size()<tcard_retained::maxFiles&&files.emplace(path,tcard_retained::Entry{tcard_asset::hash(bytes),bytes.size()}).second,"Replacement generation catalog exceeded");}
    }while(FindNextFileW(find,&entry));key_require(GetLastError()==ERROR_NO_MORE_FILES,"Replacement generation enumeration interrupted");
}
void key_activate_replace(const std::wstring& native,const JsonObject& record,State& keys,const tcard_transition::Progress& progress)
{
    const auto value=record.GetNamedObject(L"Replacement");const auto after=sync_string(value,L"After");tcard_retained::Catalog actual;size_t visited=0;
    key_scan_generation(native,L"_native-generations\\"+after+L"\\data",L"",keys,actual,visited);key_require(actual==key_read_catalog(value),"Replacement generation changed during migration");
    const auto selected=key_read_native(native,keys);key_require(selected==sync_string(value,L"Before")||selected==after,"Native selection changed before replacement");
    if(selected!=after){JsonObject pointer;pointer.Insert(L"Version",JsonValue::CreateNumberValue(1));pointer.Insert(L"Generation",JsonValue::CreateStringValue(after));key_publish_json(native,L"_native-generations\\active.json",pointer,keys);}
    if(progress)progress(L"replacement-selector",0,0);
    auto scope=key_open_json(native,L".tcard-native.required",keys);scope.Insert(L"Scope",JsonValue::CreateStringValue(after));
    if(key_read_scope(native,keys)!=after)key_publish_json(native,L".tcard-native.required",scope,keys);
    key_verify_scope(native,keys,after);if(progress)progress(L"replacement-scope",0,0);
}
