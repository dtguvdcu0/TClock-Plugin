#pragma once
#include "card_sync.h"
#include "card_assets.h"
#include "card_rich.h"
#include <map>
#include <set>
#include <functional>
#include <tuple>
#include <cstring>

namespace tcard_sync {

inline std::wstring sync_hash(const std::wstring& text)
{
    return tcard_asset::hash(tcard_asset::encode(text));
}
inline std::wstring sync_identify(const Change& change)
{
    const bool modern=change.before.display.has_value()||change.after.display.has_value();
    const bool dated=change.before.dates.has_value()||change.after.dates.has_value();
    std::wstring bytes = dated?L"TCard.Change.v3":modern?L"TCard.Change.v2":L"TCard.Change.v1";
    const auto field = [&](const std::wstring& value) {
        bytes += std::to_wstring(value.size()) + L":" + value;
    };
    field(change.note); field(change.actor); field(std::to_wstring(change.sequence)); field(std::to_wstring(change.time));
    field(std::to_wstring(change.parents.size()));
    for (const auto& parent : change.parents) field(parent);
    for (const auto* state : {&change.before, &change.after}) {
        field(sync_hash(state->source)); field(state->title);
        field(std::to_wstring(state->format)); field(state->deleted ? L"1" : L"0");
        if(modern){field(state->display?L"1":L"0");if(state->display){const auto& d=*state->display;
            field(d.color);field(d.fontFamily);field(std::to_wstring(std::bit_cast<uint64_t>(d.fontSize)));
            field(d.inheritFontFamily?L"1":L"0");field(d.inheritFontSize?L"1":L"0");field(d.live?L"1":L"0");
            }}
        if(dated){field(state->dates?L"1":L"0");if(state->dates){
            field(std::to_wstring(state->dates->created));field(std::to_wstring(state->dates->updated));}}
    }
    return sync_hash(bytes);
}
inline std::vector<std::wstring> sync_units(const Snapshot& state)
{
    std::vector<std::wstring> result;
    if (state.format == 2) {
        const auto document = tcard_rich::parse(state.source);
        for (const auto& paragraph : document.paragraphs)
            result.push_back(tcard_rich::serialize(tcard_rich::Document{{paragraph}}));
    } else {
        size_t begin = 0;
        while (begin < state.source.size()) {
            const auto end = state.source.find(L'\n', begin);
            const auto length = end == std::wstring::npos ? state.source.size() - begin : end + 1 - begin;
            result.push_back(state.source.substr(begin, length)); begin += length;
        }
    }
    if (result.size() > 32768) throw std::runtime_error("Too many merge units");
    return result;
}
struct SyncNode {
    std::wstring id, text;
    std::set<std::wstring> lineage;
    bool visible = true;
};
struct SyncState {
    Snapshot value;
    std::vector<SyncNode> nodes;
};
struct SyncHunk { size_t start = 0, count = 0; std::vector<std::wstring> insert; };

inline std::vector<SyncHunk> sync_diff(const std::vector<std::wstring>& before, const std::vector<std::wstring>& after)
{
    size_t prefix = 0, suffix = 0;
    while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) ++prefix;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
        before[before.size()-suffix-1] == after[after.size()-suffix-1]) ++suffix;
    const size_t n = before.size()-prefix-suffix, m = after.size()-prefix-suffix;
    if (n && m && (n+1) > 4000000 / (m+1)) throw std::runtime_error("Merge diff budget exceeded");
    if (!n || !m) {
        if (!n && !m) return {};
        return {{prefix, n, {after.begin()+prefix, after.end()-suffix}}};
    }
    std::vector<unsigned> lengths((n+1)*(m+1));
    const auto at = [&](size_t i, size_t j) -> unsigned& { return lengths[i*(m+1)+j]; };
    for (size_t i=n;i-->0;) for (size_t j=m;j-->0;)
        at(i,j) = before[prefix+i] == after[prefix+j] ? 1+at(i+1,j+1) : (std::max)(at(i+1,j),at(i,j+1));
    std::vector<SyncHunk> hunks;
    size_t i=0,j=0;
    while (i<n || j<m) {
        if (i<n && j<m && before[prefix+i] == after[prefix+j]) { ++i; ++j; continue; }
        SyncHunk h{prefix+i,0,{}};
        while (i<n || j<m) {
            if (i<n && j<m && before[prefix+i] == after[prefix+j]) break;
            if (j<m && (i==n || at(i,j+1)>at(i+1,j))) h.insert.push_back(after[prefix+j++]);
            else { ++h.count; ++i; }
        }
        hunks.push_back(std::move(h));
    }
    return hunks;
}
inline bool sync_overlap(const std::set<std::wstring>& a, const std::set<std::wstring>& b)
{
    for (const auto& id : a) if (b.contains(id)) return true;
    return false;
}
inline bool sync_matches(const Snapshot& actual,const Snapshot& expected)
{
    auto projected=actual;if(!expected.display)projected.display.reset();if(!expected.dates)projected.dates.reset();return projected==expected;
}
inline void sync_apply_dates(Snapshot& state,const Snapshot& after)
{
    if(!after.dates)return;
    const auto created=state.dates?(std::min)(state.dates->created,after.dates->created):after.dates->created;
    state.dates=after.dates;state.dates->created=created;
}
inline void sync_apply(SyncState& current, const SyncState& base, const Change& change)
{
    const bool linear = current.value == base.value;
    sync_apply_dates(current.value,change.after);
    if(change.after.display){
        if(!current.value.display||!change.before.display)current.value.display=change.after.display;
        else{auto& value=*current.value.display;const auto& before=*change.before.display;const auto& after=*change.after.display;
            if(before.color!=after.color)value.color=after.color;
            if(before.fontFamily!=after.fontFamily)value.fontFamily=after.fontFamily;
            if(before.fontSize!=after.fontSize)value.fontSize=after.fontSize;
            if(before.inheritFontFamily!=after.inheritFontFamily)value.inheritFontFamily=after.inheritFontFamily;
            if(before.inheritFontSize!=after.inheritFontSize)value.inheritFontSize=after.inheritFontSize;
            if(before.live!=after.live)value.live=after.live;}
    }
    if (change.before.source == change.after.source && change.before.format == change.after.format) {
        if (change.before.title != change.after.title) current.value.title = change.after.title;
        if (change.before.deleted != change.after.deleted) current.value.deleted = change.after.deleted;
        return;
    }
    if (change.before.format != change.after.format || current.value.format != change.after.format) {
        current.nodes.clear(); current.value.format = change.after.format;
        size_t i=0;
        for (const auto& unit : sync_units(change.after)) {
            const auto id = change.id + L":" + std::to_wstring(i++);
            current.nodes.push_back({id,unit,{id},true});
        }
    } else {
        std::vector<SyncNode> baseNodes;
        std::vector<std::wstring> oldUnits;
        for (const auto& node : base.nodes) if (node.visible) { baseNodes.push_back(node); oldUnits.push_back(node.text); }
        std::vector<SyncHunk> hunks;
        for(const auto& h:sync_diff(oldUnits,sync_units(change.after))){
            // Adjacent independent line replacements must not become one conflict region.
            if(h.count==h.insert.size()&&h.count>1){
                for(size_t i=0;i<h.count;++i)hunks.push_back({h.start+i,1,{h.insert[i]}});
            }else hunks.push_back(h);
        }
        size_t number=0;
        for (const auto& h : hunks) {
            std::set<std::wstring> scope;
            for (size_t i=h.start;i<h.start+h.count;++i) scope.insert(baseNodes[i].lineage.begin(),baseNodes[i].lineage.end());
            const std::wstring anchor = h.start < baseNodes.size() ? baseNodes[h.start].id : L"";
            auto position = current.nodes.end();
            if (!anchor.empty()) position=std::find_if(current.nodes.begin(),current.nodes.end(),[&](const auto& n){return n.id==anchor;});
            if (!anchor.empty() && position==current.nodes.end()) throw std::runtime_error("Missing target lineage");
            const auto offset = static_cast<size_t>(position-current.nodes.begin());
            if (!scope.empty()) for (auto& node : current.nodes) if (sync_overlap(scope,node.lineage)) node.visible=false;
            std::vector<SyncNode> inserted;
            for (const auto& unit : h.insert) {
                const auto id=change.id+L":"+std::to_wstring(number++);
                auto lineage=scope; lineage.insert(id);
                inserted.push_back({id,unit,std::move(lineage),true});
            }
            current.nodes.insert(current.nodes.begin()+offset,inserted.begin(),inserted.end());
        }
    }
    current.value.source.clear();
    if (current.value.format == 2) {
        tcard_rich::Document document;
        for (const auto& node : current.nodes) if (node.visible) {
            const auto piece = tcard_rich::parse(node.text);
            document.paragraphs.insert(document.paragraphs.end(),piece.paragraphs.begin(),piece.paragraphs.end());
        }
        current.value.source=tcard_rich::serialize(document);
    } else for (const auto& node : current.nodes) if (node.visible) current.value.source+=node.text;
    if (linear) current.value.source = change.after.source;
    if (change.before.title != change.after.title) current.value.title=change.after.title;
    // A newer content edit can revive an older concurrent delete; an older edit cannot revive a later delete.
    current.value.deleted=change.after.deleted;
}
inline bool sync_check_dates(const Snapshot& state)
{
    return !state.dates||(state.dates->created&&state.dates->updated&&state.dates->created<=1000000000000000ULL&&state.dates->updated<=1000000000000000ULL);
}
inline bool sync_check_stub(const Change& change)
{
    // Legacy empty markers describe an initial deletion boundary; their conversion time is not a fresh delete.
    return change.parents.empty()&&change.before==Snapshot{}&&change.after.source.empty()&&change.after.format==0&&
        change.after.deleted&&change.after.title==L"Untitled card"&&!change.after.dates&&
        (!change.after.display||*change.after.display==Display{});
}
inline void sync_latest(SyncState& current,const Change& change)
{
    // Metadata-only snapshots retain newer content from another branch; no text diff or node rebuild is needed.
    if(change.before.source==change.after.source&&change.before.format==change.after.format){
        sync_apply(current,SyncState{},change);return;
    }
    // Independent baselines have no shared text lineage. Use the latest verified content and retain all originals.
    auto value=change.after;if(!value.display)value.display=current.value.display;
    value.dates=current.value.dates;sync_apply_dates(value,change.after);
    current.value=std::move(value);current.nodes.clear();
}
inline bool sync_merge(const std::vector<Change>& changes, Snapshot& merged, std::wstring& error, bool optimize = true)
{
    try {
        if (changes.empty() || changes.size()>10000) throw std::runtime_error("Merge operation budget exceeded");
        std::map<std::wstring,const Change*> graph;
        std::wstring note;
        for (const auto& c : changes) {
            if (!sync_check_dates(c.before)||!sync_check_dates(c.after)||c.id != sync_identify(c) || c.id.size()!=64 || c.time>1000000000000000ULL || c.after.format>2 || c.before.format>2 ||
                c.before.source.size()>4*1024*1024 || c.after.source.size()>4*1024*1024 ||
                (!note.empty() && note!=c.note)) throw std::runtime_error("Invalid change");
            note=c.note;
            if (!std::is_sorted(c.parents.begin(),c.parents.end()) || std::adjacent_find(c.parents.begin(),c.parents.end())!=c.parents.end())
                throw std::runtime_error("Invalid dependencies");
            graph.emplace(c.id,&c);
        }
        // Independent same-group histories use the latest verified snapshot and retain their complete operation union.
        size_t roots=0;for(const auto& [id,c]:graph)if(c->parents.empty())++roots;
        if(!roots)throw std::runtime_error("Missing creation baseline");
        std::map<std::wstring,std::vector<std::wstring>> children;
        for(const auto& [id,c]:graph)for(const auto& parent:c->parents){
            if(!graph.contains(parent))throw std::runtime_error("Missing dependency");
            if(graph.at(parent)->time>=c->time)throw std::runtime_error("Invalid causal order");
            children[parent].push_back(id);
        }
        SyncState initial;
        std::wstring boundary;
        if(optimize&&roots>1){
            // Fold a verified common join and shared tail in memory; preserve every original ID and parent on disk.
            std::wstring join;std::set<std::wstring> suffix;bool shared=true;
            for(const auto& [id,c]:graph)if(children[id].empty()){
                const Change* cursor=c;
                while(cursor->parents.size()==1){
                    // A shared tail was already checked from another head; traverse each retained node only once.
                    if(!suffix.insert(cursor->id).second){cursor=graph.at(join);break;}
                    cursor=graph.at(cursor->parents[0]);
                }
                if(cursor->parents.size()<2||(!join.empty()&&join!=cursor->id)){shared=false;break;}
                join=cursor->id;
            }
            if(shared&&!join.empty()&&!suffix.empty()&&graph.size()-suffix.size()<=512){
                std::vector<Change> prefix;
                for(const auto& [id,c]:graph)if(!suffix.contains(id))prefix.push_back(*c);
                Snapshot state;
                if(!sync_merge(prefix,state,error,false))return false;
                initial.value=std::move(state);boundary=join;
                for(const auto& c:prefix)graph.erase(c.id);
            }
        }
        if(optimize&&(roots==1||!boundary.empty())){
            const Change* cursor=nullptr;
            if(boundary.empty()){for(const auto& [id,c]:graph)if(c->parents.empty())cursor=c;}
            else if(children[boundary].size()==1)cursor=graph.at(children[boundary][0]);
            std::vector<std::wstring> trunk;
            Snapshot state=initial.value;
            while(cursor){
                if(!sync_matches(state,cursor->before))throw std::runtime_error("Base snapshot mismatch");
                const auto retainedDisplay=state.display;const auto retainedDates=state.dates;
                state=cursor->after;if(!state.display)state.display=retainedDisplay;
                state.dates=retainedDates;sync_apply_dates(state,cursor->after);trunk.push_back(cursor->id);
                const auto& next=children[cursor->id];
                if(next.size()!=1||graph.at(next[0])->parents.size()!=1)break;
                cursor=graph.at(next[0]);
            }
            if(trunk.size()==graph.size()){merged=state;error.clear();return true;}
            // Every retained branch shares this verified trunk. Fold only in memory; keep original IDs on disk.
            if(!trunk.empty())boundary=trunk.back();
            for(const auto& id:trunk){
                const auto base=initial;if(roots>1)sync_latest(initial,*graph.at(id));else sync_apply(initial,base,*graph.at(id));
                std::erase_if(initial.nodes,[](const auto& node){return !node.visible;});
                // Keep shared lineage (including split/join ancestry); unique old tokens are represented by the node's own ID.
                std::map<std::wstring,size_t> uses;
                for(const auto& node:initial.nodes)for(const auto& token:node.lineage)++uses[token];
                for(auto& node:initial.nodes)std::erase_if(node.lineage,[&](const auto& token){return token!=node.id&&uses.at(token)==1;});
                if(initial.nodes.size()>65536)throw std::runtime_error("Merge output budget exceeded");
            }
            for(const auto& id:trunk)graph.erase(id);
        }
        if(graph.size()>512)throw std::runtime_error("Merge branch budget exceeded");
        std::map<std::wstring,std::set<std::wstring>> ancestors;
        std::set<std::wstring> visiting;
        std::function<const std::set<std::wstring>&(const std::wstring&)> collect = [&](const std::wstring& id) -> const std::set<std::wstring>& {
            if (ancestors.contains(id)) return ancestors.at(id);
            if (!graph.contains(id)) throw std::runtime_error("Missing dependency");
            if (!visiting.insert(id).second) throw std::runtime_error("Cyclic dependency");
            std::set<std::wstring> set;
            for (const auto& parent : graph.at(id)->parents) {
                if(parent==boundary)continue;
                const auto& inherited=collect(parent); set.insert(inherited.begin(),inherited.end()); set.insert(parent);
                if (graph.at(parent)->time>=graph.at(id)->time) throw std::runtime_error("Invalid causal order");
            }
            visiting.erase(id); return ancestors.emplace(id,std::move(set)).first->second;
        };
        for (const auto& [id,c] : graph) collect(id);
        std::map<std::set<std::wstring>,SyncState> cache;
        std::function<SyncState(const std::set<std::wstring>&)> replay = [&](const std::set<std::wstring>& selected) {
            if (cache.contains(selected)) return cache.at(selected);
            std::vector<const Change*> order;
            for (const auto& id : selected) order.push_back(graph.at(id));
            size_t selectedRoots=0;for(const auto* c:order)if(c->parents.empty())++selectedRoots;
            const bool split=roots>1&&selectedRoots!=1;
            std::sort(order.begin(),order.end(),[split](const auto* a,const auto* b){
                if(split&&sync_check_stub(*a)!=sync_check_stub(*b))return sync_check_stub(*a);
                return std::tie(a->time,a->actor,a->sequence,a->id)<std::tie(b->time,b->actor,b->sequence,b->id);
            });
            SyncState state=initial;
            for (const auto* c : order) {
                const auto base=replay(ancestors.at(c->id));
                if (!sync_matches(base.value,c->before)) throw std::runtime_error("Base snapshot mismatch");
                if(split)sync_latest(state,*c);else sync_apply(state,base,*c);
                if (state.nodes.size()>65536 || state.value.source.size()>4*1024*1024) throw std::runtime_error("Merge output budget exceeded");
            }
            cache.emplace(selected,state); return state;
        };
        std::set<std::wstring> all;
        for (const auto& [id,c] : graph) all.insert(id);
        merged=replay(all).value; error.clear(); return true;
    } catch (const std::exception& e) {
        error.assign(e.what(),e.what()+strlen(e.what())); return false;
    } catch (...) { error=L"Merge failed"; return false; }
}

}
