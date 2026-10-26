#define NOMINMAX
#include "led_clock.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>
#include <cstdio>

struct LED_CONTEXT {
    LED_OPTIONS options = {};
    SIZE size = {};
    std::vector<BYTE> pixels;
    bool ready = false;
    int item = 0, preview = -2;
    ULONGLONG cycle = 0, lastTick = 0;
    double elapsed = 0;
    SYSTEMTIME textTime = {};
    bool textReady = false;
    int columns = 1;
    WCHAR clockText[LED_TEXT_MAX+1] = {};
    WCHAR expanded[LED_MESSAGE_MAX][LED_TEXT_MAX+1] = {};
};
static const COLORREF led_colors[7] = {
    RGB(255,73,62), RGB(255,173,48), RGB(114,226,70), RGB(243,245,237),
    RGB(71,140,255), RGB(112,220,244), RGB(239,114,211)
};
static int led_bound(DWORD value, int low, int high, int fallback)
{ return value < (DWORD)low || value > (DWORD)high ? fallback : (int)value; }
void led_load(LED_OPTIONS* o, LED_READ_LONG number, LED_READ_STRING text)
{
    if (!o || !number || !text) return;
    ZeroMemory(o, sizeof(*o));
    const char* section = "ExtendedDisplay";
    o->clock = number(section,"LedShowClock",1)!=0;
    o->date = number(section,"LedShowDate",0)!=0;
    // Raw UTF-8 format boundary shared with message formats; no ACP conversion.
    char dateValue[LED_FORMAT_MAX*4+1];
    text(section,"LedDateFormat",dateValue,sizeof(dateValue),"<%yyyy%>/<%mm%>/<%dd%>");
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,dateValue,-1,o->dateFormat,_countof(o->dateFormat)))
        wcscpy_s(o->dateFormat,L"<%yyyy%>/<%mm%>/<%dd%>");
    o->seconds = number(section,"LedShowSeconds",0)!=0;
    o->colon = number(section,"LedShowColon",1)!=0;
    o->frame = number(section,"LedFrame",1)!=0;
    o->count = led_bound(number(section,"LedMessageCount",1),0,3,1);
    o->columns = led_bound(number(section,"LedMaxChars",number(section,"LedColumns",6)),0,LED_TEXT_MAX,0);
    o->speed = led_bound(number(section,"LedSpeed",40),5,50,40);
    o->brightness = led_bound(number(section,"LedBrightness",100),20,100,100);
    o->clockSeconds = led_bound(number(section,"LedClockSeconds",8),1,60,8);
    o->clockColor = led_bound(number(section,"LedClockColor",2),1,7,2);
    const WCHAR* defaults[3]={L"\"HELLO TCLOCK \"<%yyyy%>",L"",L""};
    for (int i=0;i<3;++i) {
        char key[40], value[LED_FORMAT_MAX*4+1];
        sprintf_s(key,"LedFormat%d",i+1); text(section,key,value,sizeof(value),"\x1D");
        bool legacy = value[0] == '\x1D' && value[1] == 0;
        if (legacy) { sprintf_s(key,"LedText%d",i+1); text(section,key,value,sizeof(value),"\x1D"); }
        // Raw UTF-8 format boundary: quotes are syntax, not INI wrappers.
        WCHAR format[LED_FORMAT_MAX+1] = {};
        if (legacy && value[0] == '\x1D' && value[1] == 0) wcscpy_s(o->messages[i].text,defaults[i]);
        else if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,format,_countof(format))) {
            if (legacy) {
                WCHAR* begin=format;size_t length=wcslen(begin);
                // Remove accumulated legacy storage wrappers once, then retain a literal expression.
                while(length>=2 && begin[0]==L'"' && begin[length-1]==L'"'){++begin;length-=2;begin[length]=0;}
                if(length+2<=LED_FORMAT_MAX)swprintf_s(o->messages[i].text,L"\"%ls\"",begin);
            } else wcscpy_s(o->messages[i].text,format);
        }
        sprintf_s(key,"LedText%dSeconds",i+1);o->messages[i].seconds=led_bound(number(section,key,4),1,60,4);
        sprintf_s(key,"LedText%dEffect",i+1);o->messages[i].effect=led_bound(number(section,key,i==0?2:0),0,2,i==0?2:0);
        sprintf_s(key,"LedText%dColor",i+1);o->messages[i].color=led_bound(number(section,key,2),1,7,2);
    }
}
BOOL led_store(const LED_OPTIONS* o, LED_WRITE_LONG number, LED_WRITE_STRING text)
{
    if (!o || !number || !text) return FALSE;
    BOOL ok=TRUE; const char* section="ExtendedDisplay";
#define LED_SAVE(key, value) ok = number(section,key,(DWORD)(value)) && ok
    LED_SAVE("LedShowDate",o->date);
    // Raw UTF-8 format boundary: date quotes are syntax, not INI wrappers.
    char dateValue[LED_FORMAT_MAX*4+1];
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,o->dateFormat,-1,dateValue,sizeof(dateValue),NULL,NULL))return FALSE;
    ok=text(section,"LedDateFormat",dateValue)&&ok;
    LED_SAVE("LedShowClock",o->clock); LED_SAVE("LedShowSeconds",o->seconds); LED_SAVE("LedShowColon",o->colon);
    LED_SAVE("LedFrame",o->frame);
    LED_SAVE("LedMessageCount",o->count); LED_SAVE("LedMaxChars",o->columns); LED_SAVE("LedSpeed",o->speed);
    LED_SAVE("LedBrightness",o->brightness);
    LED_SAVE("LedClockSeconds",o->clockSeconds); LED_SAVE("LedClockColor",o->clockColor);
    for (int i=0;i<3;++i) {
        char key[40], value[LED_FORMAT_MAX*4+1];
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,o->messages[i].text,-1,value,sizeof(value),NULL,NULL))return FALSE;
        sprintf_s(key,"LedFormat%d",i+1);ok=text(section,key,value)&&ok;
        sprintf_s(key,"LedText%dSeconds",i+1);LED_SAVE(key,o->messages[i].seconds);
        sprintf_s(key,"LedText%dEffect",i+1);LED_SAVE(key,o->messages[i].effect);
        sprintf_s(key,"LedText%dColor",i+1);LED_SAVE(key,o->messages[i].color);
    }
