#pragma once
#include "card_native_commit.h"
#include "card_sync_handoff.h"
#include <stop_token>

namespace tcard_native {
struct ReceiveResult {std::wstring source,header;bool ready=false,rewind=false,applied=false;};
inline ReceiveResult Receive(const std::wstring& owner,const std::wstring& target,const std::wstring& delta,const std::wstring& objects,std::stop_token stop={},const tcard_dependencies::Context* dependency=nullptr)
{
    ReceiveResult result;{std::lock_guard guard(sync_mutex);SyncLock lock(Owner(owner));if(!lock)throw std::runtime_error("Native receiver busy");result.source=Resolve(owner);}
    const auto wanted=selection::Read(target);
    if(dependency){if(tcard_blobs::Canonical(dependency->target)!=tcard_blobs::Canonical(target))throw std::runtime_error("Dependency native target differs");
        if(selection::Missing(tcard_dependency_mail::Required(tcard_dependencies::Read(delta,*dependency)),wanted))throw std::runtime_error("Dependency native membership differs");}
    else selection::Validate(delta,wanted,true);
    result.header=tcard_stage::Inventory(owner);if(!selection::Missing(wanted,selection::Read(result.header))){result.applied=true;return result;}
    const auto source=result.source;
    bool fresh=tcard_stage::Unstarted(source);
    if(!fresh){
        const auto state=tcard_stage::Read(source,false);bool stale=state.remote!=tcard_blobs::Canonical(target);
        if(sync_exists(tcard_stage::Folder(source),L"native.json")){
            const auto capture=tcard_retained::Read(source);
            stale|=tcard_retained::Collect(source,true,stop)!=capture.files||selection::Read(tcard_stage::Inventory(source))!=selection::Read(capture.local);
        }
        if(stale){RetireStage(source);fresh=true;}
    }
    if(sync_exists(tcard_stage::Folder(source),L"activation.json")){ReadPlan(source);result.ready=true;return result;}
    tcard_stage::Begin(source,target);tcard_retained::Begin(source,stop);
    tcard_retained::Cursor cursor;while(!tcard_retained::Step(source,cursor,stop)){if(stop.stop_requested())throw std::runtime_error("Native copy interrupted");}
    if(stop.stop_requested())throw std::runtime_error("Native receive interrupted");
    if(!fresh){Result imported;if(!tcard_stage::Receive(source,delta,objects,imported,dependency))throw std::runtime_error("Native candidate import failed");result.rewind=imported.pending!=0;}
    else result.rewind=true;
    result.header=tcard_stage::Inventory(tcard_stage::Data(source));
    if(tcard_stage::Inspect(source).ready){Prepare(source);result.ready=true;result.rewind=false;}
    return result;
}
}
