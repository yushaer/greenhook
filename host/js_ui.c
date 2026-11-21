/* Thread-safe double-buffered Dear ImGui HUD primitives, toasts, interactive windows & UI event queue. */
#include "js_internal.h"
#include <ctype.h>

static SRWLOCK g_ui_lock = SRWLOCK_INIT;

static MkwVkJsPrim g_write_prims[MKWVK_JS_MAX_PRIMS];
static int g_write_n = 0;
static int g_write_dirty = 0;

static MkwVkJsPrim g_front_prims[MKWVK_JS_MAX_PRIMS];
static int g_front_n = 0;

typedef struct {
    int used;
    MkwVkJsToast data;
    int64_t start_ns, end_ns;
} ToastSlot;
static ToastSlot g_toasts[MKWVK_JS_MAX_TOASTS];

typedef struct {
    JSValue on_action;
} WidgetCb;

static MkwVkJsWindow g_windows[MKWVK_JS_MAX_WINDOWS];
static WidgetCb g_win_cbs[MKWVK_JS_MAX_WINDOWS][MKWVK_JS_MAX_WIDGETS];
static JSValue g_win_on_close[MKWVK_JS_MAX_WINDOWS];
static int g_window_count = 0;

static int g_console_open_req = -1; /* -1 unchanged, 0 force close, 1 force open */
static int g_console_is_open = 0;

#define UI_EV_RING 64
static MkwVkJsEvent g_ev_ring[UI_EV_RING];
static unsigned g_ev_head = 0, g_ev_tail = 0;

