/* MinHook detours on untouched stock longshot.exe. */
#include "hooks_exe.h"
#include "hooks_vk.h"
#include "host_bridge.h"
#include "js_pipe.h"
#include "../host/js_api.h"
#include "../host/js_internal.h"
#include "../third_party/minhook/MinHook.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_APP_JS_MOUSE
#define WM_APP_JS_MOUSE (WM_APP + 13)
#endif
#define FS_PATH_MAX 1024
#define R_PATH_NOT_FOUND 0x202u
#define R_OUT_OF_MEMORY 0x5EE02u
#define STATE_SIZE 0x28u
#define NPAD_NO1 0u

/* ---- Original trampolines ---- */
static A64Fn (*g_orig_a64_lookup)(uint64_t);
static void (*g_orig_hos_close_finish)(void);
static void (*g_orig_hos_vi_native_presented)(const int64_t phases[4]);
static void (*g_orig_hos_oot_lift_player)(Ctx *, uint64_t);
static int (*g_orig_hos_oot_hearts_enabled)(void);
static void (*g_orig_hos_oot_cct_speed)(Ctx *, uint64_t, int);
static uint32_t (*g_orig_hos_oot_damage)(uint32_t, uint32_t, uint32_t);
static void (*g_orig_hos_oot_fov)(Ctx *, uint64_t);
static void (*g_orig_hos_oot_free_view)(Ctx *, uint64_t);
static void (*g_orig_hos_mouse_camera)(uint64_t);
static void (*g_orig_hos_oot_text_pane)(Ctx *, uint64_t);
static void (*g_orig_hos_oot_menu_tick)(Ctx *, uint64_t);
static void (*g_orig_hos_oot_menu_focus)(Ctx *, uint64_t, uint64_t, uint64_t);
static int (*g_orig_hos_oot_trace_save)(Ctx *);
static int (*g_orig_hos_oot_opening)(Ctx *, int);
static int (*g_orig_rx_option)(int, int);
static void (*g_orig_npad_states)(Ctx *);
static void (*g_orig_audio_apply)(const void *);
static int (*g_orig_fs_host_path)(const char *, char *, size_t, int *);
static const char *(*g_orig_fs_rom_rel)(const char *);
static void (*g_orig_fs_open_file)(Ctx *);
static void (*g_orig_fs_read_0)(Ctx *);
static void (*g_orig_fs_read_1)(Ctx *);
static void (*g_orig_fs_read_2)(Ctx *);
static void (*g_orig_fs_get_file_size)(Ctx *);
static void (*g_orig_fs_close_file)(Ctx *);
static void (*g_orig_fs_get_entry_type)(Ctx *);
static LRESULT (CALLBACK *g_orig_wndproc)(HWND, UINT, WPARAM, LPARAM);
static void (*g_orig_menus_mouse)(HWND, int, int);
static const MkwVkRenderer *(*g_orig_load_vulkan)(const char *);

/* ---- Guest memory & register helpers ---- */
static inline int gx_read(uint64_t addr, void *dst, size_t n) {
    if (!addr || !g_exe.hos_mem_region || !g_exe.hos_guest_base) return 0;
    if (!g_exe.hos_mem_region(addr, (uint64_t)n)) return 0;
    uint8_t *base = g_exe.hos_guest_base();
    if (!base) return 0;
    memcpy(dst, base + addr, n);
    return 1;
}
static inline int gx_write(uint64_t addr, const void *src, size_t n) {
    if (!addr || !g_exe.hos_mem_region || !g_exe.hos_guest_base) return 0;
    if (!g_exe.hos_mem_region(addr, (uint64_t)n)) return 0;
    uint8_t *base = g_exe.hos_guest_base();
    if (!base) return 0;
    memcpy(base + addr, src, n);
    if (g_exe.hos_mark_written) g_exe.hos_mark_written(base + addr, n);
    return 1;
}
static inline uint64_t gx_u64(uint64_t addr) { uint64_t v = 0; gx_read(addr, &v, 8); return v; }
static inline uint32_t gx_u32(uint64_t addr) { uint32_t v = 0; gx_read(addr, &v, 4); return v; }
static inline uint8_t gx_u8(uint64_t addr) { uint8_t v = 0; gx_read(addr, &v, 1); return v; }
static inline void gx_w32(uint64_t addr, uint32_t v) { gx_write(addr, &v, 4); }
static inline void gx_w64(uint64_t addr, uint64_t v) { gx_write(addr, &v, 8); }
static inline void gx_w8(uint64_t addr, uint8_t v) { gx_write(addr, &v, 1); }

