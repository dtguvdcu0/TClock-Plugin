#pragma once
#include "card_images.h"
#include "card_image_viewer.h"
#include <commctrl.h>
#include <functional>
#include <map>

namespace tcard_image_ui {
struct Entry {std::wstring root,uri;int width=0,height=0;double zoom=1;size_t bytes=0;int columns=1;};
using Resize=bool(*)(HWND,DWORD,int);
struct State {std::map<DWORD,Entry> entries;Resize resize=nullptr,preview=nullptr;HWND source=nullptr;bool dragging=false;DWORD identity=0;RECT box{};int requested=0;};
inline constexpr UINT_PTR subclass=0x54434956;
inline LRESULT CALLBACK control_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
inline State* state(HWND control){DWORD_PTR data=0;return GetWindowSubclass(control,control_proc,subclass,&data)?reinterpret_cast<State*>(data):nullptr;}
inline State* attach(HWND control){auto* s=state(control);if(!s){s=new(std::nothrow) State;if(s&&!SetWindowSubclass(control,control_proc,subclass,reinterpret_cast<DWORD_PTR>(s))){delete s;s=nullptr;}}return s;}
inline void clear(HWND control,bool preserve=false){if(auto* s=attach(control)){s->entries.clear();if(!preserve){s->preview=nullptr;s->source=nullptr;s->resize=nullptr;}}}
inline void bind(HWND control,DWORD identity,Entry entry,Resize resize=nullptr){if(auto* s=attach(control)){s->entries[identity]=std::move(entry);s->resize=resize?resize:s->preview;}}
inline bool rectangle(HWND control,const REOBJECT& item,const Entry& entry,RECT& box)
{
    POINTL p{};SendMessageW(control,EM_POSFROMCHAR,reinterpret_cast<WPARAM>(&p),item.cp);
    const UINT dpi=GetDpiForWindow(control);const LONG width=(std::max)(1L,static_cast<LONG>(item.sizel.cx*(dpi?dpi:96)*entry.zoom/2540)),height=(std::max)(1L,static_cast<LONG>(item.sizel.cy*(dpi?dpi:96)*entry.zoom/2540));box={p.x,p.y,p.x+width,p.y+height};return true;
}
inline bool find(HWND control,REOBJECT& found,RECT& box,const POINT* point=nullptr)
{
    auto* s=state(control);if(!s)return false;tcard_image::ComPtr<IRichEditOle> ole;SendMessageW(control,EM_GETOLEINTERFACE,0,reinterpret_cast<LPARAM>(ole.GetAddressOf()));if(!ole)return false;
    CHARRANGE selection{};SendMessageW(control,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    for(LONG i=0;i<ole->GetObjectCount();++i){REOBJECT item{};item.cbStruct=sizeof(item);if(FAILED(ole->GetObject(i,&item,REO_GETOBJ_NO_INTERFACES)))continue;const auto entry=s->entries.find(item.dwUser);if(entry==s->entries.end())continue;RECT r{};rectangle(control,item,entry->second,r);if(point?!PtInRect(&r,*point):!(selection.cpMin==item.cp&&selection.cpMax<=item.cp+1))continue;found=item;box=r;return true;}return false;
}
inline bool open(HWND control,const POINT* point=nullptr){REOBJECT item{};RECT r{};if(!find(control,item,r,point))return false;const auto entry=state(control)->entries.at(item.dwUser);return tcard_viewer::open(control,entry.root,entry.uri)!=nullptr;}
inline UINT menu(HWND owner,POINT point,bool editable)
{
    HMENU menu=CreatePopupMenu();if(!menu)return 0;
    AppendMenuW(menu,MF_STRING,1,tcard_lang::text(L"image.open",L"Open image").c_str());
    if(editable){AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,2,tcard_lang::text(L"image.auto",L"Automatic size").c_str());AppendMenuW(menu,MF_STRING,3,tcard_lang::text(L"image.actual",L"100%").c_str());for(int width:{160,320,640,960})AppendMenuW(menu,MF_STRING,width,(std::to_wstring(width)+L" px").c_str());}
    const UINT command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,owner,nullptr);DestroyMenu(menu);return command;
}
inline bool context(HWND control,POINT screen)
{
    POINT point=screen;REOBJECT item{};RECT r{};const bool keyboard=screen.x==-1&&screen.y==-1;if(keyboard){if(!find(control,item,r))return false;screen={r.left,r.bottom};ClientToScreen(control,&screen);}else{ScreenToClient(control,&point);if(!find(control,item,r,&point))return false;}
    auto* s=state(control);const auto entry=s->entries.at(item.dwUser);const auto resize=s->resize;const UINT command=menu(control,screen,resize&&IsWindowEnabled(control));
    if(command==1)tcard_viewer::open(control,entry.root,entry.uri);else if(command&&resize)resize(control,item.dwUser,command==2?0:command==3?entry.width:command);return true;
}
inline LRESULT CALLBACK control_proc(HWND control,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data)
{
    auto* s=reinterpret_cast<State*>(data);
    if(message==WM_NCDESTROY){RemoveWindowSubclass(control,control_proc,id);delete s;return DefSubclassProc(control,message,w,l);}
    if(message==WM_CONTEXTMENU&&context(control,{GET_X_LPARAM(l),GET_Y_LPARAM(l)}))return 0;
    if(message==WM_LBUTTONDBLCLK){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};if(open(control,&p))return 0;}
    if(message==WM_LBUTTONDOWN){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};REOBJECT item{};RECT r{};
        if(s->resize&&IsWindowEnabled(control)&&find(control,item,r)){RECT handle{r.right-10,r.bottom-10,r.right+5,r.bottom+5};if(PtInRect(&handle,p)){s->dragging=true;s->identity=item.dwUser;s->box=r;s->requested=0;SetCapture(control);return 0;}}
        if(!s->resize&&find(control,item,r,&p)){open(control,&p);return 0;}}
    if(message==WM_MOUSEMOVE&&s->dragging){const int width=static_cast<int>((std::max)(16L,GET_X_LPARAM(l)-s->box.left));s->requested=std::clamp(MulDiv(width,96,GetDpiForWindow(control)),16,8192);SetCursor(LoadCursorW(nullptr,IDC_SIZENWSE));return 0;}
    if(message==WM_LBUTTONUP&&s->dragging){s->dragging=false;ReleaseCapture();const auto resize=s->resize;const DWORD identity=s->identity;const int width=s->requested;if(width&&resize)resize(control,identity,width);return 0;}
    if(message==WM_CAPTURECHANGED)s->dragging=false;
    if(message==WM_KEYDOWN&&w==VK_RETURN&&(GetKeyState(VK_CONTROL)&0x8000)&&open(control))return 0;
    const auto result=DefSubclassProc(control,message,w,l);
    if(message==WM_PAINT&&s->resize){REOBJECT item{};RECT r{};if(find(control,item,r)){HDC dc=GetDC(control);RECT handle{r.right-7,r.bottom-7,r.right,r.bottom};FillRect(dc,&handle,GetSysColorBrush(COLOR_HIGHLIGHT));ReleaseDC(control,dc);}}
    return result;
}
// Fit display extents only. Authored size and source pixels remain independent.
inline void fit(HWND control,tcard_image::Picture& picture,LONG available=0,double zoom=1)
{
    RECT r{};GetClientRect(control,&r);const UINT dpi=GetDpiForWindow(control);const double width=available>0?available:(std::max)(1L,r.right-16)*2540.0/(dpi?dpi:96)/zoom;
    const double height=(std::max)(96.0,(r.bottom-r.top)*0.70)*2540.0/(dpi?dpi:96)/zoom;
    const double scale=(std::min)(1.0,(std::min)(width/(std::max)(1L,picture.width),height/(std::max)(1L,picture.height)));
    picture.width=(std::max)(1L,static_cast<LONG>(picture.width*scale));picture.height=(std::max)(1L,static_cast<LONG>(picture.height*scale));
}
}
