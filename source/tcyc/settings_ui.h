#pragma once

// Native view for the approved TCycle mock. Existing controls own config semantics.
static std::wstring tcyc_get_text(WindowState* st,const wchar_t* key,const wchar_t* en,const wchar_t* ja)
{
    return Tr(st,key,st->languageCode=="ja"?ja:en);
}
static int tcyc_scale(WindowState* st,int value){return MulDiv(value,static_cast<int>(st->dpi),96);}
static HWND tcyc_get_control(WindowState* st,int id)
{
    HWND control=GetDlgItem(st->panel,id);
    return control?control:GetDlgItem(st->mainWindow,id);
}
static void tcyc_set_label(WindowState* st,int id,const std::wstring& value)
{
    HWND control=tcyc_get_control(st,id);
    if(GetEditText(control)!=value)SetWindowTextW(control,value.c_str());
}
static void tcyc_fill(HDC dc,RECT rect,COLORREF color)
{
    SetDCBrushColor(dc,color);FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
static LRESULT CALLBACK tcyc_check_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)
{
    if(msg==BM_GETCHECK)return GetPropW(hwnd,L"TCycleChecked")?BST_CHECKED:BST_UNCHECKED;
    if(msg==BM_SETCHECK){
        if(wp==BST_CHECKED)SetPropW(hwnd,L"TCycleChecked",reinterpret_cast<HANDLE>(1));
        else RemovePropW(hwnd,L"TCycleChecked");
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    if(msg==WM_NCDESTROY){RemovePropW(hwnd,L"TCycleChecked");RemoveWindowSubclass(hwnd,tcyc_check_proc,1);}
    return DefSubclassProc(hwnd,msg,wp,lp);
}
static LRESULT CALLBACK tcyc_panel_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    auto* st=reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE){st=static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(st));}
    if(!st)return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg){
    case WM_COMMAND:
        if(HIWORD(wp)==EN_SETFOCUS||HIWORD(wp)==CBN_SETFOCUS||HIWORD(wp)==BN_SETFOCUS){
            RECT r{},client{};GetWindowRect(reinterpret_cast<HWND>(lp),&r);
            MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&r),2);GetClientRect(hwnd,&client);
            int delta=r.top<0?r.top:r.bottom>client.bottom?r.bottom-client.bottom:0;
            if(delta){st->scrollOffset+=delta;tcyc_layout_controls(st);}
        }
        return SendMessageW(st->mainWindow,msg,wp,lp);
    case WM_NOTIFY:case WM_DRAWITEM:case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:
        return SendMessageW(st->mainWindow,msg,wp,lp);
    case WM_VSCROLL:{
        SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&si);int next=si.nPos;
        switch(LOWORD(wp)){
        case SB_LINEUP:next-=tcyc_scale(st,24);break;
        case SB_LINEDOWN:next+=tcyc_scale(st,24);break;
        case SB_PAGEUP:next-=static_cast<int>(si.nPage);break;
        case SB_PAGEDOWN:next+=static_cast<int>(si.nPage);break;
        case SB_THUMBTRACK:next=si.nTrackPos;break;
        case SB_TOP:next=0;break;
        case SB_BOTTOM:next=si.nMax;break;
        default:return 0;
        }
        st->scrollOffset=std::clamp(next,0,std::max(0,si.nMax-static_cast<int>(si.nPage)+1));tcyc_layout_controls(st,true);return 0;
    }
    case WM_MOUSEWHEEL:
        st->scrollOffset=std::max(0,st->scrollOffset-MulDiv(GET_WHEEL_DELTA_WPARAM(wp),tcyc_scale(st,60),WHEEL_DELTA));
        tcyc_layout_controls(st,true);return 0;
    case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);tcyc_paint_surface(st,hwnd,ps.hdc);EndPaint(hwnd,&ps);return 0;}
    case WM_ERASEBKGND:return 1;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void tcyc_refresh_fonts(WindowState* st)
{
    st->dpi=GetDpiForWindow(st->mainWindow);if(!st->dpi)st->dpi=96;
    HFONT previous[]={st->uiFont,st->smallFont,st->titleFont};
    const auto family=st->languageCode=="ja"?L"Yu Gothic UI":L"Segoe UI";
    auto create=[&](int size,int weight){return CreateFontW(-tcyc_scale(st,size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);};
    st->uiFont=create(14,FW_NORMAL);st->smallFont=create(12,FW_NORMAL);st->titleFont=create(22,FW_SEMIBOLD);
    EnumChildWindows(st->mainWindow,[](HWND control,LPARAM value)->BOOL{
        auto* state=reinterpret_cast<WindowState*>(value);int id=GetDlgCtrlID(control);
        HFONT font=id==2002?state->titleFont:(id==kCtrlStatus||id==2003||id==2005||id==2011||id==2043||id==2062||id==2064||id==2073||id==2090)?state->smallFont:state->uiFont;
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return TRUE;
    },reinterpret_cast<LPARAM>(st));
    SendMessageW(st->taskList,LB_SETITEMHEIGHT,0,tcyc_scale(st,54));
    SendMessageW(st->hotkeyMod,CB_SETDROPPEDWIDTH,tcyc_scale(st,260),0);
    for(HFONT font:previous)if(font)DeleteObject(font);
}
static void tcyc_place(WindowState* st,int id,int x,int y,int width,int height)
{
    if(HWND control=tcyc_get_control(st,id))st->placements.push_back({control,x,y,width,height});
}
void tcyc_layout_controls(WindowState* st,bool scrolling)
{
    if(!st||!st->panel||st->arranging)return;st->arranging=true;
    RECT client{};GetClientRect(st->mainWindow,&client);
    const int width=MulDiv(client.right,96,st->dpi),height=MulDiv(client.bottom,96,st->dpi),side=164,footer=54;
    const int body=std::max(100,height-footer),pad=16;
    SetWindowPos(st->panel,nullptr,tcyc_scale(st,side),0,tcyc_scale(st,width-side),tcyc_scale(st,body),SWP_NOZORDER|SWP_NOACTIVATE);
    // Reserve scrollbar width consistently so conditional sections never move horizontally.
    const int right=width-side-pad-MulDiv(GetSystemMetricsForDpi(SM_CXVSCROLL,st->dpi),96,st->dpi),field=right-pad;
    st->placements.clear();st->blocks.clear();st->separators.clear();
    tcyc_place(st,2005,12,12,112,24);tcyc_place(st,kCtrlTaskAdd,128,10,28,28);
    tcyc_place(st,kCtrlTaskList,8,46,148,body-92);
    tcyc_place(st,2000,8,body-38,148,28);
    tcyc_place(st,kCtrlStatus,14,height-43,width-194,34);
    if(!st->globalView)tcyc_place(st,kCtrlTaskTestRun,width-160,height-43,146,32);
    tcyc_place(st,2003,pad,14,field,17);
    tcyc_place(st,2002,pad,32,st->globalView?field:field-166,32);
    int content=0;
    if(st->globalView){
        tcyc_place(st,2060,pad,80,field,32);st->separators.push_back(122);
        tcyc_place(st,2061,pad,140,field-140,24);tcyc_place(st,2062,pad,170,field-140,40);
        tcyc_place(st,kCtrlPollSec,right-120,144,80,30);tcyc_place(st,kCtrlSpinPollSec,right-58,144,18,30);tcyc_place(st,2065,right-32,145,32,28);
        st->separators.push_back(226);
        tcyc_place(st,2063,pad,244,field-140,24);tcyc_place(st,2064,pad,274,field-140,48);
        tcyc_place(st,kCtrlGraceSec,right-120,248,80,30);tcyc_place(st,kCtrlSpinGraceSec,right-58,248,18,30);tcyc_place(st,2066,right-32,249,32,28);
        content=340;
    }else{
        tcyc_place(st,kCtrlTaskRename,right-158,33,102,30);tcyc_place(st,kCtrlTaskDelete,right-50,33,50,30);
        tcyc_place(st,kCtrlTaskEnabled,pad,74,field-120,26);tcyc_place(st,2004,right-110,75,110,24);
        st->separators.push_back(110);
        tcyc_place(st,2010,pad,122,field-155,24);tcyc_place(st,2011,right-150,124,150,24);
        const int card=(field-5*4)/5;
        const int ids[]={kCtrlTriggerStartup,kCtrlTriggerInterval,kCtrlTriggerWeeklyTime,kCtrlTriggerHotkeyOnly,kCtrlTriggerNonRunning};
        for(int i=0;i<5;++i)tcyc_place(st,ids[i],pad+i*(card+5),154,card,62);
        int y=224;
        auto block=[&](int top,int tall){st->blocks.push_back({pad,top,right,top+tall});};
        if(IsChecked(st->triggerChecks[3])){
            block(y,38);tcyc_place(st,2050,pad+12,y+5,field-24,28);y+=44;
        }
        if(IsChecked(st->triggerChecks[0])){
            block(y,48);tcyc_place(st,2030,pad+12,y+10,94,26);
            tcyc_place(st,kCtrlIntervalSec,pad+116,y+8,84,30);tcyc_place(st,kCtrlSpinIntervalSec,pad+200,y+8,18,30);
            tcyc_place(st,2031,pad+230,y+10,field-242,26);y+=54;
        }
        if(IsChecked(st->triggerChecks[2])){
            const bool date=IsChecked(st->dateEnabled),week=IsChecked(st->weekdayEnabled)&&!IsChecked(st->weekdayEveryday);
            const int tall=date||week?92:48;block(y,tall);
            tcyc_place(st,2020,pad+12,y+10,80,26);tcyc_place(st,2021,pad+96,y+8,150,30);
            tcyc_place(st,2022,pad+260,y+10,48,26);tcyc_place(st,kCtrlTimeOfDay,pad+318,y+8,104,30);
            if(date){tcyc_place(st,2023,pad+12,y+54,80,26);tcyc_place(st,kCtrlDateValue,pad+96,y+52,150,30);}
            if(week){for(int i=0;i<7;++i)tcyc_place(st,kCtrlWeekdaySun+i,pad+12+i*44,y+52,36,30);}
            y+=tall+6;
        }
        if(IsChecked(st->triggerChecks[4])){
            block(y,78);tcyc_place(st,2080,pad+12,y+8,field-24,24);
            const int modWidth=std::min(252,field-182);
            tcyc_place(st,kCtrlHotkeyMod,pad+12,y+38,modWidth,30);
            tcyc_place(st,kCtrlHotkeyKey,pad+modWidth+30,y+38,128,30);y+=84;
        }
        if(IsChecked(st->triggerChecks[5])){
            block(y,132);tcyc_place(st,2070,pad+12,y+8,field-24,28);
            tcyc_place(st,2071,pad+12,y+41,116,26);tcyc_place(st,kCtrlWatchdogRetrySec,pad+140,y+39,84,30);tcyc_place(st,kCtrlSpinWatchdogRetrySec,pad+224,y+39,18,30);
            tcyc_place(st,2072,pad+12,y+76,116,26);tcyc_place(st,kCtrlRepeatCount,pad+140,y+74,100,30);tcyc_place(st,kCtrlSpinRepeatCount,pad+240,y+74,18,30);
            tcyc_place(st,2073,pad+12,y+108,field-24,24);y+=138;
        }
        if(st->selectedTask>=0&&st->selectedTask<static_cast<int>(st->config.tasks.size())&&(st->config.tasks[st->selectedTask].triggerMask&(1<<1))){
            tcyc_place(st,2090,pad,y,field,38);y+=48;
        }
        st->separators.push_back(y+2);y+=14;
        tcyc_place(st,2040,pad,y,field-225,30);tcyc_place(st,kCtrlActionMode,right-210,y,210,30);
        tcyc_place(st,2044,pad,y+36,field,23);tcyc_place(st,kCtrlActionPath,pad,y+58,field,32);
        const int half=(field-14)/2;
        tcyc_place(st,2041,pad,y+100,half,23);tcyc_place(st,2042,pad+half+14,y+100,half,23);
        tcyc_place(st,kCtrlActionArgs,pad,y+122,half,32);tcyc_place(st,kCtrlActionCwd,pad+half+14,y+122,half,32);
        tcyc_place(st,kCtrlSingleInstance,pad,y+164,field,26);tcyc_place(st,2043,pad,y+196,field,40);
        content=y+246;
    }
    st->contentHeight=tcyc_scale(st,content);
    SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS};
    si.nMax=st->contentHeight-1;si.nPage=static_cast<UINT>(tcyc_scale(st,body));
    st->scrollOffset=std::clamp(st->scrollOffset,0,std::max(0,st->contentHeight-static_cast<int>(si.nPage)));si.nPos=st->scrollOffset;
    SetScrollInfo(st->panel,SB_VERT,&si,TRUE);
    EnumChildWindows(st->mainWindow,[](HWND child,LPARAM param)->BOOL{
        auto* state=reinterpret_cast<WindowState*>(param);
        if(child==state->panel||GetParent(child)!=state->panel)return TRUE;
        bool visible=std::any_of(state->placements.begin(),state->placements.end(),[&](const WindowState::Placement& p){return p.control==child;});
        if(!visible&&IsWindowVisible(child))ShowWindow(child,SW_HIDE);return TRUE;
    },reinterpret_cast<LPARAM>(st));
    ShowWindow(st->taskTestRun,st->globalView?SW_HIDE:SW_SHOW);
    for(const auto& p:st->placements){
        const int top=tcyc_scale(st,p.y)-(GetParent(p.control)==st->panel?st->scrollOffset:0);
        wchar_t cls[32]{};GetClassNameW(p.control,cls,32);
        const int tall=tcyc_scale(st,wcscmp(cls,L"ComboBox")==0?240:p.height);
        SetWindowPos(p.control,nullptr,tcyc_scale(st,p.x),top,tcyc_scale(st,p.width),tall,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|(GetParent(p.control)==st->panel?SWP_NOREDRAW:0)|(GetParent(p.control)==st->panel&&scrolling?SWP_NOSIZE:0)|SWP_SHOWWINDOW);
    }
    st->arranging=false;
    InvalidateRect(st->mainWindow,nullptr,FALSE);
    // Repaint every child after placement; clipped glyph pixels must not survive scrolling.
    // Buffer the panel and present one complete frame after all child moves.
    RedrawWindow(st->panel,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME|RDW_UPDATENOW);
}

