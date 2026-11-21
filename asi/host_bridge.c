/* Stock longshot.exe COFF symbol bridge & C shims for host/js_*.c. */
#include "host_bridge.h"
#include "coff_sym.h"
#include "../host/js_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

StockExeSyms g_exe;
static HWND g_game_hwnd = NULL;
static int g_last_want_mouse = -1;

#ifndef WM_APP_JS_MOUSE
#define WM_APP_JS_MOUSE (WM_APP + 13)
#endif

#define OOT_GAMEDATA_SLOT 0x84c9e180ULL
#define OOT_EVENTMGR_SLOT 0x84c9e090ULL
#define OOT_EVENT_ARG_SIZE 0x1b20ULL

void asi_set_game_hwnd(HWND hwnd) { g_game_hwnd = hwnd; }
HWND asi_get_game_hwnd(void) { return g_game_hwnd; }

int asi_bridge_init(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    CoffSymReq reqs[] = {
        {"hos_oot_is_player", NULL},
        {"hos_oot_stats", NULL}, {"hos_oot_area_name", NULL},
        {"a64_fn_count", NULL}, {"a64_fn_addr", NULL},
        {"ZSTD_getFrameContentSize", NULL}, {"ZSTD_createDCtx", NULL},
        {"ZSTD_freeDCtx", NULL}, {"ZSTD_decompress_usingDict", NULL},
        {"a64_lookup", NULL},
        {"hos_guest_base", NULL},
        {"hos_guest_size", NULL},
        {"hos_mem_region", NULL},
        {"hos_sys_alloc", NULL},
        {"hos_mark_written", NULL},
        {"hos_log", NULL},
        {"hos_now_ns", NULL},
        {"hos_game_path_a", NULL},
        {"hos_text_focus", NULL},
        {"hos_text_put", NULL},
        {"hos_vk_note_key_press", NULL},
        {"hos_move_stick_held", NULL},
        {"rx_option", NULL},
        {"rx_option_set", NULL},
        {"rx_control_frame_info", NULL},
        {"rx_control_shot", NULL},
        {"rx_control_setting", NULL},
        {"rx_control_setting_set", NULL},
        {"hos_present_count", NULL},
        {"hos_romfs_dir", NULL},
        {"hos_user_dir", NULL},
        {"hos_text_scale", NULL},
        {"hos_camera_scale", NULL},
        {"hos_oot_protect_hearts", NULL},
        {"oot_speed_controller", NULL},
        {"oot_speed_percent", NULL},
        {"hos_oot_destination", NULL},
        {"hos_oot_teleport_ask", NULL},
        {"hos_oot_age_ask", NULL},
        {"oot_pouch_change", NULL},
        {"oot_item_count", NULL},
        {"oot_gamedata_bool", NULL},
        {"oot_teleport", NULL},
        {"oot_gameplay_view", NULL},
        {"oot_event_arg", NULL},
        {"oot_destinations", NULL},
        {"hos_close_finish", NULL},
        {"hos_vi_native_presented", NULL},
        {"hos_oot_lift_player", NULL},
        {"hos_oot_hearts_enabled", NULL},
        {"hos_oot_cct_speed", NULL},
        {"hos_oot_damage", NULL},
        {"hos_oot_fov", NULL},
        {"hos_oot_free_view", NULL},
        {"hos_mouse_camera", NULL},
        {"hos_oot_text_pane", NULL},
        {"hos_oot_menu_tick", NULL},
        {"hos_oot_menu_focus", NULL},
        {"hos_oot_trace_save", NULL},
        {"hos_oot_opening", NULL},
        {"npad_states", NULL},
        {"apply", NULL},
        {"host_path", NULL},
        {"rom_rel", NULL},
        {"hle__ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", NULL},
        {"hle__ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", NULL},
        {"hle__ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", NULL},
        {"hle__ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", NULL},
        {"hle__ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", NULL},
        {"hle__ZN2nn2fs9CloseFileENS0_10FileHandleE", NULL},
        {"hle__ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", NULL},
        {"wndproc", NULL},
        {"menus_mouse", NULL},
        {"load_vulkan", NULL},
    };
    const size_t total = sizeof reqs / sizeof reqs[0];
    const size_t got = coff_resolve_batch(exe, reqs, total);

    size_t i = 0;
    g_exe.is_player = reqs[i++].addr;
    g_exe.oot_stats = reqs[i++].addr;
    g_exe.area_name = reqs[i++].addr;
    g_exe.fn_count = reqs[i++].addr;
    g_exe.fn_addr = reqs[i++].addr;
    g_exe.zs_size = reqs[i++].addr;
    g_exe.zs_create = reqs[i++].addr;
    g_exe.zs_free = reqs[i++].addr;
    g_exe.zs_decode = reqs[i++].addr;
    g_exe.a64_lookup = (A64Fn (*)(uint64_t))reqs[i++].addr;
    g_exe.hos_guest_base = (uint8_t *(*)(void))reqs[i++].addr;
    g_exe.hos_guest_size = (uint64_t (*)(void))reqs[i++].addr;
    g_exe.hos_mem_region = (int (*)(uint64_t, uint64_t))reqs[i++].addr;
    g_exe.hos_sys_alloc = (uint64_t (*)(uint64_t, uint64_t))reqs[i++].addr;
    g_exe.hos_mark_written = (void (*)(const void *, size_t))reqs[i++].addr;
    g_exe.hos_log = (void (*)(const char *, ...))reqs[i++].addr;
    g_exe.hos_now_ns = (int64_t (*)(void))reqs[i++].addr;
    g_exe.hos_game_path_a = (DWORD (*)(char *, DWORD))reqs[i++].addr;
    g_exe.hos_text_focus = (int (*)(void))reqs[i++].addr;
    g_exe.hos_text_put = (void (*)(uint32_t))reqs[i++].addr;
    g_exe.hos_vk_note_key_press = (void (*)(unsigned))reqs[i++].addr;
    g_exe.hos_move_stick_held = (int (*)(void))reqs[i++].addr;
    g_exe.rx_option = (int (*)(int, int))reqs[i++].addr;
    g_exe.rx_option_set = (void (*)(int, int))reqs[i++].addr;
    g_exe.rx_control_frame_info = (int (*)(int64_t *, int64_t *, int *, int *))reqs[i++].addr;
    g_exe.rx_control_shot = (int (*)(const char *, int64_t, void (*)(void *, int64_t, int), void *))reqs[i++].addr;
    g_exe.rx_control_setting = (int (*)(int, HosControlSetting *))reqs[i++].addr;
    g_exe.rx_control_setting_set = (int (*)(const char *, int, HosControlSetting *))reqs[i++].addr;
    g_exe.p_hos_present_count = (volatile int64_t *)reqs[i++].addr;
    g_exe.p_hos_romfs_dir = (const char **)reqs[i++].addr;
    g_exe.p_hos_user_dir = (const char **)reqs[i++].addr;
    g_exe.p_hos_text_scale = (volatile float *)reqs[i++].addr;
    g_exe.p_hos_camera_scale = (volatile float *)reqs[i++].addr;
    g_exe.p_hos_oot_protect_hearts = (int *)reqs[i++].addr;
    g_exe.p_oot_speed_controller = (volatile uint64_t *)reqs[i++].addr;
    g_exe.p_oot_speed_percent = (volatile int *)reqs[i++].addr;
    g_exe.hos_oot_destination = (int (*)(int, const char **, const char **))reqs[i++].addr;
    g_exe.hos_oot_teleport_ask = (int (*)(int))reqs[i++].addr;
    g_exe.hos_oot_age_ask = (int (*)(int))reqs[i++].addr;
    g_exe.oot_pouch_change = (int (*)(Ctx *, const char *, int))reqs[i++].addr;
    g_exe.oot_item_count = (int (*)(Ctx *, const char *))reqs[i++].addr;
    g_exe.oot_gamedata_bool = (int (*)(Ctx *, uint32_t, int *))reqs[i++].addr;
    g_exe.oot_teleport = (int (*)(Ctx *, uint64_t, unsigned))reqs[i++].addr;
    g_exe.oot_gameplay_view = (uint64_t (*)(Ctx *))reqs[i++].addr;
    g_exe.oot_event_arg = (int (*)(Ctx *, uint64_t, const char *, uint32_t, uint64_t))reqs[i++].addr;
    g_exe.oot_destinations = (const OotDestinationEntry *)reqs[i++].addr;
    g_exe.fn_hos_close_finish = reqs[i++].addr;
    g_exe.fn_hos_vi_native_presented = reqs[i++].addr;
    g_exe.fn_hos_oot_lift_player = reqs[i++].addr;
    g_exe.fn_hos_oot_hearts_enabled = reqs[i++].addr;
    g_exe.fn_hos_oot_cct_speed = reqs[i++].addr;
    g_exe.fn_hos_oot_damage = reqs[i++].addr;
    g_exe.fn_hos_oot_fov = reqs[i++].addr;
    g_exe.fn_hos_oot_free_view = reqs[i++].addr;
    g_exe.fn_hos_mouse_camera = reqs[i++].addr;
    g_exe.fn_hos_oot_text_pane = reqs[i++].addr;
    g_exe.fn_hos_oot_menu_tick = reqs[i++].addr;
    g_exe.fn_hos_oot_menu_focus = reqs[i++].addr;
    g_exe.fn_hos_oot_trace_save = reqs[i++].addr;
    g_exe.fn_hos_oot_opening = reqs[i++].addr;
    g_exe.fn_npad_states = reqs[i++].addr;
    g_exe.fn_audio_apply = reqs[i++].addr;
    g_exe.fn_fs_host_path = reqs[i++].addr;
    g_exe.fn_fs_rom_rel = reqs[i++].addr;
    g_exe.fn_fs_open_file = reqs[i++].addr;
    g_exe.fn_fs_read_file_0 = reqs[i++].addr;
    g_exe.fn_fs_read_file_1 = reqs[i++].addr;
    g_exe.fn_fs_read_file_2 = reqs[i++].addr;
    g_exe.fn_fs_get_file_size = reqs[i++].addr;
    g_exe.fn_fs_close_file = reqs[i++].addr;
    g_exe.fn_fs_get_entry_type = reqs[i++].addr;
    g_exe.fn_wndproc = reqs[i++].addr;
    g_exe.fn_menus_mouse = reqs[i++].addr;
    g_exe.fn_load_vulkan = reqs[i++].addr;

    if (got != total && g_exe.hos_log) {
        for (size_t k = 0; k < total; k++) {
            if (!reqs[k].addr)
                g_exe.hos_log("longshot-js: WARNING missing exe symbol %s", reqs[k].name);
        }
    }
    return got == total;
}