static uint32_t pack_abgr(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

uint32_t js_parse_color(JSContext *ctx, JSValueConst val, uint32_t def) {
    if (JS_IsUndefined(val) || JS_IsNull(val)) return def;
    if (JS_IsString(val)) {
        const char *s = JS_ToCString(ctx, val);
        if (!s) return def;
        uint32_t res = def;
        if (s[0] == '#') {
            const size_t len = strlen(s + 1);
            if (len == 6 || len == 8) {
                uint32_t v = (uint32_t)strtoul(s + 1, NULL, 16);
                if (len == 6) {
                    res = pack_abgr((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 255);
                } else {
                    res = pack_abgr((v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
                }
            } else if (len == 3) {
                uint32_t v = (uint32_t)strtoul(s + 1, NULL, 16);
                uint8_t r = (uint8_t)(((v >> 8) & 0xf) * 17);
                uint8_t g = (uint8_t)(((v >> 4) & 0xf) * 17);
                uint8_t b = (uint8_t)((v & 0xf) * 17);
                res = pack_abgr(r, g, b, 255);
            }
        } else if (!_stricmp(s, "white")) res = pack_abgr(255, 255, 255, 255);
        else if (!_stricmp(s, "black")) res = pack_abgr(0, 0, 0, 255);
        else if (!_stricmp(s, "red")) res = pack_abgr(255, 80, 80, 255);
        else if (!_stricmp(s, "green")) res = pack_abgr(80, 255, 120, 255);
        else if (!_stricmp(s, "blue")) res = pack_abgr(90, 170, 255, 255);
        else if (!_stricmp(s, "yellow")) res = pack_abgr(255, 225, 80, 255);
        else if (!_stricmp(s, "cyan")) res = pack_abgr(80, 235, 255, 255);
        else if (!_stricmp(s, "gold")) res = pack_abgr(255, 210, 60, 255);
        JS_FreeCString(ctx, s);
        return res;
    }
    if (JS_IsNumber(val)) {
        uint32_t u = 0;
        if (JS_ToUint32(ctx, &u, val) < 0) return def;
        uint8_t a = (u >> 24) & 0xff;
        if (!a && u <= 0xffffffu) a = 255;
        return pack_abgr((u >> 16) & 0xff, (u >> 8) & 0xff, u & 0xff, a);
    }
    if (JS_IsArray(val)) {
        double c[4] = {255, 255, 255, 255};
        double maxv = 0;
        for (uint32_t i = 0; i < 4; i++) {
            JSValue iv = JS_GetPropertyUint32(ctx, val, i);
            if (!JS_IsUndefined(iv)) {
                JS_ToFloat64(ctx, &c[i], iv);
                if (c[i] > maxv) maxv = c[i];
            } else if (i == 3) {
                c[3] = maxv <= 1.0 ? 1.0 : 255.0;
            }
            JS_FreeValue(ctx, iv);
        }
        const double scale = maxv <= 1.0 ? 255.0 : 1.0;
        uint8_t cl[4];
        for (int i = 0; i < 4; i++) {
            long v = lround(c[i] * scale);
            cl[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        return pack_abgr(cl[0], cl[1], cl[2], cl[3]);
    }
    return def;
}

static void swap_write_to_front_locked(void) {
    if (!g_write_dirty) return;
    g_front_n = g_write_n;
    if (g_front_n > 0) memcpy(g_front_prims, g_write_prims, sizeof(MkwVkJsPrim) * (size_t)g_front_n);
    g_write_n = 0;
    g_write_dirty = 0;
}

void js_ui_begin_frame(void) {
    AcquireSRWLockExclusive(&g_ui_lock);
    if (g_write_dirty) swap_write_to_front_locked();
    else { g_write_n = 0; g_write_dirty = 0; }
    ReleaseSRWLockExclusive(&g_ui_lock);
}

void js_ui_end_frame(void) {
    AcquireSRWLockExclusive(&g_ui_lock);
    if (g_write_dirty) swap_write_to_front_locked();
    ReleaseSRWLockExclusive(&g_ui_lock);
}

static void push_prim(const MkwVkJsPrim *p) {
    AcquireSRWLockExclusive(&g_ui_lock);
    if (g_write_n < MKWVK_JS_MAX_PRIMS) g_write_prims[g_write_n++] = *p;
    g_write_dirty = 1;
    ReleaseSRWLockExclusive(&g_ui_lock);
}

static uint32_t opt_color(JSContext *ctx, JSValueConst opts, const char *key, uint32_t def) {
    if (!JS_IsObject(opts)) return def;
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    const uint32_t c = js_parse_color(ctx, v, def);
    JS_FreeValue(ctx, v);
    return c;
}
static float opt_f32(JSContext *ctx, JSValueConst opts, const char *key, float def) {
    if (!JS_IsObject(opts)) return def;
    JSValue v = JS_GetPropertyStr(ctx, opts, key);
    double d = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return (float)d;
}

/* ---- ui.draw.* ---- */
static JSValue js_draw_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double x = 0, y = 0;
    if (argc < 3 || JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[2]);
    if (!s) return JS_EXCEPTION;
    MkwVkJsPrim p = {0};
    p.kind = MKWVK_JS_PRIM_TEXT;
    p.x = (float)x;
    p.y = (float)y;
    JSValueConst opts = argc >= 4 ? argv[3] : JS_UNDEFINED;
    p.color = opt_color(ctx, opts, "color", pack_abgr(255, 255, 255, 255));
    p.bg_color = opt_color(ctx, opts, "bgColor", 0);
    p.scale = opt_f32(ctx, opts, "size", opt_f32(ctx, opts, "scale", 18.0f));
    snprintf(p.text, sizeof p.text, "%s", s);
    JS_FreeCString(ctx, s);
    push_prim(&p);
    return JS_UNDEFINED;
}

static JSValue js_draw_rect_impl(JSContext *ctx, int argc, JSValueConst *argv, int filled) {
    double x = 0, y = 0, w = 0, h = 0;
    if (argc < 4 || JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &w, argv[2]) < 0 || JS_ToFloat64(ctx, &h, argv[3]) < 0)
        return JS_UNDEFINED;
    MkwVkJsPrim p = {0};
    p.kind = filled ? MKWVK_JS_PRIM_RECT_FILLED : MKWVK_JS_PRIM_RECT;
    p.x = (float)x; p.y = (float)y; p.w = (float)w; p.h = (float)h;
    JSValueConst opts = argc >= 5 ? argv[4] : JS_UNDEFINED;
    p.color = opt_color(ctx, opts, "color", pack_abgr(255, 255, 255, 200));
    p.scale = opt_f32(ctx, opts, "rounding", 0.0f);
    p.thickness = opt_f32(ctx, opts, "thickness", 1.5f);
    push_prim(&p);
    return JS_UNDEFINED;
}
static JSValue js_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; return js_draw_rect_impl(ctx, argc, argv, 0);
}
static JSValue js_draw_rectFilled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; return js_draw_rect_impl(ctx, argc, argv, 1);
}

static JSValue js_draw_line(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (argc < 4 || JS_ToFloat64(ctx, &x1, argv[0]) < 0 || JS_ToFloat64(ctx, &y1, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &x2, argv[2]) < 0 || JS_ToFloat64(ctx, &y2, argv[3]) < 0)
        return JS_UNDEFINED;
    MkwVkJsPrim p = {0};
    p.kind = MKWVK_JS_PRIM_LINE;
    p.x = (float)x1; p.y = (float)y1; p.w = (float)x2; p.h = (float)y2;
    JSValueConst opts = argc >= 5 ? argv[4] : JS_UNDEFINED;
    p.color = opt_color(ctx, opts, "color", pack_abgr(255, 255, 255, 255));
    p.thickness = opt_f32(ctx, opts, "thickness", 1.5f);
    push_prim(&p);
    return JS_UNDEFINED;
}

static JSValue js_draw_circle_impl(JSContext *ctx, int argc, JSValueConst *argv, int filled) {
    double cx = 0, cy = 0, r = 0;
    if (argc < 3 || JS_ToFloat64(ctx, &cx, argv[0]) < 0 || JS_ToFloat64(ctx, &cy, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &r, argv[2]) < 0)
        return JS_UNDEFINED;
    MkwVkJsPrim p = {0};
    p.kind = filled ? MKWVK_JS_PRIM_CIRCLE_FILLED : MKWVK_JS_PRIM_CIRCLE;
    p.x = (float)cx; p.y = (float)cy; p.w = (float)r;
    JSValueConst opts = argc >= 4 ? argv[3] : JS_UNDEFINED;
    p.color = opt_color(ctx, opts, "color", pack_abgr(255, 255, 255, 220));
    p.thickness = opt_f32(ctx, opts, "thickness", 1.5f);
    push_prim(&p);
    return JS_UNDEFINED;
}
static JSValue js_draw_circle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; return js_draw_circle_impl(ctx, argc, argv, 0);
}
static JSValue js_draw_circleFilled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; return js_draw_circle_impl(ctx, argc, argv, 1);
}