static LRESULT tcyc_control_color(WindowState* st,UINT,WPARAM wp,LPARAM lp)
{
    HDC dc=reinterpret_cast<HDC>(wp);HWND control=reinterpret_cast<HWND>(lp);
    COLORREF color=RGB(255,255,255);
    if(GetParent(control)==st->panel){
        RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,st->panel,reinterpret_cast<POINT*>(&r),2);
        int y=MulDiv(r.top+st->scrollOffset,96,st->dpi);
        wchar_t cls[32]{};GetClassNameW(control,cls,32);
        if(wcscmp(cls,L"Static")==0||wcscmp(cls,L"Button")==0)
            for(const auto& b:st->blocks)if(y>=b.top&&y<b.bottom)color=RGB(246,248,251);
    }else if(control!=st->status)color=RGB(247,249,252);
    SetTextColor(dc,IsWindowEnabled(control)?RGB(40,48,64):RGB(135,141,151));
    SetBkColor(dc,color);SetDCBrushColor(dc,color);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}
void tcyc_paint_surface(WindowState* st,HWND hwnd,HDC dc)
{
    RECT r{};GetClientRect(hwnd,&r);tcyc_fill(dc,r,RGB(255,255,255));
    if(hwnd==st->panel){
        for(auto b:st->blocks){b={tcyc_scale(st,b.left),tcyc_scale(st,b.top)-st->scrollOffset,tcyc_scale(st,b.right),tcyc_scale(st,b.bottom)-st->scrollOffset};tcyc_fill(dc,b,RGB(246,248,251));}
        for(int y:st->separators){RECT line{tcyc_scale(st,16),tcyc_scale(st,y)-st->scrollOffset,r.right-tcyc_scale(st,16),tcyc_scale(st,y)-st->scrollOffset+1};tcyc_fill(dc,line,RGB(229,233,239));}
    }else{
        RECT side{0,0,tcyc_scale(st,164),r.bottom-tcyc_scale(st,54)};tcyc_fill(dc,side,RGB(247,249,252));
        RECT line{0,side.bottom,r.right,side.bottom+1};tcyc_fill(dc,line,RGB(225,230,237));
    }
}
static void tcyc_draw_icon(WindowState* st,HDC dc,int id,int cx,int cy,COLORREF color)
{
    struct GraphicsRuntime{ULONG_PTR token=0;GraphicsRuntime(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);}~GraphicsRuntime(){if(token)Gdiplus::GdiplusShutdown(token);}};
    static GraphicsRuntime runtime;
    Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.TranslateTransform(static_cast<float>(cx),static_cast<float>(cy));g.ScaleTransform(st->dpi/96.f,st->dpi/96.f);
    Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(color),GetGValue(color),GetBValue(color)),1.6f);
    if(id==kCtrlTriggerStartup){g.DrawArc(&pen,-9,-9,18,18,305,290);g.DrawLine(&pen,0,-12,0,0);}
    else if(id==kCtrlTriggerInterval){g.DrawEllipse(&pen,-10,-10,20,20);g.DrawLine(&pen,0,-6,0,0);g.DrawLine(&pen,0,0,5,3);}
    else if(id==kCtrlTriggerWeeklyTime){g.DrawRectangle(&pen,-10,-8,20,18);g.DrawLine(&pen,-10,-2,10,-2);g.DrawLine(&pen,-5,-11,-5,-5);g.DrawLine(&pen,5,-11,5,-5);g.DrawLine(&pen,-5,3,-2,3);g.DrawLine(&pen,3,3,6,3);}
    else if(id==kCtrlTriggerHotkeyOnly){g.DrawRectangle(&pen,-12,-7,24,15);for(int x=-8;x<=8;x+=4)g.DrawLine(&pen,x,-2,x,0);g.DrawLine(&pen,-5,4,5,4);}
    else {g.DrawArc(&pen,-12,-8,24,16,0,180);g.DrawArc(&pen,-12,-8,24,16,180,180);g.DrawEllipse(&pen,-4,-4,8,8);}
}
bool tcyc_draw_item(WindowState* st,const DRAWITEMSTRUCT& value)
{
    const auto* item=&value;
    if(item->itemID==static_cast<UINT>(-1))return true;
    HDC dc=item->hDC;RECT r=item->rcItem;
    const bool list=item->CtlID==kCtrlTaskList;
    const bool trigger=item->CtlID==kCtrlTriggerStartup||item->CtlID==kCtrlTriggerInterval||item->CtlID==kCtrlTriggerWeeklyTime||item->CtlID==kCtrlTriggerHotkeyOnly||item->CtlID==kCtrlTriggerNonRunning;
    const bool selected=list?(item->itemState&ODS_SELECTED)!=0:IsChecked(item->hwndItem)||(item->CtlID==2000&&st->globalView);
    const bool primary=item->CtlID==kCtrlTaskTestRun;
    COLORREF bg=primary?RGB(48,104,217):selected?RGB(232,240,255):list?RGB(247,249,252):RGB(246,248,251);
    COLORREF fg=(item->itemState&ODS_DISABLED)?RGB(145,151,162):primary?RGB(255,255,255):selected?RGB(39,91,184):RGB(53,63,80);
    tcyc_fill(dc,r,bg);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,fg);
    auto old=SelectObject(dc,st->uiFont);
    if(list&&item->itemID<st->config.tasks.size()){
        const auto& task=st->config.tasks[item->itemID];
        if(selected){RECT bar=r;bar.right=bar.left+tcyc_scale(st,3);tcyc_fill(dc,bar,RGB(48,104,217));}
        RECT name=r;name.left+=tcyc_scale(st,12);name.right-=tcyc_scale(st,8);name.top+=tcyc_scale(st,5);name.bottom=name.top+tcyc_scale(st,25);
        DrawTextW(dc,task.name.c_str(),-1,&name,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        std::wstring detail=task.enabled?tcyc_get_text(st,L"ui_active",L"Enabled",L"\u6709\u52b9"):tcyc_get_text(st,L"ui_inactive",L"Disabled",L"\u7121\u52b9");
        if(task.watchdogEnabled)detail+=L" / "+tcyc_get_text(st,L"ui_monitor",L"Monitoring",L"\u76e3\u8996")+L" "+std::to_wstring(ReadWatchdogCountdownSec(st->config.stateFile,task.id,task.watchdogRetrySec))+L"s";
        else if(task.triggerMask&(1<<2))detail+=L" / "+tcyc_get_text(st,L"ui_schedule",L"Schedule",L"\u65e5\u6642");
        else if(task.triggerMask&1)detail+=L" / "+std::to_wstring(task.intervalSec)+L"s";
        else if(task.triggerMask&(1<<3))detail+=L" / "+tcyc_get_text(st,L"ui_startup",L"Startup",L"\u8d77\u52d5\u6642");
        SelectObject(dc,st->smallFont);SetTextColor(dc,RGB(111,122,140));name.top+=tcyc_scale(st,24);name.bottom+=tcyc_scale(st,24);
        DrawTextW(dc,detail.c_str(),-1,&name,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    }else{
        RECT textRect=r;textRect.left+=tcyc_scale(st,5);textRect.right-=tcyc_scale(st,5);
        if(trigger){tcyc_draw_icon(st,dc,item->CtlID,(r.left+r.right)/2,r.top+tcyc_scale(st,18),fg);textRect.top+=tcyc_scale(st,34);}
        std::wstring label=GetEditText(item->hwndItem);
        DrawTextW(dc,label.c_str(),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }
    if(item->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(dc,&r);}
    SelectObject(dc,old);return true;
}
void tcyc_sync_controls(WindowState* st)
{
    if(!st||!st->panel)return;
    const bool valid=st->selectedTask>=0&&st->selectedTask<static_cast<int>(st->config.tasks.size());
    tcyc_set_label(st,2002,st->globalView?tcyc_get_text(st,L"ui_global",L"Global settings",L"\u5168\u4f53\u8a2d\u5b9a"):valid?st->config.tasks[st->selectedTask].name:L"TCycle");
    tcyc_set_label(st,2003,st->globalView?L"TCYCLE / SETTINGS":L"TCYCLE / TASK");
    tcyc_set_label(st,2004,IsChecked(st->taskEnabled)?tcyc_get_text(st,L"ui_saved",L"Auto saved",L"\u81ea\u52d5\u4fdd\u5b58"):tcyc_get_text(st,L"ui_disabled",L"Disabled",L"\u7121\u52b9"));
    SendMessageW(tcyc_get_control(st,2021),CB_SETCURSEL,IsChecked(st->dateEnabled)?2:IsChecked(st->weekdayEnabled)&&!IsChecked(st->weekdayEveryday)?1:0,0);
    tcyc_layout_controls(st);InvalidateRect(st->taskList,nullptr,FALSE);InvalidateRect(tcyc_get_control(st,2000),nullptr,FALSE);
}
bool tcyc_handle_command(WindowState* st,WPARAM wp,LPARAM)
{
    int id=LOWORD(wp),code=HIWORD(wp);
    if(id==2000&&code==BN_CLICKED){st->globalView=true;st->scrollOffset=0;tcyc_sync_controls(st);return true;}
    if(id==2021&&code==CBN_SELCHANGE){
        int choice=static_cast<int>(SendMessageW(tcyc_get_control(st,2021),CB_GETCURSEL,0,0));
        SetChecked(st->dateEnabled,choice==2);SetChecked(st->weekdayEnabled,choice==1);SetChecked(st->weekdayEveryday,choice==0);SetChecked(st->timeEnabled,true);
        st->dateEnabledDirty=st->weekdayDirty=st->timeEnabledDirty=true;
        ApplyTriggerUiState(st);PersistRealtime(st,false);return true;
    }
    if(code==BN_CLICKED){
        for(HWND control:st->triggerChecks)if(control&&GetDlgCtrlID(control)==id){SetChecked(control,!IsChecked(control));return false;}
        for(HWND control:st->weekdayChecks)if(control&&GetDlgCtrlID(control)==id){SetChecked(control,!IsChecked(control));return false;}
    }
    return false;
}

static void tcyc_build_controls(WindowState* st)
{
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.lpfnWndProc=tcyc_panel_proc;wc.hInstance=instance;wc.lpszClassName=L"TCycleSettingsPanel";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    st->panel=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_COMPOSITED,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_VSCROLL,0,0,0,0,st->mainWindow,nullptr,instance,st);
    auto make=[&](int id,const wchar_t* cls,DWORD style,const wchar_t* text,bool root=false)->HWND{
        return CreateWindowExW(wcscmp(cls,L"Edit")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|style,0,0,0,0,root?st->mainWindow:st->panel,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    };
    auto label=[&](int id,const wchar_t* en,const wchar_t* ja,bool root=false){
        auto text=tcyc_get_text(st,(L"ui_label_"+std::to_wstring(id)).c_str(),en,ja);return make(id,L"Static",SS_LEFT|SS_NOPREFIX|(id==2002?SS_ENDELLIPSIS:0),text.c_str(),root);
    };
    auto button=[&](int id,const wchar_t* en,const wchar_t* ja,bool root=false,bool check=false){
        auto text=tcyc_get_text(st,(L"ui_button_"+std::to_wstring(id)).c_str(),en,ja);
        HWND control=make(id,L"Button",WS_TABSTOP|BS_NOTIFY|BS_OWNERDRAW,text.c_str(),root);
        if(check)SetWindowSubclass(control,tcyc_check_proc,1,0);return control;
    };
    auto edit=[&](int id,bool number=false){return make(id,L"Edit",WS_TABSTOP|ES_AUTOHSCROLL|(number?ES_NUMBER:0),L"");};
    auto combo=[&](int id){return make(id,L"ComboBox",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,L"");};
    auto check=[&](int id,const wchar_t* en,const wchar_t* ja){
        auto text=tcyc_get_text(st,(L"ui_check_"+std::to_wstring(id)).c_str(),en,ja);
        return make(id,L"Button",WS_TABSTOP|BS_NOTIFY|BS_AUTOCHECKBOX,text.c_str());
    };
    auto spin=[&](int id,HWND buddy,int low,int high){
        HWND control=make(id,UPDOWN_CLASSW,UDS_ARROWKEYS|UDS_SETBUDDYINT,L"");
        SendMessageW(control,UDM_SETBUDDY,reinterpret_cast<WPARAM>(buddy),0);SendMessageW(control,UDM_SETRANGE32,low,high);
    };
    label(2005,L"TASKS",L"\u30bf\u30b9\u30af",true);
    st->taskList=make(kCtrlTaskList,L"ListBox",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,L"",true);
    st->taskAdd=button(kCtrlTaskAdd,L"+",L"+",true);
    button(2000,L"Global settings",L"\u5168\u4f53\u8a2d\u5b9a",true);
    st->status=label(kCtrlStatus,L"",L"",true);
    st->taskTestRun=button(kCtrlTaskTestRun,L"Run now",L"\u30c6\u30b9\u30c8\u5b9f\u884c",true);
    label(2002,L"",L"");label(2003,L"",L"");label(2004,L"",L"");
    st->taskRename=button(kCtrlTaskRename,L"Rename",L"\u540d\u524d\u3092\u5909\u66f4");
    st->taskDelete=button(kCtrlTaskDelete,L"Delete",L"\u524a\u9664");
    st->taskEnabled=check(kCtrlTaskEnabled,L"Enable this task",L"\u3053\u306e\u30bf\u30b9\u30af\u3092\u6709\u52b9\u306b\u3059\u308b");
    label(2010,L"When to run",L"\u5b9f\u884c\u3059\u308b\u30bf\u30a4\u30df\u30f3\u30b0");label(2011,L"Multiple selections",L"\u8907\u6570\u9078\u629e\u53ef");
    st->triggerChecks[3]=button(kCtrlTriggerStartup,L"Startup",L"\u8d77\u52d5\u6642",false,true);
    st->triggerChecks[0]=button(kCtrlTriggerInterval,L"Interval",L"\u4e00\u5b9a\u9593\u9694",false,true);
    st->triggerChecks[2]=button(kCtrlTriggerWeeklyTime,L"Schedule",L"\u65e5\u6642\u6307\u5b9a",false,true);
    st->triggerChecks[4]=button(kCtrlTriggerHotkeyOnly,L"Hotkey",L"\u30ad\u30fc\u64cd\u4f5c",false,true);
    st->triggerChecks[5]=button(kCtrlTriggerNonRunning,L"Monitor",L"\u672a\u8d77\u52d5\u6642",false,true);
    label(2050,L"Run once when TCycle starts.",L"TCycle\u306e\u8d77\u52d5\u6642\u306b\u4e00\u5ea6\u5b9f\u884c\u3057\u307e\u3059\u3002");
    label(2030,L"Run every",L"\u5b9f\u884c\u9593\u9694");label(2031,L"seconds",L"\u79d2");
    st->intervalSec=edit(kCtrlIntervalSec,true);spin(kCtrlSpinIntervalSec,st->intervalSec,0,86400);
    label(2020,L"Day",L"\u5b9f\u884c\u65e5");HWND day=combo(2021);
    for(auto text:{tcyc_get_text(st,L"ui_daily",L"Every day",L"\u6bce\u65e5"),tcyc_get_text(st,L"ui_weekdays",L"Weekdays",L"\u66dc\u65e5\u6307\u5b9a"),tcyc_get_text(st,L"ui_date",L"Specific date",L"\u65e5\u4ed8\u6307\u5b9a")})SendMessageW(day,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    label(2022,L"Time",L"\u6642\u523b");label(2023,L"Date",L"\u65e5\u4ed8");
    st->timeOfDay=edit(kCtrlTimeOfDay);st->dateValue=edit(kCtrlDateValue);
    SendMessageW(st->timeOfDay,EM_LIMITTEXT,5,0);SendMessageW(st->dateValue,EM_LIMITTEXT,10,0);
    st->dateEnabled=check(kCtrlDateEnabled,L"",L"");st->weekdayEnabled=check(kCtrlWeekdayEnabled,L"",L"");
    st->timeEnabled=check(kCtrlTimeEnabled,L"",L"");st->weekdayEveryday=check(kCtrlWeekdayEveryday,L"",L"");
    const wchar_t* en[]={L"Su",L"Mo",L"Tu",L"We",L"Th",L"Fr",L"Sa"};
    const wchar_t* ja[]={L"\u65e5",L"\u6708",L"\u706b",L"\u6c34",L"\u6728",L"\u91d1",L"\u571f"};
    for(int i=0;i<7;++i)st->weekdayChecks[i]=button(kCtrlWeekdaySun+i,en[i],ja[i],false,true);
    label(2080,L"Shortcut key",L"\u30b7\u30e7\u30fc\u30c8\u30ab\u30c3\u30c8\u30ad\u30fc");
    st->hotkeyMod=combo(kCtrlHotkeyMod);st->hotkeyKey=combo(kCtrlHotkeyKey);
    label(2070,L"Restart the target when it is not running.",L"\u5bfe\u8c61\u304c\u8d77\u52d5\u3057\u3066\u3044\u306a\u3044\u5834\u5408\u306b\u518d\u5b9f\u884c");
    label(2071,L"Retry delay (sec)",L"\u518d\u8a66\u884c\u9593\u9694\uff08\u79d2\uff09");label(2072,L"Maximum retries",L"\u6700\u5927\u518d\u8a66\u884c\u56de\u6570");
    label(2073,L"Existing unlimited retry settings are preserved.",L"\u65e2\u5b58\u306e\u7121\u5236\u9650\u8a2d\u5b9a\u3082\u4fdd\u6301\u3057\u307e\u3059\u3002");
    st->watchdogRetrySec=edit(kCtrlWatchdogRetrySec,true);spin(kCtrlSpinWatchdogRetrySec,st->watchdogRetrySec,10,3600);
    st->repeatCount=edit(kCtrlRepeatCount,true);spin(kCtrlSpinRepeatCount,st->repeatCount,1,1000000);
    SendMessageW(st->repeatCount,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(st->languageCode=="ja"?L"\u7121\u5236\u9650":L"Unlimited"));
    label(2090,L"Additional INI schedule settings are preserved.",L"INI\u306e\u8ffd\u52a0\u30b9\u30b1\u30b8\u30e5\u30fc\u30eb\u8a2d\u5b9a\u3092\u4fdd\u6301\u3057\u3066\u3044\u307e\u3059\u3002");
    label(2040,L"What to run",L"\u5b9f\u884c\u5185\u5bb9");st->actionMode=combo(kCtrlActionMode);
    st->actionPrimaryLabel=label(2044,L"Path / command",L"\u30d1\u30b9\uff0f\u30b3\u30de\u30f3\u30c9");
    st->actionPath=edit(kCtrlActionPath);st->actionArgs=edit(kCtrlActionArgs);st->actionCwd=edit(kCtrlActionCwd);
    label(2041,L"Arguments",L"\u5f15\u6570");label(2042,L"Working directory",L"\u4f5c\u696d\u30d5\u30a9\u30eb\u30c0\u30fc");
    st->singleInstance=check(kCtrlSingleInstance,L"Skip if the target is already running",L"\u65e2\u306b\u8d77\u52d5\u3057\u3066\u3044\u308b\u5834\u5408\u306f\u5b9f\u884c\u3057\u306a\u3044");
    label(2043,L"Changes are saved automatically. Run now tests the selected task.",L"\u5909\u66f4\u306f\u81ea\u52d5\u4fdd\u5b58\u3055\u308c\u307e\u3059\u3002\u30c6\u30b9\u30c8\u5b9f\u884c\u3067\u52d5\u4f5c\u3092\u78ba\u8a8d\u3067\u304d\u307e\u3059\u3002");
    label(2060,L"Settings shared by all tasks.",L"\u3059\u3079\u3066\u306e\u30bf\u30b9\u30af\u3067\u5171\u901a\u306e\u8a2d\u5b9a\u3067\u3059\u3002");
    label(2061,L"Polling interval",L"\u30c1\u30a7\u30c3\u30af\u9593\u9694");label(2062,L"How often TCycle checks task conditions.",L"\u30bf\u30b9\u30af\u306e\u5b9f\u884c\u6761\u4ef6\u3092\u78ba\u8a8d\u3059\u308b\u9593\u9694\u3002");
    label(2063,L"Grace period",L"\u5b9f\u884c\u7336\u4e88\u6642\u9593");label(2064,L"Allow scheduled tasks to run within this delay.",L"\u6307\u5b9a\u6642\u523b\u304b\u3089\u306e\u9045\u308c\u3092\u8a31\u5bb9\u3059\u308b\u6642\u9593\u3002");
    label(2065,L"sec",L"\u79d2");label(2066,L"sec",L"\u79d2");
    st->pollSec=edit(kCtrlPollSec,true);st->graceSec=edit(kCtrlGraceSec,true);
    spin(kCtrlSpinPollSec,st->pollSec,1,60);spin(kCtrlSpinGraceSec,st->graceSec,1,300);
    tcyc_refresh_fonts(st);
}
static void tcyc_size_window(WindowState* st)
{
    MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromWindow(st->mainWindow,MONITOR_DEFAULTTONEAREST),&info);
    RECT r{0,0,tcyc_scale(st,800),tcyc_scale(st,524)};
    AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(st->mainWindow,GWL_STYLE)),FALSE,0,st->dpi);
    int width=std::min(r.right-r.left,info.rcWork.right-info.rcWork.left),height=std::min(r.bottom-r.top,info.rcWork.bottom-info.rcWork.top);
    SetWindowPos(st->mainWindow,nullptr,info.rcWork.left+(info.rcWork.right-info.rcWork.left-width)/2,info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}