/* ---- C Shims consumed by host/js_*.c ---- */
A64Fn a64_lookup(uint64_t addr) { return g_exe.a64_lookup ? g_exe.a64_lookup(addr) : NULL; }
uint8_t *hos_guest_base(void) { return g_exe.hos_guest_base ? g_exe.hos_guest_base() : NULL; }
uint64_t hos_guest_size(void) { return g_exe.hos_guest_size ? g_exe.hos_guest_size() : 0; }
int hos_mem_region(uint64_t addr, uint64_t size) { return g_exe.hos_mem_region ? g_exe.hos_mem_region(addr, size) : 0; }
uint64_t hos_sys_alloc(uint64_t size, uint64_t align) { return g_exe.hos_sys_alloc ? g_exe.hos_sys_alloc(size, align) : 0; }
uint64_t hos_guest_alloc(uint64_t size) { return hos_sys_alloc(size, 16); }
void hos_mark_written(const void *p, size_t n) { if (g_exe.hos_mark_written) g_exe.hos_mark_written(p, n); }
int64_t hos_now_ns(void) { return g_exe.hos_now_ns ? g_exe.hos_now_ns() : 0; }
unsigned long hos_game_path_a(char *out, unsigned long cap) { return g_exe.hos_game_path_a ? g_exe.hos_game_path_a(out, cap) : 0; }
int hos_text_focus(void) { return g_exe.hos_text_focus ? g_exe.hos_text_focus() : 0; }
void hos_text_put(uint32_t c) { if (g_exe.hos_text_put) g_exe.hos_text_put(c); }
int rx_option(int id, int otherwise) { return g_exe.rx_option ? g_exe.rx_option(id, otherwise) : otherwise; }
void rx_option_set(int id, int value) { if (g_exe.rx_option_set) g_exe.rx_option_set(id, value); }
int rx_control_frame_info(int64_t *p, int64_t *d, int *w, int *h) {
    return g_exe.rx_control_frame_info ? g_exe.rx_control_frame_info(p, d, w, h) : 0;
}
int rx_control_shot(const char *path, int64_t after, void (*done)(void *, int64_t, int), void *ctx) {
    return g_exe.rx_control_shot ? g_exe.rx_control_shot(path, after, done, ctx) : 0;
}
int rx_control_setting(int index, HosControlSetting *out) {
    return g_exe.rx_control_setting ? g_exe.rx_control_setting(index, out) : 0;
}
int rx_control_setting_set(const char *key, int value, HosControlSetting *out) {
    return g_exe.rx_control_setting_set ? g_exe.rx_control_setting_set(key, value, out) : 0;
}
int hos_oot_destination(int index, const char **child, const char **adult) {
    return g_exe.hos_oot_destination ? g_exe.hos_oot_destination(index, child, adult) : 0;
}
int hos_oot_teleport_ask(int index) { return g_exe.hos_oot_teleport_ask ? g_exe.hos_oot_teleport_ask(index) : 0; }
int hos_oot_age_ask(int adult) { return g_exe.hos_oot_age_ask ? g_exe.hos_oot_age_ask(adult) : 0; }
int64_t hos_js_asi_present_count(void) {
    return g_exe.p_hos_present_count ? __atomic_load_n(g_exe.p_hos_present_count, __ATOMIC_RELAXED) : 0;
}
void a64_rt_invalidate_sites(uint64_t addr) { (void)addr; }
int hos_fs_host_path(const char *guest, char *out, size_t cap) {
    typedef int (*PFN_host_path)(const char *, char *, size_t, int *);
    PFN_host_path fn = (PFN_host_path)g_exe.fn_fs_host_path;
    return fn ? fn(guest, out, cap, NULL) : 0;
}