static JSValue js_draw_progress(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    double x = 0, y = 0, w = 0, h = 0, frac = 0;
    if (argc < 5 || JS_ToFloat64(ctx, &x, argv[0]) < 0 || JS_ToFloat64(ctx, &y, argv[1]) < 0 ||
        JS_ToFloat64(ctx, &w, argv[2]) < 0 || JS_ToFloat64(ctx, &h, argv[3]) < 0 ||
        JS_ToFloat64(ctx, &frac, argv[4]) < 0)
        return JS_UNDEFINED;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    MkwVkJsPrim p = {0};
    p.kind = MKWVK_JS_PRIM_PROGRESS;
    p.x = (float)x; p.y = (float)y; p.w = (float)w; p.h = (float)h;
    p.scale = (float)frac;
    JSValueConst opts = argc >= 6 ? argv[5] : JS_UNDEFINED;
    p.color = opt_color(ctx, opts, "color", pack_abgr(80, 210, 120, 235));
    p.bg_color = opt_color(ctx, opts, "bgColor", pack_abgr(20, 24, 30, 190));
    if (JS_IsObject(opts)) {
        JSValue tv = JS_GetPropertyStr(ctx, opts, "text");
        if (JS_IsString(tv)) {
            const char *ts = JS_ToCString(ctx, tv);
            if (ts) snprintf(p.text, sizeof p.text, "%s", ts), JS_FreeCString(ctx, ts);
        }
        JS_FreeValue(ctx, tv);
    }
    push_prim(&p);
    return JS_UNDEFINED;
}

static JSValue js_draw_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_ui_lock);
    g_write_n = 0;
    g_front_n = 0;
    g_write_dirty = 0;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_UNDEFINED;
}

/* ---- ui.toast ---- */
static JSValue js_ui_toast(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) return JS_EXCEPTION;
    double dur_ms = 3000.0;
    uint32_t col = pack_abgr(255, 225, 110, 255);
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue dv = JS_GetPropertyStr(ctx, argv[1], "durationMs");
        if (JS_IsNumber(dv)) JS_ToFloat64(ctx, &dur_ms, dv);
        JS_FreeValue(ctx, dv);
        col = opt_color(ctx, argv[1], "color", col);
    }
    if (dur_ms < 250.0) dur_ms = 250.0;
    if (dur_ms > 30000.0) dur_ms = 30000.0;
    const int64_t now = hos_now_ns();

    AcquireSRWLockExclusive(&g_ui_lock);
    int slot = -1;
    for (int i = 0; i < MKWVK_JS_MAX_TOASTS; i++) {
        if (!g_toasts[i].used || now >= g_toasts[i].end_ns) { slot = i; break; }
    }
    if (slot < 0) {
        int oldest = 0;
        for (int i = 1; i < MKWVK_JS_MAX_TOASTS; i++)
            if (g_toasts[i].start_ns < g_toasts[oldest].start_ns) oldest = i;
        slot = oldest;
    }
    g_toasts[slot].used = 1;
    g_toasts[slot].start_ns = now;
    g_toasts[slot].end_ns = now + (int64_t)(dur_ms * 1e6);
    g_toasts[slot].data.color = col;
    g_toasts[slot].data.alpha = 1.0f;
    snprintf(g_toasts[slot].data.text, sizeof g_toasts[slot].data.text, "%s", msg);
    ReleaseSRWLockExclusive(&g_ui_lock);

    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

