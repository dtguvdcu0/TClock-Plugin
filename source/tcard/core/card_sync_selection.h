#pragma once
#include "card_sync_store.h"
#include "card_sync_objects.h"

namespace tcard_sync::selection {
using Objects=std::set<std::pair<std::wstring,std::wstring>>;
struct Inventory {
    Objects operations,assets;
    bool operator==(const Inventory&) const=default;
};
inline bool selection_alias(const std::wstring& a,const std::wstring& b)
{
    const auto left=sync_stem(a),right=sync_stem(b);
    return CompareStringOrdinal(left.c_str(),static_cast<int>(left.size()),right.c_str(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
inline Inventory Read(const std::wstring& header)
{
    if(tcard_asset::encode(header).size()>tcard_asset::max_file)throw std::runtime_error("Inventory budget exceeded");
    const auto root=JsonObject::Parse(header);if(root.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Unsupported inventory");
    const auto notes=root.GetNamedArray(L"Notes");if(notes.Size()>1024)throw std::runtime_error("Inventory note budget exceeded");
    Inventory result;std::set<std::wstring> names;
    for(unsigned n=0;n<notes.Size();++n){
        const auto note=notes.GetObjectAt(n);const auto id=sync_string(note,L"Id"),stem=sync_stem(id);
        if(!tcard_asset::leaf(stem)||stem[0]==L'_'||names.contains(id))throw std::runtime_error("Invalid inventory identity");
        for(const auto& other:names)if(selection_alias(other,id))throw std::runtime_error("Colliding inventory identities");names.insert(id);
        const auto changes=note.GetNamedArray(L"Changes"),assets=note.GetNamedArray(L"Assets");
        if(changes.Size()>10000||assets.Size()>1024)throw std::runtime_error("Inventory object budget exceeded");
        for(unsigned i=0;i<changes.Size();++i){const auto hash=std::wstring(changes.GetStringAt(i));
            if(!sync_hex(hash)||!result.operations.emplace(id,hash).second)throw std::runtime_error("Invalid inventory operation");}
        for(unsigned i=0;i<assets.Size();++i){const auto name=std::wstring(assets.GetStringAt(i));
            if(!tcard_asset::relative(name,true)||!result.assets.emplace(id,name).second)throw std::runtime_error("Invalid inventory asset");}
    }
    return result;
}
inline std::wstring Header(const std::wstring& package)
{
    const auto original=JsonObject::Parse(package);if(original.GetNamedNumber(L"Version")!=1)throw std::runtime_error("Invalid inventory source");
    JsonObject root;root.Insert(L"Version",JsonValue::CreateNumberValue(1));JsonArray notes;
    const auto source=original.GetNamedArray(L"Notes");
    for(unsigned n=0;n<source.Size();++n){
        const auto note=source.GetObjectAt(n);JsonObject item;item.Insert(L"Id",note.GetNamedValue(L"Id"));JsonArray hashes,names;
        const auto changes=note.GetNamedArray(L"Changes"),assets=note.GetNamedArray(L"Assets");
        for(unsigned i=0;i<changes.Size();++i)hashes.Append(changes.GetObjectAt(i).GetNamedValue(L"Id"));
        for(unsigned i=0;i<assets.Size();++i)names.Append(assets.GetObjectAt(i).GetNamedValue(L"Name"));
        item.Insert(L"Changes",hashes);item.Insert(L"Assets",names);notes.Append(item);
    }
    root.Insert(L"Notes",notes);auto header=std::wstring(root.Stringify());Read(header);return header;
}
inline std::wstring Choose(const std::wstring& package,const Inventory& peer)
{
    const auto root=JsonObject::Parse(package);JsonArray selected;const auto notes=root.GetNamedArray(L"Notes");
    for(unsigned n=0;n<notes.Size();++n){
        const auto note=notes.GetObjectAt(n);const auto id=sync_string(note,L"Id");JsonArray changes,assets;
        const auto operations=note.GetNamedArray(L"Changes"),images=note.GetNamedArray(L"Assets");
        for(unsigned i=0;i<operations.Size();++i){const auto op=operations.GetObjectAt(i);
            if(!peer.operations.contains({id,sync_string(op,L"Id")}))changes.Append(op);}
        for(unsigned i=0;i<images.Size();++i){const auto image=images.GetObjectAt(i);
            if(!peer.assets.contains({id,sync_string(image,L"Name")}))assets.Append(image);}
        // The native importer needs an operation anchor to stage an asset-only repair.
        if(assets.Size()&&!changes.Size()){
            if(!operations.Size())throw std::runtime_error("Asset inventory has no operation anchor");changes.Append(operations.GetAt(0));
        }
        if(!changes.Size()&&!assets.Size())continue;
        JsonObject item;item.Insert(L"Id",JsonValue::CreateStringValue(id));item.Insert(L"Changes",changes);item.Insert(L"Assets",assets);selected.Append(item);
    }
    JsonObject delta;delta.Insert(L"Version",JsonValue::CreateNumberValue(1));delta.Insert(L"Notes",selected);return std::wstring(delta.Stringify());
}
inline Inventory Validate(const std::wstring& package,const Inventory& advertised,bool objects=false)
{
    std::vector<PreparedNote> notes;Result result;
    if(objects&&tcard_objects::Referenced(package))tcard_objects::References(package);
    else if(!PrepareChanges(package,notes,result))throw std::runtime_error("Invalid selected delta");
    const auto actual=Read(Header(package));
    for(const auto& id:actual.operations)if(!advertised.operations.contains(id))throw std::runtime_error("Unadvertised operation");
    for(const auto& id:actual.assets)if(!advertised.assets.contains(id))throw std::runtime_error("Unadvertised asset");return actual;
}
inline size_t Missing(const Inventory& expected,const Inventory& actual)
{
    size_t missing=0;for(const auto& id:expected.operations)if(!actual.operations.contains(id))++missing;
    for(const auto& id:expected.assets)if(!actual.assets.contains(id))++missing;return missing;
}
inline void Include(Inventory& target,const Inventory& other)
{
    target.operations.insert(other.operations.begin(),other.operations.end());target.assets.insert(other.assets.begin(),other.assets.end());
}
inline std::wstring Envelope(const std::wstring& package,const Inventory& peer)
{
    JsonObject root;root.Insert(L"Inventory",JsonObject::Parse(Header(package)));root.Insert(L"Delta",JsonObject::Parse(Choose(package,peer)));
    return std::wstring(root.Stringify());
}
inline Inventory Unwrap(const std::wstring& envelope,std::wstring& delta,bool objects=false)
{
    const auto root=JsonObject::Parse(envelope);auto inventory=Read(std::wstring(root.GetNamedObject(L"Inventory").Stringify()));
    auto selected=std::wstring(root.GetNamedObject(L"Delta").Stringify());Validate(selected,inventory,objects);delta=std::move(selected);return inventory;
}
}