static const char *gx_cstr(uint64_t addr, char *buf, size_t cap) {
    if (!cap) return "";
    buf[0] = 0;
    if (!addr || !g_exe.hos_guest_base || !g_exe.hos_mem_region) return "";
    if (!g_exe.hos_mem_region(addr, 1)) return "";
    const uint8_t *base = g_exe.hos_guest_base();
    if (!base) return "";
    for (size_t i = 0; i + 1 < cap; i++) {
        if (!g_exe.hos_mem_region(addr + i, 1)) break;
        char c = (char)base[addr + i];
        buf[i] = c;
        buf[i + 1] = 0;
        if (!c) return buf;
    }
    return buf;
}

/* ---- Core runtime / frame / shutdown hooks ---- */
static A64Fn hk_a64_lookup(uint64_t addr) {
    A64Fn cb = (A64Fn)(uintptr_t)a64_js_lookup_hook(addr);
    if (cb) return cb;
    return g_orig_a64_lookup(addr);
}

static void hk_hos_close_finish(void) {
    js_pipe_stop();
    hos_js_shutdown();
    g_orig_hos_close_finish();
}

static void hk_hos_vi_native_presented(const int64_t phases[4]) {
    g_orig_hos_vi_native_presented(phases);
    if (g_exe.p_hos_text_scale) {
        *g_exe.p_hos_text_scale = *g_exe.p_hos_text_scale * hos_js_text_speed_scale();
    }
    if (g_exe.p_hos_camera_scale) {
        *g_exe.p_hos_camera_scale = *g_exe.p_hos_camera_scale * hos_js_camera_speed_scale();
    }
    static int64_t frame_n = 0;
    static int64_t prev_ns = 0;
    const int64_t now = g_exe.hos_now_ns ? g_exe.hos_now_ns() : 0;
    const double dt_sec = (prev_ns && now > prev_ns) ? (double)(now - prev_ns) * 1e-9 : (1.0 / 60.0);
    prev_ns = now;
    hos_js_on_frame(++frame_n, dt_sec);
}

/* ---- OoT Player, Combat, Camera, Text, Menu, Saves, Events ---- */
static SRWLOCK g_player_lock = SRWLOCK_INIT;

static void hk_hos_oot_lift_player(Ctx *c, uint64_t actor) {
    g_orig_hos_oot_lift_player(c, actor);
    if (c && actor) hos_js_on_actor_update(c, actor);
    if (!c || !actor || gx_u64(actor) != 0x84b45e18ULL) return;
    const uint64_t components = gx_u64(actor + 624);
    const uint64_t movement = components ? gx_u64(components + 800) : 0;
    if (movement < HOS_HEAP_BASE + 0x1d50ULL || gx_u64(movement + 24) != actor) return;
    const uint64_t ctrl = hos_oot_js_player_controller(c, components);
    if (!ctrl || !g_exe.is_player || !g_exe.is_player(c, movement - 0x1d50ULL)) return;

    if (!TryAcquireSRWLockExclusive(&g_player_lock)) return;
    if (g_exe.p_oot_speed_controller && fabsf(hos_js_player_speed_scale() - 1.0f) > 0.001f) {
        __atomic_store_n(g_exe.p_oot_speed_controller, ctrl, __ATOMIC_RELAXED);
    }
    hos_js_on_player_update(c, actor, components, movement - 0x1d50ULL);
    ReleaseSRWLockExclusive(&g_player_lock);
}

static int hk_hos_oot_hearts_enabled(void) {
    const int js = hos_js_player_invincible();
    if (js >= 0) return js;
    return g_orig_hos_oot_hearts_enabled();
}

static void hk_hos_oot_cct_speed(Ctx *c, uint64_t controller, int per_second) {
    if (!c || !controller || !g_exe.p_oot_speed_controller) return;
    if (controller != __atomic_load_n(g_exe.p_oot_speed_controller, __ATOMIC_RELAXED)) return;
    const int percent = g_exe.p_oot_speed_percent ? __atomic_load_n(g_exe.p_oot_speed_percent, __ATOMIC_RELAXED) : 100;
    const float mult = (percent > 100 && percent <= 400 ? percent / 100.0f : 1.0f) * hos_js_player_speed_scale();
    if (fabsf(mult - 1.0f) <= 0.001f) return;
    if (!g_exe.hos_move_stick_held || !g_exe.hos_move_stick_held()) return;
    float speed = 0.0f;
    memcpy(&speed, &c->vreg[0][0], sizeof speed);
    if (!(fabsf(speed) * (per_second ? 1.0f : 60.0f) <= 15.0f)) return;
    speed *= mult;
    memcpy(&c->vreg[0][0], &speed, sizeof speed);
}

