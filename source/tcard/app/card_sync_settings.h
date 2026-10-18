#pragma once
#include "../core/card_peer_cache.h"
#include "../core/card_connection_log.h"

std::wstring card_sync_label()
{
    if(card_signal_busy())return tcard_text(L"sync.signal_host_preparing",L"Preparing protected signaling hosting...");
    const auto& key=g_sync_client.status;
    const wchar_t* fallback=L"Synchronization failed. Check the settings.";
    if(key==L"sync.off")fallback=L"Synchronization is off.";
    else if(key==L"sync.starting")fallback=L"Starting synchronization...";
    else if(key==L"sync.ready")fallback=L"Synchronization is running.";
    else if(key==L"sync.waiting_edit")fallback=L"Received changes will appear after editing.";
    else if(key==L"sync.pending")fallback=L"Some changes are waiting for required data.";
    else if(key==L"sync.peer_failed")fallback=L"Cannot connect to the other app. Retrying...";
    else if(key==L"sync.unavailable")fallback=L"Place TCardSync.exe beside TCard.exe.";
    else if(key==L"sync.port_busy")fallback=L"Cannot bind the selected port. Change it or close its owner.";
    else if(key==L"sync.stopping")fallback=L"Waiting for synchronization to stop.";
    std::wstring text=tcard_text(key.c_str(),fallback);
    if(!g_signal_status.empty()){
        const wchar_t* signal=g_signal_status==L"sync.signal_host_running"?L"Signaling node is running.":
            g_signal_status==L"sync.signal_host_stopping"?L"Waiting for the signaling node to stop.":
            g_signal_status==L"sync.signal_host_busy"?L"Finish editing, image imports and cleanup, then save Sync settings again to start hosting.":
            L"The signaling node could not start. Sync settings and local data were kept.";
        text+=L"\r\n"+std::wstring(tcard_text(g_signal_status.c_str(),signal));
    }
    return text;
}

#include "card_source_dialog.h"

// Edit public node metadata as part of this existing Sync settings transaction.
INT_PTR CALLBACK card_signal_settings_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        auto* settings=reinterpret_cast<tcard_bridge::Settings*>(lp);if(!settings){EndDialog(hwnd,IDCANCEL);return TRUE;}
        SetPropW(hwnd,L"TCardSync.SignalEdit",reinterpret_cast<HANDLE>(settings));
        SetWindowTextW(hwnd,tcard_text(L"sync.signaling",L"Signaling..."));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        const auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);
            const auto child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,
                rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);
        };
        control(L"BUTTON",tcard_text(L"sync.signal_enable",L"Find peers through signaling nodes"),WS_TABSTOP|BS_AUTOCHECKBOX,1740,8,8,304,14);
        CheckDlgButton(hwnd,1740,settings->signaling?BST_CHECKED:BST_UNCHECKED);
        control(L"STATIC",tcard_text(L"sync.signal_nodes",L"Nodes (IPv4:port@SHA-256 certificate pin)"),0,1741,8,28,304,12);
        control(L"EDIT",settings->signalNodes.c_str(),WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,1742,8,44,304,32);
        control(L"STATIC",tcard_text(L"sync.signal_publish",L"Published sync endpoint (optional IPv4:port)"),0,1743,8,84,304,12);
        control(L"EDIT",settings->publishedSyncEndpoint.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,1744,8,100,304,14);
        control(L"STATIC",tcard_text(L"sync.signal_scope",L"Existing IP rules and sync authentication apply. Save Sync settings to activate changes."),0,1745,8,120,304,24);
        const bool hosting=card_signal_available();
        control(L"BUTTON",tcard_text(L"sync.signal_host",L"Host a signaling node on this device"),WS_TABSTOP|BS_AUTOCHECKBOX,1746,8,148,304,14);
        CheckDlgButton(hwnd,1746,settings->hostSignal?BST_CHECKED:BST_UNCHECKED);EnableWindow(GetDlgItem(hwnd,1746),hosting);
        control(L"STATIC",tcard_text(L"sync.signal_host_pin",L"This node's SHA-256 certificate pin"),0,1747,8,168,304,12);
        control(L"EDIT",settings->hostSignalPin.c_str(),WS_TABSTOP|ES_AUTOHSCROLL,1748,8,184,304,14);EnableWindow(GetDlgItem(hwnd,1748),hosting);
        SendDlgItemMessageW(hwnd,1748,EM_SETLIMITTEXT,64,0);
        control(L"BUTTON",tcard_text(L"button.save",L"Save"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,204,216,52,16);
        control(L"BUTTON",tcard_text(L"button.cancel",L"Cancel"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,260,216,52,16);
        SendDlgItemMessageW(hwnd,1742,EM_SETLIMITTEXT,8191,0);SendDlgItemMessageW(hwnd,1744,EM_SETLIMITTEXT,32,0);tcard_ui::prepare_settings(hwnd);return TRUE;
    }
    if(message==WM_COMMAND){
        if(LOWORD(wp)==IDCANCEL){EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(LOWORD(wp)==IDOK){auto* settings=reinterpret_cast<tcard_bridge::Settings*>(GetPropW(hwnd,L"TCardSync.SignalEdit"));if(!settings){EndDialog(hwnd,IDCANCEL);return TRUE;}
            auto edited=*settings;edited.signaling=IsDlgButtonChecked(hwnd,1740)==BST_CHECKED;
            edited.signalNodes=tcard_net::Flatten(window_text(GetDlgItem(hwnd,1742)));edited.publishedSyncEndpoint=window_text(GetDlgItem(hwnd,1744));
            if(card_signal_available()){edited.hostSignal=IsDlgButtonChecked(hwnd,1746)==BST_CHECKED;edited.hostSignalPin=window_text(GetDlgItem(hwnd,1748));}
            if(edited.hostSignal&&!tcard_sync::sync_hex(edited.hostSignalPin)){MessageBoxW(hwnd,tcard_text(L"sync.signal_host_failed",L"The signaling node could not start. Sync settings and local data were kept."),L"TCard",MB_OK|MB_ICONWARNING);return TRUE;}
            std::vector<tcard_bridge::SignalNode> nodes;tcard_net::Peer published;
            if(edited.signaling&&(!tcard_bridge::ParseSignalNodes(edited.signalNodes,nodes)||nodes.empty()||!tcard_bridge::ParsePublishedSyncEndpoint(edited.publishedSyncEndpoint,published))){
                MessageBoxW(hwnd,tcard_text(L"sync.signal_invalid",L"Enter pinned nodes and an optional published sync endpoint."),L"TCard",MB_OK|MB_ICONWARNING);return TRUE;}
            *settings=std::move(edited);EndDialog(hwnd,IDOK);return TRUE;
        }
    }
    if(message==WM_DESTROY)RemovePropW(hwnd,L"TCardSync.SignalEdit");return FALSE;
}
void card_show_signal_settings(HWND owner,tcard_bridge::Settings& settings)
{
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=320;dialog.cy=240;
    std::vector<BYTE> data(sizeof(dialog));memcpy(data.data(),&dialog,sizeof(dialog));
    const auto append=[&](WORD word){data.push_back(static_cast<BYTE>(word));data.push_back(static_cast<BYTE>(word>>8));};
    append(0);append(0);append(0);append(10);for(wchar_t c:std::wstring(L"Segoe UI"))append(c);append(0);
    auto draft=settings;if(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_signal_settings_proc,reinterpret_cast<LPARAM>(&draft))==IDOK)settings=std::move(draft);
}

