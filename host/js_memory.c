/* `memory` and `native` namespaces for Longshot JavaScript scripts (Safe R/W, AOB scan, Arena Alloc, Native Calls & Hooks). */
#include "js_internal.h"
#include <ctype.h>
#ifdef LONGSHOT_ASI
#include "../third_party/minhook/MinHook.h"
#endif

#define JS_ARENA_BYTES (4u * 1024u * 1024u)
#define JS_ARENA_MAX_BLOCKS 512
#define JS_HOOK_MAX 128
#define JS_CB_MAX 32
#define JS_CB_BASE 0x07F00000ULL

/* ---- 4 MB Persistent Guest Arena (reset cleanly on hot reload) ---- */
typedef struct {
    uint32_t off;
    uint32_t size;
    int used;
} ArenaBlk;

static uint64_t g_arena_base = 0;
static ArenaBlk g_arena_blks[JS_ARENA_MAX_BLOCKS];
static int g_arena_blk_n = 0;
static SRWLOCK g_arena_lock = SRWLOCK_INIT;

static void arena_reset(void) {
    AcquireSRWLockExclusive(&g_arena_lock);
    if (g_arena_base) {
        g_arena_blk_n = 1;
        g_arena_blks[0].off = 0;
        g_arena_blks[0].size = JS_ARENA_BYTES;
        g_arena_blks[0].used = 0;
    }
    ReleaseSRWLockExclusive(&g_arena_lock);
}

static uint64_t arena_alloc(uint64_t req) {
    if (!req || req > JS_ARENA_BYTES) return 0;
    uint32_t align_sz = (uint32_t)((req + 15ULL) & ~15ULL);
    AcquireSRWLockExclusive(&g_arena_lock);
    if (!g_arena_base) {
        g_arena_base = hos_guest_alloc(JS_ARENA_BYTES);
        if (!g_arena_base) {
            ReleaseSRWLockExclusive(&g_arena_lock);
            return 0;
        }
        g_arena_blk_n = 1;
        g_arena_blks[0].off = 0;
        g_arena_blks[0].size = JS_ARENA_BYTES;
        g_arena_blks[0].used = 0;
    }
    uint64_t out = 0;
    for (int i = 0; i < g_arena_blk_n; i++) {
        if (!g_arena_blks[i].used && g_arena_blks[i].size >= align_sz) {
            if (g_arena_blks[i].size > align_sz && g_arena_blk_n < JS_ARENA_MAX_BLOCKS) {
                memmove(&g_arena_blks[i + 2], &g_arena_blks[i + 1],
                        sizeof(ArenaBlk) * (size_t)(g_arena_blk_n - i - 1));
                g_arena_blks[i + 1].off = g_arena_blks[i].off + align_sz;
                g_arena_blks[i + 1].size = g_arena_blks[i].size - align_sz;
                g_arena_blks[i + 1].used = 0;
                g_arena_blks[i].size = align_sz;
                g_arena_blk_n++;
            }
            g_arena_blks[i].used = 1;
            out = g_arena_base + g_arena_blks[i].off;
            uint8_t *base = hos_guest_base();
            if (base && hos_mem_region(out, align_sz)) {
                memset(base + out, 0, align_sz);
                hos_mark_written(base + out, align_sz);
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_arena_lock);
    return out;
}

static int arena_free(uint64_t addr) {
    if (!g_arena_base || addr < g_arena_base || addr >= g_arena_base + JS_ARENA_BYTES) return 0;
    const uint32_t off = (uint32_t)(addr - g_arena_base);
    int freed = 0;
    AcquireSRWLockExclusive(&g_arena_lock);
    for (int i = 0; i < g_arena_blk_n; i++) {
        if (g_arena_blks[i].off == off && g_arena_blks[i].used) {
            g_arena_blks[i].used = 0;
            freed = 1;
            if (i + 1 < g_arena_blk_n && !g_arena_blks[i + 1].used) {
                g_arena_blks[i].size += g_arena_blks[i + 1].size;
                memmove(&g_arena_blks[i + 1], &g_arena_blks[i + 2],
                        sizeof(ArenaBlk) * (size_t)(g_arena_blk_n - i - 2));
                g_arena_blk_n--;
            }
            if (i > 0 && !g_arena_blks[i - 1].used) {
                g_arena_blks[i - 1].size += g_arena_blks[i].size;
                memmove(&g_arena_blks[i], &g_arena_blks[i + 1],
                        sizeof(ArenaBlk) * (size_t)(g_arena_blk_n - i - 1));
                g_arena_blk_n--;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_arena_lock);
    return freed;
}

/* ---- Native AArch64 Hooks & JS Callbacks ---- */
typedef struct {
    int used;
    uint64_t addr;
    A64Fn orig;
#ifdef LONGSHOT_ASI
    void *mh_target;
#endif
    JSValue before_fn;
    JSValue after_fn;
} JsHookSlot;

typedef struct {
    int used;
    JSValue fn;
} JsCbSlot;

static SRWLOCK g_hook_lock = SRWLOCK_INIT;
static JsHookSlot g_hooks[JS_HOOK_MAX];
static JsCbSlot g_cbs[JS_CB_MAX];
static volatile LONG g_hook_active = 0;

/* Context wrapper exposed inside native.hook before/after callbacks */
static JSClassID g_ctx_class_id = 0;
typedef struct {
    Ctx *c;
    uint64_t x[32];
    int valid;
} JsHookCtxWrap;

static void js_hook_ctx_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    JsHookCtxWrap *w = (JsHookCtxWrap *)JS_GetOpaque(val, g_ctx_class_id);
    free(w);
}
static JSClassDef g_ctx_class_def = {"NativeContext", .finalizer = js_hook_ctx_finalizer};

static JsHookCtxWrap *get_wrap(JSContext *ctx, JSValueConst this_val) {
    JsHookCtxWrap *w = (JsHookCtxWrap *)JS_GetOpaque2(ctx, this_val, g_ctx_class_id);
    if (!w || !w->valid || !w->c) {
        JS_ThrowTypeError(ctx, "NativeContext is only valid during hook execution");
        return NULL;
    }
    return w;
}

static JSValue hctx_get_x(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    if (argc < 1 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32)
        return JS_ThrowRangeError(ctx, "register index 0..31");
    return JS_NewBigUint64(ctx, w->c->x[idx]);
}
static JSValue hctx_set_x(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    uint64_t val = 0;
    if (argc < 2 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32 || js_to_u64(ctx, argv[1], &val) < 0)
        return JS_ThrowRangeError(ctx, "setX(0..31, u64)");
    w->c->x[idx] = val;
    return JS_UNDEFINED;
}
static JSValue hctx_get_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    if (argc < 1 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32)
        return JS_ThrowRangeError(ctx, "vreg index 0..31");
    float f = 0;
    memcpy(&f, &w->c->vreg[idx][0], sizeof f);
    return JS_NewFloat64(ctx, (double)f);
}
static JSValue hctx_set_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    double d = 0;
    if (argc < 2 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32 || JS_ToFloat64(ctx, &d, argv[1]) < 0)
        return JS_ThrowRangeError(ctx, "setF(0..31, number)");
    float f = (float)d;
    uint32_t bits = 0;
    memcpy(&bits, &f, 4);
    w->c->vreg[idx][0] = bits;
    return JS_UNDEFINED;
}
static JSValue hctx_get_d(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    if (argc < 1 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32)
        return JS_ThrowRangeError(ctx, "vreg index 0..31");
    double d = 0;
    memcpy(&d, &w->c->vreg[idx][0], sizeof d);
    return JS_NewFloat64(ctx, d);
}
static JSValue hctx_set_d(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint32_t idx = 0;
    double d = 0;
    if (argc < 2 || JS_ToUint32(ctx, &idx, argv[0]) < 0 || idx >= 32 || JS_ToFloat64(ctx, &d, argv[1]) < 0)
        return JS_ThrowRangeError(ctx, "setD(0..31, number)");
    memcpy(&w->c->vreg[idx][0], &d, sizeof d);
    return JS_UNDEFINED;
}
static JSValue hctx_get_x_prop(JSContext *ctx, JSValueConst this_val, int magic) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    return JS_NewBigUint64(ctx, w->c->x[magic]);
}
static JSValue hctx_set_x_prop(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic) {
    JsHookCtxWrap *w = get_wrap(ctx, this_val);
    if (!w) return JS_EXCEPTION;
    uint64_t v = 0;
    if (js_to_u64(ctx, val, &v) < 0) return JS_EXCEPTION;
    w->c->x[magic] = v;
    return JS_UNDEFINED;
}

