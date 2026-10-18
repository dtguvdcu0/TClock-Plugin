#pragma once
#include "card_sync_ack.h"
#include "card_sync_bootstrap.h"

// A retained point-in-time owner proof, authenticated remotely only inside fresh v2 encrypted payloads.
namespace tcard_source_witness {
using namespace tcard_sync;
inline void Require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
inline bool Process(const std::wstring& value){const auto at=value.find(L':');return at!=std::wstring::npos&&at>0&&at+1<value.size()&&value.size()<=64&&value.find(L':',at+1)==std::wstring::npos&&value.find_first_not_of(L"0123456789:")==std::wstring::npos;}
// Reject content-transfer envelopes before any object/chunk receiver can stage originals.
inline void Control(const JsonObject& data)
{
    Require(data.Size()==4&&data.GetNamedNumber(L"Protocol")==2&&sync_string(data,L"Role")==L"replica"&&data.GetNamedNumber(L"Pending")==0,"Invalid saved source control envelope");
    (void)data.GetNamedObject(L"Data");
}
inline std::wstring Check(const std::wstring& text,const std::wstring& request,const std::wstring& source,const std::wstring& receiver,const std::wstring& group)
{
    const auto r=JsonObject::Parse(text);const auto target=sync_string(r,L"Target");
    Require(r.Size()==13&&r.GetNamedNumber(L"Version")==1&&sync_string(r,L"Kind")==L"saved-owner-source"&&sync_hex(request)&&sync_hex(source)&&sync_hex(receiver)&&sync_hex(group)&&
        sync_string(r,L"Request")==request&&sync_string(r,L"Source")==source&&sync_string(r,L"Receiver")==receiver&&sync_string(r,L"Group")==group&&
        Process(sync_string(r,L"Owner"))&&Process(sync_string(r,L"Worker"))&&sync_string(r,L"Owner")!=sync_string(r,L"Worker")&&sync_hex(sync_string(r,L"NativeNode"))&&
        sync_hex(sync_string(r,L"Challenge"))&&sync_hex(sync_string(r,L"Receipt"))&&!target.empty()&&sync_string(r,L"TargetDigest")==tcard_bridge::Hash(target),"Saved source witness binding differs");
    selection::Read(target);return target;
}
}