// Completion-driven allow rules stay in the dialog draft until Save.
void card_sync_allow(HWND hwnd)
{
    tcard_bridge::Settings edited;edited.listenPort=GetDlgItemInt(hwnd,1703,nullptr,FALSE);edited.peerPort=0;
    edited.peers=window_text(GetDlgItem(hwnd,1715));edited.whitelist=window_text(GetDlgItem(hwnd,1717));
    if(tcard_bridge::AddPeerRules(edited)&&edited.whitelist!=window_text(GetDlgItem(hwnd,1717)))SetDlgItemTextW(hwnd,1717,edited.whitelist.c_str());
}
int card_measure_text(HWND hwnd,int id,int minimum)
{
    RECT textRect{0,0,304,12};MapDialogRect(hwnd,&textRect);const int baseHeight=textRect.bottom;
    HDC dc=GetDC(hwnd);if(!dc||baseHeight<=0){if(dc)ReleaseDC(hwnd,dc);return minimum;}
    const auto font=reinterpret_cast<HFONT>(SendDlgItemMessageW(hwnd,id,WM_GETFONT,0,0));
    const auto old=font?SelectObject(dc,font):nullptr;const auto text=window_text(GetDlgItem(hwnd,id));
    DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&textRect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    if(old)SelectObject(dc,old);ReleaseDC(hwnd,dc);
    return (std::max)(minimum,((static_cast<int>(textRect.bottom)*12+baseHeight-1)/baseHeight+3)/4*4);
}
void card_wrap_path(HWND hwnd)
{
    const HWND label=GetDlgItem(hwnd,1711);auto path=window_text(label);
    path.erase(std::remove(path.begin(),path.end(),L'\r'),path.end());path.erase(std::remove(path.begin(),path.end(),L'\n'),path.end());
    RECT rect{};GetClientRect(label,&rect);HDC dc=GetDC(label);if(!dc||rect.right<=0){if(dc)ReleaseDC(label,dc);return;}
    const auto font=reinterpret_cast<HFONT>(SendMessageW(label,WM_GETFONT,0,0));const auto old=font?SelectObject(dc,font):nullptr;
    std::wstring wrapped;size_t offset=0;
    while(offset<path.size()){
        int fitted=0;SIZE size{};const auto remaining=path.size()-offset;
        if(!GetTextExtentExPointW(dc,path.data()+offset,static_cast<int>(remaining),rect.right,&fitted,nullptr,&size)){wrapped=path;break;}
        size_t count=(std::max)(size_t(1),static_cast<size_t>(fitted));
        if(count<remaining){
            const auto separator=path.find_last_of(L"\\/",offset+count-1);
            if(separator!=std::wstring::npos&&separator>=offset+count/2)count=separator+1-offset;
            if(count&&path[offset+count-1]>=0xd800&&path[offset+count-1]<=0xdbff&&path[offset+count]>=0xdc00&&path[offset+count]<=0xdfff)--count;
            if(!count)count=(std::min)(size_t(2),remaining);
        }
        if(!wrapped.empty())wrapped+=L"\r\n";wrapped.append(path,offset,count);offset+=count;
    }
    if(old)SelectObject(dc,old);ReleaseDC(label,dc);
    if(window_text(label)!=wrapped)SetWindowTextW(label,wrapped.c_str());
}
void card_sync_layout(HWND hwnd)
{
    card_wrap_path(hwnd);int y=144;
    for(const auto& row:{std::pair<int,int>{1716,1717},{1718,1719}}){
        const bool open=GetPropW(hwnd,row.first==1716?L"TCardSync.WhiteOpen":L"TCardSync.BlackOpen")!=nullptr;
        const auto move=[&](int id,int top,int height){RECT rect{8,top,312,top+height};MapDialogRect(hwnd,&rect);SetWindowPos(GetDlgItem(hwnd,id),nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);};
        const std::wstring caption=(open?L"\u25bc ":L"\u25b6 ")+std::wstring(tcard_text(row.first==1716?L"sync.whitelist":L"sync.blacklist",row.first==1716?L"Whitelist (IPv4 or CIDR)":L"Blacklist (takes priority)"));
        SetDlgItemTextW(hwnd,row.first,caption.c_str());move(row.first,y,16);y+=16;
        ShowWindow(GetDlgItem(hwnd,row.second),open?SW_SHOWNA:SW_HIDE);if(open){move(row.second,y,32);y+=40;}
    }
    const int shift=272-y;
    const int helpHeight=card_measure_text(hwnd,1706,24);
    const int pathHeight=card_measure_text(hwnd,1711,12),statusHeight=card_measure_text(hwnd,1712,12);
    const int statusTop=360+pathHeight+4,buttonsTop=statusTop+statusHeight+4;
    const int extra=helpHeight-24;
    struct Row{int id,x,y,w,h;};
    for(const auto& row:{Row{1733,8,272,304,14},{1706,8,288,304,24},{1730,8,320,100,16},{1731,116,320,88,16},{1732,212,320,100,16},
        {1710,8,344,304,12},{1711,8,360,304,pathHeight},{1712,8,statusTop,304,statusHeight},{IDOK,204,buttonsTop,52,16},{IDCANCEL,260,buttonsTop,52,16}}){
        const int top=row.y-shift+(row.y>288?extra:0),height=row.id==1706?helpHeight:row.h;
        RECT rect{row.x,top,row.x+row.w,top+height};MapDialogRect(hwnd,&rect);
        SetWindowPos(GetDlgItem(hwnd,row.id),nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    RECT desired{0,0,320,buttonsTop+24-shift+extra},client{},window{};MapDialogRect(hwnd,&desired);GetClientRect(hwnd,&client);GetWindowRect(hwnd,&window);
    SetWindowPos(hwnd,nullptr,0,0,window.right-window.left,desired.bottom+window.bottom-window.top-client.bottom,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    InvalidateRect(hwnd,nullptr,TRUE);
}
void card_copy_folder(HWND hwnd)
{
    const auto text=g_sync_client.Profile();const auto bytes=(text.size()+1)*sizeof(wchar_t);
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!memory)return;
    void* value=GlobalLock(memory);if(!value){GlobalFree(memory);return;}memcpy(value,text.c_str(),bytes);GlobalUnlock(memory);
    if(!OpenClipboard(hwnd)){GlobalFree(memory);return;}
    if(!EmptyClipboard()||!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);CloseClipboard();
}
bool card_sync_save(HWND hwnd)
{
    if(card_signal_busy()){MessageBoxW(hwnd,tcard_text(L"sync.signal_host_busy",L"Finish editing, image imports and cleanup, then save Sync settings again to start hosting."),L"TCard",MB_OK|MB_ICONINFORMATION);return false;}
    if(!g_sync_client.Available()){
        MessageBoxW(hwnd,tcard_text(L"sync.unavailable",L"Place TCardSync.exe beside TCard.exe."),L"TCard",MB_OK|MB_ICONERROR);return false;
    }
    tcard_bridge::Settings settings=tcard_bridge::ReadSettings(preferences_path());
    if(const auto draft=reinterpret_cast<const tcard_bridge::Settings*>(GetPropW(hwnd,L"TCardSync.SignalDraft"))){settings.signaling=draft->signaling;settings.signalNodes=draft->signalNodes;settings.publishedSyncEndpoint=draft->publishedSyncEndpoint;settings.hostSignal=draft->hostSignal;settings.hostSignalPin=draft->hostSignalPin;}
    settings.enabled=IsDlgButtonChecked(hwnd,1701)==BST_CHECKED;
    card_sync_allow(hwnd);
    BOOL listen=FALSE;settings.listenPort=GetDlgItemInt(hwnd,1703,&listen,FALSE);settings.peerPort=0;
    // The hidden legacy provisioning value is retained; synchronization owns its listener.
    settings.hasPolicy=true;settings.server=true;
    settings.sharePeers=IsDlgButtonChecked(hwnd,1733)==BST_CHECKED;
    settings.remoteAdmission=IsDlgButtonChecked(hwnd,1734)==BST_CHECKED;
    settings.peers=tcard_net::Flatten(window_text(GetDlgItem(hwnd,1715)));
    settings.whitelist=tcard_net::Flatten(window_text(GetDlgItem(hwnd,1717)));
    settings.blacklist=tcard_net::Flatten(window_text(GetDlgItem(hwnd,1719)));
    if(!listen||!tcard_bridge::AddPeerRules(settings)||!tcard_bridge::ValidSettings(settings)){
        MessageBoxW(hwnd,tcard_text(L"sync.invalid",L"Check IPv4/CIDR lists and ports (1-65535). Avoid a loopback target using this app's port."),L"TCard",MB_OK|MB_ICONWARNING);return false;
    }
    const auto previous=tcard_bridge::ReadSettings(preferences_path());
    const bool changed=previous.enabled!=settings.enabled||previous.listenPort!=settings.listenPort||previous.peerPort!=settings.peerPort||previous.joinProfile!=settings.joinProfile||previous.server!=settings.server||previous.peers!=settings.peers||
        previous.remoteAdmission!=settings.remoteAdmission||previous.sharePeers!=settings.sharePeers||previous.signaling!=settings.signaling||previous.signalNodes!=settings.signalNodes||previous.publishedSyncEndpoint!=settings.publishedSyncEndpoint||previous.whitelist!=settings.whitelist||previous.blacklist!=settings.blacklist||!previous.hasPolicy;
    if(changed&&!g_sync_client.Stop()){
        MessageBoxW(hwnd,card_sync_label().c_str(),L"TCard",MB_OK|MB_ICONWARNING);return false;
    }
    if(!tcard_bridge::SaveSettings(preferences_path(),settings)){
        if(changed)card_sync_start(hwnd,previous);
        MessageBoxW(hwnd,tcard_text(L"settings.save_failed",L"Could not save settings."),L"TCard",MB_OK|MB_ICONERROR);return false;
    }
    if(changed||(settings.enabled&&!g_sync_client.Running()))card_sync_start(hwnd,settings);
    card_signal_request(settings.hostSignal);return true;

}
INT_PTR CALLBACK card_join_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        SetWindowTextW(hwnd,tcard_text(L"sync.join_title",L"Join another group"));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* value,DWORD style,int id,int x,int y,int width,int height){
            RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);
            HWND child=CreateWindowExW(0,cls,value,WS_CHILD|WS_VISIBLE|style,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,
                hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);
        };
        control(L"STATIC",tcard_text(L"sync.join_choice",L"Merge adds this device's saved notes/history. Replacement uses a verified source and keeps encrypted recovery of the old data."),0,1810,8,8,344,24);
        control(L"BUTTON",tcard_text(L"sync.join_merge",L"Merge and join"),WS_TABSTOP|BS_PUSHBUTTON,1811,8,40,104,16);
        control(L"BUTTON",tcard_text(L"sync.join_replace",L"Replace from source"),WS_TABSTOP|BS_PUSHBUTTON,1812,128,40,104,16);
        control(L"BUTTON",tcard_text(L"button.cancel",L"Cancel"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDCANCEL,248,40,104,16);
        tcard_ui::prepare_settings(hwnd);SetFocus(GetDlgItem(hwnd,IDCANCEL));return FALSE;
    }
    if(message==WM_COMMAND){const int id=LOWORD(wp);
        if(id==1811){EndDialog(hwnd,IDYES);return TRUE;}
        if(id==IDCANCEL){EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(id==1812){EndDialog(hwnd,IDNO);return TRUE;}
    }
    return FALSE;
}
int card_choose_join(HWND owner)
{
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=360;dialog.cy=64;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));data.insert(data.end(),{0,0,0,9});
    const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    return static_cast<int>(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_join_proc,0));
}
INT_PTR CALLBACK card_key_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(g_key_active&&(message==WM_COMMAND||message==WM_CLOSE||(message==WM_SYSCOMMAND&&(wp&0xfff0)==SC_CLOSE)))return TRUE;
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        SetWindowTextW(hwnd,tcard_text(L"sync.keys",L"Sync key"));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* value,DWORD style,int id,int x,int y,int width,int height){
            RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);HWND child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,value,WS_CHILD|WS_VISIBLE|style,
                rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);
        };
        control(L"STATIC",tcard_text(L"sync.key_scope",L"New keys re-encrypt saved data. Joining another group requires Merge or Replace."),0,1720,8,8,344,24);
        control(L"STATIC",tcard_text(L"sync.key_current",L"Current words (21 words)"),0,1721,8,40,344,12);
        const auto display=reinterpret_cast<const std::wstring*>(lp);
        control(L"EDIT",display?display->c_str():L"",WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,1722,8,56,344,48);
        control(L"STATIC",tcard_text(L"sync.key_input",L"Paste the other PC's words to update the key"),0,1723,8,112,344,12);
        control(L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,1724,8,128,344,48);SendDlgItemMessageW(hwnd,1724,EM_SETLIMITTEXT,1024,0);
        control(L"STATIC",tcard_text(L"sync.key_keep",L"Local notes/history remain. Keep these words to restore the group."),0,1725,8,184,344,24);
        control(L"BUTTON",tcard_text(L"sync.key_update",L"Update key"),WS_TABSTOP|BS_DEFPUSHBUTTON,1726,8,216,100,16);
        control(L"BUTTON",tcard_text(L"sync.key_new",L"Create new key"),WS_TABSTOP|BS_PUSHBUTTON,1727,116,216,112,16);
        control(L"BUTTON",tcard_text(L"button.close",L"Close"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,252,216,100,16);
        tcard_ui::prepare_settings(hwnd);return TRUE;
    }
    if(message==WM_COMMAND){
        const int id=LOWORD(wp);if(id==IDCANCEL){EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(card_signal_busy()||g_source_dialog)return TRUE;
        if(id==1726||id==1727){
            auto words=window_text(GetDlgItem(hwnd,1724));std::wstring display;const auto settings=tcard_bridge::ReadSettings(preferences_path());
            bool okay=false;std::wstring failure;
            try{okay=id==1727?card_rotate_key(hwnd,display):card_join_key(hwnd,words,display);failure=g_sync_client.status;}
            catch(...){failure=L"sync.key_failed";}
            SecureZeroMemory(words.data(),words.size()*sizeof(wchar_t));
            if(failure==L"sync.key_cancelled"){
                // Match stops the previous worker; cancelling a join must restore its remembered group.
                if(id==1726&&settings.enabled){try{card_sync_start(hwnd,tcard_bridge::ReadSettings(preferences_path()));}catch(...){g_sync_client.status=L"sync.failed";}}
                return TRUE;
            }
            if(!okay&&failure!=L"sync.key_invalid"){
                // A lost response can follow a committed key pointer. Refresh rather than display stale words.
                try{if(!g_storage_locked&&g_sync_client.Key(L"show",L"",display,settings.joinProfile))SetDlgItemTextW(hwnd,1722,display.c_str());else SetDlgItemTextW(hwnd,1722,L"");}
                catch(...){SetDlgItemTextW(hwnd,1722,L"");}
            }
            if(settings.enabled)card_sync_start(hwnd,settings);
            if(okay){SetDlgItemTextW(hwnd,1722,display.c_str());SetDlgItemTextW(hwnd,1724,L"");}
            else MessageBoxW(hwnd,tcard_text(failure.c_str(),L"Cannot update the key. Existing data is preserved."),L"TCard",MB_OK|MB_ICONERROR);
            SecureZeroMemory(display.data(),display.size()*sizeof(wchar_t));return TRUE;
        }
    }
    if(message==WM_DESTROY){SetDlgItemTextW(hwnd,1722,L"");SetDlgItemTextW(hwnd,1724,L"");return TRUE;}
    return FALSE;
}
void card_show_keys(HWND owner)
{
    if(card_signal_busy())return;
    const auto settings=tcard_bridge::ReadSettings(preferences_path());std::wstring display,failure;bool okay=false;
    try{okay=g_sync_client.Key(L"show",L"",display,settings.joinProfile);failure=g_sync_client.status;}catch(...){failure=L"sync.key_failed";}
    if(settings.enabled)card_sync_start(owner,settings);
    if(!okay){MessageBoxW(owner,tcard_text(failure.c_str(),L"Cannot read the key."),L"TCard",MB_OK|MB_ICONERROR);return;}
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=360;dialog.cy=240;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));data.insert(data.end(),{0,0,0,9});
    const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_key_proc,reinterpret_cast<LPARAM>(&display));
    SecureZeroMemory(display.data(),display.size()*sizeof(wchar_t));
}
std::wstring card_sync_result(const winrt::Windows::Data::Json::JsonObject& result)
{
    std::wstring text=tcard_text(result.GetNamedBoolean(L"Listening",false)?L"sync.check_listening":L"sync.check_client",L"LAN sync check");
    const auto peers=result.GetNamedArray(L"Peers");
    if(!peers.Size())text+=L"\r\n"+std::wstring(tcard_text(L"sync.check_no_targets",L"No outgoing targets; data exchange has not been checked."));
    for(const auto& value:peers){const auto peer=value.GetObject();text+=L"\r\n\r\n"+std::wstring(peer.GetNamedString(L"Peer").c_str())+L"\r\n";
        const wchar_t* key=peer.GetNamedBoolean(L"Exchanged",false)?(peer.GetNamedBoolean(L"Converged",false)?L"sync.check_ok":L"sync.pending"):peer.GetNamedBoolean(L"Authenticated",false)?L"sync.check_exchange_failed":
            peer.GetNamedBoolean(L"Connected",false)?L"sync.check_auth_failed":L"sync.check_connect_failed";
        text+=tcard_text(key,L"Check failed.");const auto pending=peer.GetNamedNumber(L"Pending",0);
        if(pending)text+=L"\r\n"+std::wstring(tcard_text(L"sync.pending",L"Changes pending:"))+L" "+std::to_wstring(static_cast<unsigned long long>(pending));
        if(!tcard_sync::sync_string(peer,L"PeerNode").empty()){
            const auto applicationPending=peer.GetNamedNumber(L"PeerApplicationPending",0);
            const bool applied=peer.GetNamedBoolean(L"PeerApplicationKnown",false)&&applicationPending==0;
            text+=L"\r\n"+std::wstring(tcard_text(applied?L"sync.check_applied":L"sync.check_apply_pending",
                applied?L"Peer TCard application confirmed.":L"Waiting for peer TCard application confirmation:"));
            if(!applied)text+=L" "+std::to_wstring(static_cast<unsigned long long>(applicationPending));
        }
        for(const auto& field:{std::pair{L"MissingLocal",L"sync.check_missing_local"},std::pair{L"MissingPeer",L"sync.check_missing_peer"}}){
            const auto count=peer.GetNamedNumber(field.first,0);if(count>0)text+=L"\r\n"+std::wstring(tcard_text(field.second,L"Missing changes:"))+L" "+std::to_wstring(static_cast<unsigned long long>(count));}
    }
    text+=L"\r\n\r\n"+std::wstring(tcard_text(L"sync.check_apply",L"Received changes appear after settings and active editing close."));return text;
}