static uint32_t hk_hos_oot_damage(uint32_t old_life, uint32_t next_life, uint32_t hit) {
    if (next_life >= old_life) return next_life;
    if (hk_hos_oot_hearts_enabled()) return old_life;
    uint32_t js_hit = hit;
    if (hos_js_on_damage(old_life, &js_hit)) return old_life;
    const int setting = g_orig_rx_option ? g_orig_rx_option(HOS_OPT_DAMAGE_TAKEN, 1) : 1;
    const float js_mult = hos_js_damage_multiplier();
    if (js_mult <= 0.0f) return old_life;
    double mult = 1.0;
    switch (setting) {
    case 0: mult = 0.5; break;
    case 2: mult = 2.0; break;
    case 3: mult = 4.0; break;
    default: mult = 1.0; break;
    }
    mult *= (double)js_mult;
    uint64_t dmg = (setting == 0 && fabsf(js_mult - 1.0f) < 0.0001f)
                 ? (uint64_t)(js_hit + 1u) / 2u
                 : (uint64_t)llround((double)js_hit * mult);
    if (js_hit > 0 && dmg == 0 && mult > 0.0) dmg = 1;
    return dmg >= old_life ? 0 : old_life - (uint32_t)dmg;
}

static void hk_hos_oot_fov(Ctx *c, uint64_t camera) {
    int percent = g_orig_rx_option ? g_orig_rx_option(HOS_OPT_FOV, 100) : 100;
    if (percent < 50 || percent > 150) percent = 100;
    const float scale = (percent / 100.0f) * hos_js_fov_scale();
    if (fabsf(scale - 1.0f) <= 0.0005f || !camera) return;

    const uint64_t views = gx_u64(camera + 0x18);
    const uint64_t view = views ? gx_u64(views + 8) : 0;
    if (!view || gx_u64(view) != 0x84c18a88ULL) return;

    const uint64_t singleton = gx_u64(0x84ca6bc0ULL);
    const uint64_t manager = singleton ? gx_u64(singleton) : 0;
    const uint64_t cameras = (manager && gx_u32(manager + 32)) ? gx_u64(manager + 40) : 0;
    const uint64_t main_camera = cameras ? gx_u64(cameras) : 0;
    if (!main_camera || gx_u64(main_camera + 120) != view) return;

    const uint64_t event_singleton = gx_u64(0x84c9e090ULL);
    const uint64_t events = event_singleton ? gx_u64(event_singleton) : 0;
    if (events && gx_u64(events + 104)) return;

    float angle = 0.0f;
    memcpy(&angle, c->vreg[0], sizeof angle);
    if (!isfinite(angle) || angle <= 0.0f || angle >= 3.141592654f) return;
    angle = fminf(2.443460953f, fmaxf(0.087266463f, angle * scale));
    uint32_t bits = 0;
    memcpy(&bits, &angle, sizeof bits);
    c->vreg[0][0] = bits;
}

static void hk_hos_oot_free_view(Ctx *c, uint64_t view) {
    g_orig_hos_oot_free_view(c, view);
    if (!c || !view || gx_u64(view) != 0x84c18a88ULL) return;
    if (g_exe.oot_gameplay_view && g_exe.oot_gameplay_view(c) == view) {
        hos_js_on_camera_view(c, view);
    }
}

static void hk_hos_mouse_camera(uint64_t cam) {
    g_orig_hos_mouse_camera(cam);
    if (!cam || !g_exe.hos_mem_region || !g_exe.hos_mem_region(cam, 0x220)) return;
    const uint32_t state = gx_u32(cam + 0x204);
    if (state != 3 && state != 4) return;
    float pitch = 0.0f, yaw = 0.0f;
    gx_read(cam + 0x18c, &pitch, 4);
    gx_read(cam + 0x1a4, &yaw, 4);
    if (!isfinite(pitch) || !isfinite(yaw)) return;
    if (hos_js_on_mouse_camera(cam, &yaw, &pitch)) {
        gx_write(cam + 0x18c, &pitch, 4);
        gx_write(cam + 0x1a4, &yaw, 4);
    }
    if (hos_js_camera_disable_centering() > 0 && state == 3) {
        gx_w32(cam + 0x204, 4);
    }
}

