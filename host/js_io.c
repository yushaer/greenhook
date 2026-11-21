/* `events`, `game.input`, `game.audio`, `game.graphics`, `game.romfs`, sandboxed `fs`, and frame/ms timers. */
#include "js_internal.h"
#include <ctype.h>

/* ---- Atomic Audio / Text modifiers ---- */
static volatile LONG g_text_speed_bits = 0x3f800000L;
static volatile LONG g_text_size_bits  = 0x3f800000L;
static volatile LONG g_audio_vol_bits  = 0x3f800000L;
static volatile LONG g_audio_pit_bits  = 0x3f800000L;
static volatile LONG g_audio_muted     = 0;

static float load_f32(volatile LONG *p) {
    uint32_t u = (uint32_t)__atomic_load_n(p, __ATOMIC_RELAXED);
    float f = 1.0f;
    memcpy(&f, &u, 4);
    return f;
}
static void store_f32(volatile LONG *p, float f) {
    uint32_t u = 0;
    memcpy(&u, &f, 4);
    __atomic_store_n(p, (LONG)u, __ATOMIC_RELAXED);
}

float hos_js_text_speed_scale(void) { return load_f32(&g_text_speed_bits); }
float hos_js_text_size_scale(void)  { return load_f32(&g_text_size_bits); }
float hos_js_audio_volume_scale(void) { return load_f32(&g_audio_vol_bits); }
float hos_js_audio_pitch_scale(void)  { return load_f32(&g_audio_pit_bits); }
int hos_js_audio_muted(void) { return (int)__atomic_load_n(&g_audio_muted, __ATOMIC_RELAXED); }

/* ---- Frame timing & FPS measurement ---- */
static SRWLOCK g_io_lock = SRWLOCK_INIT;
static int64_t g_last_frame_num = 0;
static int64_t g_frame_ts[32];
static unsigned g_frame_ts_n = 0;
static volatile LONG g_fps_bits = 0;

/* ---- Event Bus ---- */
typedef struct {
    int used;
    int once;
    int64_t id;
    char name[48];
    JSValue fn;
} EventListener;
#define JS_EVENT_MAX 256
static int64_t next_subscription_id;
static EventListener g_listeners[JS_EVENT_MAX];
static volatile LONG g_has_frame_l = 0, g_has_player_l = 0, g_has_damage_l = 0, g_has_cam_l = 0;
static volatile LONG g_has_input_l = 0, g_has_evreq_l = 0, g_has_tl_l = 0, g_has_save_l = 0;
static volatile LONG g_has_actor_l;
static volatile LONG g_has_map_l = 0, g_has_menu_tick_l = 0, g_has_menu_focus_l = 0, g_has_bgm_l = 0, g_has_unload_l = 0;

static volatile LONG *hot_counter_for(const char *name) {
    if (!strcmp(name, "actorUpdate")) return &g_has_actor_l;
    if (!strcmp(name, "frame")) return &g_has_frame_l;
    if (!strcmp(name, "playerUpdate")) return &g_has_player_l;
    if (!strcmp(name, "damage")) return &g_has_damage_l;
    if (!strcmp(name, "cameraUpdate")) return &g_has_cam_l;
    if (!strcmp(name, "input")) return &g_has_input_l;
    if (!strcmp(name, "eventRequest")) return &g_has_evreq_l;
    if (!strcmp(name, "timelineStart")) return &g_has_tl_l;
    if (!strcmp(name, "saveRequest")) return &g_has_save_l;
    if (!strcmp(name, "mapChange")) return &g_has_map_l;
    if (!strcmp(name, "menuTick")) return &g_has_menu_tick_l;
    if (!strcmp(name, "menuFocus")) return &g_has_menu_focus_l;
    if (!strcmp(name, "bgmChange")) return &g_has_bgm_l;
    if (!strcmp(name, "unload")) return &g_has_unload_l;
    return NULL;
}

static void recount_hot_flags(void) {
    g_has_actor_l = g_has_frame_l = g_has_player_l = g_has_damage_l = g_has_cam_l = 0;
    g_has_input_l = g_has_evreq_l = g_has_tl_l = g_has_save_l = 0;
    g_has_map_l = g_has_menu_tick_l = g_has_menu_focus_l = g_has_bgm_l = g_has_unload_l = 0;
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (!g_listeners[i].used) continue;
        volatile LONG *c = hot_counter_for(g_listeners[i].name);
        if (c) InterlockedIncrement(c);
    }
}

int js_has_event_listeners(const char *name) {
    if (!hos_js_is_ready()) return 0;
    volatile LONG *c = hot_counter_for(name);
    if (c) return __atomic_load_n(c, __ATOMIC_RELAXED) > 0;
    int any = 0;
    AcquireSRWLockShared(&g_io_lock);
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (g_listeners[i].used && !strcmp(g_listeners[i].name, name)) { any = 1; break; }
    }
    ReleaseSRWLockShared(&g_io_lock);
    return any;
}

void js_fire_event_callbacks(JSContext *ctx, const char *name, int argc, JSValueConst *argv) {
    JSValue calls[JS_EVENT_MAX];
    int slots[JS_EVENT_MAX];
    int64_t once_ids[JS_EVENT_MAX];
    int n = 0;
    AcquireSRWLockShared(&g_io_lock);
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (g_listeners[i].used && !strcmp(g_listeners[i].name, name)) {
            calls[n] = JS_DupValue(ctx, g_listeners[i].fn);
            slots[n] = i;
            once_ids[n] = g_listeners[i].once ? g_listeners[i].id : 0;
            n++;
        }
    }
    ReleaseSRWLockShared(&g_io_lock);

    int removed = 0;
    for (int i = 0; i < n; i++) {
        const int slot = slots[i];
        if (once_ids[i]) {
            if (!g_listeners[slot].used || g_listeners[slot].id != once_ids[i]) {
                JS_FreeValue(ctx, calls[i]);
                continue;
            }
            JS_FreeValue(ctx, g_listeners[slot].fn);
            g_listeners[slot].fn = JS_UNDEFINED;
            g_listeners[slot].used = 0;
            removed = 1;
        }
        JSValue r = JS_Call(ctx, calls[i], JS_UNDEFINED, argc, argv);
        if (JS_IsException(r)) js_log_exception(ctx, name);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, calls[i]);
    }
    if (removed) recount_hot_flags();
}

void js_emit_event(JSContext *ctx, const char *name, int argc, JSValueConst *argv) {
    js_fire_event_callbacks(ctx, name, argc, argv);
}

static JSValue unsub_magic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *data) {
    (void)this_val, (void)argc, (void)argv;
    int64_t id;
    if (JS_ToInt64(ctx, &id, data[0]) < 0) return JS_EXCEPTION;
    AcquireSRWLockExclusive(&g_io_lock);
    if (magic >= 0 && magic < JS_EVENT_MAX && g_listeners[magic].used && g_listeners[magic].id == id) {
        JS_FreeValue(ctx, g_listeners[magic].fn);
        g_listeners[magic].fn = JS_UNDEFINED;
        g_listeners[magic].used = 0;
        recount_hot_flags();
    }
    ReleaseSRWLockExclusive(&g_io_lock);
    return JS_UNDEFINED;
}

static JSValue add_listener(JSContext *ctx, const char *name, JSValueConst fn, int once) {
    if (!JS_IsFunction(ctx, fn)) return JS_ThrowTypeError(ctx, "listener must be a function");
    int slot = -1;
    AcquireSRWLockExclusive(&g_io_lock);
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (!g_listeners[i].used) {
            slot = i;
            g_listeners[i].id = ++next_subscription_id;
            g_listeners[i].used = 1;
            g_listeners[i].once = once ? 1 : 0;
            snprintf(g_listeners[i].name, sizeof g_listeners[i].name, "%s", name);
            g_listeners[i].fn = JS_DupValue(ctx, fn);
            recount_hot_flags();
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_io_lock);
    if (slot < 0) return JS_ThrowRangeError(ctx, "event listener limit (%d) reached", JS_EVENT_MAX);
    JSValue id = JS_NewInt64(ctx, g_listeners[slot].id);
    JSValue unsub = JS_NewCFunctionData(ctx, unsub_magic, 0, slot, 1, &id);
    JS_FreeValue(ctx, id);
    return unsub;
}

