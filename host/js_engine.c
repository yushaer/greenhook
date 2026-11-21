/* Core QuickJS-NG engine, script/mod loader, ES module resolver, console ring buffer & Control Bridge REPL. */
#include "js_internal.h"
#include <ctype.h>
#include <sys/stat.h>

#define JS_MAX_MEM_BYTES (64u * 1024u * 1024u)
#define JS_MAX_STACK_BYTES (1024u * 1024u)
#define JS_LOG_RING 256
#define JS_SCRIPT_MAX MKWVK_JS_MAX_SCRIPTS

typedef struct {
    char id[64];
    char name[64];
    char version[24];
    char author[48];
    char description[192];
    char entry_path[MAX_PATH];
    char root_dir[MAX_PATH];
    char error[160];
    int enabled;
    int loaded;
} JsScriptInfo;

JSRuntime *g_js_rt = NULL;
JSContext *g_js_ctx = NULL;
_Thread_local Ctx *g_js_tls_ctx = NULL;
volatile LONG g_js_ready = 0;

static INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_js_cs;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static SRWLOCK g_meta_lock = SRWLOCK_INIT;

static int g_enter_depth = 0;
static int64_t g_entry_start_ns = 0;
static int64_t g_default_timeout_ms = 2000;
static int64_t g_active_timeout_ms = 2000;
static volatile LONG g_reload_pending = 0;
static volatile LONG g_shutting_down = 0;

static char g_scripts_dir[MAX_PATH];
static char g_data_dir[MAX_PATH];
static char g_extra_roots[32][MAX_PATH];
static int g_extra_root_count = 0;

static JsScriptInfo g_scripts[JS_SCRIPT_MAX];
static int g_script_count = 0;

static char g_disabled_ids[128][64];
static int g_disabled_count = 0;

static MkwVkJsLogLine g_log_ring[JS_LOG_RING];
static unsigned g_log_write = 0;
static int32_t g_log_seq = 0;

/* Track last-known BGM string (populated in js_oot.c, readable from js_audio) */
static char g_cached_bgm[64];
void js_set_current_bgm(const char *s) {
    AcquireSRWLockExclusive(&g_meta_lock);
    snprintf(g_cached_bgm, sizeof g_cached_bgm, "%s", s ? s : "");
    ReleaseSRWLockExclusive(&g_meta_lock);
}
const char *js_current_bgm(void) {
    return g_cached_bgm;
}

static BOOL CALLBACK init_cs_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once, (void)param, (void)ctx;
    InitializeCriticalSection(&g_js_cs);
    return TRUE;
}

static void ensure_cs(void) {
    InitOnceExecuteOnce(&g_cs_once, init_cs_cb, NULL, NULL);
}

static int js_watchdog_handler(JSContext *ctx, void *opaque) {
    (void)ctx, (void)opaque;
    if (g_active_timeout_ms <= 0 || g_entry_start_ns <= 0) return 0;
    const int64_t elapsed_ms = (hos_now_ns() - g_entry_start_ns) / 1000000LL;
    return elapsed_ms >= g_active_timeout_ms;
}

static int js_do_reload_locked(void);

int js_enter(void) {
    if (InterlockedCompareExchange(&g_shutting_down, 0, 0)) return 0;
    ensure_cs();
    EnterCriticalSection(&g_js_cs);
    if (!g_js_rt || !g_js_ctx || !InterlockedCompareExchange(&g_js_ready, 0, 0)) {
        LeaveCriticalSection(&g_js_cs);
        return 0;
    }
    JS_UpdateStackTop(g_js_rt);
    if (g_enter_depth++ == 0) {
        g_entry_start_ns = hos_now_ns();
        g_active_timeout_ms = g_default_timeout_ms;
    }
    return 1;
}

void js_leave(void) {
    if (g_enter_depth > 0 && --g_enter_depth == 0) {
        g_entry_start_ns = 0;
        if (InterlockedExchange(&g_reload_pending, 0) && !InterlockedCompareExchange(&g_shutting_down, 0, 0)) {
            js_do_reload_locked();
        }
    }
    LeaveCriticalSection(&g_js_cs);
}

void js_request_reload(void) {
    InterlockedExchange(&g_reload_pending, 1);
}

void js_console_push(int level, const char *source, const char *msg) {
    AcquireSRWLockExclusive(&g_log_lock);
    const unsigned slot = g_log_write++ % JS_LOG_RING;
    MkwVkJsLogLine *e = &g_log_ring[slot];
    e->seq = ++g_log_seq;
    e->level = level;
    snprintf(e->source, sizeof e->source, "%s", source ? source : "js");
    snprintf(e->text, sizeof e->text, "%s", msg ? msg : "");
    ReleaseSRWLockExclusive(&g_log_lock);
}

void js_console_clear(void) {
    AcquireSRWLockExclusive(&g_log_lock);
    g_log_write = 0;
    memset(g_log_ring, 0, sizeof g_log_ring);
    ReleaseSRWLockExclusive(&g_log_lock);
}

int js_console_copy(MkwVkJsLogLine *out, int max) {
    AcquireSRWLockShared(&g_log_lock);
    const unsigned total = g_log_write < JS_LOG_RING ? g_log_write : JS_LOG_RING;
    int take = (int)total < max ? (int)total : max;
    const unsigned start = g_log_write - (unsigned)take;
    for (int i = 0; i < take; i++) {
        out[i] = g_log_ring[(start + (unsigned)i) % JS_LOG_RING];
    }
    ReleaseSRWLockShared(&g_log_lock);
    return take;
}

void js_drain_jobs(JSContext *ctx) {
    JSContext *pctx = NULL;
    for (int iter = 0; iter < 1024; iter++) {
        const int r = JS_ExecutePendingJob(JS_GetRuntime(ctx), &pctx);
        if (r == 0) break;
        if (r < 0) {
            js_log_exception(pctx ? pctx : ctx, "promise");
            break;
        }
    }
}

