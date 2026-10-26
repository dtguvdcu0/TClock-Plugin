#include "led_surface.h"
#include "taskbar_surface.h"
#include "taskbar_edge.h"
#include <new>
#include <cstdio>

struct LED_SURFACE {
    CRITICAL_SECTION lock;
    HANDLE stop,changed,ready,thread;
    LED_SURFACE_STATE state;
    volatile LONG alive;
};
static LRESULT CALLBACK led_handle_surface(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if(message==WM_NCHITTEST)return HTTRANSPARENT;
    if(message==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND)return 1;
    return DefWindowProcW(window,message,wParam,lParam);
}
static DWORD WINAPI led_run_surface(void* parameter)
{
    LED_SURFACE* surface=(LED_SURFACE*)parameter;
    HINSTANCE instance=(HINSTANCE)GetWindowLongPtrW(surface->state.target,GWLP_HINSTANCE);
    // Register against this DLL rather than the borrowed Explorer instance.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&led_run_surface,(HMODULE*)&instance);
    const WCHAR* name=L"TClockLedSurface";
    WNDCLASSW cls={};cls.lpfnWndProc=led_handle_surface;cls.hInstance=instance;cls.lpszClassName=name;
    BOOL registered=RegisterClassW(&cls)!=0;DWORD classError=GetLastError();
    HWND window=NULL;
    LED_CONTEXT* context=led_create();
    HDC memory=CreateCompatibleDC(NULL);
    HBITMAP bitmap=NULL;HGDIOBJ original=NULL;RGBQUAD* pixels=NULL;SIZE allocated={};
    BOOL paused=TRUE;
    if((registered||classError==ERROR_CLASS_ALREADY_EXISTS)&&context&&memory)
        window=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
            name,L"TClock LED display",WS_POPUP,0,0,1,1,NULL,NULL,instance,NULL);
    BOOL timer=window&&SetTimer(window,1,LED_FRAME_MS,NULL)!=0;
    InterlockedExchange(&surface->alive,timer?1:0);SetEvent(surface->ready);
    if(timer){
        HANDLE signals[]={surface->stop,surface->changed};
        BOOL running=TRUE;
        while(running){
            DWORD wait=MsgWaitForMultipleObjects(2,signals,FALSE,INFINITE,QS_ALLINPUT);
            if(wait==WAIT_OBJECT_0)break;
            BOOL frame=wait==WAIT_OBJECT_0+1;
            MSG message;
            while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){
                if(message.message==WM_QUIT){running=FALSE;break;}
                if(message.hwnd==window&&message.message==WM_TIMER&&message.wParam==1)frame=TRUE;
                else {TranslateMessage(&message);DispatchMessageW(&message);}
            }
            if(!running||!frame)continue;
            // Snapshot copies never hold a lock while rendering or calling window APIs.
            LED_SURFACE_STATE state;
            EnterCriticalSection(&surface->lock);state=surface->state;LeaveCriticalSection(&surface->lock);
            if(!IsWindow(state.target)||!IsWindow(state.taskbar))break;
            led_apply_snapshot(context,&state.text);
            RECT viewport={};POINT origin={0,0};
            BOOL visible=state.visible&&IntersectRect(&viewport,&state.bounds,&state.clip)&&
                ClientToScreen(state.target,&origin);
            if(visible){
                RECT display=viewport;OffsetRect(&display,origin.x,origin.y);
                visible=tbs_can_present_rect(state.target,state.taskbar,&display);
            }
            if(!visible){if(IsWindowVisible(window))ShowWindow(window,SW_HIDE);paused=TRUE;continue;}
            SIZE size={viewport.right-viewport.left,viewport.bottom-viewport.top};
            if(size.cx<1||size.cy<1||size.cx>4096||size.cy>2048){if(IsWindowVisible(window))ShowWindow(window,SW_HIDE);paused=TRUE;continue;}
            if(size.cx!=allocated.cx||size.cy!=allocated.cy){
                BITMAPINFO info={};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=size.cx;
                info.bmiHeader.biHeight=size.cy;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                void* bits=NULL;HBITMAP next=CreateDIBSection(NULL,&info,DIB_RGB_COLORS,&bits,NULL,0);
                if(!next)break;
                if(bitmap){SelectObject(memory,original);DeleteObject(bitmap);}
                bitmap=next;pixels=(RGBQUAD*)bits;original=SelectObject(memory,bitmap);allocated=size;
            }
            ULONGLONG tick=GetTickCount64();
            if(paused){if(!led_render_snapshot(context,state.size,tick,FALSE))break;paused=FALSE;}
            if(!led_render_snapshot(context,state.size,tick,TRUE))break;
            ZeroMemory(pixels,(SIZE_T)size.cx*size.cy*sizeof(*pixels));
            RECT clip={0,0,size.cx,size.cy};
            led_blend(context,pixels,size.cx,size.cy,state.bounds.left-viewport.left,state.bounds.top-viewport.top,&clip);
            RECT display=viewport;OffsetRect(&display,origin.x,origin.y);
            tbe_clear_strip(pixels,size.cx,size.cy,&display,state.taskbar,FALSE);
            POINT destination={origin.x+viewport.left,origin.y+viewport.top},source={0,0};
            BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            GdiFlush();
            if(!UpdateLayeredWindow(window,NULL,&destination,&size,memory,&source,0,&blend,ULW_ALPHA))break;
            // Place the LED immediately above its clock host, following the taskbar stacking.
            HWND anchor=IsWindow(state.host)?state.host:state.taskbar;
            HWND preceding=GetWindow(anchor,GW_HWNDPREV);
            if(preceding!=window||!IsWindowVisible(window)){
                if(preceding==window)preceding=GetWindow(window,GW_HWNDPREV);
                SetWindowPos(window,preceding?preceding:HWND_TOP,0,0,0,0,
                    SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
            }
        }
        KillTimer(window,1);
    }
    if(window)DestroyWindow(window);
    InterlockedExchange(&surface->alive,0);
    if(bitmap){SelectObject(memory,original);DeleteObject(bitmap);}
    if(memory)DeleteDC(memory);
    led_destroy(context);
    if(registered)UnregisterClassW(name,instance);
    return 0;
}
LED_SURFACE* led_start_surface(HINSTANCE instance,const LED_SURFACE_STATE* state)
{
    (void)instance;
    if(!state)return NULL;
    LED_SURFACE* surface=new(std::nothrow) LED_SURFACE{};
    if(!surface)return NULL;
    InitializeCriticalSection(&surface->lock);surface->state=*state;
    surface->stop=CreateEventW(NULL,TRUE,FALSE,NULL);surface->changed=CreateEventW(NULL,FALSE,FALSE,NULL);surface->ready=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(surface->stop&&surface->changed&&surface->ready)
        surface->thread=CreateThread(NULL,0,led_run_surface,surface,0,NULL);
    if(!surface->thread||WaitForSingleObject(surface->ready,2000)!=WAIT_OBJECT_0||!InterlockedCompareExchange(&surface->alive,0,0)){
        led_stop_surface(surface);return NULL;
    }
    return surface;
}
BOOL led_publish_surface(LED_SURFACE* surface,const LED_SURFACE_STATE* state)
{
    if(!surface||!state||!InterlockedCompareExchange(&surface->alive,0,0))return FALSE;
    EnterCriticalSection(&surface->lock);surface->state=*state;LeaveCriticalSection(&surface->lock);
    SetEvent(surface->changed);return TRUE;
}
void led_stop_surface(LED_SURFACE* surface)
{
    if(!surface)return;
    if(surface->stop)SetEvent(surface->stop);
    if(surface->thread){WaitForSingleObject(surface->thread,INFINITE);CloseHandle(surface->thread);}
    if(surface->ready)CloseHandle(surface->ready);
    if(surface->changed)CloseHandle(surface->changed);
    if(surface->stop)CloseHandle(surface->stop);
    DeleteCriticalSection(&surface->lock);delete surface;
}
