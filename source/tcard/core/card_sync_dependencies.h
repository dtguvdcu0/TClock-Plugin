#pragma once
#include "card_sync_objects.h"
#include "card_sync_selection.h"
#include "card_native_root.h"

// Independent authenticated context comes from the transport/owner boundary, never the package.
namespace tcard_dependencies {
using namespace tcard_sync;
using Bytes=tcard_crypto::Bytes;
struct Context {
    std::wstring space,group,source,receiver,target;
    bool operator==(const Context&)const=default;
};
struct Descriptor {
    unsigned kind=0; // 1: immutable source; 2: image. Equal hashes do not erase this distinction.
    tcard_objects::Manifest object;
};
struct Manifest {
    Context context;std::wstring note,operation,originalHash,original;
    std::vector<Descriptor> descriptors;
};
inline std::wstring Digest(const std::wstring& value){return tcard_bridge::Hash(value);}
// Only the two known legacy reference limits may enter the separately negotiated dependency route.
inline bool LegacyFallback(const tcard_sync::Result& result)
{
    return result.error==L"Reference object budget exceeded"||result.error==L"Reference image budget exceeded";
}
inline void CheckContext(const Context& value)
{
    if(value.space.empty()||value.space.size()>128||!sync_hex(value.group)||!sync_hex(value.source)||!sync_hex(value.receiver))
        throw std::runtime_error("Dependency authenticated context unavailable");
    selection::Read(value.target);
}
inline JsonObject EncodeDescriptor(const Descriptor& value)
{
    auto object=tcard_objects::Encode(value.object);object.Insert(L"Kind",JsonValue::CreateNumberValue(value.kind));return object;
}
inline Descriptor ReadDescriptor(const JsonObject& value)
{
    const auto kind=tcard_chunks::Number(value,L"Kind",2);auto object=tcard_objects::Read(value);
    if(!kind||(kind==2)!=(object.name.starts_with(L"images/")))throw std::runtime_error("Dependency kind differs");
    return {static_cast<unsigned>(kind),std::move(object)};
}
inline std::wstring Key(const Descriptor& value){return std::to_wstring(value.kind)+L":"+value.object.name;}
inline const Descriptor& Find(const Manifest& value,unsigned kind,const std::wstring& name)
{
    const auto found=std::find_if(value.descriptors.begin(),value.descriptors.end(),[&](const auto& item){return item.kind==kind&&item.object.name==name;});
    if(found==value.descriptors.end())throw std::runtime_error("Missing bound dependency descriptor");return *found;
}
inline std::wstring Encode(const Manifest& value)
{
    CheckContext(value.context);JsonObject root;JsonArray objects;root.Insert(L"DependencyVersion",JsonValue::CreateNumberValue(1));
    root.Insert(L"Space",JsonValue::CreateStringValue(value.context.space));root.Insert(L"Group",JsonValue::CreateStringValue(value.context.group));
    root.Insert(L"Source",JsonValue::CreateStringValue(value.context.source));root.Insert(L"Receiver",JsonValue::CreateStringValue(value.context.receiver));
    root.Insert(L"TargetDigest",JsonValue::CreateStringValue(Digest(value.context.target)));root.Insert(L"Note",JsonValue::CreateStringValue(value.note));
    root.Insert(L"Operation",JsonValue::CreateStringValue(value.operation));root.Insert(L"OriginalHash",JsonValue::CreateStringValue(value.originalHash));
    root.Insert(L"Original",JsonObject::Parse(value.original));for(const auto& item:value.descriptors)objects.Append(EncodeDescriptor(item));root.Insert(L"Descriptors",objects);
    const auto text=std::wstring(root.Stringify());if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Dependency metadata budget exceeded");return text;
}
inline Manifest Read(const std::wstring& text,const Context& expected)
{
    CheckContext(expected);if(tcard_asset::encode(text).size()>tcard_asset::max_file-65536)throw std::runtime_error("Dependency metadata budget exceeded");
    const auto root=JsonObject::Parse(text);if(root.GetNamedNumber(L"DependencyVersion")!=1||sync_string(root,L"Space")!=expected.space||sync_string(root,L"Group")!=expected.group||
        sync_string(root,L"Source")!=expected.source||sync_string(root,L"Receiver")!=expected.receiver||sync_string(root,L"TargetDigest")!=Digest(expected.target))throw std::runtime_error("Dependency authenticated binding differs");
    Manifest value{expected,sync_string(root,L"Note"),sync_string(root,L"Operation"),sync_string(root,L"OriginalHash"),std::wstring(root.GetNamedObject(L"Original").Stringify()),{}};
    if(!tcard_asset::leaf(sync_stem(value.note))||sync_stem(value.note).starts_with(L"_")||!sync_hex(value.operation)||Digest(value.original)!=value.originalHash)throw std::runtime_error("Dependency original binding differs");
    const auto original=JsonObject::Parse(value.original);if(sync_string(original,L"Note")!=value.note||sync_string(original,L"Id")!=value.operation)throw std::runtime_error("Dependency original identity differs");
    const auto target=selection::Read(expected.target);if(!target.operations.contains({value.note,value.operation}))throw std::runtime_error("Dependency original outside captured target");
    const auto objects=root.GetNamedArray(L"Descriptors");if(objects.Size()>1026)throw std::runtime_error("Dependency descriptor budget exceeded");size_t total=0;std::set<std::wstring> found;
    for(const auto& item:objects){auto descriptor=ReadDescriptor(item.GetObject());if(!found.insert(Key(descriptor)).second||descriptor.object.size>128*1024*1024-total)throw std::runtime_error("Duplicate or excessive dependency descriptor");
        if(descriptor.kind==2&&!target.assets.contains({value.note,descriptor.object.name}))throw std::runtime_error("Dependency image outside captured target");total+=descriptor.object.size;value.descriptors.push_back(std::move(descriptor));}
    for(const auto* side:{L"Before",L"After"}){const auto state=original.GetNamedObject(side);if(state.HasKey(L"Source")||state.HasKey(L"Object"))throw std::runtime_error("Dependency original mixes inline source");Find(value,1,sync_string(state,L"SourceFile"));}
    return value;
}
// Logical membership only: callers authenticate/read the manifest first. This never proves durable application or ACK.
inline selection::Inventory Required(const Manifest& manifest)
{
    selection::Inventory result;result.operations.emplace(manifest.note,manifest.operation);
    for(const auto& descriptor:manifest.descriptors)if(descriptor.kind==2)result.assets.emplace(manifest.note,descriptor.object.name);
    return result;
}
class Reader {
public:
    virtual ~Reader()=default;
    // Bind the complete context/original/descriptor set, preventing cross-note custody substitution.
    virtual void Bind(const Manifest& manifest)const=0;
    virtual Bytes Read(const Descriptor& descriptor)=0;
};
inline Bytes ReadVerified(Reader& reader,const Descriptor& descriptor)
{
    auto bytes=reader.Read(descriptor);const auto actual=tcard_objects::Describe(descriptor.object.name,bytes);
    if(std::wstring(tcard_objects::Encode(actual).Stringify())!=std::wstring(tcard_objects::Encode(descriptor.object).Stringify()))throw std::runtime_error("Dependency provider bytes differ");return bytes;
}
inline void Verify(const Manifest& manifest,Reader& reader)
{
    reader.Bind(manifest);for(const auto& descriptor:manifest.descriptors){const auto bytes=ReadVerified(reader,descriptor);}
}
class SourceReader final:public Reader {
    std::wstring storage_;Manifest manifest_;
public:
    SourceReader(const std::wstring& storage,const std::wstring& manifest,const Context& expected):storage_(storage),manifest_(tcard_dependencies::Read(manifest,expected)){}
    void Bind(const Manifest& manifest)const override
    {
        if(Digest(Encode(manifest))!=Digest(Encode(manifest_)))throw std::runtime_error("Dependency provider manifest binding differs");
    }
    Bytes Read(const Descriptor& descriptor)override
    {
        std::lock_guard guard(sync_mutex);SyncLock lock(tcard_native::Base(storage_));if(!lock)throw std::runtime_error("Dependency source busy");
        const auto directory=tcard_native::Resolve(storage_);if(tcard_chunks::Group(directory)!=manifest_.context.group)throw std::runtime_error("Dependency source key differs");
        const auto& bound=Find(manifest_,descriptor.kind,descriptor.object.name);if(std::wstring(EncodeDescriptor(bound).Stringify())!=std::wstring(EncodeDescriptor(descriptor).Stringify()))throw std::runtime_error("Unbound dependency source read");
        Bytes bytes;if(!tcard_asset::read(sync_archive(directory,manifest_.note),descriptor.object.name,bytes)||bytes.size()!=descriptor.object.size||tcard_asset::hash(bytes)!=descriptor.object.hash)throw std::runtime_error("Dependency immutable source unavailable");return bytes;
    }
};
}
