#pragma once
#include "card_rich.h"
#include "card_download.h"
#include <set>

namespace tcard_web {
using Attributes = std::map<std::wstring,std::wstring>;
struct Node {
    std::wstring tag,text;
    Attributes attrs,css;
    size_t parent=0;
    std::vector<size_t> children;
};
struct Tree { std::vector<Node> nodes{Node{}}; };
inline bool void_tag(const std::wstring& tag)
{
    return tag==L"img"||tag==L"br"||tag==L"hr"||tag==L"input"||tag==L"meta"||
        tag==L"link"||tag==L"source"||tag==L"wbr"||tag==L"area"||tag==L"base"||tag==L"embed";
}
inline Tree parse(std::wstring_view html)
{
    Tree tree;std::vector<size_t> stack{0};
    const auto lower=tcard_rich::lower(std::wstring(html));
    for(size_t at=0;at<html.size()&&tree.nodes.size()<24000;){
        if(html.substr(at,4)==L"<!--"){
            auto end=html.find(L"-->",at+4);at=end==html.npos?html.size():end+3;continue;
        }
        if(html[at]!=L'<'){
            auto end=html.find(L'<',at);if(end==html.npos)end=html.size();
            Node node;node.text=std::wstring(html.substr(at,end-at));node.parent=stack.back();
            tree.nodes[stack.back()].children.push_back(tree.nodes.size());tree.nodes.push_back(std::move(node));at=end;continue;
        }
        size_t end=at+1;wchar_t quote=0;
        for(;end<html.size();++end){
            wchar_t c=html[end];if(quote){if(c==quote)quote=0;}
            else if(c==L'"'||c==L'\'')quote=c;else if(c==L'>')break;
        }
        if(end==html.size())break;
        auto raw=tcard_rich::trim(std::wstring(html.substr(at+1,end-at-1)));at=end+1;
        bool closing=!raw.empty()&&raw[0]==L'/';if(closing)raw.erase(0,1);
        auto space=raw.find_first_of(L" \r\n\t/");auto tag=tcard_rich::lower(raw.substr(0,space));
        if(tag.empty()||tag[0]==L'!')continue;
        if(closing){
            for(size_t n=stack.size();n>1;--n)if(tree.nodes[stack[n-1]].tag==tag){stack.resize(n-1);break;}
            continue;
        }
        if(stack.size()>=96)continue;
        Node node;node.tag=tag;node.parent=stack.back();
        if(space!=raw.npos)node.attrs=tcard_rich::attributes(std::wstring_view(raw).substr(space));
        size_t index=tree.nodes.size();tree.nodes[stack.back()].children.push_back(index);tree.nodes.push_back(std::move(node));
        if(tag==L"script"||tag==L"style"||tag==L"textarea"){
            auto close=lower.find(L"</"+tag,at);if(close==lower.npos)close=html.size();
            Node text;text.text=std::wstring(html.substr(at,close-at));text.parent=index;
            tree.nodes[index].children.push_back(tree.nodes.size());tree.nodes.push_back(std::move(text));
            at=close;continue;
        }
        if(!void_tag(tag)&&!raw.ends_with(L"/"))stack.push_back(index);
    }
    return tree;
}
inline bool token(const std::wstring& value,const std::wstring& word)
{
    size_t at=0;
    while(at<value.size()){
        at=value.find_first_not_of(L" \t\r\n",at);if(at==value.npos)break;
        auto end=value.find_first_of(L" \t\r\n",at);
        if(value.substr(at,end==value.npos?end:end-at)==word)return true;
        if(end==value.npos)break;at=end+1;
    }
    return false;
}
struct Rule { std::wstring selector;Attributes values;int specificity=0; };
inline Attributes declarations(std::wstring css)
{
    Attributes result;
    for(size_t at=0;at<css.size();){
        auto end=css.find(L';',at);if(end==css.npos)end=css.size();
        auto colon=css.find(L':',at);
        if(colon<end){
            auto key=tcard_rich::lower(tcard_rich::trim(css.substr(at,colon-at)));
            auto value=tcard_rich::trim(css.substr(colon+1,end-colon-1));
            if(key.size()<64&&value.size()<512)result[key]=value;
        }
        at=end+1;
    }
    return result;
}
inline std::vector<Rule> rules(std::wstring css)
{
    for(size_t at=0;(at=css.find(L"/*",at))!=css.npos;){
        auto end=css.find(L"*/",at+2);css.erase(at,end==css.npos?css.size()-at:end+2-at);
    }
    std::vector<Rule> result;
    for(size_t at=0;at<css.size()&&result.size()<12000;){
        auto open=css.find(L'{',at);if(open==css.npos)break;
        size_t end=open+1;int depth=1;
        while(end<css.size()&&depth){if(css[end]==L'{')++depth;else if(css[end]==L'}')--depth;++end;}
        auto selectors=tcard_rich::trim(css.substr(at,open-at));
        if(!selectors.empty()&&selectors[0]!=L'@'&&selectors.size()<1024){
            auto values=declarations(css.substr(open+1,end-open-2));
            for(size_t start=0;start<selectors.size();){
                auto comma=selectors.find(L',',start);
                auto selector=tcard_rich::trim(selectors.substr(start,comma==selectors.npos?comma:comma-start));
                if(selector.find_first_of(L":[]+~\\")==selector.npos){
                    int score=0;for(wchar_t c:selector)score+=c==L'#'?100:c==L'.'?10:0;
                    if(!selector.empty())result.push_back({selector,values,score+1});
                }
                if(comma==selectors.npos)break;start=comma+1;
            }
        }
        at=end;
    }
    return result;
}
inline bool simple(const Node& node,std::wstring_view selector)
{
    size_t at=0;
    while(at<selector.size()){
        wchar_t kind=selector[at];if(kind==L'.'||kind==L'#')++at;else kind=0;
        auto end=selector.find_first_of(L".#",at);if(end==selector.npos)end=selector.size();
        auto name=std::wstring(selector.substr(at,end-at));if(name.empty())return false;
        if(kind==L'.'){
            auto found=node.attrs.find(L"class");if(found==node.attrs.end()||!token(found->second,name))return false;
        }else if(kind==L'#'){
            auto found=node.attrs.find(L"id");if(found==node.attrs.end()||found->second!=name)return false;
        }else if(name!=L"*"&&node.tag!=tcard_rich::lower(name))return false;
        at=end;
    }
    return true;
}
inline bool matches(const Tree& tree,size_t index,std::wstring selector)
{
    selector=tcard_rich::trim(selector);
    const auto end=selector.find_last_of(L" \t\r\n>");
    const auto part=end==selector.npos?selector:selector.substr(end+1);
    if(part.empty()||!simple(tree.nodes[index],part))return false;
    if(end==selector.npos)return true;
    auto rest=tcard_rich::trim(selector.substr(0,end+1));
    bool child=!rest.empty()&&rest.back()==L'>';if(child){rest.pop_back();rest=tcard_rich::trim(rest);}
    for(size_t parent=tree.nodes[index].parent;parent;parent=tree.nodes[parent].parent){
        if(matches(tree,parent,rest))return true;if(child)break;
    }
    return false;
}
inline void cascade(Tree& tree,const std::vector<Rule>& ruleset)
{
    size_t comparisons=0;
    for(size_t i=1;i<tree.nodes.size();++i){
        auto& node=tree.nodes[i];if(node.tag.empty())continue;
        std::map<std::wstring,int> priorities;
        auto apply=[&](const Attributes& values,int rank){
            for(const auto& [key,raw]:values){
                auto value=raw;int priority=rank;
                auto important=tcard_rich::lower(value).find(L"!important");
                if(important!=value.npos){value=tcard_rich::trim(value.substr(0,important));priority+=1000000;}
                if(!priorities.contains(key)||priority>=priorities[key]){node.css[key]=value;priorities[key]=priority;}
            }
        };
        for(const auto& rule:ruleset){
            if(++comparisons>4000000)break;
            if(matches(tree,i,rule.selector))apply(rule.values,rule.specificity);
        }
        if(node.attrs.contains(L"style"))apply(declarations(node.attrs.at(L"style")),10000);
    }
}
inline std::wstring value(const Node& node,const wchar_t* property)
{
    const auto found=node.css.find(property);return found==node.css.end()?L"":tcard_rich::lower(found->second);
}
inline std::wstring style(const Node& node)
{
    static const std::set<std::wstring> accepted={L"color",L"font-size",L"font-weight",L"font-style",
        L"text-decoration",L"text-decoration-line",L"white-space",L"text-align",L"width",L"height",L"max-width",L"max-height"};
    std::wstring result;
    for(const auto& [key,item]:node.css){
        const auto lower=tcard_rich::lower(item);
        if(accepted.contains(key)&&lower.find(L"url(")==lower.npos&&lower.find(L"var(")==lower.npos&&
            lower.find(L"expression")==lower.npos&&item.find_first_of(L"<>\"'\\")==item.npos)
            result+=key+L":"+item+L";";
    }
    return result;
}
inline std::wstring render(const Tree& tree,size_t index,bool cell=false)
{
    const auto& node=tree.nodes[index];
    if(node.tag.empty()&&index)return node.text;
    const auto& tag=node.tag;
    if(tag==L"script"||tag==L"style"||tag==L"head"||tag==L"iframe"||tag==L"object"||tag==L"template"||
        tag==L"svg"||tag==L"noscript"||tag==L"link"||tag==L"meta"||node.attrs.contains(L"hidden")||
        value(node,L"display")==L"none"||value(node,L"visibility")==L"hidden")return {};
    if(tag==L"img"){
        std::wstring result=L"<img";
        for(const wchar_t* key:{L"src",L"alt",L"width",L"height"})
            if(node.attrs.contains(key))result+=L" "+std::wstring(key)+L"=\""+tcard_rich::escape(node.attrs.at(key))+L"\"";
        if(!node.attrs.contains(L"src")&&node.attrs.contains(L"data-src"))
            result+=L" src=\""+tcard_rich::escape(node.attrs.at(L"data-src"))+L"\"";
        return result+L" style=\""+tcard_rich::escape(style(node))+L"\">";
    }
    const auto display=value(node,L"display");
    const bool row=(display==L"flex"||display==L"inline-flex"||display==L"grid"||display==L"inline-grid")&&
        value(node,L"flex-direction").find(L"column")!=0;
    std::wstring body;
    if(row&&!cell){
        size_t count=0;
        for(auto child:node.children){
            const auto& entry=tree.nodes[child];
            if(entry.tag.empty()&&tcard_rich::trim(tcard_rich::decode(entry.text)).empty())continue;
            auto content=render(tree,child,true);while(content.ends_with(L"<br>"))content.resize(content.size()-4);if(content.empty())continue;
            if(count%4==0)body+=L"<tr>";body+=L"<td>"+content+L"</td>";
            if(++count%4==0)body+=L"</tr>";
        }
        if(count%4)body+=L"</tr>";
        return L"<table style=\""+tcard_rich::escape(style(node))+L"\">"+body+L"</table>";
    }
    for(auto child:node.children)body+=render(tree,child,cell||tag==L"td"||tag==L"th");
    if(index==0)return body;
    if(tag==L"br"||tag==L"hr")return L"<br>";
    static const std::set<std::wstring> allowed={L"p",L"div",L"span",L"b",L"strong",L"i",L"em",L"u",L"s",L"strike",
        L"del",L"pre",L"code",L"blockquote",L"section",L"article",L"header",L"footer",L"h1",L"h2",L"h3",L"h4",
        L"h5",L"h6",L"ul",L"ol",L"li",L"table",L"thead",L"tbody",L"tr",L"td",L"th",L"a"};
    if(!allowed.contains(tag))return body;
    const bool block=tag==L"p"||tag==L"div"||tag==L"section"||tag==L"article"||tag==L"li"||tag==L"pre"||
        (tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6');
    const auto output=(cell&&block)||display==L"inline"||display==L"inline-block"?L"span":tag;
    std::wstring attrs=L" style=\""+tcard_rich::escape(style(node))+L"\"";
    for(const wchar_t* key:{L"start",L"value",L"href"})
        if(node.attrs.contains(key))attrs+=L" "+std::wstring(key)+L"=\""+tcard_rich::escape(node.attrs.at(key))+L"\"";
    return L"<"+output+attrs+L">"+body+L"</"+output+L">"+(cell&&block?L"<br>":L"");
}
inline std::wstring css_text(const Tree& tree)
{
    std::wstring css;
    for(const auto& node:tree.nodes)if(node.tag==L"style")
        for(auto child:node.children)css+=tree.nodes[child].text+L"\n";
    return css;
}
inline tcard_clip::Clip import(const tcard_clip::Clip& clip,const std::atomic_bool& cancelled,
    const tcard_clip::ImageResolver& images={},const tcard_clip::ImageResolver& pages={})
{
    if(clip.htmlSource.empty())return clip;
    const auto deadline=GetTickCount64()+60000;
    auto read=[&](const std::wstring& url)->std::wstring{
        if(cancelled||GetTickCount64()>=deadline)return {};
        return pages?pages(url):tcard_download::request(url,cancelled,deadline,true);
    };
    Tree tree=parse(clip.htmlSource);
    auto context=parse(clip.htmlContext);
    std::wstring css=css_text(context)+css_text(tree);
    // Source pages supply styles only; the copied fragment remains the content authority.
    if(!clip.url.empty()){
        URL_COMPONENTS parts{};
        if(tcard_download::split(clip.url,parts)){
            auto source=read(clip.url);
            if(!source.empty())context=parse(source);
        }
    }
    css+=css_text(context);
    std::set<std::wstring> sheets;
    for(const auto& node:context.nodes){
        if(node.tag!=L"link"||!node.attrs.contains(L"href")||!node.attrs.contains(L"rel")||
            !token(tcard_rich::lower(node.attrs.at(L"rel")),L"stylesheet"))continue;
        const auto url=tcard_clip::url(node.attrs.at(L"href"),clip.url);
        if(url.empty()||sheets.contains(url)||sheets.size()>=6)continue;
        sheets.insert(url);
        auto content=read(url);if(css.size()+content.size()<=4*1024*1024)css+=L"\n"+content;
    }
    cascade(tree,rules(css));
    auto doc=tcard_rich::parse(render(tree,0));
    std::map<std::wstring,std::wstring> cache;size_t budget=3*1024*1024;
    bool missing=false;
    for(auto& paragraph:doc.paragraphs)for(auto& run:paragraph.runs){
        if(run.image.empty())continue;
        auto address=run.image;
        std::wstring uri;
        if(address.starts_with(L"data:image/")){
            if(!tcard_image::decode(address).dib.empty())uri=address;
        }else{
            address=tcard_clip::url(address,clip.url);
            if(!address.empty()&&!cancelled&&GetTickCount64()<deadline){
                auto found=cache.find(address);
                if(found==cache.end()&&cache.size()<tcard_download::max_requests){
                    auto content=images?images(address):tcard_download::fetch(address,cancelled,deadline);
                    found=cache.emplace(address,std::move(content)).first;
                }
                if(found!=cache.end())uri=found->second;
            }
        }
        if(!uri.empty()&&uri.size()<=budget&&!tcard_image::decode(uri).dib.empty()){
            budget-=uri.size();run.image=std::move(uri);
        }else{
            missing=true;run.text=run.alt.empty()?L"[Image unavailable]":L"["+run.alt+L"]";
            run.image.clear();run.alt.clear();
        }
    }
    if(cancelled)throw std::runtime_error("Import cancelled");
    auto result=clip;result.source=tcard_rich::serialize(doc);result.richHtml=true;
    result.externalImages=missing;result.htmlSource.clear();result.htmlContext.clear();
    if(result.source.size()>4*1024*1024)throw std::runtime_error("Imported note too large");
    return result;
}
}