static JSValue js_events_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "events.on(name, fn)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue r = add_listener(ctx, name, argv[1], 0);
    JS_FreeCString(ctx, name);
    return r;
}
static JSValue js_events_once(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "events.once(name, fn)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue r = add_listener(ctx, name, argv[1], 1);
    JS_FreeCString(ctx, name);
    return r;
}
static JSValue js_events_off(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    int removed = 0;
    AcquireSRWLockExclusive(&g_io_lock);
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (g_listeners[i].used && !strcmp(g_listeners[i].name, name) &&
            JS_IsSameValue(ctx, g_listeners[i].fn, argv[1])) {
            JS_FreeValue(ctx, g_listeners[i].fn);
            g_listeners[i].fn = JS_UNDEFINED;
            g_listeners[i].used = 0;
            removed++;
        }
    }
    if (removed) recount_hot_flags();
    ReleaseSRWLockExclusive(&g_io_lock);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, removed > 0);
}
static JSValue js_events_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    js_fire_event_callbacks(ctx, name, argc - 1, argc > 1 ? argv + 1 : NULL);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue game_on_helper(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    static const char *const kEvNames[] = {
        "frame", "playerUpdate", "damage", "cameraUpdate", "input", "eventRequest",
        "timelineStart", "saveRequest", "mapChange", "menuTick", "menuFocus", "bgmChange", "unload", "actorUpdate"
    };
    if (argc < 1 || magic < 0 || magic >= (int)(sizeof kEvNames / sizeof kEvNames[0]))
        return JS_ThrowTypeError(ctx, "expected listener callback");
    return add_listener(ctx, kEvNames[magic], argv[0], 0);
}

static JSValue js_game_defer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "game.defer(fn)");
    if (g_js_tls_ctx) {
        JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue ex = JS_GetException(ctx);
            JSValue rej = JS_NewSettledPromise(ctx, true, ex);
            JS_FreeValue(ctx, ex);
            return rej;
        }
        JSValue ful = JS_NewSettledPromise(ctx, false, r);
        JS_FreeValue(ctx, r);
        return ful;
    }
    JSValue res_rej[2];
    JSValue p = JS_NewPromiseCapability(ctx, res_rej);
    if (JS_IsException(p)) return p;
    js_queue_player_task(ctx, JS_DupValue(ctx, argv[0]), res_rej[0], res_rej[1]);
    return p;
}

/* ---- Timers & frame waiters ---- */
typedef struct {
    int used, interval;
    int id;
    int64_t next_ns, period_ns;
    JSValue fn;
} TimerSlot;
#define TIMER_MAX 128
static TimerSlot g_timers[TIMER_MAX];
static int g_next_timer_id = 1;

typedef struct {
    int used;
    int64_t target_frame;
    JSValue resolve;
} FrameWaiter;
#define WAITER_MAX 64
static FrameWaiter g_waiters[WAITER_MAX];

static JSValue js_set_timeout_impl(JSContext *ctx, int argc, JSValueConst *argv, int interval) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "timer callback required");
    double ms = 0;
    if (argc >= 2 && JS_ToFloat64(ctx, &ms, argv[1]) < 0) return JS_EXCEPTION;
    if (!isfinite(ms) || ms < 0) ms = 0;
    const int64_t pns = (int64_t)(ms * 1e6);
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].used) {
            g_timers[i].used = 1;
            g_timers[i].interval = interval;
            g_timers[i].id = g_next_timer_id++;
            g_timers[i].period_ns = pns > 1000000 ? pns : (interval ? 1000000 : pns);
            g_timers[i].next_ns = hos_now_ns() + g_timers[i].period_ns;
            g_timers[i].fn = JS_DupValue(ctx, argv[0]);
            return JS_NewInt32(ctx, g_timers[i].id);
        }
    }
    return JS_ThrowRangeError(ctx, "timer limit (%d) reached", TIMER_MAX);
}

static JSValue js_setTimeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return js_set_timeout_impl(ctx, argc, argv, 0);
}
static JSValue js_setInterval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return js_set_timeout_impl(ctx, argc, argv, 1);
}
static JSValue js_clearTimer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t id = 0;
    if (argc < 1 || JS_ToInt32(ctx, &id, argv[0]) < 0) return JS_UNDEFINED;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (g_timers[i].used && g_timers[i].id == id) {
            JS_FreeValue(ctx, g_timers[i].fn);
            memset(&g_timers[i], 0, sizeof g_timers[i]);
            break;
        }
    }
    return JS_UNDEFINED;
}

static JSValue make_frame_waiter(JSContext *ctx, int64_t delta) {
    if (delta < 1) delta = 1;
    JSValue rr[2];
    JSValue p = JS_NewPromiseCapability(ctx, rr);
    if (JS_IsException(p)) return p;
    JS_FreeValue(ctx, rr[1]);
    for (int i = 0; i < WAITER_MAX; i++) {
        if (!g_waiters[i].used) {
            g_waiters[i].used = 1;
            g_waiters[i].target_frame = g_last_frame_num + delta;
            g_waiters[i].resolve = rr[0];
            return p;
        }
    }
    JS_FreeValue(ctx, rr[0]);
    JS_FreeValue(ctx, p);
    return JS_ThrowRangeError(ctx, "waitFrames limit (%d) reached", WAITER_MAX);
}
static JSValue js_nextFrame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return make_frame_waiter(ctx, 1);
}
static JSValue js_waitFrames(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int64_t n = 1;
    if (argc >= 1 && JS_ToInt64(ctx, &n, argv[0]) < 0) return JS_EXCEPTION;
    return make_frame_waiter(ctx, n);
}
static JSValue js_sleep(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double ms = 0;
    if (argc >= 1 && JS_ToFloat64(ctx, &ms, argv[0]) < 0) return JS_EXCEPTION;
    if (!isfinite(ms) || ms < 0) ms = 0;
    JSValue rr[2];
    JSValue p = JS_NewPromiseCapability(ctx, rr);
    if (JS_IsException(p)) return p;
    JS_FreeValue(ctx, rr[1]);
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].used) {
            g_timers[i].used = 1;
            g_timers[i].interval = 0;
            g_timers[i].id = g_next_timer_id++;
            g_timers[i].period_ns = (int64_t)(ms * 1e6);
            g_timers[i].next_ns = hos_now_ns() + g_timers[i].period_ns;
            g_timers[i].fn = rr[0];
            return p;
        }
    }
    JS_FreeValue(ctx, rr[0]);
    JS_FreeValue(ctx, p);
    return JS_ThrowRangeError(ctx, "timer limit (%d) reached", TIMER_MAX);
}
static JSValue js_now_ms(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_now_ns() / 1e6);
}

typedef struct {
    int used;
    volatile LONG done;
    int ok;
    int64_t present;
    char path[MAX_PATH];
    JSValue resolve;
} ShotWait;
static ShotWait g_shot_wait;
static void on_shot_done(void *p, int64_t present, int ok) {
    (void)p;
    g_shot_wait.ok = ok;
    g_shot_wait.present = present;
    __atomic_store_n(&g_shot_wait.done, 1, __ATOMIC_RELEASE);
}

void js_io_tick_timers(JSContext *ctx, int64_t frame_num) {
    g_last_frame_num = frame_num;
    if (g_shot_wait.used && __atomic_load_n(&g_shot_wait.done, __ATOMIC_ACQUIRE)) {
        JSValue res = g_shot_wait.resolve;
        g_shot_wait.used = 0;
        g_shot_wait.resolve = JS_UNDEFINED;
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "ok", JS_NewBool(ctx, g_shot_wait.ok));
        JS_SetPropertyStr(ctx, o, "present", JS_NewInt64(ctx, g_shot_wait.present));
        JS_SetPropertyStr(ctx, o, "path", JS_NewString(ctx, g_shot_wait.path));
        JSValue r = JS_Call(ctx, res, JS_UNDEFINED, 1, &o);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, res);
    }
    for (int i = 0; i < WAITER_MAX; i++) {
        if (g_waiters[i].used && frame_num >= g_waiters[i].target_frame) {
            JSValue res_fn = g_waiters[i].resolve;
            g_waiters[i].used = 0;
            g_waiters[i].resolve = JS_UNDEFINED;
            JSValue fv = JS_NewInt64(ctx, frame_num);
            JSValue r = JS_Call(ctx, res_fn, JS_UNDEFINED, 1, &fv);
            JS_FreeValue(ctx, fv);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, res_fn);
        }
    }
    const int64_t now = hos_now_ns();
    for (int i = 0; i < TIMER_MAX; i++) {
        if (!g_timers[i].used || now < g_timers[i].next_ns) continue;
        JSValue fn = JS_DupValue(ctx, g_timers[i].fn);
        if (g_timers[i].interval) {
            g_timers[i].next_ns = now + g_timers[i].period_ns;
        } else {
            JS_FreeValue(ctx, g_timers[i].fn);
            memset(&g_timers[i], 0, sizeof g_timers[i]);
        }
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) js_log_exception(ctx, "timer");
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
    }
}

