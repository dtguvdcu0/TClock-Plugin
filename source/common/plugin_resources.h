#ifndef TCLOCK_PLUGIN_RESOURCES_H
#define TCLOCK_PLUGIN_RESOURCES_H
/* Optional plain UTF-16 RT_STRING metadata, readable without executing a plugin.
   Keep this header RC-compatible: constants only, no Windows/C ABI declarations.
   Languages: UI locale, same-language neutral, en-US, English neutral, neutral.
   Empty entries try the next language. Missing/invalid text stays blank.
   Readers bound text to this capacity, preserving UTF-16 surrogate pairs. */
/* Automatic discovery requires an invariant (LANG_NEUTRAL) RT_STRING header:
   tclock.plugin/1:<provider-id>. ID uses1-31 lowercase ASCII letters/digits/._-.
   It must match tclock.plugin/1 runtime metadata. Other DLLs are not discovered.
   NAME is optional/localized; the file name is used when unavailable. */
#define TC_PLUGIN_HEADER_ID 61001
#define TC_PLUGIN_NAME_ID 61002
/* Optional neutral hint: "none" disables Settings without loading the DLL.
   Missing/unknown hints retain legacy settings/INI fallback behavior. */
#define TC_PLUGIN_SETTINGS_HINT_ID 61003
#define TC_PLUGIN_DESCRIPTION_ID 61000
#define TC_PLUGIN_DESCRIPTION_CHARS 1024
#endif