std::wstring card_format_peers()
{
    std::vector<tcard_log::Event> events;
    if(!tcard_log::Read(g_sync_client.Profile(),events))return tcard_text(L"sync.cache_unavailable",L"Connection log unavailable. Encrypted synchronization storage is required.");
    if(events.empty())return tcard_text(L"sync.cache_empty",L"No connection events recorded.");
    std::wstring text;
    for(auto it=events.rbegin();it!=events.rend();++it){ULARGE_INTEGER ticks{};ticks.QuadPart=it->time*10000000;FILETIME stamp{ticks.LowPart,ticks.HighPart};SYSTEMTIME local{};wchar_t date[64]{};
        if(tcard_ui::convert_time(stamp,local))swprintf_s(date,L"%04u-%02u-%02u %02u:%02u:%02u",local.wYear,local.wMonth,local.wDay,local.wHour,local.wMinute,local.wSecond);
        const std::wstring outcome=tcard_text((L"sync.log_"+it->outcome).c_str(),it->outcome.c_str());
        const std::wstring direction=tcard_text(it->inbound?L"sync.log_inbound":L"sync.log_outbound",it->inbound?L"Incoming":L"Outgoing");
        const std::wstring stage=tcard_text((L"sync.log_"+it->stage).c_str(),it->stage.c_str());
        text+=std::wstring(date)+L"  "+outcome+L"  "+direction+L"  "+it->peer.ip+L":"+std::to_wstring(it->peer.port)+L"  "+stage+L"\r\n";
    }return text;
}
INT_PTR CALLBACK card_handle_peers(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        SetWindowTextW(hwnd,tcard_text(L"sync.cache",L"Connection log"));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            RECT r{x,y,x+width,y+height};MapDialogRect(hwnd,&r);
            HWND child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,
                r.left,r.top,r.right-r.left,r.bottom-r.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);
        };
        control(L"STATIC",tcard_text(L"sync.cache_scope",L"Connection success, failure and disconnection (latest 256 events)."),0,1740,8,8,344,28);
        control(L"EDIT",card_format_peers().c_str(),WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,1741,8,40,344,160);
        control(L"BUTTON",tcard_text(L"sync.cache_refresh",L"Refresh"),WS_TABSTOP|BS_PUSHBUTTON,1742,8,216,80,16);
        control(L"BUTTON",tcard_text(L"sync.cache_clear",L"Clear log"),WS_TABSTOP|BS_PUSHBUTTON,1743,96,216,96,16);
        control(L"BUTTON",tcard_text(L"button.close",L"Close"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,280,216,72,16);
        tcard_ui::prepare_settings(hwnd);return TRUE;
    }
    if(message==WM_COMMAND){
        if(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL){EndDialog(hwnd,IDOK);return TRUE;}
        if(card_signal_busy())return TRUE;
        if(LOWORD(wp)==1742){SetDlgItemTextW(hwnd,1741,card_format_peers().c_str());return TRUE;}
        if(LOWORD(wp)==1743){
            if(MessageBoxW(hwnd,tcard_text(L"sync.cache_confirm",L"Clear the connection log? Endpoint candidates, IP settings, notes and keys are kept."),L"TCard",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return TRUE;
            const auto settings=tcard_bridge::ReadSettings(preferences_path());const bool running=g_sync_client.Running();std::wstring unused;
            const bool okay=g_sync_client.Key(L"log-clear",L"",unused);
            if(running)card_sync_start(hwnd,settings);
            if(!okay)MessageBoxW(hwnd,tcard_text(L"sync.cache_clear_failed",L"Could not clear the connection log. Existing data is preserved."),L"TCard",MB_OK|MB_ICONWARNING);
            SetDlgItemTextW(hwnd,1741,card_format_peers().c_str());return TRUE;
        }
    }
    return FALSE;
}
void card_show_peers(HWND owner)
{
    if(card_signal_busy())return;
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=360;dialog.cy=240;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));data.insert(data.end(),{0,0,0,9});
    const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_handle_peers,0);
}

