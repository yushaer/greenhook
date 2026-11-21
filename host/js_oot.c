/* High-level Ocarina of Time `game.*` API (player, inventory, flags, world, camera, actors) + host game hooks. */
#include "js_internal.h"
#include "../gpu/vk/oot_cheat_items.h"

#define GD_SLOT     0x84c9e180ULL
#define EVENT_SLOT  0x84c9e090ULL
#define WORLD_SLOT  0x84c9e898ULL
#define CAM_SLOT    0x84ca6bc0ULL
#define ACTOR_SLOT  0x84ca24c0ULL
#define POUCH_SLOT  0x84e98e28ULL
#define VIEW_VTABLE 0x84c18a88ULL

#define FLAG_MAP      0x1d6189dau
#define FLAG_LOADING  0xef2b60c2u
#define FLAG_CHILD    0xcea220d3u
#define FLAG_ROT_MAP  0xa4aee8cdu
#define FLAG_LIFE     0x31ab5580u
#define FLAG_MAX_LIFE 0xfbe01da1u
#define FLAG_RUPEES   0xa77921d7u
#define FLAG_PROMPT   0x9824206cu
#define FLAG_MUSIC    0xcad3ffe5u

enum { KD_BOOL, KD_INT, KD_FLOAT, KD_ENUM, KD_TEXT, KD_UINT, KD_COUNT };
static const struct { uint32_t at, stride, val_off; } kContainers[KD_COUNT] = {
    {0x008, 0x18, 0x16}, {0x0c8, 0x28, 0x1c}, {0x188, 0x28, 0x1c},
    {0x248, 0x38, 0x1c}, {0x608, 0x108, 0x68}, {0x788, 0x28, 0x1c},
};

#define OOT_NAME(label, name, kind) name,
#define OOT_KIND(label, name, kind) kind,
#define OOT_LABEL(label, name, kind) label,
#define OOT_NONE(name)
static const char *const kItemLabels[] = {OOT_CHEAT_ITEMS(OOT_NONE, OOT_LABEL)};
static const char *const kItemNames[]  = {OOT_CHEAT_ITEMS(OOT_NONE, OOT_NAME)};
static const unsigned char kItemKinds[] = {OOT_CHEAT_ITEMS(OOT_NONE, OOT_KIND)};
#define K_ITEM_COUNT (sizeof kItemNames / sizeof kItemNames[0])

/* ---- Atomic overrides read lock-free on game hot paths ---- */
static volatile LONG g_invincible = -1;
static volatile LONG g_flight = -1;
static volatile LONG g_disable_centering = -1;
static volatile LONG g_speed_scale_bits = 0x3f800000L;      /* 1.0f */
static volatile LONG g_damage_mult_bits = 0x3f800000L;      /* 1.0f */
static volatile LONG g_fov_scale_bits = 0x3f800000L;        /* 1.0f */
static volatile LONG g_cam_speed_scale_bits = 0x3f800000L;  /* 1.0f */

static float atomic_load_f32(volatile LONG *p) {
    uint32_t u = (uint32_t)__atomic_load_n(p, __ATOMIC_RELAXED);
    float f = 1.0f;
    memcpy(&f, &u, 4);
    return f;
}
static void atomic_store_f32(volatile LONG *p, float f) {
    uint32_t u = 0;
    memcpy(&u, &f, 4);
    __atomic_store_n(p, (LONG)u, __ATOMIC_RELAXED);
}

int hos_js_player_invincible(void) { return (int)__atomic_load_n(&g_invincible, __ATOMIC_RELAXED); }
int hos_js_player_flight(void) { return (int)__atomic_load_n(&g_flight, __ATOMIC_RELAXED); }
float hos_js_player_speed_scale(void) { return atomic_load_f32(&g_speed_scale_bits); }
float hos_js_damage_multiplier(void) { return atomic_load_f32(&g_damage_mult_bits); }
float hos_js_fov_scale(void) { return atomic_load_f32(&g_fov_scale_bits); }
float hos_js_camera_speed_scale(void) { return atomic_load_f32(&g_cam_speed_scale_bits); }
int hos_js_camera_disable_centering(void) { return (int)__atomic_load_n(&g_disable_centering, __ATOMIC_RELAXED); }

/* ---- Cached telemetry & camera state ---- */
static SRWLOCK g_oot_lock = SRWLOCK_INIT;
static uint64_t g_last_actor = 0, g_last_components = 0, g_last_player = 0, g_last_controller = 0;
static float g_last_pos[3] = {0};
static float g_last_facing = 0.0f;
static char g_last_map[80] = "";
static char g_last_bgm[64] = "";
static char g_last_prompt[64] = "";
static int g_last_life = -1, g_last_max_life = -1, g_last_rupees = -1, g_last_is_child = -1;
static int g_item_cache[K_ITEM_COUNT];
static int g_item_cache_valid = 0;

static int g_cam_override_on = 0;
static float g_cam_override_mat[12];
static float g_cam_override_eye[3];
static float g_cam_override_target[3];

static int g_player_cam_angles_on = 0;
static float g_player_cam_yaw = 0.0f, g_player_cam_pitch = 0.0f;
static float g_last_cam_yaw = 0.0f, g_last_cam_pitch = 0.0f;

/* ---- Deferred tasks / off-thread actions ---- */
typedef struct {
    int used;
    JSValue fn;
    JSValue resolve;
    JSValue reject;
} PlayerTask;
#define PLAYER_TASK_MAX 64
static PlayerTask g_player_tasks[PLAYER_TASK_MAX];

enum {
    ACT_SET_POS = 1,
    ACT_SET_AGE,
    ACT_SET_TIME,
    ACT_TELEPORT_IDX,
    ACT_TELEPORT_CUSTOM,
    ACT_TRIGGER_EVENT,
    ACT_POUCH_CHANGE,
    ACT_POUCH_SET,
    ACT_SET_BOOL,
};
typedef struct {
    int kind;
    float f[3];
    int ival;
    uint32_t uval;
    uint64_t u64;
    char s1[96], s2[96], s3[96];
} QueuedAction;
#define ACTION_QUEUE_MAX 64
static QueuedAction g_action_q[ACTION_QUEUE_MAX];
static int g_action_q_n = 0;

static int queue_action(const QueuedAction *a) {
    AcquireSRWLockExclusive(&g_oot_lock);
    const int accepted = g_action_q_n < ACTION_QUEUE_MAX;
    if (accepted) g_action_q[g_action_q_n++] = *a;
    ReleaseSRWLockExclusive(&g_oot_lock);
    return accepted;
}

void js_queue_player_task(JSContext *ctx, JSValue fn, JSValue resolve, JSValue reject) {
    AcquireSRWLockExclusive(&g_oot_lock);
    for (int i = 0; i < PLAYER_TASK_MAX; i++) {
        if (!g_player_tasks[i].used) {
            g_player_tasks[i].used = 1;
            g_player_tasks[i].fn = fn;
            g_player_tasks[i].resolve = resolve;
            g_player_tasks[i].reject = reject;
            ReleaseSRWLockExclusive(&g_oot_lock);
            return;
        }
    }
    ReleaseSRWLockExclusive(&g_oot_lock);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "player task queue full"));
    JSValue r = JS_Call(ctx, reject, JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, reject);
}

typedef struct {
    int used;
    uint32_t hash;
    int64_t id;
    int has_base;
    int kind;
    int64_t ival;
    float fval;
    char sval[64];
    JSValue key_val;
    JSValue cb;
} FlagWatch;
#define FLAG_WATCH_MAX 64
static int64_t next_watch_id;
static FlagWatch g_watches[FLAG_WATCH_MAX];

/* ---- Direct GameDataMgr & Singleton readers ---- */
static uint64_t read_singleton(uint64_t slot) {
    uint64_t holder = 0, mgr = 0;
    if (!js_mem_u64(slot, &holder) || !holder) return 0;
    if (!js_mem_u64(holder, &mgr)) return 0;
    return mgr;
}

static uint64_t find_gd_entry(uint64_t mgr, int kind, uint32_t hash) {
    if (!mgr || kind < 0 || kind >= KD_COUNT || !hash) return 0;
    const uint64_t box = mgr + kContainers[kind].at;
    uint64_t entries = 0, table = 0;
    uint32_t count = 0, size = 0;
    if (!js_mem_u32(box + 8, &count) || !js_mem_u64(box + 16, &entries) ||
        !js_mem_u64(box + 24, &table) || !js_mem_u32(box + 36, &size))
        return 0;
    if (!entries || !table || !size || size > (1u << 22)) return 0;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1 == size ? 0 : slot + 1)) {
        uint32_t pair[2] = {0, 0};
        if (!js_mem_read(table + (uint64_t)slot * 8ULL, pair, sizeof pair) || !pair[0]) return 0;
        if (pair[0] == hash) return pair[1] < count ? entries + (uint64_t)pair[1] * kContainers[kind].stride : 0;
    }
    return 0;
}

static int gd_read_num(uint64_t mgr, int kind, uint32_t hash, int64_t *out) {
    const uint64_t e = find_gd_entry(mgr, kind, hash);
    if (!e) return 0;
    const uint64_t at = e + kContainers[kind].val_off;
    if (kind == KD_BOOL) {
        uint8_t b = 0;
        if (!js_mem_u8(at, &b)) return 0;
        *out = b & 1;
        return 1;
    }
    uint32_t u = 0;
    if (!js_mem_u32(at, &u)) return 0;
    *out = (kind == KD_INT) ? (int64_t)(int32_t)u : (int64_t)u;
    return 1;
}

static int gd_read_f32(uint64_t mgr, uint32_t hash, float *out) {
    const uint64_t e = find_gd_entry(mgr, KD_FLOAT, hash);
    return e && js_mem_f32(e + kContainers[KD_FLOAT].val_off, out);
}

static int gd_read_str(uint64_t mgr, uint32_t hash, char *buf, size_t cap) {
    if (!cap) return 0;
    buf[0] = 0;
    const uint64_t e = find_gd_entry(mgr, KD_TEXT, hash);
    uint64_t p = 0;
    if (!e || !js_mem_u64(e + kContainers[KD_TEXT].val_off, &p) || !p) return 0;
    size_t n = cap > 64 ? 64 : cap;
    if (!js_mem_read(p, buf, n)) { buf[0] = 0; return 0; }
    buf[n - 1] = 0;
    return 1;
}

static int gd_read_any(uint64_t mgr, uint32_t hash, int *out_kind, int64_t *out_i, float *out_f, char *out_s, size_t scap) {
    if (gd_read_num(mgr, KD_BOOL, hash, out_i))  { *out_kind = KD_BOOL;  return 1; }
    if (gd_read_num(mgr, KD_INT, hash, out_i))   { *out_kind = KD_INT;   return 1; }
    if (gd_read_f32(mgr, hash, out_f))           { *out_kind = KD_FLOAT; return 1; }
    if (gd_read_num(mgr, KD_ENUM, hash, out_i))  { *out_kind = KD_ENUM;  return 1; }
    if (gd_read_str(mgr, hash, out_s, scap))     { *out_kind = KD_TEXT;  return 1; }
    if (gd_read_num(mgr, KD_UINT, hash, out_i))  { *out_kind = KD_UINT;  return 1; }
    return 0;
}

static JSValue gd_val_to_js(JSContext *ctx, int kind, int64_t ival, float fval, const char *sval) {
    switch (kind) {
    case KD_BOOL: return JS_NewBool(ctx, ival != 0);
    case KD_INT:  return JS_NewInt32(ctx, (int32_t)ival);
    case KD_UINT:
    case KD_ENUM: return JS_NewUint32(ctx, (uint32_t)ival);
    case KD_FLOAT: return JS_NewFloat64(ctx, (double)fval);
    case KD_TEXT: return JS_NewString(ctx, sval);
    default: return JS_NULL;
    }
}