void hos_log(const char *fmt, ...) {
    if (!g_exe.hos_log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_exe.hos_log("%s", buf);
}

void hos_display_js_want_mouse(int want) {
    const int w = want ? 1 : 0;
    if (w == g_last_want_mouse) return;
    g_last_want_mouse = w;
    if (g_game_hwnd) PostMessageA(g_game_hwnd, WM_APP_JS_MOUSE, (WPARAM)w, 0);
}

/* ---- Direct guest memory helpers (safe across >4GB addresses via hos_guest_base) ---- */
static inline int g_read(uint64_t addr, void *dst, size_t n) {
    if (!addr || !hos_mem_region(addr, (uint64_t)n)) return 0;
    uint8_t *base = hos_guest_base();
    if (!base) return 0;
    memcpy(dst, base + addr, n);
    return 1;
}
static inline int g_write(uint64_t addr, const void *src, size_t n) {
    if (!addr || !hos_mem_region(addr, (uint64_t)n)) return 0;
    uint8_t *base = hos_guest_base();
    if (!base) return 0;
    memcpy(base + addr, src, n);
    hos_mark_written(base + addr, n);
    return 1;
}
static inline uint64_t g_u64(uint64_t addr) { uint64_t v = 0; g_read(addr, &v, 8); return v; }
static inline uint32_t g_u32(uint64_t addr) { uint32_t v = 0; g_read(addr, &v, 4); return v; }
static inline uint8_t g_u8(uint64_t addr) { uint8_t v = 0; g_read(addr, &v, 1); return v; }
static inline void g_w64(uint64_t addr, uint64_t v) { g_write(addr, &v, 8); }
static inline void g_w32(uint64_t addr, uint32_t v) { g_write(addr, &v, 4); }
static inline void g_w8(uint64_t addr, uint8_t v) { g_write(addr, &v, 1); }

/* ---- Mod folder `scripts/` discovery (<game_dir>/mods/<mod>/scripts) ---- */
#define MAX_MOD_SCRIPTS 64
typedef struct {
    char name[96];
    char dir[MAX_PATH];
} ModScriptDir;
static ModScriptDir g_mod_scripts[MAX_MOD_SCRIPTS];
static int g_mod_scripts_n = -1;

static void scan_mod_scripts(void) {
    g_mod_scripts_n = 0;
    char mods_root[MAX_PATH];
    DWORD n = hos_game_path_a(mods_root, MAX_PATH);
    while (n && mods_root[n - 1] != '\\' && mods_root[n - 1] != '/') n--;
    mods_root[n] = 0;
    if (n + 8 >= MAX_PATH) return;
    strcat(mods_root, "mods");

    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", mods_root);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char dis[MAX_PATH];
        snprintf(dis, sizeof dis, "%s\\%s\\.disabled", mods_root, fd.cFileName);
        if (GetFileAttributesA(dis) != INVALID_FILE_ATTRIBUTES) continue;
        char sdir[MAX_PATH];
        snprintf(sdir, sizeof sdir, "%s\\%s\\scripts", mods_root, fd.cFileName);
        DWORD at = GetFileAttributesA(sdir);
        if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
            if (g_mod_scripts_n < MAX_MOD_SCRIPTS) {
                ModScriptDir *m = &g_mod_scripts[g_mod_scripts_n++];
                snprintf(m->name, sizeof m->name, "%s", fd.cFileName);
                snprintf(m->dir, sizeof m->dir, "%s", sdir);
            }
        }
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
}

