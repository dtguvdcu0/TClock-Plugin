#pragma once
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// Native presentation for the approved profile/target/settings layout.
bool persistActiveProfile(SettingsDialog*, bool);
bool persistActiveProfileNoUI(SettingsDialog*, bool);

static std::wstring tcap_get_text(SettingsDialog* dlg,const wchar_t* key,const wchar_t* en,const wchar_t* ja)
{
    return translateId(*dlg->app,key,dlg->app->language=="ja"?ja:en);
}
static int tcap_scale(SettingsDialog* dlg,int value){return MulDiv(value,static_cast<int>(dlg->uiDpi),96);}
static HWND tcap_get_control(SettingsDialog* dlg,int id)
{
    HWND control=GetDlgItem(dlg->panel,id);
    return control?control:GetDlgItem(dlg->hwnd,id);
}
static void tcap_set_label(SettingsDialog* dlg,int id,const std::wstring& text)
{
    SetWindowTextW(tcap_get_control(dlg,id),text.c_str());
}
static void tcap_fill(HDC dc,RECT rect,COLORREF color)
{
    HBRUSH brush=CreateSolidBrush(color);FillRect(dc,&rect,brush);DeleteObject(brush);
}
static LRESULT CALLBACK tcap_panel_proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp)
{
    auto* dlg=reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE){
        dlg=static_cast<SettingsDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(dlg));
    }
    if(!dlg)return DefWindowProcW(hwnd,message,wp,lp);
    switch(message){
    case WM_COMMAND:
        if (HIWORD(wp)==EN_SETFOCUS || HIWORD(wp)==CBN_SETFOCUS || HIWORD(wp)==BN_SETFOCUS) {
            RECT control{},client{};
            GetWindowRect(reinterpret_cast<HWND>(lp),&control);
            MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&control),2);
            GetClientRect(hwnd,&client);
            int delta=control.top<0?control.top:control.bottom>client.bottom?control.bottom-client.bottom:0;
            if(delta){dlg->scrollOffset+=delta;tcap_layout_controls(dlg);}
        }
        return SendMessageW(dlg->hwnd,message,wp,lp);
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return SendMessageW(dlg->hwnd,message,wp,lp);
    case WM_VSCROLL:{
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&info);
        int next=info.nPos;
        switch(LOWORD(wp)){
        case SB_LINEUP:next-=tcap_scale(dlg,24);break;
        case SB_LINEDOWN:next+=tcap_scale(dlg,24);break;
        case SB_PAGEUP:next-=info.nPage;break;
        case SB_PAGEDOWN:next+=info.nPage;break;
        case SB_THUMBTRACK:next=info.nTrackPos;break;
        default:return 0;
        }
        dlg->scrollOffset=std::clamp(next,0,std::max(0,info.nMax-static_cast<int>(info.nPage)+1));
        tcap_layout_controls(dlg,true);return 0;
    }
    case WM_MOUSEWHEEL:
        dlg->scrollOffset=std::max(0,dlg->scrollOffset-MulDiv(GET_WHEEL_DELTA_WPARAM(wp),tcap_scale(dlg,60),WHEEL_DELTA));
        tcap_layout_controls(dlg,true);return 0;
    case WM_PAINT:{
        PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);tcap_paint_surface(dlg,hwnd,ps.hdc);EndPaint(hwnd,&ps);return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
