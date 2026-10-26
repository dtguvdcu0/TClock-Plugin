#ifndef TCLOCK_PLUGIN_API_H
#define TCLOCK_PLUGIN_API_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TC_PLUGIN_ABI 1u
#define TC_PLUGIN_INFO_ID L"tclock.plugin/1"
#define TC_PLUGIN_ACTIONS_ID L"tclock.actions/1"
#define TC_PLUGIN_SETTINGS_ID L"tclock.settings/1"
#define TC_PLUGIN_ID_CHARS 32
#define TC_PLUGIN_LABEL_CHARS 128
typedef struct TC_PLUGIN_INFO {
    UINT size;
    WCHAR id[TC_PLUGIN_ID_CHARS];
    WCHAR name[TC_PLUGIN_LABEL_CHARS];
} TC_PLUGIN_INFO;
typedef struct TC_PLUGIN_ACTION {
    UINT size;
    WCHAR id[TC_PLUGIN_ID_CHARS];
    WCHAR label[TC_PLUGIN_LABEL_CHARS];
} TC_PLUGIN_ACTION;
typedef struct TC_ACTION_CONTEXT {
    UINT size;
    HWND owner;
    LANGID language;
    HRESULT (WINAPI *copy_text)(HWND owner, LPCWSTR text);
} TC_ACTION_CONTEXT;
typedef struct TC_ACTION_PROVIDER {
    UINT size;
    UINT (WINAPI *count)(void);
    HRESULT (WINAPI *describe)(UINT index, LANGID language, TC_PLUGIN_ACTION* out);
    HRESULT (WINAPI *invoke)(LPCWSTR action, const TC_ACTION_CONTEXT* context);
} TC_ACTION_PROVIDER;
typedef struct TC_SETTINGS_CONTEXT {
    UINT size;
    HWND owner;
    LANGID language;
    LPCWSTR ini_path;
} TC_SETTINGS_CONTEXT;
typedef struct TC_PLUGIN_SETTINGS {
    UINT size;
    HRESULT (WINAPI *reload)(const TC_SETTINGS_CONTEXT* context);
    HRESULT (WINAPI *show)(const TC_SETTINGS_CONTEXT* context);
} TC_PLUGIN_SETTINGS;

#define TC_PLUGIN_EXTENSION_ID L"tclock.extension/1"
#define TC_PLUGIN_COLOR_DATA 0x54435043u
typedef struct TC_EXTENSION_CONTEXT {
    UINT size;
    HWND owner;
    LANGID language;
    LPCWSTR ini_path;
    LPCWSTR legacy_ini;
    void (WINAPI *changed)(HWND page);
} TC_EXTENSION_CONTEXT;
typedef struct TC_TEXT_CONTEXT {
    UINT size;
    UINT kind;
    COLORREF base;
    SYSTEMTIME time;
    BOOL holiday;
    BOOL vpn;
} TC_TEXT_CONTEXT;
typedef struct TC_PLUGIN_EXTENSION {
    UINT size;
    HRESULT (WINAPI *reload)(const TC_EXTENSION_CONTEXT* context);
    LPCWSTR (WINAPI *label)(LANGID language);
    HWND (WINAPI *create)(const TC_EXTENSION_CONTEXT* context);
    HRESULT (WINAPI *apply)(HWND page);
    COLORREF (WINAPI *color)(const TC_TEXT_CONTEXT* context);
} TC_PLUGIN_EXTENSION;

#define TC_PLUGIN_RUNTIME_ID L"tclock.runtime/1"
#define TC_HOST_REGISTRY_ID L"tclock.host.registry/1"
#define TC_HOST_SERVICES_ID L"tclock.host.services/1"
#define TC_COLOR_TRANSFORM 0u
#define TC_COLOR_REPLACE 1u
#define TC_TEXT_DATE 2u
#define TC_TEXT_WEEKDAY 4u
#define TC_TEXT_TIME 8u
#define TC_TEXT_ACCENT 88u

typedef ULONGLONG TC_REGISTRATION;
typedef HRESULT (WINAPI *TC_QUERY_HOST)(void* plugin, UINT abi, LPCWSTR id, const void** out);

typedef struct TC_PLUGIN_CONTEXT {
    UINT size;
    void* plugin;
    TC_QUERY_HOST query_host;
    HWND owner;
    LANGID language;
    LPCWSTR ini_path;
    LPCWSTR legacy_ini;
} TC_PLUGIN_CONTEXT;

typedef struct TC_PLUGIN_RUNTIME {
    UINT size;
    HRESULT (WINAPI *start)(const TC_PLUGIN_CONTEXT* context);
    HRESULT (WINAPI *reload)(const TC_PLUGIN_CONTEXT* context);
    void (WINAPI *stop)(void);
} TC_PLUGIN_RUNTIME;

typedef struct TC_COMMAND_REGISTRATION {
    UINT size;
    LPCWSTR id;
    LPCWSTR japanese;
    LPCWSTR english;
    void* user;
    HRESULT (WINAPI *invoke)(void* user, const TC_ACTION_CONTEXT* context);
} TC_COMMAND_REGISTRATION;

typedef struct TC_PAGE_REGISTRATION {
    UINT size;
    LPCWSTR id;
    LPCWSTR japanese;
    LPCWSTR english;
    void* user;
    HWND (WINAPI *create)(void* user, const TC_EXTENSION_CONTEXT* context);
    HRESULT (WINAPI *apply)(void* user, HWND page);
} TC_PAGE_REGISTRATION;

typedef struct TC_COLOR_REGISTRATION {
    UINT size;
    LPCWSTR id;
    INT priority;
    UINT mode;
    void* user;
    HRESULT (WINAPI *query)(void* user, const TC_TEXT_CONTEXT* context, COLORREF* color);
} TC_COLOR_REGISTRATION;

typedef struct TC_HOST_REGISTRY {
    UINT size;
    HRESULT (WINAPI *command)(void* plugin, const TC_COMMAND_REGISTRATION* entry, TC_REGISTRATION* token);
    HRESULT (WINAPI *page)(void* plugin, const TC_PAGE_REGISTRATION* entry, TC_REGISTRATION* token);
    HRESULT (WINAPI *color)(void* plugin, const TC_COLOR_REGISTRATION* entry, TC_REGISTRATION* token);
    HRESULT (WINAPI *remove)(void* plugin, TC_REGISTRATION token);
} TC_HOST_REGISTRY;

typedef struct TC_HOST_SERVICES {
    UINT size;
    HRESULT (WINAPI *copy_text)(HWND owner, LPCWSTR text);
    void (WINAPI *changed)(HWND page);
    void (WINAPI *log)(LPCWSTR text);
} TC_HOST_SERVICES;

typedef HRESULT (WINAPI *TC_GET_INTERFACE)(UINT abi, LPCWSTR id, const void** out);
#ifdef __cplusplus
}
#endif
#endif