/* ---- Frame Hook ---- */
void hos_js_on_frame(int64_t frame_num, double dt_sec) {
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&g_io_lock);
    g_frame_ts[g_frame_ts_n++ % 32] = now;
    if (g_frame_ts_n >= 2) {
        const unsigned count = g_frame_ts_n < 32 ? g_frame_ts_n : 32;
        const int64_t oldest = g_frame_ts[(g_frame_ts_n - count) % 32];
        const double span = (double)(now - oldest) / 1e9;
        if (span > 0.0) {
            float fps = (float)((double)(count - 1) / span);
            uint32_t b = 0;
            memcpy(&b, &fps, 4);
            __atomic_store_n(&g_fps_bits, (LONG)b, __ATOMIC_RELAXED);
        }
    }
    g_last_frame_num = frame_num;
    ReleaseSRWLockExclusive(&g_io_lock);

    if (!hos_js_is_ready()) return;
    if (!js_enter()) return;

    js_ui_pump_events(g_js_ctx);
    if (!hos_js_is_ready()) { js_leave(); return; }

    js_io_tick_timers(g_js_ctx, frame_num);
    js_ui_begin_frame();

    if (js_has_event_listeners("frame")) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "frame", JS_NewInt64(g_js_ctx, frame_num));
        JS_SetPropertyStr(g_js_ctx, ev, "dt", JS_NewFloat64(g_js_ctx, dt_sec));
        JS_SetPropertyStr(g_js_ctx, ev, "fps", JS_NewFloat64(g_js_ctx, (double)load_f32(&g_fps_bits)));
        js_emit_event(g_js_ctx, "frame", 1, &ev);
        JS_FreeValue(g_js_ctx, ev);
    }

    js_drain_jobs(g_js_ctx);
    js_ui_end_frame();
    js_leave();
}

/* ---- game.input ---- */
static uint64_t g_in_buttons = 0, g_in_prev = 0, g_in_raw = 0;
static int32_t g_in_stick[4] = {0};
static int g_ov_active = 0, g_ov_block_game = 0;
static uint64_t g_ov_buttons = 0, g_ov_mask = 0;
static int32_t g_ov_stick[4] = {0};
static uint32_t g_ov_stick_mask = 0;

void hos_js_on_input(uint64_t *buttons, int32_t stick[4], uint64_t raw_buttons, const int32_t raw_stick[4]) {
    AcquireSRWLockExclusive(&g_io_lock);
    g_in_prev = g_in_buttons;
    g_in_buttons = *buttons;
    g_in_raw = raw_buttons;
    memcpy(g_in_stick, stick, 4 * sizeof(int32_t));
    const int ov_on = g_ov_active, ov_block = g_ov_block_game;
    const uint64_t ov_b = g_ov_buttons, ov_m = g_ov_mask;
    const uint32_t ov_sm = g_ov_stick_mask;
    int32_t ov_s[4];
    memcpy(ov_s, g_ov_stick, sizeof ov_s);
    ReleaseSRWLockExclusive(&g_io_lock);

    if (ov_block) {
        *buttons = 0;
        memset(stick, 0, 4 * sizeof(int32_t));
    }
    if (ov_on) {
        *buttons = (*buttons & ~ov_m) | (ov_b & ov_m);
        for (int i = 0; i < 4; i++)
            if (ov_sm & (1u << i)) stick[i] = ov_s[i];
    }

    if (js_has_event_listeners("input") && js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "buttons", JS_NewBigUint64(g_js_ctx, *buttons));
        JS_SetPropertyStr(g_js_ctx, ev, "rawButtons", JS_NewBigUint64(g_js_ctx, raw_buttons));
        JS_SetPropertyStr(g_js_ctx, ev, "lx", JS_NewInt32(g_js_ctx, stick[0]));
        JS_SetPropertyStr(g_js_ctx, ev, "ly", JS_NewInt32(g_js_ctx, stick[1]));
        JS_SetPropertyStr(g_js_ctx, ev, "rx", JS_NewInt32(g_js_ctx, stick[2]));
        JS_SetPropertyStr(g_js_ctx, ev, "ry", JS_NewInt32(g_js_ctx, stick[3]));
        JS_SetPropertyStr(g_js_ctx, ev, "blockGameInput", JS_NewBool(g_js_ctx, ov_block));
        js_fire_event_callbacks(g_js_ctx, "input", 1, &ev);

        JSValue bv = JS_GetPropertyStr(g_js_ctx, ev, "blockGameInput");
        if (JS_ToBool(g_js_ctx, bv)) {
            *buttons = 0;
            memset(stick, 0, 4 * sizeof(int32_t));
        }
        JS_FreeValue(g_js_ctx, bv);

        JSValue btv = JS_GetPropertyStr(g_js_ctx, ev, "buttons");
        uint64_t nb = *buttons;
        if (js_to_u64(g_js_ctx, btv, &nb) == 0) *buttons = nb;
        JS_FreeValue(g_js_ctx, btv);

        static const char *const snames[4] = {"lx", "ly", "rx", "ry"};
        for (int i = 0; i < 4; i++) {
            JSValue sv = JS_GetPropertyStr(g_js_ctx, ev, snames[i]);
            int32_t iv = 0;
            if (JS_IsNumber(sv) && JS_ToInt32(g_js_ctx, &iv, sv) == 0) {
                if (iv < -32767) iv = -32767;
                if (iv > 32767) iv = 32767;
                stick[i] = iv;
            }
            JS_FreeValue(g_js_ctx, sv);
        }
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }

    (void)raw_stick;
}

static uint64_t parse_btn_mask(JSContext *ctx, JSValueConst val) {
    if (JS_IsString(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return 0;
        static const struct { const char *n; int b; } kBtns[] = {
            {"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3}, {"LS", 4}, {"RS", 5}, {"L", 6}, {"R", 7},
            {"ZL", 8}, {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"LEFT", 12}, {"UP", 13}, {"RIGHT", 14}, {"DOWN", 15},
        };
        uint64_t m = 0;
        for (size_t i = 0; i < sizeof kBtns / sizeof kBtns[0]; i++)
            if (!_stricmp(s, kBtns[i].n)) { m = 1ULL << kBtns[i].b; break; }
        JS_FreeCString(ctx, s);
        return m;
    }
    uint64_t m = 0;
    js_to_u64(ctx, val, &m);
    return m;
}

static JSValue js_input_getState(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_io_lock);
    const uint64_t cur = g_in_buttons, prev = g_in_prev, raw = g_in_raw;
    const int32_t s0 = g_in_stick[0], s1 = g_in_stick[1], s2 = g_in_stick[2], s3 = g_in_stick[3];
    ReleaseSRWLockShared(&g_io_lock);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "buttons", JS_NewUint32(ctx, (uint32_t)cur));
    JS_SetPropertyStr(ctx, o, "rawButtons", JS_NewUint32(ctx, (uint32_t)raw));
    JS_SetPropertyStr(ctx, o, "justPressed", JS_NewUint32(ctx, (uint32_t)(cur & ~prev)));
    JS_SetPropertyStr(ctx, o, "justReleased", JS_NewUint32(ctx, (uint32_t)(~cur & prev)));
    JS_SetPropertyStr(ctx, o, "lx", JS_NewInt32(ctx, s0));
    JS_SetPropertyStr(ctx, o, "ly", JS_NewInt32(ctx, s1));
    JS_SetPropertyStr(ctx, o, "rx", JS_NewInt32(ctx, s2));
    JS_SetPropertyStr(ctx, o, "ry", JS_NewInt32(ctx, s3));
    return o;
}
static JSValue js_input_isDown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const uint64_t m = parse_btn_mask(ctx, argv[0]);
    AcquireSRWLockShared(&g_io_lock);
    const uint64_t b = g_in_buttons;
    ReleaseSRWLockShared(&g_io_lock);
    return JS_NewBool(ctx, m && (b & m) == m);
}
static JSValue js_input_justPressed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const uint64_t m = parse_btn_mask(ctx, argv[0]);
    AcquireSRWLockShared(&g_io_lock);
    const uint64_t jp = g_in_buttons & ~g_in_prev;
    ReleaseSRWLockShared(&g_io_lock);
    return JS_NewBool(ctx, (jp & m) != 0);
}
static JSValue js_input_justReleased(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const uint64_t m = parse_btn_mask(ctx, argv[0]);
    AcquireSRWLockShared(&g_io_lock);
    const uint64_t jr = ~g_in_buttons & g_in_prev;
    ReleaseSRWLockShared(&g_io_lock);
    return JS_NewBool(ctx, (jr & m) != 0);
}

