/* Entrypoint for plugins/longshot_js.asi (loaded by winmm.dll outside loader lock). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "host_bridge.h"
#include "hooks_exe.h"
#include "js_pipe.h"
#include "../host/js_api.h"
#include "../third_party/minhook/MinHook.h"

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK plugin_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    if (MH_Initialize() != MH_OK) {
        OutputDebugStringA("longshot-js: MH_Initialize failed\n");
        return TRUE;
    }
    const int bridge_ok = asi_bridge_init();
    const int hooks_n = asi_hooks_exe_install();
    hos_js_init();
    js_pipe_start();
    if (g_exe.hos_log) {
        g_exe.hos_log("longshot-js: ASI runtime active on stock v3.0.0 (bridge=%s, hooks=%d)",
                      bridge_ok ? "complete" : "partial", hooks_n);
    }
    return TRUE;
}

__declspec(dllexport) void InitializeASI(void) {
    InitOnceExecuteOnce(&g_init_once, plugin_init_once, NULL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