static int parse_hash_arg(JSContext *ctx, JSValueConst val, uint32_t *out) {
    if (JS_IsString(val)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, val);
        if (!s) return 0;
        if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            *out = (uint32_t)strtoul(s + 2, NULL, 16);
        else
            *out = js_murmur3_32(s, len, 0);
        JS_FreeCString(ctx, s);
        return 1;
    }
    return js_to_u32(ctx, val, out) == 0;
}

static uint32_t song_flag_hash(const char *name) {
    static const struct { const char *name; uint32_t hash; } songs[] = {
        {"Item_Melody_ZeldaLullaby", 0x01c20002u},
        {"Item_Melody_EponaSong", 0xb492ac33u},
        {"Item_Melody_SariaSong", 0xd3f8bfccu},
        {"Item_Melody_SunSong", 0x165dd1adu},
        {"Item_Melody_TimeSong", 0x03b3450eu},
        {"Item_Melody_StormSong", 0x2b39dd4fu},
        {"Item_Melody_ForestMinuet", 0xdc0a0b54u},
        {"Item_Melody_FireBolero", 0x49041ab9u},
        {"Item_Melody_WaterSerenade", 0x3fa63986u},
        {"Item_Melody_ShadowNocturne", 0xc2fb95f4u},
        {"Item_Melody_SpiritRequiem", 0x802a36a8u},
        {"Item_Melody_LightPrelude", 0xd6320ba4u},
    };
    for (size_t i = 0; i < sizeof songs / sizeof songs[0]; i++)
        if (!strcmp(name, songs[i].name)) return songs[i].hash;
    return 0;
}

static const char *resolve_item_actor_name(const char *name) {
    for (size_t i = 0; i < K_ITEM_COUNT; i++) {
        if (!_stricmp(name, kItemNames[i]) || !_stricmp(name, kItemLabels[i])) return kItemNames[i];
    }
    return name;
}

static int edit_item(Ctx *c, const char *name, int amount, int absolute) {
    if (absolute) {
        const int have = hos_oot_js_item_count(c, name);
        if (have < 0) return 0;
        amount -= have;
    }
    if (!amount) return 1;
    const uint32_t song = song_flag_hash(name);
    if (song) return hos_oot_js_gamedata_set_bool(c, song, amount > 0);
    char alias[96];
    if (!strncmp(name, "Armor_Child_Mask_", 17)) {
        snprintf(alias, sizeof alias, "Item_Mask_%s", name + 17);
        name = alias;
    }
    return hos_oot_js_pouch_change(c, name, amount) >= 0;
}

static void drain_actions(Ctx *c, uint64_t actor, uint64_t components) {
    QueuedAction batch[ACTION_QUEUE_MAX];
    int count = 0;
    AcquireSRWLockExclusive(&g_oot_lock);
    if (g_action_q_n > 0) {
        count = g_action_q_n;
        memcpy(batch, g_action_q, sizeof(QueuedAction) * (size_t)count);
        g_action_q_n = 0;
    }
    ReleaseSRWLockExclusive(&g_oot_lock);

    for (int i = 0; i < count; i++) {
        QueuedAction *a = &batch[i];
        switch (a->kind) {
        case ACT_SET_POS:
            hos_oot_js_set_player_pos(c, actor, components, a->f[0], a->f[1], a->f[2]);
            break;
        case ACT_SET_AGE:
            hos_oot_js_toggle_age(c, actor, a->ival);
            break;
        case ACT_SET_TIME:
            hos_oot_js_set_time_seconds(c, a->uval);
            break;
        case ACT_TELEPORT_IDX:
            hos_oot_js_teleport_index(c, actor, a->uval);
            break;
        case ACT_TELEPORT_CUSTOM:
            hos_oot_js_teleport_custom(c, actor, a->s1, a->s2, a->s3);
            break;
        case ACT_TRIGGER_EVENT:
            hos_oot_js_trigger_event(c, a->s1, a->u64 ? a->u64 : actor);
            break;
        case ACT_POUCH_CHANGE:
        case ACT_POUCH_SET:
            edit_item(c, a->s1, a->ival, a->kind == ACT_POUCH_SET);
            break;
        case ACT_SET_BOOL:
            hos_oot_js_gamedata_set_bool(c, a->uval, a->ival);
            break;
        }
    }
}

void js_drain_player_tasks(JSContext *ctx, Ctx *c) {
    (void)c;
    PlayerTask taken[PLAYER_TASK_MAX];
    int n = 0;
    AcquireSRWLockExclusive(&g_oot_lock);
    for (int i = 0; i < PLAYER_TASK_MAX; i++) {
        if (g_player_tasks[i].used) {
            taken[n++] = g_player_tasks[i];
            g_player_tasks[i].used = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_oot_lock);
    for (int i = 0; i < n; i++) {
        JSValue r = JS_Call(ctx, taken[i].fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue ex = JS_GetException(ctx);
            JSValue rr = JS_Call(ctx, taken[i].reject, JS_UNDEFINED, 1, &ex);
            JS_FreeValue(ctx, rr);
            JS_FreeValue(ctx, ex);
        } else {
            JSValue rr = JS_Call(ctx, taken[i].resolve, JS_UNDEFINED, 1, &r);
            JS_FreeValue(ctx, rr);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, taken[i].fn);
        JS_FreeValue(ctx, taken[i].resolve);
        JS_FreeValue(ctx, taken[i].reject);
        js_drain_jobs(ctx);
    }
}

static void tick_flag_watchers(JSContext *ctx, uint64_t mgr) {
    if (!mgr) return;
    for (int i = 0; i < FLAG_WATCH_MAX; i++) {
        FlagWatch *w = &g_watches[i];
        if (!w->used) continue;
        int kind = 0;
        int64_t ival = 0;
        float fval = 0.0f;
        char sval[64] = "";
        if (!gd_read_any(mgr, w->hash, &kind, &ival, &fval, sval, sizeof sval)) continue;
        if (!w->has_base) {
            w->has_base = 1;
            w->kind = kind;
            w->ival = ival;
            w->fval = fval;
            snprintf(w->sval, sizeof w->sval, "%s", sval);
            continue;
        }
        int changed = 0;
        if (kind == KD_FLOAT) changed = fabsf(fval - w->fval) > 1e-6f;
        else if (kind == KD_TEXT) changed = strcmp(sval, w->sval) != 0;
        else changed = ival != w->ival || kind != w->kind;
        if (changed) {
            JSValue nv = gd_val_to_js(ctx, kind, ival, fval, sval);
            JSValue ov = gd_val_to_js(ctx, w->kind, w->ival, w->fval, w->sval);
            JSValue kv = JS_DupValue(ctx, w->key_val);
            JSValue argv[3] = {nv, ov, kv};
            JSValue cb = JS_DupValue(ctx, w->cb);
            w->kind = kind;
            w->ival = ival;
            w->fval = fval;
            snprintf(w->sval, sizeof w->sval, "%s", sval);
            JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 3, argv);
            if (JS_IsException(r)) js_log_exception(ctx, "flags.watch");
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, cb);
            JS_FreeValue(ctx, nv);
            JS_FreeValue(ctx, ov);
            JS_FreeValue(ctx, kv);
        }
    }
}

void js_oot_tick_player(JSContext *ctx, Ctx *c, uint64_t actor, uint64_t components, uint64_t player) {
    (void)c, (void)actor, (void)components, (void)player;
    tick_flag_watchers(ctx, read_singleton(GD_SLOT));
}

/* ---- Host hooks called from hos_main.c / oot_*.c / mouse_native.c ---- */
void hos_js_on_player_update(struct GuestContext *c, uint64_t actor, uint64_t components, uint64_t player) {
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;

    drain_actions(c, actor, components);

    float pos[3] = {0}, m816 = 0.0f, m840 = 1.0f;
    js_mem_read(actor + 0x31c, pos, sizeof pos);
    js_mem_f32(actor + 816, &m816);
    js_mem_f32(actor + 840, &m840);
    const float facing = atan2f(m816, m840);
    const uint64_t ctrl = hos_oot_js_player_controller(c, components);

    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t life = -1, max_life = -1, rupees = -1, child = -1;
    char map[80] = "", bgm[64] = "", prompt[64] = "";
    if (mgr) {
        gd_read_num(mgr, KD_INT, FLAG_LIFE, &life);
        gd_read_num(mgr, KD_INT, FLAG_MAX_LIFE, &max_life);
        gd_read_num(mgr, KD_INT, FLAG_RUPEES, &rupees);
        gd_read_num(mgr, KD_BOOL, FLAG_CHILD, &child);
        gd_read_str(mgr, FLAG_MAP, map, sizeof map);
        gd_read_str(mgr, FLAG_MUSIC, bgm, sizeof bgm);
        gd_read_str(mgr, FLAG_PROMPT, prompt, sizeof prompt);
    }

    char prev_map[80] = "", prev_bgm[64] = "";
    AcquireSRWLockExclusive(&g_oot_lock);
    g_last_actor = actor;
    g_last_components = components;
    g_last_player = player;
    g_last_controller = ctrl;
    memcpy(g_last_pos, pos, sizeof pos);
    g_last_facing = facing;
    if (life >= 0) g_last_life = (int)life;
    if (max_life >= 0) g_last_max_life = (int)max_life;
    if (rupees >= 0) g_last_rupees = (int)rupees;
    if (child >= 0) g_last_is_child = (int)(child != 0);
    snprintf(prev_map, sizeof prev_map, "%s", g_last_map);
    snprintf(prev_bgm, sizeof prev_bgm, "%s", g_last_bgm);
    if (map[0]) snprintf(g_last_map, sizeof g_last_map, "%s", map);
    snprintf(g_last_bgm, sizeof g_last_bgm, "%s", bgm);
    snprintf(g_last_prompt, sizeof g_last_prompt, "%s", prompt);
    ReleaseSRWLockExclusive(&g_oot_lock);
    js_set_current_bgm(bgm);

    /* Refresh inventory cache periodically (every ~30 updates) when scripts exist */
    static LONG item_tick = 0;
    if (hos_js_is_ready() && (InterlockedIncrement(&item_tick) % 30 == 1 || !g_item_cache_valid)) {
        int tmp[K_ITEM_COUNT];
        for (size_t i = 0; i < K_ITEM_COUNT; i++) tmp[i] = hos_oot_js_item_count(c, kItemNames[i]);
        AcquireSRWLockExclusive(&g_oot_lock);
        memcpy(g_item_cache, tmp, sizeof tmp);
        g_item_cache_valid = 1;
        ReleaseSRWLockExclusive(&g_oot_lock);
    }

    if (js_enter()) {
        js_drain_player_tasks(g_js_ctx, c);
        js_oot_tick_player(g_js_ctx, c, actor, components, player);

        if (map[0] && strcmp(map, prev_map) != 0 && js_has_event_listeners("mapChange")) {
            JSValue argv[2] = {JS_NewString(g_js_ctx, map), JS_NewString(g_js_ctx, prev_map)};
            js_emit_event(g_js_ctx, "mapChange", 2, argv);
            JS_FreeValue(g_js_ctx, argv[0]);
            JS_FreeValue(g_js_ctx, argv[1]);
        }
        if (strcmp(bgm, prev_bgm) != 0 && js_has_event_listeners("bgmChange")) {
            JSValue argv[2] = {JS_NewString(g_js_ctx, bgm), JS_NewString(g_js_ctx, prev_bgm)};
            js_emit_event(g_js_ctx, "bgmChange", 2, argv);
            JS_FreeValue(g_js_ctx, argv[0]);
            JS_FreeValue(g_js_ctx, argv[1]);
        }
        if (js_has_event_listeners("playerUpdate")) {
            JSValue info = JS_NewObject(g_js_ctx);
            JS_SetPropertyStr(g_js_ctx, info, "actor", JS_NewBigUint64(g_js_ctx, actor));
            JS_SetPropertyStr(g_js_ctx, info, "player", JS_NewBigUint64(g_js_ctx, player));
            JS_SetPropertyStr(g_js_ctx, info, "controller", JS_NewBigUint64(g_js_ctx, ctrl));
            JS_SetPropertyStr(g_js_ctx, info, "components", JS_NewBigUint64(g_js_ctx, components));
            JSValue pv = JS_NewObject(g_js_ctx);
            JS_SetPropertyStr(g_js_ctx, pv, "x", JS_NewFloat64(g_js_ctx, pos[0]));
            JS_SetPropertyStr(g_js_ctx, pv, "y", JS_NewFloat64(g_js_ctx, pos[1]));
            JS_SetPropertyStr(g_js_ctx, pv, "z", JS_NewFloat64(g_js_ctx, pos[2]));
            JS_SetPropertyStr(g_js_ctx, info, "pos", pv);
            JS_SetPropertyStr(g_js_ctx, info, "facing", JS_NewFloat64(g_js_ctx, facing));
            JS_SetPropertyStr(g_js_ctx, info, "life", JS_NewInt32(g_js_ctx, g_last_life));
            JS_SetPropertyStr(g_js_ctx, info, "maxLife", JS_NewInt32(g_js_ctx, g_last_max_life));
            JS_SetPropertyStr(g_js_ctx, info, "rupees", JS_NewInt32(g_js_ctx, g_last_rupees));
            js_emit_event(g_js_ctx, "playerUpdate", 1, &info);
            JS_FreeValue(g_js_ctx, info);
        }
        js_drain_jobs(g_js_ctx);
        js_leave();
    }

    g_js_tls_ctx = prev;
}

