#pragma once

static void begin_edit(TCARD_WUI_HOST host);
static void end_edit(TCARD_WUI_HOST host,bool save);
constexpr int kToolFirst=5100,kToolCount=12,kToolStatus=5150,kMenuButton=5151;
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
 if(tcard_edit::composing(host->sourceEditor)||tcard_edit::composing(host->titleEditor)||!IsWindowEnabled(host->window))return false;
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
 const int rich[]={0,1,2,3,4,5,6,7},md[]={0,1,3,8,4,9,10,7,11},preview[]={11};
 const bool readOnly=!host->editing||host->previewing;
 const int* ids=readOnly?preview:host->markdown?md:rich;
 const int count=readOnly?1:host->markdown?9:8;
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
static INT_PTR CALLBACK card_scale_proc(HWND dialog,UINT message,WPARAM wParam,LPARAM lParam)
{
 if(const INT_PTR painted=tcard_ui::paint_settings(dialog,message,wParam,lParam))return painted;
 if(message==WM_INITDIALOG){
  SetWindowTextW(dialog,tcard_text(L"scale.custom",L"Text scale (%)"));
  const UINT dpi=GetDpiForWindow(dialog);const int d=dpi?dpi:96;
  CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",std::to_wstring(static_cast<int>(lParam)).c_str(),
   WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,MulDiv(12,d,96),MulDiv(12,d,96),MulDiv(85,d,96),MulDiv(26,d,96),
   dialog,reinterpret_cast<HMENU>(100),g_instance,nullptr);
  CreateWindowExW(0,L"BUTTON",tcard_text(L"button.apply",L"Apply"),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
   MulDiv(110,d,96),MulDiv(12,d,96),MulDiv(70,d,96),MulDiv(26,d,96),dialog,reinterpret_cast<HMENU>(IDOK),g_instance,nullptr);
  tcard_ui::prepare_settings(dialog);
  SendDlgItemMessageW(dialog,100,EM_SETLIMITTEXT,3,0);SendDlgItemMessageW(dialog,100,EM_SETSEL,0,-1);SetFocus(GetDlgItem(dialog,100));return FALSE;
 }
 if(message==WM_COMMAND&&LOWORD(wParam)==IDOK){
  BOOL ok=FALSE;const UINT percent=GetDlgItemInt(dialog,100,&ok,FALSE);
  if(ok&&percent>=50&&percent<=200){EndDialog(dialog,percent);return TRUE;}
  MessageBeep(MB_ICONINFORMATION);SetFocus(GetDlgItem(dialog,100));return TRUE;
 }
 if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(wParam)==IDCANCEL)){EndDialog(dialog,0);return TRUE;}
 return FALSE;
}
static int card_get_scale(TCARD_WUI_HOST host)
{
 return tcard_rich_edit::selection_scale(host->sourceEditor);
}
static void card_change_scale(TCARD_WUI_HOST host)
{
 const int current=card_get_scale(host);
 HMENU menu=CreatePopupMenu();if(!menu)return;
 for(int n:{90,100,110,125,150})AppendMenuW(menu,MF_STRING|(n==current?MF_CHECKED:0),n,(std::to_wstring(n)+L"%").c_str());
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,1000,tcard_text(L"scale.custom",L"Custom..."));
 RECT r{};GetWindowRect(GetDlgItem(host->window,kToolFirst+6),&r);host->menuOpen=true;
 int percent=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.left,r.top,host->window,nullptr);DestroyMenu(menu);
 if(percent==1000){
  struct Template {DLGTEMPLATE dialog;WORD menu,windowClass,title;} layout{};
  layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;layout.dialog.cx=135;layout.dialog.cy=42;
  percent=static_cast<int>(DialogBoxIndirectParamW(g_instance,&layout.dialog,host->window,card_scale_proc,current));
 }
 host->menuOpen=false;
 if(percent>=50&&percent<=200&&tcard_rich_edit::apply(host->sourceEditor,0,percent))
  SetWindowTextW(GetDlgItem(host->window,kToolFirst+6),(std::to_wstring(percent)+L"%").c_str());
}
static void card_insert_date(TCARD_WUI_HOST host)
{
 SYSTEMTIME now{};GetLocalTime(&now);wchar_t stamp[64]{};
 swprintf_s(stamp,L"\u2500\u2500 %04u/%02u/%02u %02u:%02u:%02u --",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
 const auto text=tcard_rich_edit::raw_text(host->sourceEditor);
 CHARRANGE selection{};SendMessageW(host->sourceEditor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
 selection.cpMin=selection.cpMax;SendMessageW(host->sourceEditor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));
 std::wstring insertion;
 if(selection.cpMax>0&&static_cast<size_t>(selection.cpMax)<=text.size()&&text[selection.cpMax-1]!=L'\r'&&text[selection.cpMax-1]!=L'\n')insertion+=L"\r\n";
 insertion+=stamp;insertion+=L"\r\n";
 const auto limit=SendMessageW(host->sourceEditor,EM_GETLIMITTEXT,0,0);
 const size_t nativeLength=insertion.size()-std::count(insertion.begin(),insertion.end(),L'\n');
 if(text.size()+nativeLength>static_cast<size_t>(limit)){MessageBeep(MB_ICONINFORMATION);return;}
 const auto tom=host->markdown?Microsoft::WRL::ComPtr<ITextDocument>{}:tcard_rich_edit::text_document(host->sourceEditor);
 if(tom)tom->BeginEditCollection();
 SendMessageW(host->sourceEditor,EM_STOPGROUPTYPING,0,0);
 SendMessageW(host->sourceEditor,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(insertion.c_str()));
 if(!host->markdown){
  const auto after=tcard_rich_edit::raw_text(host->sourceEditor);
  const LONG end=selection.cpMax+static_cast<LONG>(after.size()-text.size());
  const LONG begin=end-26;
  CHARRANGE marker{(std::max)(0L,begin),end};SendMessageW(host->sourceEditor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&marker));
  auto face=tcard_rich_edit::format({},host->fontFamilyText);face.dwEffects&=~CFE_AUTOCOLOR;face.crTextColor=host->state.textColor;
  SendMessageW(host->sourceEditor,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&face));
  PARAFORMAT2 para{};para.cbSize=sizeof(para);para.dwMask=PFM_NUMBERING|PFM_STARTINDENT|PFM_OFFSET;
  SendMessageW(host->sourceEditor,EM_SETPARAFORMAT,0,reinterpret_cast<LPARAM>(&para));
  SendMessageW(host->sourceEditor,EM_SETSEL,end,end);
 }
 SendMessageW(host->sourceEditor,EM_STOPGROUPTYPING,0,0);
 if(tom)tom->EndEditCollection();
 card_mark_dirty(host);SetFocus(host->sourceEditor);
}
static void card_markdown_tool(TCARD_WUI_HOST host,int tool)
{
 CHARRANGE selection{};SendMessageW(host->sourceEditor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
 const auto raw=tcard_rich_edit::raw_text(host->sourceEditor);
 std::wstring chosen=raw.substr(min(raw.size(),static_cast<size_t>(selection.cpMin)),
  min(raw.size()-min(raw.size(),static_cast<size_t>(selection.cpMin)),static_cast<size_t>(selection.cpMax-selection.cpMin)));
 if(chosen.empty())chosen=tcard_text(L"editor.text",L"text");
 std::wstring result;
 if(tool==0)result=L"**"+chosen+L"**";
 if(tool==1)result=L"*"+chosen+L"*";
 if(tool==3)result=L"~~"+chosen+L"~~";
 if(tool==4)result=L"- "+chosen;
 if(tool==8)result=L"## "+chosen;
 if(tool==9)result=L"["+chosen+L"](https://)";
 if(tool==10)result=std::wstring(1,96)+chosen+std::wstring(1,96);
 SendMessageW(host->sourceEditor,EM_STOPGROUPTYPING,0,0);
 SendMessageW(host->sourceEditor,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(result.c_str()));
 SendMessageW(host->sourceEditor,EM_STOPGROUPTYPING,0,0);card_mark_dirty(host);SetFocus(host->sourceEditor);
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
 if(tcard_edit::composing(host->sourceEditor))return;
 if(tool==11){card_toggle_view(host);return;}
 if(tool==7){card_insert_date(host);return;}
 if(host->markdown){card_markdown_tool(host,tool);return;}
 if(tool==6){card_change_scale(host);return;}
 if(tool==4){tcard_rich_edit::bullet(host->sourceEditor);return;}
 if(tool==5){
  HMENU menu=CreatePopupMenu();if(!menu)return;
  const COLORREF colors[]={host->state.textColor,RGB(208,48,48),RGB(25,105,180),RGB(27,125,65),RGB(139,70,166)};
  const wchar_t* labels[]={L"Default",L"Red",L"Blue",L"Green",L"Purple"};
  for(int i=0;i<5;++i)AppendMenuW(menu,MF_STRING,i+1,tcard_text((std::wstring(L"ink.")+labels[i]).c_str(),labels[i]));
  AppendMenuW(menu,MF_STRING,10,tcard_text(L"scale.custom",L"Custom..."));
  RECT r{};GetWindowRect(GetDlgItem(host->window,kToolFirst+5),&r);host->menuOpen=true;
  int command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.left,r.top,host->window,nullptr);DestroyMenu(menu);
  COLORREF color=host->state.textColor;bool apply=false;
  if(command>=1&&command<=5){color=colors[command-1];apply=true;}
  if(command==10){static COLORREF custom[16]{};CHOOSECOLORW chooser{sizeof(chooser)};
   chooser.hwndOwner=host->window;chooser.rgbResult=color;chooser.lpCustColors=custom;chooser.Flags=CC_FULLOPEN|CC_RGBINIT;
   if(ChooseColorW(&chooser)){color=chooser.rgbResult;apply=true;}}
  host->menuOpen=false;if(apply)tcard_rich_edit::apply(host->sourceEditor,0,0,&color);return;
 }
 const unsigned marks[]={tcard_rich::Bold,tcard_rich::Italic,tcard_rich::Underline,tcard_rich::Strike};
 if(tool>=0&&tool<4)tcard_rich_edit::apply(host->sourceEditor,marks[tool]);
}
static void card_create_tools(TCARD_WUI_HOST host)
{
 const wchar_t* labels[]={L"B",L"I",L"U",L"ab",L"\u2261",L"A",L"100%",L"\u2500",L"H",L"\u2197",L"<>",L"Preview"};
 const wchar_t* keys[]={L"tool.bold",L"tool.italic",L"tool.underline",L"tool.strike",L"tool.list",L"tool.color",L"tool.scale",L"tool.checkpoint",L"tool.heading",L"tool.link",L"tool.code",L"tool.view"};
 const wchar_t* tips[]={L"Bold",L"Italic",L"Underline",L"Strikethrough",L"List",L"Text color",L"Selection scale (%)",L"Today's stopping point",L"Heading",L"Link",L"Code",L"Edit / Preview"};
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
 host->toolsVisible=true;
}
static LRESULT CALLBACK card_read_proc(HWND control,UINT message,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data)
{
 auto* host=reinterpret_cast<TCARD_WUI_HOST>(data);
 if(message==WM_NCDESTROY){RemoveWindowSubclass(control,card_read_proc,0x54435250);return DefSubclassProc(control,message,w,l);}
 if(message==WM_LBUTTONDOWN&&!host->editing){
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