// Multiline EDIT controls have no native cue banner; paint examples without changing their text.
void card_paint_cue(HWND hwnd,HDC dc)
{
    if(!dc||GetWindowTextLengthW(hwnd))return;
    RECT rect{};SendMessageW(hwnd,EM_GETRECT,0,reinterpret_cast<LPARAM>(&rect));
    const auto font=reinterpret_cast<HFONT>(SendMessageW(hwnd,WM_GETFONT,0,0));
    const auto previous=font?SelectObject(dc,font):nullptr;
    const auto color=SetTextColor(dc,GetSysColor(COLOR_GRAYTEXT));const auto mode=SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,L"192.168.1.10\r\n192.168.1.20:49022",-1,&rect,DT_LEFT|DT_TOP|DT_NOPREFIX);
    SetBkMode(dc,mode);SetTextColor(dc,color);if(previous)SelectObject(dc,previous);
}
LRESULT CALLBACK card_handle_cue(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR)
{
    if(message==WM_NCDESTROY)RemoveWindowSubclass(hwnd,card_handle_cue,id);
    const auto result=DefSubclassProc(hwnd,message,wp,lp);
    if(message==WM_PAINT){HDC dc=GetDC(hwnd);card_paint_cue(hwnd,dc);if(dc)ReleaseDC(hwnd,dc);}
    else if(message==WM_PRINTCLIENT||(message==WM_PRINT&&(lp&PRF_CLIENT)))card_paint_cue(hwnd,reinterpret_cast<HDC>(wp));
    return result;
}