enum { TEXT_ANIM_WRAPPER = 0x28, TEXT_WRAPPER_PANE = 0x08, TEXT_WRAPPER_SCALE_X = 0x28, TEXT_WRAPPER_SCALE_Y = 0x38 };
typedef struct { uint64_t addr; uint32_t game, written, seen; } JsTextSizeField;

static float menu_text_size_scale(int v) {
    static const float k[] = {1.0f, 1.10f, 1.25f, 1.50f, 1.75f};
    return v > 0 && v < (int)(sizeof k / sizeof k[0]) ? k[v] : 1.0f;
}

static void hk_hos_oot_text_pane(Ctx *c, uint64_t anim) {
    (void)c;
    static const char *env = NULL;
    static int looked = 0;
    if (!looked) looked = 1, env = getenv("HOS_TEXT_SIZE");
    const int opt = (env && *env) ? atoi(env) : (g_orig_rx_option ? g_orig_rx_option(HOS_OPT_TEXT_SIZE, 0) : 0);
    const float s = menu_text_size_scale(opt) * hos_js_text_size_scale();
    const uint64_t wrapper = anim ? gx_u64(anim + TEXT_ANIM_WRAPPER) : 0;
    if (!wrapper || !gx_u64(wrapper + TEXT_WRAPPER_PANE)) return;

    static JsTextSizeField kept[64];
    static uint32_t tick = 0;
    static volatile long lock = 0;
    static const unsigned offs[] = {TEXT_WRAPPER_SCALE_X, TEXT_WRAPPER_SCALE_Y};
    while (__sync_lock_test_and_set(&lock, 1)) {}
    tick++;
    for (unsigned i = 0; i < 2; i++) {
        const uint64_t a = wrapper + offs[i];
        const uint32_t now = gx_u32(a);
        JsTextSizeField *f = NULL, *oldest = &kept[0];
        for (unsigned k = 0; k < 64 && !f; k++) {
            if (kept[k].addr == a) f = &kept[k];
            else if (kept[k].seen < oldest->seen) oldest = &kept[k];
        }
        if (!f) {
            if (fabsf(s - 1.0f) < 0.0005f) continue;
            f = oldest; f->addr = a; f->game = f->written = now;
        }
        f->seen = tick;
        if (now != f->written) f->game = now;
        float g = 0.0f;
        memcpy(&g, &f->game, 4);
        float v = g * s;
        memcpy(&f->written, &v, 4);
        if (f->written != now) gx_w32(a, f->written);
    }
    __sync_lock_release(&lock);
}

static void hk_hos_oot_menu_tick(Ctx *c, uint64_t screen) {
    g_orig_hos_oot_menu_tick(c, screen);
    hos_js_on_menu_tick(c, screen);
}

static void hk_hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) {
    g_orig_hos_oot_menu_focus(c, screen, pane, lr);
    hos_js_on_menu_focus(c, screen, pane, lr);
}

static int hk_hos_oot_trace_save(Ctx *c) {
    const int blocked = g_orig_hos_oot_trace_save(c);
    if (blocked) return 1;
    if (!c) return 0;
    const uint64_t caller = c->x[30];
    const uint64_t parent = caller == 0x80bc7764ULL ? gx_u64(c->x[31] + 24) : 0;
    const uint32_t reason = (uint32_t)c->x[1];
    const uint32_t mode = (uint32_t)c->x[2];
    const int is_transition = (parent == 0x80bc7710ULL && reason == 6) ||
                              (parent == 0x80bc7864ULL && reason == 2) ||
                              (parent == 0x832a9764ULL && reason == 1);
    if (hos_js_on_save_request(c, reason, mode, caller, is_transition)) {
        c->x[0] = 0;
        return 1;
    }
    return 0;
}

static int hk_hos_oot_opening(Ctx *c, int mode) {
    if (c) {
        char name[80];
        name[0] = 0;
        uint64_t p = 0;
        if (mode == 0) {
            const uint64_t arg = c->x[1];
            p = arg ? gx_u64(arg + 6800) : 0;
        } else if (mode == 1 && c->x[1]) {
            p = gx_u64(c->x[1]);
        }
        if (p) gx_cstr(p, name, sizeof name);
        if (mode == 0) {
            hos_js_on_event_request(c, name, c->x[1]);
        } else if (mode == 1) {
            if (hos_js_on_timeline_start(c, name, c->x[0])) {
                const uint64_t player = c->x[0];
                if (player) gx_w8(player + 0xf25, gx_u8(player + 0xf25) & ~(uint8_t)1);
                return 1;
            }
        }
    }
    return g_orig_hos_oot_opening(c, mode);
}

