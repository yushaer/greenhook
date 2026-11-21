/* Standalone shared JavaScript overlay/UI structs (extracted for ASI plugin build). */
#pragma once

#include <stdint.h>

enum {
    MKWVK_JS_MAX_PRIMS = 1024,
    MKWVK_JS_MAX_TOASTS = 16,
    MKWVK_JS_MAX_WINDOWS = 16,
    MKWVK_JS_MAX_WIDGETS = 128,
    MKWVK_JS_MAX_SCRIPTS = 64,
    MKWVK_JS_MAX_LOGS = 64,
};
enum {
    MKWVK_JS_PRIM_TEXT = 1,
    MKWVK_JS_PRIM_RECT = 2,
    MKWVK_JS_PRIM_RECT_FILLED = 3,
    MKWVK_JS_PRIM_LINE = 4,
    MKWVK_JS_PRIM_CIRCLE = 5,
    MKWVK_JS_PRIM_CIRCLE_FILLED = 6,
    MKWVK_JS_PRIM_PROGRESS = 7,
};
enum {
    MKWVK_JS_WIDGET_LABEL = 1,
    MKWVK_JS_WIDGET_SEPARATOR = 2,
    MKWVK_JS_WIDGET_SAME_LINE = 3,
    MKWVK_JS_WIDGET_BUTTON = 4,
    MKWVK_JS_WIDGET_CHECKBOX = 5,
    MKWVK_JS_WIDGET_SLIDER = 6,
    MKWVK_JS_WIDGET_PROGRESS = 7,
    /* Layout scopes. Each opener is matched by its closer later in the same window. */
    MKWVK_JS_WIDGET_TAB_BAR = 8,
    MKWVK_JS_WIDGET_TAB = 9,
    MKWVK_JS_WIDGET_TAB_END = 10,
    MKWVK_JS_WIDGET_TAB_BAR_END = 11,
    MKWVK_JS_WIDGET_CHILD = 12,       /* fval: height in pixels, 0 fills the remaining space */
    MKWVK_JS_WIDGET_CHILD_END = 13,
};
enum {
    MKWVK_JS_EV_CLICK = 1,
    MKWVK_JS_EV_CHECKBOX = 2,
    MKWVK_JS_EV_SLIDER = 3,
    MKWVK_JS_EV_WINDOW_CLOSE = 4,
    MKWVK_JS_EV_EVAL = 5,
    MKWVK_JS_EV_RELOAD = 6,
    MKWVK_JS_EV_SCRIPT_TOGGLE = 7,
    MKWVK_JS_EV_CONSOLE_CLEAR = 8,
    MKWVK_JS_EV_CONSOLE_STATE = 9,
};
typedef struct MkwVkJsPrim {
    int32_t kind;
    float x, y, w, h;        /* text pos; rect x,y,w,h; line x1,y1,x2,y2; circle cx,cy,r,0 */
    float scale, thickness;  /* font size/scale or stroke thickness; progress fraction in scale */
    uint32_t color;          /* ABGR packed (ImGui IM_COL32 order) */
    uint32_t bg_color;
    char text[160];
} MkwVkJsPrim;
typedef struct MkwVkJsToast {
    char text[192];
    uint32_t color;
    float alpha;
} MkwVkJsToast;
typedef struct MkwVkJsWidget {
    int32_t kind;
    char id[48];
    char label[128];
    float fval, fmin, fmax;
    int32_t bval;
    uint32_t color;
} MkwVkJsWidget;
typedef struct MkwVkJsWindow {
    int32_t used, open;
    char id[48];
    char title[96];
    float x, y, w, h;
    int32_t widget_count;
    MkwVkJsWidget widgets[MKWVK_JS_MAX_WIDGETS];
} MkwVkJsWindow;
typedef struct MkwVkJsScript {
    char id[64];
    char name[64];
    char version[24];
    char author[48];
    char error[160];
    int32_t enabled, loaded;
} MkwVkJsScript;
typedef struct MkwVkJsLogLine {
    int32_t seq, level;      /* 0 log, 1 info, 2 warn, 3 error */
    char source[48];
    char text[224];
} MkwVkJsLogLine;
typedef struct MkwVkJsSnapshot {
    uint32_t struct_size;
    int32_t prim_count;
    MkwVkJsPrim prims[MKWVK_JS_MAX_PRIMS];
    int32_t toast_count;
    MkwVkJsToast toasts[MKWVK_JS_MAX_TOASTS];
    int32_t window_count;
    MkwVkJsWindow windows[MKWVK_JS_MAX_WINDOWS];
    int32_t script_count;
    MkwVkJsScript scripts[MKWVK_JS_MAX_SCRIPTS];
    int32_t log_count;
    MkwVkJsLogLine logs[MKWVK_JS_MAX_LOGS];
    int32_t console_open_req; /* -1 unchanged, 0 force close, 1 force open */
    int32_t runtime_ready;
    char scripts_dir[260];
} MkwVkJsSnapshot;
typedef struct MkwVkJsEvent {
    int32_t kind;
    char target_id[64];      /* window_id */
    char widget_id[64];      /* widget_id or script_id */
    float fval;
    int32_t bval;
    char text[1024];
} MkwVkJsEvent;