int hos_mods_count(void) {
    if (g_mod_scripts_n < 0) scan_mod_scripts();
    return g_mod_scripts_n;
}

int hos_mod_scripts_dir(int index, char *out, size_t cap, const char **name_out) {
    if (g_mod_scripts_n < 0) scan_mod_scripts();
    if (index < 0 || index >= g_mod_scripts_n || !out || !cap) return 0;
    snprintf(out, cap, "%s", g_mod_scripts[index].dir);
    if (name_out) *name_out = g_mod_scripts[index].name;
    return 1;
}

/* ---- OoT High-Level Wrappers (mirroring host/hos_main.c for host/js_oot.c) ---- */
static uint64_t bridge_singleton(uint64_t slot) {
    const uint64_t holder = g_u64(slot);
    return holder ? g_u64(holder) : 0;
}

static int bridge_gamedata_string(Ctx *c, uint32_t hash, char *buf, size_t size) {
    const uint64_t mgr = bridge_singleton(OOT_GAMEDATA_SLOT);
    A64Fn get = a64_lookup(0x80cd0ea0ULL);
    if (size) buf[0] = 0;
    if (!c || !mgr || !get || !size) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    g_w64(out, 0);
    c->x[31] = out;
    c->x[0] = mgr; c->x[1] = out; c->x[2] = hash;
    get(c);
    const int found = (int)(c->x[0] & 1);
    const uint64_t text = g_u64(out);
    *c = saved;
    if (!found || !text) return 0;
    for (size_t i = 0; i + 1 < size; i++) {
        buf[i] = (char)g_u8(text + i);
        buf[i + 1] = 0;
        if (!buf[i]) break;
    }
    return 1;
}