/* ---- ui.window ---- */
static JSValue win_setOpen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    if (magic < 0 || magic >= g_window_count || argc < 1) return JS_UNDEFINED;
    const int on = JS_ToBool(ctx, argv[0]) ? 1 : 0;
    AcquireSRWLockExclusive(&g_ui_lock);
    g_windows[magic].open = on;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_UNDEFINED;
}
static JSValue win_isOpen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val, (void)argc, (void)argv;
    if (magic < 0 || magic >= g_window_count) return JS_FALSE;
    AcquireSRWLockShared(&g_ui_lock);
    const int on = g_windows[magic].used && g_windows[magic].open;
    ReleaseSRWLockShared(&g_ui_lock);
    return JS_NewBool(ctx, on);
}
static JSValue win_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    if (magic >= 0 && magic < g_window_count) {
        AcquireSRWLockExclusive(&g_ui_lock);
        g_windows[magic].open = 0;
        ReleaseSRWLockExclusive(&g_ui_lock);
    }
    return JS_UNDEFINED;
}
static JSValue win_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val, (void)argc, (void)argv;
    if (magic < 0 || magic >= g_window_count) return this_val;
    AcquireSRWLockExclusive(&g_ui_lock);
    for (int i = 0; i < g_windows[magic].widget_count; i++) {
        if (!JS_IsUndefined(g_win_cbs[magic][i].on_action)) {
            JS_FreeValue(ctx, g_win_cbs[magic][i].on_action);
            g_win_cbs[magic][i].on_action = JS_UNDEFINED;
        }
    }
    g_windows[magic].widget_count = 0;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_DupValue(ctx, this_val);
}

static MkwVkJsWidget *find_or_add_widget(int wi, int kind, const char *id, int *out_idx) {
    MkwVkJsWindow *w = &g_windows[wi];
    if (id && *id && kind != MKWVK_JS_WIDGET_LABEL && kind != MKWVK_JS_WIDGET_SEPARATOR && kind != MKWVK_JS_WIDGET_SAME_LINE) {
        for (int i = 0; i < w->widget_count; i++) {
            if (w->widgets[i].kind == kind && !strcmp(w->widgets[i].id, id)) {
                *out_idx = i;
                return &w->widgets[i];
            }
        }
    }
    if (w->widget_count >= MKWVK_JS_MAX_WIDGETS) return NULL;
    const int idx = w->widget_count++;
    MkwVkJsWidget *wg = &w->widgets[idx];
    memset(wg, 0, sizeof *wg);
    wg->kind = kind;
    if (id) snprintf(wg->id, sizeof wg->id, "%s", id);
    *out_idx = idx;
    return wg;
}