/* ---- Option overrides (flight + audio volume/mute) & Audio pitch ---- */
static int hk_rx_option(int id, int otherwise) {
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        const int js_f = hos_js_player_flight();
        if (js_f >= 0) return js_f;
    }
    const int base = g_orig_rx_option(id, otherwise);
    if (id == HOS_OPT_VOLUME) {
        if (hos_js_audio_muted()) return 0;
        const float sc = hos_js_audio_volume_scale();
        if (fabsf(sc - 1.0f) > 0.001f) {
            int v = (int)lround((double)base * (double)sc);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            return v;
        }
    }
    return base;
}

typedef struct { uint64_t buffer, size; int32_t start, end; int loop; uint64_t context; } StockAudioWB;
typedef struct {
    int op, idx, tgt;
    int i0, i1, i2;
    uint64_t wbp;
    float f, b[3], a[2];
    StockAudioWB wb;
    int8_t in[24], out[24];
} StockAudioCmd;

static void hk_audio_apply(const void *raw_cmd) {
    const StockAudioCmd *cmd = (const StockAudioCmd *)raw_cmd;
    if (cmd && cmd->op == 4 /* OP_VOICE_PITCH */) {
        const float ps = hos_js_audio_pitch_scale();
        if (fabsf(ps - 1.0f) > 0.0005f) {
            StockAudioCmd copy = *cmd;
            copy.f *= ps;
            g_orig_audio_apply(&copy);
            return;
        }
    }
    g_orig_audio_apply(raw_cmd);
}

/* ---- HID Input Interceptor ---- */
static void hk_npad_states(Ctx *c) {
    const uint64_t out = c->x[0];
    const uint64_t id_ptr = c->x[2];
    g_orig_npad_states(c);
    const int ret = (int)(int32_t)c->x[0];
    if (ret <= 0 || !out || !id_ptr) return;
    if (gx_u32(id_ptr) != NPAD_NO1) return;
    if (!g_exe.hos_mem_region(out, (uint64_t)ret * STATE_SIZE)) return;

    uint64_t raw_buttons = 0;
    int32_t raw_stick[4] = {0, 0, 0, 0};
    gx_read(out + 8, &raw_buttons, 8);
    gx_read(out + 16, raw_stick, 16);

    uint64_t buttons = raw_buttons;
    int32_t stick[4];
    memcpy(stick, raw_stick, sizeof stick);

    if (g_exe.hos_text_focus && g_exe.hos_text_focus()) {
        buttons = 0;
        memset(stick, 0, sizeof stick);
    }
    hos_js_on_input(&buttons, stick, raw_buttons, raw_stick);

    for (int i = 0; i < ret; i++) {
        const uint64_t e = out + (uint64_t)i * STATE_SIZE;
        gx_write(e + 8, &buttons, 8);
        gx_write(e + 16, stick, 16);
    }
}

/* ---- RomFS Overrides & Virtual Files (0x70000000 | slot) ---- */
#define MAX_VHANDLES 64
#define VHANDLE_TAG 0x70000000ULL

typedef struct {
    int used;
    uint8_t *buf;
    uint64_t len;
} VHandle;

static VHandle g_vhandles[MAX_VHANDLES];
static SRWLOCK g_vh_lock = SRWLOCK_INIT;

static int vhandle_slot(uint64_t id) {
    if ((id & 0xFFFFFFFFFFFFFFC0ULL) != VHANDLE_TAG) return -1;
    return (int)(id & 0x3FULL);
}

static uint64_t vhandle_alloc(uint8_t *buf, uint64_t len) {
    AcquireSRWLockExclusive(&g_vh_lock);
    for (int i = 0; i < MAX_VHANDLES; i++) {
        if (!g_vhandles[i].used) {
            g_vhandles[i].used = 1;
            g_vhandles[i].buf = buf;
            g_vhandles[i].len = len;
            ReleaseSRWLockExclusive(&g_vh_lock);
            return VHANDLE_TAG | (uint64_t)i;
        }
    }
    ReleaseSRWLockExclusive(&g_vh_lock);
    return 0;
}