static JSValue make_hook_ctx(JSContext *ctx, Ctx *c, JsHookCtxWrap **out_wrap) {
    JsHookCtxWrap *w = (JsHookCtxWrap *)calloc(1, sizeof *w);
    w->c = c;
    w->valid = 1;
    JSValue obj = JS_NewObjectClass(ctx, (int)g_ctx_class_id);
    JS_SetOpaque(obj, w);
    JS_SetPropertyStr(ctx, obj, "getX", JS_NewCFunction(ctx, hctx_get_x, "getX", 1));
    JS_SetPropertyStr(ctx, obj, "setX", JS_NewCFunction(ctx, hctx_set_x, "setX", 2));
    JS_SetPropertyStr(ctx, obj, "getF", JS_NewCFunction(ctx, hctx_get_f, "getF", 1));
    JS_SetPropertyStr(ctx, obj, "setF", JS_NewCFunction(ctx, hctx_set_f, "setF", 2));
    JS_SetPropertyStr(ctx, obj, "getD", JS_NewCFunction(ctx, hctx_get_d, "getD", 1));
    JS_SetPropertyStr(ctx, obj, "setD", JS_NewCFunction(ctx, hctx_set_d, "setD", 2));
    static const JSCFunctionListEntry xprops[] = {
        JS_CGETSET_MAGIC_DEF("x0", hctx_get_x_prop, hctx_set_x_prop, 0),
        JS_CGETSET_MAGIC_DEF("x1", hctx_get_x_prop, hctx_set_x_prop, 1),
        JS_CGETSET_MAGIC_DEF("x2", hctx_get_x_prop, hctx_set_x_prop, 2),
        JS_CGETSET_MAGIC_DEF("x3", hctx_get_x_prop, hctx_set_x_prop, 3),
        JS_CGETSET_MAGIC_DEF("x4", hctx_get_x_prop, hctx_set_x_prop, 4),
        JS_CGETSET_MAGIC_DEF("x5", hctx_get_x_prop, hctx_set_x_prop, 5),
        JS_CGETSET_MAGIC_DEF("x6", hctx_get_x_prop, hctx_set_x_prop, 6),
        JS_CGETSET_MAGIC_DEF("x7", hctx_get_x_prop, hctx_set_x_prop, 7),
        JS_CGETSET_MAGIC_DEF("lr", hctx_get_x_prop, hctx_set_x_prop, 30),
        JS_CGETSET_MAGIC_DEF("sp", hctx_get_x_prop, hctx_set_x_prop, 31),
    };
    JS_SetPropertyFunctionList(ctx, obj, xprops, (int)(sizeof xprops / sizeof xprops[0]));
    *out_wrap = w;
    return obj;
}

static void js_tramp_dispatch(Ctx *c, int slot) {
    A64Fn orig = NULL;
    AcquireSRWLockShared(&g_hook_lock);
    const int used = g_hooks[slot].used;
    orig = g_hooks[slot].orig;
    ReleaseSRWLockShared(&g_hook_lock);
    if (!used || !orig) return;

    int run_orig = 1;
    Ctx *prev_tls = g_js_tls_ctx;
    g_js_tls_ctx = c;

    if (js_enter()) {
        AcquireSRWLockShared(&g_hook_lock);
        JSValue bfn = g_hooks[slot].used && !JS_IsUndefined(g_hooks[slot].before_fn)
                          ? JS_DupValue(g_js_ctx, g_hooks[slot].before_fn)
                          : JS_UNDEFINED;
        ReleaseSRWLockShared(&g_hook_lock);
        if (!JS_IsUndefined(bfn)) {
            JsHookCtxWrap *w = NULL;
            JSValue hobj = make_hook_ctx(g_js_ctx, c, &w);
            JSValue r = JS_Call(g_js_ctx, bfn, JS_UNDEFINED, 1, &hobj);
            w->valid = 0;
            w->c = NULL;
            if (JS_IsException(r)) {
                js_log_exception(g_js_ctx, "native.hook.before");
            } else if (JS_IsBool(r) && !JS_ToBool(g_js_ctx, r)) {
                run_orig = 0;
            }
            JS_FreeValue(g_js_ctx, r);
            JS_FreeValue(g_js_ctx, hobj);
            JS_FreeValue(g_js_ctx, bfn);
            js_drain_jobs(g_js_ctx);
        }
        js_leave();
    }

    if (run_orig) orig(c);

    if (js_enter()) {
        AcquireSRWLockShared(&g_hook_lock);
        JSValue afn = g_hooks[slot].used && !JS_IsUndefined(g_hooks[slot].after_fn)
                          ? JS_DupValue(g_js_ctx, g_hooks[slot].after_fn)
                          : JS_UNDEFINED;
        ReleaseSRWLockShared(&g_hook_lock);
        if (!JS_IsUndefined(afn)) {
            JsHookCtxWrap *w = NULL;
            JSValue hobj = make_hook_ctx(g_js_ctx, c, &w);
            JSValue r = JS_Call(g_js_ctx, afn, JS_UNDEFINED, 1, &hobj);
            w->valid = 0;
            w->c = NULL;
            if (JS_IsException(r)) js_log_exception(g_js_ctx, "native.hook.after");
            JS_FreeValue(g_js_ctx, r);
            JS_FreeValue(g_js_ctx, hobj);
            JS_FreeValue(g_js_ctx, afn);
            js_drain_jobs(g_js_ctx);
        }
        js_leave();
    }

    g_js_tls_ctx = prev_tls;
}