int hos_js_on_damage(uint32_t old_life, uint32_t *in_out_hit) {
    if (!in_out_hit || !js_has_event_listeners("damage")) return 0;
    int cancel = 0;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "oldLife", JS_NewUint32(g_js_ctx, old_life));
        JS_SetPropertyStr(g_js_ctx, ev, "damage", JS_NewUint32(g_js_ctx, *in_out_hit));
        JS_SetPropertyStr(g_js_ctx, ev, "cancel", JS_FALSE);
        js_fire_event_callbacks(g_js_ctx, "damage", 1, &ev);
        JSValue cv = JS_GetPropertyStr(g_js_ctx, ev, "cancel");
        if (JS_ToBool(g_js_ctx, cv)) cancel = 1;
        JS_FreeValue(g_js_ctx, cv);
        JSValue dv = JS_GetPropertyStr(g_js_ctx, ev, "damage");
        uint32_t new_dmg = *in_out_hit;
        if (JS_IsNumber(dv) && JS_ToUint32(g_js_ctx, &new_dmg, dv) == 0) *in_out_hit = new_dmg;
        JS_FreeValue(g_js_ctx, dv);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    return cancel;
}

int hos_js_on_camera_view(struct GuestContext *c, uint64_t view) {
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;

    int active = 0;
    float mat[12];
    AcquireSRWLockShared(&g_oot_lock);
    active = g_cam_override_on;
    if (active) memcpy(mat, g_cam_override_mat, sizeof mat);
    ReleaseSRWLockShared(&g_oot_lock);

    if (js_has_event_listeners("cameraUpdate") && js_enter()) {
        double eye[3] = {0}, at[3] = {0};
        js_mem_read(view + 96, eye, sizeof eye);
        js_mem_read(view + 120, at, sizeof at);
        JSValue ev = JS_NewObject(g_js_ctx);
        JSValue eobj = JS_NewObject(g_js_ctx), tobj = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, eobj, "x", JS_NewFloat64(g_js_ctx, eye[0]));
        JS_SetPropertyStr(g_js_ctx, eobj, "y", JS_NewFloat64(g_js_ctx, eye[1]));
        JS_SetPropertyStr(g_js_ctx, eobj, "z", JS_NewFloat64(g_js_ctx, eye[2]));
        JS_SetPropertyStr(g_js_ctx, tobj, "x", JS_NewFloat64(g_js_ctx, at[0]));
        JS_SetPropertyStr(g_js_ctx, tobj, "y", JS_NewFloat64(g_js_ctx, at[1]));
        JS_SetPropertyStr(g_js_ctx, tobj, "z", JS_NewFloat64(g_js_ctx, at[2]));
        JS_SetPropertyStr(g_js_ctx, ev, "view", JS_NewBigUint64(g_js_ctx, view));
        JS_SetPropertyStr(g_js_ctx, ev, "eye", eobj);
        JS_SetPropertyStr(g_js_ctx, ev, "target", tobj);
        js_emit_event(g_js_ctx, "cameraUpdate", 1, &ev);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();

        AcquireSRWLockShared(&g_oot_lock);
        active = g_cam_override_on;
        if (active) memcpy(mat, g_cam_override_mat, sizeof mat);
        ReleaseSRWLockShared(&g_oot_lock);
    }

    if (active && view) {
        js_mem_write(view + 8, mat, sizeof mat);
    }
    g_js_tls_ctx = prev;
    return active;
}

int hos_js_on_mouse_camera(uint64_t cam, float *yaw, float *pitch) {
    (void)cam;
    AcquireSRWLockExclusive(&g_oot_lock);
    if (yaw && pitch) {
        g_last_cam_yaw = *yaw;
        g_last_cam_pitch = *pitch;
    }
    const int on = g_player_cam_angles_on;
    if (on && yaw && pitch) {
        *yaw = g_player_cam_yaw;
        *pitch = g_player_cam_pitch;
    }
    ReleaseSRWLockExclusive(&g_oot_lock);
    return on;
}

void hos_js_on_event_request(struct GuestContext *c, const char *name, uint64_t arg) {
    if (!name || !*name || !js_has_event_listeners("eventRequest")) return;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "name", JS_NewString(g_js_ctx, name));
        JS_SetPropertyStr(g_js_ctx, ev, "arg", JS_NewBigUint64(g_js_ctx, arg));
        js_emit_event(g_js_ctx, "eventRequest", 1, &ev);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    g_js_tls_ctx = prev;
}

int hos_js_on_timeline_start(struct GuestContext *c, const char *name, uint64_t player) {
    if (!name || !*name || !js_has_event_listeners("timelineStart")) return 0;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    int skip = 0;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "name", JS_NewString(g_js_ctx, name));
        JS_SetPropertyStr(g_js_ctx, ev, "player", JS_NewBigUint64(g_js_ctx, player));
        JS_SetPropertyStr(g_js_ctx, ev, "cancel", JS_FALSE);
        js_fire_event_callbacks(g_js_ctx, "timelineStart", 1, &ev);
        JSValue cv = JS_GetPropertyStr(g_js_ctx, ev, "cancel");
        if (JS_ToBool(g_js_ctx, cv)) skip = 1;
        JS_FreeValue(g_js_ctx, cv);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    g_js_tls_ctx = prev;
    return skip;
}

int hos_js_on_save_request(struct GuestContext *c, uint32_t reason, uint32_t mode, uint64_t caller, int is_transition) {
    if (!js_has_event_listeners("saveRequest")) return 0;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    int cancel = 0;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "reason", JS_NewUint32(g_js_ctx, reason));
        JS_SetPropertyStr(g_js_ctx, ev, "mode", JS_NewUint32(g_js_ctx, mode));
        JS_SetPropertyStr(g_js_ctx, ev, "caller", JS_NewBigUint64(g_js_ctx, caller));
        JS_SetPropertyStr(g_js_ctx, ev, "isTransition", JS_NewBool(g_js_ctx, is_transition));
        JS_SetPropertyStr(g_js_ctx, ev, "cancel", JS_FALSE);
        js_fire_event_callbacks(g_js_ctx, "saveRequest", 1, &ev);
        JSValue cv = JS_GetPropertyStr(g_js_ctx, ev, "cancel");
        if (JS_ToBool(g_js_ctx, cv)) cancel = 1;
        JS_FreeValue(g_js_ctx, cv);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    g_js_tls_ctx = prev;
    return cancel;
}

void hos_js_on_menu_tick(struct GuestContext *c, uint64_t screen) {
    if (!js_has_event_listeners("menuTick")) return;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "screen", JS_NewBigUint64(g_js_ctx, screen));
        uint64_t vt = 0;
        js_mem_u64(screen, &vt);
        JS_SetPropertyStr(g_js_ctx, ev, "vtable", JS_NewBigUint64(g_js_ctx, vt));
        js_emit_event(g_js_ctx, "menuTick", 1, &ev);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    g_js_tls_ctx = prev;
}

void hos_js_on_menu_focus(struct GuestContext *c, uint64_t screen, uint64_t pane, uint64_t lr) {
    if (!js_has_event_listeners("menuFocus")) return;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    if (js_enter()) {
        JSValue ev = JS_NewObject(g_js_ctx);
        JS_SetPropertyStr(g_js_ctx, ev, "screen", JS_NewBigUint64(g_js_ctx, screen));
        JS_SetPropertyStr(g_js_ctx, ev, "pane", JS_NewBigUint64(g_js_ctx, pane));
        JS_SetPropertyStr(g_js_ctx, ev, "lr", JS_NewBigUint64(g_js_ctx, lr));
        js_emit_event(g_js_ctx, "menuFocus", 1, &ev);
        JS_FreeValue(g_js_ctx, ev);
        js_drain_jobs(g_js_ctx);
        js_leave();
    }
    g_js_tls_ctx = prev;
}

/* ---- game.player ---- */
static JSValue js_player_getPos(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_oot_lock);
    const uint64_t actor = g_last_actor;
    float p[3] = {g_last_pos[0], g_last_pos[1], g_last_pos[2]};
    ReleaseSRWLockShared(&g_oot_lock);
    if (actor) js_mem_read(actor + 0x31c, p, sizeof p);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, p[0]));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, p[1]));
    JS_SetPropertyStr(ctx, o, "z", JS_NewFloat64(ctx, p[2]));
    return o;
}

static JSValue js_player_setPos(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double x = 0, y = 0, z = 0;
    if (argc < 3 || JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &z, argv[2]) < 0)
        return JS_ThrowTypeError(ctx, "setPos(x, y, z)");
    if (g_js_tls_ctx && g_last_actor && g_last_components) {
        return JS_NewBool(ctx, hos_oot_js_set_player_pos(g_js_tls_ctx, g_last_actor, g_last_components,
                                                          (float)x, (float)y, (float)z));
    }
    QueuedAction a = {.kind = ACT_SET_POS, .f = {(float)x, (float)y, (float)z}};
    return JS_NewBool(ctx, queue_action(&a));
}

static JSValue js_player_getFacing(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_oot_lock);
    const uint64_t actor = g_last_actor;
    float facing = g_last_facing;
    ReleaseSRWLockShared(&g_oot_lock);
    if (actor) {
        float m816 = 0, m840 = 1;
        if (js_mem_f32(actor + 816, &m816) && js_mem_f32(actor + 840, &m840))
            facing = atan2f(m816, m840);
    }
    return JS_NewFloat64(ctx, (double)facing);
}