static int bridge_gamedata_set_bool_direct(uint32_t hash, int value) {
    const uint64_t mgr = bridge_singleton(OOT_GAMEDATA_SLOT);
    if (!mgr) return 0;
    const uint64_t table = g_u64(mgr + 32);
    const uint32_t size = g_u32(mgr + 44);
    if (!table || !size) return 0;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1) % size) {
        const uint32_t key = g_u32(table + slot * 8ULL);
        if (!key) return 0;
        if (key != hash) continue;
        const uint32_t index = g_u32(table + slot * 8ULL + 4);
        const uint32_t count = g_u32(mgr + 16);
        if (index >= count) return 0;
        g_w8(g_u64(mgr + 24) + index * 24ULL + 22, value ? 1 : 0);
        return 1;
    }
    return 0;
}

static int bridge_send_event_request(Ctx *c, uint64_t actor, const char *event_name,
                                     const char *banc, const char *start, const char *boot) {
    if (!c || !event_name || !*event_name || !g_exe.oot_event_arg) return 0;
    const uint64_t events = bridge_singleton(OOT_EVENTMGR_SLOT);
    A64Fn make_arg = a64_lookup(0x80d99f10ULL), request = a64_lookup(0x80d9a820ULL);
    A64Fn set_link = a64_lookup(0x802dc440ULL), release_link = a64_lookup(0x8022b6f0ULL);
    if (!events || !make_arg || !request || !set_link || !release_link) return 0;
    if (g_u64(events + 104)) return 0;

    static uint64_t strings;
    if (banc) {
        const size_t bl = strlen(banc), sl = start ? strlen(start) : 0, el = boot ? strlen(boot) : 0;
        if (bl >= 128 || sl >= 128 || el >= 256) return 0;
        if (!strings) strings = hos_sys_alloc(512, 16);
        if (!strings) return 0;
        uint8_t zero[512] = {0};
        g_write(strings, zero, sizeof zero);
        g_write(strings, banc, bl + 1);
        g_write(strings + 128, start ? start : "", sl + 1);
        g_write(strings + 256, boot ? boot : "", el + 1);
    }

    const Ctx saved = *c;
    const uint64_t arg = (c->x[31] - OOT_EVENT_ARG_SIZE - 0x100) & ~0xfULL;
    c->x[31] = arg;
    c->x[0] = arg;
    make_arg(c);
    int ok = 1;
    {
        const uint64_t buf = g_u64(arg + 6800);
        const uint32_t cap = g_u32(arg + 6808);
        const size_t len = strlen(event_name) + 1;
        if (!buf || len > cap || !g_write(buf, event_name, len)) ok = 0;
    }
    if (ok && banc) {
        ok = g_exe.oot_event_arg(c, arg, "NextBanc", 1, strings) &&
             g_exe.oot_event_arg(c, arg, "NextStartPos", 1, strings + 128) &&
             g_exe.oot_event_arg(c, arg, "BootEventName", 1, strings + 256) &&
             g_exe.oot_event_arg(c, arg, "IsCheckAir", 4, 0);
    }
    if (ok && actor) {
        c->x[31] = arg;
        c->x[0] = arg + 0x1950; c->x[1] = actor; c->x[2] = 0;
        set_link(c);
    }
    uint64_t result = 0;
    if (ok) {
        c->x[31] = arg;
        c->x[0] = events; c->x[1] = arg;
        request(c);
        result = c->x[0];
    }
    for (unsigned i = 0; i < 2; i++) {
        const uint64_t link = arg + (i ? 0x1950 : 0x1968);
        g_w64(link, 0x84994b20ULL);
        c->x[31] = arg;
        c->x[0] = link;
        release_link(c);
    }
    *c = saved;
    return ok && result != 0;
}

