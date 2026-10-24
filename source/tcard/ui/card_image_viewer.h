#pragma once
#include "card_image_codec.h"
#include "card_language.h"
#include "../core/card_assets.h"
#include <thread>
#include <atomic>
#include <memory>
#include <windowsx.h>

namespace tcard_viewer {
inline constexpr UINT request=WM_APP+0x173;inline constexpr UINT_PTR timer=0x54435649;
inline std::wstring class_name(){return L"TCard.ImageViewer."+std::to_wstring(GetCurrentProcessId());}
struct Request {const wchar_t* root;const wchar_t* uri;HWND origin;bool show;};
struct Job {std::atomic_bool done=false;std::vector<BYTE> dib;std::jthread worker;~Job(){worker.request_stop();if(worker.joinable())worker.join();}};
struct State {HWND origin=nullptr;std::vector<BYTE> dib;UINT width=0,height=0;double zoom=1;bool fit=true,drag=false;POINT pan{},mouse{};std::unique_ptr<Job> job;};
inline RECT area(HWND window){RECT r{};GetClientRect(window,&r);r.top=36;return r;}
inline void constrain(HWND window,State& s){const auto r=area(window);const LONG x=(std::max)(0L,static_cast<LONG>((s.width*s.zoom-(r.right-r.left))/2)),y=(std::max)(0L,static_cast<LONG>((s.height*s.zoom-(r.bottom-r.top))/2));s.pan.x=std::clamp(s.pan.x,-x,x);s.pan.y=std::clamp(s.pan.y,-y,y);}
inline void scale(HWND window,State& s,double zoom,bool fit=false){const auto r=area(window);s.fit=fit;if(fit&&s.width&&s.height)zoom=(std::min)(1.0,(std::min)(double((std::max)(1L,r.right))/s.width,double((std::max)(1L,r.bottom-r.top))/s.height));s.zoom=std::clamp(zoom,0.02,8.0);constrain(window,s);InvalidateRect(window,nullptr,FALSE);}
inline LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){s=new(std::nothrow) State;if(!s)return FALSE;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcW(window,message,w,l);
    switch(message){
    case WM_CREATE:{const wchar_t* labels[]={L"Fit",L"100%",L"-",L"+"};const wchar_t* keys[]={L"image.fit",L"image.actual",L"image.zoom_out",L"image.zoom_in"};for(int i=0;i<4;++i)CreateWindowExW(0,L"BUTTON",tcard_lang::text(keys[i],labels[i]).c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,4+i*70,4,66,28,window,reinterpret_cast<HMENU>(INT_PTR(1+i)),GetModuleHandleW(nullptr),nullptr);return 0;}
    case request:{const auto& r=*reinterpret_cast<const Request*>(l);s->job.reset();KillTimer(window,timer);s->dib.clear();s->dib.shrink_to_fit();s->width=s->height=0;s->origin=r.origin;s->pan={};s->fit=true;SetWindowLongPtrW(window,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(GetAncestor(r.origin,GA_ROOT)));
        const std::wstring root=r.root,uri=r.uri;s->job=std::make_unique<Job>();auto* job=s->job.get();
        job->worker=std::jthread([job,root,uri](std::stop_token stop){try{auto bytes=tcard_asset::relative(uri,true)?tcard_clip::image_bytes(tcard_asset::load_image(root,uri)):tcard_clip::image_bytes(uri);if(!stop.stop_requested()){tcard_codec::Source source(bytes);auto dib=source.pixels();if(!stop.stop_requested())job->dib=std::move(dib);}}catch(...){}job->done=true;});
        SetTimer(window,timer,50,nullptr);InvalidateRect(window,nullptr,TRUE);if(r.show){ShowWindow(window,SW_SHOWNORMAL);SetForegroundWindow(window);SetFocus(window);}return TRUE;}
    case WM_TIMER:if(w==timer&&s->job&&s->job->done.load()){KillTimer(window,timer);s->job->worker.join();s->dib=std::move(s->job->dib);s->job.reset();if(s->dib.size()>=sizeof(BITMAPINFOHEADER)){BITMAPINFOHEADER h{};memcpy(&h,s->dib.data(),sizeof(h));s->width=h.biWidth;s->height=-h.biHeight;}scale(window,*s,1,true);return 0;}break;
    case WM_COMMAND:if(LOWORD(w)>=1&&LOWORD(w)<=4){const int command=LOWORD(w);if(command<=2)s->pan={};scale(window,*s,command==1?1:command==2?1:s->zoom*(command==3?0.8:1.25),command==1);SetFocus(window);return 0;}break;
    case WM_SIZE:if(s->fit)scale(window,*s,1,true);else{constrain(window,*s);InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_MOUSEWHEEL:if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL)scale(window,*s,s->zoom*(GET_WHEEL_DELTA_WPARAM(w)>0?1.25:0.8));else{s->pan.y+=GET_WHEEL_DELTA_WPARAM(w);constrain(window,*s);InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_LBUTTONDOWN:if(GET_Y_LPARAM(l)>=36){s->drag=true;s->mouse={GET_X_LPARAM(l),GET_Y_LPARAM(l)};SetCapture(window);SetFocus(window);}return 0;
    case WM_MOUSEMOVE:if(s->drag){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};s->pan.x+=p.x-s->mouse.x;s->pan.y+=p.y-s->mouse.y;s->mouse=p;constrain(window,*s);InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_LBUTTONUP:case WM_CAPTURECHANGED:s->drag=false;if(GetCapture()==window)ReleaseCapture();return 0;
    case WM_KEYDOWN:if(w==VK_ESCAPE){SendMessageW(window,WM_CLOSE,0,0);return 0;}if(w==VK_ADD||w==VK_OEM_PLUS)scale(window,*s,s->zoom*1.25);if(w==VK_SUBTRACT||w==VK_OEM_MINUS)scale(window,*s,s->zoom*0.8);if(w==VK_LEFT)s->pan.x+=40;if(w==VK_RIGHT)s->pan.x-=40;if(w==VK_UP)s->pan.y+=40;if(w==VK_DOWN)s->pan.y-=40;constrain(window,*s);InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=BeginPaint(window,&paint);RECT r=area(window);FillRect(dc,&r,GetSysColorBrush(COLOR_WINDOW));if(s->width&&s->height&&!s->dib.empty()){const int width=static_cast<int>(s->width*s->zoom),height=static_cast<int>(s->height*s->zoom);SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);StretchDIBits(dc,(r.right-width)/2+s->pan.x,r.top+(r.bottom-r.top-height)/2+s->pan.y,width,height,0,0,s->width,s->height,s->dib.data()+sizeof(BITMAPINFOHEADER),reinterpret_cast<const BITMAPINFO*>(s->dib.data()),DIB_RGB_COLORS,SRCCOPY);}else{const auto text=tcard_lang::text(s->job?L"image.loading":L"message.image_failed",s->job?L"Loading image...":L"The image could not be opened.");DrawTextW(dc,text.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}EndPaint(window,&paint);return 0;}
    case WM_CLOSE:{const HWND origin=s->origin;const bool focus=GetForegroundWindow()==window;DestroyWindow(window);if(focus&&IsWindow(origin)&&IsWindowVisible(origin)&&IsWindowEnabled(origin)){SetForegroundWindow(GetAncestor(origin,GA_ROOT));SetFocus(origin);}return 0;}
    case WM_NCDESTROY:KillTimer(window,timer);SetWindowLongPtrW(window,GWLP_USERDATA,0);delete s;break;
    }
    return DefWindowProcW(window,message,w,l);
}
inline HWND open(HWND origin,const std::wstring& root,const std::wstring& uri,bool show=true)
{
    if(!IsWindow(origin)||(!tcard_asset::relative(uri,true)&&!uri.starts_with(L"data:image/")))return nullptr;
    const auto name=class_name();HWND window=FindWindowW(name.c_str(),nullptr);
    if(!window){WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);cls.lpszClassName=name.c_str();if(!RegisterClassExW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;
        MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromWindow(origin,MONITOR_DEFAULTTONEAREST),&monitor))return nullptr;const RECT r=monitor.rcWork;const int width=(std::min)(1000L,r.right-r.left),height=(std::min)(760L,r.bottom-r.top);
        window=CreateWindowExW(0,name.c_str(),tcard_lang::text(L"image.viewer",L"Image - TCard").c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,r.left+(r.right-r.left-width)/2,r.top+(r.bottom-r.top-height)/2,width,height,GetAncestor(origin,GA_ROOT),nullptr,GetModuleHandleW(nullptr),nullptr);}
    if(window){Request data{root.c_str(),uri.c_str(),origin,show};SendMessageW(window,request,0,reinterpret_cast<LPARAM>(&data));}return window;
}
}
