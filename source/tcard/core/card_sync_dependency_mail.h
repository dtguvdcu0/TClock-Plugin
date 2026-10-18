#pragma once
#include "card_sync_dependency_pool.h"

// Fixed mailbox providers carry independently bound custody, never application acknowledgements.
namespace tcard_dependency_mail {
using namespace tcard_sync;
using tcard_dependencies::Context;
// Match the committed-copy wrapper's stable-owner order before entering provider/Pool APIs.
struct Guard {
    std::unique_lock<std::recursive_mutex> process{sync_mutex};
    std::vector<std::unique_ptr<SyncLock>> owners;
    Guard(const std::wstring& storage,const std::wstring& custody)
    {
        const auto base=tcard_native::Base(storage);const std::set<std::wstring> paths{sync_owner(base),tcard_chunks::budget::Owner(base),tcard_chunks::budget::Owner(custody)};
        for(const auto& path:paths){if(path.empty())throw std::runtime_error("Dependency mailbox owner unavailable");
            auto owner=std::make_unique<SyncLock>(path);if(!*owner)throw std::runtime_error("Dependency mailbox owner busy");owners.push_back(std::move(owner));}
    }
};
inline bool Referenced(const std::wstring& packet)
{
    return JsonObject::Parse(packet).HasKey(L"DependencyVersion");
}
inline selection::Inventory Required(const tcard_dependencies::Manifest& manifest)
{
    return tcard_dependencies::Required(manifest);
}
inline std::wstring Base(const std::wstring& mailbox,const std::wstring& leaf,bool create=false)
{
    if(leaf!=L"incoming.json"&&leaf!=L"outgoing.json")throw std::runtime_error("Unknown dependency mailbox direction");
    const auto base=mailbox+L"\\"+leaf.substr(0,leaf.size()-5)+L"-objects";
    if(create&&!tcard_bridge::Protect(base))throw std::runtime_error("Cannot protect dependency mailbox");return base;
}
inline selection::Inventory Decode(const std::wstring& packet,const Context& expected,std::wstring& delta)
{
    const auto root=JsonObject::Parse(packet);
    if(root.GetNamedNumber(L"BatchVersion")!=3||sync_string(root,L"Kind")!=L"mailbox-batch"||sync_string(root,L"Group")!=expected.group)
        throw std::runtime_error("Dependency mailbox capability or group differs");
    delta=std::wstring(root.GetNamedObject(L"Delta").Stringify());const auto manifest=tcard_dependencies::Read(delta,expected);
    const auto advertised=selection::Read(std::wstring(root.GetNamedObject(L"Inventory").Stringify())),required=tcard_dependencies::Required(manifest);
    if(selection::Missing(required,advertised)||selection::Missing(selection::Read(expected.target),advertised))
        throw std::runtime_error("Dependency mailbox exceeds captured inventory");return required;
}
inline Context Expected(const std::wstring& packet,const Context& channel,bool outgoing)
{
    auto expected=channel;const auto root=JsonObject::Parse(packet);
    if(outgoing){const auto boundary=root.GetNamedObject(L"SourceTarget");
        expected.target=std::wstring(boundary.GetNamedObject(L"Target").Stringify());
        if(boundary.GetNamedNumber(L"Version")!=1||sync_string(boundary,L"Direction")!=L"outgoing"||
            sync_string(boundary,L"Group")!=channel.group||sync_string(boundary,L"Source")!=channel.source||
            sync_string(boundary,L"TargetDigest")!=tcard_dependencies::Digest(expected.target))throw std::runtime_error("Dependency source boundary differs");
    }else{
        if(!root.GetNamedBoolean(L"NativeAtomic",false))throw std::runtime_error("Dependency native boundary absent");
        expected.target=std::wstring(root.GetNamedObject(L"Target").Stringify());
        if(sync_string(root,L"TargetDigest")!=tcard_dependencies::Digest(expected.target))throw std::runtime_error("Dependency native boundary differs");
    }
    tcard_dependencies::CheckContext(expected);return expected;
}
inline bool Current(const std::wstring& packet,const Context& channel)
{
    const auto manifest=JsonObject::Parse(packet).GetNamedObject(L"Delta");
    return sync_string(manifest,L"Space")==channel.space&&sync_string(manifest,L"Group")==channel.group&&
        sync_string(manifest,L"Source")==channel.source&&sync_string(manifest,L"Receiver")==channel.receiver;
}
inline void Stage(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const std::wstring& package,const Context& expected)
{
    Guard guard(storage,Base(mailbox,leaf));const auto manifest=tcard_dependencies::Read(package,expected);tcard_dependencies::SourceReader source(storage,package,expected);
    tcard_dependencies::Pool pool(Base(mailbox,leaf,true),package,expected);source.Bind(manifest);
    // Seal sequentially: complete objects release provisional slots while retaining their byte charge.
    for(const auto& descriptor:manifest.descriptors){const auto missing=pool.Missing(descriptor);
        if(!missing.empty()){auto bytes=tcard_dependencies::ReadVerified(source,descriptor);
            struct Wipe{tcard_dependencies::Bytes& bytes;~Wipe(){SecureZeroMemory(bytes.data(),bytes.size());}} wipe{bytes};
            for(const auto index:missing){const auto begin=index*tcard_chunks::chunkSize,end=(std::min)(bytes.size(),begin+tcard_chunks::chunkSize);
                pool.Put(descriptor,index,{bytes.begin()+begin,bytes.begin()+end});}}
        pool.Seal(descriptor);}
    if(!pool.Ready())throw std::runtime_error("Dependency mailbox proof incomplete");
}
inline bool Cleanup(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const PreparedNote& note,const Context& expected,Result& result)
{
    const auto base=Base(mailbox,leaf);
    return WithDependencyCommit(storage,base,note,expected,[&](const DependencyCommitProof& proof){
        tcard_dependencies::Pool pool(base,note.package,expected,false);return pool.Release(storage,proof);
    },result);
}
inline bool Prepare(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const std::wstring& package,const Context& expected,PreparedNote& note,Result& result)
{
    try{const auto manifest=tcard_dependencies::Read(package,expected);PreparedNote committed{manifest.note,package};Result proof;
        if(WithDependencyCommit(storage,Base(mailbox,leaf),committed,expected,[](const DependencyCommitProof&){return true;},proof)){note=std::move(committed);result={};return true;}
        Guard guard(storage,Base(mailbox,leaf));tcard_dependencies::Pool pool(Base(mailbox,leaf),package,expected);
        if(!pool.Ready())throw std::runtime_error("Dependency mailbox proof incomplete");
        return PrepareDependencyNote(storage,package,expected,pool,note,result);
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}
}
inline bool Import(const std::wstring& storage,const std::wstring& mailbox,const std::wstring& leaf,const PreparedNote& note,const Context& expected,Result& result)
{
    try{Result proof;if(WithDependencyCommit(storage,Base(mailbox,leaf),note,expected,[](const DependencyCommitProof&){return true;},proof)){result={};return true;}
        Guard guard(storage,Base(mailbox,leaf));tcard_dependencies::Pool pool(Base(mailbox,leaf),note.package,expected);
        return ImportDependencyNote(storage,note,expected,pool,result);
    }catch(const std::exception& error){result.error.assign(error.what(),error.what()+strlen(error.what()));return false;}
}
}