static uint64_t vhandle_read(Ctx *c, uint64_t id, int64_t off, uint64_t dst_guest, uint64_t size, int *bad_range) {
    (void)c;
    *bad_range = 0;
    const int slot = vhandle_slot(id);
    if (slot < 0 || off < 0) return 0;
    if (!g_exe.hos_mem_region || !g_exe.hos_guest_base || !g_exe.hos_mem_region(dst_guest, size)) {
        *bad_range = 1;
        return 0;
    }
    uint64_t copied = 0;
    AcquireSRWLockShared(&g_vh_lock);
    if (g_vhandles[slot].used && g_vhandles[slot].buf && (uint64_t)off < g_vhandles[slot].len) {
        const uint64_t avail = g_vhandles[slot].len - (uint64_t)off;
        copied = size < avail ? size : avail;
        if (copied) {
            uint8_t *dst = g_exe.hos_guest_base() + dst_guest;
            memcpy(dst, g_vhandles[slot].buf + off, (size_t)copied);
            if (g_exe.hos_mark_written) g_exe.hos_mark_written(dst, (size_t)copied);
        }
    }
    ReleaseSRWLockShared(&g_vh_lock);
    return copied;
}

static _Thread_local int tls_in_host_path = 0;

static int hk_fs_host_path(const char *guest_mem, char *out, size_t cap, int *writable) {
    if (!tls_in_host_path && guest_mem && g_orig_fs_rom_rel) {
        tls_in_host_path = 1;
        const char *vrel = g_orig_fs_rom_rel(guest_mem);
        tls_in_host_path = 0;
        if (vrel) {
            char alt[FS_PATH_MAX];
            if (hos_js_romfs_redirect(vrel, alt, sizeof alt)) {
                snprintf(out, cap, "%s", alt);
                for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
                if (writable) *writable = 0;
                return 1;
            }
        }
    }
    return g_orig_fs_host_path(guest_mem, out, cap, writable);
}

static void hk_fs_open_file(Ctx *c) {
    char guest[FS_PATH_MAX];
    const char *g = gx_cstr(c->x[1], guest, sizeof guest);
    const int want_write = (c->x[2] & 6) != 0;
    if (!want_write && g[0] && g_orig_fs_rom_rel) {
        const char *vrel = g_orig_fs_rom_rel(g);
        uint8_t *vbuf = NULL;
        uint64_t vlen = 0;
        if (vrel && hos_js_romfs_virtual_copy(vrel, &vbuf, &vlen)) {
            const uint64_t vh = vhandle_alloc(vbuf, vlen);
            if (vh) {
                gx_w64(c->x[0], vh);
                c->x[0] = 0;
                return;
            }
            free(vbuf);
        }
    }
    g_orig_fs_open_file(c);
}

static void hk_fs_read_0(Ctx *c) {
    if (vhandle_slot(c->x[0]) >= 0) {
        int bad = 0;
        const uint64_t n = vhandle_read(c, c->x[0], (int64_t)c->x[1], c->x[2], c->x[3], &bad);
        c->x[0] = bad ? R_OUT_OF_MEMORY : (n == c->x[3] ? 0 : 0x2EE202ULL);
        return;
    }
    g_orig_fs_read_0(c);
}

static void hk_fs_read_1(Ctx *c) {
    if (vhandle_slot(c->x[1]) >= 0) {
        int bad = 0;
        const uint64_t n = vhandle_read(c, c->x[1], (int64_t)c->x[2], c->x[3], c->x[4], &bad);
        gx_w64(c->x[0], n);
        c->x[0] = bad ? R_OUT_OF_MEMORY : 0;
        return;
    }
    g_orig_fs_read_1(c);
}

static void hk_fs_read_2(Ctx *c) {
    if (vhandle_slot(c->x[1]) >= 0) {
        int bad = 0;
        const uint64_t n = vhandle_read(c, c->x[1], (int64_t)c->x[2], c->x[3], c->x[4], &bad);
        gx_w64(c->x[0], n);
        c->x[0] = bad ? R_OUT_OF_MEMORY : 0;
        return;
    }
    g_orig_fs_read_2(c);
}

static void hk_fs_get_file_size(Ctx *c) {
    const int slot = vhandle_slot(c->x[1]);
    if (slot >= 0) {
        uint64_t sz = 0;
        AcquireSRWLockShared(&g_vh_lock);
        if (g_vhandles[slot].used) sz = g_vhandles[slot].len;
        ReleaseSRWLockShared(&g_vh_lock);
        gx_w64(c->x[0], sz);
        c->x[0] = 0;
        return;
    }
    g_orig_fs_get_file_size(c);
}

