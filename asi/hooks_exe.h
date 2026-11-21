/* MinHook detours on untouched stock longshot.exe. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Creates and enables all hooks on stock longshot.exe (host_bridge must be initialized first). */
int asi_hooks_exe_install(void);

#ifdef __cplusplus
}
#endif
