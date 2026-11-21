/* Dear ImGui overlay, HUD primitives, windows & F9 console hooks for stock longshot_vk.dll. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Attaches MinHook detours and COFF symbol bindings to loaded stock longshot_vk.dll. */
void asi_hooks_vk_attach(void *vk_hmodule);

#ifdef __cplusplus
}
#endif