#undef LED_SAVE
    return ok;
}
BOOL led_validate_text(const WCHAR* text)
{
    if (!text) return FALSE;
    for (int i=0;text[i];++i)if(i>=LED_TEXT_MAX || text[i]<32 || text[i]>126)return FALSE;
    return TRUE;
}
BOOL led_expand_text(const WCHAR* format,const SYSTEMTIME* time,WCHAR* output)
{
    if(!format || !time || !output)return FALSE;
    output[0]=0;
    if(wcslen(format)>LED_FORMAT_MAX)return FALSE;
    WCHAR expanded[LED_TEXT_MAX+2] = {};
    if(!FormatDisplayTextW(format,time,expanded,_countof(expanded)) || !led_validate_text(expanded))return FALSE;
    wcscpy_s(output,LED_TEXT_MAX+1,expanded);return TRUE;
}
BOOL led_expand_clock(const LED_OPTIONS* o,const SYSTEMTIME* time,WCHAR* output)
{
    if(!o||!time||!output)return FALSE;
    output[0]=0;WCHAR date[LED_TEXT_MAX+1]={},clock[9];
    if(o->seconds)swprintf_s(clock,L"%02u%ls%02u%ls%02u",time->wHour,o->colon?L":" : L"",time->wMinute,o->colon?L":" : L"",time->wSecond);
    else swprintf_s(clock,L"%02u%ls%02u",time->wHour,o->colon?L":" : L"",time->wMinute);
    if(o->date&&!led_expand_text(o->dateFormat,time,date))return FALSE;
    size_t length=wcslen(date),clockLength=wcslen(clock);
    if(length+(length?1:0)+clockLength>LED_TEXT_MAX)return FALSE;
    if(length)swprintf_s(output,LED_TEXT_MAX+1,L"%ls %ls",date,clock);
    else wcscpy_s(output,LED_TEXT_MAX+1,clock);
    return TRUE;
}
int led_get_columns(LED_CONTEXT* c,const SYSTEMTIME* time)
{
    if(!c || !time)return 1;
    SYSTEMTIME stamp=*time;stamp.wMilliseconds=0;
    if(!c->textReady || memcmp(&stamp,&c->textTime,sizeof(stamp))) {
        c->clockText[0]=0;
        if(c->options.clock&&!led_expand_clock(&c->options,time,c->clockText))wcscpy_s(c->clockText,L"?");
        int longest=(int)wcslen(c->clockText);
        for(int i=0;i<LED_MESSAGE_MAX;++i) {
            c->expanded[i][0]=0;
            if(i<c->options.count) {
                // Unsupported dynamic output is explicit, never silently transliterated.
                if(!led_expand_text(c->options.messages[i].text,time,c->expanded[i]))wcscpy_s(c->expanded[i],L"?");
                const WCHAR* visible=c->expanded[i];while(*visible==L' ')++visible;
                if(*visible)longest=std::max(longest,(int)wcslen(c->expanded[i]));
            }
        }
        c->columns=c->options.columns?c->options.columns:std::max(1,longest);
        c->textTime=stamp;c->textReady=true;
    }
    return c->columns;
}
LED_CONTEXT* led_create(void) { return new(std::nothrow) LED_CONTEXT; }
void led_destroy(LED_CONTEXT* c) { delete c; }
void led_reset(LED_CONTEXT* c) { if(c){c->ready=false;c->textReady=false;c->item=0;c->cycle=0;c->elapsed=0;c->lastTick=0;} }
BOOL led_take_snapshot(LED_CONTEXT* c,const SYSTEMTIME* time,LED_SNAPSHOT* snapshot)
{
    if(!c||!time||!snapshot)return FALSE;
    led_get_columns(c,time);snapshot->options=c->options;snapshot->time=c->textTime;snapshot->columns=c->columns;
    wcscpy_s(snapshot->clockText,c->clockText);memcpy(snapshot->expanded,c->expanded,sizeof(snapshot->expanded));return TRUE;
}
void led_apply_snapshot(LED_CONTEXT* c,const LED_SNAPSHOT* snapshot)
{
    if(!c||!snapshot)return;
    led_configure(c,&snapshot->options);
    c->columns=snapshot->columns;c->textTime=snapshot->time;c->textReady=true;
    wcscpy_s(c->clockText,snapshot->clockText);memcpy(c->expanded,snapshot->expanded,sizeof(c->expanded));
}
BOOL led_render_snapshot(LED_CONTEXT* c,SIZE size,ULONGLONG tick,BOOL animate)
{
    // No INI or formatter access on the independent presentation thread.
    return c&&c->textReady?led_render(c,size,&c->textTime,tick,animate):FALSE;
}
void led_configure(LED_CONTEXT* c,const LED_OPTIONS* o)
{ if(c&&o&&memcmp(&c->options,o,sizeof(*o))){c->options=*o;led_reset(c);} }
BOOL led_get_size(int heightDip,int cross,UINT dpi,BOOL vertical,int columns,SIZE* size)
{
    if (!size || columns<1 || columns>LED_TEXT_MAX || cross<2 || dpi<48 || dpi>768 || heightDip<0 || heightDip>256)return FALSE;
    int height=heightDip?MulDiv(heightDip,dpi,96):std::min(cross-2,MulDiv(32,dpi,96));
    height=std::min(height,4096*11/(columns*6+3));
    if (vertical)height=std::min(height,std::max(1,(int)((LONGLONG)(cross-2)*11/(columns*6+3))));
    else height=std::min(height,cross-2);
    if(height<1){size->cx=size->cy=0;return FALSE;}
    size->cx=std::max(1,MulDiv(height,columns*6+3,11));
    if(vertical)size->cx=std::min((int)size->cx,cross-2);
    size->cy=height;return TRUE;
}
static const BYTE led_glyphs[95][7] = {
    {0,0,0,0,0,0,0}, // ASCII 32
    {4,4,4,4,4,0,4}, // ASCII 33
    {10,10,10,0,0,0,0}, // ASCII 34
    {10,31,10,10,31,10,0}, // ASCII 35
    {4,15,20,14,5,30,4}, // ASCII 36
    {25,26,2,4,8,11,19}, // ASCII 37
    {12,18,20,8,21,18,13}, // ASCII 38
    {4,4,8,0,0,0,0}, // ASCII 39
    {2,4,8,8,8,4,2}, // ASCII 40
    {8,4,2,2,2,4,8}, // ASCII 41
    {0,21,14,31,14,21,0}, // ASCII 42
    {0,4,4,31,4,4,0}, // ASCII 43
    {0,0,0,0,4,4,8}, // ASCII 44
    {0,0,0,31,0,0,0}, // ASCII 45
    {0,0,0,0,0,4,4}, // ASCII 46
    {1,2,2,4,8,8,16}, // ASCII 47
    {14,17,19,21,25,17,14}, // ASCII 48
    {4,12,4,4,4,4,14}, // ASCII 49
    {14,17,1,2,4,8,31}, // ASCII 50
    {30,1,1,14,1,1,30}, // ASCII 51
    {2,6,10,18,31,2,2}, // ASCII 52
    {31,16,16,30,1,1,30}, // ASCII 53
    {14,16,16,30,17,17,14}, // ASCII 54
    {31,1,2,4,8,8,8}, // ASCII 55
    {14,17,17,14,17,17,14}, // ASCII 56
    {14,17,17,15,1,1,14}, // ASCII 57
    {0,4,4,0,4,4,0}, // ASCII 58
    {0,4,4,0,4,4,8}, // ASCII 59
    {1,2,4,8,4,2,1}, // ASCII 60
    {0,0,31,0,31,0,0}, // ASCII 61
    {16,8,4,2,4,8,16}, // ASCII 62
    {14,17,1,2,4,0,4}, // ASCII 63
    {14,17,23,21,23,16,14}, // ASCII 64
    {14,17,17,31,17,17,17}, // ASCII 65
    {30,17,17,30,17,17,30}, // ASCII 66
    {15,16,16,16,16,16,15}, // ASCII 67
    {30,17,17,17,17,17,30}, // ASCII 68
    {31,16,16,30,16,16,31}, // ASCII 69
    {31,16,16,30,16,16,16}, // ASCII 70
    {14,17,16,23,17,17,14}, // ASCII 71
    {17,17,17,31,17,17,17}, // ASCII 72
    {14,4,4,4,4,4,14}, // ASCII 73
    {7,2,2,2,18,18,12}, // ASCII 74
    {17,18,20,24,20,18,17}, // ASCII 75
    {16,16,16,16,16,16,31}, // ASCII 76
    {17,27,21,21,17,17,17}, // ASCII 77
    {17,25,21,19,17,17,17}, // ASCII 78
    {14,17,17,17,17,17,14}, // ASCII 79
    {30,17,17,30,16,16,16}, // ASCII 80
    {14,17,17,17,21,18,13}, // ASCII 81
    {30,17,17,30,20,18,17}, // ASCII 82
    {15,16,16,14,1,1,30}, // ASCII 83
    {31,4,4,4,4,4,4}, // ASCII 84
    {17,17,17,17,17,17,14}, // ASCII 85
    {17,17,17,17,17,10,4}, // ASCII 86
    {17,17,17,21,21,21,10}, // ASCII 87
    {17,17,10,4,10,17,17}, // ASCII 88
    {17,17,10,4,4,4,4}, // ASCII 89
    {31,1,2,4,8,16,31}, // ASCII 90
    {14,8,8,8,8,8,14}, // ASCII 91
    {16,8,8,4,2,2,1}, // ASCII 92
    {14,2,2,2,2,2,14}, // ASCII 93
    {4,10,17,0,0,0,0}, // ASCII 94
    {0,0,0,0,0,0,31}, // ASCII 95
    {8,4,2,0,0,0,0}, // ASCII 96
    {14,17,17,31,17,17,17}, // ASCII 97
    {30,17,17,30,17,17,30}, // ASCII 98
    {15,16,16,16,16,16,15}, // ASCII 99
    {30,17,17,17,17,17,30}, // ASCII 100
    {31,16,16,30,16,16,31}, // ASCII 101
    {31,16,16,30,16,16,16}, // ASCII 102
    {14,17,16,23,17,17,14}, // ASCII 103
    {17,17,17,31,17,17,17}, // ASCII 104
    {14,4,4,4,4,4,14}, // ASCII 105
    {7,2,2,2,18,18,12}, // ASCII 106
    {17,18,20,24,20,18,17}, // ASCII 107
    {16,16,16,16,16,16,31}, // ASCII 108
    {17,27,21,21,17,17,17}, // ASCII 109
    {17,25,21,19,17,17,17}, // ASCII 110
    {14,17,17,17,17,17,14}, // ASCII 111
    {30,17,17,30,16,16,16}, // ASCII 112
    {14,17,17,17,21,18,13}, // ASCII 113
    {30,17,17,30,20,18,17}, // ASCII 114
    {15,16,16,14,1,1,30}, // ASCII 115
    {31,4,4,4,4,4,4}, // ASCII 116
    {17,17,17,17,17,17,14}, // ASCII 117
    {17,17,17,17,17,10,4}, // ASCII 118
    {17,17,17,21,21,21,10}, // ASCII 119
    {17,17,10,4,10,17,17}, // ASCII 120
    {17,17,10,4,4,4,4}, // ASCII 121
    {31,1,2,4,8,16,31}, // ASCII 122
    {3,4,4,8,4,4,3}, // ASCII 123
    {4,4,4,4,4,4,4}, // ASCII 124
    {24,4,4,2,4,4,24}, // ASCII 125
    {0,0,9,22,0,0,0}, // ASCII 126
};