void tcap_refresh_fonts(SettingsDialog* dlg)
{
    dlg->uiDpi=GetDpiForWindow(dlg->hwnd);
    if(!dlg->uiDpi)dlg->uiDpi=96;
    HFONT previous[]={dlg->uiFont,dlg->smallFont,dlg->titleFont};
    const wchar_t* family=dlg->app->language=="ja"?L"Yu Gothic UI":L"Segoe UI";
    auto create=[&](int size,int weight){return CreateFontW(-tcap_scale(dlg,size),0,0,0,weight,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);};
    dlg->uiFont=create(14,FW_NORMAL);dlg->smallFont=create(12,FW_NORMAL);dlg->titleFont=create(22,FW_SEMIBOLD);
    EnumChildWindows(dlg->hwnd,[](HWND child,LPARAM parameter)->BOOL{
        auto* d=reinterpret_cast<SettingsDialog*>(parameter);
        const int id=GetDlgCtrlID(child);
        HFONT font=id==160?d->titleFont:(id>=170&&id<=179)?d->smallFont:d->uiFont;
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return TRUE;
    },reinterpret_cast<LPARAM>(dlg));
    if(dlg->tab)SendMessageW(dlg->tab,LB_SETITEMHEIGHT,0,tcap_scale(dlg,50));
    for(HFONT font:previous)if(font)DeleteObject(font);
}
static void tcap_place(SettingsDialog* dlg,int id,int x,int y,int width,int height,bool visible=true)
{
    if(HWND control=tcap_get_control(dlg,id))dlg->placements.push_back({control,x,y,width,height,visible});
}
void tcap_fill_apps(SettingsDialog* dlg, const std::wstring& selected, const std::wstring& executable)
{
    const bool suppressed = dlg->suppressSave;
    dlg->suppressSave = true;
    dlg->appsFormat = SendMessageW(dlg->formatCombo,CB_GETCURSEL,0,0)==1?"jpg":"png";
    dlg->openApps = {{L"", tcap_get_text(dlg,L"ui_default_app",L"Windows default app",L"Windows\u306e\u65e2\u5b9a\u30a2\u30d7\u30ea")}};
    auto choices = tcap_list_apps(dlg->appsFormat=="jpg"?L".jpg":L".png");
    dlg->openApps.insert(dlg->openApps.end(),choices.begin(),choices.end());
    int index = selected.empty()?0:-1;
    for(size_t i=1;i<dlg->openApps.size();++i)
        if(_wcsicmp(dlg->openApps[i].name.c_str(),selected.c_str())==0)index=static_cast<int>(i);
    if(index<0){
        index=static_cast<int>(dlg->openApps.size());
        dlg->openApps.push_back({selected,tcap_get_text(dlg,L"ui_unavailable_app",L"Previously selected app (unavailable)",L"\u524d\u306b\u9078\u3093\u3060\u30a2\u30d7\u30ea (\u5229\u7528\u3067\u304d\u307e\u305b\u3093)")});
    }
    if(!executable.empty()){
        index=static_cast<int>(dlg->openApps.size());
        dlg->openApps.push_back({executable,std::filesystem::path(executable).filename().wstring(),true});
    }
    dlg->openApps.push_back({L"",tcap_get_text(dlg,L"ui_choose_app",L"Choose executable...",L"\u5b9f\u884c\u30d5\u30a1\u30a4\u30eb\u3092\u6307\u5b9a..."),false,true});
    SendMessageW(dlg->openAppCombo,CB_RESETCONTENT,0,0);
    for(const auto& app:dlg->openApps)SendMessageW(dlg->openAppCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(app.label.c_str()));
    SendMessageW(dlg->openAppCombo,CB_SETCURSEL,index,0);
    dlg->suppressSave=suppressed;
}