static JSValue win_label(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (magic < 0 || magic >= g_window_count || argc < 1) return JS_DupValue(ctx, this_val);
    const char *txt = JS_ToCString(ctx, argv[0]);
    if (!txt) return JS_EXCEPTION;
    uint32_t col = argc >= 2 ? opt_color(ctx, argv[1], "color", pack_abgr(230, 235, 242, 255))
                             : pack_abgr(230, 235, 242, 255);
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    MkwVkJsWidget *wg = find_or_add_widget(magic, MKWVK_JS_WIDGET_LABEL, NULL, &idx);
    if (wg) {
        snprintf(wg->label, sizeof wg->label, "%s", txt);
        wg->color = col;
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
    JS_FreeCString(ctx, txt);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_sep(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)argc, (void)argv;
    if (magic < 0 || magic >= g_window_count) return JS_DupValue(ctx, this_val);
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    find_or_add_widget(magic, MKWVK_JS_WIDGET_SEPARATOR, NULL, &idx);
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_sameLine(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)argc, (void)argv;
    if (magic < 0 || magic >= g_window_count) return JS_DupValue(ctx, this_val);
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    find_or_add_widget(magic, MKWVK_JS_WIDGET_SAME_LINE, NULL, &idx);
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_button(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (magic < 0 || magic >= g_window_count || argc < 2) return JS_DupValue(ctx, this_val);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *label = JS_ToCString(ctx, argv[1]);
    if (!id || !label) {
        if (id) JS_FreeCString(ctx, id);
        if (label) JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
    }
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    MkwVkJsWidget *wg = find_or_add_widget(magic, MKWVK_JS_WIDGET_BUTTON, id, &idx);
    if (wg) {
        snprintf(wg->label, sizeof wg->label, "%s", label);
        if (!JS_IsUndefined(g_win_cbs[magic][idx].on_action))
            JS_FreeValue(ctx, g_win_cbs[magic][idx].on_action);
        g_win_cbs[magic][idx].on_action = (argc >= 3 && JS_IsFunction(ctx, argv[2]))
                                              ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, label);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_checkbox(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (magic < 0 || magic >= g_window_count || argc < 2) return JS_DupValue(ctx, this_val);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *label = JS_ToCString(ctx, argv[1]);
    if (!id || !label) {
        if (id) JS_FreeCString(ctx, id);
        if (label) JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
    }
    const int init = argc >= 3 ? (JS_ToBool(ctx, argv[2]) ? 1 : 0) : 0;
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    int existed = 0;
    for (int i = 0; i < g_windows[magic].widget_count; i++)
        if (g_windows[magic].widgets[i].kind == MKWVK_JS_WIDGET_CHECKBOX && !strcmp(g_windows[magic].widgets[i].id, id))
            existed = 1;
    MkwVkJsWidget *wg = find_or_add_widget(magic, MKWVK_JS_WIDGET_CHECKBOX, id, &idx);
    if (wg) {
        snprintf(wg->label, sizeof wg->label, "%s", label);
        if (!existed) wg->bval = init;
        if (argc >= 4) {
            if (!JS_IsUndefined(g_win_cbs[magic][idx].on_action))
                JS_FreeValue(ctx, g_win_cbs[magic][idx].on_action);
            g_win_cbs[magic][idx].on_action = JS_IsFunction(ctx, argv[3]) ? JS_DupValue(ctx, argv[3]) : JS_UNDEFINED;
        }
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, label);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_slider(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (magic < 0 || magic >= g_window_count || argc < 5) return JS_DupValue(ctx, this_val);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *label = JS_ToCString(ctx, argv[1]);
    double fmin = 0, fmax = 1, init = 0;
    if (!id || !label || JS_ToFloat64(ctx, &fmin, argv[2]) < 0 || JS_ToFloat64(ctx, &fmax, argv[3]) < 0 ||
        JS_ToFloat64(ctx, &init, argv[4]) < 0) {
        if (id) JS_FreeCString(ctx, id);
        if (label) JS_FreeCString(ctx, label);
        return JS_EXCEPTION;
    }
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1, existed = 0;
    for (int i = 0; i < g_windows[magic].widget_count; i++)
        if (g_windows[magic].widgets[i].kind == MKWVK_JS_WIDGET_SLIDER && !strcmp(g_windows[magic].widgets[i].id, id))
            existed = 1;
    MkwVkJsWidget *wg = find_or_add_widget(magic, MKWVK_JS_WIDGET_SLIDER, id, &idx);
    if (wg) {
        snprintf(wg->label, sizeof wg->label, "%s", label);
        wg->fmin = (float)fmin;
        wg->fmax = (float)fmax;
        if (!existed) wg->fval = (float)init;
        if (argc >= 6) {
            if (!JS_IsUndefined(g_win_cbs[magic][idx].on_action))
                JS_FreeValue(ctx, g_win_cbs[magic][idx].on_action);
            g_win_cbs[magic][idx].on_action = JS_IsFunction(ctx, argv[5]) ? JS_DupValue(ctx, argv[5]) : JS_UNDEFINED;
        }
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, label);
    return JS_DupValue(ctx, this_val);
}

static JSValue win_progress(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (magic < 0 || magic >= g_window_count || argc < 1) return JS_DupValue(ctx, this_val);
    double frac = 0;
    if (JS_ToFloat64(ctx, &frac, argv[0]) < 0) return JS_EXCEPTION;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    const char *label = (argc >= 2 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;
    AcquireSRWLockExclusive(&g_ui_lock);
    int idx = -1;
    MkwVkJsWidget *wg = find_or_add_widget(magic, MKWVK_JS_WIDGET_PROGRESS, NULL, &idx);
    if (wg) {
        wg->fval = (float)frac;
        if (label) snprintf(wg->label, sizeof wg->label, "%s", label);
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
    if (label) JS_FreeCString(ctx, label);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_ui_window(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "ui.window(id, title, opts?)");
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *title = JS_ToCString(ctx, argv[1]);
    if (!id || !title) {
        if (id) JS_FreeCString(ctx, id);
        if (title) JS_FreeCString(ctx, title);
        return JS_EXCEPTION;
    }
    JSValueConst opts = argc >= 3 ? argv[2] : JS_UNDEFINED;
    float x = opt_f32(ctx, opts, "x", 60.0f);
    float y = opt_f32(ctx, opts, "y", 60.0f);
    float w = opt_f32(ctx, opts, "w", opt_f32(ctx, opts, "width", 340.0f));
    float h = opt_f32(ctx, opts, "h", opt_f32(ctx, opts, "height", 0.0f));
    int open = 1;
    JSValue on_close = JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        JSValue ov = JS_GetPropertyStr(ctx, opts, "open");
        if (JS_IsBool(ov)) open = JS_ToBool(ctx, ov) ? 1 : 0;
        JS_FreeValue(ctx, ov);
        JSValue cv = JS_GetPropertyStr(ctx, opts, "onClose");
        if (JS_IsFunction(ctx, cv)) on_close = cv;
        else JS_FreeValue(ctx, cv);
    }

    AcquireSRWLockExclusive(&g_ui_lock);
    int wi = -1;
    for (int i = 0; i < g_window_count; i++)
        if (g_windows[i].used && !strcmp(g_windows[i].id, id)) { wi = i; break; }
    if (wi < 0 && g_window_count < MKWVK_JS_MAX_WINDOWS) {
        wi = g_window_count++;
        memset(&g_windows[wi], 0, sizeof g_windows[wi]);
        for (int j = 0; j < MKWVK_JS_MAX_WIDGETS; j++) g_win_cbs[wi][j].on_action = JS_UNDEFINED;
        g_win_on_close[wi] = JS_UNDEFINED;
        g_windows[wi].open = open;
    }
    if (wi >= 0) {
        g_windows[wi].used = 1;
        snprintf(g_windows[wi].id, sizeof g_windows[wi].id, "%s", id);
        snprintf(g_windows[wi].title, sizeof g_windows[wi].title, "%s", title);
        g_windows[wi].x = x; g_windows[wi].y = y; g_windows[wi].w = w; g_windows[wi].h = h;
        if (!JS_IsUndefined(on_close)) {
            if (!JS_IsUndefined(g_win_on_close[wi])) JS_FreeValue(ctx, g_win_on_close[wi]);
            g_win_on_close[wi] = on_close;
        }
    } else if (!JS_IsUndefined(on_close)) {
        JS_FreeValue(ctx, on_close);
    }
    ReleaseSRWLockExclusive(&g_ui_lock);

    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, title);
    if (wi < 0) return JS_ThrowRangeError(ctx, "window limit (%d) reached", MKWVK_JS_MAX_WINDOWS);

    JSValue handle = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, handle, "id", JS_NewString(ctx, g_windows[wi].id));
    JS_SetPropertyStr(ctx, handle, "setOpen", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_setOpen, "setOpen", 1, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "isOpen", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_isOpen, "isOpen", 0, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "close", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_close, "close", 0, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "clear", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_clear, "clear", 0, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "label", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_label, "label", 2, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "separator", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_sep, "separator", 0, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "sameLine", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_sameLine, "sameLine", 0, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "button", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_button, "button", 3, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "checkbox", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_checkbox, "checkbox", 4, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "slider", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_slider, "slider", 6, JS_CFUNC_generic_magic, wi));
    JS_SetPropertyStr(ctx, handle, "progressBar", JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)win_progress, "progressBar", 2, JS_CFUNC_generic_magic, wi));
    return handle;
}