static JSValue js_player_setFacing(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double rad = 0;
    if (argc < 1 || JS_ToFloat64(ctx, &rad, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setFacing(radians)");
    const float deg = (float)(rad * 57.29577951308232);
    const uint64_t mgr = read_singleton(GD_SLOT);
    const uint64_t e = find_gd_entry(mgr, KD_FLOAT, FLAG_ROT_MAP);
    if (e) js_mem_write(e + kContainers[KD_FLOAT].val_off, &deg, 4);
    AcquireSRWLockShared(&g_oot_lock);
    const uint64_t actor = g_last_actor;
    ReleaseSRWLockShared(&g_oot_lock);
    if (actor) {
        const float s = sinf((float)rad), co = cosf((float)rad);
        js_mem_write(actor + 816, &s, 4);
        js_mem_write(actor + 840, &co, 4);
        return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue js_player_getLife(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t v = -1;
    if (mgr && gd_read_num(mgr, KD_INT, FLAG_LIFE, &v)) return JS_NewInt32(ctx, (int32_t)v);
    return JS_NewInt32(ctx, g_last_life);
}

static JSValue js_player_setLife(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t qh = 0;
    if (argc < 1 || JS_ToInt32(ctx, &qh, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setLife(quarterHearts)");
    if (qh < 0) qh = 0;
    const uint64_t mgr = read_singleton(GD_SLOT);
    const uint64_t e = find_gd_entry(mgr, KD_INT, FLAG_LIFE);
    int ok = 0;
    if (e) ok |= js_mem_write(e + kContainers[KD_INT].val_off, &qh, 4);
    AcquireSRWLockShared(&g_oot_lock);
    const uint64_t player = g_last_player;
    ReleaseSRWLockShared(&g_oot_lock);
    uint64_t life_holder = 0;
    if (player && js_mem_u64(player + 7184, &life_holder) && life_holder) {
        ok |= js_mem_write(life_holder + 8, &qh, 4);
    }
    return JS_NewBool(ctx, ok);
}

static JSValue js_player_getMaxLife(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t v = -1;
    if (mgr && gd_read_num(mgr, KD_INT, FLAG_MAX_LIFE, &v)) return JS_NewInt32(ctx, (int32_t)v);
    return JS_NewInt32(ctx, g_last_max_life);
}

static JSValue js_player_setMaxLife(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t qh = 0;
    if (argc < 1 || JS_ToInt32(ctx, &qh, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setMaxLife(quarterHearts)");
    if (qh < 4) qh = 4;
    const uint64_t mgr = read_singleton(GD_SLOT);
    const uint64_t e = find_gd_entry(mgr, KD_INT, FLAG_MAX_LIFE);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_INT].val_off, &qh, 4));
}

static JSValue js_player_getRupees(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t v = -1;
    if (mgr && gd_read_num(mgr, KD_INT, FLAG_RUPEES, &v)) return JS_NewInt32(ctx, (int32_t)v);
    return JS_NewInt32(ctx, g_last_rupees);
}

static JSValue js_player_setRupees(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t r = 0;
    if (argc < 1 || JS_ToInt32(ctx, &r, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setRupees(amount)");
    if (r < 0) r = 0;
    const uint64_t mgr = read_singleton(GD_SLOT);
    const uint64_t e = find_gd_entry(mgr, KD_INT, FLAG_RUPEES);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_INT].val_off, &r, 4));
}

static JSValue js_player_getAge(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t child = -1;
    if (mgr && gd_read_num(mgr, KD_BOOL, FLAG_CHILD, &child))
        return JS_NewString(ctx, child ? "child" : "adult");
    if (g_last_is_child >= 0) return JS_NewString(ctx, g_last_is_child ? "child" : "adult");
    return JS_NULL;
}

static JSValue js_player_setAge(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setAge('child' | 'adult')");
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    const int want_adult = !_stricmp(s, "adult") ? 1 : (!_stricmp(s, "child") ? 0 : -1);
    JS_FreeCString(ctx, s);
    if (want_adult < 0) return JS_ThrowRangeError(ctx, "age must be 'child' or 'adult'");
    if (g_js_tls_ctx && g_last_actor) {
        return JS_NewBool(ctx, hos_oot_js_toggle_age(g_js_tls_ctx, g_last_actor, want_adult));
    }
    QueuedAction a = {.kind = ACT_SET_AGE, .ival = want_adult};
    return JS_NewBool(ctx, queue_action(&a));
}

static JSValue js_player_getSpeedScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_player_speed_scale());
}
static JSValue js_player_setSpeedScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setSpeedScale(number)");
    if (d < 0.05) d = 0.05;
    if (d > 10.0) d = 10.0;
    atomic_store_f32(&g_speed_scale_bits, (float)d);
    return JS_UNDEFINED;
}

static JSValue js_player_setInvincible(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        __atomic_store_n(&g_invincible, -1, __ATOMIC_RELAXED);
        return JS_UNDEFINED;
    }
    __atomic_store_n(&g_invincible, JS_ToBool(ctx, argv[0]) ? 1 : 0, __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}
static JSValue js_player_getInvincible(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const int v = hos_js_player_invincible();
    return v < 0 ? JS_NULL : JS_NewBool(ctx, v != 0);
}

static JSValue js_player_setDamageMult(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setDamageMultiplier(number)");
    if (d < 0.0) d = 0.0;
    if (d > 100.0) d = 100.0;
    atomic_store_f32(&g_damage_mult_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_player_getDamageMult(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_damage_multiplier());
}

static JSValue js_player_setFlight(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        __atomic_store_n(&g_flight, -1, __ATOMIC_RELAXED);
        return JS_UNDEFINED;
    }
    __atomic_store_n(&g_flight, JS_ToBool(ctx, argv[0]) ? 1 : 0, __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}
static JSValue js_player_getFlight(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const int v = hos_js_player_flight();
    return v < 0 ? JS_NULL : JS_NewBool(ctx, v != 0);
}

static JSValue js_player_getPrompt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    char buf[64] = "";
    if (mgr && gd_read_str(mgr, FLAG_PROMPT, buf, sizeof buf)) return JS_NewString(ctx, buf);
    AcquireSRWLockShared(&g_oot_lock);
    JSValue s = JS_NewString(ctx, g_last_prompt);
    ReleaseSRWLockShared(&g_oot_lock);
    return s;
}

static JSValue js_player_getPtrs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_oot_lock);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "actor", JS_NewBigUint64(ctx, g_last_actor));
    JS_SetPropertyStr(ctx, o, "player", JS_NewBigUint64(ctx, g_last_player));
    JS_SetPropertyStr(ctx, o, "components", JS_NewBigUint64(ctx, g_last_components));
    JS_SetPropertyStr(ctx, o, "controller", JS_NewBigUint64(ctx, g_last_controller));
    ReleaseSRWLockShared(&g_oot_lock);
    return o;
}

/* ---- game.inventory ---- */
static JSValue js_inv_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSValue arr = JS_NewArray(ctx);
    AcquireSRWLockShared(&g_oot_lock);
    for (size_t i = 0; i < K_ITEM_COUNT; i++) {
        int c = g_item_cache_valid ? g_item_cache[i] : -1;
        if (g_js_tls_ctx) c = hos_oot_js_item_count(g_js_tls_ctx, kItemNames[i]);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "label", JS_NewString(ctx, kItemLabels[i]));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, kItemNames[i]));
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, kItemKinds[i] ? "quantity" : "toggle"));
        JS_SetPropertyStr(ctx, o, "count", JS_NewInt32(ctx, c));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    ReleaseSRWLockShared(&g_oot_lock);
    return arr;
}

static JSValue js_inv_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    const char *name = resolve_item_actor_name(raw);
    int val = -1;
    uint32_t song = song_flag_hash(name);
    if (song) {
        const uint64_t mgr = read_singleton(GD_SLOT);
        int64_t b = 0;
        if (mgr && gd_read_num(mgr, KD_BOOL, song, &b)) val = (int)(b != 0);
    } else if (g_js_tls_ctx) {
        val = hos_oot_js_item_count(g_js_tls_ctx, name);
    } else if (g_item_cache_valid) {
        AcquireSRWLockShared(&g_oot_lock);
        for (size_t i = 0; i < K_ITEM_COUNT; i++) {
            if (!strcmp(name, kItemNames[i])) { val = g_item_cache[i]; break; }
        }
        ReleaseSRWLockShared(&g_oot_lock);
    }
    JS_FreeCString(ctx, raw);
    return JS_NewInt32(ctx, val);
}

static JSValue js_inv_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue v = js_inv_get(ctx, this_val, argc, argv);
    if (JS_IsException(v)) return v;
    int32_t n = 0;
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return JS_NewBool(ctx, n > 0);
}

static JSValue inv_edit(JSContext *ctx, const char *raw, int amount, int absolute) {
    const char *name = resolve_item_actor_name(raw);
    if (!absolute && !amount) return JS_TRUE;
    if (g_js_tls_ctx) return JS_NewBool(ctx, edit_item(g_js_tls_ctx, name, amount, absolute));
    QueuedAction a = {.kind = absolute ? ACT_POUCH_SET : ACT_POUCH_CHANGE, .ival = amount};
    if (strlen(name) >= sizeof a.s1) return JS_FALSE;
    snprintf(a.s1, sizeof a.s1, "%s", name);
    return JS_NewBool(ctx, queue_action(&a));
}

static JSValue js_inv_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t delta = 1;
    if (argc < 1) return JS_ThrowTypeError(ctx, "add(itemName, delta = 1)");
    if (argc >= 2 && JS_ToInt32(ctx, &delta, argv[1]) < 0) return JS_EXCEPTION;
    const char *raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    JSValue r = inv_edit(ctx, raw, delta, 0);
    JS_FreeCString(ctx, raw);
    return r;
}

static JSValue js_inv_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t delta = 1;
    if (argc < 1) return JS_ThrowTypeError(ctx, "remove(itemName, delta = 1)");
    if (argc >= 2 && JS_ToInt32(ctx, &delta, argv[1]) < 0) return JS_EXCEPTION;
    if (delta == INT32_MIN) return JS_ThrowRangeError(ctx, "delta is out of range");
    const char *raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    JSValue r = inv_edit(ctx, raw, -delta, 0);
    JS_FreeCString(ctx, raw);
    return r;
}

static JSValue js_inv_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t target = 1;
    if (argc < 2 || JS_ToInt32(ctx, &target, argv[1]) < 0) return JS_ThrowTypeError(ctx, "set(itemName, count)");
    if (target < 0) return JS_ThrowRangeError(ctx, "count must be nonnegative");
    const char *raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    JSValue r = inv_edit(ctx, raw, target, 1);
    JS_FreeCString(ctx, raw);
    return r;
}

/* ---- game.flags ---- */
/* Return at most one page of occupied hash slots from a typed GameDataMgr table. */
static JSValue js_flags_list(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    static const char *const names[KD_COUNT] = {"bool", "int", "float", "enum", "string", "uint"};
    if (!argc || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "flags.list(kind, cursor?)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    int kind = 0;
    while (kind < KD_COUNT && strcmp(name, names[kind])) kind++;
    JS_FreeCString(ctx, name);
    if (kind == KD_COUNT) return JS_ThrowRangeError(ctx, "unknown flag kind");

    double cursor = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsNumber(argv[1])) return JS_ThrowTypeError(ctx, "cursor must be a number");
        if (JS_ToFloat64(ctx, &cursor, argv[1]) < 0) return JS_EXCEPTION;
        if (!(cursor >= 0 && cursor <= (1u << 22)) || cursor != floor(cursor))
            return JS_ThrowRangeError(ctx, "invalid flag cursor");
    }
    const uint64_t mgr = read_singleton(GD_SLOT);
    if (!mgr) return JS_NULL;
    const uint64_t box = mgr + kContainers[kind].at;
    uint32_t size, count;
    uint64_t table;
    if (!js_mem_u32(box + 36, &size) || size > (1u << 22) ||
        !js_mem_u32(box + 8, &count) || count > size || !js_mem_u64(box + 24, &table))
        return JS_NULL;
    const uint32_t start = (uint32_t)cursor;
    const uint32_t end = start + 128 < size ? start + 128 : size;
    uint32_t pairs[128][2];
    if (start < end && (!table || table > UINT64_MAX - (uint64_t)end * 8 ||
        !js_mem_read(table + (uint64_t)start * 8, pairs, (end - start) * 8)))
        return JS_NULL;
    JSValue hashes = JS_NewArray(ctx);
    unsigned n = 0;
    for (uint32_t i = start; i < end; i++) {
        const uint32_t *pair = pairs[i - start];
        if (pair[0] && pair[1] < count)
            JS_SetPropertyUint32(ctx, hashes, n++, JS_NewUint32(ctx, pair[0]));
    }
    JSValue page = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, page, "hashes", hashes);
    JS_SetPropertyStr(ctx, page, "next", end < size ? JS_NewUint32(ctx, end) : JS_NULL);
    return page;
}