static void js_cb_dispatch(Ctx *c, int slot) {
    Ctx *prev_tls = g_js_tls_ctx;
    g_js_tls_ctx = c;
    if (js_enter()) {
        AcquireSRWLockShared(&g_hook_lock);
        JSValue fn = g_cbs[slot].used ? JS_DupValue(g_js_ctx, g_cbs[slot].fn) : JS_UNDEFINED;
        ReleaseSRWLockShared(&g_hook_lock);
        if (!JS_IsUndefined(fn)) {
            JSValue args[8];
            for (int i = 0; i < 8; i++) args[i] = JS_NewBigUint64(g_js_ctx, c->x[i]);
            JSValue r = JS_Call(g_js_ctx, fn, JS_UNDEFINED, 8, args);
            for (int i = 0; i < 8; i++) JS_FreeValue(g_js_ctx, args[i]);
            if (JS_IsException(r)) {
                js_log_exception(g_js_ctx, "native.callback");
                c->x[0] = 0;
            } else {
                uint64_t rv = 0;
                if (js_to_u64(g_js_ctx, r, &rv) == 0) c->x[0] = rv;
                else if (JS_IsBool(r)) c->x[0] = (uint64_t)(JS_ToBool(g_js_ctx, r) ? 1 : 0);
            }
            JS_FreeValue(g_js_ctx, r);
            JS_FreeValue(g_js_ctx, fn);
            js_drain_jobs(g_js_ctx);
        }
        js_leave();
    }
    g_js_tls_ctx = prev_tls;
}

#define DEFINE_TRAMP_8(base) \
    static void js_tramp_##base(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base); } \
    static void js_tramp_##base##_1(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 1); } \
    static void js_tramp_##base##_2(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 2); } \
    static void js_tramp_##base##_3(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 3); } \
    static void js_tramp_##base##_4(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 4); } \
    static void js_tramp_##base##_5(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 5); } \
    static void js_tramp_##base##_6(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 6); } \
    static void js_tramp_##base##_7(Ctx * A64_NOESC c) { js_tramp_dispatch(c, base + 7); }

#define LIST_TRAMP_8(base) \
    js_tramp_##base, js_tramp_##base##_1, js_tramp_##base##_2, js_tramp_##base##_3, \
    js_tramp_##base##_4, js_tramp_##base##_5, js_tramp_##base##_6, js_tramp_##base##_7

DEFINE_TRAMP_8(0)   DEFINE_TRAMP_8(8)   DEFINE_TRAMP_8(16)  DEFINE_TRAMP_8(24)
DEFINE_TRAMP_8(32)  DEFINE_TRAMP_8(40)  DEFINE_TRAMP_8(48)  DEFINE_TRAMP_8(56)
DEFINE_TRAMP_8(64)  DEFINE_TRAMP_8(72)  DEFINE_TRAMP_8(80)  DEFINE_TRAMP_8(88)
DEFINE_TRAMP_8(96)  DEFINE_TRAMP_8(104) DEFINE_TRAMP_8(112) DEFINE_TRAMP_8(120)

static const A64Fn g_tramps[JS_HOOK_MAX] = {
    LIST_TRAMP_8(0),   LIST_TRAMP_8(8),   LIST_TRAMP_8(16),  LIST_TRAMP_8(24),
    LIST_TRAMP_8(32),  LIST_TRAMP_8(40),  LIST_TRAMP_8(48),  LIST_TRAMP_8(56),
    LIST_TRAMP_8(64),  LIST_TRAMP_8(72),  LIST_TRAMP_8(80),  LIST_TRAMP_8(88),
    LIST_TRAMP_8(96),  LIST_TRAMP_8(104), LIST_TRAMP_8(112), LIST_TRAMP_8(120),
};

#define DEFINE_CB_8(base) \
    static void js_cb_##base(Ctx * A64_NOESC c) { js_cb_dispatch(c, base); } \
    static void js_cb_##base##_1(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 1); } \
    static void js_cb_##base##_2(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 2); } \
    static void js_cb_##base##_3(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 3); } \
    static void js_cb_##base##_4(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 4); } \
    static void js_cb_##base##_5(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 5); } \
    static void js_cb_##base##_6(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 6); } \
    static void js_cb_##base##_7(Ctx * A64_NOESC c) { js_cb_dispatch(c, base + 7); }

#define LIST_CB_8(base) \
    js_cb_##base, js_cb_##base##_1, js_cb_##base##_2, js_cb_##base##_3, \
    js_cb_##base##_4, js_cb_##base##_5, js_cb_##base##_6, js_cb_##base##_7

DEFINE_CB_8(0) DEFINE_CB_8(8) DEFINE_CB_8(16) DEFINE_CB_8(24)

static const A64Fn g_cb_tramps[JS_CB_MAX] = {
    LIST_CB_8(0), LIST_CB_8(8), LIST_CB_8(16), LIST_CB_8(24),
};

void (*a64_js_lookup_hook(uint64_t addr))(struct GuestContext *) {
    if (!__atomic_load_n(&g_hook_active, __ATOMIC_RELAXED)) return NULL;
    if (addr >= JS_CB_BASE && addr < JS_CB_BASE + JS_CB_MAX * 4ULL && (addr & 3ULL) == 0) {
        const unsigned slot = (unsigned)((addr - JS_CB_BASE) / 4ULL);
        AcquireSRWLockShared(&g_hook_lock);
        A64Fn r = g_cbs[slot].used ? g_cb_tramps[slot] : NULL;
        ReleaseSRWLockShared(&g_hook_lock);
        return (void (*)(struct GuestContext *))(uintptr_t)r;
    }
#ifdef LONGSHOT_ASI
    return NULL;
#else
    A64Fn r = NULL;
    AcquireSRWLockShared(&g_hook_lock);
    for (int i = 0; i < JS_HOOK_MAX; i++) {
        if (g_hooks[i].used && g_hooks[i].addr == addr) {
            r = g_tramps[i];
            break;
        }
    }
    ReleaseSRWLockShared(&g_hook_lock);
    return (void (*)(struct GuestContext *))(uintptr_t)r;
#endif
}

/* ---- memory.* JS bindings ---- */
static int get_addr(JSContext *ctx, JSValueConst val, uint64_t *out) {
    if (js_to_u64(ctx, val, out) < 0) {
        JS_ThrowRangeError(ctx, "invalid guest address");
        return -1;
    }
    return 0;
}

#define DEFINE_READ_NUM(fname, ctype, conv) \
    static JSValue js_mem_##fname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { \
        (void)this_val; \
        uint64_t a = 0; \
        if (argc < 1 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION; \
        ctype v = 0; \
        if (!js_mem_read(a, &v, sizeof v)) return JS_ThrowRangeError(ctx, "unmapped guest read"); \
        return conv(ctx, v); \
    }