int hos_oot_js_pouch_count(struct GuestContext *c, const char *name) {
    if (!c || !name || strlen(name) >= 0x60) return -1;
    const uint64_t manager = g_u64(0x84e98e28ULL);
    A64Fn count = a64_lookup(0x805b63e0ULL);
    if (!manager || !count) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    g_write(frame + 0x10, name, strlen(name) + 1);
    g_w64(frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager; c->x[1] = frame; c->x[2] = 0; c->x[3] = 0;
    count(c);
    const int n = (int)(int32_t)c->x[0];
    *c = saved;
    return n;
}

int hos_oot_js_pouch_change(struct GuestContext *c, const char *name, int delta) {
    return (c && name && g_exe.oot_pouch_change) ? g_exe.oot_pouch_change(c, name, delta) : -1;
}

int hos_oot_js_item_count(struct GuestContext *c, const char *name) {
    return (c && name && g_exe.oot_item_count) ? g_exe.oot_item_count(c, name) : -1;
}

int hos_oot_js_gamedata_set_bool(struct GuestContext *c, uint32_t hash, int value) {
    if (!c) return 0;
    const int v = value ? 1 : 0;
    A64Fn set_bool = a64_lookup(0x806a2b20ULL);
    const uint64_t mgr = bridge_singleton(OOT_GAMEDATA_SLOT);
    if (set_bool && mgr) {
        const Ctx saved = *c;
        c->x[0] = mgr; c->x[1] = (uint64_t)v; c->x[2] = hash;
        set_bool(c);
        *c = saved;
    }
    return bridge_gamedata_set_bool_direct(hash, v);
}

void hos_oot_js_set_time_seconds(struct GuestContext *c, uint32_t seconds) {
    if (!c) return;
    const uint64_t singleton = g_u64(0x84c9e898ULL);
    const uint64_t world = singleton ? g_u64(singleton) : 0;
    A64Fn set_time = a64_lookup(0x834722d0ULL);
    if (!set_time || !world || !g_u64(world + 0x510)) return;
    const Ctx saved = *c;
    c->x[0] = world;
    c->x[1] = seconds % 86400U;
    set_time(c);
    *c = saved;
}

uint64_t hos_oot_js_player_controller(struct GuestContext *c, uint64_t components) {
    (void)c;
    if (!components) return 0;
    static const unsigned offs[] = {80, 32, 16, 8};
    uint64_t ctrl = components;
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        ctrl = g_u64(ctrl + offs[i]);
        if (!ctrl) return 0;
    }
    return g_u64(ctrl) == 0x84c54f10ULL ? ctrl : 0;
}