static JSValue js_flags_hash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewUint32(ctx, 0);
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    const uint32_t h = js_murmur3_32(s, len, 0);
    JS_FreeCString(ctx, s);
    return JS_NewUint32(ctx, h);
}

static JSValue js_flags_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int kind = 0;
    int64_t ival = 0;
    float fval = 0.0f;
    char sval[64] = "";
    if (!mgr || !gd_read_any(mgr, h, &kind, &ival, &fval, sval, sizeof sval)) return JS_NULL;
    return gd_val_to_js(ctx, kind, ival, fval, sval);
}

static JSValue js_flags_getBool(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; int64_t v = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_num(mgr, KD_BOOL, h, &v) ? JS_NewBool(ctx, v != 0) : JS_NULL;
}

static JSValue js_flags_setBool(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h)) return JS_FALSE;
    const int val = JS_ToBool(ctx, argv[1]) ? 1 : 0;
    const int persist = argc >= 3 ? JS_ToBool(ctx, argv[2]) : 1;
    const uint64_t mgr = read_singleton(GD_SLOT);
    const uint64_t e = find_gd_entry(mgr, KD_BOOL, h);
    uint8_t b = (uint8_t)val;
    int ok = e && js_mem_write(e + kContainers[KD_BOOL].val_off, &b, 1);
    if (persist) {
        if (g_js_tls_ctx) ok |= hos_oot_js_gamedata_set_bool(g_js_tls_ctx, h, val);
        else {
            QueuedAction a = {.kind = ACT_SET_BOOL, .uval = h, .ival = val};
            ok = queue_action(&a);
        }
    }
    return JS_NewBool(ctx, ok);
}

static JSValue js_flags_getInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; int64_t v = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_num(mgr, KD_INT, h, &v) ? JS_NewInt32(ctx, (int32_t)v) : JS_NULL;
}
static JSValue js_flags_setInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; int32_t v = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h) || JS_ToInt32(ctx, &v, argv[1]) < 0) return JS_FALSE;
    const uint64_t e = find_gd_entry(read_singleton(GD_SLOT), KD_INT, h);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_INT].val_off, &v, 4));
}

static JSValue js_flags_getUInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; int64_t v = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_num(mgr, KD_UINT, h, &v) ? JS_NewUint32(ctx, (uint32_t)v) : JS_NULL;
}
static JSValue js_flags_setUInt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0, v = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h) || js_to_u32(ctx, argv[1], &v) < 0) return JS_FALSE;
    const uint64_t e = find_gd_entry(read_singleton(GD_SLOT), KD_UINT, h);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_UINT].val_off, &v, 4));
}

static JSValue js_flags_getFloat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; float f = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_f32(mgr, h, &f) ? JS_NewFloat64(ctx, (double)f) : JS_NULL;
}
static JSValue js_flags_setFloat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; double d = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h) || JS_ToFloat64(ctx, &d, argv[1]) < 0) return JS_FALSE;
    float f = (float)d;
    const uint64_t e = find_gd_entry(read_singleton(GD_SLOT), KD_FLOAT, h);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_FLOAT].val_off, &f, 4));
}

static JSValue js_flags_getEnum(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; int64_t v = 0;
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_num(mgr, KD_ENUM, h, &v) ? JS_NewUint32(ctx, (uint32_t)v) : JS_NULL;
}
static JSValue js_flags_setEnum(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0, v = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h)) return JS_FALSE;
    if (JS_IsString(argv[1])) {
        if (!parse_hash_arg(ctx, argv[1], &v)) return JS_FALSE;
    } else if (js_to_u32(ctx, argv[1], &v) < 0) return JS_FALSE;
    const uint64_t e = find_gd_entry(read_singleton(GD_SLOT), KD_ENUM, h);
    return JS_NewBool(ctx, e && js_mem_write(e + kContainers[KD_ENUM].val_off, &v, 4));
}

static JSValue js_flags_getString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0; char buf[64];
    if (argc < 1 || !parse_hash_arg(ctx, argv[0], &h)) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    return mgr && gd_read_str(mgr, h, buf, sizeof buf) ? JS_NewString(ctx, buf) : JS_NULL;
}
static JSValue js_flags_setString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h)) return JS_FALSE;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) return JS_EXCEPTION;
    const uint64_t e = find_gd_entry(read_singleton(GD_SLOT), KD_TEXT, h);
    uint64_t p = 0;
    int ok = e && js_mem_u64(e + kContainers[KD_TEXT].val_off, &p) && p;
    if (ok) {
        char buf[64];
        memset(buf, 0, sizeof buf);
        snprintf(buf, sizeof buf, "%s", s);
        ok = js_mem_write(p, buf, sizeof buf);
    }
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static JSValue js_flags_getBool64(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t id = 0;
    if (argc < 1 || js_to_u64(ctx, argv[0], &id) < 0) return JS_NULL;
    const uint64_t mgr = read_singleton(GD_SLOT);
    uint32_t count = 0; uint64_t entries = 0;
    if (!mgr || !js_mem_u32(mgr + 0xc58, &count) || !js_mem_u64(mgr + 0xc60, &entries) || !count || count > (1u << 20))
        return JS_NULL;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t k = 0; uint8_t b = 0;
        if (js_mem_u64(entries + (uint64_t)i * 16ULL, &k) && k == id &&
            js_mem_u8(entries + (uint64_t)i * 16ULL + 12, &b))
            return JS_NewBool(ctx, b != 0);
    }
    return JS_NULL;
}
static JSValue js_flags_setBool64(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t id = 0;
    if (argc < 2 || js_to_u64(ctx, argv[0], &id) < 0) return JS_FALSE;
    const uint8_t val = JS_ToBool(ctx, argv[1]) ? 1 : 0;
    const uint64_t mgr = read_singleton(GD_SLOT);
    uint32_t count = 0; uint64_t entries = 0;
    if (!mgr || !js_mem_u32(mgr + 0xc58, &count) || !js_mem_u64(mgr + 0xc60, &entries) || !count || count > (1u << 20))
        return JS_FALSE;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t k = 0;
        if (js_mem_u64(entries + (uint64_t)i * 16ULL, &k) && k == id)
            return JS_NewBool(ctx, js_mem_write(entries + (uint64_t)i * 16ULL + 12, &val, 1));
    }
    return JS_FALSE;
}

static JSValue js_flags_unwatch_slot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *data) {
    (void)this_val, (void)argc, (void)argv;
    int64_t id;
    if (JS_ToInt64(ctx, &id, data[0]) < 0) return JS_EXCEPTION;
    if (magic >= 0 && magic < FLAG_WATCH_MAX && g_watches[magic].used && g_watches[magic].id == id) {
        JS_FreeValue(ctx, g_watches[magic].cb);
        JS_FreeValue(ctx, g_watches[magic].key_val);
        memset(&g_watches[magic], 0, sizeof g_watches[magic]);
    }
    return JS_UNDEFINED;
}

static JSValue js_flags_watch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t h = 0;
    if (argc < 2 || !parse_hash_arg(ctx, argv[0], &h) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "flags.watch(nameOrHash, callback)");
    for (int i = 0; i < FLAG_WATCH_MAX; i++) {
        if (!g_watches[i].used) {
            g_watches[i].used = 1;
            g_watches[i].hash = h;
            g_watches[i].has_base = 0;
            g_watches[i].key_val = JS_DupValue(ctx, argv[0]);
            g_watches[i].cb = JS_DupValue(ctx, argv[1]);
            g_watches[i].id = ++next_watch_id;
            JSValue id = JS_NewInt64(ctx, g_watches[i].id);
            JSValue unsub = JS_NewCFunctionData(ctx, js_flags_unwatch_slot, 0, i, 1, &id);
            JS_FreeValue(ctx, id);
            return unsub;
        }
    }
    return JS_ThrowRangeError(ctx, "flag watch limit (%d) reached", FLAG_WATCH_MAX);
}

/* ---- game.world ---- */
static JSValue js_world_getMap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    char buf[80] = "";
    if (mgr && gd_read_str(mgr, FLAG_MAP, buf, sizeof buf) && buf[0]) return JS_NewString(ctx, buf);
    AcquireSRWLockShared(&g_oot_lock);
    JSValue s = g_last_map[0] ? JS_NewString(ctx, g_last_map) : JS_NULL;
    ReleaseSRWLockShared(&g_oot_lock);
    return s;
}

static JSValue js_world_area(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    JSValue key = argc && !JS_IsUndefined(argv[0]) ? JS_DupValue(ctx, argv[0]) : js_world_getMap(ctx, self, 0, NULL);
    if (JS_IsNull(key) || JS_IsException(key)) return key;
    const char *map = JS_ToCString(ctx, key);
    JS_FreeValue(ctx, key);
    if (!map) return JS_EXCEPTION;
    char area[256];
    int ok = js_area_name(map, area, sizeof area);
    JS_FreeCString(ctx, map);
    return ok ? JS_NewString(ctx, area) : JS_NULL;
}

static JSValue js_player_progress(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self, (void)argc, (void)argv;
    int values[5];
    if (!js_player_stats(values)) return JS_NULL;
    static const char *const names[] = {"life", "maxLife", "rupees", "skulltulaTokens", "heartPieces"};
    JSValue progress = JS_NewObject(ctx);
    for (int i = 0; i < 5; i++)
        JS_SetPropertyStr(ctx, progress, names[i], values[i] < 0 ? JS_NULL : JS_NewInt32(ctx, values[i]));
    return progress;
}

static JSValue js_world_isLoading(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    int64_t v = 0;
    return JS_NewBool(ctx, mgr && gd_read_num(mgr, KD_BOOL, FLAG_LOADING, &v) && v != 0);
}

static JSValue js_world_isEventPlaying(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t ev = read_singleton(EVENT_SLOT);
    uint64_t cur = 0;
    return JS_NewBool(ctx, ev && js_mem_u64(ev + 104, &cur) && cur != 0);
}

static JSValue js_world_getMusic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t mgr = read_singleton(GD_SLOT);
    char buf[64] = "";
    if (mgr && gd_read_str(mgr, FLAG_MUSIC, buf, sizeof buf)) return JS_NewString(ctx, buf);
    AcquireSRWLockShared(&g_oot_lock);
    JSValue s = JS_NewString(ctx, g_last_bgm);
    ReleaseSRWLockShared(&g_oot_lock);
    return s;
}

static JSValue js_world_getTime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t world = read_singleton(WORLD_SLOT);
    uint64_t tmgr = 0;
    if (!world || !js_mem_u64(world + 0x510, &tmgr) || !tmgr) return JS_NULL;
    uint32_t flag = 0;
    if (!js_mem_u32(tmgr + 8, &flag)) return JS_NULL;
    const uint64_t slot = tmgr + ((flag & 0x200u) ? 0x50u : 0x20u);
    uint64_t total = 0;
    if (!js_mem_u64(slot + 8, &total)) return JS_NULL;
    const uint32_t sec = (uint32_t)(total % 86400ULL);
    const uint32_t h = sec / 3600u, m = (sec % 3600u) / 60u;
    char fmt[16];
    snprintf(fmt, sizeof fmt, "%02u:%02u", h, m);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "totalSeconds", JS_NewBigUint64(ctx, total));
    JS_SetPropertyStr(ctx, o, "seconds", JS_NewUint32(ctx, sec));
    JS_SetPropertyStr(ctx, o, "hour", JS_NewUint32(ctx, h));
    JS_SetPropertyStr(ctx, o, "minute", JS_NewUint32(ctx, m));
    JS_SetPropertyStr(ctx, o, "formatted", JS_NewString(ctx, fmt));
    return o;
}

