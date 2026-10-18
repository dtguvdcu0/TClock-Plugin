#pragma once
#include "card_language.h"
#include <windows.h>
#include <string>

namespace tcard_fonts {
inline int CALLBACK collect(const LOGFONTW* font,const TEXTMETRICW* metrics,DWORD,LPARAM parameter)
{
    const auto* name=font->lfFaceName;
    if(!name[0]||name[0]==L'@'||name[0]==L'&'||name[0]==L'$'||name[1]==L'@'||name[1]==L'$')return 1;
    // Match the clock font page's pitch marker and native CBS_SORT ordering.
    const std::wstring label=std::wstring(1,(metrics->tmPitchAndFamily&TMPF_FIXED_PITCH)?L' ':L'*')+name;
    const HWND combo=reinterpret_cast<HWND>(parameter);
    if(SendMessageW(combo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(label.c_str()))==CB_ERR)
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    return 1;
}
inline void populate(HWND combo,HWND owner)
{
    LOGFONTW font{};font.lfCharSet=DEFAULT_CHARSET;
    HDC dc=GetDC(owner);
    if(dc){EnumFontFamiliesExW(dc,&font,collect,reinterpret_cast<LPARAM>(combo),0);ReleaseDC(owner,dc);}
    if(SendMessageW(combo,CB_GETCOUNT,0,0)==0)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L" Segoe UI"));
}
inline void select(HWND combo,const std::wstring& family)
{
    for(const auto prefix:{L' ',L'*'}){
        const auto label=std::wstring(1,prefix)+family;
        const auto found=SendMessageW(combo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(label.c_str()));
        if(found!=CB_ERR){SendMessageW(combo,CB_SETCURSEL,found,0);return;}
    }
    // Keep a saved family even when it is unavailable on this computer.
    const auto label=L" "+family;
    const auto index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    if(index>=0)SendMessageW(combo,CB_SETCURSEL,index,0);
}
inline std::wstring read(HWND combo)
{
    const auto length=GetWindowTextLengthW(combo);
    std::wstring value(static_cast<size_t>(length)+1,L'\0');
    value.resize(GetWindowTextW(combo,value.data(),static_cast<int>(value.size())));
    if(!value.empty()&&(value[0]==L' '||value[0]==L'*'))value.erase(0,1);
    return value;
}

constexpr UINT refreshMessage = WM_APP + 0x374;
struct Defaults { std::wstring family = L"Yu Gothic UI"; double size = 10; };
inline const Defaults& defaults(bool refresh = false)
{
    static Defaults value;
    static bool loaded = false;
    if (!loaded || refresh) {
        value = Defaults{};
        wchar_t path[32768]{}, family[256]{}, size[64]{};
        const DWORD length = GetModuleFileNameW(nullptr,path,ARRAYSIZE(path));
        if (length && length < ARRAYSIZE(path)) {
            std::wstring ini(path,length);
            const auto slash=ini.find_last_of(L"\\/");
            if(slash!=ini.npos){
                ini.resize(slash+1);ini+=L"TCard.ini";
                GetPrivateProfileStringW(L"TCard",L"DefaultFontFamily",L"",family,ARRAYSIZE(family),ini.c_str());
                GetPrivateProfileStringW(L"TCard",L"DefaultFontSize",L"",size,ARRAYSIZE(size),ini.c_str());
                std::wstring name(family);
                const auto first=name.find_first_not_of(L" \t");
                if(first!=name.npos){name=name.substr(first,name.find_last_not_of(L" \t")-first+1);if(name.size()<LF_FACESIZE)value.family=name;}
                wchar_t* end=nullptr;const double points=wcstod(size,&end);
                while(end&&iswspace(*end))++end;
                if(end!=size&&end&&!*end&&points>=8&&points<=48)value.size=points;
            }
        }
        loaded=true;
    }
    return value;
}
inline std::wstring inherited_label(bool size)
{
    wchar_t points[32]{};swprintf_s(points,L"%g",defaults().size);
    auto label=tcard_lang::text(L"font.inherit",L"Follow global settings (%s)");
    if(const auto marker=label.find(L"%s");marker!=label.npos)
        label.replace(marker,2,size?std::wstring(points):defaults().family);
    return label;
}
inline bool inherited(HWND combo)
{
    const auto index=SendMessageW(combo,CB_GETCURSEL,0,0);
    if(index==CB_ERR||SendMessageW(combo,CB_GETITEMDATA,index,0)!=0x5443)return false;
    const auto length=SendMessageW(combo,CB_GETLBTEXTLEN,index,0);
    if(length<0||length>1024)return false;
    std::wstring label(static_cast<size_t>(length)+1,L'\0');
    SendMessageW(combo,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(label.data()));label.resize(static_cast<size_t>(length));
    return read(combo)==label;
}
inline void inheritance(HWND combo,bool size,bool selected)
{
    for(LRESULT i=SendMessageW(combo,CB_GETCOUNT,0,0)-1;i>=0;--i)
        if(SendMessageW(combo,CB_GETITEMDATA,i,0)==0x5443)SendMessageW(combo,CB_DELETESTRING,i,0);
    const auto label=inherited_label(size);
    const auto index=SendMessageW(combo,CB_INSERTSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    if(index>=0){SendMessageW(combo,CB_SETITEMDATA,index,0x5443);if(selected)SendMessageW(combo,CB_SETCURSEL,index,0);}
}
inline void select_size(HWND combo,double size,bool inherit)
{
    if(SendMessageW(combo,CB_GETCOUNT,0,0)==0)
        for(const auto points:{8,9,10,11,12,14,16,18,20,24,28,32,36,48}){
            const auto label=std::to_wstring(points);SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
    inheritance(combo,true,inherit);
    if(!inherit){wchar_t value[32]{};swprintf_s(value,L"%g",size);SetWindowTextW(combo,value);}
}
inline bool read_size(HWND combo,double& size)
{
    if(inherited(combo)){size=defaults().size;return true;}
    wchar_t text[128]{};GetWindowTextW(combo,text,ARRAYSIZE(text));
    wchar_t* end=nullptr;size=wcstod(text,&end);while(end&&iswspace(*end))++end;
    return end!=text&&end&&!*end&&size>=8&&size<=48;
}
inline std::wstring resolved_family(HWND combo)
{
    return inherited(combo)?defaults().family:read(combo);
}
}