void js_log_exception(JSContext *ctx, const char *where) {
    JSValue ex = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, ex);
    JSValue st = JS_GetPropertyStr(ctx, ex, "stack");
    const char *stack = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    char full[512];
    if (stack && *stack) {
        snprintf(full, sizeof full, "%s\n%s", msg ? msg : "Error", stack);
    } else {
        snprintf(full, sizeof full, "%s", msg ? msg : "Error");
    }
    js_console_push(3, where ? where : "error", full);
    hos_log("js [%s]: %s", where ? where : "error", msg ? msg : "Error");
    if (stack) JS_FreeCString(ctx, stack);
    if (msg) JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, st);
    JS_FreeValue(ctx, ex);
}

void js_json_escape_append(char *dst, size_t cap, size_t *pos, const char *s) {
    if (!s) s = "";
    const unsigned char *p = (const unsigned char *)s;
    while (*p && *pos + 8 < cap) {
        const unsigned char c = *p++;
        if (c == '"' || c == '\\') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = (char)c;
        } else if (c == '\n') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'n';
        } else if (c == '\r') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'r';
        } else if (c == '\t') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 't';
        } else if (c < 0x20) {
            *pos += (size_t)snprintf(dst + *pos, cap - *pos, "\\u%04x", c);
        } else {
            dst[(*pos)++] = (char)c;
        }
    }
    if (*pos < cap) dst[*pos] = 0;
}

const char *js_scripts_dir(void) { return g_scripts_dir; }
const char *js_data_dir(void) { return g_data_dir; }

static void resolve_dirs(void) {
    const char *env_js = getenv("HOS_JS_DIR");
    if (env_js && *env_js) {
        GetFullPathNameA(env_js, sizeof g_scripts_dir, g_scripts_dir, NULL);
    } else {
        char base[MAX_PATH];
        const DWORD n = hos_game_path_a(base, sizeof base);
        char *slash = NULL;
        for (DWORD i = 0; i < n && base[i]; i++)
            if (base[i] == '\\' || base[i] == '/') slash = &base[i];
        if (slash) *slash = 0;
        else strcpy(base, ".");
        snprintf(g_scripts_dir, sizeof g_scripts_dir, "%s\\scripts", base);
    }

    const char *env_data = getenv("HOS_JS_DATA_DIR");
    if (env_data && *env_data) {
        GetFullPathNameA(env_data, sizeof g_data_dir, g_data_dir, NULL);
    } else {
        char base[MAX_PATH];
        const DWORD n = hos_game_path_a(base, sizeof base);
        char *slash = NULL;
        for (DWORD i = 0; i < n && base[i]; i++)
            if (base[i] == '\\' || base[i] == '/') slash = &base[i];
        if (slash) *slash = 0;
        else strcpy(base, ".");
        snprintf(g_data_dir, sizeof g_data_dir, "%s\\scripts_data", base);
    }
}

static char *read_host_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long sz = ftell(f);
    if (sz < 0 || sz > 16L * 1024L * 1024L || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t got = sz > 0 ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

static int path_is_under(const char *path, const char *root) {
    char full_p[MAX_PATH], full_r[MAX_PATH];
    if (!GetFullPathNameA(path, sizeof full_p, full_p, NULL)) return 0;
    if (!GetFullPathNameA(root, sizeof full_r, full_r, NULL)) return 0;
    size_t rlen = strlen(full_r);
    while (rlen > 0 && (full_r[rlen - 1] == '\\' || full_r[rlen - 1] == '/')) full_r[--rlen] = 0;
    if (!rlen) return 0;
    if (_strnicmp(full_p, full_r, rlen) != 0) return 0;
    return full_p[rlen] == 0 || full_p[rlen] == '\\' || full_p[rlen] == '/';
}

static int is_allowed_script_path(const char *path) {
    if (g_scripts_dir[0] && path_is_under(path, g_scripts_dir)) return 1;
    for (int i = 0; i < g_extra_root_count; i++)
        if (g_extra_roots[i][0] && path_is_under(path, g_extra_roots[i])) return 1;
    return 0;
}

static int is_virtual_module(const char *name) {
    return !strcmp(name, "longshot") || !strcmp(name, "longshot/game") || !strcmp(name, "longshot/ui") ||
           !strcmp(name, "longshot/memory") || !strcmp(name, "longshot/native") || !strcmp(name, "longshot/fs") ||
           !strcmp(name, "longshot/events");
}

static char *js_module_normalize(JSContext *ctx, const char *base_name, const char *name, void *opaque) {
    (void)opaque;
    if (!strcmp(name, "greenhook/data")) name = "lib/greenhook-data.js";
    if (!strcmp(name, "greenhook")) name = "longshot";
    if (!strncmp(name, "greenhook/", 10)) {
        char legacy[32];
        int n = snprintf(legacy, sizeof legacy, "longshot/%s", name + 10);
        if (n > 0 && n < (int)sizeof legacy && is_virtual_module(legacy)) return js_strdup(ctx, legacy);
    }
    if (is_virtual_module(name)) return js_strdup(ctx, name);

    char combined[MAX_PATH * 2];
    if (name[0] == '.' && (name[1] == '/' || name[1] == '\\' || (name[1] == '.' && (name[2] == '/' || name[2] == '\\')))) {
        char base_dir[MAX_PATH];
        snprintf(base_dir, sizeof base_dir, "%s", base_name ? base_name : g_scripts_dir);
        char *slash = NULL;
        for (char *p = base_dir; *p; p++)
            if (*p == '\\' || *p == '/') slash = p;
        if (slash && !strchr(base_name ? base_name : "", '<')) *slash = 0;
        else snprintf(base_dir, sizeof base_dir, "%s", g_scripts_dir);
        snprintf(combined, sizeof combined, "%s\\%s", base_dir, name);
    } else if ((isalpha((unsigned char)name[0]) && name[1] == ':') || name[0] == '\\' || name[0] == '/') {
        snprintf(combined, sizeof combined, "%s", name);
    } else {
        snprintf(combined, sizeof combined, "%s\\%s", g_scripts_dir, name);
    }
    for (char *p = combined; *p; p++)
        if (*p == '/') *p = '\\';

    char resolved[MAX_PATH];
    if (!GetFullPathNameA(combined, sizeof resolved, resolved, NULL)) {
        JS_ThrowReferenceError(ctx, "invalid module path: %s", name);
        return NULL;
    }
    if (!is_allowed_script_path(resolved)) {
        JS_ThrowReferenceError(ctx, "module import outside scripts directory is forbidden: %s", name);
        return NULL;
    }
    return js_strdup(ctx, resolved);
}

static int js_builtin_module_init(JSContext *ctx, JSModuleDef *m) {
    const JSAtom atom = JS_GetModuleName(ctx, m);
    const char *mname = JS_AtomToCString(ctx, atom);
    JSValue global = JS_GetGlobalObject(ctx);

    if (mname && !strcmp(mname, "longshot")) {
        static const char *const names[] = {"Greenhook", "Longshot", "game", "ui", "memory", "native", "fs", "events"};
        JSValue def = JS_NewObject(ctx);
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            JSValue v = JS_GetPropertyStr(ctx, global, names[i]);
            JS_SetModuleExport(ctx, m, names[i], JS_DupValue(ctx, v));
            JS_SetPropertyStr(ctx, def, names[i], v);
        }
        JS_SetModuleExport(ctx, m, "default", def);
    } else if (mname && !strncmp(mname, "longshot/", 9)) {
        const char *sub = mname + 9;
        JSValue v = JS_GetPropertyStr(ctx, global, sub);
        JS_SetModuleExport(ctx, m, sub, JS_DupValue(ctx, v));
        JS_SetModuleExport(ctx, m, "default", v);
    }

    if (mname) JS_FreeCString(ctx, mname);
    JS_FreeValue(ctx, global);
    JS_FreeAtom(ctx, atom);
    return 0;
}