#define JS_RET_U32(ctx, v) JS_NewUint32((ctx), (uint32_t)(v))
#define JS_RET_I32(ctx, v) JS_NewInt32((ctx), (int32_t)(v))
#define JS_RET_U64(ctx, v) JS_NewBigUint64((ctx), (uint64_t)(v))
#define JS_RET_I64(ctx, v) JS_NewBigInt64((ctx), (int64_t)(v))
#define JS_RET_F64(ctx, v) JS_NewFloat64((ctx), (double)(v))

DEFINE_READ_NUM(readU8,  uint8_t,  JS_RET_U32)
DEFINE_READ_NUM(readS8,  int8_t,   JS_RET_I32)
DEFINE_READ_NUM(readU16, uint16_t, JS_RET_U32)
DEFINE_READ_NUM(readS16, int16_t,  JS_RET_I32)
DEFINE_READ_NUM(readU32, uint32_t, JS_RET_U32)
DEFINE_READ_NUM(readS32, int32_t,  JS_RET_I32)
DEFINE_READ_NUM(readU64, uint64_t, JS_RET_U64)
DEFINE_READ_NUM(readS64, int64_t,  JS_RET_I64)
DEFINE_READ_NUM(readF32, float,    JS_RET_F64)
DEFINE_READ_NUM(readF64, double,   JS_RET_F64)

static JSValue js_mem_writeU8(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; uint32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToUint32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    uint8_t b = (uint8_t)v;
    return js_mem_write(a, &b, 1) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeS8(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; int32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    int8_t b = (int8_t)v;
    return js_mem_write(a, &b, 1) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeU16(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; uint32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToUint32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    uint16_t b = (uint16_t)v;
    return js_mem_write(a, &b, 2) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeS16(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; int32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    int16_t b = (int16_t)v;
    return js_mem_write(a, &b, 2) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeU32(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; uint32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || js_to_u32(ctx, argv[1], &v) < 0) return JS_EXCEPTION;
    return js_mem_write(a, &v, 4) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeS32(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; int32_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_EXCEPTION;
    return js_mem_write(a, &v, 4) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeU64(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0, v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || js_to_u64(ctx, argv[1], &v) < 0) return JS_EXCEPTION;
    return js_mem_write(a, &v, 8) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeS64(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; int64_t v = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    if (JS_IsBigInt(argv[1])) { if (JS_ToBigInt64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION; }
    else { if (JS_ToInt64(ctx, &v, argv[1]) < 0) return JS_EXCEPTION; }
    return js_mem_write(a, &v, 8) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeF32(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; double d = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToFloat64(ctx, &d, argv[1]) < 0) return JS_EXCEPTION;
    float f = (float)d;
    return js_mem_write(a, &f, 4) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}
static JSValue js_mem_writeF64(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; double d = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToFloat64(ctx, &d, argv[1]) < 0) return JS_EXCEPTION;
    return js_mem_write(a, &d, 8) ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest write");
}

static JSValue js_mem_readBytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0; uint32_t len = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0 || JS_ToUint32(ctx, &len, argv[1]) < 0) return JS_EXCEPTION;
    if (len > 16u * 1024u * 1024u) return JS_ThrowRangeError(ctx, "readBytes max 16 MB");
    if (!len) return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    if (!js_mem_ok(a, len)) return JS_ThrowRangeError(ctx, "unmapped guest readBytes");
    const uint8_t *p = hos_guest_base() + a;
    return JS_NewUint8ArrayCopy(ctx, p, len);
}

static JSValue js_mem_writeBytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    size_t len = 0;
    uint8_t *src = JS_GetUint8Array(ctx, &len, argv[1]);
    if (!src) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        src = JS_GetArrayBuffer(ctx, &len, argv[1]);
        if (src) {
            if (len && !js_mem_write(a, src, len)) return JS_ThrowRangeError(ctx, "unmapped guest writeBytes");
            return JS_UNDEFINED;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
        if (!JS_IsArray(argv[1])) return JS_ThrowTypeError(ctx, "writeBytes expects Uint8Array, ArrayBuffer, or number[]");
        JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        if (n > 16u * 1024u * 1024u) return JS_ThrowRangeError(ctx, "writeBytes max 16 MB");
        uint8_t *tmp = n ? (uint8_t *)malloc(n) : NULL;
        for (uint32_t i = 0; i < n; i++) {
            JSValue iv = JS_GetPropertyUint32(ctx, argv[1], i);
            uint32_t b = 0;
            JS_ToUint32(ctx, &b, iv);
            JS_FreeValue(ctx, iv);
            tmp[i] = (uint8_t)b;
        }
        const int ok = !n || js_mem_write(a, tmp, n);
        free(tmp);
        return ok ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest writeBytes");
    }
    if (len && !js_mem_write(a, src, len)) return JS_ThrowRangeError(ctx, "unmapped guest writeBytes");
    return JS_UNDEFINED;
}

static JSValue js_mem_readCString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0;
    uint32_t max_len = 4096;
    if (argc < 1 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    if (argc >= 2 && JS_ToUint32(ctx, &max_len, argv[1]) < 0) return JS_EXCEPTION;
    if (max_len > 1024u * 1024u) max_len = 1024u * 1024u;
    if (!max_len) return JS_NewString(ctx, "");
    char stack_buf[256];
    char *buf = max_len <= sizeof stack_buf ? stack_buf : (char *)malloc(max_len);
    size_t i = 0;
    for (; i + 1 < max_len; i++) {
        uint8_t ch = 0;
        if (!js_mem_u8(a + i, &ch)) {
            if (i == 0) {
                if (buf != stack_buf) free(buf);
                return JS_ThrowRangeError(ctx, "unmapped guest CString");
            }
            break;
        }
        if (!ch) break;
        buf[i] = (char)ch;
    }
    buf[i] = 0;
    JSValue s = JS_NewStringLen(ctx, buf, i);
    if (buf != stack_buf) free(buf);
    return s;
}

static JSValue js_mem_writeCString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_EXCEPTION;
    const int ok = js_mem_write(a, s, len + 1);
    JS_FreeCString(ctx, s);
    return ok ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "unmapped guest writeCString");
}

static JSValue js_mem_isMapped(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0, sz = 1;
    if (argc < 1 || js_to_u64(ctx, argv[0], &a) < 0) return JS_FALSE;
    if (argc >= 2 && js_to_u64(ctx, argv[1], &sz) < 0) return JS_FALSE;
    return JS_NewBool(ctx, js_mem_ok(a, sz));
}

