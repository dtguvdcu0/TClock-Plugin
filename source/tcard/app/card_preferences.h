#pragma once

constexpr int kSortField = 1201;
constexpr int kGlobalSettings = 1202;
int g_sort_field = 1;
bool g_sort_descending = true;

void fill_list();
void refresh_preview();
void update_open_card();

std::wstring preferences_path() { return module_dir() + L"\\TCard.ini"; }

#include "card_sync_settings.h"

void create_sort(HWND hwnd)
{
    const auto ini = preferences_path();
    const auto limit=GetPrivateProfileIntW(L"TCard",L"HistoryLimit",0,ini.c_str());
    g_history_limit=limit<=10000?limit:0;
    HWND settings=CreateWindowExW(0,L"BUTTON",tcard_text(L"settings.global",L"Global settings"),
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,148,32,hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGlobalSettings)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(settings,WM_SETFONT,reinterpret_cast<WPARAM>(g_ui_font),TRUE);
    SetWindowSubclass(settings,tcard_ui::button_proc,1,0);
    g_sort_field = (std::min)(2, static_cast<int>(GetPrivateProfileIntW(L"TCard", L"SortField", 1, ini.c_str())));
    if (g_sort_field < 0) g_sort_field = 1;
    g_sort_descending = GetPrivateProfileIntW(L"TCard", L"SortDescending", 1, ini.c_str()) != 0;
    HWND sort = CreateWindowExW(0, L"BUTTON", L"Sort", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 96, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSortField)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(sort, WM_SETFONT, reinterpret_cast<WPARAM>(g_ui_font), TRUE);
    SetWindowSubclass(sort, tcard_ui::button_proc, 1, 0);
}

void apply_sort(int field, bool descending)
{
    if (!WritePrivateProfileStringW(L"TCard", L"SortField", std::to_wstring(field).c_str(), preferences_path().c_str()) ||
        !WritePrivateProfileStringW(L"TCard", L"SortDescending", descending ? L"1" : L"0", preferences_path().c_str())) {
        MessageBoxW(g_main, L"Could not save sort settings.", L"TCard", MB_OK | MB_ICONERROR);
        return;
    }
    g_sort_field = field; g_sort_descending = descending;
    fill_list(); refresh_preview();
}

void show_sort()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const std::wstring fields[] = {tcard_lang::text(L"sort.created",L"Date created"),tcard_lang::text(L"sort.modified",L"Date modified"),tcard_lang::text(L"sort.title",L"Title")};
    for (int field = 0; field < 3; ++field) {
        HMENU child = CreatePopupMenu();
        if (!child) { DestroyMenu(menu); return; }
        for (int direction = 0; direction < 2; ++direction) {
            const wchar_t* label = field == 2 ? (direction ? tcard_text(L"sort.ztoa",L"Z to A") : tcard_text(L"sort.atoz",L"A to Z")) :
                (direction ? tcard_text(L"sort.newest",L"Newest first") : tcard_text(L"sort.oldest",L"Oldest first"));
            AppendMenuW(child, MF_STRING, 1400 + field * 2 + direction, label);
        }
        if (field == g_sort_field) CheckMenuRadioItem(child, 0, 1, g_sort_descending ? 1 : 0, MF_BYPOSITION);
        if (!AppendMenuW(menu, MF_POPUP | (field == g_sort_field ? MF_CHECKED : 0), reinterpret_cast<UINT_PTR>(child), fields[field].c_str())) {
            DestroyMenu(child); DestroyMenu(menu); return;
        }
    }
    RECT anchor{};
    GetWindowRect(GetDlgItem(g_main, kSortField), &anchor);
    TPMPARAMS placement{}; placement.cbSize = sizeof(placement); placement.rcExclude = anchor;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        anchor.left, anchor.top, g_main, &placement);
    DestroyMenu(menu);
    if (command >= 1400 && command < 1406) apply_sort(static_cast<int>((command - 1400) / 2), (command - 1400) % 2 != 0);
}

bool card_save_settings(const std::wstring& family,const std::wstring& size,unsigned history,const wchar_t* language)
{
    const auto path=preferences_path();
    std::vector<wchar_t> section(32768);
    const auto length=GetPrivateProfileSectionW(L"TCard",section.data(),static_cast<DWORD>(section.size()),path.c_str());
    if(length>=section.size()-2)return false;
    std::wstring values;
    for(const wchar_t* entry=section.data();*entry;entry+=wcslen(entry)+1){
        if(_wcsnicmp(entry,L"DefaultFontFamily=",18)==0||_wcsnicmp(entry,L"DefaultFontSize=",16)==0||_wcsnicmp(entry,L"HistoryLimit=",13)==0||_wcsnicmp(entry,L"Language=",9)==0)continue;
        values.append(entry);values.push_back(L'\0');
    }
    values+=L"DefaultFontFamily="+family;values.push_back(L'\0');
    values+=L"DefaultFontSize="+size;values.push_back(L'\0');
    values+=L"HistoryLimit="+std::to_wstring(history);values.push_back(L'\0');
    values+=std::wstring(L"Language=")+language;values.push_back(L'\0');values.push_back(L'\0');
    return WritePrivateProfileSectionW(L"TCard",values.c_str(),path.c_str())!=FALSE;
}