static int js_json_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue v = JS_GetModulePrivateValue(ctx, m);
    JS_SetModuleExport(ctx, m, "default", v);
    return 0;
}

static int set_module_meta(JSContext *ctx, JSValueConst func_val, const char *filename, int is_main) {
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta)) return -1;
    char url[MAX_PATH + 16];
    snprintf(url, sizeof url, "file:///%s", filename);
    for (char *p = url; *p; p++)
        if (*p == '\\') *p = '/';
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "main", JS_NewBool(ctx, is_main != 0), JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta);
    return 0;
}

static JSModuleDef *js_module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    (void)opaque;
    if (!strcmp(module_name, "longshot")) {
        JSModuleDef *m = JS_NewCModule(ctx, module_name, js_builtin_module_init);
        if (!m) return NULL;
        JS_AddModuleExport(ctx, m, "Greenhook");
        JS_AddModuleExport(ctx, m, "Longshot");
        JS_AddModuleExport(ctx, m, "game");
        JS_AddModuleExport(ctx, m, "ui");
        JS_AddModuleExport(ctx, m, "memory");
        JS_AddModuleExport(ctx, m, "native");
        JS_AddModuleExport(ctx, m, "fs");
        JS_AddModuleExport(ctx, m, "events");
        JS_AddModuleExport(ctx, m, "default");
        return m;
    }
    if (!strncmp(module_name, "longshot/", 9) && is_virtual_module(module_name)) {
        JSModuleDef *m = JS_NewCModule(ctx, module_name, js_builtin_module_init);
        if (!m) return NULL;
        JS_AddModuleExport(ctx, m, module_name + 9);
        JS_AddModuleExport(ctx, m, "default");
        return m;
    }
    if (!is_allowed_script_path(module_name)) {
        JS_ThrowReferenceError(ctx, "module outside allowed directory: %s", module_name);
        return NULL;
    }
    size_t len = 0;
    char *buf = read_host_file(module_name, &len);
    if (!buf) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return NULL;
    }
    const size_t nlen = strlen(module_name);
    if (nlen > 5 && !_stricmp(module_name + nlen - 5, ".json")) {
        JSValue parsed = JS_ParseJSON(ctx, buf, len, module_name);
        free(buf);
        if (JS_IsException(parsed)) return NULL;
        JSModuleDef *m = JS_NewCModule(ctx, module_name, js_json_module_init);
        if (!m) {
            JS_FreeValue(ctx, parsed);
            return NULL;
        }
        JS_AddModuleExport(ctx, m, "default");
        JS_SetModulePrivateValue(ctx, m, parsed);
        return m;
    }
    JSValue func_val = JS_Eval(ctx, buf, len, module_name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(buf);
    if (JS_IsException(func_val)) return NULL;
    if (set_module_meta(ctx, func_val, module_name, 0) < 0) {
        JS_FreeValue(ctx, func_val);
        return NULL;
    }
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    return m;
}

/* ---- console & TextEncoder / TextDecoder ---- */
static void append_val_text(JSContext *ctx, JSValueConst v, char *dst, size_t cap, size_t *pos) {
    if (*pos + 2 >= cap) return;
    if (JS_IsString(v)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, v);
        if (s) {
            const size_t room = cap - 1 - *pos;
            const size_t take = len < room ? len : room;
            memcpy(dst + *pos, s, take);
            *pos += take;
            dst[*pos] = 0;
            JS_FreeCString(ctx, s);
        }
        return;
    }
    if (JS_IsBigInt(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            *pos += (size_t)snprintf(dst + *pos, cap - *pos, "%sn", s);
            JS_FreeCString(ctx, s);
        }
        return;
    }
    if (JS_IsObject(v) && !JS_IsNull(v) && !JS_IsFunction(ctx, v)) {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json) && JS_IsString(json)) {
            const char *js = JS_ToCString(ctx, json);
            if (js) {
                *pos += (size_t)snprintf(dst + *pos, cap - *pos, "%s", js);
                JS_FreeCString(ctx, js);
                JS_FreeValue(ctx, json);
                return;
            }
        }
        if (JS_IsException(json)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, json);
    }
    const char *s = JS_ToCString(ctx, v);
    if (s) {
        *pos += (size_t)snprintf(dst + *pos, cap - *pos, "%s", s);
        JS_FreeCString(ctx, s);
    }
}