void tcap_layout_controls(SettingsDialog* dlg,bool scrolling)
{
    if(!dlg||!dlg->panel||dlg->arranging)return;
    dlg->arranging=true;
    RECT area{};GetClientRect(dlg->hwnd,&area);
    const int width=MulDiv(area.right,96,dlg->uiDpi),height=MulDiv(area.bottom,96,dlg->uiDpi);
    const int side=160,footer=54,pad=16,bodyHeight=std::max(80,height-footer);
    SetWindowPos(dlg->panel,nullptr,tcap_scale(dlg,side),0,tcap_scale(dlg,width-side),tcap_scale(dlg,bodyHeight),SWP_NOZORDER|SWP_NOACTIVATE);
    const int custom=dlg->targetMode==3?40:0;
    const auto action=SendMessageW(dlg->actionCombo,CB_GETCURSEL,0,0);
    const int appRow=action==2?56:0;
    dlg->contentHeight=tcap_scale(dlg,450+custom+appRow+(dlg->detailsOpen?144:0));
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
    scroll.nMax=dlg->contentHeight-1;scroll.nPage=tcap_scale(dlg,bodyHeight);
    dlg->scrollOffset=std::clamp(dlg->scrollOffset,0,std::max(0,dlg->contentHeight-static_cast<int>(scroll.nPage)));
    scroll.nPos=dlg->scrollOffset;SetScrollInfo(dlg->panel,SB_VERT,&scroll,TRUE);
    RECT panel{};GetClientRect(dlg->panel,&panel);
    const int right=MulDiv(panel.right,96,dlg->uiDpi)-pad,field=right-pad;
    dlg->placements.clear();dlg->separators.clear();
    tcap_place(dlg,170,12,12,108,24);
    tcap_place(dlg,130,124,10,28,28);
    tcap_place(dlg,120,8,46,144,std::max(70,bodyHeight-92));
    tcap_place(dlg,131,8,bodyHeight-38,68,28);
    tcap_place(dlg,132,84,bodyHeight-38,68,28);
    tcap_place(dlg,171,14,height-40,std::max(100,width-350),26);
    tcap_place(dlg,153,width-300,height-43,132,32);
    tcap_place(dlg,142,width-160,height-43,146,32);
    tcap_place(dlg,160,pad,14,field,32);
    tcap_place(dlg,161,pad,56,field,22);
    const int gap=5,cardWidth=(field-gap*3)/4;
    for(int i=0;i<4;++i)tcap_place(dlg,150+i+(i==3?1:0),pad+i*(cardWidth+gap),84,cardWidth,64);
    tcap_place(dlg,104,pad,156,field-88,28,custom!=0);
    tcap_place(dlg,141,right-80,156,80,28,custom!=0);
    dlg->separators.push_back(156+custom);
    const int save=168+custom;
    tcap_place(dlg,162,pad,save,field,22);
    const bool opening=action>0;
    const bool saving=action!=1;
    tcap_place(dlg,180,pad,save+28,std::min(280,field),30);
    tcap_place(dlg,172,pad,save+64,field,20);
    tcap_place(dlg,101,pad,save+88,field-84,30,saving);
    tcap_place(dlg,140,right-76,save+88,76,30,saving);
    tcap_place(dlg,182,pad,save+126,field,20,appRow!=0);
    tcap_place(dlg,181,pad,save+88+appRow,field,30,opening);
    const int image=save+appRow;
    tcap_place(dlg,173,pad,image+126,126,20);
    tcap_place(dlg,102,pad,image+148,126,30);
    const bool png=SendMessageW(dlg->formatCombo,CB_GETCURSEL,0,0)!=1;
    tcap_place(dlg,174,pad+144,image+126,110,20,png);
    tcap_place(dlg,175,pad+144,image+126,110,20,!png);
    tcap_place(dlg,103,pad+144,image+148,74,30,png);
    tcap_place(dlg,113,pad+144,image+148,74,30,!png);
    tcap_place(dlg,176,pad+236,image+126,std::max(50,field-236),54);
    dlg->separators.push_back(image+186);
    const int hotkey=image+198;
    tcap_place(dlg,163,pad,hotkey,116,26);
    tcap_place(dlg,105,right-270,hotkey,130,28);
    tcap_place(dlg,106,right-130,hotkey,130,28);
    dlg->separators.push_back(hotkey+36);
    const int detail=hotkey+44;
    tcap_place(dlg,155,pad,detail,field,30);
    tcap_place(dlg,156,pad,detail+38,field,25,dlg->detailsOpen);
    tcap_place(dlg,177,pad,detail+68,72,24,dlg->detailsOpen);
    tcap_place(dlg,107,pad+78,detail+66,106,28,dlg->detailsOpen);
    tcap_place(dlg,178,pad+202,detail+68,90,24,dlg->detailsOpen);
    tcap_place(dlg,108,pad+296,detail+66,74,28,dlg->detailsOpen);
    tcap_place(dlg,109,pad,detail+104,field,25,dlg->detailsOpen);
    tcap_place(dlg,179,pad,detail+136,72,24,dlg->detailsOpen);
    tcap_place(dlg,110,pad+78,detail+134,106,28,dlg->detailsOpen);
    tcap_place(dlg,111,pad+202,detail+136,std::max(70,field-202),24,dlg->detailsOpen);
    for(const auto& p:dlg->placements){
        const bool inPanel=GetParent(p.window)==dlg->panel;
        const int y=tcap_scale(dlg,p.y)-(inPanel?dlg->scrollOffset:0);
        wchar_t name[32]{};GetClassNameW(p.window,name,32);
        const int h=tcap_scale(dlg,wcscmp(name,L"ComboBox")==0?220:p.height);
        SetWindowPos(p.window,nullptr,tcap_scale(dlg,p.x),y,tcap_scale(dlg,p.width),h,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|(inPanel?SWP_NOREDRAW:0)|(inPanel&&scrolling?SWP_NOSIZE:0)|(p.visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    }
    dlg->arranging=false;
    InvalidateRect(dlg->hwnd,nullptr,FALSE);
    // Repaint every child after placement; clipped glyph pixels must not survive scrolling.
    // Buffer the panel and present one complete frame after all child moves.
    RedrawWindow(dlg->panel,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME|RDW_UPDATENOW);
}
void tcap_paint_surface(SettingsDialog* dlg,HWND window,HDC dc)
{
    RECT area{};GetClientRect(window,&area);tcap_fill(dc,area,RGB(255,255,255));
    if(window==dlg->hwnd){
        RECT side{0,0,tcap_scale(dlg,160),area.bottom-tcap_scale(dlg,54)};
        tcap_fill(dc,side,RGB(246,248,251));
        RECT footer{0,side.bottom,area.right,area.bottom};tcap_fill(dc,footer,RGB(250,251,253));
        RECT line{side.right-1,0,side.right,side.bottom};tcap_fill(dc,line,RGB(226,232,239));
        line={0,side.bottom,area.right,side.bottom+1};tcap_fill(dc,line,RGB(226,232,239));
    }else{
        for(int y:dlg->separators){
            const int top=tcap_scale(dlg,y)-dlg->scrollOffset;
            RECT line{tcap_scale(dlg,16),top,area.right-tcap_scale(dlg,16),top+1};tcap_fill(dc,line,RGB(229,234,240));
        }
    }
}
static void tcap_draw_text(SettingsDialog* dlg,HDC dc,std::wstring text,RECT rect,COLORREF color,HFONT font,UINT flags)
{
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
    const auto old=SelectObject(dc,font?font:dlg->uiFont);
    DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&rect,flags|DT_NOPREFIX);
    SelectObject(dc,old);
}
static void tcap_draw_icon(SettingsDialog* dlg,HDC dc,int target,RECT rect,bool selected)
{
    struct GraphicsRuntime {
        ULONG_PTR token=0;
        GraphicsRuntime(){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&token,&input,nullptr);}
        ~GraphicsRuntime(){if(token)Gdiplus::GdiplusShutdown(token);}
    };
    static GraphicsRuntime runtime;
    if(!runtime.token)return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const float size=static_cast<float>(tcap_scale(dlg,27));
    graphics.TranslateTransform((rect.left+rect.right-size)/2.0f,static_cast<float>(rect.top+tcap_scale(dlg,7)));
    graphics.ScaleTransform(size/32.0f,size/32.0f);
    const Gdiplus::Color ink=selected?Gdiplus::Color(255,35,102,184):Gdiplus::Color(255,114,139,167);
    Gdiplus::Pen pen(ink,1.5f);
    pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush tint(selected?Gdiplus::Color(255,224,237,252):Gdiplus::Color(255,240,244,249));
    Gdiplus::SolidBrush solid(ink);
    auto box=[&](float x,float y,float width,float height,float radius,bool fill){
        Gdiplus::GraphicsPath path;
        path.AddArc(x,y,radius*2,radius*2,180,90);
        path.AddArc(x+width-radius*2,y,radius*2,radius*2,270,90);
        path.AddArc(x+width-radius*2,y+height-radius*2,radius*2,radius*2,0,90);
        path.AddArc(x,y+height-radius*2,radius*2,radius*2,90,90);path.CloseFigure();
        if(fill)graphics.FillPath(&tint,&path);
        graphics.DrawPath(&pen,&path);
    };
    switch(target){
    case 0:
        box(3,6,19,14,2,true);box(10,11,19,14,2,true);
        graphics.DrawLine(&pen,19.5f,25.0f,19.5f,28.0f);
        graphics.DrawLine(&pen,15.0f,28.0f,24.0f,28.0f);
        break;
    case 1:
        box(3,5,26,19,2,true);
        graphics.DrawLine(&pen,16.0f,24.0f,16.0f,28.0f);
        graphics.DrawLine(&pen,10.0f,28.0f,22.0f,28.0f);
        graphics.FillEllipse(&solid,13.5f,12.0f,5.0f,5.0f);
        break;
    case 2:
        box(3,5,26,23,2,false);
        graphics.DrawLine(&pen,3.0f,12.0f,29.0f,12.0f);
        for(float x:{6.0f,9.0f,12.0f})graphics.FillEllipse(&solid,x,8.0f,1.5f,1.5f);
        graphics.DrawLine(&pen,8.0f,18.0f,21.0f,18.0f);
        graphics.DrawLine(&pen,8.0f,22.0f,17.0f,22.0f);
        break;
    case 3:
        box(4,4,10,10,2,true);box(18,4,10,10,2,false);
        box(4,18,10,10,2,false);box(18,18,10,10,2,false);
        graphics.DrawLine(&pen,6.5f,9.0f,8.3f,11.0f);
        graphics.DrawLine(&pen,8.3f,11.0f,11.5f,7.0f);
        break;
    }
}