static JSValue js_world_setTime(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t sec = 0;
    if (argc < 1) return JS_ThrowTypeError(ctx, "setTime(hourOrSeconds, minute?)");
    if (argc >= 2) {
        double h = 0, m = 0;
        if (JS_ToFloat64(ctx, &h, argv[0]) < 0 || JS_ToFloat64(ctx, &m, argv[1]) < 0) return JS_EXCEPTION;
        long total = lround(h * 3600.0 + m * 60.0) % 86400L;
        if (total < 0) total += 86400L;
        sec = (uint32_t)total;
    } else {
        double v = 0;
        if (JS_ToFloat64(ctx, &v, argv[0]) < 0 || !isfinite(v)) return JS_EXCEPTION;
        long total = (v >= 0.0 && v <= 24.0) ? lround(v * 3600.0) : lround(v);
        total %= 86400L;
        if (total < 0) total += 86400L;
        sec = (uint32_t)total;
    }
    if (g_js_tls_ctx) {
        hos_oot_js_set_time_seconds(g_js_tls_ctx, sec);
        return JS_TRUE;
    }
    QueuedAction a = {.kind = ACT_SET_TIME, .uval = sec};
    return JS_NewBool(ctx, queue_action(&a));
}

static JSValue js_world_destinations(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0;; i++) {
        const char *child = NULL, *adult = NULL;
        if (!hos_oot_destination(i, &child, &adult)) break;
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "index", JS_NewInt32(ctx, i));
        JS_SetPropertyStr(ctx, o, "child", child ? JS_NewString(ctx, child) : JS_NULL);
        JS_SetPropertyStr(ctx, o, "adult", adult ? JS_NewString(ctx, adult) : JS_NULL);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
    }
    return arr;
}

static JSValue js_world_teleport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "teleport(index | name | { banc, startPos, bootEvent })");
    if (JS_IsNumber(argv[0])) {
        uint32_t idx = 0;
        if (JS_ToUint32(ctx, &idx, argv[0]) < 0) return JS_EXCEPTION;
        if (g_js_tls_ctx && g_last_actor) return JS_NewBool(ctx, hos_oot_js_teleport_index(g_js_tls_ctx, g_last_actor, idx));
        QueuedAction a = {.kind = ACT_TELEPORT_IDX, .uval = idx};
        return JS_NewBool(ctx, queue_action(&a));
    }
    if (JS_IsString(argv[0])) {
        const char *q = JS_ToCString(ctx, argv[0]);
        if (!q) return JS_EXCEPTION;
        int match = -1;
        for (int i = 0; match < 0; i++) {
            const char *c = NULL, *a = NULL;
            if (!hos_oot_destination(i, &c, &a)) break;
            if ((c && !_stricmp(c, q)) || (a && !_stricmp(a, q)) ||
                (c && strstr(c, q)) || (a && strstr(a, q)))
                match = i;
        }
        JS_FreeCString(ctx, q);
        if (match < 0) return JS_FALSE;
        if (g_js_tls_ctx && g_last_actor)
            return JS_NewBool(ctx, hos_oot_js_teleport_index(g_js_tls_ctx, g_last_actor, (unsigned)match));
        QueuedAction a = {.kind = ACT_TELEPORT_IDX, .uval = (uint32_t)match};
        return JS_NewBool(ctx, queue_action(&a));
    }
    if (JS_IsObject(argv[0])) {
        JSValue vb = JS_GetPropertyStr(ctx, argv[0], "banc");
        JSValue vs = JS_GetPropertyStr(ctx, argv[0], "startPos");
        JSValue ve = JS_GetPropertyStr(ctx, argv[0], "bootEvent");
        const char *sb = JS_ToCString(ctx, vb);
        const char *ss = JS_IsString(vs) ? JS_ToCString(ctx, vs) : NULL;
        const char *se = JS_IsString(ve) ? JS_ToCString(ctx, ve) : NULL;
        int sent = 0;
        if (sb && *sb) {
            const char *start = ss && *ss ? ss : "Entrance";
            const char *boot = se && *se ? se : "Dm_SY_BancChangeArrive";
            if (g_js_tls_ctx && g_last_actor) {
                sent = hos_oot_js_teleport_custom(g_js_tls_ctx, g_last_actor, sb, start, boot);
            } else {
                QueuedAction a = {.kind = ACT_TELEPORT_CUSTOM};
                snprintf(a.s1, sizeof a.s1, "%s", sb);
                snprintf(a.s2, sizeof a.s2, "%s", start);
                snprintf(a.s3, sizeof a.s3, "%s", boot);
                sent = queue_action(&a);
            }
        }
        if (sb) JS_FreeCString(ctx, sb);
        if (ss) JS_FreeCString(ctx, ss);
        if (se) JS_FreeCString(ctx, se);
        JS_FreeValue(ctx, vb);
        JS_FreeValue(ctx, vs);
        JS_FreeValue(ctx, ve);
        return JS_NewBool(ctx, sent);
    }
    return JS_FALSE;
}

static JSValue js_world_triggerEvent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "triggerEvent(name, actorPtr?)");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    uint64_t actor = g_last_actor;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (js_to_u64(ctx, argv[1], &actor) < 0) {
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
    }
    int ok = 1;
    if (g_js_tls_ctx) ok = hos_oot_js_trigger_event(g_js_tls_ctx, name, actor);
    else {
        QueuedAction a = {.kind = ACT_TRIGGER_EVENT, .u64 = actor};
        snprintf(a.s1, sizeof a.s1, "%s", name);
        ok = queue_action(&a);
    }
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

static JSValue js_world_singletons(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "gameDataMgr", JS_NewBigUint64(ctx, read_singleton(GD_SLOT)));
    JS_SetPropertyStr(ctx, o, "eventMgr", JS_NewBigUint64(ctx, read_singleton(EVENT_SLOT)));
    JS_SetPropertyStr(ctx, o, "worldMgr", JS_NewBigUint64(ctx, read_singleton(WORLD_SLOT)));
    JS_SetPropertyStr(ctx, o, "cameraMgr", JS_NewBigUint64(ctx, read_singleton(CAM_SLOT)));
    JS_SetPropertyStr(ctx, o, "actorMgr", JS_NewBigUint64(ctx, read_singleton(ACTOR_SLOT)));
    uint64_t pm = 0;
    js_mem_u64(POUCH_SLOT, &pm);
    JS_SetPropertyStr(ctx, o, "pouchMgr", JS_NewBigUint64(ctx, pm));
    return o;
}

/* ---- game.camera ---- */
static uint64_t read_gameplay_view(void) {
    const uint64_t mgr = read_singleton(CAM_SLOT);
    if (!mgr) return 0;
    uint32_t count = 0; uint64_t cameras = 0, cam0 = 0, view = 0;
    if (!js_mem_u32(mgr + 32, &count) || !count || !js_mem_u64(mgr + 40, &cameras) || !cameras) return 0;
    if (!js_mem_u64(cameras, &cam0) || !cam0) return 0;
    if (!js_mem_u64(cam0 + 120, &view) || !view) return 0;
    uint64_t vt = 0;
    if (!js_mem_u64(view, &vt) || vt != VIEW_VTABLE) return 0;
    return view;
}

static JSValue js_cam_getPose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    const uint64_t view = read_gameplay_view();
    if (!view) return JS_NULL;
    float mat[12] = {0};
    double eye[3] = {0}, target[3] = {0};
    if (!js_mem_read(view + 8, mat, sizeof mat)) return JS_NULL;
    js_mem_read(view + 96, eye, sizeof eye);
    js_mem_read(view + 120, target, sizeof target);
    JSValue o = JS_NewObject(ctx);
    JSValue e = JS_NewObject(ctx), t = JS_NewObject(ctx), m = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, e, "x", JS_NewFloat64(ctx, eye[0]));
    JS_SetPropertyStr(ctx, e, "y", JS_NewFloat64(ctx, eye[1]));
    JS_SetPropertyStr(ctx, e, "z", JS_NewFloat64(ctx, eye[2]));
    JS_SetPropertyStr(ctx, t, "x", JS_NewFloat64(ctx, target[0]));
    JS_SetPropertyStr(ctx, t, "y", JS_NewFloat64(ctx, target[1]));
    JS_SetPropertyStr(ctx, t, "z", JS_NewFloat64(ctx, target[2]));
    for (uint32_t i = 0; i < 12; i++) JS_SetPropertyUint32(ctx, m, i, JS_NewFloat64(ctx, (double)mat[i]));
    JS_SetPropertyStr(ctx, o, "eye", e);
    JS_SetPropertyStr(ctx, o, "target", t);
    JS_SetPropertyStr(ctx, o, "matrix", m);
    AcquireSRWLockShared(&g_oot_lock);
    JS_SetPropertyStr(ctx, o, "yaw", JS_NewFloat64(ctx, g_last_cam_yaw));
    JS_SetPropertyStr(ctx, o, "pitch", JS_NewFloat64(ctx, g_last_cam_pitch));
    ReleaseSRWLockShared(&g_oot_lock);
    return o;
}

static int read_vec3_js(JSContext *ctx, JSValueConst v, float out[3]) {
    if (JS_IsArray(v)) {
        for (uint32_t i = 0; i < 3; i++) {
            double d = 0;
            JSValue iv = JS_GetPropertyUint32(ctx, v, i);
            int bad = JS_ToFloat64(ctx, &d, iv) < 0;
            JS_FreeValue(ctx, iv);
            if (bad) return 0;
            out[i] = (float)d;
        }
        return 1;
    }
    if (JS_IsObject(v)) {
        static const char *const k[3] = {"x", "y", "z"};
        for (int i = 0; i < 3; i++) {
            double d = 0;
            JSValue iv = JS_GetPropertyStr(ctx, v, k[i]);
            int bad = JS_ToFloat64(ctx, &d, iv) < 0;
            JS_FreeValue(ctx, iv);
            if (bad) return 0;
            out[i] = (float)d;
        }
        return 1;
    }
    return 0;
}

static JSValue js_cam_setOverride(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        AcquireSRWLockExclusive(&g_oot_lock);
        g_cam_override_on = 0;
        ReleaseSRWLockExclusive(&g_oot_lock);
        return JS_UNDEFINED;
    }
    if (!JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "setOverride(pose | null)");
    float mat[12] = {0}, eye[3] = {0}, target[3] = {0};
    JSValue mv = JS_GetPropertyStr(ctx, argv[0], "matrix");
    int ok = 0;
    if (JS_IsArray(mv)) {
        ok = 1;
        for (uint32_t i = 0; i < 12; i++) {
            double d = 0;
            JSValue iv = JS_GetPropertyUint32(ctx, mv, i);
            if (JS_ToFloat64(ctx, &d, iv) < 0) ok = 0;
            JS_FreeValue(ctx, iv);
            mat[i] = (float)d;
        }
    }
    JS_FreeValue(ctx, mv);
    if (!ok) {
        JSValue ev = JS_GetPropertyStr(ctx, argv[0], "eye");
        JSValue tv = JS_GetPropertyStr(ctx, argv[0], "target");
        const int has_eye = read_vec3_js(ctx, ev, eye);
        const int has_tgt = read_vec3_js(ctx, tv, target);
        JS_FreeValue(ctx, ev);
        JS_FreeValue(ctx, tv);
        if (!has_eye) return JS_ThrowTypeError(ctx, "pose must include eye and target (or yaw/pitch or matrix)");
        double ax[3][3];
        if (has_tgt) {
            double fx = target[0] - eye[0], fy = target[1] - eye[1], fz = target[2] - eye[2];
            double flen = sqrt(fx * fx + fy * fy + fz * fz);
            if (flen < 1e-6) flen = 1.0, fz = 1.0;
            fx /= flen; fy /= flen; fz /= flen;
            /* Forward = -Z axis of view space */
            double zx = -fx, zy = -fy, zz = -fz;
            /* Right = cross(up(0,1,0), Z) */
            double rx = zz, ry = 0.0, rz = -zx;
            double rlen = hypot(rx, rz);
            if (rlen < 1e-6) rx = 1.0, rz = 0.0, rlen = 1.0;
            rx /= rlen; rz /= rlen;
            double ux = zy * rz - zz * ry, uy = zz * rx - zx * rz, uz = zx * ry - zy * rx;
            ax[0][0] = rx; ax[0][1] = ry; ax[0][2] = rz;
            ax[1][0] = ux; ax[1][1] = uy; ax[1][2] = uz;
            ax[2][0] = zx; ax[2][1] = zy; ax[2][2] = zz;
        } else {
            double yaw = 0.0, pitch = 0.0;
            JSValue yv = JS_GetPropertyStr(ctx, argv[0], "yaw");
            JSValue pv = JS_GetPropertyStr(ctx, argv[0], "pitch");
            JS_ToFloat64(ctx, &yaw, yv);
            JS_ToFloat64(ctx, &pitch, pv);
            JS_FreeValue(ctx, yv);
            JS_FreeValue(ctx, pv);
            const double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
            ax[0][0] = cy;       ax[0][1] = 0;  ax[0][2] = -sy;
            ax[1][0] = -sp * sy; ax[1][1] = cp; ax[1][2] = -sp * cy;
            ax[2][0] = cp * sy;  ax[2][1] = sp; ax[2][2] = cp * cy;
        }
        for (int r = 0; r < 3; r++) {
            mat[r * 4 + 0] = (float)ax[r][0];
            mat[r * 4 + 1] = (float)ax[r][1];
            mat[r * 4 + 2] = (float)ax[r][2];
            mat[r * 4 + 3] = (float)-(ax[r][0] * eye[0] + ax[r][1] * eye[1] + ax[r][2] * eye[2]);
        }
    }
    AcquireSRWLockExclusive(&g_oot_lock);
    memcpy(g_cam_override_mat, mat, sizeof mat);
    memcpy(g_cam_override_eye, eye, sizeof eye);
    memcpy(g_cam_override_target, target, sizeof target);
    g_cam_override_on = 1;
    ReleaseSRWLockExclusive(&g_oot_lock);
    return JS_UNDEFINED;
}