static JSValue console_emit(JSContext *ctx, int level, int argc, JSValueConst *argv) {
    char buf[512] = "";
    size_t pos = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && pos + 2 < sizeof buf) buf[pos++] = ' ';
        append_val_text(ctx, argv[i], buf, sizeof buf, &pos);
    }
    buf[pos < sizeof buf ? pos : sizeof buf - 1] = 0;
    js_console_push(level, level == 3 ? "error" : level == 2 ? "warn" : "script", buf);
    hos_log("js: %s", buf);
    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return console_emit(ctx, 0, argc, argv);
}
static JSValue js_console_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return console_emit(ctx, 1, argc, argv);
}
static JSValue js_console_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return console_emit(ctx, 2, argc, argv);
}
static JSValue js_console_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return console_emit(ctx, 3, argc, argv);
}
static JSValue js_console_clear_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    js_console_clear();
    return JS_UNDEFINED;
}

static JSValue js_text_encoder_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    JSValue arr = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return arr;
}
static JSValue js_text_encoder_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    (void)new_target, (void)argc, (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    JS_SetPropertyStr(ctx, obj, "encode", JS_NewCFunction(ctx, js_text_encoder_encode, "encode", 1));
    return obj;
}
static JSValue js_text_decoder_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    size_t len = 0;
    uint8_t *p = JS_GetUint8Array(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        p = JS_GetArrayBuffer(ctx, &len, argv[0]);
        if (!p) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "TextDecoder.decode expects Uint8Array or ArrayBuffer");
        }
    }
    return JS_NewStringLen(ctx, (const char *)p, len);
}
static JSValue js_text_decoder_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    (void)new_target, (void)argc, (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, "utf-8"));
    JS_SetPropertyStr(ctx, obj, "decode", JS_NewCFunction(ctx, js_text_decoder_decode, "decode", 1));
    return obj;
}

/* ---- Script discovery & scripts.json ---- */
static void load_disabled_list(JSContext *ctx) {
    g_disabled_count = 0;
    char cfg[MAX_PATH];
    snprintf(cfg, sizeof cfg, "%s\\scripts.json", g_scripts_dir);
    size_t len = 0;
    char *raw = read_host_file(cfg, &len);
    if (!raw) return;
    JSValue parsed = JS_ParseJSON(ctx, raw, len, cfg);
    free(raw);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    JSValue arr = JS_GetPropertyStr(ctx, parsed, "disabled");
    if (JS_IsArray(arr)) {
        JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < n && g_disabled_count < 128; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, arr, i);
            const char *s = JS_ToCString(ctx, item);
            if (s && *s) snprintf(g_disabled_ids[g_disabled_count++], 64, "%s", s);
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, item);
        }
    }
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, parsed);
}

static void save_disabled_list(void) {
    CreateDirectoryA(g_scripts_dir, NULL);
    char cfg[MAX_PATH];
    snprintf(cfg, sizeof cfg, "%s\\scripts.json", g_scripts_dir);
    FILE *f = fopen(cfg, "wb");
    if (!f) return;
    fputs("{\n  \"disabled\": [", f);
    for (int i = 0; i < g_disabled_count; i++) {
        char esc[160];
        size_t pos = 0;
        js_json_escape_append(esc, sizeof esc, &pos, g_disabled_ids[i]);
        fprintf(f, "%s\"%s\"", i ? ", " : "", esc);
    }
    fputs("]\n}\n", f);
    fclose(f);
}

static int is_id_disabled(const char *id) {
    for (int i = 0; i < g_disabled_count; i++)
        if (!_stricmp(g_disabled_ids[i], id)) return 1;
    return 0;
}

static int script_cmp(const void *a, const void *b) {
    return _stricmp(((const JsScriptInfo *)a)->id, ((const JsScriptInfo *)b)->id);
}

static void add_script_candidate(JSContext *ctx, const char *id, const char *entry_path, const char *root_dir,
                                 const char *manifest_path) {
    if (g_script_count >= JS_SCRIPT_MAX) return;
    for (int i = 0; i < g_script_count; i++)
        if (!_stricmp(g_scripts[i].id, id)) return;
    if (GetFileAttributesA(entry_path) == INVALID_FILE_ATTRIBUTES) return;

    JsScriptInfo *s = &g_scripts[g_script_count++];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", id);
    snprintf(s->name, sizeof s->name, "%s", id);
    snprintf(s->version, sizeof s->version, "1.0.0");
    snprintf(s->entry_path, sizeof s->entry_path, "%s", entry_path);
    snprintf(s->root_dir, sizeof s->root_dir, "%s", root_dir);
    s->enabled = !is_id_disabled(id);

    if (manifest_path && GetFileAttributesA(manifest_path) != INVALID_FILE_ATTRIBUTES) {
        size_t mlen = 0;
        char *mbuf = read_host_file(manifest_path, &mlen);
        if (mbuf) {
            JSValue m = JS_ParseJSON(ctx, mbuf, mlen, manifest_path);
            free(mbuf);
            if (!JS_IsException(m) && JS_IsObject(m)) {
                struct { const char *k; char *dst; size_t cap; } fields[] = {
                    {"name", s->name, sizeof s->name},
                    {"version", s->version, sizeof s->version},
                    {"author", s->author, sizeof s->author},
                    {"description", s->description, sizeof s->description},
                };
                for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
                    JSValue fv = JS_GetPropertyStr(ctx, m, fields[i].k);
                    if (JS_IsString(fv)) {
                        const char *fs = JS_ToCString(ctx, fv);
                        if (fs && *fs) snprintf(fields[i].dst, fields[i].cap, "%s", fs);
                        if (fs) JS_FreeCString(ctx, fs);
                    }
                    JS_FreeValue(ctx, fv);
                }
                JSValue en = JS_GetPropertyStr(ctx, m, "entry");
                if (JS_IsString(en)) {
                    const char *es = JS_ToCString(ctx, en);
                    if (es && *es && !strstr(es, "..") && !strchr(es, ':')) {
                        snprintf(s->entry_path, sizeof s->entry_path, "%s\\%s", root_dir, es);
                        for (char *p = s->entry_path; *p; p++)
                            if (*p == '/') *p = '\\';
                    }
                    if (es) JS_FreeCString(ctx, es);
                }
                JS_FreeValue(ctx, en);
                JSValue enb = JS_GetPropertyStr(ctx, m, "enabled");
                if (JS_IsBool(enb) && !JS_ToBool(ctx, enb)) s->enabled = 0;
                JS_FreeValue(ctx, enb);
            } else if (JS_IsException(m)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, m);
        }
    }
}

