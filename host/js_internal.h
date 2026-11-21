/* Internal shared header for Longshot JavaScript runtime (host/js_*.c). */
#pragma once

#include "hos.h"
#include "hos_paths.h"
#include "js_api.h"
#include "control.h"
#include "../gpu/vk/vk_api.h"
#include "../asi/js_ui_types.h"
#include "quickjs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ---- Runtime & thread synchronization ---- */
extern JSRuntime *g_js_rt;
extern JSContext *g_js_ctx;
extern _Thread_local Ctx *g_js_tls_ctx;
extern volatile LONG g_js_ready;

int js_enter(void);
void js_leave(void);
void js_drain_jobs(JSContext *ctx);
void js_log_exception(JSContext *ctx, const char *where);
void js_console_push(int level, const char *source, const char *msg);
void js_console_clear(void);
int js_console_copy(MkwVkJsLogLine *out, int max);

/* ---- Paths & script registry helpers ---- */
const char *js_scripts_dir(void);
const char *js_data_dir(void);
int js_scripts_copy(MkwVkJsScript *out, int max);
int js_set_script_enabled(const char *id, int enabled);
void js_request_reload(void);

/* ---- Safe guest memory helpers (always bounds-checked before access) ---- */
uint8_t *hos_guest_base(void);
uint64_t hos_guest_size(void);
uint64_t hos_guest_alloc(uint64_t size);
#if !defined(A64_WRITE_TRACK) || defined(__EMSCRIPTEN__)
void hos_mark_written(const void *p, size_t n);
#endif
int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);
int rx_control_frame_info(int64_t *presents, int64_t *draws, int *width, int *height);
int rx_control_setting(int index, HosControlSetting *out);
int rx_control_setting_set(const char *key, int value, HosControlSetting *out);
int rx_control_shot(const char *path, int64_t after, void (*done)(void *ctx, int64_t present, int ok), void *ctx);
int hos_oot_destination(int index, const char **child, const char **adult);
int hos_oot_teleport_ask(int index);
int hos_oot_age_ask(int adult);
int hos_mods_count(void);
int hos_mod_scripts_dir(int index, char *out, size_t cap, const char **name_out);
#ifdef LONGSHOT_ASI
int64_t hos_js_asi_present_count(void);
#else
extern volatile int64_t hos_present_count;
#endif

int js_player_stats(int values[5]);
int js_area_name(const char *map, char *out, size_t cap);
const uint32_t *js_lifted_functions(uint32_t *total);
uint64_t js_zstd_size(const void *src, size_t size);
int js_zstd_decode(void *dst, size_t cap, const void *src, size_t size, const void *dict, size_t dict_size);

static inline int js_mem_ok(uint64_t addr, uint64_t size) {
    if (!addr || addr < 0x100000ULL || !size) return 0;
    return hos_mem_region(addr, size) != 0;
}

static inline int js_mem_read(uint64_t addr, void *dst, uint64_t size) {
    if (!js_mem_ok(addr, size)) return 0;
    uint8_t *base = hos_guest_base();
    if (!base) return 0;
    memcpy(dst, base + addr, (size_t)size);
    return 1;
}

static inline int js_mem_write(uint64_t addr, const void *src, uint64_t size) {
    if (!js_mem_ok(addr, size)) return 0;
    uint8_t *base = hos_guest_base();
    if (!base) return 0;
    memcpy(base + addr, src, (size_t)size);
    hos_mark_written(base + addr, (size_t)size);
    return 1;
}

static inline int js_mem_u8(uint64_t a, uint8_t *v) { return js_mem_read(a, v, 1); }
static inline int js_mem_u16(uint64_t a, uint16_t *v) { return js_mem_read(a, v, 2); }
static inline int js_mem_u32(uint64_t a, uint32_t *v) { return js_mem_read(a, v, 4); }
static inline int js_mem_u64(uint64_t a, uint64_t *v) { return js_mem_read(a, v, 8); }
static inline int js_mem_f32(uint64_t a, float *v) { return js_mem_read(a, v, 4); }

static inline uint32_t js_murmur3_32(const void *key, size_t len, uint32_t seed) {
    const uint8_t *s = (const uint8_t *)key;
    uint32_t h = seed, k;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        k = (uint32_t)s[i] | ((uint32_t)s[i + 1] << 8) | ((uint32_t)s[i + 2] << 16) | ((uint32_t)s[i + 3] << 24);
        k *= 0xcc9e2d51u;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593u;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5u + 0xe6546b64u;
    }
    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)s[i + 2] << 16; /* fallthrough */
    case 2: k ^= (uint32_t)s[i + 1] << 8;  /* fallthrough */
    case 1:
        k ^= (uint32_t)s[i];
        k *= 0xcc9e2d51u;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static inline int js_to_u64(JSContext *ctx, JSValueConst val, uint64_t *out) {
    if (JS_IsBigInt(val)) return JS_ToBigUint64(ctx, out, val);
    double d = 0;
    if (JS_ToFloat64(ctx, &d, val) < 0 || !isfinite(d) || d < 0) return -1;
    *out = (uint64_t)d;
    return 0;
}

static inline int js_to_u32(JSContext *ctx, JSValueConst val, uint32_t *out) {
    if (JS_IsBigInt(val)) {
        uint64_t v = 0;
        if (JS_ToBigUint64(ctx, &v, val) < 0) return -1;
        *out = (uint32_t)v;
        return 0;
    }
    return JS_ToUint32(ctx, out, val);
}

/* ---- Shared helpers between submodules ---- */
void js_json_escape_append(char *dst, size_t cap, size_t *pos, const char *s);
void js_set_current_bgm(const char *s);
const char *js_current_bgm(void);
void js_emit_event(JSContext *ctx, const char *name, int argc, JSValueConst *argv);
void js_fire_event_callbacks(JSContext *ctx, const char *name, int argc, JSValueConst *argv);
int js_has_event_listeners(const char *name);
void js_queue_player_task(JSContext *ctx, JSValue fn, JSValue resolve, JSValue reject);
void js_drain_player_tasks(JSContext *ctx, Ctx *c);
uint32_t js_parse_color(JSContext *ctx, JSValueConst val, uint32_t def);

/* ---- Submodule registration & lifecycle ---- */
void js_register_memory(JSContext *ctx, JSValue ns_mem, JSValue ns_native);
void js_memory_reset(JSContext *ctx);

void js_register_oot(JSContext *ctx, JSValue ns_game);
void js_oot_reset(JSContext *ctx);
void js_oot_tick_player(JSContext *ctx, Ctx *c, uint64_t actor, uint64_t components, uint64_t player);

void js_register_io(JSContext *ctx, JSValue ns_game, JSValue ns_fs, JSValue ns_events, JSValue global);
void js_io_reset(JSContext *ctx);
void js_io_tick_timers(JSContext *ctx, int64_t frame_num);

void js_register_ui(JSContext *ctx, JSValue ns_ui);
void js_ui_reset(JSContext *ctx);
void js_ui_pump_events(JSContext *ctx);
void js_ui_begin_frame(void);
void js_ui_end_frame(void);