/* ---- ui.console ---- */
static JSValue js_cons_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_ui_lock);
    g_console_open_req = 1;
    g_console_is_open = 1;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_UNDEFINED;
}
static JSValue js_cons_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_ui_lock);
    g_console_open_req = 0;
    g_console_is_open = 0;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_UNDEFINED;
}
static JSValue js_cons_toggle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockExclusive(&g_ui_lock);
    g_console_is_open = !g_console_is_open;
    g_console_open_req = g_console_is_open;
    ReleaseSRWLockExclusive(&g_ui_lock);
    return JS_NewBool(ctx, g_console_is_open);
}
static JSValue js_cons_isOpen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val, (void)argc, (void)argv;
    AcquireSRWLockShared(&g_ui_lock);
    const int o = g_console_is_open;
    ReleaseSRWLockShared(&g_ui_lock);
    return JS_NewBool(ctx, o);
}
static JSValue js_cons_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx, (void)this_val, (void)argc, (void)argv;
    js_console_clear();
    return JS_UNDEFINED;
}

/* ---- Renderer-side exports (NEVER acquire g_js_cs!) ---- */
int hos_js_ui_active(void) {
    const int64_t now = hos_now_ns();
    int active = 0;
    AcquireSRWLockShared(&g_ui_lock);
    if (g_front_n > 0) active = 1;
    for (int i = 0; i < g_window_count && !active; i++)
        if (g_windows[i].used && g_windows[i].open) active = 1;
    for (int i = 0; i < MKWVK_JS_MAX_TOASTS && !active; i++)
        if (g_toasts[i].used && now < g_toasts[i].end_ns) active = 1;
    if (g_console_open_req >= 0 || g_console_is_open) active = 1;
    ReleaseSRWLockShared(&g_ui_lock);
    return active;
}

int hos_js_ui_snapshot_vk(void *out_ptr) {
    MkwVkJsSnapshot *out = (MkwVkJsSnapshot *)out_ptr;
    if (!out || out->struct_size != sizeof(MkwVkJsSnapshot)) return 0;

    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&g_ui_lock);
    out->prim_count = g_front_n;
    if (g_front_n > 0) memcpy(out->prims, g_front_prims, sizeof(MkwVkJsPrim) * (size_t)g_front_n);

    out->toast_count = 0;
    for (int i = 0; i < MKWVK_JS_MAX_TOASTS; i++) {
        if (!g_toasts[i].used || now >= g_toasts[i].end_ns) continue;
        MkwVkJsToast t = g_toasts[i].data;
        const int64_t rem = g_toasts[i].end_ns - now;
        if (rem < 400000000LL) t.alpha = (float)rem / 400000000.0f;
        out->toasts[out->toast_count++] = t;
    }

    out->window_count = g_window_count;
    if (g_window_count > 0) memcpy(out->windows, g_windows, sizeof(MkwVkJsWindow) * (size_t)g_window_count);

    out->console_open_req = g_console_open_req;
    g_console_open_req = -1;
    ReleaseSRWLockExclusive(&g_ui_lock);

    out->runtime_ready = hos_js_is_ready();
    snprintf(out->scripts_dir, sizeof out->scripts_dir, "%s", js_scripts_dir());
    out->script_count = js_scripts_copy(out->scripts, MKWVK_JS_MAX_SCRIPTS);
    out->log_count = js_console_copy(out->logs, MKWVK_JS_MAX_LOGS);
    return 1;
}

