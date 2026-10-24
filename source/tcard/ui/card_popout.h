#pragma once

static void begin_edit(TCARD_WUI_HOST host);
static void end_edit(TCARD_WUI_HOST host,bool save);
constexpr int kToolFirst=5100,kToolCount=tcard_tools::count,kToolStatus=5150,kMenuButton=5151;
constexpr UINT_PTR kCardTimer=0x54434944;
static int card_px(TCARD_WUI_HOST host,int dip)
{
 const UINT dpi=GetDpiForWindow(host->window);
 return MulDiv(dip,dpi?dpi:96,96);
}
static void card_mark_dirty(TCARD_WUI_HOST host)
{
 if(!host->editing||host->loadingDraft||!host->draftReady)return;
 if(auto* state=tcard_rich_edit::state(host->sourceEditor);state&&state->loading)return;
 host->editDirty=true;host->historyPending=true;
 const auto now=GetTickCount64();host->saveDue=now+650;host->historyDue=now+3000;
 SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.saving",L"Saving..."));
}
static void card_refresh_style(TCARD_WUI_HOST host)
{
 if(!host->editing||host->loadingDraft)return;
 double size=0;if(!tcard_fonts::read_size(host->fontSizeEditor,size))return;
 const auto family=tcard_fonts::resolved_family(host->fontFamilyEditor);
 if(!host->markdown)tcard_rich_edit::configure(host->sourceEditor,true,size,family.empty()?L"Segoe UI":family,host->editColor,host->state.textColor,true);
 SendMessageW(host->sourceEditor,EM_SETBKGNDCOLOR,0,host->editColor);
 host->state.backColor=host->editColor;
 if(host->editorBrush)DeleteObject(host->editorBrush);
 host->editorBrush=CreateSolidBrush(host->editColor);
 update_read_brush(host);release_render(host);InvalidateRect(host->window,nullptr,FALSE);
}
static bool card_save_draft(TCARD_WUI_HOST host,bool checkpoint)
{
 if(!host->editing)return true;
 if(!host->draftReady)return false;
 if(tcard_edit::composing(host->sourceEditor)||tcard_edit::composing(host->titleEditor)||!IsWindowEnabled(host->window)||!IsWindowEnabled(host->sourceEditor))return false;
 double size=0;
 if(!tcard_fonts::read_size(host->fontSizeEditor,size)){
  SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.invalid_size",L"Check size"));
  SetWindowTextW(GetDlgItem(host->window,5024),tcard_text(L"message.font_size",L"Enter a font size from 8 to 48."));return false;
 }
 bool readable=true;
 auto document=host->markdown?tcard_rich::Document{}:tcard_rich_edit::document(host->sourceEditor,false,&readable);
 if(!readable){SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.save_failed",L"Save failed"));return false;}
 host->titleText=read_window_text(host->titleEditor);
 host->sourceText=host->markdown?read_window_text(host->sourceEditor):tcard_rich::serialize(document);
 if(host->sourceText.size()>4*1024*1024){
  SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.too_large",L"Note is too large"));return false;
 }
 host->richHtml=!host->markdown;host->textText=host->sourceText;
 const bool sizeChanged=host->fontSize!=static_cast<float>(size);
 host->fontSize=static_cast<float>(size);
 host->inheritFontFamily=tcard_fonts::inherited(host->fontFamilyEditor);
 host->inheritFontSize=tcard_fonts::inherited(host->fontSizeEditor);
 host->fontFamilyText=tcard_fonts::resolved_family(host->fontFamilyEditor);
 if(host->fontFamilyText.empty())host->fontFamilyText=L"Segoe UI";
 host->state.backColor=host->editColor;host->checkpointRequested=checkpoint;
 sync_legacy_state(host);
 bool saved=true;
 if(host->saveCallback)saved=host->saveCallback(host->saveCallbackContext);
 else if(host->callback)host->callback(3,host->callbackContext);
 host->checkpointRequested=false;
 if(!saved){
  SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.save_failed",L"Save failed"));
  SetWindowTextW(GetDlgItem(host->window,5024),tcard_text(L"message.save_failed",L"Could not save. Your draft is still open."));
  host->saveDue=GetTickCount64()+5000;return false;
 }
 host->editDirty=false;if(checkpoint)host->historyPending=false;
 SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.saved",L"Saved"));
 SetWindowTextW(GetDlgItem(host->window,5024),L"");
 update_read_brush(host);release_render(host);
 if(!host->markdown)tcard_rich_edit::configure(host->sourceEditor,true,host->fontSize,host->fontFamilyText,host->editColor,host->state.textColor,true);
 if(sizeChanged){RECT area{};GetClientRect(host->window,&area);SendMessageW(host->window,WM_SIZE,SIZE_RESTORED,MAKELPARAM(area.right,area.bottom));}
 return true;
}
static void card_layout_tools(TCARD_WUI_HOST host,int width,int height)
{
 int x=card_px(host,10),top=height-card_px(host,38);
 const bool readOnly=!host->editing||host->previewing;
 const auto ids=tcard_tools::items(host->markdown,readOnly);const int count=static_cast<int>(ids.size());
 const bool visible=host->toolsVisible&&!host->appearanceOpen&&(host->editing||host->markdown);
 bool wanted[kToolCount]{};
 if(visible)for(int n=0;n<count;++n)wanted[ids[n]]=true;
 for(int i=0;i<kToolCount;++i){
  HWND button=GetDlgItem(host->window,kToolFirst+i);
  if(button&&((GetWindowLongPtrW(button,GWL_STYLE)&WS_VISIBLE)!=0)!=wanted[i])
   ShowWindow(button,wanted[i]?SW_SHOWNA:SW_HIDE);
 }
 HWND status=GetDlgItem(host->window,kToolStatus);
 if(visible)for(int n=0;n<count;++n){
  int i=ids[n],w=card_px(host,i==6?48:i==11?58:host->markdown?22:26);
  card_move_control(GetDlgItem(host->window,kToolFirst+i),x,top,w,card_px(host,30));x+=w;
 }
 const bool showStatus=visible&&width-x>=card_px(host,56);
 if(showStatus)card_move_control(status,x+card_px(host,4),top,max(1,width-x-card_px(host,14)),card_px(host,30));
 if(status&&((GetWindowLongPtrW(status,GWL_STYLE)&WS_VISIBLE)!=0)!=showStatus)
  ShowWindow(status,showStatus?SW_SHOWNA:SW_HIDE);
}
static void card_toggle_view(TCARD_WUI_HOST host,bool focus=true)
{
 if(!host->markdown)return;
 if(!host->previewing&&(host->editDirty||host->historyPending)&&!card_save_draft(host,true))return;
 host->previewing=!host->previewing;
 if(host->previewing){host->textText=read_window_text(host->sourceEditor);sync_read_text(host);}
 ShowWindow(host->sourceEditor,host->previewing?SW_HIDE:SW_SHOW);
 ShowWindow(host->readEditor,host->previewing?SW_SHOW:SW_HIDE);
 SetWindowTextW(GetDlgItem(host->window,kToolFirst+11),host->previewing?tcard_text(L"button.edit",L"Edit"):tcard_text(L"button.preview",L"Preview"));
 RECT r{};GetClientRect(host->window,&r);SendMessageW(host->window,WM_SIZE,0,MAKELPARAM(r.right,r.bottom));
 if(focus)SetFocus(host->previewing?host->readEditor:host->sourceEditor);
}
static void card_run_tool(TCARD_WUI_HOST host,int tool)
{
 if(tool==11){card_toggle_view(host);return;}
 tcard_tools::Context context{host->window,host->sourceEditor,kToolFirst,host->markdown,host->menuOpen,host->fontFamilyText,{host->state.textColor},[host]{card_mark_dirty(host);}};
 tcard_tools::run(&context,tool);
}
static void card_create_tools(TCARD_WUI_HOST host)
{
 const auto& labels=tcard_tools::labels;
 const auto& keys=tcard_tools::keys;
 const auto& tips=tcard_tools::tips;
 host->toolsTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
  CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,host->window,nullptr,g_instance,nullptr);
 for(int i=0;i<kToolCount;++i){
  if(HWND existing=GetDlgItem(host->window,kToolFirst+i))DestroyWindow(existing);
  HWND button=CreateWindowExW(0,L"BUTTON",i==11?tcard_text(L"button.preview",labels[i]):labels[i],WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
   0,0,20,28,host->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolFirst+i)),g_instance,nullptr);
  SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(host->buttonFont),0);
  SetWindowSubclass(button,tcard_ui::button_proc,1,0);
  host->toolLabels[i]=tcard_text(keys[i],tips[i]);
  if(host->toolsTooltip){
   TTTOOLINFOW info{};info.cbSize=sizeof(info);info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
   info.hwnd=host->window;info.uId=reinterpret_cast<UINT_PTR>(button);info.lpszText=host->toolLabels[i].data();
   SendMessageW(host->toolsTooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
  }
 }
 HWND status=CreateWindowExW(0,L"STATIC",tcard_text(L"status.saved",L"Saved"),WS_CHILD|SS_RIGHT|SS_CENTERIMAGE,
  0,0,20,28,host->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolStatus)),g_instance,nullptr);
 SendMessageW(status,WM_SETFONT,reinterpret_cast<WPARAM>(host->buttonFont),0);
 tcard_tools::attach(host->sourceEditor);host->toolsVisible=true;
}
static LRESULT CALLBACK card_read_proc(HWND control,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data)
{
 auto* host=reinterpret_cast<TCARD_WUI_HOST>(data);
 if(message==WM_NCDESTROY){RemoveWindowSubclass(control,card_read_proc,0x54435250);return DefSubclassProc(control,message,w,l);}
 if(message==WM_LBUTTONDOWN&&!host->editing){
  POINT imagePoint{static_cast<short>(LOWORD(l)),static_cast<short>(HIWORD(l))};if(tcard_image_ui::open(control,&imagePoint))return 0;
  const HWND mainEditor=host->editorOwner?GetDlgItem(host->editorOwner,1006):nullptr;
  if(mainEditor&&IsWindowVisible(mainEditor)){MessageBeep(MB_ICONINFORMATION);return 0;}
  begin_edit(host);
  if(host->sourceEditor){
   POINT point{static_cast<short>(LOWORD(l)),static_cast<short>(HIWORD(l))};
   MapWindowPoints(control,host->sourceEditor,&point,1);
   PostMessageW(host->sourceEditor,message,w,MAKELPARAM(point.x,point.y));return 0;
  }
 }
 return DefSubclassProc(control,message,w,l);
}
static void card_read_tools(TCARD_WUI_HOST host,int width,int height)
{
 if(host->editing)return;
 HWND button=GetDlgItem(host->window,kToolFirst+11);
 if(!host->markdown){if(button)ShowWindow(button,SW_HIDE);return;}
 if(!button){
  button=CreateWindowExW(0,L"BUTTON",tcard_text(L"button.edit",L"Edit"),WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
   0,0,58,30,host->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kToolFirst+11)),g_instance,nullptr);
  SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(host->buttonFont?host->buttonFont:GetStockObject(DEFAULT_GUI_FONT)),0);
  SetWindowSubclass(button,tcard_ui::button_proc,1,0);
 }
 MoveWindow(button,card_px(host,10),height-card_px(host,38),card_px(host,58),card_px(host,30),TRUE);
 card_layout_tools(host,width,height);
}
static void card_update_hover(TCARD_WUI_HOST host)
{
 POINT point{};GetCursorPos(&point);HWND hovered=WindowFromPoint(point),focus=GetFocus();
 const bool visible=host->menuOpen||hovered==host->window||IsChild(host->window,hovered)||focus==host->window||IsChild(host->window,focus);
 if(visible!=host->toolsVisible){
  host->toolsVisible=visible;RECT r{};GetClientRect(host->window,&r);card_layout_tools(host,r.right,r.bottom);
 }
}
static void card_update_timer(TCARD_WUI_HOST host)
{
 card_update_hover(host);
 if(!host->editing)return;
 const auto* rich=tcard_rich_edit::state(host->sourceEditor);
 if(host->previewOnBlur&&!host->menuOpen&&!host->appearanceOpen&&IsWindowEnabled(host->window)&&
    !(rich&&rich->pasteJob)&&!tcard_edit::composing(host->sourceEditor)&&!tcard_edit::composing(host->titleEditor)){
  host->previewOnBlur=false;
  // Commit before leaving the draft; failed saves retain the editor and block incoming synchronization.
  end_edit(host,true);if(!host->editing)return;
 }
 if(const auto* value=tcard_rich_edit::state(host->sourceEditor);value&&value->pasteJob){
  SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.downloading",L"Loading images..."));return;
 }
 if(const auto* value=tcard_rich_edit::state(host->sourceEditor);value&&value->pasteFailed)
  SetWindowTextW(GetDlgItem(host->window,kToolStatus),tcard_text(L"status.paste_failed",L"Paste failed"));
 const auto now=GetTickCount64();
 if(!host->menuOpen&&IsWindowEnabled(host->window)&&!tcard_edit::composing(host->sourceEditor)&&!tcard_edit::composing(host->titleEditor)){
  if(host->editDirty&&now>=host->saveDue)card_save_draft(host,false);
  if(host->historyPending&&!host->editDirty&&now>=host->historyDue)card_save_draft(host,true);
 }
}
