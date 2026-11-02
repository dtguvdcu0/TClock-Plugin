#pragma once
#include "card_rich_edit.h"
#include "card_media_import.h"
#include "card_language.h"
#include "card_theme.h"
#include <commdlg.h>
#include <functional>
#include <cwctype>
#include <span>
#pragma comment(lib,"comdlg32.lib")

namespace tcard_tools {
inline constexpr int count=13;
inline constexpr const wchar_t* labels[]={L"B",L"I",L"U",L"ab",L"\u2022",L"A",L"100%",L"\u2500",L"H",L"\u2197",L"<>",L"Preview",L"Img"};
inline constexpr const wchar_t* keys[]={L"tool.bold",L"tool.italic",L"tool.underline",L"tool.strike",L"tool.list",L"tool.color",L"tool.scale",L"tool.checkpoint",L"tool.heading",L"tool.link",L"tool.code",L"tool.view",L"tool.image"};
inline constexpr const wchar_t* tips[]={L"Bold",L"Italic",L"Underline",L"Strikethrough",L"List",L"Text color",L"Selection scale (%)",L"Today's stopping point",L"Heading",L"Link",L"Code",L"Edit / Preview",L"Insert image..."};
inline std::span<const int> items(bool markdown,bool preview=false)
{
    static constexpr int rich[]={0,1,2,3,4,5,6,7,12},md[]={0,1,3,8,4,9,10,7,12,11},view[]={11};
    return preview?std::span<const int>(view):markdown?std::span<const int>(md):std::span<const int>(rich);
}
inline const wchar_t* text(const wchar_t* key,const wchar_t* fallback){static thread_local std::wstring value;value=tcard_lang::text(key,fallback);return value.c_str();}
inline bool insert_uri(HWND editor,bool markdown,std::wstring uri)
{
    auto input=tcard_clip::image_bytes(uri);return !input.empty()&&tcard_media::begin(editor,markdown,std::move(input));
}
inline void failed(HWND owner){MessageBoxW(owner,text(L"message.image_failed",L"The image could not be inserted. Check its format and size."),L"TCard",MB_OK|MB_ICONINFORMATION);}
inline bool insert_file(HWND owner,HWND editor,bool markdown)
{
    wchar_t path[32768]{};OPENFILENAMEW picker{sizeof(picker)};picker.hwndOwner=owner;picker.lpstrFile=path;picker.nMaxFile=ARRAYSIZE(path);
    picker.lpstrFilter=L"Images (*.png;*.jpg;*.jpeg;*.gif;*.bmp)\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0\0";picker.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&picker))return false;const std::wstring selected=path;
    const bool okay=tcard_media::begin(editor,markdown,{},selected);if(!okay)failed(owner);return okay;
}
struct MarkdownImage {size_t first=0,last=0,titleFirst=0,titleLast=0;std::wstring uri,title;};
struct MarkdownImages {
    const std::wstring& source;std::vector<MarkdownImage> images;
    static int block(MD_BLOCKTYPE,void*,void*){return 0;}static int leave(MD_SPANTYPE,void*,void*){return 0;}static int text(MD_TEXTTYPE,const MD_CHAR*,MD_SIZE,void*){return 0;}
    static int enter(MD_SPANTYPE type,void* detail,void* context){if(type!=MD_SPAN_IMG)return 0;auto& self=*static_cast<MarkdownImages*>(context);const auto& image=*static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        const auto base=reinterpret_cast<uintptr_t>(self.source.data()),src=reinterpret_cast<uintptr_t>(image.src.text);if(src<base||(src-base)%sizeof(wchar_t))return 0;const size_t at=(src-base)/sizeof(wchar_t);if(at>self.source.size()||image.src.size>self.source.size()-at)return 0;
        const auto first=self.source.rfind(L"![",at);if(first==std::wstring::npos)return 0;const auto label=self.source.find(L"](",first);if(label==std::wstring::npos||label>=at)return 0;
        size_t end=at+image.src.size,titleFirst=end,titleLast=end;if(image.title.size){const auto title=reinterpret_cast<uintptr_t>(image.title.text);if(title<base||(title-base)%sizeof(wchar_t))return 0;titleFirst=(title-base)/sizeof(wchar_t);titleLast=titleFirst+image.title.size;if(titleLast>self.source.size()||titleFirst<=end)return 0;end=titleLast+1;}
        if(end<self.source.size()&&self.source[end]==L'>')++end;while(end<self.source.size()&&iswspace(self.source[end]))++end;if(end>=self.source.size()||self.source[end]!=L')')return 0;
        self.images.push_back({first,end+1,titleFirst,titleLast,tcard_md::attribute(image.src),tcard_md::attribute(image.title)});return 0;
    }
};
inline std::vector<MarkdownImage> markdown_images(const std::wstring& source)
{
    MarkdownImages images{source,{}};MD_PARSER parser{};parser.flags=MD_FLAG_TABLES|MD_FLAG_TASKLISTS|MD_FLAG_STRIKETHROUGH|MD_FLAG_FOOTNOTES|MD_FLAG_ADMONITIONS;parser.enter_block=parser.leave_block=MarkdownImages::block;parser.enter_span=MarkdownImages::enter;parser.leave_span=MarkdownImages::leave;parser.text=MarkdownImages::text;
    if(md_parse(source.data(),static_cast<MD_SIZE>(source.size()),&parser,&images))return {};return images.images;
}
inline bool markdown_size(HWND editor,const MarkdownImage& image,int width)
{
    auto title=image.title;const auto marker=title.rfind(L"[tcard-size:");if(marker!=std::wstring::npos&&title.back()==L']'){title.erase(marker);while(!title.empty()&&iswspace(title.back()))title.pop_back();}
    if(width){if(!title.empty())title+=L' ';title+=L"[tcard-size:"+std::to_wstring(std::clamp(width,1,8192))+L"]";}
    auto raw=tcard_rich_edit::raw_text(editor);if(image.last>raw.size())return false;const bool hadTitle=image.titleFirst!=image.titleLast;
    size_t first=hadTitle?image.titleFirst-1:image.last-1,last=hadTitle?image.titleLast+1:first;std::wstring replacement;
    if(!title.empty()){replacement=hadTitle?L"\"":L" \"";for(wchar_t c:title){if(c==L'"'||c==L'\\')replacement+=L'\\';replacement+=c;}replacement+=L'"';}
    CHARRANGE selection{};POINT scroll{};SendMessageW(editor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));SendMessageW(editor,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    if(raw.size()-(last-first)+replacement.size()>static_cast<size_t>(SendMessageW(editor,EM_GETLIMITTEXT,0,0)))return false;
    SendMessageW(editor,EM_STOPGROUPTYPING,0,0);SendMessageW(editor,EM_SETSEL,first,last);SendMessageW(editor,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(replacement.c_str()));SendMessageW(editor,EM_STOPGROUPTYPING,0,0);
    selection.cpMin=selection.cpMax=static_cast<LONG>(image.first);SendMessageW(editor,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&selection));SendMessageW(editor,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));return true;
}
inline bool preview_resize(HWND control,DWORD identity,int width)
{
    auto* actions=tcard_image_ui::state(control);auto* view=tcard_md::view(control);if(!actions||!view||!actions->source||!IsWindow(actions->source))return false;const HWND editor=actions->source;
    if(!IsWindowEnabled(editor)||tcard_media::busy(editor)||tcard_edit::composing(editor)||tcard_edit::assets(editor)!=view->assetRoot)return false;
    const auto raw=tcard_rich_edit::raw_text(editor);std::wstring shown;shown.reserve(view->source.size());for(size_t i=0;i<view->source.size();++i){const auto c=view->source[i];if(c==L'\r'&&i+1<view->source.size()&&view->source[i+1]==L'\n')++i;shown+=c==L'\n'?L'\r':c;}if(raw!=shown)return false;auto images=markdown_images(raw);std::erase_if(images,[](const MarkdownImage& image){return !tcard_asset::relative(image.uri,true)&&!image.uri.starts_with(L"data:image/");});
    if(images.size()!=actions->entries.size()||!identity||identity>images.size()||!actions->entries.contains(identity)||images[identity-1].uri!=actions->entries.at(identity).uri)return false;
    if(!markdown_size(editor,images[identity-1],width))return false;
    tcard_md::render(control,tcard_rich_edit::raw_text(editor),true,view->size,view->family,view->assetRoot,view->paper);return true;
}
inline void preview_images(HWND control,HWND editor)
{
    if(auto* s=tcard_image_ui::attach(control)){s->source=editor;s->preview=editor?preview_resize:nullptr;s->resize=s->preview;}
}
inline bool markdown_menu(HWND editor,POINT screen)
{
    CHARRANGE selection{};SendMessageW(editor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));const auto raw=tcard_rich_edit::raw_text(editor);
    for(const auto& image:markdown_images(raw)){if(selection.cpMin<static_cast<LONG>(image.first)||selection.cpMin>static_cast<LONG>(image.last))continue;
        if(!tcard_asset::relative(image.uri,true)&&!image.uri.starts_with(L"data:image/"))return false;
        if(screen.x==-1&&screen.y==-1){POINTL position{};SendMessageW(editor,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&position),selection.cpMin);screen={position.x,position.y+20};ClientToScreen(editor,&screen);}
        const UINT command=tcard_image_ui::menu(editor,screen,IsWindowEnabled(editor));
        if(command==1)tcard_viewer::open(editor,tcard_edit::assets(editor),image.uri);
        else if(command){int width=command==2?0:command==3?-1:command;
            if(width<0){const auto uri=tcard_asset::relative(image.uri,true)?tcard_asset::load_image(tcard_edit::assets(editor),image.uri):image.uri;auto picture=tcard_image::decode(uri);if(picture.dib.empty())return true;width=MulDiv(picture.width,96,2540);}markdown_size(editor,image,width);}
        return true;
    }return false;
}
inline LRESULT CALLBACK paste_proc(HWND editor,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR)
{
    if(message==WM_NCDESTROY){RemoveWindowSubclass(editor,paste_proc,id);return DefSubclassProc(editor,message,w,l);}
    const auto* rich=tcard_rich_edit::state(editor);
    if(message==WM_CONTEXTMENU&&(!rich||!rich->logical)&&markdown_menu(editor,{GET_X_LPARAM(l),GET_Y_LPARAM(l)}))return 0;
    if(message==WM_PASTE&&!(GetKeyState(VK_SHIFT)&0x8000)&&(!rich||!rich->logical)&&OpenClipboard(editor)){
        tcard_clip::ClipboardData bitmap;
        try{bitmap.png=tcard_clip::clipboard_data(RegisterClipboardFormatW(L"PNG"),tcard_clip::max_image);}catch(...){}
        if(bitmap.png.empty())try{bitmap.dib=tcard_clip::clipboard_data(CF_DIB,tcard_codec::inputLimit-sizeof(BITMAPFILEHEADER));}catch(...){}
        CloseClipboard();
        if(!bitmap.png.empty()||!bitmap.dib.empty()){if(!tcard_media::clipboard(editor,std::move(bitmap),true))failed(GetParent(editor));return 0;}
    }
    return DefSubclassProc(editor,message,w,l);
}
inline void attach(HWND editor){tcard_media::attach(editor);SetWindowSubclass(editor,paste_proc,0x5443494d,0);}
struct Context {
    HWND window,sourceEditor;int first;bool markdown;bool& menuOpen;std::wstring fontFamilyText;
    struct {COLORREF textColor;} state;
    std::function<void()> changed;
};
inline void tool_mark_dirty(Context* host){if(host->changed)host->changed();}
inline INT_PTR CALLBACK tool_scale_proc(HWND dialog,UINT message,WPARAM wParam,LPARAM lParam)
{
 if(const INT_PTR painted=tcard_ui::paint_settings(dialog,message,wParam,lParam))return painted;
 if(message==WM_INITDIALOG){
  SetWindowTextW(dialog,text(L"scale.custom",L"Text scale (%)"));
  const UINT dpi=GetDpiForWindow(dialog);const int d=dpi?dpi:96;
  CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",std::to_wstring(static_cast<int>(lParam)).c_str(),
   WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,MulDiv(12,d,96),MulDiv(12,d,96),MulDiv(85,d,96),MulDiv(26,d,96),
   dialog,reinterpret_cast<HMENU>(100),GetModuleHandleW(nullptr),nullptr);
  CreateWindowExW(0,L"BUTTON",text(L"button.apply",L"Apply"),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
   MulDiv(110,d,96),MulDiv(12,d,96),MulDiv(70,d,96),MulDiv(26,d,96),dialog,reinterpret_cast<HMENU>(IDOK),GetModuleHandleW(nullptr),nullptr);
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
inline int tool_get_scale(Context* host)
{
 return tcard_rich_edit::selection_scale(host->sourceEditor);
}
inline void tool_change_scale(Context* host)
{
 const int current=tool_get_scale(host);
 HMENU menu=CreatePopupMenu();if(!menu)return;
 for(int n:{90,100,110,125,150})AppendMenuW(menu,MF_STRING|(n==current?MF_CHECKED:0),n,(std::to_wstring(n)+L"%").c_str());
 AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,1000,text(L"scale.custom",L"Custom..."));
 RECT r{};GetWindowRect(GetDlgItem(host->window,host->first+6),&r);host->menuOpen=true;
 int percent=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.left,r.top,host->window,nullptr);DestroyMenu(menu);
 if(percent==1000){
  struct Template {DLGTEMPLATE dialog;WORD menu,windowClass,title;} layout{};
  layout.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;layout.dialog.cx=135;layout.dialog.cy=42;
  percent=static_cast<int>(DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&layout.dialog,host->window,tool_scale_proc,current));
 }
 host->menuOpen=false;
 if(percent>=50&&percent<=200&&tcard_rich_edit::apply(host->sourceEditor,0,percent))
  SetWindowTextW(GetDlgItem(host->window,host->first+6),(std::to_wstring(percent)+L"%").c_str());
}
inline void tool_insert_date(Context* host)
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
 tool_mark_dirty(host);SetFocus(host->sourceEditor);
}
inline void tool_markdown_tool(Context* host,int tool)
{
 CHARRANGE selection{};SendMessageW(host->sourceEditor,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
 const auto raw=tcard_rich_edit::raw_text(host->sourceEditor);
 std::wstring chosen=raw.substr(min(raw.size(),static_cast<size_t>(selection.cpMin)),
  min(raw.size()-min(raw.size(),static_cast<size_t>(selection.cpMin)),static_cast<size_t>(selection.cpMax-selection.cpMin)));
 if(chosen.empty())chosen=text(L"editor.text",L"text");
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
 SendMessageW(host->sourceEditor,EM_STOPGROUPTYPING,0,0);tool_mark_dirty(host);SetFocus(host->sourceEditor);
}
inline void run(Context* host,int tool)
{
 if(tcard_edit::composing(host->sourceEditor))return;
 if(tool==11)return;
 if(tool==12){host->menuOpen=true;
  if(host->markdown&&markdown_menu(host->sourceEditor,{-1,-1})){host->menuOpen=false;return;}
  REOBJECT image{};RECT box{};if(!host->markdown&&tcard_image_ui::find(host->sourceEditor,image,box)){tcard_image_ui::context(host->sourceEditor,{-1,-1});host->menuOpen=false;return;}
  insert_file(host->window,host->sourceEditor,host->markdown);host->menuOpen=false;return;}
 if(tool==7){tool_insert_date(host);return;}
 if(host->markdown){tool_markdown_tool(host,tool);return;}
 if(tool==6){tool_change_scale(host);return;}
 if(tool==4){tcard_rich_edit::bullet(host->sourceEditor);return;}
 if(tool==5){
  HMENU menu=CreatePopupMenu();if(!menu)return;
  const COLORREF colors[]={host->state.textColor,RGB(208,48,48),RGB(25,105,180),RGB(27,125,65),RGB(139,70,166)};
  const wchar_t* colorNames[]={L"Default",L"Red",L"Blue",L"Green",L"Purple"};
  for(int i=0;i<5;++i)AppendMenuW(menu,MF_STRING,i+1,text((std::wstring(L"ink.")+colorNames[i]).c_str(),colorNames[i]));
  AppendMenuW(menu,MF_STRING,10,text(L"scale.custom",L"Custom..."));
  RECT r{};GetWindowRect(GetDlgItem(host->window,host->first+5),&r);host->menuOpen=true;
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
}

namespace tcard_tools {
// Text toolbar labels show their own effect: bold B, italic I, underlined U, struck ab, monospace code.
// Commands without a format effect use short words (list, link, scale, checkpoint, image, view); widths follow the label.
inline std::wstring label(HWND button,int tool)
{
    if(tool==4)return text(L"tool.label_list",L"List");
    if(tool==7)return text(L"tool.label_checkpoint",L"Stop");
    if(tool==9)return text(L"tool.label_link",L"Link");
    if(tool==12)return text(L"tool.label_image",L"Image");
    const int length=GetWindowTextLengthW(button);std::wstring value(static_cast<size_t>((std::max)(0,length))+1,L'\0');
    value.resize(static_cast<size_t>((std::max)(0,GetWindowTextW(button,value.data(),length+1))));return value;
}
inline HFONT label_font(int tool,UINT dpi)
{
    const bool word=tool==4||tool==6||tool==7||tool==9||tool==11||tool==12;
    const int size=tcard_ui::dip_to_px(word?12:15,dpi);
    const int weight=tool==0||tool==8?FW_BOLD:word?FW_NORMAL:FW_SEMIBOLD;
    return CreateFontW(-size,0,0,0,weight,tool==1,tool==2,tool==3,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_DONTCARE,tool==10?L"Consolas":tool==1?L"Georgia":L"Segoe UI");
}
inline int width(HWND button,int tool)
{
    const UINT dpi=button?GetDpiForWindow(button):96;const int minimum=tcard_ui::dip_to_px(28,dpi),padding=tcard_ui::dip_to_px(14,dpi);
    HDC dc=button?GetDC(button):nullptr;if(!dc)return minimum;
    HFONT font=label_font(tool,dpi);const auto old=font?SelectObject(dc,font):nullptr;const auto value=label(button,tool);
    SIZE extent{};const bool measured=GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&extent)!=FALSE;
    if(old)SelectObject(dc,old);if(font)DeleteObject(font);ReleaseDC(button,dc);
    return measured?(std::max)(minimum,static_cast<int>(extent.cx)+padding):minimum;
}
// Flat toolbar button: paper background, hover/pressed shade, no frame; the color command carries a red bar.
inline void paint(const DRAWITEMSTRUCT& item,int tool,COLORREF paper,COLORREF ink)
{
    POINT cursor{};GetCursorPos(&cursor);ScreenToClient(item.hwndItem,&cursor);
    const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0,hot=!disabled&&PtInRect(&item.rcItem,cursor);
    const UINT dpi=GetDpiForWindow(item.hwndItem);
    SetDCBrushColor(item.hDC,paper);FillRect(item.hDC,&item.rcItem,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if(hot||pressed){
        RECT face=item.rcItem;InflateRect(&face,-tcard_ui::dip_to_px(1,dpi),-tcard_ui::dip_to_px(2,dpi));
        HBRUSH shade=CreateSolidBrush(tcard_ui::shade_color(paper,pressed?0.84f:0.91f));HPEN edge=CreatePen(PS_NULL,0,0);
        const auto oldBrush=SelectObject(item.hDC,shade),oldPen=SelectObject(item.hDC,edge);const int radius=tcard_ui::dip_to_px(6,dpi);
        RoundRect(item.hDC,face.left,face.top,face.right+1,face.bottom+1,radius,radius);
        SelectObject(item.hDC,oldBrush);SelectObject(item.hDC,oldPen);DeleteObject(shade);DeleteObject(edge);
    }
    HFONT font=label_font(tool,dpi);const auto old=font?SelectObject(item.hDC,font):nullptr;
    SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,disabled?RGB(160,160,160):ink);
    RECT text=item.rcItem;const auto value=label(item.hwndItem,tool);
    if(tool==5)text.bottom-=tcard_ui::dip_to_px(3,dpi);
    DrawTextW(item.hDC,value.c_str(),-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    if(tool==5){
        const int half=tcard_ui::dip_to_px(6,dpi),center=(item.rcItem.left+item.rcItem.right)/2,top=(item.rcItem.top+item.rcItem.bottom)/2+tcard_ui::dip_to_px(7,dpi);
        RECT bar{center-half,top,center+half,top+tcard_ui::dip_to_px(3,dpi)};
        SetDCBrushColor(item.hDC,disabled?RGB(200,200,200):RGB(214,48,39));FillRect(item.hDC,&bar,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    if(old)SelectObject(item.hDC,old);if(font)DeleteObject(font);
    if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT)){RECT focus=item.rcItem;InflateRect(&focus,-2,-3);DrawFocusRect(item.hDC,&focus);}
}
}