void card_refresh_status(HWND hwnd)
{
    const auto text=card_sync_label();if(window_text(GetDlgItem(hwnd,1712))==text)return;
    SetDlgItemTextW(hwnd,1712,text.c_str());card_sync_layout(hwnd);
}
INT_PTR CALLBACK card_sync_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_DESTROY){delete reinterpret_cast<tcard_bridge::Settings*>(RemovePropW(hwnd,L"TCardSync.SignalDraft"));RemovePropW(hwnd,L"TCardSync.WhiteOpen");RemovePropW(hwnd,L"TCardSync.BlackOpen");}
    if(const INT_PTR painted=tcard_ui::paint_settings(hwnd,message,wp,lp))return painted;
    if(message==WM_INITDIALOG){
        SetWindowTextW(hwnd,tcard_text(L"sync.settings",L"Sync settings"));const auto font=SendMessageW(hwnd,WM_GETFONT,0,0);
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){
            RECT rect{x,y,x+width,y+height};MapDialogRect(hwnd,&rect);
            HWND child=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,
                rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,font,TRUE);return child;
        };
        auto settings=tcard_bridge::ReadSettings(preferences_path());
        auto* draft=new tcard_bridge::Settings(settings);if(!SetPropW(hwnd,L"TCardSync.SignalDraft",reinterpret_cast<HANDLE>(draft))){delete draft;EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(!settings.hasPolicy)settings.whitelist=g_sync_client.DefaultWhitelist();
        settings.peers=tcard_bridge::EditPeers(settings);settings.peerPort=0;
        control(L"STATIC",tcard_text(L"sync.local",L"Synchronize on your local LAN and allowed networks."),0,1700,8,8,304,12);
        control(L"BUTTON",tcard_text(L"sync.enable",L"Enable synchronization"),WS_TABSTOP|BS_AUTOCHECKBOX,1701,8,28,304,14);
        CheckDlgButton(hwnd,1701,settings.enabled?BST_CHECKED:BST_UNCHECKED);
        control(L"STATIC",tcard_text(L"sync.listen_port",L"Connection port"),SS_CENTERIMAGE,1702,8,44,84,14);
        control(L"EDIT",std::to_wstring(settings.listenPort).c_str(),WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,1703,96,44,64,14);
        control(L"STATIC",tcard_text(L"sync.port_help",L"This device's listening port"),SS_CENTERIMAGE,1704,168,44,144,14);
        control(L"BUTTON",tcard_text(L"sync.remote_admission",L"Allow connections outside the LAN"),WS_TABSTOP|BS_AUTOCHECKBOX,1734,8,60,140,14);
        CheckDlgButton(hwnd,1734,settings.remoteAdmission?BST_CHECKED:BST_UNCHECKED);
        control(L"STATIC",tcard_text(L"sync.remote_help",L"Whitelist entry required"),SS_CENTERIMAGE,1705,152,60,160,14);
        control(L"STATIC",tcard_text(L"sync.peers",L"Priority destinations"),0,1714,8,84,216,16);
        control(L"BUTTON",tcard_text(L"sync.signaling",L"Signaling..."),WS_TABSTOP|BS_PUSHBUTTON,1735,232,84,80,16);
        const DWORD listStyle=WS_TABSTOP|ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|WS_VSCROLL;
        const auto peers=control(L"EDIT",tcard_bridge::EditLines(settings.peers).c_str(),listStyle,1715,8,104,304,32);
        SetWindowSubclass(peers,card_handle_cue,1,0);
        control(L"BUTTON",L"",WS_TABSTOP|BS_PUSHBUTTON|BS_LEFT|BS_FLAT,1716,8,160,304,16);
        control(L"EDIT",tcard_bridge::EditLines(settings.whitelist).c_str(),listStyle,1717,8,176,304,32);
        control(L"BUTTON",L"",WS_TABSTOP|BS_PUSHBUTTON|BS_LEFT|BS_FLAT,1718,8,216,304,16);
        control(L"EDIT",tcard_bridge::EditLines(settings.blacklist).c_str(),listStyle,1719,8,232,304,32);
        control(L"BUTTON",tcard_text(L"sync.share_peers",L"Share and use endpoints with authenticated sync partners"),WS_TABSTOP|BS_AUTOCHECKBOX,1733,8,272,304,14);
        CheckDlgButton(hwnd,1733,settings.sharePeers?BST_CHECKED:BST_UNCHECKED);
        std::wstring sharingHelp=tcard_text(L"sync.share_help",L"Exchange endpoint information with sync partners and use it as candidates.\nEndpoints are valid for up to 30 days. No notes are transferred before authentication.");
        if(const auto line=sharingHelp.find(L"\\n");line!=std::wstring::npos)sharingHelp.replace(line,2,L"\r\n");
        control(L"STATIC",sharingHelp.c_str(),0,1706,8,288,304,24);
        control(L"BUTTON",tcard_text(L"sync.key_button",L"Key display / update"),WS_TABSTOP|BS_PUSHBUTTON,1730,8,320,100,16);
        control(L"BUTTON",tcard_text(L"sync.check",L"Check synchronization"),WS_TABSTOP|BS_PUSHBUTTON,1731,116,320,88,16);
        control(L"BUTTON",tcard_text(L"sync.cache",L"Connection log"),WS_TABSTOP|BS_PUSHBUTTON,1732,212,320,100,16);
        control(L"STATIC",tcard_text(L"sync.folder",L"Synchronization folder"),0,1710,8,344,304,12);
        control(L"STATIC",g_sync_client.Profile().c_str(),SS_LEFT|SS_NOPREFIX|SS_NOTIFY,1711,8,360,304,24);
        control(L"STATIC",card_sync_label().c_str(),0,1712,8,392,304,24);
        control(L"BUTTON",tcard_text(L"button.save",L"Save"),WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK,204,416,52,16);
        control(L"BUTTON",tcard_text(L"button.cancel",L"Cancel"),WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL,260,416,52,16);
        SendDlgItemMessageW(hwnd,1703,EM_SETLIMITTEXT,5,0);
        for(int id:{1715,1717,1719})SendDlgItemMessageW(hwnd,id,EM_SETLIMITTEXT,4096,0);
        ShowWindow(GetDlgItem(hwnd,1735),card_signal_available()?SW_SHOWNA:SW_HIDE);
        card_sync_allow(hwnd);card_sync_layout(hwnd);SetTimer(hwnd,1,500,nullptr);tcard_ui::prepare_settings(hwnd);return TRUE;
    }
    if(message==WM_CONTEXTMENU&&reinterpret_cast<HWND>(wp)==GetDlgItem(hwnd,1711)){
        HMENU menu=CreatePopupMenu();if(!menu)return TRUE;
        AppendMenuW(menu,MF_STRING,1,tcard_text(L"sync.folder_copy",L"Copy folder path"));
        POINT point{static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp))};if(point.x==-1&&point.y==-1){RECT rect{};GetWindowRect(GetDlgItem(hwnd,1711),&rect);point={rect.left,rect.bottom};}
        const auto choice=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,hwnd,nullptr);DestroyMenu(menu);if(choice==1)card_copy_folder(hwnd);return TRUE;
    }
    if(message==WM_COMMAND&&LOWORD(wp)==1715&&HIWORD(wp)==EN_CHANGE){InvalidateRect(GetDlgItem(hwnd,1715),nullptr,TRUE);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wp)==1715&&HIWORD(wp)==EN_KILLFOCUS){card_sync_allow(hwnd);return TRUE;}
    if(message==WM_COMMAND&&(LOWORD(wp)==1716||LOWORD(wp)==1718)&&HIWORD(wp)==BN_CLICKED){
        const auto name=LOWORD(wp)==1716?L"TCardSync.WhiteOpen":L"TCardSync.BlackOpen";
        if(GetPropW(hwnd,name))RemovePropW(hwnd,name);else SetPropW(hwnd,name,reinterpret_cast<HANDLE>(1));card_sync_layout(hwnd);return TRUE;
    }
    if(card_signal_busy()&&(message==WM_TIMER||message==WM_COMMAND)){
        if(message==WM_COMMAND&&LOWORD(wp)==IDCANCEL)EndDialog(hwnd,IDCANCEL);
        else card_refresh_status(hwnd);return TRUE;
    }
    if(message==WM_TIMER&&wp==2){
        try{
            if(!g_sync_client.Running()||g_sync_client.status==L"sync.port_busy"||g_sync_client.status==L"sync.failed"){
                KillTimer(hwnd,2);RemovePropW(hwnd,L"TCardSync.Check");EnableWindow(GetDlgItem(hwnd,1731),TRUE);MessageBoxW(hwnd,card_sync_label().c_str(),L"TCard",MB_OK|MB_ICONWARNING);return TRUE;
            }
            if(GetPropW(hwnd,L"TCardSync.Check")==reinterpret_cast<HANDLE>(1)){
                if(g_sync_client.Check(g_file_path))SetPropW(hwnd,L"TCardSync.Check",reinterpret_cast<HANDLE>(2));
            }else{winrt::Windows::Data::Json::JsonObject result;if(g_sync_client.CheckResult(result)){
                KillTimer(hwnd,2);RemovePropW(hwnd,L"TCardSync.Check");EnableWindow(GetDlgItem(hwnd,1731),TRUE);
                // Localization returns a transient buffer: own both strings before invoking the modal API.
                const auto detail=card_sync_result(result);const std::wstring caption=tcard_text(L"sync.check",L"Sync check");
                MessageBoxW(hwnd,detail.c_str(),caption.c_str(),MB_OK|MB_ICONINFORMATION);
            }}
        }catch(...){KillTimer(hwnd,2);RemovePropW(hwnd,L"TCardSync.Check");EnableWindow(GetDlgItem(hwnd,1731),TRUE);}
        return TRUE;
    }
    if(message==WM_TIMER){
        ShowWindow(GetDlgItem(hwnd,1735),card_signal_available()?SW_SHOWNA:SW_HIDE);
        try{g_sync_client.Ready();}catch(...){g_sync_client.status=L"sync.failed";}
        card_refresh_status(hwnd);return TRUE;
    }
    if(message==WM_DESTROY){KillTimer(hwnd,1);KillTimer(hwnd,2);RemovePropW(hwnd,L"TCardSync.Check");return TRUE;}
    if(message==WM_COMMAND){
        if(LOWORD(wp)==1735){if(!card_signal_available())return TRUE;if(auto* draft=reinterpret_cast<tcard_bridge::Settings*>(GetPropW(hwnd,L"TCardSync.SignalDraft"))){auto edited=*draft;card_show_signal_settings(hwnd,edited);if(IsWindow(hwnd)&&GetPropW(hwnd,L"TCardSync.SignalDraft")==reinterpret_cast<HANDLE>(draft))*draft=std::move(edited);}return TRUE;}
        if(LOWORD(wp)==1732){card_show_peers(hwnd);return TRUE;}
        if(LOWORD(wp)==1730){KillTimer(hwnd,2);RemovePropW(hwnd,L"TCardSync.Check");EnableWindow(GetDlgItem(hwnd,1731),TRUE);card_show_keys(hwnd);return TRUE;}
        if(LOWORD(wp)==1731){
            if(IsDlgButtonChecked(hwnd,1701)!=BST_CHECKED){MessageBoxW(hwnd,tcard_text(L"sync.check_enable",L"Enable synchronization before checking."),L"TCard",MB_OK|MB_ICONINFORMATION);return TRUE;}
            if(card_sync_save(hwnd)){SetPropW(hwnd,L"TCardSync.Check",reinterpret_cast<HANDLE>(1));EnableWindow(GetDlgItem(hwnd,1731),FALSE);SetTimer(hwnd,2,300,nullptr);}return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL){EndDialog(hwnd,IDCANCEL);return TRUE;}
        if(LOWORD(wp)==IDOK){
            if(card_sync_save(hwnd))EndDialog(hwnd,IDOK);return TRUE;
        }
    }
    return FALSE;
}
void card_show_sync(HWND owner)
{
    if(card_signal_busy())return;
    if(!g_sync_client.Available())return;
    DLGTEMPLATE dialog{};dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_SETFONT;dialog.cx=320;dialog.cy=360;
    std::vector<WORD> data(sizeof(dialog)/sizeof(WORD));memcpy(data.data(),&dialog,sizeof(dialog));data.insert(data.end(),{0,0,0,9});
    const wchar_t face[]=L"Segoe UI";data.insert(data.end(),face,face+ARRAYSIZE(face));
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<const DLGTEMPLATE*>(data.data()),owner,card_sync_proc,0);
}