static JSValue js_cam_setPlayerAngles(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double y = 0, p = 0;
    if (argc < 2 || JS_ToFloat64(ctx, &y, argv[0]) < 0 || JS_ToFloat64(ctx, &p, argv[1]) < 0)
        return JS_ThrowTypeError(ctx, "setPlayerAngles(yawDeg, pitchDeg)");
    AcquireSRWLockExclusive(&g_oot_lock);
    g_player_cam_yaw = (float)y;
    g_player_cam_pitch = (float)p;
    g_player_cam_angles_on = 1;
    ReleaseSRWLockExclusive(&g_oot_lock);
    return JS_UNDEFINED;
}

static JSValue js_cam_clearPlayerAngles(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_oot_lock);
    g_player_cam_angles_on = 0;
    ReleaseSRWLockExclusive(&g_oot_lock);
    return JS_UNDEFINED;
}

static JSValue js_cam_setFovScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setFovScale(scale)");
    if (d < 0.25) d = 0.25;
    if (d > 2.5) d = 2.5;
    atomic_store_f32(&g_fov_scale_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_cam_getFovScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_fov_scale());
}
static JSValue js_cam_setSpeedScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double d = 1.0;
    if (argc < 1 || JS_ToFloat64(ctx, &d, argv[0]) < 0) return JS_ThrowTypeError(ctx, "setSpeedScale(scale)");
    if (d < 0.1) d = 0.1;
    if (d > 8.0) d = 8.0;
    atomic_store_f32(&g_cam_speed_scale_bits, (float)d);
    return JS_UNDEFINED;
}
static JSValue js_cam_getSpeedScale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    return JS_NewFloat64(ctx, (double)hos_js_camera_speed_scale());
}
static JSValue js_cam_setDisableCentering(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        __atomic_store_n(&g_disable_centering, -1, __ATOMIC_RELAXED);
        return JS_UNDEFINED;
    }
    __atomic_store_n(&g_disable_centering, JS_ToBool(ctx, argv[0]) ? 1 : 0, __ATOMIC_RELAXED);
    return JS_UNDEFINED;
}

static JSValue js_cam_worldToScreen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double wx = 0, wy = 0, wz = 0, fov = 50.0;
    if (argc < 3 || JS_ToFloat64(ctx, &wx, argv[0]) < 0 || JS_ToFloat64(ctx, &wy, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &wz, argv[2]) < 0)
        return JS_ThrowTypeError(ctx, "worldToScreen(x, y, z, fovDeg?)");
    if (argc >= 4) JS_ToFloat64(ctx, &fov, argv[3]);
    const uint64_t view = read_gameplay_view();
    if (!view) return JS_NULL;
    float m[12];
    if (!js_mem_read(view + 8, m, sizeof m)) return JS_NULL;
    /* Return ui.draw pixels, which follow the overlay image rather than the render frame. */
    int width = 1280, height = 720;
    js_overlay_size(&width, &height);
    const double cx = m[0] * wx + m[1] * wy + m[2] * wz + m[3];
    const double cy = m[4] * wx + m[5] * wy + m[6] * wz + m[7];
    const double cz = m[8] * wx + m[9] * wy + m[10] * wz + m[11];
    const double depth = -cz;
    const double t = tan((fov * 0.5) * 0.017453292519943295);
    const double aspect = (double)width / (double)height;
    int visible = depth > 0.05 && t > 1e-4;
    double sx = -10000.0, sy = -10000.0;
    if (visible) {
        const double nx = cx / (depth * t * aspect);
        const double ny = cy / (depth * t);
        sx = (nx * 0.5 + 0.5) * (double)width;
        sy = (-ny * 0.5 + 0.5) * (double)height;
        visible = isfinite(sx) && isfinite(sy) && sx >= -200.0 && sx <= width + 200.0 &&
                  sy >= -200.0 && sy <= height + 200.0;
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, sx));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, sy));
    JS_SetPropertyStr(ctx, o, "depth", JS_NewFloat64(ctx, depth));
    JS_SetPropertyStr(ctx, o, "visible", JS_NewBool(ctx, visible));
    return o;
}

/* ---- game.actors ---- */
static JSValue js_actors_getPlayer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_oot_lock);
    const uint64_t actor = g_last_actor;
    const uint64_t comp = g_last_components;
    ReleaseSRWLockShared(&g_oot_lock);
    if (!actor) return JS_NULL;
    float p[3] = {0}, m816 = 0, m840 = 1;
    uint64_t vt = 0;
    js_mem_u64(actor, &vt);
    js_mem_read(actor + 0x31c, p, sizeof p);
    js_mem_f32(actor + 816, &m816);
    js_mem_f32(actor + 840, &m840);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "ptr", JS_NewBigUint64(ctx, actor));
    JS_SetPropertyStr(ctx, o, "vtable", JS_NewBigUint64(ctx, vt));
    JS_SetPropertyStr(ctx, o, "components", JS_NewBigUint64(ctx, comp));
    JSValue pv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pv, "x", JS_NewFloat64(ctx, p[0]));
    JS_SetPropertyStr(ctx, pv, "y", JS_NewFloat64(ctx, p[1]));
    JS_SetPropertyStr(ctx, pv, "z", JS_NewFloat64(ctx, p[2]));
    JS_SetPropertyStr(ctx, o, "pos", pv);
    JS_SetPropertyStr(ctx, o, "facing", JS_NewFloat64(ctx, (double)atan2f(m816, m840)));
    return o;
}

static JSValue actor_snapshot(JSContext *ctx, uint64_t ptr) {
    if (!js_mem_ok(ptr, 864)) return JS_NULL;
    uint64_t vt = 0, comp = 0, res = 0;
    float p[3] = {0};
    js_mem_u64(ptr, &vt);
    js_mem_u64(ptr + 16, &res);
    js_mem_u64(ptr + 624, &comp);
    js_mem_read(ptr + 0x31c, p, sizeof p);
    char name[48] = "";
    if (res && js_mem_ok(res + 88, 32)) js_mem_read(res + 88, name, 32), name[31] = 0;
    JSValue o = JS_NewObject(ctx);
    uint32_t instance = 0, status = 0;
    js_mem_u32(ptr + 24, &instance);
    js_mem_u32(ptr + 44, &status);
    JS_SetPropertyStr(ctx, o, "instanceId", JS_NewUint32(ctx, instance));
    JS_SetPropertyStr(ctx, o, "statusCode", JS_NewUint32(ctx, status));
    JS_SetPropertyStr(ctx, o, "ptr", JS_NewBigUint64(ctx, ptr));
    JS_SetPropertyStr(ctx, o, "vtable", JS_NewBigUint64(ctx, vt));
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "components", JS_NewBigUint64(ctx, comp));
    JSValue pv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, pv, "x", JS_NewFloat64(ctx, p[0]));
    JS_SetPropertyStr(ctx, pv, "y", JS_NewFloat64(ctx, p[1]));
    JS_SetPropertyStr(ctx, pv, "z", JS_NewFloat64(ctx, p[2]));
    JS_SetPropertyStr(ctx, o, "pos", pv);
    return o;
}

static JSValue js_actors_inspect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint64_t ptr;
    if (!argc || js_to_u64(ctx, argv[0], &ptr) < 0) return JS_NULL;
    return actor_snapshot(ctx, ptr);
}

void hos_js_on_actor_update(Ctx *c, uint64_t ptr) {
    if (!js_has_event_listeners("actorUpdate") || !js_enter()) return;
    Ctx *prev = g_js_tls_ctx;
    g_js_tls_ctx = c;
    JSValue ev = actor_snapshot(g_js_ctx, ptr);
    if (!JS_IsNull(ev)) js_emit_event(g_js_ctx, "actorUpdate", 1, &ev);
    JS_FreeValue(g_js_ctx, ev);
    g_js_tls_ctx = prev;
    js_leave();
}

static JSValue js_actors_methods(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    uint64_t actor, table;
    double start, count;
    if (argc < 3) return JS_ThrowTypeError(ctx, "actors.getMethods(actor, firstSlot, count)");
    if (js_to_u64(ctx, argv[0], &actor) < 0) return JS_ThrowTypeError(ctx, "invalid actor");
    if (JS_ToFloat64(ctx, &start, argv[1]) < 0 || JS_ToFloat64(ctx, &count, argv[2]) < 0) return JS_EXCEPTION;
    if (!(start >= 0 && start <= UINT32_MAX && count >= 1 && count <= 128) ||
        floor(start) != start || floor(count) != count) return JS_ThrowRangeError(ctx, "invalid vtable slot range");
    if (!js_mem_u64(actor, &table) || !table) return JS_NULL;
    uint64_t methods[128];
    if (table > UINT64_MAX - ((uint64_t)start + (uint64_t)count) * 8 ||
        !js_mem_read(table + (uint64_t)start * 8, methods, (uint64_t)count * 8)) return JS_NULL;
    JSValue out = JS_NewArray(ctx);
    for (unsigned i = 0; i < (unsigned)count; i++)
        JS_SetPropertyUint32(ctx, out, i, methods[i] && a64_lookup(methods[i])
            ? JS_NewBigUint64(ctx, methods[i]) : JS_NULL);
    return out;
}

static JSValue js_actors_setPos(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint64_t ptr = 0; double x = 0, y = 0, z = 0;
    if (argc < 4 || js_to_u64(ctx, argv[0], &ptr) < 0 || JS_ToFloat64(ctx, &x, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &y, argv[2]) < 0 || JS_ToFloat64(ctx, &z, argv[3]) < 0)
        return JS_ThrowTypeError(ctx, "actors.setPos(ptr, x, y, z)");
    if (ptr == g_last_actor) return js_player_setPos(ctx, this_val, 3, argv + 1);
    float pos[3] = {(float)x, (float)y, (float)z};
    return JS_NewBool(ctx, js_mem_write(ptr + 0x31c, pos, sizeof pos));
}