bool tcap_draw_item(SettingsDialog* dlg,const DRAWITEMSTRUCT& item)
{
    RECT rect=item.rcItem;const HDC dc=item.hDC;
    if(item.CtlID==120){
        tcap_fill(dc,rect,RGB(246,248,251));
        if(item.itemID==static_cast<UINT>(-1)||item.itemID>=dlg->app->profiles.size())return true;
        const bool selected=(item.itemState&ODS_SELECTED)!=0;
        InflateRect(&rect,-tcap_scale(dlg,3),-tcap_scale(dlg,3));
        tcap_fill(dc,rect,selected?RGB(230,239,251):RGB(246,248,251));
        if(selected){RECT mark=rect;mark.right=mark.left+tcap_scale(dlg,3);mark.top+=tcap_scale(dlg,10);mark.bottom-=tcap_scale(dlg,10);tcap_fill(dc,mark,RGB(35,102,184));}
        const auto& profile=dlg->app->profiles[item.itemID];
        RECT label=rect;label.left+=tcap_scale(dlg,12);label.right-=tcap_scale(dlg,6);label.top+=tcap_scale(dlg,4);label.bottom=label.top+tcap_scale(dlg,22);
        tcap_draw_text(dlg,dc,utf8ToWide(profile.name),label,selected?RGB(35,93,159):RGB(37,49,63),dlg->uiFont,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        label.top+=tcap_scale(dlg,21);label.bottom=rect.bottom;
        const auto target=displaysToString(profile.settings);
        std::wstring detail=target=="all"||target=="0"?tcap_get_text(dlg,L"ui_all",L"All displays",L"\u3059\u3079\u3066\u306e\u753b\u9762"):
            target=="active_window"?tcap_get_text(dlg,L"ui_active_window",L"Active window",L"\u64cd\u4f5c\u4e2d\u306e\u30a6\u30a3\u30f3\u30c9\u30a6"):
            target=="active_display"?tcap_get_text(dlg,L"ui_active_display",L"Active display",L"\u64cd\u4f5c\u4e2d\u306e\u753b\u9762"):utf8ToWide(target);
        if(profile.settings.autoCapture)detail=std::to_wstring(profile.settings.autoSeconds)+L"s / "+tcap_get_text(dlg,L"ui_auto",L"Auto capture",L"\u81ea\u52d5\u64ae\u5f71");
        tcap_draw_text(dlg,dc,detail,label,RGB(112,127,146),dlg->smallFont,DT_SINGLELINE|DT_END_ELLIPSIS);
        if(item.itemState&ODS_FOCUS)DrawFocusRect(dc,&rect);return true;
    }
    if(item.CtlType!=ODT_BUTTON)return false;
    const int id=static_cast<int>(item.CtlID);
    const int target=id>=150&&id<=152?id-150:id==154?3:-1;
    const bool selected=target>=0&&target==dlg->targetMode,primary=id==142,disabled=(item.itemState&ODS_DISABLED)!=0;
    COLORREF paper=primary?RGB(35,102,184):selected?RGB(240,246,254):RGB(255,255,255);
    if(item.itemState&ODS_SELECTED)paper=primary?RGB(23,84,153):RGB(225,237,250);
    HPEN pen=CreatePen(PS_SOLID,1,primary?paper:selected?RGB(110,161,220):RGB(213,222,232));
    HBRUSH brush=CreateSolidBrush(paper);
    auto oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,brush);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,tcap_scale(dlg,6),tcap_scale(dlg,6));
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(pen);DeleteObject(brush);
    wchar_t caption[256]{};GetWindowTextW(item.hwndItem,caption,256);
    RECT textRect=rect;InflateRect(&textRect,-tcap_scale(dlg,5),0);
    if(target>=0){
        tcap_draw_icon(dlg,dc,target,rect,selected);
        textRect.top+=tcap_scale(dlg,32);
    }
    UINT textFlags=DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS;
    if(target>=0){
        RECT measured=textRect;
        auto previous=SelectObject(dc,dlg->smallFont);
        DrawTextW(dc,caption,-1,&measured,DT_CENTER|DT_WORDBREAK|DT_CALCRECT|DT_NOPREFIX);
        SelectObject(dc,previous);
        const int textHeight=measured.bottom-measured.top;
        textRect.top+=std::max<LONG>(0,(textRect.bottom-textRect.top-textHeight)/2);
        textFlags=DT_CENTER|DT_WORDBREAK;
    }
    tcap_draw_text(dlg,dc,caption,textRect,disabled?RGB(162,171,183):primary?RGB(255,255,255):selected?RGB(35,93,159):RGB(57,75,96),
        target>=0?dlg->smallFont:dlg->uiFont,textFlags);
    if(item.itemState&ODS_FOCUS){InflateRect(&rect,-3,-3);DrawFocusRect(dc,&rect);}
    return true;
}

