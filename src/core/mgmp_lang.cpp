#include "mgmp_lang.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

constexpr ULONGLONG kRereadMs = 5000;

char      g_code[16] = "";
ULONGLONG g_at       = 0;

// "current_language xx" in one settings.txt, into `out`. False if not there.
bool read_settings(const wchar_t* path, char* out, size_t cap) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        const char* key = "current_language";
        if (strncmp(line, key, strlen(key)) != 0) continue;
        const char* p = line + strlen(key);
        while (*p == ' ' || *p == '\t') ++p;
        size_t n = 0;
        while (p[n] && p[n] != '\r' && p[n] != '\n' && p[n] != ' ' && n + 1 < cap) ++n;
        if (n) { memcpy(out, p, n); out[n] = 0; found = true; }
        break;
    }
    fclose(f);
    return found;
}

// The settings.txt of the most recently used Steam account folder.
bool from_game(char* out, size_t cap) {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return false;
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\Glaiel Games\\Mewgenics\\*", appdata);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME best = {};
    bool any = false;
    char got[16] = "";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Glaiel Games\\Mewgenics\\%s\\settings.txt",
                     appdata, fd.cFileName);
        WIN32_FILE_ATTRIBUTE_DATA at;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &at)) continue;
        if (any && CompareFileTime(&at.ftLastWriteTime, &best) <= 0) continue;
        char code[16];
        if (!read_settings(path, code, sizeof(code))) continue;
        best = at.ftLastWriteTime;
        strcpy_s(got, code);
        any = true;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (any) strcpy_s(out, cap, got);
    return any;
}

} // namespace

const char* lang_code() {
    const ULONGLONG now = GetTickCount64();
    if (!g_code[0] || now - g_at > kRereadMs) {
        g_at = now;
        char code[16] = "";
        if (!from_game(code, sizeof(code)))
            strcpy_s(code, PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? "ru" : "en");
        strcpy_s(g_code, code);
    }
    return g_code;
}

bool lang_ru() { return strcmp(lang_code(), "ru") == 0; }

} // namespace mgmp
