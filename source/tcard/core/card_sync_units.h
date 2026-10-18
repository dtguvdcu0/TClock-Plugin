#pragma once
#include "card_sync_selection.h"

namespace tcard_sync {
inline size_t sync_measure_json(const std::wstring& text)
{
    if(text.size()>tcard_asset::max_file)return tcard_asset::max_file+1;
    // JSON content sizing only: UTF-16 to UTF-8, never a path or ACP conversion.
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!size&&!text.empty())throw std::runtime_error("Invalid selected JSON text");return static_cast<size_t>(size);
}
inline bool sync_select_note(const std::wstring& directory,const std::wstring& id,const std::vector<Change>& changes,
    const std::vector<std::wstring>& images,const selection::Inventory& peer,size_t limit,JsonObject& item,size_t& count,size_t& measured,bool& partial)
{
    JsonArray operations,assets;item.Insert(L"Id",JsonValue::CreateStringValue(id));item.Insert(L"Changes",operations);item.Insert(L"Assets",assets);
    size_t used=sync_measure_json(std::wstring(item.Stringify()));count=0;measured=0;partial=false;if(used>limit)return false;
    std::map<std::wstring,JsonObject> chosen,packedImages;
    std::set<std::wstring> available;
    for(auto it=peer.operations.lower_bound({id,L""});it!=peer.operations.end()&&it->first==id;++it)available.insert(it->second);
    auto packImage=[&](const std::wstring& name,JsonObject& object,size_t& size){
        std::vector<BYTE> bytes;if(!tcard_asset::read(sync_archive(directory,id),name,bytes)||tcard_asset::hash(bytes)!=name.substr(7,64))
            throw std::runtime_error("Missing archived image");
        object.Insert(L"Name",JsonValue::CreateStringValue(name));object.Insert(L"Bytes",JsonValue::CreateStringValue(L""));
        size=sync_measure_json(std::wstring(object.Stringify()))+bytes.size()*2;
        if(size>limit-used)return false;
        std::wstring hex;hex.reserve(bytes.size()*2);for(BYTE b:bytes){hex+=L"0123456789abcdef"[b>>4];hex+=L"0123456789abcdef"[b&15];}
        object.SetNamedValue(L"Bytes",JsonValue::CreateStringValue(hex));return true;
    };
    const bool repair=std::all_of(changes.begin(),changes.end(),[&](const auto& c){return peer.operations.contains({id,c.id});});
    std::vector<const Change*> order;for(const auto& c:changes)order.push_back(&c);
    std::sort(order.begin(),order.end(),[](const auto* a,const auto* b){return std::tie(a->time,a->actor,a->sequence,a->id)<std::tie(b->time,b->actor,b->sequence,b->id);});
    for(const auto* change:order){
        for(const auto& parent:change->parents)if(!available.contains(parent))throw std::runtime_error("Selected dependency order is incomplete");
        const auto operation=sync_pack(*change,true);size_t size=sync_measure_json(std::wstring(operation.Stringify()))+(chosen.empty()?0:1);
        std::set<std::wstring> needed;
        if(repair)needed.insert(images.begin(),images.end());
        else for(const auto* state:{&change->before,&change->after})for(const auto& name:sync_images(*state))
            if(!peer.assets.contains({id,name})&&!packedImages.contains(name))needed.insert(name);
        std::map<std::wstring,JsonObject> candidates;bool fit=size<=limit-used;
        for(const auto& name:needed){
            if(!fit)break;JsonObject object;size_t bytes=0;
            if(!packImage(name,object,bytes)||bytes+(packedImages.empty()&&candidates.empty()?0:1)>limit-used-size){fit=false;break;}
            size+=bytes+(packedImages.empty()&&candidates.empty()?0:1);candidates.emplace(name,std::move(object));
        }
        if(!fit){partial=true;break;}
        used+=size;chosen.emplace(change->id,operation);packedImages.insert(candidates.begin(),candidates.end());available.insert(change->id);
    }
    if(chosen.empty())return false;
    // Preserve existing selected bytes when the whole missing note fits, despite causal admission order.
    for(const auto& change:changes)if(chosen.contains(change.id))operations.Append(chosen.at(change.id));
    for(const auto& [name,object]:packedImages)assets.Append(object);
    count=chosen.size();measured=used; // The final envelope encoder verifies the publication budget.
    return true;
}
}