static void scan_scripts_folder(JSContext *ctx, const char *dir, const char *id_prefix) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_' || fd.cFileName[0] == '-') continue;
        char full[MAX_PATH];
        snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!_stricmp(fd.cFileName, "lib") || !_stricmp(fd.cFileName, "shared") ||
                !_stricmp(fd.cFileName, "node_modules"))
                continue;
            char manifest[MAX_PATH], entry[MAX_PATH], id[64];
            snprintf(manifest, sizeof manifest, "%s\\mod.json", full);
            snprintf(entry, sizeof entry, "%s\\main.js", full);
            if (GetFileAttributesA(entry) == INVALID_FILE_ATTRIBUTES)
                snprintf(entry, sizeof entry, "%s\\index.js", full);
            snprintf(id, sizeof id, "%s%s", id_prefix, fd.cFileName);
            add_script_candidate(ctx, id, entry, full, manifest);
        } else {
            const size_t len = strlen(fd.cFileName);
            if (len > 3 && !_stricmp(fd.cFileName + len - 3, ".js")) {
                char id[64];
                snprintf(id, sizeof id, "%s%s", id_prefix, fd.cFileName);
                add_script_candidate(ctx, id, full, dir, NULL);
            }
        }
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static void discover_all_scripts(JSContext *ctx) {
    AcquireSRWLockExclusive(&g_meta_lock);
    g_script_count = 0;
    g_extra_root_count = 0;
    scan_scripts_folder(ctx, g_scripts_dir, "");
    const int nmods = hos_mods_count();
    for (int i = 0; i < nmods; i++) {
        char mdir[MAX_PATH];
        const char *mname = NULL;
        if (hos_mod_scripts_dir(i, mdir, sizeof mdir, &mname)) {
            if (g_extra_root_count < 32) snprintf(g_extra_roots[g_extra_root_count++], MAX_PATH, "%s", mdir);
            char prefix[64];
            snprintf(prefix, sizeof prefix, "mod:%s/", mname ? mname : "mod");
            scan_scripts_folder(ctx, mdir, prefix);
        }
    }
    if (g_script_count > 1) qsort(g_scripts, (size_t)g_script_count, sizeof g_scripts[0], script_cmp);
    ReleaseSRWLockExclusive(&g_meta_lock);
}

/* ---- Longshot global API ---- */
static JSValue js_longshot_reload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    js_request_reload();
    return JS_TRUE;
}

static JSValue js_longshot_scripts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSValue arr = JS_NewArray(ctx);
    AcquireSRWLockShared(&g_meta_lock);
    for (int i = 0; i < g_script_count; i++) {
        const JsScriptInfo *s = &g_scripts[i];
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "id", JS_NewString(ctx, s->id));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, s->name));
        JS_SetPropertyStr(ctx, o, "version", JS_NewString(ctx, s->version));
        JS_SetPropertyStr(ctx, o, "author", JS_NewString(ctx, s->author));
        JS_SetPropertyStr(ctx, o, "description", JS_NewString(ctx, s->description));
        JS_SetPropertyStr(ctx, o, "path", JS_NewString(ctx, s->entry_path));
        JS_SetPropertyStr(ctx, o, "enabled", JS_NewBool(ctx, s->enabled));
        JS_SetPropertyStr(ctx, o, "loaded", JS_NewBool(ctx, s->loaded));
        JS_SetPropertyStr(ctx, o, "error", JS_NewString(ctx, s->error));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    ReleaseSRWLockShared(&g_meta_lock);
    return arr;
}

static JSValue js_longshot_set_enabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setScriptEnabled(id, enabled)");
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    const int on = JS_ToBool(ctx, argv[1]);
    const int found = js_set_script_enabled(id, on);
    JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, found);
}

static JSValue js_longshot_stats(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &mu);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "memoryUsedBytes", JS_NewInt64(ctx, mu.memory_used_size));
    JS_SetPropertyStr(ctx, o, "mallocSize", JS_NewInt64(ctx, mu.malloc_size));
    JS_SetPropertyStr(ctx, o, "objectCount", JS_NewInt64(ctx, mu.obj_count));
    JS_SetPropertyStr(ctx, o, "atomCount", JS_NewInt64(ctx, mu.atom_count));
    return o;
}

