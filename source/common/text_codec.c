#include "text_codec.h"

UINT tc_current_ansi_codepage(void)
{
    return GetACP();
}

static int tc_len_no_nul_w(const wchar_t* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int tc_len_no_nul_a(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int tc_utf8_to_utf16(const char* utf8, wchar_t* outWide, int outWideCch)
{
    int n;
    if (!utf8 || !outWide || outWideCch <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, outWide, outWideCch);
    if (n <= 0) {
        outWide[0] = L'\0';
        return 0;
    }
    outWide[outWideCch - 1] = L'\0';
    return tc_len_no_nul_w(outWide);
}

int tc_utf16_to_utf8(const wchar_t* wide, char* outUtf8, int outUtf8Bytes)
{
    int n;
    if (!wide || !outUtf8 || outUtf8Bytes <= 0) return 0;
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, outUtf8, outUtf8Bytes, NULL, NULL);
    if (n <= 0) {
        outUtf8[0] = '\0';
        return 0;
    }
    outUtf8[outUtf8Bytes - 1] = '\0';
    return tc_len_no_nul_a(outUtf8);
}

int tc_ansi_to_utf16(UINT codepage, const char* ansi, wchar_t* outWide, int outWideCch)
{
    int n;
    if (!codepage) codepage = tc_current_ansi_codepage();
    if (!ansi || !outWide || outWideCch <= 0) return 0;
    /* Compatibility boundary: ansi->wide conversion helper intentionally preserves configurable codepage contract. */
    n = MultiByteToWideChar(codepage, 0, ansi, -1, outWide, outWideCch);
    if (n <= 0) {
        outWide[0] = L'\0';
        return 0;
    }
    outWide[outWideCch - 1] = L'\0';
    return tc_len_no_nul_w(outWide);
}

/* Compatibility boundary: ANSI egress helper is intentionally kept for legacy char-based callers. */
int tc_utf16_to_ansi(UINT codepage, const wchar_t* wide, char* outAnsi, int outAnsiBytes)
{
    int n;
    if (!codepage) codepage = tc_current_ansi_codepage();
    if (!wide || !outAnsi || outAnsiBytes <= 0) return 0;
    /* Compatibility boundary: wide->ansi conversion helper intentionally preserves configurable codepage contract. */
    n = WideCharToMultiByte(codepage, 0, wide, -1, outAnsi, outAnsiBytes, NULL, NULL);
    if (n <= 0) {
        outAnsi[0] = '\0';
        return 0;
    }
    outAnsi[outAnsiBytes - 1] = '\0';
    return tc_len_no_nul_a(outAnsi);
}

int tc_ansi_to_utf16_compat(UINT preferredCodePage, const char* ansi, wchar_t* outWide, int outWideCch)
{
    int n;
    DWORD flags;
    UINT cp = preferredCodePage;
    UINT fallbackCp = tc_current_ansi_codepage();

    if (!cp) cp = fallbackCp;
    if (!ansi || !outWide || outWideCch <= 0) return 0;

    flags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
    /* Compatibility boundary: compat parser attempts preferred codepage first by design. */
    n = MultiByteToWideChar(cp, flags, ansi, -1, outWide, outWideCch);
    if (n <= 0 && flags != 0) {
        /* Compatibility boundary: retry without strict flag keeps legacy input tolerance. */
        n = MultiByteToWideChar(cp, 0, ansi, -1, outWide, outWideCch);
    }
    if (n <= 0 && cp != fallbackCp) {
        /* Compatibility boundary: fallback ACP decode retained for legacy text ingress. */
        n = MultiByteToWideChar(fallbackCp, 0, ansi, -1, outWide, outWideCch);
    }
    if (n <= 0) {
        outWide[0] = L'\0';
        return 0;
    }
    outWide[outWideCch - 1] = L'\0';
    return tc_len_no_nul_w(outWide);
}

/* Compatibility boundary: ANSI egress helper with preferred/fallback codepage is kept for legacy callers. */
int tc_utf16_to_ansi_compat(UINT preferredCodePage, const wchar_t* wide, char* outAnsi, int outAnsiBytes)
{
    int n;
    UINT cp = preferredCodePage;
    UINT fallbackCp = tc_current_ansi_codepage();

    if (!cp) cp = fallbackCp;
    if (!wide || !outAnsi || outAnsiBytes <= 0) return 0;

    /* Compatibility boundary: compat encoder keeps preferred codepage output contract. */
    n = WideCharToMultiByte(cp, 0, wide, -1, outAnsi, outAnsiBytes, NULL, NULL);
    if (n <= 0 && cp != fallbackCp) {
        /* Compatibility boundary: fallback ACP encode retained for legacy consumers. */
        n = WideCharToMultiByte(fallbackCp, 0, wide, -1, outAnsi, outAnsiBytes, NULL, NULL);
    }
    if (n <= 0) {
        outAnsi[0] = '\0';
        return 0;
    }
    outAnsi[outAnsiBytes - 1] = '\0';
    return tc_len_no_nul_a(outAnsi);
}

int tc_ansi_bytes_to_utf16_compat(UINT preferredCodePage, const char* ansi, int ansiBytes, wchar_t* outWide, int outWideCch)
{
    int n;
    DWORD flags;
    UINT cp = preferredCodePage;
    UINT fallbackCp = tc_current_ansi_codepage();
    int srcBytes = ansiBytes;

    if (!cp) cp = fallbackCp;
    if (!ansi || !outWide || outWideCch <= 1) return 0;
    if (srcBytes < 0) srcBytes = tc_len_no_nul_a(ansi);
    if (srcBytes <= 0) {
        outWide[0] = L'\0';
        return 0;
    }

    flags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
    /* Compatibility boundary: byte-length decoder preserves legacy format-parser behavior. */
    n = MultiByteToWideChar(cp, flags, ansi, srcBytes, outWide, outWideCch - 1);
    if (n <= 0 && flags != 0) {
        /* Compatibility boundary: non-strict retry keeps historical parser tolerance. */
        n = MultiByteToWideChar(cp, 0, ansi, srcBytes, outWide, outWideCch - 1);
    }
    if (n <= 0 && cp != fallbackCp) {
        /* Compatibility boundary: ACP fallback retained for legacy byte streams. */
        n = MultiByteToWideChar(fallbackCp, 0, ansi, srcBytes, outWide, outWideCch - 1);
    }
    if (n <= 0) {
        outWide[0] = L'\0';
        return 0;
    }
    outWide[n] = L'\0';
    return n;
}

BOOL tc_path_utf8_or_ansi_to_utf16(const char* path, wchar_t* outWide, int outWideCch)
{
    if (!path || !outWide || outWideCch <= 0) return FALSE;
    outWide[0] = L'\0';
    if (tc_utf8_to_utf16(path, outWide, outWideCch) > 0) return TRUE;
    if (tc_ansi_to_utf16_compat(0, path, outWide, outWideCch) > 0) return TRUE;
    outWide[0] = L'\0';
    return FALSE;
}

HANDLE tc_find_first_file_utf8_compat(const char* path, WIN32_FIND_DATAW* findData)
{
    WIN32_FIND_DATAW dummy;
    wchar_t wPath[MAX_PATH];
    if (!findData) findData = &dummy;
    if (!tc_path_utf8_or_ansi_to_utf16(path, wPath, (int)(sizeof(wPath) / sizeof(wPath[0])))) {
        return INVALID_HANDLE_VALUE;
    }
    return FindFirstFileW(wPath, findData);
}

BOOL tc_text_codec_selfcheck(void)
{
    const char* s = "codec-selfcheck";
    wchar_t wbuf[64];
    char abuf[64];
    if (tc_utf8_to_utf16(s, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0]))) <= 0) return FALSE;
    if (tc_utf16_to_utf8(wbuf, abuf, (int)sizeof(abuf)) <= 0) return FALSE;
    return (lstrcmp(s, abuf) == 0) ? TRUE : FALSE;
}
