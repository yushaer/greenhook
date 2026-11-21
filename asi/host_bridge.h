/* Stock longshot.exe COFF symbol bridge & C shims for longshot_js.asi. */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "../host/hos.h"
#include "../host/control.h"
#include "../gpu/vk/vk_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *child_banc, *child_start, *child_boot;
    const char *adult_banc, *adult_start, *adult_boot;
} OotDestinationEntry;

typedef struct StockExeSyms {
    int (*is_player)(Ctx *, uint64_t);
    int (*oot_stats)(int *, int *, int *, int *, int *);
    int (*area_name)(const char *, char *, size_t);
    const uint32_t *fn_count, *fn_addr;
    unsigned long long (*zs_size)(const void *, size_t);
    void *(*zs_create)(void);
    size_t (*zs_free)(void *);
    size_t (*zs_decode)(void *, void *, size_t, const void *, size_t, const void *, size_t);
    /* Core runtime & host functions */
    A64Fn (*a64_lookup)(uint64_t addr);
    uint8_t *(*hos_guest_base)(void);
    uint64_t (*hos_guest_size)(void);
    int (*hos_mem_region)(uint64_t addr, uint64_t size);
    uint64_t (*hos_sys_alloc)(uint64_t size, uint64_t align);
    void (*hos_mark_written)(const void *p, size_t n);
    void (*hos_log)(const char *fmt, ...);
    int64_t (*hos_now_ns)(void);
    DWORD (*hos_game_path_a)(char *out, DWORD cap);
    int (*hos_text_focus)(void);
    void (*hos_text_put)(uint32_t c);
    void (*hos_vk_note_key_press)(unsigned key);
    int (*hos_move_stick_held)(void);
    int (*rx_option)(int id, int otherwise);
    void (*rx_option_set)(int id, int value);
    int (*rx_control_frame_info)(int64_t *presents, int64_t *draws, int *width, int *height);
    int (*rx_control_shot)(const char *path, int64_t after, void (*done)(void *, int64_t, int), void *ctx);
    int (*rx_control_setting)(int index, HosControlSetting *out);
    int (*rx_control_setting_set)(const char *key, int value, HosControlSetting *out);

    /* Globals */
    volatile int64_t *p_hos_present_count;
    const char **p_hos_romfs_dir;
    const char **p_hos_user_dir;
    volatile float *p_hos_text_scale;
    volatile float *p_hos_camera_scale;
    int *p_hos_oot_protect_hearts;
    volatile uint64_t *p_oot_speed_controller;
    volatile int *p_oot_speed_percent;

    /* OoT helpers */
    int (*hos_oot_destination)(int index, const char **child, const char **adult);
    int (*hos_oot_teleport_ask)(int index);
    int (*hos_oot_age_ask)(int adult);
    int (*oot_pouch_change)(Ctx *c, const char *name, int delta);
    int (*oot_item_count)(Ctx *c, const char *name);
    int (*oot_gamedata_bool)(Ctx *c, uint32_t hash, int *value);
    int (*oot_teleport)(Ctx *c, uint64_t actor, unsigned index);
    uint64_t (*oot_gameplay_view)(Ctx *c);
    int (*oot_event_arg)(Ctx *c, uint64_t arg, const char *name, uint32_t type, uint64_t value);
    const OotDestinationEntry *oot_destinations;

    /* Hook targets */
    void *fn_hos_close_finish;
    void *fn_hos_vi_native_presented;
    void *fn_hos_oot_lift_player;
    void *fn_hos_oot_hearts_enabled;
    void *fn_hos_oot_cct_speed;
    void *fn_hos_oot_damage;
    void *fn_hos_oot_fov;
    void *fn_hos_oot_free_view;
    void *fn_hos_mouse_camera;
    void *fn_hos_oot_text_pane;
    void *fn_hos_oot_menu_tick;
    void *fn_hos_oot_menu_focus;
    void *fn_hos_oot_trace_save;
    void *fn_hos_oot_opening;
    void *fn_npad_states;
    void *fn_audio_apply;
    void *fn_fs_host_path;
    void *fn_fs_rom_rel;
    void *fn_fs_open_file;
    void *fn_fs_read_file_0;
    void *fn_fs_read_file_1;
    void *fn_fs_read_file_2;
    void *fn_fs_get_file_size;
    void *fn_fs_close_file;
    void *fn_fs_get_entry_type;
    void *fn_wndproc;
    void *fn_menus_mouse;
    void *fn_load_vulkan;
} StockExeSyms;

extern StockExeSyms g_exe;

/* Resolves all symbols from the host longshot.exe module. Returns 1 if all required symbols succeeded. */
int asi_bridge_init(void);

/* Window handle captured by wndproc hook (used for WM_APP+13 mouse release). */
void asi_set_game_hwnd(HWND hwnd);
HWND asi_get_game_hwnd(void);

#ifdef __cplusplus
}
#endif