void js_register_oot(JSContext *ctx, JSValue ns_game) {
    JSValue player = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, player, "getProgress", JS_NewCFunction(ctx, js_player_progress, "getProgress", 0));
    JS_SetPropertyStr(ctx, player, "getPos", JS_NewCFunction(ctx, js_player_getPos, "getPos", 0));
    JS_SetPropertyStr(ctx, player, "setPos", JS_NewCFunction(ctx, js_player_setPos, "setPos", 3));
    JS_SetPropertyStr(ctx, player, "getFacing", JS_NewCFunction(ctx, js_player_getFacing, "getFacing", 0));
    JS_SetPropertyStr(ctx, player, "setFacing", JS_NewCFunction(ctx, js_player_setFacing, "setFacing", 1));
    JS_SetPropertyStr(ctx, player, "getLife", JS_NewCFunction(ctx, js_player_getLife, "getLife", 0));
    JS_SetPropertyStr(ctx, player, "setLife", JS_NewCFunction(ctx, js_player_setLife, "setLife", 1));
    JS_SetPropertyStr(ctx, player, "getMaxLife", JS_NewCFunction(ctx, js_player_getMaxLife, "getMaxLife", 0));
    JS_SetPropertyStr(ctx, player, "setMaxLife", JS_NewCFunction(ctx, js_player_setMaxLife, "setMaxLife", 1));
    JS_SetPropertyStr(ctx, player, "getRupees", JS_NewCFunction(ctx, js_player_getRupees, "getRupees", 0));
    JS_SetPropertyStr(ctx, player, "setRupees", JS_NewCFunction(ctx, js_player_setRupees, "setRupees", 1));
    JS_SetPropertyStr(ctx, player, "getAge", JS_NewCFunction(ctx, js_player_getAge, "getAge", 0));
    JS_SetPropertyStr(ctx, player, "setAge", JS_NewCFunction(ctx, js_player_setAge, "setAge", 1));
    JS_SetPropertyStr(ctx, player, "getSpeedScale", JS_NewCFunction(ctx, js_player_getSpeedScale, "getSpeedScale", 0));
    JS_SetPropertyStr(ctx, player, "setSpeedScale", JS_NewCFunction(ctx, js_player_setSpeedScale, "setSpeedScale", 1));
    JS_SetPropertyStr(ctx, player, "setInvincible", JS_NewCFunction(ctx, js_player_setInvincible, "setInvincible", 1));
    JS_SetPropertyStr(ctx, player, "getInvincible", JS_NewCFunction(ctx, js_player_getInvincible, "getInvincible", 0));
    JS_SetPropertyStr(ctx, player, "setDamageMultiplier", JS_NewCFunction(ctx, js_player_setDamageMult, "setDamageMultiplier", 1));
    JS_SetPropertyStr(ctx, player, "getDamageMultiplier", JS_NewCFunction(ctx, js_player_getDamageMult, "getDamageMultiplier", 0));
    JS_SetPropertyStr(ctx, player, "setFlight", JS_NewCFunction(ctx, js_player_setFlight, "setFlight", 1));
    JS_SetPropertyStr(ctx, player, "getFlight", JS_NewCFunction(ctx, js_player_getFlight, "getFlight", 0));
    JS_SetPropertyStr(ctx, player, "getPrompt", JS_NewCFunction(ctx, js_player_getPrompt, "getPrompt", 0));
    JS_SetPropertyStr(ctx, player, "getPointers", JS_NewCFunction(ctx, js_player_getPtrs, "getPointers", 0));

    JSValue inv = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, inv, "list", JS_NewCFunction(ctx, js_inv_list, "list", 0));
    JS_SetPropertyStr(ctx, inv, "get", JS_NewCFunction(ctx, js_inv_get, "get", 1));
    JS_SetPropertyStr(ctx, inv, "has", JS_NewCFunction(ctx, js_inv_has, "has", 1));
    JS_SetPropertyStr(ctx, inv, "add", JS_NewCFunction(ctx, js_inv_add, "add", 2));
    JS_SetPropertyStr(ctx, inv, "remove", JS_NewCFunction(ctx, js_inv_remove, "remove", 2));
    JS_SetPropertyStr(ctx, inv, "set", JS_NewCFunction(ctx, js_inv_set, "set", 2));

    JSValue flags = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, flags, "list", JS_NewCFunction(ctx, js_flags_list, "list", 1));
    JS_SetPropertyStr(ctx, flags, "hash", JS_NewCFunction(ctx, js_flags_hash, "hash", 1));
    JS_SetPropertyStr(ctx, flags, "get", JS_NewCFunction(ctx, js_flags_get, "get", 1));
    JS_SetPropertyStr(ctx, flags, "getBool", JS_NewCFunction(ctx, js_flags_getBool, "getBool", 1));
    JS_SetPropertyStr(ctx, flags, "setBool", JS_NewCFunction(ctx, js_flags_setBool, "setBool", 3));
    JS_SetPropertyStr(ctx, flags, "getInt", JS_NewCFunction(ctx, js_flags_getInt, "getInt", 1));
    JS_SetPropertyStr(ctx, flags, "setInt", JS_NewCFunction(ctx, js_flags_setInt, "setInt", 2));
    JS_SetPropertyStr(ctx, flags, "getUInt", JS_NewCFunction(ctx, js_flags_getUInt, "getUInt", 1));
    JS_SetPropertyStr(ctx, flags, "setUInt", JS_NewCFunction(ctx, js_flags_setUInt, "setUInt", 2));
    JS_SetPropertyStr(ctx, flags, "getFloat", JS_NewCFunction(ctx, js_flags_getFloat, "getFloat", 1));
    JS_SetPropertyStr(ctx, flags, "setFloat", JS_NewCFunction(ctx, js_flags_setFloat, "setFloat", 2));
    JS_SetPropertyStr(ctx, flags, "getEnum", JS_NewCFunction(ctx, js_flags_getEnum, "getEnum", 1));
    JS_SetPropertyStr(ctx, flags, "setEnum", JS_NewCFunction(ctx, js_flags_setEnum, "setEnum", 2));
    JS_SetPropertyStr(ctx, flags, "getString", JS_NewCFunction(ctx, js_flags_getString, "getString", 1));
    JS_SetPropertyStr(ctx, flags, "setString", JS_NewCFunction(ctx, js_flags_setString, "setString", 2));
    JS_SetPropertyStr(ctx, flags, "getBool64", JS_NewCFunction(ctx, js_flags_getBool64, "getBool64", 1));
    JS_SetPropertyStr(ctx, flags, "setBool64", JS_NewCFunction(ctx, js_flags_setBool64, "setBool64", 2));
    JS_SetPropertyStr(ctx, flags, "watch", JS_NewCFunction(ctx, js_flags_watch, "watch", 2));

    JSValue world = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, world, "getAreaName", JS_NewCFunction(ctx, js_world_area, "getAreaName", 1));
    JS_SetPropertyStr(ctx, world, "getMap", JS_NewCFunction(ctx, js_world_getMap, "getMap", 0));
    JS_SetPropertyStr(ctx, world, "isLoading", JS_NewCFunction(ctx, js_world_isLoading, "isLoading", 0));
    JS_SetPropertyStr(ctx, world, "isEventPlaying", JS_NewCFunction(ctx, js_world_isEventPlaying, "isEventPlaying", 0));
    JS_SetPropertyStr(ctx, world, "getMusic", JS_NewCFunction(ctx, js_world_getMusic, "getMusic", 0));
    JS_SetPropertyStr(ctx, world, "getTime", JS_NewCFunction(ctx, js_world_getTime, "getTime", 0));
    JS_SetPropertyStr(ctx, world, "setTime", JS_NewCFunction(ctx, js_world_setTime, "setTime", 2));
    JS_SetPropertyStr(ctx, world, "destinations", JS_NewCFunction(ctx, js_world_destinations, "destinations", 0));
    JS_SetPropertyStr(ctx, world, "teleport", JS_NewCFunction(ctx, js_world_teleport, "teleport", 1));
    JS_SetPropertyStr(ctx, world, "triggerEvent", JS_NewCFunction(ctx, js_world_triggerEvent, "triggerEvent", 2));
    JS_SetPropertyStr(ctx, world, "getSingletons", JS_NewCFunction(ctx, js_world_singletons, "getSingletons", 0));

    JSValue cam = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cam, "getPose", JS_NewCFunction(ctx, js_cam_getPose, "getPose", 0));
    JS_SetPropertyStr(ctx, cam, "setOverride", JS_NewCFunction(ctx, js_cam_setOverride, "setOverride", 1));
    JS_SetPropertyStr(ctx, cam, "setPlayerAngles", JS_NewCFunction(ctx, js_cam_setPlayerAngles, "setPlayerAngles", 2));
    JS_SetPropertyStr(ctx, cam, "clearPlayerAngles", JS_NewCFunction(ctx, js_cam_clearPlayerAngles, "clearPlayerAngles", 0));
    JS_SetPropertyStr(ctx, cam, "setFovScale", JS_NewCFunction(ctx, js_cam_setFovScale, "setFovScale", 1));
    JS_SetPropertyStr(ctx, cam, "getFovScale", JS_NewCFunction(ctx, js_cam_getFovScale, "getFovScale", 0));
    JS_SetPropertyStr(ctx, cam, "setSpeedScale", JS_NewCFunction(ctx, js_cam_setSpeedScale, "setSpeedScale", 1));
    JS_SetPropertyStr(ctx, cam, "getSpeedScale", JS_NewCFunction(ctx, js_cam_getSpeedScale, "getSpeedScale", 0));
    JS_SetPropertyStr(ctx, cam, "setDisableCentering", JS_NewCFunction(ctx, js_cam_setDisableCentering, "setDisableCentering", 1));
    JS_SetPropertyStr(ctx, cam, "worldToScreen", JS_NewCFunction(ctx, js_cam_worldToScreen, "worldToScreen", 4));

    JSValue actors = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, actors, "getPlayer", JS_NewCFunction(ctx, js_actors_getPlayer, "getPlayer", 0));
    JS_SetPropertyStr(ctx, actors, "getMethods", JS_NewCFunction(ctx, js_actors_methods, "getMethods", 3));
    JS_SetPropertyStr(ctx, actors, "inspect", JS_NewCFunction(ctx, js_actors_inspect, "inspect", 1));
    JS_SetPropertyStr(ctx, actors, "setPos", JS_NewCFunction(ctx, js_actors_setPos, "setPos", 4));

    JS_SetPropertyStr(ctx, ns_game, "player", player);
    JS_SetPropertyStr(ctx, ns_game, "inventory", inv);
    JS_SetPropertyStr(ctx, ns_game, "flags", flags);
    JS_SetPropertyStr(ctx, ns_game, "world", world);
    JS_SetPropertyStr(ctx, ns_game, "camera", cam);
    JS_SetPropertyStr(ctx, ns_game, "actors", actors);
}

void js_oot_reset(JSContext *ctx) {
    __atomic_store_n(&g_invincible, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_flight, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_disable_centering, -1, __ATOMIC_RELAXED);
    atomic_store_f32(&g_speed_scale_bits, 1.0f);
    atomic_store_f32(&g_damage_mult_bits, 1.0f);
    atomic_store_f32(&g_fov_scale_bits, 1.0f);
    atomic_store_f32(&g_cam_speed_scale_bits, 1.0f);

    AcquireSRWLockExclusive(&g_oot_lock);
    g_cam_override_on = 0;
    g_player_cam_angles_on = 0;
    g_action_q_n = 0;
    for (int i = 0; i < PLAYER_TASK_MAX; i++) {
        if (g_player_tasks[i].used && ctx) {
            JS_FreeValue(ctx, g_player_tasks[i].fn);
            JS_FreeValue(ctx, g_player_tasks[i].resolve);
            JS_FreeValue(ctx, g_player_tasks[i].reject);
        }
        g_player_tasks[i].used = 0;
    }
    ReleaseSRWLockExclusive(&g_oot_lock);

    for (int i = 0; i < FLAG_WATCH_MAX; i++) {
        if (g_watches[i].used && ctx) {
            JS_FreeValue(ctx, g_watches[i].cb);
            JS_FreeValue(ctx, g_watches[i].key_val);
        }
        memset(&g_watches[i], 0, sizeof g_watches[i]);
    }
}