static void register_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue cons = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cons, "log", JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, cons, "info", JS_NewCFunction(ctx, js_console_info, "info", 0));
    JS_SetPropertyStr(ctx, cons, "warn", JS_NewCFunction(ctx, js_console_warn, "warn", 0));
    JS_SetPropertyStr(ctx, cons, "error", JS_NewCFunction(ctx, js_console_error, "error", 0));
    JS_SetPropertyStr(ctx, cons, "debug", JS_NewCFunction(ctx, js_console_log, "debug", 0));
    JS_SetPropertyStr(ctx, cons, "clear", JS_NewCFunction(ctx, js_console_clear_fn, "clear", 0));
    JS_SetPropertyStr(ctx, global, "console", cons);

    JS_SetPropertyStr(ctx, global, "TextEncoder", JS_NewCFunction2(ctx, js_text_encoder_ctor, "TextEncoder", 0, JS_CFUNC_constructor, 0));
    JS_SetPropertyStr(ctx, global, "TextDecoder", JS_NewCFunction2(ctx, js_text_decoder_ctor, "TextDecoder", 0, JS_CFUNC_constructor, 0));

    JSValue ls = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ls, "version", JS_NewString(ctx, "0.1.0-beta.2"));
    JS_SetPropertyStr(ctx, ls, "gameBuild", JS_NewString(ctx, "e0f3ccfa"));
    JS_SetPropertyStr(ctx, ls, "scriptsDir", JS_NewString(ctx, g_scripts_dir));
    JS_SetPropertyStr(ctx, ls, "dataDir", JS_NewString(ctx, g_data_dir));
    JS_SetPropertyStr(ctx, ls, "reload", JS_NewCFunction(ctx, js_longshot_reload, "reload", 0));
    JS_SetPropertyStr(ctx, ls, "scripts", JS_NewCFunction(ctx, js_longshot_scripts, "scripts", 0));
    JS_SetPropertyStr(ctx, ls, "setScriptEnabled", JS_NewCFunction(ctx, js_longshot_set_enabled, "setScriptEnabled", 2));
    JS_SetPropertyStr(ctx, ls, "stats", JS_NewCFunction(ctx, js_longshot_stats, "stats", 0));

    JSValue ns_mem = JS_NewObject(ctx);
    JSValue ns_native = JS_NewObject(ctx);
    JSValue ns_game = JS_NewObject(ctx);
    JSValue ns_fs = JS_NewObject(ctx);
    JSValue ns_events = JS_NewObject(ctx);
    JSValue ns_ui = JS_NewObject(ctx);

    js_register_memory(ctx, ns_mem, ns_native);
    js_register_oot(ctx, ns_game);
    js_register_io(ctx, ns_game, ns_fs, ns_events, global);
    js_register_ui(ctx, ns_ui);

    JS_SetPropertyStr(ctx, global, "Greenhook", JS_DupValue(ctx, ls));
    JS_SetPropertyStr(ctx, global, "Longshot", ls);
    JS_SetPropertyStr(ctx, global, "memory", ns_mem);
    JS_SetPropertyStr(ctx, global, "native", ns_native);
    JS_SetPropertyStr(ctx, global, "game", ns_game);
    JS_SetPropertyStr(ctx, global, "fs", ns_fs);
    JS_SetPropertyStr(ctx, global, "events", ns_events);
    JS_SetPropertyStr(ctx, global, "ui", ns_ui);

    JS_FreeValue(ctx, global);
}

static void load_discovered_scripts(JSContext *ctx) {
    int loaded_ok = 0;
    for (int i = 0; i < g_script_count; i++) {
        JsScriptInfo *s = &g_scripts[i];
        if (!s->enabled) continue;
        size_t len = 0;
        char *code = read_host_file(s->entry_path, &len);
        if (!code) {
            snprintf(s->error, sizeof s->error, "cannot read file");
            hos_log("js: failed reading %s", s->entry_path);
            js_console_push(3, s->id, s->error);
            continue;
        }
        g_entry_start_ns = hos_now_ns();
        g_active_timeout_ms = g_default_timeout_ms > 5000 || g_default_timeout_ms <= 0 ? g_default_timeout_ms : 5000;
        JSValue compiled = JS_Eval(ctx, code, len, s->entry_path, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        free(code);
        if (JS_IsException(compiled)) {
            JSValue ex = JS_GetException(ctx);
            const char *em = JS_ToCString(ctx, ex);
            snprintf(s->error, sizeof s->error, "%s", em ? em : "compile error");
            if (em) JS_FreeCString(ctx, em);
            JS_FreeValue(ctx, ex);
            hos_log("js: [%s] error: %s", s->id, s->error);
            js_console_push(3, s->id, s->error);
            continue;
        }
        set_module_meta(ctx, compiled, s->entry_path, 1);
        JSValue res = JS_EvalFunction(ctx, compiled);
        if (JS_IsException(res)) {
            JSValue ex = JS_GetException(ctx);
            const char *em = JS_ToCString(ctx, ex);
            snprintf(s->error, sizeof s->error, "%s", em ? em : "runtime error");
            if (em) JS_FreeCString(ctx, em);
            JS_FreeValue(ctx, ex);
            hos_log("js: [%s] runtime error: %s", s->id, s->error);
            js_console_push(3, s->id, s->error);
            continue;
        }
        js_drain_jobs(ctx);
        if (JS_IsPromise(res) && JS_PromiseState(ctx, res) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, res);
            const char *em = JS_ToCString(ctx, reason);
            snprintf(s->error, sizeof s->error, "%s", em ? em : "rejected module promise");
            if (em) JS_FreeCString(ctx, em);
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, res);
            hos_log("js: [%s] rejected promise: %s", s->id, s->error);
            js_console_push(3, s->id, s->error);
            continue;
        }
        JS_FreeValue(ctx, res);
        s->loaded = 1;
        loaded_ok++;
        hos_log("js: loaded %s (%s)", s->id, s->name);
        js_console_push(1, s->id, "loaded");
    }
    g_entry_start_ns = 0;
    g_active_timeout_ms = g_default_timeout_ms;
    hos_log("js: %d/%d script(s) loaded from %s", loaded_ok, g_script_count, g_scripts_dir);
}

static void destroy_runtime_locked(void) {
    InterlockedExchange(&g_js_ready, 0);
    if (g_js_ctx) {
        js_emit_event(g_js_ctx, "unload", 0, NULL);
        js_drain_jobs(g_js_ctx);
        js_memory_reset(g_js_ctx);
        js_oot_reset(g_js_ctx);
        js_io_reset(g_js_ctx);
        js_ui_reset(g_js_ctx);
        JS_FreeContext(g_js_ctx);
        g_js_ctx = NULL;
    } else {
        js_memory_reset(NULL);
        js_oot_reset(NULL);
        js_io_reset(NULL);
        js_ui_reset(NULL);
    }
    if (g_js_rt) {
        JS_FreeRuntime(g_js_rt);
        g_js_rt = NULL;
    }
}