static int parse_vk(JSContext *ctx, JSValueConst val) {
    if (JS_IsNumber(val)) {
        int32_t v = 0;
        JS_ToInt32(ctx, &v, val);
        return v;
    }
    const char *s = JS_ToCString(ctx, val);
    if (!s) return 0;
    int vk = 0;
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) {
        vk = toupper((unsigned char)s[0]);
    } else if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int fn = atoi(s + 1);
        if (fn >= 1 && fn <= 12) vk = VK_F1 + fn - 1;
    } else {
        static const struct { const char *n; int vk; } kKeys[] = {
            {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Return", VK_RETURN},
            {"Shift", VK_SHIFT}, {"Ctrl", VK_CONTROL}, {"Control", VK_CONTROL},
            {"Alt", VK_MENU}, {"Esc", VK_ESCAPE}, {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
            {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
            {"Backspace", VK_BACK}, {"Insert", VK_INSERT}, {"Delete", VK_DELETE},
            {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
            {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
            {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        };
        for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
            if (!_stricmp(s, kKeys[i].n)) { vk = kKeys[i].vk; break; }
    }
    JS_FreeCString(ctx, s);
    return vk;
}

static JSValue js_input_isKeyDown(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const int vk = parse_vk(ctx, argv[0]);
    if (vk <= 0 || vk >= 256) return JS_FALSE;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return JS_FALSE;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return JS_FALSE;
    return JS_NewBool(ctx, (GetAsyncKeyState(vk) & 0x8000) != 0);
}

static volatile LONG g_overlay_w = 0, g_overlay_h = 0, g_overlay_ui_mouse = 0;

void hos_js_set_overlay_state(int width, int height, int ui_wants_mouse) {
    InterlockedExchange(&g_overlay_w, width > 0 ? width : 0);
    InterlockedExchange(&g_overlay_h, height > 0 ? height : 0);
    InterlockedExchange(&g_overlay_ui_mouse, ui_wants_mouse ? 1 : 0);
}

void js_overlay_size(int *width, int *height) {
    int w = (int)InterlockedCompareExchange(&g_overlay_w, 0, 0);
    int h = (int)InterlockedCompareExchange(&g_overlay_h, 0, 0);
    if (w <= 0 || h <= 0) {
        int64_t r = 0, d = 0;
        w = 1280, h = 720;
        if (!rx_control_frame_info(&r, &d, &w, &h) || w <= 0 || h <= 0) w = 1280, h = 720;
    }
    *width = w;
    *height = h;
}

static JSValue js_input_getMouse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    int64_t r = 0, d = 0;
    int w = 1280, h = 720;
    rx_control_frame_info(&r, &d, &w, &h);
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    int cw = 0, ch = 0;
    if (fg) {
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            ScreenToClient(fg, &pt);
            RECT rc;
            if (GetClientRect(fg, &rc)) cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        }
    }
    /* Map client pixels into ui.draw pixels the same way the overlay maps the pointer for Dear ImGui. */
    int ow = 0, oh = 0;
    js_overlay_size(&ow, &oh);
    const double ox = cw > 0 ? (double)pt.x * ow / cw : pt.x;
    const double oy = ch > 0 ? (double)pt.y * oh / ch : pt.y;
    uint32_t btns = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) |
                    ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2u : 0u) |
                    ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4u : 0u);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewInt32(ctx, pt.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewInt32(ctx, pt.y));
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, o, "buttons", JS_NewUint32(ctx, btns));
    JS_SetPropertyStr(ctx, o, "overlayX", JS_NewFloat64(ctx, ox));
    JS_SetPropertyStr(ctx, o, "overlayY", JS_NewFloat64(ctx, oy));
    JS_SetPropertyStr(ctx, o, "overlayWidth", JS_NewInt32(ctx, ow));
    JS_SetPropertyStr(ctx, o, "overlayHeight", JS_NewInt32(ctx, oh));
    JS_SetPropertyStr(ctx, o, "overUi", JS_NewBool(ctx, InterlockedCompareExchange(&g_overlay_ui_mouse, 0, 0) != 0));
    return o;
}

static JSValue js_input_setOverride(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        AcquireSRWLockExclusive(&g_io_lock);
        g_ov_active = 0;
        g_ov_mask = 0;
        g_ov_stick_mask = 0;
        ReleaseSRWLockExclusive(&g_io_lock);
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "setOverride(obj | null)");
    uint64_t b = 0, mask = 0xFFFFULL;
    int block = g_ov_block_game;
    int32_t s[4] = {0};
    uint32_t sm = 0;
    JSValue bv = JS_GetPropertyStr(ctx, argv[0], "buttons");
    JSValue mv = JS_GetPropertyStr(ctx, argv[0], "mask");
    JSValue kv = JS_GetPropertyStr(ctx, argv[0], "blockGameInput");
    if (!JS_IsUndefined(bv)) b = parse_btn_mask(ctx, bv);
    if (!JS_IsUndefined(mv)) js_to_u64(ctx, mv, &mask);
    if (!JS_IsUndefined(kv)) block = JS_ToBool(ctx, kv) ? 1 : 0;
    JS_FreeValue(ctx, bv);
    JS_FreeValue(ctx, mv);
    JS_FreeValue(ctx, kv);
    static const char *const snames[4] = {"lx", "ly", "rx", "ry"};
    for (int i = 0; i < 4; i++) {
        JSValue sv = JS_GetPropertyStr(ctx, argv[0], snames[i]);
        int32_t iv = 0;
        if (JS_IsNumber(sv) && JS_ToInt32(ctx, &iv, sv) == 0) {
            if (iv < -32767) iv = -32767;
            if (iv > 32767) iv = 32767;
            s[i] = iv;
            sm |= 1u << i;
        }
        JS_FreeValue(ctx, sv);
    }
    AcquireSRWLockExclusive(&g_io_lock);
    g_ov_active = 1;
    g_ov_buttons = b;
    g_ov_mask = mask;
    g_ov_block_game = block;
    memcpy(g_ov_stick, s, sizeof s);
    g_ov_stick_mask = sm;
    ReleaseSRWLockExclusive(&g_io_lock);
    return JS_UNDEFINED;
}

static JSValue js_input_clearOverride(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_io_lock);
    g_ov_active = 0;
    g_ov_block_game = 0;
    g_ov_mask = 0;
    g_ov_stick_mask = 0;
    ReleaseSRWLockExclusive(&g_io_lock);
    return JS_UNDEFINED;
}

static JSValue js_input_setBlockGame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const int blk = JS_ToBool(ctx, argv[0]) ? 1 : 0;
    AcquireSRWLockExclusive(&g_io_lock);
    g_ov_block_game = blk;
    ReleaseSRWLockExclusive(&g_io_lock);
    return JS_UNDEFINED;
}

/* ---- game.audio ---- */
static JSValue js_audio_getMusic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewString(ctx, js_current_bgm());
}
static JSValue js_audio_setVol(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setVolumeScale(0..4)");
    if (d < 0.0) d = 0.0;
    if (d > 4.0) d = 4.0;
    store_f32(&g_audio_vol_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_audio_getVol(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_audio_volume_scale());
}
static JSValue js_audio_setPitch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setPitchScale(0.25..4)");
    if (d < 0.25) d = 0.25;
    if (d > 4.0) d = 4.0;
    store_f32(&g_audio_pit_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_audio_getPitch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_audio_pitch_scale());
}
static JSValue js_audio_setMute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    __atomic_store_n(&g_audio_muted, JS_ToBool(ctx, argv[0]) ? 1 : 0, __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}
static JSValue js_audio_isMuted(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewBool(ctx, hos_js_audio_muted() != 0);
}

/* ---- game.graphics ---- */
static JSValue js_gfx_getFrame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
#ifdef LONGSHOT_ASI
    return JS_NewInt64(ctx, hos_js_asi_present_count());
#else
    return JS_NewInt64(ctx, __atomic_load_n(&hos_present_count, __ATOMIC_RELAXED));