static int parse_hex_pattern(const char *s, uint8_t *bytes, uint8_t *mask, size_t max_len) {
    int n = 0;
    while (*s && (size_t)n < max_len) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if (!*s) break;
        if (s[0] == '?') {
            bytes[n] = 0;
            mask[n] = 0;
            n++;
            s += (s[1] == '?') ? 2 : 1;
        } else if (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1])) {
            char tmp[3] = {s[0], s[1], 0};
            bytes[n] = (uint8_t)strtoul(tmp, NULL, 16);
            mask[n] = 0xFF;
            n++;
            s += 2;
        } else {
            return -1;
        }
    }
    return n;
}

static JSValue js_mem_scan(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t start = 0, len = 0;
    if (argc < 3 || get_addr(ctx, argv[0], &start) < 0 || js_to_u64(ctx, argv[1], &len) < 0) return JS_EXCEPTION;
    const char *pat = JS_ToCString(ctx, argv[2]);
    if (!pat) return JS_EXCEPTION;
    uint8_t bytes[256], mask[256];
    const int plen = parse_hex_pattern(pat, bytes, mask, sizeof bytes);
    JS_FreeCString(ctx, pat);
    if (plen <= 0) return JS_ThrowSyntaxError(ctx, "invalid hex pattern (expected bytes like \"48 8B ?? 10\")");
    if (len > 256ULL * 1024ULL * 1024ULL) len = 256ULL * 1024ULL * 1024ULL;
    JSValue arr = JS_NewArray(ctx);
    if (!js_mem_ok(start, len) || len < (uint64_t)plen) return arr;
    const uint8_t *p = hos_guest_base() + start;
    uint32_t out_n = 0;
    const uint64_t limit = len - (uint64_t)plen;
    for (uint64_t i = 0; i <= limit && out_n < 1024; i++) {
        if ((p[i] & mask[0]) != (bytes[0] & mask[0])) continue;
        int match = 1;
        for (int j = 1; j < plen; j++) {
            if ((p[i + (uint64_t)j] & mask[j]) != (bytes[j] & mask[j])) {
                match = 0;
                break;
            }
        }
        if (match) JS_SetPropertyUint32(ctx, arr, out_n++, JS_NewBigUint64(ctx, start + i));
    }
    return arr;
}

static JSValue js_mem_alloc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t sz = 0;
    if (argc < 1 || js_to_u64(ctx, argv[0], &sz) < 0) return JS_ThrowRangeError(ctx, "alloc(size)");
    const uint64_t a = arena_alloc(sz);
    if (!a) return JS_ThrowRangeError(ctx, "JS scratch guest arena exhausted (4 MB max)");
    return JS_NewBigUint64(ctx, a);
}

static JSValue js_mem_free(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0;
    if (argc < 1 || get_addr(ctx, argv[0], &a) < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, arena_free(a));
}

/* ---- native.* JS bindings ---- */
/* In the 3.0.0 main image an AI node's slot 19 returns its class name via
   ADRP X0; ADD X0,X0,#imm; RET. Decode that accessor without invoking game code. */
static JSValue js_native_ai_class(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (!argc || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "expected AI class name");
    size_t length;
    const char *name = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!name) return JS_EXCEPTION;
    const uint64_t first = 0x849974f8ULL, last = 0x84c85dd0ULL;
    JSValue matches = JS_NewArray(ctx);
    if (JS_IsException(matches) || !length || length > 255 || memchr(name, 0, length) ||
        !js_mem_ok(first - 16, last - first + 16 + 128 * 8)) goto done;
    uint8_t *base = hos_guest_base();
    if (!base) goto done;
    uint32_t found = 0;
    for (uint64_t address = first; address <= last; address += 8) {
        uint64_t header[3], getter;
        memcpy(header, base + address - 16, sizeof header);
        if (header[0] || header[1] || header[2] < 0x80020000 || header[2] >= 0x83c67000) continue;
        memcpy(&getter, base + address + 19 * 8, sizeof getter);
        uint32_t code[3];
        if (getter < 0x80020000 || getter > 0x83c67000 - sizeof code ||
            !js_mem_read(getter, code, sizeof code)) continue;
        if ((code[0] & 0x9f00001fU) != 0x90000000U || (code[1] & 0xffc003ffU) != 0x91000000U ||
            code[2] != 0xd65f03c0U) continue;
        int32_t pages = (int32_t)(((code[0] >> 5) & 0x7ffff) * 4 + ((code[0] >> 29) & 3));
        if (pages & (1 << 20)) pages -= 1 << 21;
        uint64_t text = (getter & ~0xfffULL) + (int64_t)pages * 4096 + ((code[1] >> 10) & 0xfff);
        if (text < 0x83c67000 || text + length + 1 > 0x8486d000 ||
            !js_mem_ok(text, length + 1) || memcmp(base + text, name, length + 1)) continue;
        JSValue entry = JS_NewObject(ctx), methods = JS_NewArray(ctx);
        for (unsigned slot = 0; slot < 128; slot++) {
            uint64_t fn;
            memcpy(&fn, base + address + slot * 8, sizeof fn);
            if (fn < 0x80020000 || fn >= 0x83c67000) break;
            JS_SetPropertyUint32(ctx, methods, slot, a64_lookup(fn) ? JS_NewBigUint64(ctx, fn) : JS_NULL);
        }
        JS_SetPropertyStr(ctx, entry, "vtable", JS_NewBigUint64(ctx, address));
        JS_SetPropertyStr(ctx, entry, "methods", methods);
        JS_SetPropertyUint32(ctx, matches, found++, entry);
    }
done:
    JS_FreeCString(ctx, name);
    return matches;
}

/* libc++abi ships with Longshot's LLVM toolchain. */
extern char *__cxa_demangle(const char *, char *, size_t *, int *);
static JSValue js_native_demangle_symbol(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (!argc || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "expected a native symbol name");
    size_t length;
    const char *name = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!name) return JS_EXCEPTION;
    JSValue result = JS_NULL;
    if (length >= 2 && length <= 4096 && !strncmp(name, "_Z", 2) && !memchr(name, 0, length)) {
        int status;
        char *pretty = __cxa_demangle(name, NULL, NULL, &status);
        if (pretty && status == 0) result = JS_NewString(ctx, pretty);
        free(pretty);
    }
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_native_list_functions(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    double start = 0, limit = 128;
    if ((argc && JS_ToFloat64(ctx, &start, argv[0]) < 0) ||
        (argc > 1 && JS_ToFloat64(ctx, &limit, argv[1]) < 0)) return JS_EXCEPTION;
    if (!(start >= 0 && start <= UINT32_MAX && limit >= 1 && limit <= 1024) ||
        floor(start) != start || floor(limit) != limit)
        return JS_ThrowRangeError(ctx, "expected a nonnegative index and page size 1..1024");
    uint32_t total = 0;
    const uint32_t *addresses = js_lifted_functions(&total);
    if (!addresses) return JS_NULL;
    JSValue page = JS_NewObject(ctx), items = JS_NewArray(ctx);
    uint32_t i = (uint32_t)start;
    for (unsigned n = 0; n < (uint32_t)limit && i < total; n++, i++)
        JS_SetPropertyUint32(ctx, items, n, JS_NewBigUint64(ctx, addresses[i]));
    JS_SetPropertyStr(ctx, page, "addresses", items);
    JS_SetPropertyStr(ctx, page, "total", JS_NewUint32(ctx, total));
    JS_SetPropertyStr(ctx, page, "next", i < total ? JS_NewUint32(ctx, i) : JS_NULL);
    return page;
}