static void hk_fs_close_file(Ctx *c) {
    const int slot = vhandle_slot(c->x[0]);
    if (slot >= 0) {
        uint8_t *buf = NULL;
        AcquireSRWLockExclusive(&g_vh_lock);
        if (g_vhandles[slot].used) {
            buf = g_vhandles[slot].buf;
            g_vhandles[slot].used = 0;
            g_vhandles[slot].buf = NULL;
            g_vhandles[slot].len = 0;
        }
        ReleaseSRWLockExclusive(&g_vh_lock);
        free(buf);
        c->x[0] = 0;
        return;
    }
    g_orig_fs_close_file(c);
}

static void hk_fs_get_entry_type(Ctx *c) {
    char guest[FS_PATH_MAX];
    const char *g = gx_cstr(c->x[1], guest, sizeof guest);
    if (g[0] && g_orig_fs_rom_rel) {
        const char *vrel = g_orig_fs_rom_rel(g);
        if (vrel && hos_js_romfs_virtual_exists(vrel, NULL)) {
            gx_w32(c->x[0], 1);
            c->x[0] = 0;
            return;
        }
    }
    g_orig_fs_get_entry_type(c);
}

/* ---- Display window hook (WM_APP+13 JS mouse unlock + VK_UP/VK_DOWN console history) ---- */
static volatile LONG g_js_want_mouse = 0;
static volatile LONG g_insert_menu_open = 0;
static volatile LONG g_game_menu_open = 0;

static void hk_menus_mouse(HWND w, int insert_open, int game_open) {
    InterlockedExchange(&g_insert_menu_open, insert_open ? 1 : 0);
    InterlockedExchange(&g_game_menu_open, game_open ? 1 : 0);
    g_orig_menus_mouse(w, (insert_open || InterlockedCompareExchange(&g_js_want_mouse, 0, 0)) ? 1 : 0, game_open);
}

static LRESULT CALLBACK hk_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (w && !asi_get_game_hwnd()) asi_set_game_hwnd(w);
    if (msg == WM_APP_JS_MOUSE) {
        InterlockedExchange(&g_js_want_mouse, wp ? 1 : 0);
        const int ins = InterlockedCompareExchange(&g_insert_menu_open, 0, 0);
        const int gam = InterlockedCompareExchange(&g_game_menu_open, 0, 0);
        hk_menus_mouse(w, ins, gam);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        if (g_exe.hos_text_focus && g_exe.hos_text_put && g_exe.hos_text_focus()) {
            g_exe.hos_text_put(HOS_TEXT_KEY | (uint32_t)wp);
        }
    }
    return g_orig_wndproc(w, msg, wp, lp);
}

static const MkwVkRenderer *hk_load_vulkan(const char *dll) {
    const MkwVkRenderer *r = g_orig_load_vulkan(dll);
    if (r) {
        HMODULE vk_mod = GetModuleHandleA("longshot_vk.dll");
        if (vk_mod) asi_hooks_vk_attach(vk_mod);
        else if (g_exe.hos_log) g_exe.hos_log("longshot-js: WARNING longshot_vk.dll module handle not found");
    }
    return r;
}

typedef struct {
    const char *name;
    void *target;
    void *detour;
    void **tramp;
} HookEntry;