#endif
}
static JSValue js_gfx_getFps(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)load_f32(&g_fps_bits));
}
static JSValue js_gfx_getWindowSize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    int64_t r = 0, d = 0; int w = 0, h = 0;
    rx_control_frame_info(&r, &d, &w, &h);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    return o;
}
static JSValue setting_to_js(JSContext *ctx, const HosControlSetting *s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "key", JS_NewString(ctx, s->key));
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, s->name));
    JS_SetPropertyStr(ctx, o, "text", JS_NewString(ctx, s->text));
    JS_SetPropertyStr(ctx, o, "tab", JS_NewString(ctx, s->tab));
    JS_SetPropertyStr(ctx, o, "value", JS_NewInt32(ctx, s->value));
    JS_SetPropertyStr(ctx, o, "min", JS_NewInt32(ctx, s->lo));
    JS_SetPropertyStr(ctx, o, "max", JS_NewInt32(ctx, s->hi));
    JS_SetPropertyStr(ctx, o, "step", JS_NewInt32(ctx, s->step));
    return o;
}
static JSValue js_gfx_getOption(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    if (JS_IsNumber(argv[0])) {
        int32_t id = 0;
        JS_ToInt32(ctx, &id, argv[0]);
        return JS_NewInt32(ctx, rx_option(id, -1));
    }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    HosControlSetting s;
    for (int i = 0; rx_control_setting(i, &s); i++) {
        if (!strcmp(s.key, key)) {
            JS_FreeCString(ctx, key);
            return JS_NewInt32(ctx, s.value);
        }
    }
    JS_FreeCString(ctx, key);
    return JS_NULL;
}
static JSValue js_gfx_setOption(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_FALSE;
    int32_t val = 0;
    if (JS_ToInt32(ctx, &val, argv[1]) < 0) return JS_EXCEPTION;
    if (JS_IsNumber(argv[0])) {
        int32_t id = 0;
        JS_ToInt32(ctx, &id, argv[0]);
        rx_option_set(id, val);
        return JS_TRUE;
    }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    HosControlSetting out;
    const int r = rx_control_setting_set(key, val, &out);
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, r == 1);
}
static JSValue js_gfx_listOptions(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSValue arr = JS_NewArray(ctx);
    HosControlSetting s;
    for (int i = 0; rx_control_setting(i, &s); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, setting_to_js(ctx, &s));
    return arr;
}
static JSValue js_gfx_setTextSpeed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_EXCEPTION;
    if (d < 0.1) d = 0.1;
    if (d > 100.0) d = 100.0;
    store_f32(&g_text_speed_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_gfx_getTextSpeed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_text_speed_scale());
}
static JSValue js_gfx_setTextSize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_EXCEPTION;
    if (d < 0.25) d = 0.25;
    if (d > 4.0) d = 4.0;
    store_f32(&g_text_size_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_gfx_getTextSize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_text_size_scale());
}
static JSValue js_gfx_setResScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t pct = 100;
    if (argc < 1 || JS_ToInt32(ctx, &pct, argv[0]) < 0) return JS_FALSE;
    HosControlSetting out;
    return JS_NewBool(ctx, rx_control_setting_set("resolution_scale", pct, &out) == 1);
}

static int resolve_sandbox_path(const char *rel, char *out, size_t cap);

static JSValue js_gfx_takeShot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (g_shot_wait.used) return JS_ThrowPlainError(ctx, "another screenshot is already pending");
    char name[128] = "screenshots/shot.png";
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s && *s) {
            if (strstr(s, "/") || strstr(s, "\\")) snprintf(name, sizeof name, "%s", s);
            else snprintf(name, sizeof name, "screenshots/%s", s);
        }
        if (s) JS_FreeCString(ctx, s);
    }
    char host[MAX_PATH];
    if (!resolve_sandbox_path(name, host, sizeof host))
        return JS_ThrowRangeError(ctx, "invalid screenshot path");
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", host);
    for (char *p = dir; *p; p++)
        if (*p == '\\' || *p == '/') { char ch = *p; *p = 0; CreateDirectoryA(dir, NULL); *p = ch; }
    JSValue rr[2];
    JSValue p = JS_NewPromiseCapability(ctx, rr);
    if (JS_IsException(p)) return p;
    g_shot_wait.used = 1;
    __atomic_store_n(&g_shot_wait.done, 0, __ATOMIC_RELEASE);
    snprintf(g_shot_wait.path, sizeof g_shot_wait.path, "%s", host);
    g_shot_wait.resolve = rr[0];
    if (!rx_control_shot(host, -1, on_shot_done, NULL)) {
        g_shot_wait.used = 0;
        g_shot_wait.resolve = JS_UNDEFINED;
        JSValue r = JS_Call(ctx, rr[1], JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, rr[0]);
        JS_FreeValue(ctx, rr[1]);
        return p;
    }
    JS_FreeValue(ctx, rr[1]);
    return p;
}

/* ---- game.romfs (virtual files + host redirects) ---- */
typedef struct {
    int used;
    char key[MAX_PATH];   /* lower-case, '/' separated, no leading 'rom:/' */
    char host[MAX_PATH];
    uint8_t *vbuf;
    uint64_t vlen;
} RomOverride;
#define ROM_OV_MAX 128
static RomOverride g_rom_ovs[ROM_OV_MAX];
static SRWLOCK g_rom_lock = SRWLOCK_INIT;

static void norm_rom_key(const char *in, char *out, size_t cap) {
    if (!in) { if (cap) out[0] = 0; return; }
    if (!_strnicmp(in, "rom:/", 5)) in += 5;
    else if (!_strnicmp(in, "rom:", 4)) in += 4;
    while (*in == '/' || *in == '\\') in++;
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++)
        out[i] = (in[i] == '\\') ? '/' : (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

int hos_js_romfs_redirect(const char *rel, char *out, size_t cap) {
    if (!rel || !*rel) return 0;
    char key[MAX_PATH];
    norm_rom_key(rel, key, sizeof key);
    int found = 0;
    AcquireSRWLockShared(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && !g_rom_ovs[i].vbuf && !strcmp(g_rom_ovs[i].key, key)) {
            snprintf(out, cap, "%s", g_rom_ovs[i].host);
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_rom_lock);
    return found;
}

int hos_js_romfs_virtual_copy(const char *rel, uint8_t **out_buf, uint64_t *out_len) {
    if (!rel || !*rel || !out_buf || !out_len) return 0;
    char key[MAX_PATH];
    norm_rom_key(rel, key, sizeof key);
    int found = 0;
    AcquireSRWLockShared(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && g_rom_ovs[i].vbuf && !strcmp(g_rom_ovs[i].key, key)) {
            uint8_t *cp = (uint8_t *)malloc((size_t)(g_rom_ovs[i].vlen ? g_rom_ovs[i].vlen : 1));
            if (cp) {
                if (g_rom_ovs[i].vlen) memcpy(cp, g_rom_ovs[i].vbuf, (size_t)g_rom_ovs[i].vlen);
                *out_buf = cp;
                *out_len = g_rom_ovs[i].vlen;
                found = 1;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&g_rom_lock);
    return found;
}

int hos_js_romfs_virtual_exists(const char *rel, uint64_t *out_len) {
    if (!rel || !*rel) return 0;
    char key[MAX_PATH];
    norm_rom_key(rel, key, sizeof key);
    int found = 0;
    AcquireSRWLockShared(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && g_rom_ovs[i].vbuf && !strcmp(g_rom_ovs[i].key, key)) {
            if (out_len) *out_len = g_rom_ovs[i].vlen;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_rom_lock);
    return found;
}

static JSValue js_romfs_redirect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "romfs.redirect(romPath, hostFilePath)");
    const char *rp = JS_ToCString(ctx, argv[0]);
    const char *hp = JS_ToCString(ctx, argv[1]);
    if (!rp || !hp) {
        if (rp) JS_FreeCString(ctx, rp);
        if (hp) JS_FreeCString(ctx, hp);
        return JS_EXCEPTION;
    }
    char key[MAX_PATH];
    norm_rom_key(rp, key, sizeof key);
    AcquireSRWLockExclusive(&g_rom_lock);
    int slot = -1;
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && !strcmp(g_rom_ovs[i].key, key)) { slot = i; break; }
        if (!g_rom_ovs[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {
        free(g_rom_ovs[slot].vbuf);
        memset(&g_rom_ovs[slot], 0, sizeof g_rom_ovs[slot]);
        g_rom_ovs[slot].used = 1;
        snprintf(g_rom_ovs[slot].key, sizeof g_rom_ovs[slot].key, "%s", key);
        snprintf(g_rom_ovs[slot].host, sizeof g_rom_ovs[slot].host, "%s", hp);
    }
    ReleaseSRWLockExclusive(&g_rom_lock);
    JS_FreeCString(ctx, rp);
    JS_FreeCString(ctx, hp);
    return JS_NewBool(ctx, slot >= 0);
}

static JSValue js_romfs_setVirtual(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "romfs.setVirtualFile(romPath, data)");
    const char *rp = JS_ToCString(ctx, argv[0]);
    if (!rp) return JS_EXCEPTION;
    char key[MAX_PATH];
    norm_rom_key(rp, key, sizeof key);
    JS_FreeCString(ctx, rp);

    uint8_t *copy = NULL;
    size_t len = 0;
    if (JS_IsString(argv[1])) {
        const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
        if (!s) return JS_EXCEPTION;
        copy = (uint8_t *)malloc(len ? len : 1);
        if (copy && len) memcpy(copy, s, len);
        JS_FreeCString(ctx, s);
    } else {
        uint8_t *p = JS_GetUint8Array(ctx, &len, argv[1]);
        if (!p) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            p = JS_GetArrayBuffer(ctx, &len, argv[1]);
        }
        if (!p) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "setVirtualFile expects string, Uint8Array, or ArrayBuffer");
        }
        copy = (uint8_t *)malloc(len ? len : 1);
        if (copy && len) memcpy(copy, p, len);
    }
    if (!copy) return JS_ThrowOutOfMemory(ctx);

    AcquireSRWLockExclusive(&g_rom_lock);
    int slot = -1;
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && !strcmp(g_rom_ovs[i].key, key)) { slot = i; break; }
        if (!g_rom_ovs[i].used && slot < 0) slot = i;
    }
    if (slot >= 0) {
        free(g_rom_ovs[slot].vbuf);
        memset(&g_rom_ovs[slot], 0, sizeof g_rom_ovs[slot]);
        g_rom_ovs[slot].used = 1;
        snprintf(g_rom_ovs[slot].key, sizeof g_rom_ovs[slot].key, "%s", key);
        g_rom_ovs[slot].vbuf = copy;
        g_rom_ovs[slot].vlen = (uint64_t)len;
    } else {
        free(copy);
    }
    ReleaseSRWLockExclusive(&g_rom_lock);
    return JS_NewBool(ctx, slot >= 0);
}