int hos_oot_js_set_player_pos(struct GuestContext *c, uint64_t actor, uint64_t components, float x, float y, float z) {
    if (!c) return 0;
    if (!components && actor) components = g_u64(actor + 624);
    const uint64_t ctrl = components ? hos_oot_js_player_controller(c, components) : 0;
    if (!ctrl || g_u64(0x84c54f10ULL + 0xe8) != 0x806d2530ULL) return 0;
    A64Fn change_position = a64_lookup(0x806d2530ULL);
    if (!change_position) return 0;

    const float pos[3] = {x, y, z};
    const Ctx saved = *c;
    const uint64_t arg = (c->x[31] - 16) & ~0xfULL;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t bits = 0;
        memcpy(&bits, &pos[i], 4);
        g_w32(arg + i * 4, bits);
    }
    g_w32(arg + 12, 0);
    c->x[31] = arg; c->x[0] = ctrl; c->x[1] = arg;
    change_position(c);
    *c = saved;
    if (actor) g_write(actor + 796, pos, sizeof pos);
    return 1;
}

int hos_oot_js_teleport_index(struct GuestContext *c, uint64_t actor, unsigned index) {
    return (c && actor && g_exe.oot_teleport) ? g_exe.oot_teleport(c, actor, index) : 0;
}