void hos_js_ui_event_vk(const void *ev_ptr) {
    const MkwVkJsEvent *ev = (const MkwVkJsEvent *)ev_ptr;
    if (!ev) return;

    AcquireSRWLockExclusive(&g_ui_lock);
    /* Apply immediate local UI state changes so next render snapshot never flickers */
    if (ev->kind == MKWVK_JS_EV_CONSOLE_STATE) {
        g_console_is_open = ev->bval ? 1 : 0;
        ReleaseSRWLockExclusive(&g_ui_lock);
        return;
    }
    if (ev->kind == MKWVK_JS_EV_CONSOLE_CLEAR) {
        ReleaseSRWLockExclusive(&g_ui_lock);
        js_console_clear();
        return;
    }
    if (ev->kind == MKWVK_JS_EV_CHECKBOX || ev->kind == MKWVK_JS_EV_SLIDER || ev->kind == MKWVK_JS_EV_WINDOW_CLOSE) {
        for (int w = 0; w < g_window_count; w++) {
            if (!g_windows[w].used || strcmp(g_windows[w].id, ev->target_id)) continue;
            if (ev->kind == MKWVK_JS_EV_WINDOW_CLOSE) {
                g_windows[w].open = 0;
            } else {
                for (int i = 0; i < g_windows[w].widget_count; i++) {
                    if (!strcmp(g_windows[w].widgets[i].id, ev->widget_id)) {
                        if (ev->kind == MKWVK_JS_EV_CHECKBOX) g_windows[w].widgets[i].bval = ev->bval;
                        else g_windows[w].widgets[i].fval = ev->fval;
                        break;
                    }
                }
            }
            break;
        }
    }
    if (g_ev_tail - g_ev_head < UI_EV_RING) {
        g_ev_ring[g_ev_tail++ % UI_EV_RING] = *ev;
    }
    ReleaseSRWLockExclusive(&g_ui_lock);
}

void js_ui_pump_events(JSContext *ctx) {
    MkwVkJsEvent batch[UI_EV_RING];
    int n = 0;
    AcquireSRWLockExclusive(&g_ui_lock);
    while (g_ev_head < g_ev_tail && n < UI_EV_RING) {
        batch[n++] = g_ev_ring[g_ev_head++ % UI_EV_RING];
    }
    ReleaseSRWLockExclusive(&g_ui_lock);

    for (int i = 0; i < n; i++) {
        const MkwVkJsEvent *ev = &batch[i];
        if (ev->kind == MKWVK_JS_EV_RELOAD) {
            js_request_reload();
            continue;
        }
        if (ev->kind == MKWVK_JS_EV_SCRIPT_TOGGLE) {
            js_set_script_enabled(ev->widget_id, ev->bval);
            js_request_reload();
            continue;
        }
        if (ev->kind == MKWVK_JS_EV_EVAL) {
            js_console_push(0, ">", ev->text);
            JSValue val = JS_Eval(ctx, ev->text, strlen(ev->text), "<console>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(val)) {
                js_log_exception(ctx, "console");
            } else {
                js_drain_jobs(ctx);
                if (!JS_IsUndefined(val)) {
                    const char *s = JS_ToCString(ctx, val);
                    if (s) js_console_push(1, "=>", s), JS_FreeCString(ctx, s);
                }
            }
            JS_FreeValue(ctx, val);
            continue;
        }
        if (ev->kind == MKWVK_JS_EV_WINDOW_CLOSE) {
            JSValue cb = JS_UNDEFINED;
            AcquireSRWLockShared(&g_ui_lock);
            for (int w = 0; w < g_window_count; w++) {
                if (g_windows[w].used && !strcmp(g_windows[w].id, ev->target_id) && !JS_IsUndefined(g_win_on_close[w])) {
                    cb = JS_DupValue(ctx, g_win_on_close[w]);
                    break;
                }
            }
            ReleaseSRWLockShared(&g_ui_lock);
            if (!JS_IsUndefined(cb)) {
                JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, 0, NULL);
                if (JS_IsException(r)) js_log_exception(ctx, "ui.window.onClose");
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, cb);
            }
            continue;
        }
        if (ev->kind == MKWVK_JS_EV_CLICK || ev->kind == MKWVK_JS_EV_CHECKBOX || ev->kind == MKWVK_JS_EV_SLIDER) {
            JSValue cb = JS_UNDEFINED;
            AcquireSRWLockShared(&g_ui_lock);
            for (int w = 0; w < g_window_count; w++) {
                if (!g_windows[w].used || strcmp(g_windows[w].id, ev->target_id)) continue;
                for (int k = 0; k < g_windows[w].widget_count; k++) {
                    if (!strcmp(g_windows[w].widgets[k].id, ev->widget_id) &&
                        !JS_IsUndefined(g_win_cbs[w][k].on_action)) {
                        cb = JS_DupValue(ctx, g_win_cbs[w][k].on_action);
                        break;
                    }
                }
                break;
            }
            ReleaseSRWLockShared(&g_ui_lock);
            if (!JS_IsUndefined(cb)) {
                JSValue arg = (ev->kind == MKWVK_JS_EV_CHECKBOX) ? JS_NewBool(ctx, ev->bval != 0)
                            : (ev->kind == MKWVK_JS_EV_SLIDER)   ? JS_NewFloat64(ctx, (double)ev->fval)
                                                                 : JS_UNDEFINED;
                JSValue r = JS_Call(ctx, cb, JS_UNDEFINED, ev->kind == MKWVK_JS_EV_CLICK ? 0 : 1, &arg);
                if (JS_IsException(r)) js_log_exception(ctx, "ui.widget");
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, arg);
                JS_FreeValue(ctx, cb);
            }
        }
    }
}