void tcap_sync_controls(SettingsDialog* dlg)
{
    if(!dlg||!dlg->panel)return;
    const auto raw=toLower(trimCopy(getControlText(dlg->displaysEdit)));
    dlg->targetMode=raw=="active_window"?2:raw=="active_display"?1:(raw.empty()||raw=="all"||raw=="0")?0:3;
    if(dlg->activeProfile>=0&&dlg->activeProfile<static_cast<int>(dlg->app->profiles.size()))
        tcap_set_label(dlg,160,utf8ToWide(dlg->app->profiles[dlg->activeProfile].name));
    const auto action=SendMessageW(dlg->actionCombo,CB_GETCURSEL,0,0);
    const bool opening=action>0;
    tcap_set_label(dlg,172,action==1?tcap_get_text(dlg,L"ui_open_app",L"Open with",L"\u958b\u304f\u30a2\u30d7\u30ea"):
        tcap_get_text(dlg,L"ui_folder",L"Output folder",L"\u4fdd\u5b58\u5148\u30d5\u30a9\u30eb\u30c0\u30fc"));
    const auto format=SendMessageW(dlg->formatCombo,CB_GETCURSEL,0,0)==1?"jpg":"png";
    if(dlg->appsFormat!=format&&dlg->activeProfile>=0&&dlg->activeProfile<static_cast<int>(dlg->app->profiles.size()))
        tcap_fill_apps(dlg,dlg->app->profiles[dlg->activeProfile].settings.openApp,dlg->app->profiles[dlg->activeProfile].settings.openExecutable);
    if(opening){
        const bool suppressed=dlg->suppressSave;dlg->suppressSave=true;
        setBurstFpsSelection(dlg->burstFpsCombo,0);
        SendMessageW(dlg->autoCaptureCheck,BM_SETCHECK,BST_UNCHECKED,0);
        dlg->suppressSave=suppressed;
    }
    EnableWindow(tcap_get_control(dlg,156),!opening);EnableWindow(dlg->autoCaptureCheck,!opening);
    EnableWindow(tcap_get_control(dlg,153),action!=1);
    const bool burst=burstFpsFromCombo(dlg->burstFpsCombo)>0;
    SendMessageW(tcap_get_control(dlg,156),BM_SETCHECK,burst?BST_CHECKED:BST_UNCHECKED,0);
    EnableWindow(dlg->burstFpsCombo,burst);EnableWindow(dlg->burstSecondsEdit,burst);
    const bool automatic=SendMessageW(dlg->autoCaptureCheck,BM_GETCHECK,0,0)==BST_CHECKED;
    EnableWindow(dlg->autoIntervalEdit,automatic);
    const bool png=SendMessageW(dlg->formatCombo,CB_GETCURSEL,0,0)!=1;
    tcap_set_label(dlg,176,png?tcap_get_text(dlg,L"ui_png_help",L"0-9. Higher compression gives smaller files without changing quality.",L"0\u20139\u3002\u9ad8\u3044\u307b\u3069\u5c0f\u3055\u304f\u4fdd\u5b58\u3002\n\u753b\u8cea\u306f\u5909\u308f\u308a\u307e\u305b\u3093\u3002"):
        tcap_get_text(dlg,L"ui_jpg_help",L"1-100. Higher quality gives larger files.",L"1\u2013100\u3002\u9ad8\u3044\u307b\u3069\u9ad8\u753b\u8cea\u3002\n\u30b5\u30a4\u30ba\u3082\u5927\u304d\u304f\u306a\u308a\u307e\u3059\u3002"));
    auto summary=tcap_get_text(dlg,L"ui_automation",L"Burst / automatic capture",L"\u9023\u5199\u30fb\u81ea\u52d5\u64ae\u5f71")+L"   ";
    if(burst)summary+=std::to_wstring(burstFpsFromCombo(dlg->burstFpsCombo))+L" fps";
    if(automatic)summary+=(burst?L" / ":L"")+utf8ToWide(getControlText(dlg->autoIntervalEdit))+L"s";
    if(!burst&&!automatic)summary+=tcap_get_text(dlg,L"ui_off",L"Off",L"\u30aa\u30d5");
    summary+=dlg->detailsOpen?L"   \u2303":L"   \u2304";tcap_set_label(dlg,155,summary);
    tcap_set_label(dlg,142,burst?tcap_get_text(dlg,L"ui_capture_burst",L"Capture burst",L"\u9023\u5199\u3059\u308b"):tcap_get_text(dlg,L"ui_capture_now",L"Capture now",L"\u4eca\u3059\u3050\u64ae\u5f71"));
    tcap_layout_controls(dlg);InvalidateRect(dlg->tab,nullptr,FALSE);
    for(int id:{150,151,152,154})InvalidateRect(tcap_get_control(dlg,id),nullptr,FALSE);
}
static void tcap_select_monitors(SettingsDialog* dlg)
{
    HMENU menu=CreatePopupMenu();
    struct MonitorMenu { HMENU menu; int index=0; std::vector<int> selected; } state{menu};
    bool all=false,active=false,window=false;
    state.selected=parseDisplayList(getControlText(dlg->displaysEdit),all,active,window);
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM parameter)->BOOL{
        auto* value=reinterpret_cast<MonitorMenu*>(parameter);MONITORINFO info{sizeof(info)};
        if(!GetMonitorInfoW(monitor,&info))return TRUE;
        const int index=++value->index;
        const auto label=std::to_wstring(index)+L"   "+std::to_wstring(info.rcMonitor.right-info.rcMonitor.left)+L" \u00d7 "+std::to_wstring(info.rcMonitor.bottom-info.rcMonitor.top);
        AppendMenuW(value->menu,MF_STRING|(std::find(value->selected.begin(),value->selected.end(),index)!=value->selected.end()?MF_CHECKED:0),index,label.c_str());
        return TRUE;
    },reinterpret_cast<LPARAM>(&state));
    RECT rect{};GetWindowRect(dlg->displaysHelpBtn,&rect);
    const int selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,rect.left,rect.bottom,0,dlg->hwnd,nullptr);DestroyMenu(menu);
    if(selected<=0)return;
    auto found=std::find(state.selected.begin(),state.selected.end(),selected);
    if(found==state.selected.end())state.selected.push_back(selected);
    else if(state.selected.size()>1)state.selected.erase(found);
    std::sort(state.selected.begin(),state.selected.end());
    std::wstring text;for(int index:state.selected){if(!text.empty())text+=L",";text+=std::to_wstring(index);}
    SetWindowTextW(dlg->displaysEdit,text.c_str());
}
bool tcap_handle_command(SettingsDialog* dlg,WPARAM wp,LPARAM)
{
    const int id=LOWORD(wp);
    if((id==180||id==181)&&HIWORD(wp)==CBN_SELCHANGE){
        if(dlg->suppressSave)return true;
        if(id==181){
            const auto index=SendMessageW(dlg->openAppCombo,CB_GETCURSEL,0,0);
            if(index>=0&&static_cast<size_t>(index)<dlg->openApps.size()&&dlg->openApps[static_cast<size_t>(index)].browse){
                std::wstring executable;
                const auto title=tcap_get_text(dlg,L"ui_choose_app",L"Choose executable...",L"\u5b9f\u884c\u30d5\u30a1\u30a4\u30eb\u3092\u6307\u5b9a...");
                const HRESULT result=tcap_choose_app(dlg->hwnd,title,executable);
                const auto& settings=dlg->app->profiles[dlg->activeProfile].settings;
                tcap_fill_apps(dlg,settings.openApp,SUCCEEDED(result)?executable:settings.openExecutable);
                if(FAILED(result)){
                    if(result!=HRESULT_FROM_WIN32(ERROR_CANCELLED))MessageBoxW(dlg->hwnd,
                        tcap_get_text(dlg,L"ui_choose_app_failed",L"Could not select an executable.",L"\u5b9f\u884c\u30d5\u30a1\u30a4\u30eb\u3092\u9078\u629e\u3067\u304d\u307e\u305b\u3093\u3067\u3057\u305f\u3002").c_str(),L"TCapture",MB_OK|MB_ICONERROR);
                    return true;
                }
            }
        }
        persistActiveProfile(dlg,false);
        return true;
    }
    if(HIWORD(wp)!=BN_CLICKED)return false;
    if(id==153){if(persistActiveProfileNoUI(dlg,true))openOutputFolder(*dlg->app,dlg->activeProfile);return true;}
    if(id==141){tcap_select_monitors(dlg);return true;}
    if(id==155){
        dlg->detailsOpen=!dlg->detailsOpen;
        tcap_sync_controls(dlg);return true;
    }
    if(id==156){
        const bool enabled=SendMessageW(tcap_get_control(dlg,156),BM_GETCHECK,0,0)==BST_CHECKED;
        setBurstFpsSelection(dlg->burstFpsCombo,enabled?5:0);
        persistActiveProfile(dlg,false);tcap_sync_controls(dlg);return true;
    }
    const int target=id>=150&&id<=152?id-150:id==154?3:-1;
    if(target>=0){
        const wchar_t* values[]={L"all",L"active_display",L"active_window",L"1"};
        SetWindowTextW(dlg->displaysEdit,values[target]);tcap_sync_controls(dlg);return true;
    }
    return false;
}
void buildSettingsLayout(SettingsDialog* dlg)
{
    WNDCLASSW panelClass{};panelClass.lpfnWndProc=tcap_panel_proc;panelClass.hInstance=dlg->app->hInstance;
    panelClass.lpszClassName=L"TCaptureSettingsPanel";panelClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&panelClass);
    dlg->panel=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_COMPOSITED,panelClass.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_VSCROLL,
        0,0,1,1,dlg->hwnd,reinterpret_cast<HMENU>(159),dlg->app->hInstance,dlg);
    auto create=[&](int id,const wchar_t* type,const std::wstring& text,DWORD style,bool outer=false)->HWND{
        HWND control=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,text.c_str(),WS_CHILD|WS_VISIBLE|style,
            0,0,1,1,outer?dlg->hwnd:dlg->panel,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),dlg->app->hInstance,nullptr);
        if(wcscmp(type,L"EDIT")==0)native_edit::attach(control);return control;
    };
    auto label=[&](int id,const wchar_t* key,const wchar_t* en,const wchar_t* ja,bool outer=false){
        return create(id,L"STATIC",tcap_get_text(dlg,key,en,ja),SS_CENTERIMAGE,outer);
    };
    auto button=[&](int id,const wchar_t* key,const wchar_t* en,const wchar_t* ja,bool outer=false){
        return create(id,L"BUTTON",tcap_get_text(dlg,key,en,ja),BS_OWNERDRAW|BS_NOTIFY|WS_TABSTOP,outer);
    };
    auto edit=[&](int id,bool number=false){return create(id,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP|(number?ES_NUMBER:0));};
    auto combo=[&](int id){return create(id,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP);};
    label(170,L"ui_profiles",L"Profiles",L"\u30d7\u30ed\u30d5\u30a1\u30a4\u30eb",true);
    dlg->addBtn=button(130,L"ui_add",L"+",L"+",true);
    dlg->tab=create(120,L"LISTBOX",L"",LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP,true);
    dlg->renameBtn=button(131,L"button_rename",L"Rename",L"\u540d\u524d\u5909\u66f4",true);
    dlg->deleteBtn=button(132,L"button_delete",L"Delete",L"\u524a\u9664",true);
    label(171,L"ui_saved",L"Settings saved automatically",L"\u8a2d\u5b9a\u306f\u81ea\u52d5\u4fdd\u5b58",true);
    button(153,L"ui_open_folder",L"Open folder",L"\u4fdd\u5b58\u5148\u3092\u958b\u304f",true);
    dlg->captureBtn=button(142,L"ui_capture_now",L"Capture now",L"\u4eca\u3059\u3050\u64ae\u5f71",true);
    label(160,L"ui_profile_title",L"Profile",L"\u30d7\u30ed\u30d5\u30a1\u30a4\u30eb");
    label(161,L"ui_target",L"Capture target",L"\u64ae\u5f71\u5bfe\u8c61");
    button(150,L"ui_all",L"All displays",L"\u3059\u3079\u3066\u306e\u753b\u9762");
    button(151,L"ui_active_display",L"Active display",L"\u64cd\u4f5c\u4e2d\u306e\u753b\u9762");
    button(152,L"ui_active_window",L"Active window",L"\u64cd\u4f5c\u4e2d\u306e\u30a6\u30a3\u30f3\u30c9\u30a6");
    button(154,L"ui_select_display",L"Select displays",L"\u753b\u9762\u3092\u6307\u5b9a");
    dlg->displaysEdit=edit(104);dlg->displaysHelpBtn=button(141,L"ui_choose",L"Choose...",L"\u753b\u9762\u3092\u9078\u629e");
    label(162,L"ui_save",L"Save",L"\u4fdd\u5b58");label(172,L"ui_folder",L"Output folder",L"\u4fdd\u5b58\u5148\u30d5\u30a9\u30eb\u30c0\u30fc");
    tcap_set_label(dlg,162,tcap_get_text(dlg,L"ui_action",L"After capture",L"\u64ae\u5f71\u5f8c\u306e\u52d5\u4f5c"));
    dlg->actionCombo=combo(180);dlg->openAppCombo=combo(181);
    for(const auto& text:{tcap_get_text(dlg,L"ui_action_save",L"Save",L"\u4fdd\u5b58"),
                         tcap_get_text(dlg,L"ui_action_open",L"Open in another app",L"\u4ed6\u306e\u30c4\u30fc\u30eb\u3067\u958b\u304f"),
                         tcap_get_text(dlg,L"ui_action_save_open",L"Save and open in another app",L"\u4fdd\u5b58\u3057\u3066\u4ed6\u306e\u30c4\u30fc\u30eb\u3067\u958b\u304f")})
        SendMessageW(dlg->actionCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    label(182,L"ui_open_app",L"Open with",L"\u958b\u304f\u30a2\u30d7\u30ea");
    addTooltip(dlg,dlg->actionCombo,tcap_get_text(dlg,L"ui_open_help",L"Open only uses a temporary image. Save and open edits the image in your output folder. Burst and automatic capture are disabled.",
        L"\u958b\u304f\u5834\u5408\u306f\u4e00\u6642\u753b\u50cf\u3092\u4f7f\u3044\u307e\u3059\u3002\u9023\u5199\u30fb\u81ea\u52d5\u64ae\u5f71\u306f\u7121\u52b9\u3067\u3059\u3002"));
    dlg->outputEdit=edit(101);button(140,L"ui_browse",L"Browse...",L"\u5909\u66f4...");
    label(173,L"ui_format",L"Image format",L"\u753b\u50cf\u5f62\u5f0f");dlg->formatCombo=combo(102);
    for(const wchar_t* format:{L"png",L"jpg"})SendMessageW(dlg->formatCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(format));
    dlg->compressionLabelPng=label(174,L"ui_compression",L"Compression",L"\u5727\u7e2e\u30ec\u30d9\u30eb");
    dlg->compressionLabelJpg=label(175,L"ui_quality",L"Quality",L"\u753b\u8cea");
    dlg->compressionEditPng=edit(103,true);dlg->compressionEditJpg=edit(113,true);create(176,L"STATIC",L"",0);
    label(163,L"ui_shortcut",L"Shortcut",L"\u30b7\u30e7\u30fc\u30c8\u30ab\u30c3\u30c8");
    dlg->hotkeyModCombo=combo(105);dlg->hotkeyKeyCombo=combo(106);
    for(const auto& option:modOptions()){
        const auto text=option.label==L"None"?translateId(*dlg->app,L"option_none",L"None"):option.label;
        SendMessageW(dlg->hotkeyModCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    }
    for(const auto& option:keyOptions()){
        const auto text=option.label==L"(None)"?translateId(*dlg->app,L"option_none",L"None"):option.label;
        SendMessageW(dlg->hotkeyKeyCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    }
    button(155,L"ui_automation",L"Burst / automatic capture",L"\u9023\u5199\u30fb\u81ea\u52d5\u64ae\u5f71");
    create(156,L"BUTTON",tcap_get_text(dlg,L"ui_burst",L"Burst capture",L"\u9023\u5199"),BS_AUTOCHECKBOX|WS_TABSTOP);
    label(177,L"ui_rate",L"Rate",L"\u64ae\u5f71\u983b\u5ea6");
    label(178,L"ui_seconds",L"Seconds",L"\u64ae\u5f71\u6642\u9593 (\u79d2)");
    dlg->burstFpsCombo=combo(107);populateBurstFpsCombo(dlg->burstFpsCombo);dlg->burstSecondsEdit=edit(108,true);
    dlg->autoCaptureCheck=create(109,L"BUTTON",tcap_get_text(dlg,L"ui_auto",L"Auto capture",L"\u81ea\u52d5\u64ae\u5f71"),BS_AUTOCHECKBOX|WS_TABSTOP);
    label(179,L"ui_interval",L"Interval (s)",L"\u9593\u9694 (\u79d2)");
    dlg->autoIntervalEdit=edit(110,true);dlg->autoStatusLabel=create(111,L"STATIC",L"",SS_CENTERIMAGE);
    tcap_refresh_fonts(dlg);
    for(int id:{103,113,108,110})SendMessageW(tcap_get_control(dlg,id),EM_SETLIMITTEXT,8,0);
    addTooltip(dlg,dlg->displaysEdit,tcap_get_text(dlg,L"ui_display_tip",L"Comma-separated display numbers, e.g. 1,3",L"\u753b\u9762\u756a\u53f7\u3092\u30ab\u30f3\u30de\u3067\u533a\u5207\u308a\u307e\u3059\u3002\u4f8b: 1,3"));
    tcap_layout_controls(dlg);native_edit::prepare(dlg->hwnd);
}
