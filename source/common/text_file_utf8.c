#include "text_file_utf8.h"
#include "text_codec.h"

static BOOL tc_is_valid_utf8_bytes(const unsigned char* data, DWORD len)
{
    int n;
    if (!data || len == 0) return TRUE;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)data, (int)len, NULL, 0);
    return (n > 0) ? TRUE : FALSE;
}

BOOL tc_read_text_file_utf8(const char* path, char** outText, DWORD* outSize, BOOL* outHadBom)
{
    HANDLE h;
    DWORD sz;
    DWORD rd = 0;
    unsigned char* raw;
    DWORD start = 0;

    if (!path || !outText || !outSize) return FALSE;
    *outText = NULL;
    *outSize = 0;
    if (outHadBom) *outHadBom = FALSE;

    {
        wchar_t wPath[MAX_PATH];
        if (tc_utf8_to_utf16(path, wPath, (int)(sizeof(wPath) / sizeof(wPath[0]))) <= 0) {
            if (tc_ansi_to_utf16_compat(0, path, wPath, (int)(sizeof(wPath) / sizeof(wPath[0]))) <= 0) return FALSE;
        }
        h = CreateFileW(wPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    sz = GetFileSize(h, NULL);
    raw = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz + 1);
    if (!raw) {
        CloseHandle(h);
        return FALSE;
    }

    if (sz > 0) {
        if (!ReadFile(h, raw, sz, &rd, NULL)) {
            HeapFree(GetProcessHeap(), 0, raw);
            CloseHandle(h);
            return FALSE;
        }
    }
    CloseHandle(h);
    raw[rd] = '\0';

    if (rd >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        start = 3;
        if (outHadBom) *outHadBom = TRUE;
    }

    if (!tc_is_valid_utf8_bytes(raw + start, rd - start)) {
        HeapFree(GetProcessHeap(), 0, raw);
        return FALSE;
    }

    if (start > 0) {
        DWORD i;
        for (i = 0; i < rd - start; i++) {
            raw[i] = raw[start + i];
        }
        rd -= start;
        raw[rd] = '\0';
    }

    *outText = (char*)raw;
    *outSize = rd;
    return TRUE;
}

BOOL tc_write_text_file_utf8(const char* path, const char* text, DWORD size, BOOL writeBom)
{
    HANDLE h;
    DWORD wr;
    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};

    if (!path) return FALSE;
    if (!text && size > 0) return FALSE;

    {
        wchar_t wPath[MAX_PATH];
        if (tc_utf8_to_utf16(path, wPath, (int)(sizeof(wPath) / sizeof(wPath[0]))) <= 0) {
            if (tc_ansi_to_utf16_compat(0, path, wPath, (int)(sizeof(wPath) / sizeof(wPath[0]))) <= 0) return FALSE;
        }
        h = CreateFileW(wPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    if (writeBom) {
        if (!WriteFile(h, bom, 3, &wr, NULL) || wr != 3) {
            CloseHandle(h);
            return FALSE;
        }
    }

    if (size > 0) {
        if (!WriteFile(h, text, size, &wr, NULL) || wr != size) {
            CloseHandle(h);
            return FALSE;
        }
    }

    CloseHandle(h);
    return TRUE;
}

void tc_free_text_buffer(void* p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

BOOL tc_text_file_utf8_selfcheck(void)
{
    char tmpPath[MAX_PATH];
    char tmpFile[MAX_PATH];
    char* outText = NULL;
    DWORD outSize = 0;
    BOOL hadBom = FALSE;
    const char* sample = "text-file-selfcheck";
    BOOL ok = FALSE;

    if (!GetTempPath((DWORD)sizeof(tmpPath), tmpPath)) return FALSE;
    if (!GetTempFileName(tmpPath, "tcu", 0, tmpFile)) return FALSE;

    if (!tc_write_text_file_utf8(tmpFile, sample, (DWORD)lstrlen(sample), TRUE)) goto cleanup;
    if (!tc_read_text_file_utf8(tmpFile, &outText, &outSize, &hadBom)) goto cleanup;
    if (!hadBom) goto cleanup;
    if (outSize != (DWORD)lstrlen(sample)) goto cleanup;
    if (lstrcmp(outText, sample) != 0) goto cleanup;
    ok = TRUE;

cleanup:
    if (outText) tc_free_text_buffer(outText);
    {
        wchar_t wTmpFile[MAX_PATH];
        /* Migrate cleanup to W API; ignore cleanup failure in selfcheck path. */
        if (tc_utf8_to_utf16(tmpFile, wTmpFile, (int)(sizeof(wTmpFile) / sizeof(wTmpFile[0]))) > 0 ||
            tc_ansi_to_utf16_compat(0, tmpFile, wTmpFile, (int)(sizeof(wTmpFile) / sizeof(wTmpFile[0]))) > 0) {
            DeleteFileW(wTmpFile);
        }
    }
    return ok;
}