static JSValue js_romfs_clearOverride(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *rp = JS_ToCString(ctx, argv[0]);
    if (!rp) return JS_EXCEPTION;
    char key[MAX_PATH];
    norm_rom_key(rp, key, sizeof key);
    JS_FreeCString(ctx, rp);
    int removed = 0;
    AcquireSRWLockExclusive(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        if (g_rom_ovs[i].used && !strcmp(g_rom_ovs[i].key, key)) {
            free(g_rom_ovs[i].vbuf);
            memset(&g_rom_ovs[i], 0, sizeof g_rom_ovs[i]);
            removed = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_rom_lock);
    return JS_NewBool(ctx, removed);
}

static JSValue js_romfs_clearAll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        free(g_rom_ovs[i].vbuf);
        memset(&g_rom_ovs[i], 0, sizeof g_rom_ovs[i]);
    }
    ReleaseSRWLockExclusive(&g_rom_lock);
    return JS_UNDEFINED;
}

static int resolve_romfs_host(const char *rp, char *out, size_t cap) {
    char key[MAX_PATH];
    norm_rom_key(rp, key, sizeof key);
    if (hos_js_romfs_redirect(key, out, cap)) return 1;
    char guest[MAX_PATH];
    if (snprintf(guest, sizeof guest, "content:/%s", key) >= (int)sizeof guest) return 0;
    if (hos_fs_host_path(guest, out, cap)) return 1;
    snprintf(guest, sizeof guest, "rom:/%s", key);
    return hos_fs_host_path(guest, out, cap);
}

static JSValue js_romfs_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *rp = JS_ToCString(ctx, argv[0]);
    if (!rp) return JS_EXCEPTION;
    int ok = hos_js_romfs_virtual_exists(rp, NULL);
    if (!ok) {
        char hp[MAX_PATH];
        ok = resolve_romfs_host(rp, hp, sizeof hp) && GetFileAttributesA(hp) != INVALID_FILE_ATTRIBUTES;
    }
    JS_FreeCString(ctx, rp);
    return JS_NewBool(ctx, ok);
}

static JSValue js_romfs_readFile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *rp = JS_ToCString(ctx, argv[0]);
    if (!rp) return JS_EXCEPTION;
    uint8_t *vbuf = NULL;
    uint64_t vlen = 0;
    if (hos_js_romfs_virtual_copy(rp, &vbuf, &vlen)) {
        JS_FreeCString(ctx, rp);
        JSValue arr = JS_NewUint8ArrayCopy(ctx, vbuf, (size_t)vlen);
        free(vbuf);
        return arr;
    }
    char hp[MAX_PATH];
    if (!resolve_romfs_host(rp, hp, sizeof hp)) {
        JS_FreeCString(ctx, rp);
        return JS_NULL;
    }
    JS_FreeCString(ctx, rp);
    FILE *f = fopen(hp, "rb");
    if (!f) return JS_NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 64L * 1024L * 1024L) { fclose(f); return JS_NULL; }
    uint8_t *buf = sz ? (uint8_t *)malloc((size_t)sz) : NULL;
    size_t got = sz && buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (sz && !buf) return JS_ThrowOutOfMemory(ctx);
    if (got != (size_t)sz) { free(buf); return JS_NULL; }
    JSValue arr = JS_NewUint8ArrayCopy(ctx, buf ? buf : (const uint8_t *)"", got);
    free(buf);
    return arr;
}

static JSValue js_romfs_decompress(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (!argc) return JS_ThrowTypeError(ctx, "decompressZstd(bytes, dictionary?)");
    size_t size = 0, dict_size = 0;
    const uint8_t *src = JS_GetUint8Array(ctx, &size, argv[0]), *dict = NULL;
    if (!src) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        dict = JS_GetUint8Array(ctx, &dict_size, argv[1]);
        if (!dict) return JS_EXCEPTION;
    }
    const uint64_t cap = js_zstd_size(src, size);
    if (cap > 32u * 1024 * 1024) return JS_ThrowRangeError(ctx, "invalid or unknown Zstd size, or above 32 MiB");
    uint8_t *dst = js_malloc(ctx, cap ? (size_t)cap : 1);
    if (!dst) return JS_EXCEPTION;
    int ok = js_zstd_decode(dst, (size_t)cap, src, size, dict, dict_size);
    JSValue result = ok ? JS_NewUint8ArrayCopy(ctx, dst, (size_t)cap)
                        : JS_ThrowTypeError(ctx, "invalid Zstd frame or dictionary");
    js_free(ctx, dst);
    return result;
}

static JSValue js_romfs_listDir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *rp = argc >= 1 ? JS_ToCString(ctx, argv[0]) : NULL;
    char hp[MAX_PATH];
    JSValue arr = JS_NewArray(ctx);
    uint32_t out_n = 0;
    if (resolve_romfs_host(rp ? rp : "", hp, sizeof hp)) {
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s\\*", hp);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                JS_SetPropertyUint32(ctx, arr, out_n++, JS_NewString(ctx, fd.cFileName));
            } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
    }
    if (rp) JS_FreeCString(ctx, rp);
    return arr;
}

/* ---- Sandboxed `fs` (<game_dir>/scripts_data/) ---- */
static int resolve_sandbox_path(const char *rel, char *out, size_t cap) {
    if (!rel || !*rel) return 0;
    if (rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) return 0;
    for (const char *p = rel; *p;) {
        const char *sep = strpbrk(p, "/\\");
        const size_t n = sep ? (size_t)(sep - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)p[i] < 0x20 || p[i] == ':' || p[i] == '*' || p[i] == '?' || p[i] == '"') return 0;
        if (!sep) break;
        p = sep + 1;
    }
    const char *root = js_data_dir();
    if (!root || !*root) return 0;
    char canon_root[MAX_PATH], combined[MAX_PATH * 2], full[MAX_PATH];
    if (!GetFullPathNameA(root, sizeof canon_root, canon_root, NULL)) return 0;
    size_t rlen = strlen(canon_root);
    while (rlen && (canon_root[rlen - 1] == '\\' || canon_root[rlen - 1] == '/')) canon_root[--rlen] = 0;
    snprintf(combined, sizeof combined, "%s\\%s", canon_root, rel);
    for (char *p = combined; *p; p++)
        if (*p == '/') *p = '\\';
    if (!GetFullPathNameA(combined, sizeof full, full, NULL)) return 0;
    if (_strnicmp(full, canon_root, rlen) != 0) return 0;
    if (full[rlen] != '\\') return 0;
    if (strlen(full) + 1 > cap) return 0;
    strcpy(out, full);
    return 1;
}