static int create_runtime_and_load_locked(void) {
    resolve_dirs();
    g_js_rt = JS_NewRuntime();
    if (!g_js_rt) return 0;
    JS_SetMemoryLimit(g_js_rt, JS_MAX_MEM_BYTES);
    JS_SetMaxStackSize(g_js_rt, JS_MAX_STACK_BYTES);
    JS_UpdateStackTop(g_js_rt);
    JS_SetInterruptHandler(g_js_rt, js_watchdog_handler, NULL);
    JS_SetModuleLoaderFunc(g_js_rt, js_module_normalize, js_module_loader, NULL);

    g_js_ctx = JS_NewContext(g_js_rt);
    if (!g_js_ctx) {
        JS_FreeRuntime(g_js_rt);
        g_js_rt = NULL;
        return 0;
    }

    register_globals(g_js_ctx);
    load_disabled_list(g_js_ctx);
    discover_all_scripts(g_js_ctx);
    InterlockedExchange(&g_js_ready, 1);
    load_discovered_scripts(g_js_ctx);
    return 1;
}

static int js_do_reload_locked(void) {
    destroy_runtime_locked();
    return create_runtime_and_load_locked();
}

void hos_js_init(void) {
    ensure_cs();
    const char *to = getenv("HOS_JS_TIMEOUT_MS");
    if (to && *to) g_default_timeout_ms = _atoi64(to);
    InterlockedExchange(&g_shutting_down, 0);
    EnterCriticalSection(&g_js_cs);
    if (!g_js_rt) create_runtime_and_load_locked();
    LeaveCriticalSection(&g_js_cs);
}

void hos_js_shutdown(void) {
    InterlockedExchange(&g_shutting_down, 1);
    ensure_cs();
    EnterCriticalSection(&g_js_cs);
    destroy_runtime_locked();
    LeaveCriticalSection(&g_js_cs);
}

int hos_js_reload(void) {
    ensure_cs();
    EnterCriticalSection(&g_js_cs);
    if (g_enter_depth > 0) {
        InterlockedExchange(&g_reload_pending, 1);
        LeaveCriticalSection(&g_js_cs);
        return 1;
    }
    if (g_js_rt) JS_UpdateStackTop(g_js_rt);
    const int ok = js_do_reload_locked();
    LeaveCriticalSection(&g_js_cs);
    return ok;
}

int hos_js_is_ready(void) {
    return InterlockedCompareExchange(&g_js_ready, 0, 0) != 0;
}

int js_scripts_copy(MkwVkJsScript *out, int max) {
    AcquireSRWLockShared(&g_meta_lock);
    const int n = g_script_count < max ? g_script_count : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        snprintf(out[i].id, sizeof out[i].id, "%s", g_scripts[i].id);
        snprintf(out[i].name, sizeof out[i].name, "%s", g_scripts[i].name);
        snprintf(out[i].version, sizeof out[i].version, "%s", g_scripts[i].version);
        snprintf(out[i].author, sizeof out[i].author, "%s", g_scripts[i].author);
        snprintf(out[i].error, sizeof out[i].error, "%s", g_scripts[i].error);
        out[i].enabled = g_scripts[i].enabled;
        out[i].loaded = g_scripts[i].loaded;
    }
    ReleaseSRWLockShared(&g_meta_lock);
    return n;
}

int js_set_script_enabled(const char *id, int enabled) {
    if (!id || !*id) return 0;
    int found = 0;
    AcquireSRWLockExclusive(&g_meta_lock);
    for (int i = 0; i < g_script_count; i++) {
        if (!_stricmp(g_scripts[i].id, id)) {
            g_scripts[i].enabled = enabled ? 1 : 0;
            found = 1;
        }
    }
    int pos = -1;
    for (int i = 0; i < g_disabled_count; i++) {
        if (!_stricmp(g_disabled_ids[i], id)) {
            pos = i;
            break;
        }
    }
    if (enabled && pos >= 0) {
        g_disabled_ids[pos][0] = 0;
        for (int i = pos; i + 1 < g_disabled_count; i++)
            memcpy(g_disabled_ids[i], g_disabled_ids[i + 1], 64);
        g_disabled_count--;
    } else if (!enabled && pos < 0 && g_disabled_count < 128) {
        snprintf(g_disabled_ids[g_disabled_count++], 64, "%s", id);
    }
    save_disabled_list();
    ReleaseSRWLockExclusive(&g_meta_lock);
    return found || 1;
}

/* ---- Control Bridge JSON endpoints ---- */
static void append_js_result_json(JSContext *ctx, JSValueConst v, char *out, size_t cap, size_t *pos) {
    char tmp[1024] = "";
    size_t tp = 0;
    append_val_text(ctx, v, tmp, sizeof tmp, &tp);
    *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"text\":\"");
    js_json_escape_append(out, cap, pos, tmp);
    *pos += (size_t)snprintf(out + *pos, cap - *pos, "\",\"value\":");

    if (JS_IsUndefined(v) || JS_IsFunction(ctx, v) || JS_IsSymbol(v)) {
        *pos += (size_t)snprintf(out + *pos, cap - *pos, "null");
    } else if (JS_IsNull(v)) {
        *pos += (size_t)snprintf(out + *pos, cap - *pos, "null");
    } else if (JS_IsBool(v)) {
        *pos += (size_t)snprintf(out + *pos, cap - *pos, JS_ToBool(ctx, v) ? "true" : "false");
    } else if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        if (isfinite(d)) *pos += (size_t)snprintf(out + *pos, cap - *pos, "%.17g", d);
        else *pos += (size_t)snprintf(out + *pos, cap - *pos, "null");
    } else if (JS_IsBigInt(v)) {
        const char *s = JS_ToCString(ctx, v);
        *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"%sn\"", s ? s : "0");
        if (s) JS_FreeCString(ctx, s);
    } else {
        JSValue j = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(j) && JS_IsString(j)) {
            const char *js = JS_ToCString(ctx, j);
            if (js && *pos + strlen(js) + 16 < cap) {
                *pos += (size_t)snprintf(out + *pos, cap - *pos, "%s", js);
            } else {
                *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"");
                js_json_escape_append(out, cap, pos, tmp);
                *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"");
            }
            if (js) JS_FreeCString(ctx, js);
        } else {
            if (JS_IsException(j)) JS_FreeValue(ctx, JS_GetException(ctx));
            *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"");
            js_json_escape_append(out, cap, pos, tmp);
            *pos += (size_t)snprintf(out + *pos, cap - *pos, "\"");
        }
        JS_FreeValue(ctx, j);
    }
}