void js_register_ui(JSContext *ctx, JSValue ns_ui) {
    JSValue draw = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, draw, "text", JS_NewCFunction(ctx, js_draw_text, "text", 4));
    JS_SetPropertyStr(ctx, draw, "rect", JS_NewCFunction(ctx, js_draw_rect, "rect", 5));
    JS_SetPropertyStr(ctx, draw, "rectFilled", JS_NewCFunction(ctx, js_draw_rectFilled, "rectFilled", 5));
    JS_SetPropertyStr(ctx, draw, "line", JS_NewCFunction(ctx, js_draw_line, "line", 5));
    JS_SetPropertyStr(ctx, draw, "circle", JS_NewCFunction(ctx, js_draw_circle, "circle", 4));
    JS_SetPropertyStr(ctx, draw, "circleFilled", JS_NewCFunction(ctx, js_draw_circleFilled, "circleFilled", 4));
    JS_SetPropertyStr(ctx, draw, "progressBar", JS_NewCFunction(ctx, js_draw_progress, "progressBar", 6));
    JS_SetPropertyStr(ctx, draw, "clear", JS_NewCFunction(ctx, js_draw_clear, "clear", 0));

    JSValue cons = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cons, "open", JS_NewCFunction(ctx, js_cons_open, "open", 0));
    JS_SetPropertyStr(ctx, cons, "close", JS_NewCFunction(ctx, js_cons_close, "close", 0));
    JS_SetPropertyStr(ctx, cons, "toggle", JS_NewCFunction(ctx, js_cons_toggle, "toggle", 0));
    JS_SetPropertyStr(ctx, cons, "isOpen", JS_NewCFunction(ctx, js_cons_isOpen, "isOpen", 0));
    JS_SetPropertyStr(ctx, cons, "clear", JS_NewCFunction(ctx, js_cons_clear, "clear", 0));

    JS_SetPropertyStr(ctx, ns_ui, "draw", draw);
    JS_SetPropertyStr(ctx, ns_ui, "toast", JS_NewCFunction(ctx, js_ui_toast, "toast", 2));
    JS_SetPropertyStr(ctx, ns_ui, "window", JS_NewCFunction(ctx, js_ui_window, "window", 3));
    JS_SetPropertyStr(ctx, ns_ui, "console", cons);
}

void js_ui_reset(JSContext *ctx) {
    AcquireSRWLockExclusive(&g_ui_lock);
    g_write_n = g_front_n = 0;
    g_write_dirty = 0;
    for (int w = 0; w < g_window_count; w++) {
        for (int i = 0; i < g_windows[w].widget_count; i++) {
            if (ctx && !JS_IsUndefined(g_win_cbs[w][i].on_action))
                JS_FreeValue(ctx, g_win_cbs[w][i].on_action);
            g_win_cbs[w][i].on_action = JS_UNDEFINED;
        }
        if (ctx && !JS_IsUndefined(g_win_on_close[w])) JS_FreeValue(ctx, g_win_on_close[w]);
        g_win_on_close[w] = JS_UNDEFINED;
        memset(&g_windows[w], 0, sizeof g_windows[w]);
    }
    g_window_count = 0;
    g_ev_head = g_ev_tail = 0;
    ReleaseSRWLockExclusive(&g_ui_lock);
}