int hos_oot_js_teleport_custom(struct GuestContext *c, uint64_t actor, const char *banc,
                               const char *start_pos, const char *boot_event) {
    if (!c || !banc || !*banc || !g_exe.oot_gamedata_bool) return 0;
    int loading = 0;
    if (g_exe.oot_gamedata_bool(c, 0xef2b60c2u, &loading) && loading) return 0;
    const int sent = bridge_send_event_request(c, actor, "Dm_SY_BancChangeGo",
                                               banc, start_pos ? start_pos : "", boot_event ? boot_event : "");
    if (sent) {
        int hidden = 0;
        if (g_exe.oot_gamedata_bool(c, 0x9a4dd779u, &hidden) && hidden)
            hos_oot_js_gamedata_set_bool(c, 0x9a4dd779u, 0);
    }
    return sent;
}

int hos_oot_js_trigger_event(struct GuestContext *c, const char *event_name, uint64_t actor) {
    return bridge_send_event_request(c, actor, event_name, NULL, NULL, NULL);
}

int hos_oot_js_toggle_age(struct GuestContext *c, uint64_t actor, int target_adult) {
    if (!c || !actor || !g_exe.oot_gamedata_bool || !g_exe.oot_teleport) return 0;
    int child = 1;
    if (!g_exe.oot_gamedata_bool(c, 0xcea220d3u, &child)) return 0;
    if (target_adult >= 0 && (!child == (target_adult != 0))) return 1;

    char current[80];
    bridge_gamedata_string(c, 0x1d6189dau, current, sizeof current);

    unsigned target = 3;
    if (g_exe.oot_destinations) {
        const char *dc = NULL, *da = NULL;
        for (int i = 0; hos_oot_destination(i, &dc, &da); i++) {
            const OotDestinationEntry *d = &g_exe.oot_destinations[i];
            const char *other = child ? d->adult_banc : d->child_banc;
            if (other && ((d->child_banc && !strcmp(d->child_banc, current)) ||
                          (d->adult_banc && !strcmp(d->adult_banc, current)))) {
                target = (unsigned)i;
                break;
            }
        }
    }
    if (!bridge_gamedata_set_bool_direct(0xcea220d3u, !child)) return 0;
    if (!g_exe.oot_teleport(c, actor, target)) {
        bridge_gamedata_set_bool_direct(0xcea220d3u, child);
        return 0;
    }
    return 1;
}

uint64_t hos_oot_js_gameplay_view(struct GuestContext *c) {
    return (c && g_exe.oot_gameplay_view) ? g_exe.oot_gameplay_view(c) : 0;
}

const uint32_t *js_lifted_functions(uint32_t *total) {
    *total = g_exe.fn_count ? *g_exe.fn_count : 0;
    return g_exe.fn_addr;
}

uint64_t js_zstd_size(const void *src, size_t size) {
    return g_exe.zs_size ? g_exe.zs_size(src, size) : UINT64_MAX;
}

int js_zstd_decode(void *dst, size_t cap, const void *src, size_t size, const void *dict, size_t dict_size) {
    if (!g_exe.zs_create || !g_exe.zs_free || !g_exe.zs_decode) return 0;
    void *dctx = g_exe.zs_create();
    if (!dctx) return 0;
    const size_t got = g_exe.zs_decode(dctx, dst, cap, src, size, dict, dict_size);
    g_exe.zs_free(dctx);
    return got == cap;
}

int js_player_stats(int values[5]) {
    return g_exe.oot_stats && g_exe.oot_stats(values, values+1, values+2, values+3, values+4);
}
int js_area_name(const char *map, char *out, size_t cap) {
    return g_exe.area_name && g_exe.area_name(map, out, cap);
}