static int led_palette(int fixed)
{ return fixed>=1&&fixed<=7?fixed-1:1; }
struct LED_FRAME { WCHAR text[LED_TEXT_MAX+1]; double offset,duration; int color,effect,count; };
static LED_FRAME led_sample(LED_CONTEXT* c,const SYSTEMTIME* time)
{
    LED_FRAME f={};int entries[4],count=0;
    led_get_columns(c,time);
    if(c->options.clock)entries[count++]=-1;
    for(int i=0;i<c->options.count;++i){const WCHAR* text=c->expanded[i];while(*text==L' ')++text;if(*text)entries[count++]=i;}
    if(c->preview>=-1){
        int selected=c->preview;
        count=selected<0?(c->options.clock?1:0):(selected<c->options.count&&c->expanded[selected][0]?1:0);
        if(count)entries[0]=selected;
    }
    f.count=count;if(!count){f.duration=1;return f;}
    c->item%=count;int current=entries[c->item];int hold;
    if(current<0){
        wcscpy_s(f.text,c->clockText);
        hold=c->options.clockSeconds;f.color=led_palette(c->options.clockColor);
    }else{
        wcscpy_s(f.text,c->expanded[current]);hold=c->options.messages[current].seconds;
        f.effect=c->options.messages[current].effect;f.color=led_palette(c->options.messages[current].color);
    }
    int cols=c->columns*6-1,width=(int)wcslen(f.text)*6-1;
    double center=(cols-width)/2.0,speed=c->options.speed;
    f.offset=center;f.duration=hold;
    if(f.effect==1){double entry=(cols-center)/speed;f.duration=entry+hold+(center+width)/speed;
        f.offset=c->elapsed<entry?cols-c->elapsed*speed:c->elapsed<entry+hold?center:center-(c->elapsed-entry-hold)*speed;
    }else if(f.effect==2){f.duration=std::max((double)hold,(cols+width)/speed);f.offset=cols-c->elapsed*speed;}
    return f;
}
BOOL led_render(LED_CONTEXT* c,SIZE size,const SYSTEMTIME* time,ULONGLONG tick,BOOL animate)
{
    if(!c||!time||size.cx<1||size.cy<1||size.cx>4096||size.cy>2048)return FALSE;
    try {
        if(!c->ready||tick<c->lastTick){c->elapsed=0;c->lastTick=tick;c->ready=true;}
        if(animate)c->elapsed+=std::min(0.2,(tick-c->lastTick)/1000.0);
        c->lastTick=tick;LED_FRAME f=led_sample(c,time);
        if(f.count&&c->elapsed>=f.duration){c->elapsed=0;if(++c->item>=f.count){c->item=0;++c->cycle;}f=led_sample(c,time);}
        if(c->preview>=-1&&!animate)f.offset=(c->columns*6-1-((int)wcslen(f.text)*6-1))/2.0;
        c->size=size;c->pixels.assign((size_t)size.cx*size.cy*4,0);
        // Opaque clean frame; no textures or embedded raster assets.
        int radius=c->options.frame?std::max(1,(int)size.cy/12):0;
        for(int y=0;y<size.cy;++y)for(int x=0;x<size.cx;++x){int ex=std::min(x,(int)size.cx-1-x),ey=std::min(y,(int)size.cy-1-y);if(radius&&ex<radius&&ey<radius&&(ex-radius)*(ex-radius)+(ey-radius)*(ey-radius)>radius*radius)continue;
            BYTE* p=c->pixels.data()+((size_t)y*size.cx+x)*4;bool edge=c->options.frame&&(ex==0||ey==0);p[0]=edge?50:16;p[1]=edge?43:12;p[2]=edge?36:9;p[3]=255;
        }
        const int cols=c->columns*6-1;double pitch=std::min(size.cx/(cols+4.0),size.cy/11.0),left=(size.cx-cols*pitch)/2,top=(size.cy-7*pitch)/2;
        // Translate an unchanged dot raster in device pixels; never crossfade adjacent LEDs.
        const double position=f.effect&&animate?f.offset:std::floor(f.offset+0.5);
        const int shift=(int)std::floor(position*pitch+0.5);
        const int clipLeft=std::max(0,(int)std::ceil(left)),clipRight=std::min((int)size.cx-1,(int)std::ceil(left+cols*pitch)-1);
        COLORREF color=led_colors[f.color];double bright=c->options.brightness/100.0;
        for(int i=0;f.text[i];++i){WCHAR ch=f.text[i];if(ch>=L'a'&&ch<=L'z')ch-=L'a'-L'A';if(ch<32||ch>126)ch=L'?';
            for(int y=0;y<7;++y)for(int x=0;x<5;++x){if(!(led_glyphs[ch-32][y]&(1<<(4-x))))continue;
                double cx=left+(i*6+x+.5)*pitch,cy=top+(y+.5)*pitch,r=pitch*.32;
                int x0=std::max(clipLeft,(int)std::floor(cx-r)+shift),x1=std::min(clipRight,(int)std::ceil(cx+r)+shift);
                int y0=std::max(0,(int)std::floor(cy-r)),y1=std::min((int)size.cy-1,(int)std::ceil(cy+r));
                for(int py=y0;py<=y1;++py)for(int px=x0;px<=x1;++px){
                    // Evaluate coverage at the original raster coordinate so edge pixels stay identical.
                    double dx=(px-shift)+.5-cx,dy=py+.5-cy,d=std::sqrt(dx*dx+dy*dy),a=std::max(0.0,std::min(1.0,r+.5-d));
                    if(a<=0)continue;
                    BYTE* p=c->pixels.data()+((size_t)py*size.cx+px)*4;double rgb[3]={GetBValue(color)*bright,GetGValue(color)*bright,GetRValue(color)*bright};
                    for(int k=0;k<3;++k)p[k]=(BYTE)std::min(255.0,p[k]*(1-a)+rgb[k]*a);p[3]=255;
                }
            }
        }
        return TRUE;
    }catch(...){return FALSE;}
}
BOOL led_render_preview(LED_CONTEXT* c,SIZE size,const SYSTEMTIME* time,ULONGLONG tick,int target,BOOL animate)
{
    if(!c||target<-2||target>=LED_MESSAGE_MAX)return FALSE;
    if(c->preview!=target){c->preview=target;led_reset(c);}
    return led_render(c,size,time,tick,animate);
}
BOOL led_is_active(const LED_CONTEXT* c)
{ return c&&c->options.count>0; }
void led_draw(const LED_CONTEXT* c,HDC dc,int x,int y)
{
    if(!c||c->pixels.empty())return;
    BITMAPINFO info={};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=c->size.cx;
    info.bmiHeader.biHeight=-c->size.cy;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    SetDIBitsToDevice(dc,x,y,c->size.cx,c->size.cy,0,0,0,c->size.cy,c->pixels.data(),&info,DIB_RGB_COLORS);
}
void led_blend(const LED_CONTEXT* c,RGBQUAD* pixels,int width,int height,int x,int y,const RECT* clip)
{
    if(!c||c->pixels.empty()||!pixels||!clip||width<=0||height<=0)return;
    int left=std::max(0,std::max((int)clip->left,x)),top=std::max(0,std::max((int)clip->top,y));
    int right=std::min(width,std::min((int)clip->right,x+(int)c->size.cx)),bottom=std::min(height,std::min((int)clip->bottom,y+(int)c->size.cy));
    for(int dy=top;dy<bottom;++dy)for(int dx=left;dx<right;++dx){const BYTE* source=c->pixels.data()+((size_t)(dy-y)*c->size.cx+dx-x)*4;BYTE* target=(BYTE*)(pixels+(size_t)(height-1-dy)*width+dx);unsigned inverse=255-source[3];for(int k=0;k<4;++k)target[k]=(BYTE)(source[k]+(target[k]*inverse+127)/255);}
}

const BYTE* led_get_pixels(const LED_CONTEXT* context)
{
    return context && !context->pixels.empty() ? context->pixels.data() : nullptr;
}
