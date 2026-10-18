#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

// Pure configuration parsing. Socket I/O and adapter discovery belong to TCardSync.
namespace tcard_net {
struct Rule { uint32_t network=0,mask=0; };
struct Peer { std::wstring ip; unsigned short port=0; uint32_t address=0; };
inline bool ParseNumber(const std::wstring& text,unsigned maximum,unsigned& number)
{
    if(text.empty()||text.size()>5||(text.size()>1&&text[0]==L'0'))return false;
    number=0;for(wchar_t c:text){if(c<L'0'||c>L'9')return false;number=number*10+c-L'0';if(number>maximum)return false;}return true;
}
inline bool ParseAddress(const std::wstring& text,uint32_t& address)
{
    address=0;size_t start=0;
    for(unsigned i=0;i<4;++i){const auto end=text.find(L'.',start);
        if((i==3)!=(end==std::wstring::npos))return false;unsigned octet=0;
        if(!ParseNumber(text.substr(start,end==std::wstring::npos?end:end-start),255,octet))return false;
        address=(address<<8)|octet;start=end+1;
    }return true;
}
inline std::wstring FormatAddress(uint32_t address)
{
    return std::to_wstring(address>>24)+L"."+std::to_wstring((address>>16)&255)+L"."+
        std::to_wstring((address>>8)&255)+L"."+std::to_wstring(address&255);
}
inline bool SplitList(const std::wstring& text,std::vector<std::wstring>& values)
{
    values.clear();if(text.size()>4096)return false;size_t at=0;
    while(at<text.size()){
        at=text.find_first_not_of(L" ,;\t\r\n",at);if(at==std::wstring::npos)break;
        const auto end=text.find_first_of(L" ,;\t\r\n",at);values.push_back(text.substr(at,end==std::wstring::npos?end:end-at));
        if(values.size()>64)return false;if(end==std::wstring::npos)break;at=end;
    }return true;
}
inline bool ParseRules(const std::wstring& text,std::vector<Rule>& rules)
{
    rules.clear();std::vector<std::wstring> tokens;if(!SplitList(text,tokens))return false;
    for(const auto& token:tokens){const auto slash=token.find(L'/');uint32_t address=0;unsigned prefix=32;
        if(!ParseAddress(token.substr(0,slash),address)||(slash!=std::wstring::npos&&!ParseNumber(token.substr(slash+1),32,prefix)))return false;
        const uint32_t mask=prefix?0xffffffffu<<(32-prefix):0;rules.push_back({address&mask,mask});
    }return true;
}
inline bool Matches(uint32_t address,const std::vector<Rule>& rules)
{
    return std::any_of(rules.begin(),rules.end(),[&](const Rule& rule){return (address&rule.mask)==rule.network;});
}
inline bool IsAllowed(uint32_t address,const std::vector<Rule>& allow,const std::vector<Rule>& deny)
{
    return !Matches(address,deny)&&Matches(address,allow);
}
inline bool ParsePeers(const std::wstring& text,unsigned port,std::vector<Peer>& peers)
{
    peers.clear();std::vector<std::wstring> tokens;if(!SplitList(text,tokens)||port>65535)return false;
    for(const auto& token:tokens){const auto colon=token.find(L':');unsigned selected=port;uint32_t address=0;
        const auto ip=token.substr(0,colon);
        if(!ParseAddress(ip,address)||(colon!=std::wstring::npos&&!ParseNumber(token.substr(colon+1),65535,selected))||!selected||
            (address>>24)==0||(address>>24)>=224||address==0xffffffffu)return false;
        if(std::none_of(peers.begin(),peers.end(),[&](const Peer& p){return p.address==address&&p.port==selected;}))
            peers.push_back({ip,static_cast<unsigned short>(selected),address});
    }return true;
}
inline std::wstring Flatten(const std::wstring& text)
{
    std::vector<std::wstring> tokens;if(!SplitList(text,tokens))return text;std::wstring result;
    for(const auto& token:tokens){if(!result.empty())result+=L";";result+=token;}return result;
}
struct Policy {
    std::vector<Rule> allow,deny,lan;
    // Off-link admission is explicit; it never supplies an allow entry or authentication authority.
    bool remoteAdmission=false;
    bool Allows(uint32_t address)const{return (remoteAdmission||Matches(address,lan))&&IsAllowed(address,allow,deny);}
};
}