static JSValue js_native_has_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t a = 0;
    if (argc < 1 || get_addr(ctx, argv[0], &a) < 0) return JS_FALSE;
    return JS_NewBool(ctx, a64_lookup(a) != NULL);
}

static JSValue js_native_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (!g_js_tls_ctx)
        return JS_ThrowPlainError(ctx, "native.call() requires a live guest thread context (use game.onPlayerUpdate or game.defer)");
    uint64_t addr = 0;
    if (argc < 1 || get_addr(ctx, argv[0], &addr) < 0) return JS_EXCEPTION;
    A64Fn fn = a64_lookup(addr);
    if (!fn) return JS_ThrowReferenceError(ctx, "no lifted function at 0x%llx", (unsigned long long)addr);

    Ctx *c = g_js_tls_ctx;
    const Ctx saved = *c;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "expected native register arguments");
    // GetTime returns a 24-byte object in caller-owned storage through x8.
    const int get_time = addr == 0x83472280ULL;
    JSValue options = argc >= 2 ? argv[1] : JS_UNDEFINED;
    JSValue buffer = JS_IsObject(options) ? JS_GetPropertyStr(ctx, options, "indirectResult") : JS_UNDEFINED;
    if (JS_IsException(buffer)) return JS_EXCEPTION;
    if (!JS_IsUndefined(buffer) || get_time) {
        uint64_t p;
        int valid = !JS_IsUndefined(buffer) && js_to_u64(ctx, buffer, &p) == 0 && js_mem_ok(p, get_time ? 24 : 1);
        JS_FreeValue(ctx, buffer);
        if (!valid) return JS_ThrowRangeError(ctx, "expected mapped indirectResult buffer");
        c->x[8] = p;
    }
    static const char *const names[] = {"x", "f", "d"};
    for (int bank = 0; bank < 3 && JS_IsObject(options); bank++) {
        JSValue regs = JS_GetPropertyStr(ctx, options, names[bank]);
        if (JS_IsUndefined(regs)) continue;
        if (!JS_IsArray(regs)) {
            *c = saved;
            if (JS_IsException(regs)) return JS_EXCEPTION;
            JS_FreeValue(ctx, regs);
            return JS_ThrowTypeError(ctx, "expected register array");
        }
        for (uint32_t i = 0; i < 8; i++) {
            JSValue reg = JS_GetPropertyUint32(ctx, regs, i);
            if (JS_IsUndefined(reg)) continue;
            int ok = !JS_IsException(reg);
            if (ok && bank == 0) ok = js_to_u64(ctx, reg, &c->x[i]) == 0;
            if (ok && bank != 0) {
                double d = 0;
                ok = JS_ToFloat64(ctx, &d, reg) == 0;
                if (ok && bank == 2) memcpy(&c->vreg[i][0], &d, sizeof d);
                if (ok && bank == 1) {
                    float f = (float)d;
                    uint32_t bits;
                    memcpy(&bits, &f, sizeof bits);
                    c->vreg[i][0] = bits;
                }
            }
            JS_FreeValue(ctx, reg);
            if (!ok) {
                JS_FreeValue(ctx, regs);
                *c = saved;
                return JS_ThrowTypeError(ctx, "invalid %s register %u", names[bank], i);
            }
        }
        JS_FreeValue(ctx, regs);
    }

    fn(c);

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "x0", JS_NewBigUint64(ctx, c->x[0]));
    JS_SetPropertyStr(ctx, res, "x1", JS_NewBigUint64(ctx, c->x[1]));
    JS_SetPropertyStr(ctx, res, "x2", JS_NewBigUint64(ctx, c->x[2]));
    JS_SetPropertyStr(ctx, res, "x3", JS_NewBigUint64(ctx, c->x[3]));
    float f0 = 0;
    double d0 = 0;
    memcpy(&f0, &c->vreg[0][0], sizeof f0);
    memcpy(&d0, &c->vreg[0][0], sizeof d0);
    JS_SetPropertyStr(ctx, res, "f0", JS_NewFloat64(ctx, (double)f0));
    JS_SetPropertyStr(ctx, res, "d0", JS_NewFloat64(ctx, d0));
    *c = saved;
    return res;
}