int asi_hooks_exe_install(void) {
    const HookEntry list[] = {
        {"a64_lookup", (void *)g_exe.a64_lookup, (void *)hk_a64_lookup, (void **)&g_orig_a64_lookup},
        {"hos_close_finish", g_exe.fn_hos_close_finish, (void *)hk_hos_close_finish, (void **)&g_orig_hos_close_finish},
        {"hos_vi_native_presented", g_exe.fn_hos_vi_native_presented, (void *)hk_hos_vi_native_presented, (void **)&g_orig_hos_vi_native_presented},
        {"hos_oot_lift_player", g_exe.fn_hos_oot_lift_player, (void *)hk_hos_oot_lift_player, (void **)&g_orig_hos_oot_lift_player},
        {"hos_oot_hearts_enabled", g_exe.fn_hos_oot_hearts_enabled, (void *)hk_hos_oot_hearts_enabled, (void **)&g_orig_hos_oot_hearts_enabled},
        {"hos_oot_cct_speed", g_exe.fn_hos_oot_cct_speed, (void *)hk_hos_oot_cct_speed, (void **)&g_orig_hos_oot_cct_speed},
        {"hos_oot_damage", g_exe.fn_hos_oot_damage, (void *)hk_hos_oot_damage, (void **)&g_orig_hos_oot_damage},
        {"hos_oot_fov", g_exe.fn_hos_oot_fov, (void *)hk_hos_oot_fov, (void **)&g_orig_hos_oot_fov},
        {"hos_oot_free_view", g_exe.fn_hos_oot_free_view, (void *)hk_hos_oot_free_view, (void **)&g_orig_hos_oot_free_view},
        {"hos_mouse_camera", g_exe.fn_hos_mouse_camera, (void *)hk_hos_mouse_camera, (void **)&g_orig_hos_mouse_camera},
        {"hos_oot_text_pane", g_exe.fn_hos_oot_text_pane, (void *)hk_hos_oot_text_pane, (void **)&g_orig_hos_oot_text_pane},
        {"hos_oot_menu_tick", g_exe.fn_hos_oot_menu_tick, (void *)hk_hos_oot_menu_tick, (void **)&g_orig_hos_oot_menu_tick},
        {"hos_oot_menu_focus", g_exe.fn_hos_oot_menu_focus, (void *)hk_hos_oot_menu_focus, (void **)&g_orig_hos_oot_menu_focus},
        {"hos_oot_trace_save", g_exe.fn_hos_oot_trace_save, (void *)hk_hos_oot_trace_save, (void **)&g_orig_hos_oot_trace_save},
        {"hos_oot_opening", g_exe.fn_hos_oot_opening, (void *)hk_hos_oot_opening, (void **)&g_orig_hos_oot_opening},
        {"rx_option", (void *)g_exe.rx_option, (void *)hk_rx_option, (void **)&g_orig_rx_option},
        {"npad_states", g_exe.fn_npad_states, (void *)hk_npad_states, (void **)&g_orig_npad_states},
        {"audio_apply", g_exe.fn_audio_apply, (void *)hk_audio_apply, (void **)&g_orig_audio_apply},
        {"host_path", g_exe.fn_fs_host_path, (void *)hk_fs_host_path, (void **)&g_orig_fs_host_path},
        {"OpenFile", g_exe.fn_fs_open_file, (void *)hk_fs_open_file, (void **)&g_orig_fs_open_file},
        {"ReadFile_0", g_exe.fn_fs_read_file_0, (void *)hk_fs_read_0, (void **)&g_orig_fs_read_0},
        {"ReadFile_1", g_exe.fn_fs_read_file_1, (void *)hk_fs_read_1, (void **)&g_orig_fs_read_1},
        {"ReadFile_2", g_exe.fn_fs_read_file_2, (void *)hk_fs_read_2, (void **)&g_orig_fs_read_2},
        {"GetFileSize", g_exe.fn_fs_get_file_size, (void *)hk_fs_get_file_size, (void **)&g_orig_fs_get_file_size},
        {"CloseFile", g_exe.fn_fs_close_file, (void *)hk_fs_close_file, (void **)&g_orig_fs_close_file},
        {"GetEntryType", g_exe.fn_fs_get_entry_type, (void *)hk_fs_get_entry_type, (void **)&g_orig_fs_get_entry_type},
        {"wndproc", g_exe.fn_wndproc, (void *)hk_wndproc, (void **)&g_orig_wndproc},
        {"menus_mouse", g_exe.fn_menus_mouse, (void *)hk_menus_mouse, (void **)&g_orig_menus_mouse},
        {"load_vulkan", g_exe.fn_load_vulkan, (void *)hk_load_vulkan, (void **)&g_orig_load_vulkan},
    };
    g_orig_fs_rom_rel = (const char *(*)(const char *))g_exe.fn_fs_rom_rel;

    int created = 0;
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        if (!list[i].target) continue;
        MH_STATUS st = MH_CreateHook(list[i].target, list[i].detour, list[i].tramp);
        if (st == MH_OK) {
            created++;
        } else if (g_exe.hos_log) {
            g_exe.hos_log("longshot-js: MH_CreateHook(%s) failed (%d)", list[i].name, (int)st);
        }
    }
    MH_STATUS en = MH_EnableHook(MH_ALL_HOOKS);
    if (en != MH_OK && g_exe.hos_log) {
        g_exe.hos_log("longshot-js: MH_EnableHook failed (%d)", (int)en);
    }
    return created;
}