int hos_js_eval_json(const char *code, const char *filename, char *out_json, size_t cap) {
    if (!out_json || cap < 64) return 0;
    if (!code) code = "";
    if (!filename || !*filename) filename = "<repl>";
    if (!js_enter()) {
        snprintf(out_json, cap, "{\"ok\":false,\"error\":\"JS runtime not initialized\"}");
        return 0;
    }
    js_console_push(0, "repl", code);
    JSValue val = JS_Eval(g_js_ctx, code, strlen(code), filename, JS_EVAL_TYPE_GLOBAL);
    size_t pos = 0;
    int ok = 1;
    if (JS_IsException(val)) {
        ok = 0;
        JSValue ex = JS_GetException(g_js_ctx);
        const char *msg = JS_ToCString(g_js_ctx, ex);
        JSValue st = JS_GetPropertyStr(g_js_ctx, ex, "stack");
        const char *stk = JS_IsString(st) ? JS_ToCString(g_js_ctx, st) : NULL;
        pos += (size_t)snprintf(out_json, cap, "{\"ok\":false,\"error\":\"");
        js_json_escape_append(out_json, cap, &pos, msg ? msg : "Error");
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"stack\":\"");
        js_json_escape_append(out_json, cap, &pos, stk ? stk : "");
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\"}");
        js_console_push(3, "repl", msg ? msg : "Error");
        if (stk) JS_FreeCString(g_js_ctx, stk);
        if (msg) JS_FreeCString(g_js_ctx, msg);
        JS_FreeValue(g_js_ctx, st);
        JS_FreeValue(g_js_ctx, ex);
    } else {
        js_drain_jobs(g_js_ctx);
        pos += (size_t)snprintf(out_json, cap, "{\"ok\":true,");
        append_js_result_json(g_js_ctx, val, out_json, cap, &pos);
        if (pos + 2 < cap) out_json[pos++] = '}', out_json[pos] = 0;
        char preview[320] = "";
        size_t pp = 0;
        append_val_text(g_js_ctx, val, preview, sizeof preview, &pp);
        if (preview[0]) js_console_push(1, "=>", preview);
    }
    JS_FreeValue(g_js_ctx, val);
    js_leave();
    return ok;
}

int hos_js_list_json(char *out_json, size_t cap) {
    if (!out_json || cap < 64) return 0;
    size_t pos = 0;
    pos += (size_t)snprintf(out_json, cap, "{\"ready\":%s,\"scriptsDir\":\"", hos_js_is_ready() ? "true" : "false");
    js_json_escape_append(out_json, cap, &pos, g_scripts_dir);
    pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"scripts\":[");
    AcquireSRWLockShared(&g_meta_lock);
    for (int i = 0; i < g_script_count && pos + 128 < cap; i++) {
        const JsScriptInfo *s = &g_scripts[i];
        if (i > 0) pos += (size_t)snprintf(out_json + pos, cap - pos, ",");
        pos += (size_t)snprintf(out_json + pos, cap - pos, "{\"id\":\"");
        js_json_escape_append(out_json, cap, &pos, s->id);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"name\":\"");
        js_json_escape_append(out_json, cap, &pos, s->name);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"version\":\"");
        js_json_escape_append(out_json, cap, &pos, s->version);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"author\":\"");
        js_json_escape_append(out_json, cap, &pos, s->author);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"enabled\":%s,\"loaded\":%s,\"error\":\"",
                                s->enabled ? "true" : "false", s->loaded ? "true" : "false");
        js_json_escape_append(out_json, cap, &pos, s->error);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\"}");
    }
    ReleaseSRWLockShared(&g_meta_lock);
    snprintf(out_json + pos, cap - pos, "]}");
    return 1;
}

int hos_js_log_json(int since_seq, char *out_json, size_t cap) {
    if (!out_json || cap < 64) return 0;
    MkwVkJsLogLine tmp[JS_LOG_RING];
    const int n = js_console_copy(tmp, JS_LOG_RING);
    size_t pos = 0;
    const int latest = n > 0 ? tmp[n - 1].seq : g_log_seq;
    pos += (size_t)snprintf(out_json, cap, "{\"seq\":%d,\"lines\":[", latest);
    int first = 1;
    for (int i = 0; i < n && pos + 128 < cap; i++) {
        if (tmp[i].seq <= since_seq) continue;
        if (!first) pos += (size_t)snprintf(out_json + pos, cap - pos, ",");
        first = 0;
        pos += (size_t)snprintf(out_json + pos, cap - pos, "{\"seq\":%d,\"level\":%d,\"source\":\"",
                                tmp[i].seq, tmp[i].level);
        js_json_escape_append(out_json, cap, &pos, tmp[i].source);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\",\"text\":\"");
        js_json_escape_append(out_json, cap, &pos, tmp[i].text);
        pos += (size_t)snprintf(out_json + pos, cap - pos, "\"}");
    }
    snprintf(out_json + pos, cap - pos, "]}");
    return 1;
}