INT_PTR CALLBACK card_settings_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        SetWindowTextW(hwnd,tcard_text(L"settings.global",L"Global settings"));
        const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);
            HWND child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,
                WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);return child;
        };
        control(L"STATIC",tcard_text(L"settings.new_notes",L"Applies to notes following global settings."),0,1600,8,8,254,12);
        control(L"STATIC",tcard_text(L"label.font",L"Font"),SS_CENTERIMAGE,1601,8,28,84,14);
        HWND family=control(L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST|CBS_SORT,1602,96,28,108,160);
        control(L"STATIC",tcard_text(L"label.size",L"Size"),SS_CENTERIMAGE,1603,210,28,24,14);
        HWND size=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,1604,238,28,24,14);
        control(L"STATIC",tcard_text(L"settings.language",L"Language"),SS_CENTERIMAGE,1608,8,44,84,14);
        HWND language=control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,1609,96,44,166,120);
        SendMessageW(language,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"\u65e5\u672c\u8a9e"));
        SendMessageW(language,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(language,CB_SETCURSEL,tcard_lang::g_code==L"ja"?0:1,0);
        control(L"STATIC",tcard_text(L"settings.history",L"History limit per note"),SS_CENTERIMAGE,1605,8,60,112,14);
        HWND history=control(L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,1606,124,60,138,120);
        control(L"STATIC",tcard_text(L"settings.next_checkpoint",L"Applies at the next history save"),0,1607,8,78,254,12);
        const auto stored=GetPrivateProfileIntW(L"TCard",L"HistoryLimit",0,preferences_path().c_str());
        const unsigned limit=stored<=10000?stored:0;
        const unsigned limits[]={10,20,50,100,200,500,1000,0};
        bool selected=false;
        auto addHistory=[&](unsigned value){
            const auto label=value?std::to_wstring(value)+L" "+tcard_text(L"settings.entries",L"entries"):
                std::wstring(tcard_text(L"settings.unlimited",L"Unlimited"));
            const auto index=SendMessageW(history,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(index>=0){
                SendMessageW(history,CB_SETITEMDATA,index,value);
                if(value==limit){SendMessageW(history,CB_SETCURSEL,index,0);selected=true;}
            }
        };
        for(const auto value:limits)addHistory(value);
        if(!selected)addHistory(limit);
        const bool sync=g_sync_client.Available();
        if(sync)control(L"BUTTON",tcard_text(L"sync.settings",L"Sync settings"),WS_TABSTOP|BS_PUSHBUTTON,1610,8,102,254,16);
        const int buttons=sync?126:102;
        control(L"BUTTON",tcard_text(L"button.save",L"Save"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,154,buttons,52,16);
        control(L"BUTTON",tcard_text(L"button.cancel",L"Cancel"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,210,buttons,52,16);
        tcard::CardRecord defaults;tcard_apply_defaults(defaults);
        tcard_fonts::populate(family,hwnd);tcard_fonts::select(family,defaults.fontFamily);
        wchar_t points[32]{};swprintf_s(points,L"%g",defaults.fontSize);SetWindowTextW(size,points);
        SendMessageW(size,EM_SETLIMITTEXT,16,0);
        RECT owner{},rect{};GetWindowRect(g_main,&owner);GetWindowRect(hwnd,&rect);
        SetWindowPos(hwnd,nullptr,owner.left+(owner.right-owner.left-(rect.right-rect.left))/2,
            owner.top+(owner.bottom-owner.top-(rect.bottom-rect.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        tcard_ui::prepare_settings(hwnd);
        return TRUE;
    }
    if(message==WM_COMMAND){
        if(LOWORD(wp)==1610){card_show_sync(hwnd);return TRUE;}
        if((LOWORD(wp)==1604&&HIWORD(wp)==EN_CHANGE)||
            ((LOWORD(wp)==1602||LOWORD(wp)==1606)&&HIWORD(wp)==CBN_SELCHANGE))
            SetDlgItemTextW(hwnd,1607,tcard_text(L"settings.next_checkpoint",L"Applies at the next history save"));
        if(LOWORD(wp)==IDCANCEL){EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(LOWORD(wp)==IDOK){
            wchar_t text[32]{};GetDlgItemTextW(hwnd,1604,text,ARRAYSIZE(text));double points=0;
            if(!tcard_ui::valid_size(text,points)){
                SetDlgItemTextW(hwnd,1607,tcard_text(L"message.font_size",L"Enter a font size from 8 to 48."));
                MessageBeep(MB_ICONWARNING);
                SetFocus(GetDlgItem(hwnd,1604));return TRUE;
            }
            const auto family=tcard_fonts::read(GetDlgItem(hwnd,1602));
            if(family.empty()||family.size()>=LF_FACESIZE)return TRUE;
            swprintf_s(text,L"%g",points);
            const HWND history=GetDlgItem(hwnd,1606);
            const auto selected=SendMessageW(history,CB_GETCURSEL,0,0);
            if(selected==CB_ERR)return TRUE;
            const auto limit=SendMessageW(history,CB_GETITEMDATA,selected,0);
            if(limit<0||limit>10000)return TRUE;
            const auto languageIndex=SendDlgItemMessageW(hwnd,1609,CB_GETCURSEL,0,0);
            if(languageIndex!=0&&languageIndex!=1)return TRUE;
            const wchar_t* language=languageIndex==0?L"ja":L"en";
            if(tcard_lang::read_catalog(tcard_lang::language_file(language)).empty()||
                !card_save_settings(family,text,static_cast<unsigned>(limit),language)){
                SetDlgItemTextW(hwnd,1607,tcard_text(L"settings.save_failed",L"Could not save settings."));
                MessageBeep(MB_ICONERROR);return TRUE;
            }
            g_history_limit=static_cast<unsigned>(limit);
            EndDialog(hwnd,IDOK);return TRUE;
        }
    }
    return FALSE;
}

bool card_show_settings()
{
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;
    dialog.cx=270;dialog.cy=g_sync_client.Available()?150:126;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));
    data.insert(data.end(),{0,0,0,9});
    const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),g_main,card_settings_proc,0)==IDOK;
}