static void ensure_parent_dirs(const char *path) {
    const char *root = js_data_dir();
    CreateDirectoryA(root, NULL);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + strlen(root); *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
}

static JSValue fs_bad_path(JSContext *ctx) {
    return JS_ThrowRangeError(ctx, "fs path rejected (must be relative inside scripts_data/ with no .. or drive)");
}

static JSValue js_fs_readText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    FILE *f = fopen(host, "rb");
    if (!f) return JS_NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 16L * 1024L * 1024L) { fclose(f); return JS_NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t got = sz && buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    buf[got] = 0;
    JSValue s = JS_NewStringLen(ctx, buf, got);
    free(buf);
    return s;
}

static JSValue js_fs_writeText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    size_t len = 0;
    const char *txt = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!txt) return JS_EXCEPTION;
    ensure_parent_dirs(host);
    FILE *f = fopen(host, "wb");
    int written = 0;
    if (f) {
        written = fwrite(txt, 1, len, f) == len;
        if (fclose(f) != 0) written = 0;
    }
    JS_FreeCString(ctx, txt);
    return JS_NewBool(ctx, written);
}

static JSValue js_fs_readBytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    FILE *f = fopen(host, "rb");
    if (!f) return JS_NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 32L * 1024L * 1024L) { fclose(f); return JS_NULL; }
    uint8_t *buf = sz ? (uint8_t *)malloc((size_t)sz) : NULL;
    size_t got = sz && buf ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    JSValue arr = JS_NewUint8ArrayCopy(ctx, buf ? buf : (const uint8_t *)"", got);
    free(buf);
    return arr;
}

static JSValue js_fs_writeBytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    size_t len = 0;
    uint8_t *p = JS_GetUint8Array(ctx, &len, argv[1]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        p = JS_GetArrayBuffer(ctx, &len, argv[1]);
    }
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "writeBytes expects Uint8Array or ArrayBuffer");
    }
    ensure_parent_dirs(host);
    FILE *f = fopen(host, "wb");
    if (f) {
        if (len) fwrite(p, 1, len, f);
        fclose(f);
    }
    return JS_NewBool(ctx, f != NULL);
}

static JSValue js_fs_readJson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue s = js_fs_readText(ctx, this_val, argc, argv);
    if (JS_IsNull(s) || JS_IsException(s)) return s;
    size_t len = 0;
    const char *raw = JS_ToCStringLen(ctx, &len, s);
    JS_FreeValue(ctx, s);
    if (!raw) return JS_NULL;
    JSValue parsed = JS_ParseJSON(ctx, raw, len, "scripts_data");
    JS_FreeCString(ctx, raw);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_NULL;
    }
    return parsed;
}

static JSValue js_fs_writeJson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return fs_bad_path(ctx);
    JSValue space = JS_NewInt32(ctx, 2);
    JSValue str = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, space);
    JS_FreeValue(ctx, space);
    if (JS_IsException(str)) return str;
    JSValue wargs[2] = {argv[0], str};
    JSValue r = js_fs_writeText(ctx, this_val, 2, wargs);
    JS_FreeValue(ctx, str);
    return r;
}

static JSValue js_fs_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    return JS_NewBool(ctx, GetFileAttributesA(host) != INVALID_FILE_ATTRIBUTES);
}

static JSValue js_fs_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    return JS_NewBool(ctx, DeleteFileA(host) != 0);
}

static JSValue js_fs_mkdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return fs_bad_path(ctx);
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel) return JS_EXCEPTION;
    char host[MAX_PATH];
    const int ok = resolve_sandbox_path(rel, host, sizeof host);
    JS_FreeCString(ctx, rel);
    if (!ok) return fs_bad_path(ctx);
    ensure_parent_dirs(host);
    return JS_NewBool(ctx, CreateDirectoryA(host, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

static JSValue js_fs_listDir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    char host[MAX_PATH];
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        snprintf(host, sizeof host, "%s", js_data_dir());
    } else {
        const char *rel = JS_ToCString(ctx, argv[0]);
        if (!rel) return JS_EXCEPTION;
        if (!*rel) {
            snprintf(host, sizeof host, "%s", js_data_dir());
            JS_FreeCString(ctx, rel);
        } else {
            const int ok = resolve_sandbox_path(rel, host, sizeof host);
            JS_FreeCString(ctx, rel);
            if (!ok) return fs_bad_path(ctx);
        }
    }
    JSValue arr = JS_NewArray(ctx);
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", host);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    uint32_t idx = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, fd.cFileName));
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    return arr;
}