static JSValue js_native_hook(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t addr = 0;
    if (argc < 2 || get_addr(ctx, argv[0], &addr) < 0 || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx, "native.hook(addr, { before?, after? })");
    A64Fn orig = a64_lookup(addr);
    if (!orig) return JS_ThrowReferenceError(ctx, "no lifted function at 0x%llx", (unsigned long long)addr);

    JSValue bf = JS_GetPropertyStr(ctx, argv[1], "before");
    JSValue af = JS_GetPropertyStr(ctx, argv[1], "after");
    if (!JS_IsFunction(ctx, bf)) { JS_FreeValue(ctx, bf); bf = JS_UNDEFINED; }
    if (!JS_IsFunction(ctx, af)) { JS_FreeValue(ctx, af); af = JS_UNDEFINED; }
    if (JS_IsUndefined(bf) && JS_IsUndefined(af))
        return JS_ThrowTypeError(ctx, "native.hook requires before and/or after function");

    int chosen = -1;
    int existing = 0;
    AcquireSRWLockExclusive(&g_hook_lock);
    for (int i = 0; i < JS_HOOK_MAX; i++) {
        if (g_hooks[i].used && g_hooks[i].addr == addr) {
            JS_FreeValue(ctx, g_hooks[i].before_fn);
            JS_FreeValue(ctx, g_hooks[i].after_fn);
            chosen = i;
            existing = 1;
            break;
        }
    }
    if (chosen < 0) {
        for (int i = 0; i < JS_HOOK_MAX; i++) {
            if (!g_hooks[i].used) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen < 0) {
        ReleaseSRWLockExclusive(&g_hook_lock);
        JS_FreeValue(ctx, bf);
        JS_FreeValue(ctx, af);
        return JS_ThrowRangeError(ctx, "hook limit (%d) reached", JS_HOOK_MAX);
    }
#ifdef LONGSHOT_ASI
    if (existing) {
        g_hooks[chosen].before_fn = bf;
        g_hooks[chosen].after_fn = af;
        ReleaseSRWLockExclusive(&g_hook_lock);
        return JS_NewInt32(ctx, chosen + 1);
    }
    ReleaseSRWLockExclusive(&g_hook_lock);
    void *tramp = NULL;
    if (MH_CreateHook((void *)orig, (void *)g_tramps[chosen], &tramp) != MH_OK) {
        JS_FreeValue(ctx, bf);
        JS_FreeValue(ctx, af);
        return JS_ThrowInternalError(ctx, "MinHook create failed for 0x%llx", (unsigned long long)addr);
    }
    AcquireSRWLockExclusive(&g_hook_lock);
    g_hooks[chosen].used = 1;
    g_hooks[chosen].addr = addr;
    g_hooks[chosen].mh_target = (void *)orig;
    g_hooks[chosen].orig = (A64Fn)tramp;
    g_hooks[chosen].before_fn = bf;
    g_hooks[chosen].after_fn = af;
    __atomic_add_fetch(&g_hook_active, 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_hook_lock);
    if (MH_EnableHook((void *)orig) != MH_OK) {
        MH_RemoveHook((void *)orig);
        AcquireSRWLockExclusive(&g_hook_lock);
        JS_FreeValue(ctx, g_hooks[chosen].before_fn);
        JS_FreeValue(ctx, g_hooks[chosen].after_fn);
        g_hooks[chosen].before_fn = JS_UNDEFINED;
        g_hooks[chosen].after_fn = JS_UNDEFINED;
        g_hooks[chosen].used = 0;
        g_hooks[chosen].addr = 0;
        g_hooks[chosen].orig = NULL;
        g_hooks[chosen].mh_target = NULL;
        ReleaseSRWLockExclusive(&g_hook_lock);
        return JS_ThrowInternalError(ctx, "MinHook enable failed for 0x%llx", (unsigned long long)addr);
    }
    return JS_NewInt32(ctx, chosen + 1);
#else
    if (!existing) g_hooks[chosen].orig = orig;
    g_hooks[chosen].used = 1;
    g_hooks[chosen].addr = addr;
    g_hooks[chosen].before_fn = bf;
    g_hooks[chosen].after_fn = af;
    __atomic_add_fetch(&g_hook_active, 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_hook_lock);

    a64_rt_invalidate_sites(addr);
    return JS_NewInt32(ctx, chosen + 1);
#endif
}

static JSValue js_native_unhook(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    uint64_t val = 0;
    if (js_to_u64(ctx, argv[0], &val) < 0) return JS_FALSE;
    int removed = 0;
    uint64_t target = 0;
#ifdef LONGSHOT_ASI
    void *mh_tgt = NULL;
#endif
    AcquireSRWLockExclusive(&g_hook_lock);
    if (val >= 1 && val <= JS_HOOK_MAX) {
        int i = (int)val - 1;
        if (g_hooks[i].used) {
            target = g_hooks[i].addr;
#ifdef LONGSHOT_ASI
            mh_tgt = g_hooks[i].mh_target;
            g_hooks[i].mh_target = NULL;
#endif
            JS_FreeValue(ctx, g_hooks[i].before_fn);
            JS_FreeValue(ctx, g_hooks[i].after_fn);
            g_hooks[i].before_fn = JS_UNDEFINED;
            g_hooks[i].after_fn = JS_UNDEFINED;
            g_hooks[i].used = 0;
            g_hooks[i].orig = NULL;
            removed = 1;
        }
    } else {
        for (int i = 0; i < JS_HOOK_MAX; i++) {
            if (g_hooks[i].used && g_hooks[i].addr == val) {
                target = g_hooks[i].addr;
#ifdef LONGSHOT_ASI
                mh_tgt = g_hooks[i].mh_target;
                g_hooks[i].mh_target = NULL;
#endif
                JS_FreeValue(ctx, g_hooks[i].before_fn);
                JS_FreeValue(ctx, g_hooks[i].after_fn);
                g_hooks[i].before_fn = JS_UNDEFINED;
                g_hooks[i].after_fn = JS_UNDEFINED;
                g_hooks[i].used = 0;
                g_hooks[i].orig = NULL;
                removed = 1;
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_hook_lock);
#ifdef LONGSHOT_ASI
    (void)target;
    if (mh_tgt) {
        MH_DisableHook(mh_tgt);
        MH_RemoveHook(mh_tgt);
    }
#else
    if (target) a64_rt_invalidate_sites(target);
#endif
    return JS_NewBool(ctx, removed);
}

static JSValue js_native_create_cb(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "createCallback(fn) expects a function");
    int chosen = -1;
    AcquireSRWLockExclusive(&g_hook_lock);
    for (int i = 0; i < JS_CB_MAX; i++) {
        if (!g_cbs[i].used) {
            chosen = i;
            g_cbs[i].used = 1;
            g_cbs[i].fn = JS_DupValue(ctx, argv[0]);
            __atomic_add_fetch(&g_hook_active, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hook_lock);
    if (chosen < 0) return JS_ThrowRangeError(ctx, "callback limit (%d) reached", JS_CB_MAX);
    return JS_NewBigUint64(ctx, JS_CB_BASE + (uint64_t)chosen * 4ULL);
}

void js_register_memory(JSContext *ctx, JSValue ns_mem, JSValue ns_native) {
    if (!g_ctx_class_id) {
        JS_NewClassID(JS_GetRuntime(ctx), &g_ctx_class_id);
        JS_NewClass(JS_GetRuntime(ctx), g_ctx_class_id, &g_ctx_class_def);
    }

    JS_SetPropertyStr(ctx, ns_mem, "readU8", JS_NewCFunction(ctx, js_mem_readU8, "readU8", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readS8", JS_NewCFunction(ctx, js_mem_readS8, "readS8", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readU16", JS_NewCFunction(ctx, js_mem_readU16, "readU16", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readS16", JS_NewCFunction(ctx, js_mem_readS16, "readS16", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readU32", JS_NewCFunction(ctx, js_mem_readU32, "readU32", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readS32", JS_NewCFunction(ctx, js_mem_readS32, "readS32", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readU64", JS_NewCFunction(ctx, js_mem_readU64, "readU64", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readS64", JS_NewCFunction(ctx, js_mem_readS64, "readS64", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readF32", JS_NewCFunction(ctx, js_mem_readF32, "readF32", 1));
    JS_SetPropertyStr(ctx, ns_mem, "readF64", JS_NewCFunction(ctx, js_mem_readF64, "readF64", 1));

    JS_SetPropertyStr(ctx, ns_mem, "writeU8", JS_NewCFunction(ctx, js_mem_writeU8, "writeU8", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeS8", JS_NewCFunction(ctx, js_mem_writeS8, "writeS8", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeU16", JS_NewCFunction(ctx, js_mem_writeU16, "writeU16", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeS16", JS_NewCFunction(ctx, js_mem_writeS16, "writeS16", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeU32", JS_NewCFunction(ctx, js_mem_writeU32, "writeU32", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeS32", JS_NewCFunction(ctx, js_mem_writeS32, "writeS32", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeU64", JS_NewCFunction(ctx, js_mem_writeU64, "writeU64", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeS64", JS_NewCFunction(ctx, js_mem_writeS64, "writeS64", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeF32", JS_NewCFunction(ctx, js_mem_writeF32, "writeF32", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeF64", JS_NewCFunction(ctx, js_mem_writeF64, "writeF64", 2));

    JS_SetPropertyStr(ctx, ns_mem, "readBytes", JS_NewCFunction(ctx, js_mem_readBytes, "readBytes", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeBytes", JS_NewCFunction(ctx, js_mem_writeBytes, "writeBytes", 2));
    JS_SetPropertyStr(ctx, ns_mem, "readCString", JS_NewCFunction(ctx, js_mem_readCString, "readCString", 2));
    JS_SetPropertyStr(ctx, ns_mem, "writeCString", JS_NewCFunction(ctx, js_mem_writeCString, "writeCString", 2));
    JS_SetPropertyStr(ctx, ns_mem, "isMapped", JS_NewCFunction(ctx, js_mem_isMapped, "isMapped", 2));
    JS_SetPropertyStr(ctx, ns_mem, "scan", JS_NewCFunction(ctx, js_mem_scan, "scan", 3));
    JS_SetPropertyStr(ctx, ns_mem, "alloc", JS_NewCFunction(ctx, js_mem_alloc, "alloc", 1));
    JS_SetPropertyStr(ctx, ns_mem, "free", JS_NewCFunction(ctx, js_mem_free, "free", 1));

    JS_SetPropertyStr(ctx, ns_native, "getAiClass", JS_NewCFunction(ctx, js_native_ai_class, "getAiClass", 1));
    JS_SetPropertyStr(ctx, ns_native, "demangleSymbol", JS_NewCFunction(ctx, js_native_demangle_symbol, "demangleSymbol", 1));
    JS_SetPropertyStr(ctx, ns_native, "listFunctions", JS_NewCFunction(ctx, js_native_list_functions, "listFunctions", 2));
    JS_SetPropertyStr(ctx, ns_native, "hasFunction", JS_NewCFunction(ctx, js_native_has_fn, "hasFunction", 1));
    JS_SetPropertyStr(ctx, ns_native, "call", JS_NewCFunction(ctx, js_native_call, "call", 2));
    JS_SetPropertyStr(ctx, ns_native, "hook", JS_NewCFunction(ctx, js_native_hook, "hook", 2));
    JS_SetPropertyStr(ctx, ns_native, "unhook", JS_NewCFunction(ctx, js_native_unhook, "unhook", 1));
    JS_SetPropertyStr(ctx, ns_native, "createCallback", JS_NewCFunction(ctx, js_native_create_cb, "createCallback", 1));

    JSValue addrs = JS_NewObject(ctx);
    static const struct { const char *name; uint64_t val; } kAddrs[] = {
        {"PouchCountQuery", 0x82a3c260ULL},
        {"BancChangeEventArgQuery", 0x829df380ULL},
        {"TimelineStart", 0x83b5bfb0ULL},
        {"MostRecentEventSkipQuery", 0x80de6cb0ULL},
        {"TimeManagerSetTime", 0x80f0e640ULL},
        {"PlayerCameraUpdate", 0x80644c80ULL},
        {"TextPaneAdvance", 0x80b1e140ULL},
        {"OcarinaNoteWaitStart", 0x828a9190ULL},
        {"OcarinaNoteWaitTick", 0x828a9280ULL},
        {"VoiceResourceUpdate", 0x80a9ca30ULL},
        {"MoviePlayerUpdate", 0x83b7c710ULL},
        {"AnimationEntryAdvance", 0x802cfca0ULL},
        {"SkeletonPoseTimeUpdate", 0x80944aa0ULL},
        {"FrameStepUpdate", 0x80706be0ULL},
        {"GlobalStepUpdate", 0x83b6ea90ULL},
        {"GetSystemTick", 0x85106370ULL},
        {"GetSystemTickFrequency", 0x851063a0ULL},
        {"GameDataMgrSlot", 0x84c9e180ULL},
        {"EventMgrSlot", 0x84c9e090ULL},
        {"WorldMgrSlot", 0x84c9e898ULL},
        {"CameraMgrSlot", 0x84ca6bc0ULL},
        {"ActorMgrSlot", 0x84ca24c0ULL},
        {"PouchMgrSlot", 0x84e98e28ULL},
        {"PouchSingletonSlot", 0x84c9e0d0ULL},
        {"GetBool", 0x8022d9c0ULL},
        {"SetBoolPersist", 0x806a2b20ULL},
        {"GetString64", 0x80cd0ea0ULL},
        {"RequestEvent", 0x80d9a820ULL},
        {"EventRequestArgCtor", 0x80d99f10ULL},
        {"ActorLinkSet", 0x802dc440ULL},
        {"ActorLinkRelease", 0x8022b6f0ULL},
        {"SetTime", 0x834722d0ULL},
        {"GetTime", 0x83472280ULL},
        {"PouchAddRemove", 0x828d4a70ULL},
        {"PouchCount", 0x805b63e0ULL},
        {"ChangePosition", 0x806d2530ULL},
    };
    for (size_t i = 0; i < sizeof kAddrs / sizeof kAddrs[0]; i++)
        JS_SetPropertyStr(ctx, addrs, kAddrs[i].name, JS_NewBigUint64(ctx, kAddrs[i].val));
    JS_SetPropertyStr(ctx, ns_native, "addresses", addrs);
}

void js_memory_reset(JSContext *ctx) {
    int had = 0;
#ifdef LONGSHOT_ASI
    void *mh_targets[JS_HOOK_MAX];
    int mh_n = 0;
#endif
    AcquireSRWLockExclusive(&g_hook_lock);
    for (int i = 0; i < JS_HOOK_MAX; i++) {
        if (g_hooks[i].used) {
            had = 1;
#ifdef LONGSHOT_ASI
            if (g_hooks[i].mh_target) mh_targets[mh_n++] = g_hooks[i].mh_target;
            g_hooks[i].mh_target = NULL;
#endif
            if (ctx) {
                JS_FreeValue(ctx, g_hooks[i].before_fn);
                JS_FreeValue(ctx, g_hooks[i].after_fn);
            }
            g_hooks[i].before_fn = JS_UNDEFINED;
            g_hooks[i].after_fn = JS_UNDEFINED;
            g_hooks[i].used = 0;
            g_hooks[i].addr = 0;
            g_hooks[i].orig = NULL;
        }
    }
    for (int i = 0; i < JS_CB_MAX; i++) {
        if (g_cbs[i].used) {
            had = 1;
            if (ctx) JS_FreeValue(ctx, g_cbs[i].fn);
            g_cbs[i].fn = JS_UNDEFINED;
            g_cbs[i].used = 0;
        }
    }
    __atomic_store_n(&g_hook_active, 0, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&g_hook_lock);
#ifdef LONGSHOT_ASI
    for (int i = 0; i < mh_n; i++) {
        MH_DisableHook(mh_targets[i]);
        MH_RemoveHook(mh_targets[i]);
    }
    (void)had;
#else
    if (had) a64_rt_invalidate_sites(0);
#endif
    g_ctx_class_id = 0;
    arena_reset();
}
