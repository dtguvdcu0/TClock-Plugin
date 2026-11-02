#ifndef TCLOCK_PAGE_API_H
#define TCLOCK_PAGE_API_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif

/* tclock.page/1: a plugin offers one of its registered settings pages (TC_HOST_REGISTRY.page)
   as the replacement of a built-in settings entry. The user selects it explicitly on the
   Plugins page; the host keeps the built-in page and its settings unchanged and shows it
   again when the selection is cleared or the plugin is unavailable. See page_api.md. */
#define TC_PLUGIN_PAGE_ID L"tclock.page/1"

/* Built-in settings entries that can be replaced. */
#define TC_PAGE_TARGET_CHIME L"chime"

#pragma pack(push, 8)
typedef struct TC_PAGE_SUBSTITUTE {
    UINT size;
    LPCWSTR target; /* TC_PAGE_TARGET_* */
    LPCWSTR page;   /* id of a page the same plugin registered through TC_HOST_REGISTRY.page */
} TC_PAGE_SUBSTITUTE;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif
#endif
