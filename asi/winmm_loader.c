/* Universal winmm.dll proxy ASI loader for stock Longshot (outside DllMain loader lock). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "winmm_exports.inc"

void *g_winmm_procs[WINMM_STUB_COUNT];

typedef MMRESULT (WINAPI *PFN_timeBeginPeriod)(UINT);
static HMODULE g_real_winmm = NULL;
static PFN_timeBeginPeriod g_real_timeBeginPeriod = NULL;
static INIT_ONCE g_asi_once = INIT_ONCE_STATIC_INIT;

static int cmp_wstr(const void *a, const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

static void load_asi_dir(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    if (_snwprintf(pat, MAX_PATH, L"%ls\\*.asi", dir) < 0) return;

    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;

    wchar_t *names[64];
    size_t count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (count < 64) {
            size_t len = wcslen(fd.cFileName) + 1;
            names[count] = (wchar_t *)malloc(len * sizeof(wchar_t));
            if (names[count]) memcpy(names[count], fd.cFileName, len * sizeof(wchar_t)), count++;
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);

    if (count > 1) qsort(names, count, sizeof(names[0]), cmp_wstr);

    for (size_t i = 0; i < count; i++) {
        wchar_t full[MAX_PATH];
        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, names[i]) > 0) {
            HMODULE mod = LoadLibraryW(full);
            char logbuf[MAX_PATH + 64];
            if (mod) {
                typedef void (*PFN_InitializeASI)(void);
                PFN_InitializeASI init_fn = (PFN_InitializeASI)(void *)GetProcAddress(mod, "InitializeASI");
                if (init_fn) init_fn();
                _snprintf(logbuf, sizeof logbuf, "longshot-js winmm: loaded %ls\n", full);
            } else {
                _snprintf(logbuf, sizeof logbuf, "longshot-js winmm: FAILED (%lu) %ls\n",
                          (unsigned long)GetLastError(), full);
            }
            OutputDebugStringA(logbuf);
        }
        free(names[i]);
    }
}

static BOOL CALLBACK load_asi_plugins(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    char dis[8] = "";
    if (GetEnvironmentVariableA("LONGSHOT_JS_DISABLE", dis, sizeof dis) > 0 && !strcmp(dis, "1"))
        return TRUE;

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash) slash = wcsrchr(exe, L'/');
    if (!slash) return TRUE;
    if (_wcsicmp(slash + 1, L"longshot.exe") != 0) return TRUE;

    *slash = 0;
    wchar_t pdir[MAX_PATH];
    if (_snwprintf(pdir, MAX_PATH, L"%ls\\plugins", exe) > 0) load_asi_dir(pdir);

    wchar_t *parent = wcsrchr(exe, L'\\');
    if (!parent) parent = wcsrchr(exe, L'/');
    if (parent) {
        *parent = 0;
        if (_snwprintf(pdir, MAX_PATH, L"%ls\\plugins", exe) > 0) load_asi_dir(pdir);
    }
    return TRUE;
}

MMRESULT WINAPI proxy_timeBeginPeriod(UINT uPeriod) {
    InitOnceExecuteOnce(&g_asi_once, load_asi_plugins, NULL, NULL);
    return g_real_timeBeginPeriod ? g_real_timeBeginPeriod(uPeriod) : TIMERR_NOERROR;
}

static int resolve_real_winmm(void) {
    wchar_t sysdir[MAX_PATH], path[MAX_PATH];
    UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
    if (!n || n + 16 >= MAX_PATH) return 0;
    _snwprintf(path, MAX_PATH, L"%ls\\winmm.dll", sysdir);
    g_real_winmm = LoadLibraryW(path);
    if (!g_real_winmm) return 0;

    g_real_timeBeginPeriod = (PFN_timeBeginPeriod)(void *)GetProcAddress(g_real_winmm, "timeBeginPeriod");
    for (int i = 0; i < WINMM_STUB_COUNT; i++) {
        const char *spec = g_winmm_exports[i].name
                         ? g_winmm_exports[i].name
                         : MAKEINTRESOURCEA(g_winmm_exports[i].ordinal);
        g_winmm_procs[i] = (void *)GetProcAddress(g_real_winmm, spec);
    }
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        if (!resolve_real_winmm()) return FALSE;
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!reserved && g_real_winmm) {
            FreeLibrary(g_real_winmm);
            g_real_winmm = NULL;
        }
    }
    return TRUE;
}