void js_register_io(JSContext *ctx, JSValue ns_game, JSValue ns_fs, JSValue ns_events, JSValue global) {
    JS_SetPropertyStr(ctx, ns_events, "on", JS_NewCFunction(ctx, js_events_on, "on", 2));
    JS_SetPropertyStr(ctx, ns_events, "once", JS_NewCFunction(ctx, js_events_once, "once", 2));
    JS_SetPropertyStr(ctx, ns_events, "off", JS_NewCFunction(ctx, js_events_off, "off", 2));
    JS_SetPropertyStr(ctx, ns_events, "emit", JS_NewCFunction(ctx, js_events_emit, "emit", 1));

    static const char *const kOnNames[] = {
        "onFrame", "onPlayerUpdate", "onDamage", "onCameraUpdate", "onInput", "onEventRequest",
        "onTimelineStart", "onSaveRequest", "onMapChange", "onMenuTick", "onMenuFocus", "onBgmChange", "onUnload", "onActorUpdate"
    };
    for (int i = 0; i < (int)(sizeof kOnNames / sizeof kOnNames[0]); i++) {
        JS_SetPropertyStr(ctx, ns_game, kOnNames[i],
                          JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)game_on_helper, kOnNames[i], 1, JS_CFUNC_generic_magic, i));
    }
    JS_SetPropertyStr(ctx, ns_game, "defer", JS_NewCFunction(ctx, js_game_defer, "defer", 1));

    JSValue input = JS_NewObject(ctx);
    JSValue btns = JS_NewObject(ctx);
    static const struct { const char *k; uint32_t v; } kB[] = {
        {"A", 1u<<0}, {"B", 1u<<1}, {"X", 1u<<2}, {"Y", 1u<<3}, {"LS", 1u<<4}, {"RS", 1u<<5},
        {"L", 1u<<6}, {"R", 1u<<7}, {"ZL", 1u<<8}, {"ZR", 1u<<9}, {"PLUS", 1u<<10}, {"MINUS", 1u<<11},
        {"LEFT", 1u<<12}, {"UP", 1u<<13}, {"RIGHT", 1u<<14}, {"DOWN", 1u<<15},
    };
    for (size_t i = 0; i < sizeof kB / sizeof kB[0]; i++)
        JS_SetPropertyStr(ctx, btns, kB[i].k, JS_NewUint32(ctx, kB[i].v));
    JS_SetPropertyStr(ctx, input, "Buttons", btns);
    JS_SetPropertyStr(ctx, input, "getState", JS_NewCFunction(ctx, js_input_getState, "getState", 0));
    JS_SetPropertyStr(ctx, input, "isDown", JS_NewCFunction(ctx, js_input_isDown, "isDown", 1));
    JS_SetPropertyStr(ctx, input, "justPressed", JS_NewCFunction(ctx, js_input_justPressed, "justPressed", 1));
    JS_SetPropertyStr(ctx, input, "justReleased", JS_NewCFunction(ctx, js_input_justReleased, "justReleased", 1));
    JS_SetPropertyStr(ctx, input, "isKeyDown", JS_NewCFunction(ctx, js_input_isKeyDown, "isKeyDown", 1));
    JS_SetPropertyStr(ctx, input, "getMouse", JS_NewCFunction(ctx, js_input_getMouse, "getMouse", 0));
    JS_SetPropertyStr(ctx, input, "setOverride", JS_NewCFunction(ctx, js_input_setOverride, "setOverride", 1));
    JS_SetPropertyStr(ctx, input, "clearOverride", JS_NewCFunction(ctx, js_input_clearOverride, "clearOverride", 0));
    JS_SetPropertyStr(ctx, input, "setBlockGameInput", JS_NewCFunction(ctx, js_input_setBlockGame, "setBlockGameInput", 1));
    JS_SetPropertyStr(ctx, ns_game, "input", input);

    JSValue audio = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio, "getMusic", JS_NewCFunction(ctx, js_audio_getMusic, "getMusic", 0));
    JS_SetPropertyStr(ctx, audio, "setVolumeScale", JS_NewCFunction(ctx, js_audio_setVol, "setVolumeScale", 1));
    JS_SetPropertyStr(ctx, audio, "getVolumeScale", JS_NewCFunction(ctx, js_audio_getVol, "getVolumeScale", 0));
    JS_SetPropertyStr(ctx, audio, "setPitchScale", JS_NewCFunction(ctx, js_audio_setPitch, "setPitchScale", 1));
    JS_SetPropertyStr(ctx, audio, "getPitchScale", JS_NewCFunction(ctx, js_audio_getPitch, "getPitchScale", 0));
    JS_SetPropertyStr(ctx, audio, "setMute", JS_NewCFunction(ctx, js_audio_setMute, "setMute", 1));
    JS_SetPropertyStr(ctx, audio, "isMuted", JS_NewCFunction(ctx, js_audio_isMuted, "isMuted", 0));
    JS_SetPropertyStr(ctx, ns_game, "audio", audio);

    JSValue gfx = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gfx, "getFrame", JS_NewCFunction(ctx, js_gfx_getFrame, "getFrame", 0));
    JS_SetPropertyStr(ctx, gfx, "getFps", JS_NewCFunction(ctx, js_gfx_getFps, "getFps", 0));
    JS_SetPropertyStr(ctx, gfx, "getWindowSize", JS_NewCFunction(ctx, js_gfx_getWindowSize, "getWindowSize", 0));
    JS_SetPropertyStr(ctx, gfx, "getOption", JS_NewCFunction(ctx, js_gfx_getOption, "getOption", 1));
    JS_SetPropertyStr(ctx, gfx, "setOption", JS_NewCFunction(ctx, js_gfx_setOption, "setOption", 2));
    JS_SetPropertyStr(ctx, gfx, "listOptions", JS_NewCFunction(ctx, js_gfx_listOptions, "listOptions", 0));
    JS_SetPropertyStr(ctx, gfx, "setTextSpeedScale", JS_NewCFunction(ctx, js_gfx_setTextSpeed, "setTextSpeedScale", 1));
    JS_SetPropertyStr(ctx, gfx, "getTextSpeedScale", JS_NewCFunction(ctx, js_gfx_getTextSpeed, "getTextSpeedScale", 0));
    JS_SetPropertyStr(ctx, gfx, "setTextSizeScale", JS_NewCFunction(ctx, js_gfx_setTextSize, "setTextSizeScale", 1));
    JS_SetPropertyStr(ctx, gfx, "getTextSizeScale", JS_NewCFunction(ctx, js_gfx_getTextSize, "getTextSizeScale", 0));
    JS_SetPropertyStr(ctx, gfx, "setResolutionScale", JS_NewCFunction(ctx, js_gfx_setResScale, "setResolutionScale", 1));
    JS_SetPropertyStr(ctx, gfx, "takeScreenshot", JS_NewCFunction(ctx, js_gfx_takeShot, "takeScreenshot", 1));
    JS_SetPropertyStr(ctx, ns_game, "graphics", gfx);

    JSValue romfs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, romfs, "redirect", JS_NewCFunction(ctx, js_romfs_redirect, "redirect", 2));
    JS_SetPropertyStr(ctx, romfs, "setVirtualFile", JS_NewCFunction(ctx, js_romfs_setVirtual, "setVirtualFile", 2));
    JS_SetPropertyStr(ctx, romfs, "clearOverride", JS_NewCFunction(ctx, js_romfs_clearOverride, "clearOverride", 1));
    JS_SetPropertyStr(ctx, romfs, "clearAllOverrides", JS_NewCFunction(ctx, js_romfs_clearAll, "clearAllOverrides", 0));
    JS_SetPropertyStr(ctx, romfs, "exists", JS_NewCFunction(ctx, js_romfs_exists, "exists", 1));
    JS_SetPropertyStr(ctx, romfs, "decompressZstd", JS_NewCFunction(ctx, js_romfs_decompress, "decompressZstd", 2));
    JS_SetPropertyStr(ctx, romfs, "readFile", JS_NewCFunction(ctx, js_romfs_readFile, "readFile", 1));
    JS_SetPropertyStr(ctx, romfs, "listDir", JS_NewCFunction(ctx, js_romfs_listDir, "listDir", 1));
    JS_SetPropertyStr(ctx, ns_game, "romfs", romfs);

    JS_SetPropertyStr(ctx, ns_fs, "readText", JS_NewCFunction(ctx, js_fs_readText, "readText", 1));
    JS_SetPropertyStr(ctx, ns_fs, "writeText", JS_NewCFunction(ctx, js_fs_writeText, "writeText", 2));
    JS_SetPropertyStr(ctx, ns_fs, "readBytes", JS_NewCFunction(ctx, js_fs_readBytes, "readBytes", 1));
    JS_SetPropertyStr(ctx, ns_fs, "writeBytes", JS_NewCFunction(ctx, js_fs_writeBytes, "writeBytes", 2));
    JS_SetPropertyStr(ctx, ns_fs, "readJson", JS_NewCFunction(ctx, js_fs_readJson, "readJson", 1));
    JS_SetPropertyStr(ctx, ns_fs, "writeJson", JS_NewCFunction(ctx, js_fs_writeJson, "writeJson", 2));
    JS_SetPropertyStr(ctx, ns_fs, "exists", JS_NewCFunction(ctx, js_fs_exists, "exists", 1));
    JS_SetPropertyStr(ctx, ns_fs, "remove", JS_NewCFunction(ctx, js_fs_remove, "remove", 1));
    JS_SetPropertyStr(ctx, ns_fs, "mkdir", JS_NewCFunction(ctx, js_fs_mkdir, "mkdir", 1));
    JS_SetPropertyStr(ctx, ns_fs, "listDir", JS_NewCFunction(ctx, js_fs_listDir, "listDir", 1));

    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunction(ctx, js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_clearTimer, "clearInterval", 1));
    JS_SetPropertyStr(ctx, global, "nextFrame", JS_NewCFunction(ctx, js_nextFrame, "nextFrame", 0));
    JS_SetPropertyStr(ctx, global, "waitFrames", JS_NewCFunction(ctx, js_waitFrames, "waitFrames", 1));
    JS_SetPropertyStr(ctx, global, "sleep", JS_NewCFunction(ctx, js_sleep, "sleep", 1));
    JS_SetPropertyStr(ctx, global, "now", JS_NewCFunction(ctx, js_now_ms, "now", 0));
}

void js_io_reset(JSContext *ctx) {
    store_f32(&g_text_speed_bits, 1.0f);
    store_f32(&g_text_size_bits, 1.0f);
    store_f32(&g_audio_vol_bits, 1.0f);
    store_f32(&g_audio_pit_bits, 1.0f);
    __atomic_store_n(&g_audio_muted, 0, __ATOMIC_RELAXED);

    AcquireSRWLockExclusive(&g_io_lock);
    for (int i = 0; i < JS_EVENT_MAX; i++) {
        if (g_listeners[i].used && ctx) JS_FreeValue(ctx, g_listeners[i].fn);
        memset(&g_listeners[i], 0, sizeof g_listeners[i]);
    }
    recount_hot_flags();
    for (int i = 0; i < TIMER_MAX; i++) {
        if (g_timers[i].used && ctx) JS_FreeValue(ctx, g_timers[i].fn);
        memset(&g_timers[i], 0, sizeof g_timers[i]);
    }
    for (int i = 0; i < WAITER_MAX; i++) {
        if (g_waiters[i].used && ctx) JS_FreeValue(ctx, g_waiters[i].resolve);
        memset(&g_waiters[i], 0, sizeof g_waiters[i]);
    }
    if (g_shot_wait.used && ctx) JS_FreeValue(ctx, g_shot_wait.resolve);
    memset(&g_shot_wait, 0, sizeof g_shot_wait);
    g_ov_active = 0;
    g_ov_block_game = 0;
    g_ov_mask = 0;
    g_ov_stick_mask = 0;
    ReleaseSRWLockExclusive(&g_io_lock);

    AcquireSRWLockExclusive(&g_rom_lock);
    for (int i = 0; i < ROM_OV_MAX; i++) {
        free(g_rom_ovs[i].vbuf);
        memset(&g_rom_ovs[i], 0, sizeof g_rom_ovs[i]);
    }
    ReleaseSRWLockExclusive(&g_rom_lock);
}
