#pragma once

#include <windows.h>
#include <stddef.h>

#ifdef TCARD_WUI_EXPORTS
#define TCARD_WUI_API __declspec(dllexport)
#else
#define TCARD_WUI_API __declspec(dllimport)
#endif

typedef void (WINAPI *TCARD_WUI_COMMAND_CALLBACK)(UINT command, void* context);
typedef BOOL (WINAPI *TCARD_WUI_SAVE_CALLBACK)(void* context);
typedef struct TCARD_WUI_HOST__* TCARD_WUI_HOST;

#define TCARD_WUI_STATE_ABI_VERSION 2
#define TCARD_WUI_TITLE_CAPACITY 1024
#define TCARD_WUI_TEXT_CAPACITY 16384

typedef struct TCARD_WUI_TEXT_STATE {
    DWORD cb;
    DWORD version;
    COLORREF backColor;
    COLORREF textColor;
    COLORREF secondaryColor;
    const WCHAR* title;
    DWORD titleLength;
    const WCHAR* text;
    DWORD textLength;
    const WCHAR* source;
    DWORD sourceLength;
    const WCHAR* fontFamily;
    DWORD fontFamilyLength;
    FLOAT fontSize;
    BOOL markdown;
    ULONGLONG reservedTail;
    BOOL richHtml;
    BOOL checkpoint;
    BOOL inheritFontFamily;
    BOOL inheritFontSize;
} TCARD_WUI_TEXT_STATE;

#define TCARD_WUI_TEXT_STATE_MIN_CB ((DWORD)offsetof(TCARD_WUI_TEXT_STATE, reservedTail))

typedef struct TCARD_WUI_TEXT_STATE_BUFFER {
    DWORD cb;
    DWORD version;
    COLORREF backColor;
    COLORREF textColor;
    COLORREF secondaryColor;
    WCHAR* title;
    DWORD titleCapacity;
    DWORD titleLength;
    WCHAR* text;
    DWORD textCapacity;
    DWORD textLength;
    WCHAR* source;
    DWORD sourceCapacity;
    DWORD sourceLength;
    WCHAR* fontFamily;
    DWORD fontFamilyCapacity;
    DWORD fontFamilyLength;
    FLOAT fontSize;
    BOOL markdown;
    ULONGLONG reservedTail;
    BOOL richHtml;
    BOOL checkpoint;
    BOOL inheritFontFamily;
    BOOL inheritFontSize;
} TCARD_WUI_TEXT_STATE_BUFFER;

#define TCARD_WUI_TEXT_STATE_BUFFER_MIN_CB ((DWORD)offsetof(TCARD_WUI_TEXT_STATE_BUFFER, reservedTail))

typedef struct TCARD_WUI_STATE {
    DWORD cb;
    DWORD version;
    COLORREF backColor;
    COLORREF textColor;
    COLORREF secondaryColor;
    WCHAR title[TCARD_WUI_TITLE_CAPACITY];
    WCHAR text[TCARD_WUI_TEXT_CAPACITY];
    WCHAR source[TCARD_WUI_TEXT_CAPACITY];
} TCARD_WUI_STATE;

#ifdef __cplusplus
extern "C" {
#endif

TCARD_WUI_API TCARD_WUI_HOST WINAPI TCardWuiCreateCard(HWND owner);
TCARD_WUI_API void WINAPI TCardWuiDestroyCard(TCARD_WUI_HOST host);
TCARD_WUI_API BOOL WINAPI TCardWuiSetCardState(TCARD_WUI_HOST host, const TCARD_WUI_STATE* state);
TCARD_WUI_API BOOL WINAPI TCardWuiSetCardTextState(TCARD_WUI_HOST host, const TCARD_WUI_TEXT_STATE* state);
TCARD_WUI_API BOOL WINAPI TCardWuiSetCardAssetRoot(TCARD_WUI_HOST host, const WCHAR* root, DWORD length);
TCARD_WUI_API BOOL WINAPI TCardWuiGetCardState(TCARD_WUI_HOST host, TCARD_WUI_STATE* state);
TCARD_WUI_API BOOL WINAPI TCardWuiGetCardTextState(TCARD_WUI_HOST host, TCARD_WUI_TEXT_STATE_BUFFER* state);
TCARD_WUI_API BOOL WINAPI TCardWuiShowCard(TCARD_WUI_HOST host, BOOL visible);
TCARD_WUI_API HWND WINAPI TCardWuiGetCardWindow(TCARD_WUI_HOST host);
TCARD_WUI_API void WINAPI TCardWuiSetCardCommandCallback(TCARD_WUI_HOST host, TCARD_WUI_COMMAND_CALLBACK callback, void* context);
TCARD_WUI_API void WINAPI TCardWuiSetCardSaveCallback(TCARD_WUI_HOST host, TCARD_WUI_SAVE_CALLBACK callback, void* context);
TCARD_WUI_API BOOL WINAPI TCardWuiExecuteCommand(TCARD_WUI_HOST host, UINT command);
TCARD_WUI_API BOOL WINAPI TCardWuiIsTopmost(TCARD_WUI_HOST host);
TCARD_WUI_API BOOL WINAPI TCardWuiIsCardEditing(TCARD_WUI_HOST host);

TCARD_WUI_API BOOL WINAPI TCardWuiCreateHost(HWND owner);
TCARD_WUI_API void WINAPI TCardWuiDestroyHost(void);
TCARD_WUI_API BOOL WINAPI TCardWuiSetState(const TCARD_WUI_STATE* state);
TCARD_WUI_API BOOL WINAPI TCardWuiShow(BOOL visible);
TCARD_WUI_API HWND WINAPI TCardWuiGetWindow(void);
TCARD_WUI_API void WINAPI TCardWuiSetCommandCallback(TCARD_WUI_COMMAND_CALLBACK callback, void* context);

#ifdef __cplusplus
}
#endif
