// (longshot-js) Dear ImGui overlay, HUD primitives, windows, F9 Console & Script Manager
// hooked directly into untouched stock longshot_vk.dll via COFF symbol resolution + MinHook.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "../gpu/vk/vk_api.h"
#include "../gpu/vk/vk_ui_fonts.h"
#include "../gpu/vk/vk_menu_hit.h"
#include "js_ui_types.h"
#include "../host/js_api.h"
#include "../third_party/minhook/MinHook.h"
#include "coff_sym.h"
#include "hooks_vk.h"
#include "host_bridge.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef struct VkImage_T* VkImage;
typedef struct VkImageView_T* VkImageView;

namespace {

constexpr uint16_t kBoundDirect = 0x30;   // mkwvk::rec::B_DIRECT verified from stock longshot_vk.dll
constexpr size_t kMaxHistory = 64;
constexpr size_t kCmdCap = sizeof(MkwVkJsEvent{}.text);

// Longshot warm palette (sRGB IM_COL32; vk_imgui.cpp handles sRGB/HDR10 conversion automatically)
constexpr ImU32 rgba32(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImU32 scale_alpha(ImU32 c, float k) {
    const int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(k, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp(a, 0, 255) << IM_COL32_A_SHIFT);
}
constexpr ImU32 kInk = rgba32(240, 232, 214);
constexpr ImU32 kInkBright = rgba32(255, 250, 240);
constexpr ImU32 kDim = rgba32(168, 156, 136);
constexpr ImU32 kGold = rgba32(222, 152, 40);
constexpr ImU32 kEdge = rgba32(122, 98, 64, 150);
constexpr ImU32 kBoxEdge = rgba32(122, 98, 64, 220);

struct VkSyms {
    void (*fn_menu_tick)();
    void (*fn_menu_record)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    void (*fn_menu_record_image)(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t);
    bool (*menu_is_open)();
    int64_t (*prof_now)();
    const MkwVkHost** p_host;
    void** p_editing;
    void** p_GImGui;

    bool (*fn_ui_begin)(uint32_t, uint32_t);
    const mkwvk::ui::Fonts& (*ui_fonts)();
    void (*fn_ui_record)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    void (*fn_ui_record_to)(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t);
    void (*rec_boundary)(uint16_t);

    ImGuiIO& (*ig_GetIO)();
    bool (*ig_Begin)(const char*, bool*, ImGuiWindowFlags);
    void (*ig_End)();
    bool (*ig_BeginChild)(const char*, const ImVec2&, ImGuiChildFlags, ImGuiWindowFlags);
    void (*ig_EndChild)();
    bool (*ig_BeginTabBar)(const char*, ImGuiTabBarFlags);
    void (*ig_EndTabBar)();
    bool (*ig_BeginTabItem)(const char*, bool*, ImGuiTabItemFlags);
    void (*ig_EndTabItem)();
    bool (*ig_BeginTable)(const char*, int, ImGuiTableFlags, const ImVec2&, float);
    void (*ig_EndTable)();
    void (*ig_TableSetupColumn)(const char*, ImGuiTableColumnFlags, float, ImGuiID);
    void (*ig_TableHeadersRow)();
    void (*ig_TableNextRow)(ImGuiTableRowFlags, float);
    bool (*ig_TableSetColumnIndex)(int);
    bool (*ig_Button)(const char*, const ImVec2&);
    bool (*ig_InvisibleButton)(const char*, const ImVec2&, ImGuiButtonFlags);
    bool (*ig_IsItemClicked)(ImGuiMouseButton);
    bool (*ig_Checkbox)(const char*, bool*);
    bool (*ig_SliderFloat)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
    void (*ig_ProgressBar)(float, const ImVec2&, const char*);
    void (*ig_Text)(const char*, ...);
    void (*ig_TextWrapped)(const char*, ...);
    void (*ig_TextUnformatted)(const char*, const char*);
    void (*ig_TextDisabled)(const char*, ...);
    void (*ig_Separator)();
    void (*ig_SameLine)(float, float);
    void (*ig_PushID_i)(int);
    void (*ig_PopID)();
    void (*ig_PushStyleColor_v)(ImGuiCol, const ImVec4&);
    void (*ig_PopStyleColor)(int);
    void (*ig_PushStyleVar_f)(ImGuiStyleVar, float);
    void (*ig_PushStyleVar_v)(ImGuiStyleVar, const ImVec2&);
    void (*ig_PopStyleVar)(int);
    void (*ig_PushFont)(ImFont*, float);
    void (*ig_PopFont)();
    void (*ig_SetNextWindowPos)(const ImVec2&, ImGuiCond, const ImVec2&);
    void (*ig_SetNextWindowSize)(const ImVec2&, ImGuiCond);
    ImFont* (*ig_GetFont)();
    float (*ig_GetFontSize)();
    float (*ig_GetFrameHeight)();
    ImVec2 (*ig_GetContentRegionAvail)();
    ImVec2 (*ig_GetCursorScreenPos)();
    ImDrawList* (*ig_GetWindowDrawList)();
    ImDrawList* (*ig_GetBackgroundDrawList)();
    ImDrawList* (*ig_GetForegroundDrawList)();
    void (*ig_SetScrollHereY)(float);
    ImVec4 (*ig_ColorConvertU32ToFloat4)(ImU32);

    void (*dl_AddText)(ImDrawList*, const ImFont*, float, const ImVec2&, ImU32, const char*, const char*, float, const ImVec4*);
    void (*dl_AddRect)(ImDrawList*, const ImVec2&, const ImVec2&, ImU32, float, ImDrawFlags, float);
    void (*dl_AddRectFilled)(ImDrawList*, const ImVec2&, const ImVec2&, ImU32, float, ImDrawFlags);
    void (*dl_AddLine)(ImDrawList*, const ImVec2&, const ImVec2&, ImU32, float);
    void (*dl_AddCircle)(ImDrawList*, const ImVec2&, float, ImU32, int, float);
    void (*dl_AddCircleFilled)(ImDrawList*, const ImVec2&, float, ImU32, int);
    void (*dl_PushClipRect)(ImDrawList*, const ImVec2&, const ImVec2&, bool);
    void (*dl_PopClipRect)(ImDrawList*);

    ImVec2 (*font_CalcTextSizeA)(ImFont*, float, float, float, const char*, const char*, const char**);
    void (*io_AddMousePosEvent)(ImGuiIO*, float, float);
    void (*io_AddMouseButtonEvent)(ImGuiIO*, int, bool);
    void (*io_AddMouseWheelEvent)(ImGuiIO*, float, float);
};

VkSyms g_vk{};
bool g_vk_ready = false;

void (*orig_menu_tick)() = nullptr;
void (*orig_menu_record)(VkCommandBuffer, uint32_t, uint32_t, uint32_t) = nullptr;
void (*orig_menu_record_image)(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t) = nullptr;
bool (*orig_ui_begin)(uint32_t, uint32_t) = nullptr;
void (*orig_ui_record)(VkCommandBuffer, uint32_t, uint32_t, uint32_t) = nullptr;
void (*orig_ui_record_to)(VkCommandBuffer, VkImage, VkImageView, uint32_t, uint32_t) = nullptr;
int (*orig_host_take_text)(int, uint32_t*, int) = nullptr;
int (*orig_host_take_key_press)(int) = nullptr;

MkwVkJsSnapshot g_snap{};
bool g_snap_valid = false;
bool g_console_open = false;
bool g_console_focus = true;
bool g_auto_scroll = true;
int g_last_log_seq = 0;

char g_cmd[kCmdCap]{};
int g_cmd_len = 0;
int g_cmd_caret = 0;
std::vector<std::string> g_history;
int g_hist_idx = -1;

MkwVkPointer g_ptr{};
bool g_have_ptr = false;
bool g_had_ptr = false;
uint32_t g_prev_held = 0;

thread_local bool g_in_menu_tick = false;
thread_local bool g_drew_this_frame = false;

void send_event(int32_t kind, const char* target = nullptr, const char* widget = nullptr,
                float fval = 0.0f, int32_t bval = 0, const char* text = nullptr) {
    MkwVkJsEvent ev{};
    ev.kind = kind;
    if (target) snprintf(ev.target_id, sizeof ev.target_id, "%s", target);
    if (widget) snprintf(ev.widget_id, sizeof ev.widget_id, "%s", widget);
    ev.fval = fval;
    ev.bval = bval;
    if (text) snprintf(ev.text, sizeof ev.text, "%s", text);
    hos_js_ui_event_vk(&ev);
}

void push_history(const char* line) {
    if (!line || !*line) return;
    if (g_history.empty() || g_history.back() != line) {
        if (g_history.size() >= kMaxHistory) g_history.erase(g_history.begin());
        g_history.emplace_back(line);
    }
    g_hist_idx = -1;
}

void set_cmd(const char* s) {
    g_cmd_len = s ? snprintf(g_cmd, sizeof g_cmd, "%s", s) : 0;
    if (g_cmd_len < 0) g_cmd_len = 0;
    if (g_cmd_len >= (int)sizeof g_cmd) g_cmd_len = (int)sizeof g_cmd - 1;
    g_cmd[g_cmd_len] = 0;
    g_cmd_caret = g_cmd_len;
}

int utf8_prev(const char* s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

int utf8_next(const char* s, int len, int pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

int encode_utf8(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

void submit_console_cmd() {
    if (g_cmd_len <= 0) return;
    push_history(g_cmd);
    send_event(MKWVK_JS_EV_EVAL, nullptr, nullptr, 0.0f, 0, g_cmd);
    set_cmd("");
    g_auto_scroll = true;
}

void handle_console_input(uint32_t in) {
    if (in & HOS_TEXT_KEY) {
        const uint32_t vk = in & ~(uint32_t)HOS_TEXT_KEY;
        switch (vk) {
        case VK_RETURN: submit_console_cmd(); break;
        case VK_ESCAPE:
            if (g_cmd_len > 0) set_cmd("");
            else {
                g_console_open = false;
                send_event(MKWVK_JS_EV_CONSOLE_STATE, nullptr, nullptr, 0.0f, 0);
            }
            break;
        case VK_LEFT: g_cmd_caret = utf8_prev(g_cmd, g_cmd_caret); break;
        case VK_RIGHT: g_cmd_caret = utf8_next(g_cmd, g_cmd_len, g_cmd_caret); break;
        case VK_HOME: g_cmd_caret = 0; break;
        case VK_END: g_cmd_caret = g_cmd_len; break;
        case VK_BACK:
            if (g_cmd_caret > 0) {
                const int p = utf8_prev(g_cmd, g_cmd_caret);
                memmove(g_cmd + p, g_cmd + g_cmd_caret, (size_t)(g_cmd_len - g_cmd_caret + 1));
                g_cmd_len -= (g_cmd_caret - p);
                g_cmd_caret = p;
            }
            break;
        case VK_DELETE:
            if (g_cmd_caret < g_cmd_len) {
                const int n = utf8_next(g_cmd, g_cmd_len, g_cmd_caret);
                memmove(g_cmd + g_cmd_caret, g_cmd + n, (size_t)(g_cmd_len - n + 1));
                g_cmd_len -= (n - g_cmd_caret);
            }
            break;
        case VK_UP:
            if (!g_history.empty()) {
                g_hist_idx = g_hist_idx < 0 ? (int)g_history.size() - 1 : std::max(0, g_hist_idx - 1);
                set_cmd(g_history[(size_t)g_hist_idx].c_str());
            }
            break;
        case VK_DOWN:
            if (!g_history.empty() && g_hist_idx >= 0) {
                g_hist_idx++;
                if (g_hist_idx >= (int)g_history.size()) {
                    g_hist_idx = -1;
                    set_cmd("");
                } else {
                    set_cmd(g_history[(size_t)g_hist_idx].c_str());
                }
            }
            break;
        default: break;
        }
        return;
    }
    if (in < 0x20 || in == 0x7F) return;
    char utf8[4];
    const int nb = encode_utf8(in, utf8);
    if (nb <= 0 || g_cmd_len + nb >= (int)sizeof g_cmd) return;
    memmove(g_cmd + g_cmd_caret + nb, g_cmd + g_cmd_caret, (size_t)(g_cmd_len - g_cmd_caret + 1));
    memcpy(g_cmd + g_cmd_caret, utf8, (size_t)nb);
    g_cmd_len += nb;
    g_cmd_caret += nb;
}

bool has_open_js_window() {
    if (!g_snap_valid) return false;
    for (int i = 0; i < g_snap.window_count; i++)
        if (g_snap.windows[i].used && g_snap.windows[i].open) return true;
    return false;
}

bool js_overlay_active() {
    if (!g_vk_ready) return false;
    if (g_console_open) return true;
    if (!g_snap_valid) return false;
    return g_snap.prim_count > 0 || g_snap.toast_count > 0 || has_open_js_window();
}

bool js_overlay_wants_text() { return g_console_open && g_console_focus; }

int hk_host_take_text(int focus, uint32_t* out, int max) {
    const bool editing_menu = g_vk.p_editing && (*g_vk.p_editing != nullptr);
    if (g_in_menu_tick && focus == HOS_TEXT_OFF && !editing_menu && js_overlay_wants_text()) {
        return 0;
    }
    return orig_host_take_text(focus, out, max);
}

int hk_host_take_key_press(int vk) {
    if (g_in_menu_tick && vk == VK_ESCAPE && g_vk.menu_is_open && !g_vk.menu_is_open() && js_overlay_wants_text()) {
        return 0;
    }
    return orig_host_take_key_press(vk);
}

void ensure_host_callbacks_hooked(const MkwVkHost* host) {
    static bool done = false;
    if (done || !host) return;
    done = true;
    if (host->take_text) {
        if (MH_CreateHook((void*)host->take_text, (void*)hk_host_take_text, (void**)&orig_host_take_text) == MH_OK) {
            MH_EnableHook((void*)host->take_text);
        }
    }
    if (host->take_key_press) {
        if (MH_CreateHook((void*)host->take_key_press, (void*)hk_host_take_key_press, (void**)&orig_host_take_key_press) == MH_OK) {
            MH_EnableHook((void*)host->take_key_press);
        }
    }
}

void js_overlay_tick(const MkwVkHost* host, bool main_menu_open) {
    ensure_host_callbacks_hooked(host);

    if (!main_menu_open && host && host->take_key_press) {
        auto key_pressed = [&](int vk) -> int {
            return orig_host_take_key_press ? orig_host_take_key_press(vk) : host->take_key_press(vk);
        };
        if (key_pressed(VK_F9)) {
            g_console_open = !g_console_open;
            if (g_console_open) g_console_focus = true;
            send_event(MKWVK_JS_EV_CONSOLE_STATE, nullptr, nullptr, 0.0f, g_console_open ? 1 : 0);
        }
        if (key_pressed(VK_F8)) {
            send_event(MKWVK_JS_EV_RELOAD);
        }
    }

    g_snap.struct_size = sizeof g_snap;
    g_snap_valid = hos_js_ui_snapshot_vk(&g_snap) != 0;

    if (g_snap_valid && g_snap.console_open_req >= 0) {
        const bool req = g_snap.console_open_req != 0;
        if (req != g_console_open) {
            g_console_open = req;
            if (req) g_console_focus = true;
            send_event(MKWVK_JS_EV_CONSOLE_STATE, nullptr, nullptr, 0.0f, req ? 1 : 0);
        }
    }

    const bool want_mouse = !main_menu_open && (g_console_open || has_open_js_window());
    hos_display_js_want_mouse(want_mouse ? 1 : 0);

    if (want_mouse && host && host->pointer) g_have_ptr = host->pointer(&g_ptr) != 0;
    else {
        g_have_ptr = false;
        g_ptr = {};
    }

    if (!main_menu_open && g_console_open && g_console_focus && host && host->take_text) {
        uint32_t typed[64];
        const int n = orig_host_take_text ? orig_host_take_text(HOS_TEXT_ON, typed, 64)
                                          : host->take_text(HOS_TEXT_ON, typed, 64);
        for (int i = 0; i < n; i++) handle_console_input(typed[i]);
    }
}

void js_overlay_before_begin(uint32_t w, uint32_t h) {
    if (!g_vk.p_GImGui || !*g_vk.p_GImGui) return;
    ImGuiIO& io = g_vk.ig_GetIO();
    io.MouseDrawCursor = false;
    /* WantCaptureMouse is last frame's answer: the cursor is over a window or a widget holds it. */
    hos_js_set_overlay_state((int)w, (int)h, g_have_ptr && io.WantCaptureMouse);
    if (g_have_ptr) {
        g_had_ptr = true;
        int ix = 0, iy = 0;
        mkwvk::menu_hit::to_image(g_ptr.x, g_ptr.y, g_ptr.width, g_ptr.height, (int)w, (int)h, &ix, &iy);
        g_vk.io_AddMousePosEvent(&io, (float)ix, (float)iy);
        const uint32_t masks[3] = {HOS_MOUSE_LEFT, HOS_MOUSE_RIGHT, HOS_MOUSE_MIDDLE};
        for (int b = 0; b < 3; b++) {
            const bool down = (g_ptr.held & masks[b]) != 0;
            const bool prev = (g_prev_held & masks[b]) != 0;
            if (!down && !prev && (g_ptr.pressed & masks[b])) {
                g_vk.io_AddMouseButtonEvent(&io, b, true);
                g_vk.io_AddMouseButtonEvent(&io, b, false);
            } else if (down != prev) {
                g_vk.io_AddMouseButtonEvent(&io, b, down);
            }
        }
        g_prev_held = g_ptr.held;
        if (g_ptr.wheel != 0) g_vk.io_AddMouseWheelEvent(&io, 0.0f, (float)g_ptr.wheel);
    } else if (g_had_ptr || g_prev_held) {
        g_had_ptr = false;
        for (int b = 0; b < 3; b++)
            if (g_prev_held & (1u << b)) g_vk.io_AddMouseButtonEvent(&io, b, false);
        g_prev_held = 0;
        g_vk.io_AddMousePosEvent(&io, -FLT_MAX, -FLT_MAX);
    }
}

/* Index of the closer matching the opener at `start`, or the last widget when it is missing. */
static int matching_close(const MkwVkJsWindow& win, int start, int open_kind, int close_kind) {
    int depth = 0;
    for (int j = start + 1; j < win.widget_count; j++) {
        if (win.widgets[j].kind == open_kind) depth++;
        else if (win.widgets[j].kind == close_kind && depth-- == 0) return j;
    }
    return win.widget_count - 1;
}

/* Handles a layout scope widget. Returns false when `wg` is not a scope. Scopes that are open are pushed
   on `stack` so their closer (or the end of the window) ends them; hidden contents are skipped. */
static bool draw_scope(const MkwVkJsWindow& win, int& i, int* stack, int& sp, int cap) {
    const MkwVkJsWidget& wg = win.widgets[i];
    const int top = sp > 0 ? stack[sp - 1] : 0;
    switch (wg.kind) {
    case MKWVK_JS_WIDGET_TAB_BAR:
        if (sp < cap && g_vk.ig_BeginTabBar(wg.id[0] ? wg.id : "##tabs", ImGuiTabBarFlags_FittingPolicyScroll))
            stack[sp++] = MKWVK_JS_WIDGET_TAB_BAR;
        else i = matching_close(win, i, MKWVK_JS_WIDGET_TAB_BAR, MKWVK_JS_WIDGET_TAB_BAR_END);
        return true;
    case MKWVK_JS_WIDGET_TAB: {
        char name[192];
        snprintf(name, sizeof name, "%s###%s", wg.label, wg.id);
        if (top == MKWVK_JS_WIDGET_TAB_BAR && sp < cap && g_vk.ig_BeginTabItem(name, nullptr, 0))
            stack[sp++] = MKWVK_JS_WIDGET_TAB;
        else i = matching_close(win, i, MKWVK_JS_WIDGET_TAB, MKWVK_JS_WIDGET_TAB_END);
        return true;
    }
    case MKWVK_JS_WIDGET_TAB_END:
        if (top == MKWVK_JS_WIDGET_TAB) { g_vk.ig_EndTabItem(); sp--; }
        return true;
    case MKWVK_JS_WIDGET_TAB_BAR_END:
        if (top == MKWVK_JS_WIDGET_TAB_BAR) { g_vk.ig_EndTabBar(); sp--; }
        return true;
    case MKWVK_JS_WIDGET_CHILD:
        if (sp >= cap) { i = matching_close(win, i, MKWVK_JS_WIDGET_CHILD, MKWVK_JS_WIDGET_CHILD_END); return true; }
        /* EndChild is required even when BeginChild reports the region as clipped. */
        stack[sp++] = MKWVK_JS_WIDGET_CHILD;
        if (!g_vk.ig_BeginChild(wg.id[0] ? wg.id : "##child", ImVec2(0.0f, wg.fval), ImGuiChildFlags_Borders, 0))
            i = matching_close(win, i, MKWVK_JS_WIDGET_CHILD, MKWVK_JS_WIDGET_CHILD_END) - 1;
        return true;
    case MKWVK_JS_WIDGET_CHILD_END:
        if (top == MKWVK_JS_WIDGET_CHILD) { g_vk.ig_EndChild(); sp--; }
        return true;
    default:
        return false;
    }
}

static void close_scopes(int* stack, int& sp) {
    while (sp > 0) {
        switch (stack[--sp]) {
        case MKWVK_JS_WIDGET_TAB: g_vk.ig_EndTabItem(); break;
        case MKWVK_JS_WIDGET_TAB_BAR: g_vk.ig_EndTabBar(); break;
        case MKWVK_JS_WIDGET_CHILD: g_vk.ig_EndChild(); break;
        default: break;
        }
    }
}

void draw_hud_prims(ImDrawList* dl, ImFont* font) {
    if (!g_snap_valid || g_snap.prim_count <= 0 || !dl) return;
    ImFont* f = font ? font : g_vk.ig_GetFont();
    for (int i = 0; i < g_snap.prim_count; i++) {
        const MkwVkJsPrim& p = g_snap.prims[i];
        const ImU32 col = p.color ? (ImU32)p.color : kInk;
        switch (p.kind) {
        case MKWVK_JS_PRIM_TEXT: {
            float px = p.scale > 0.0f ? (p.scale <= 4.0f ? p.scale * 18.0f : p.scale) : 18.0f;
            px = std::clamp(px, 10.0f, 96.0f);
            const ImVec2 sz = g_vk.font_CalcTextSizeA(f, px, FLT_MAX, 0.0f, p.text, nullptr, nullptr);
            if (p.bg_color) {
                g_vk.dl_AddRectFilled(dl, ImVec2(p.x - 6.0f, p.y - 3.0f), ImVec2(p.x + sz.x + 6.0f, p.y + sz.y + 3.0f),
                                      (ImU32)p.bg_color, 4.0f, 0);
            }
            g_vk.dl_AddText(dl, f, px, ImVec2(p.x + 1.0f, p.y + 1.0f),
                            IM_COL32(0, 0, 0, ((col >> 24) & 0xFF) * 3 / 4), p.text, nullptr, 0.0f, nullptr);
            g_vk.dl_AddText(dl, f, px, ImVec2(p.x, p.y), col, p.text, nullptr, 0.0f, nullptr);
            break;
        }
        case MKWVK_JS_PRIM_RECT:
            g_vk.dl_AddRect(dl, ImVec2(p.x, p.y), ImVec2(p.x + p.w, p.y + p.h), col, std::max(0.0f, p.scale), 0,
                            p.thickness > 0.0f ? p.thickness : 1.5f);
            break;
        case MKWVK_JS_PRIM_RECT_FILLED:
            g_vk.dl_AddRectFilled(dl, ImVec2(p.x, p.y), ImVec2(p.x + p.w, p.y + p.h), col, std::max(0.0f, p.scale), 0);
            break;
        case MKWVK_JS_PRIM_LINE:
            g_vk.dl_AddLine(dl, ImVec2(p.x, p.y), ImVec2(p.w, p.h), col, p.thickness > 0.0f ? p.thickness : 1.5f);
            break;
        case MKWVK_JS_PRIM_CIRCLE:
            g_vk.dl_AddCircle(dl, ImVec2(p.x, p.y), std::max(0.5f, p.w), col, 0, p.thickness > 0.0f ? p.thickness : 1.5f);
            break;
        case MKWVK_JS_PRIM_CIRCLE_FILLED:
            g_vk.dl_AddCircleFilled(dl, ImVec2(p.x, p.y), std::max(0.5f, p.w), col, 0);
            break;
        case MKWVK_JS_PRIM_PROGRESS: {
            const float frac = std::clamp(p.scale, 0.0f, 1.0f);
            const ImU32 bg = p.bg_color ? (ImU32)p.bg_color : IM_COL32(20, 24, 30, 190);
            const float round = std::min(4.0f, p.h * 0.4f);
            g_vk.dl_AddRectFilled(dl, ImVec2(p.x, p.y), ImVec2(p.x + p.w, p.y + p.h), bg, round, 0);
            if (frac > 0.001f)
                g_vk.dl_AddRectFilled(dl, ImVec2(p.x, p.y), ImVec2(p.x + p.w * frac, p.y + p.h), col, round, 0);
            g_vk.dl_AddRect(dl, ImVec2(p.x, p.y), ImVec2(p.x + p.w, p.y + p.h), kEdge, round, 0, 1.0f);
            if (p.text[0]) {
                const float px = std::clamp(p.h * 0.72f, 11.0f, 22.0f);
                const ImVec2 sz = g_vk.font_CalcTextSizeA(f, px, FLT_MAX, 0.0f, p.text, nullptr, nullptr);
                const ImVec2 tp(p.x + (p.w - sz.x) * 0.5f, p.y + (p.h - sz.y) * 0.5f);
                g_vk.dl_AddText(dl, f, px, ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 190),
                                p.text, nullptr, 0.0f, nullptr);
                g_vk.dl_AddText(dl, f, px, tp, kInkBright, p.text, nullptr, 0.0f, nullptr);
            }
            break;
        }
        default: break;
        }
    }
}

void draw_toasts(ImDrawList* dl, ImFont* font, uint32_t w, uint32_t h) {
    if (!g_snap_valid || g_snap.toast_count <= 0 || !dl) return;
    ImFont* f = font ? font : g_vk.ig_GetFont();
    const float px = std::clamp(h * 0.023f, 15.0f, 22.0f);
    const float margin = std::clamp(w * 0.018f, 14.0f, 36.0f);
    const float pad_x = 14.0f, pad_y = 9.0f;
    const float max_text_w = std::clamp(w * 0.34f, 240.0f, 520.0f);
    float y = margin;

    for (int i = 0; i < g_snap.toast_count; i++) {
        const MkwVkJsToast& t = g_snap.toasts[i];
        const float a = std::clamp(t.alpha, 0.0f, 1.0f);
        if (a <= 0.01f) continue;
        const ImVec2 sz = g_vk.font_CalcTextSizeA(f, px, FLT_MAX, max_text_w, t.text, nullptr, nullptr);
        const float bw = sz.x + pad_x * 2.0f + 6.0f;
        const float bh = sz.y + pad_y * 2.0f;
        const float bx = (float)w - margin - bw;
        const ImU32 accent = t.color ? scale_alpha((ImU32)t.color, a) : scale_alpha(kGold, a);
        g_vk.dl_AddRectFilled(dl, ImVec2(bx, y), ImVec2(bx + bw, y + bh), scale_alpha(rgba32(24, 20, 15, 232), a), 6.0f, 0);
        g_vk.dl_AddRectFilled(dl, ImVec2(bx, y), ImVec2(bx + 4.5f, y + bh), accent, 6.0f, ImDrawFlags_RoundCornersLeft);
        g_vk.dl_AddRect(dl, ImVec2(bx, y), ImVec2(bx + bw, y + bh), scale_alpha(kBoxEdge, a), 6.0f, 0, 1.0f);
        g_vk.dl_AddText(dl, f, px, ImVec2(bx + pad_x + 4.0f, y + pad_y), scale_alpha(kInkBright, a),
                        t.text, nullptr, max_text_w, nullptr);
        y += bh + 8.0f;
    }
}

void push_panel_style() {
    g_vk.ig_PushStyleVar_f(ImGuiStyleVar_WindowRounding, 6.0f);
    g_vk.ig_PushStyleVar_f(ImGuiStyleVar_FrameRounding, 4.0f);
    g_vk.ig_PushStyleVar_f(ImGuiStyleVar_WindowBorderSize, 1.0f);
    g_vk.ig_PushStyleVar_v(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    g_vk.ig_PushStyleVar_v(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_WindowBg, ImVec4(0.09f, 0.08f, 0.06f, 0.94f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_TitleBg, ImVec4(0.16f, 0.12f, 0.07f, 0.98f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_TitleBgActive, ImVec4(0.24f, 0.18f, 0.10f, 0.98f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_Border, ImVec4(0.48f, 0.38f, 0.25f, 0.85f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_FrameBg, ImVec4(0.17f, 0.14f, 0.10f, 0.95f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_FrameBgHovered, ImVec4(0.26f, 0.20f, 0.14f, 0.98f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_FrameBgActive, ImVec4(0.32f, 0.24f, 0.15f, 1.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_Button, ImVec4(0.28f, 0.21f, 0.12f, 0.95f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.31f, 0.16f, 0.98f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_ButtonActive, ImVec4(0.87f, 0.60f, 0.16f, 1.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_SliderGrab, ImVec4(0.87f, 0.60f, 0.16f, 0.95f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_SliderGrabActive, ImVec4(0.95f, 0.70f, 0.24f, 1.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_CheckMark, ImVec4(0.95f, 0.70f, 0.24f, 1.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_Tab, ImVec4(0.18f, 0.14f, 0.09f, 0.95f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_TabHovered, ImVec4(0.38f, 0.28f, 0.16f, 0.98f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_TabSelected, ImVec4(0.55f, 0.39f, 0.16f, 1.0f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_PlotHistogram, ImVec4(0.87f, 0.60f, 0.16f, 0.95f));
    g_vk.ig_PushStyleColor_v(ImGuiCol_Text, ImVec4(0.95f, 0.92f, 0.85f, 1.0f));
}

void pop_panel_style() {
    g_vk.ig_PopStyleColor(18);
    g_vk.ig_PopStyleVar(5);
}

void draw_js_windows() {
    if (!g_snap_valid || g_snap.window_count <= 0) return;
    for (int w = 0; w < g_snap.window_count; w++) {
        MkwVkJsWindow& win = g_snap.windows[w];
        if (!win.used || !win.open) continue;
        char wname[160];
        snprintf(wname, sizeof wname, "%s###jswin_%s", win.title[0] ? win.title : win.id, win.id);
        g_vk.ig_SetNextWindowPos(ImVec2(win.x, win.y), ImGuiCond_FirstUseEver, ImVec2(0.0f, 0.0f));
        if (win.w > 40.0f || win.h > 40.0f) {
            g_vk.ig_SetNextWindowSize(ImVec2(win.w > 40.0f ? win.w : 340.0f, win.h > 40.0f ? win.h : 0.0f),
                                      ImGuiCond_FirstUseEver);
        }
        const ImGuiWindowFlags flags = win.h <= 0.0f ? ImGuiWindowFlags_AlwaysAutoResize : 0;
        bool open = true;
        if (g_vk.ig_Begin(wname, &open, flags)) {
            int scopes[16], scope_n = 0;
            for (int i = 0; i < win.widget_count; i++) {
                if (draw_scope(win, i, scopes, scope_n, 16)) continue;
                MkwVkJsWidget& wg = win.widgets[i];
                g_vk.ig_PushID_i(i);
                switch (wg.kind) {
                case MKWVK_JS_WIDGET_LABEL:
                    if (wg.color) {
                        g_vk.ig_PushStyleColor_v(ImGuiCol_Text, g_vk.ig_ColorConvertU32ToFloat4((ImU32)wg.color));
                        g_vk.ig_TextWrapped("%s", wg.label);
                        g_vk.ig_PopStyleColor(1);
                    } else {
                        g_vk.ig_TextWrapped("%s", wg.label);
                    }
                    break;
                case MKWVK_JS_WIDGET_SEPARATOR: g_vk.ig_Separator(); break;
                case MKWVK_JS_WIDGET_SAME_LINE: g_vk.ig_SameLine(0.0f, -1.0f); break;
                case MKWVK_JS_WIDGET_BUTTON:
                    if (g_vk.ig_Button(wg.label, ImVec2(0.0f, 0.0f)))
                        send_event(MKWVK_JS_EV_CLICK, win.id, wg.id);
                    break;
                case MKWVK_JS_WIDGET_CHECKBOX: {
                    bool v = wg.bval != 0;
                    if (g_vk.ig_Checkbox(wg.label, &v)) {
                        wg.bval = v ? 1 : 0;
                        send_event(MKWVK_JS_EV_CHECKBOX, win.id, wg.id, 0.0f, wg.bval);
                    }
                    break;
                }
                case MKWVK_JS_WIDGET_SLIDER: {
                    float v = wg.fval;
                    if (g_vk.ig_SliderFloat(wg.label, &v, wg.fmin, wg.fmax, "%.3g", 0)) {
                        wg.fval = v;
                        send_event(MKWVK_JS_EV_SLIDER, win.id, wg.id, v);
                    }
                    break;
                }
                case MKWVK_JS_WIDGET_PROGRESS:
                    g_vk.ig_ProgressBar(std::clamp(wg.fval, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f),
                                        wg.label[0] ? wg.label : nullptr);
                    break;
                default: break;
                }
                g_vk.ig_PopID();
            }
            close_scopes(scopes, scope_n);
        }
        g_vk.ig_End();
        if (!open) {
            win.open = 0;
            send_event(MKWVK_JS_EV_WINDOW_CLOSE, win.id);
        }
    }
}

void draw_console(ImFont* font, uint32_t w, uint32_t h) {
    if (!g_console_open) return;
    const float cw = std::clamp((float)w * 0.62f, 520.0f, 860.0f);
    const float ch = std::clamp((float)h * 0.62f, 360.0f, 580.0f);
    g_vk.ig_SetNextWindowPos(ImVec2(((float)w - cw) * 0.5f, ((float)h - ch) * 0.5f),
                             ImGuiCond_FirstUseEver, ImVec2(0.0f, 0.0f));
    g_vk.ig_SetNextWindowSize(ImVec2(cw, ch), ImGuiCond_FirstUseEver);

    bool open = true;
    if (g_vk.ig_Begin("Longshot JS Console & Script Manager (F9 toggle, F8 reload)###longshot_js_console", &open, 0)) {
        if (g_vk.ig_Button("Reload All (F8)", ImVec2(0.0f, 0.0f))) send_event(MKWVK_JS_EV_RELOAD);
        g_vk.ig_SameLine(0.0f, -1.0f);
        if (g_vk.ig_Button("Clear Logs", ImVec2(0.0f, 0.0f))) send_event(MKWVK_JS_EV_CONSOLE_CLEAR);
        g_vk.ig_SameLine(0.0f, -1.0f);
        g_vk.ig_Checkbox("Auto-scroll", &g_auto_scroll);
        g_vk.ig_SameLine(0.0f, -1.0f);
        g_vk.ig_TextDisabled("| Runtime: %s | Scripts: %d",
                             g_snap.runtime_ready ? "Ready" : "Starting", g_snap.script_count);
        if (g_snap.scripts_dir[0]) g_vk.ig_TextDisabled("Folder: %s", g_snap.scripts_dir);
        g_vk.ig_Separator();

        if (g_vk.ig_BeginTabBar("js_console_tabs", 0)) {
            if (g_vk.ig_BeginTabItem("Console / REPL", nullptr, 0)) {
                const float input_h = g_vk.ig_GetFrameHeight() + 10.0f;
                if (g_vk.ig_BeginChild("js_log_scroll", ImVec2(0.0f, -input_h), ImGuiChildFlags_Borders,
                                       ImGuiWindowFlags_HorizontalScrollbar)) {
                    int latest = g_last_log_seq;
                    for (int i = 0; i < g_snap.log_count; i++) {
                        const MkwVkJsLogLine& l = g_snap.logs[i];
                        if (l.seq > latest) latest = l.seq;
                        ImVec4 col(0.92f, 0.90f, 0.84f, 1.0f);
                        if (l.level == 3) col = ImVec4(1.0f, 0.42f, 0.42f, 1.0f);
                        else if (l.level == 2) col = ImVec4(1.0f, 0.78f, 0.30f, 1.0f);
                        else if (!strcmp(l.source, "=>")) col = ImVec4(0.95f, 0.76f, 0.30f, 1.0f);
                        else if (l.level == 1) col = ImVec4(0.55f, 0.88f, 0.96f, 1.0f);
                        g_vk.ig_PushStyleColor_v(ImGuiCol_Text, col);
                        g_vk.ig_Text("[%s] %s", l.source, l.text);
                        g_vk.ig_PopStyleColor(1);
                    }
                    if (g_auto_scroll && latest != g_last_log_seq) g_vk.ig_SetScrollHereY(1.0f);
                    g_last_log_seq = latest;
                }
                g_vk.ig_EndChild();

                const float run_w = 64.0f;
                const float box_w = std::max(120.0f, g_vk.ig_GetContentRegionAvail().x - run_w - 8.0f);
                const float box_h = g_vk.ig_GetFrameHeight() + 4.0f;
                const ImVec2 p0 = g_vk.ig_GetCursorScreenPos();
                const ImVec2 p1(p0.x + box_w, p0.y + box_h);
                g_vk.ig_InvisibleButton("##js_repl_box", ImVec2(box_w, box_h), 0);
                if (g_vk.ig_IsItemClicked(0)) g_console_focus = true;
                ImDrawList* wdl = g_vk.ig_GetWindowDrawList();
                g_vk.dl_AddRectFilled(wdl, p0, p1, IM_COL32(24, 20, 15, 245), 4.0f, 0);
                g_vk.dl_AddRect(wdl, p0, p1, g_console_focus ? kGold : kEdge, 4.0f, 0, 1.2f);
                ImFont* f = font ? font : g_vk.ig_GetFont();
                const float fpx = g_vk.ig_GetFontSize();
                const ImVec2 text_pos(p0.x + 8.0f, p0.y + (box_h - fpx) * 0.5f);
                g_vk.dl_PushClipRect(wdl, ImVec2(p0.x + 4.0f, p0.y), ImVec2(p1.x - 4.0f, p1.y), true);
                const float prompt_w = g_vk.font_CalcTextSizeA(f, fpx, FLT_MAX, 0.0f, "> ", nullptr, nullptr).x;
                g_vk.dl_AddText(wdl, f, fpx, text_pos, kGold, "> ", nullptr, 0.0f, nullptr);
                const int64_t now_ns = g_vk.prof_now ? g_vk.prof_now() : 0;
                if (g_cmd_len > 0) {
                    const float caret_off = g_vk.font_CalcTextSizeA(f, fpx, FLT_MAX, 0.0f, g_cmd, g_cmd + g_cmd_caret, nullptr).x;
                    const float avail = std::max(20.0f, box_w - 20.0f - prompt_w);
                    const float scroll_x = caret_off > avail ? caret_off - avail : 0.0f;
                    g_vk.dl_PushClipRect(wdl, ImVec2(text_pos.x + prompt_w, p0.y), ImVec2(p1.x - 6.0f, p1.y), true);
                    g_vk.dl_AddText(wdl, f, fpx, ImVec2(text_pos.x + prompt_w - scroll_x, text_pos.y),
                                    kInkBright, g_cmd, nullptr, 0.0f, nullptr);
                    if (g_console_focus && ((now_ns / 500000000LL) & 1) == 0) {
                        const float cx = text_pos.x + prompt_w + caret_off - scroll_x;
                        g_vk.dl_AddLine(wdl, ImVec2(cx, p0.y + 4.0f), ImVec2(cx, p1.y - 4.0f), kGold, 1.5f);
                    }
                    g_vk.dl_PopClipRect(wdl);
                } else {
                    const char* hint = "Enter JS expression (e.g. game.player.getPos()) and press Enter...";
                    g_vk.dl_AddText(wdl, f, fpx, ImVec2(text_pos.x + prompt_w, text_pos.y), kDim, hint, nullptr, 0.0f, nullptr);
                    if (g_console_focus && ((now_ns / 500000000LL) & 1) == 0) {
                        const float cx = text_pos.x + prompt_w;
                        g_vk.dl_AddLine(wdl, ImVec2(cx, p0.y + 4.0f), ImVec2(cx, p1.y - 4.0f), kGold, 1.5f);
                    }
                }
                g_vk.dl_PopClipRect(wdl);

                g_vk.ig_SameLine(0.0f, -1.0f);
                if (g_vk.ig_Button("Run", ImVec2(run_w, box_h))) submit_console_cmd();
                g_vk.ig_EndTabItem();
            }
            if (g_vk.ig_BeginTabItem("Scripts", nullptr, 0)) {
                if (g_snap.script_count == 0) {
                    g_vk.ig_TextWrapped("No scripts discovered yet. Drop .js files into %s and press F8 to reload.",
                                        g_snap.scripts_dir[0] ? g_snap.scripts_dir : "scripts/");
                } else if (g_vk.ig_BeginTable("js_scripts_tbl", 5,
                                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                                             ImVec2(0.0f, 0.0f), 0.0f)) {
                    g_vk.ig_TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 34.0f, 0);
                    g_vk.ig_TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch, 1.1f, 0);
                    g_vk.ig_TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.3f, 0);
                    g_vk.ig_TableSetupColumn("Ver", ImGuiTableColumnFlags_WidthFixed, 64.0f, 0);
                    g_vk.ig_TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.5f, 0);
                    g_vk.ig_TableHeadersRow();
                    for (int i = 0; i < g_snap.script_count; i++) {
                        MkwVkJsScript& s = g_snap.scripts[i];
                        g_vk.ig_TableNextRow(0, 0.0f);
                        g_vk.ig_PushID_i(i);
                        g_vk.ig_TableSetColumnIndex(0);
                        bool en = s.enabled != 0;
                        if (g_vk.ig_Checkbox("##en", &en)) {
                            s.enabled = en ? 1 : 0;
                            send_event(MKWVK_JS_EV_SCRIPT_TOGGLE, nullptr, s.id, 0.0f, s.enabled);
                        }
                        g_vk.ig_TableSetColumnIndex(1);
                        g_vk.ig_TextUnformatted(s.id, nullptr);
                        g_vk.ig_TableSetColumnIndex(2);
                        g_vk.ig_TextUnformatted(s.name[0] ? s.name : s.id, nullptr);
                        g_vk.ig_TableSetColumnIndex(3);
                        g_vk.ig_TextUnformatted(s.version[0] ? s.version : "-", nullptr);
                        g_vk.ig_TableSetColumnIndex(4);
                        if (s.error[0]) {
                            g_vk.ig_PushStyleColor_v(ImGuiCol_Text, ImVec4(1.0f, 0.42f, 0.42f, 1.0f));
                            g_vk.ig_TextWrapped("%s", s.error);
                            g_vk.ig_PopStyleColor(1);
                        } else if (s.loaded) {
                            g_vk.ig_PushStyleColor_v(ImGuiCol_Text, ImVec4(0.45f, 0.92f, 0.55f, 1.0f));
                            g_vk.ig_TextUnformatted("Loaded", nullptr);
                            g_vk.ig_PopStyleColor(1);
                        } else {
                            g_vk.ig_TextDisabled(s.enabled ? "Idle" : "Disabled");
                        }
                        g_vk.ig_PopID();
                    }
                    g_vk.ig_EndTable();
                }
                g_vk.ig_EndTabItem();
            }
            g_vk.ig_EndTabBar();
        }
    }
    g_vk.ig_End();
    if (!open) {
        g_console_open = false;
        send_event(MKWVK_JS_EV_CONSOLE_STATE, nullptr, nullptr, 0.0f, 0);
    }
}

void js_overlay_draw(uint32_t w, uint32_t h, bool main_menu_open) {
    const mkwvk::ui::Fonts& f = g_vk.ui_fonts();
    draw_hud_prims(g_vk.ig_GetBackgroundDrawList(), f.body);
    draw_toasts(g_vk.ig_GetForegroundDrawList(), f.strong ? f.strong : f.body, w, h);

    if (main_menu_open) return;

    if (g_console_open || has_open_js_window()) {
        if (f.body) g_vk.ig_PushFont(f.body, 17.0f);
        push_panel_style();
        draw_js_windows();
        draw_console(f.body, w, h);
        pop_panel_style();
        if (f.body) g_vk.ig_PopFont();
    }
}

/* ---- MinHook Detours on longshot_vk.dll ---- */
void hk_menu_tick() {
    g_in_menu_tick = true;
    orig_menu_tick();
    g_in_menu_tick = false;

    const MkwVkHost* host = g_vk.p_host ? *g_vk.p_host : nullptr;
    const bool open = g_vk.menu_is_open ? g_vk.menu_is_open() : false;
    js_overlay_tick(host, open);
}

bool hk_ui_begin(uint32_t w, uint32_t h) {
    if (js_overlay_active() && g_vk.p_GImGui && *g_vk.p_GImGui) {
        js_overlay_before_begin(w, h);
    }
    return orig_ui_begin(w, h);
}

void hk_ui_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    if (!g_drew_this_frame && js_overlay_active()) {
        g_drew_this_frame = true;
        const bool open = g_vk.menu_is_open ? g_vk.menu_is_open() : false;
        js_overlay_draw(w, h, open);
    }
    orig_ui_record(cmd, index, w, h);
}

void hk_ui_record_to(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    if (!g_drew_this_frame && js_overlay_active()) {
        g_drew_this_frame = true;
        const bool open = g_vk.menu_is_open ? g_vk.menu_is_open() : false;
        js_overlay_draw(w, h, open);
    }
    orig_ui_record_to(cmd, image, view, w, h);
}

void hk_menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    g_drew_this_frame = false;
    orig_menu_record(cmd, index, w, h);
    if (!g_drew_this_frame && js_overlay_active()) {
        if (hk_ui_begin(w, h)) {
            if (g_vk.rec_boundary) g_vk.rec_boundary(kBoundDirect);
            g_drew_this_frame = true;
            const bool open = g_vk.menu_is_open ? g_vk.menu_is_open() : false;
            js_overlay_draw(w, h, open);
            orig_ui_record(cmd, index, w, h);
        }
    }
}

void hk_menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    g_drew_this_frame = false;
    orig_menu_record_image(cmd, image, view, w, h);
    if (!g_drew_this_frame && js_overlay_active()) {
        if (hk_ui_begin(w, h)) {
            if (g_vk.rec_boundary) g_vk.rec_boundary(kBoundDirect);
            g_drew_this_frame = true;
            const bool open = g_vk.menu_is_open ? g_vk.menu_is_open() : false;
            js_overlay_draw(w, h, open);
            orig_ui_record_to(cmd, image, view, w, h);
        }
    }
}

INIT_ONCE g_vk_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK attach_once(PINIT_ONCE, PVOID param, PVOID*) {
    void* vk_mod = param;
    CoffSymReq reqs[] = {
        {"_ZN5mkwvk9menu_tickEv", nullptr},
        {"_ZN5mkwvk11menu_recordEP17VkCommandBuffer_Tjjj", nullptr},
        {"_ZN5mkwvk17menu_record_imageEP17VkCommandBuffer_TP9VkImage_TP13VkImageView_Tjj", nullptr},
        {"_ZN5mkwvk12menu_is_openEv", nullptr},
        {"_ZN5mkwvk8prof_nowEv", nullptr},
        {"_ZN5mkwvk4hostE", nullptr},
        {"_ZN5mkwvk12_GLOBAL__N_17editingE", nullptr},
        {"GImGui", nullptr},
        {"_ZN5mkwvk2ui5beginEjj", nullptr},
        {"_ZN5mkwvk2ui5fontsEv", nullptr},
        {"_ZN5mkwvk2ui6recordEP17VkCommandBuffer_Tjjj", nullptr},
        {"_ZN5mkwvk2ui9record_toEP17VkCommandBuffer_TP9VkImage_TP13VkImageView_Tjj", nullptr},
        {"_ZN5mkwvk3rec8boundaryENS0_5BoundE", nullptr},
        {"_ZN5ImGui5GetIOEv", nullptr},
        {"_ZN5ImGui5BeginEPKcPbi", nullptr},
        {"_ZN5ImGui3EndEv", nullptr},
        {"_ZN5ImGui10BeginChildEPKcRK6ImVec2ii", nullptr},
        {"_ZN5ImGui8EndChildEv", nullptr},
        {"_ZN5ImGui11BeginTabBarEPKci", nullptr},
        {"_ZN5ImGui9EndTabBarEv", nullptr},
        {"_ZN5ImGui12BeginTabItemEPKcPbi", nullptr},
        {"_ZN5ImGui10EndTabItemEv", nullptr},
        {"_ZN5ImGui10BeginTableEPKciiRK6ImVec2f", nullptr},
        {"_ZN5ImGui8EndTableEv", nullptr},
        {"_ZN5ImGui16TableSetupColumnEPKcifj", nullptr},
        {"_ZN5ImGui15TableHeadersRowEv", nullptr},
        {"_ZN5ImGui12TableNextRowEif", nullptr},
        {"_ZN5ImGui19TableSetColumnIndexEi", nullptr},
        {"_ZN5ImGui6ButtonEPKcRK6ImVec2", nullptr},
        {"_ZN5ImGui15InvisibleButtonEPKcRK6ImVec2i", nullptr},
        {"_ZN5ImGui13IsItemClickedEi", nullptr},
        {"_ZN5ImGui8CheckboxEPKcPb", nullptr},
        {"_ZN5ImGui11SliderFloatEPKcPfffS1_i", nullptr},
        {"_ZN5ImGui11ProgressBarEfRK6ImVec2PKc", nullptr},
        {"_ZN5ImGui4TextEPKcz", nullptr},
        {"_ZN5ImGui11TextWrappedEPKcz", nullptr},
        {"_ZN5ImGui15TextUnformattedEPKcS1_", nullptr},
        {"_ZN5ImGui12TextDisabledEPKcz", nullptr},
        {"_ZN5ImGui9SeparatorEv", nullptr},
        {"_ZN5ImGui8SameLineEff", nullptr},
        {"_ZN5ImGui6PushIDEi", nullptr},
        {"_ZN5ImGui5PopIDEv", nullptr},
        {"_ZN5ImGui14PushStyleColorEiRK6ImVec4", nullptr},
        {"_ZN5ImGui13PopStyleColorEi", nullptr},
        {"_ZN5ImGui12PushStyleVarEif", nullptr},
        {"_ZN5ImGui12PushStyleVarEiRK6ImVec2", nullptr},
        {"_ZN5ImGui11PopStyleVarEi", nullptr},
        {"_ZN5ImGui8PushFontEP6ImFontf", nullptr},
        {"_ZN5ImGui7PopFontEv", nullptr},
        {"_ZN5ImGui16SetNextWindowPosERK6ImVec2iS2_", nullptr},
        {"_ZN5ImGui17SetNextWindowSizeERK6ImVec2i", nullptr},
        {"_ZN5ImGui7GetFontEv", nullptr},
        {"_ZN5ImGui11GetFontSizeEv", nullptr},
        {"_ZN5ImGui14GetFrameHeightEv", nullptr},
        {"_ZN5ImGui21GetContentRegionAvailEv", nullptr},
        {"_ZN5ImGui18GetCursorScreenPosEv", nullptr},
        {"_ZN5ImGui17GetWindowDrawListEv", nullptr},
        {"_ZN5ImGui21GetBackgroundDrawListEv", nullptr},
        {"_ZN5ImGui21GetForegroundDrawListEv", nullptr},
        {"_ZN5ImGui14SetScrollHereYEf", nullptr},
        {"_ZN5ImGui23ColorConvertU32ToFloat4Ej", nullptr},
        {"_ZN10ImDrawList7AddTextEP6ImFontfRK6ImVec2jPKcS6_fPK6ImVec4", nullptr},
        {"_ZN10ImDrawList7AddRectERK6ImVec2S2_jffi", nullptr},
        {"_ZN10ImDrawList13AddRectFilledERK6ImVec2S2_jfi", nullptr},
        {"_ZN10ImDrawList7AddLineERK6ImVec2S2_jf", nullptr},
        {"_ZN10ImDrawList9AddCircleERK6ImVec2fjif", nullptr},
        {"_ZN10ImDrawList15AddCircleFilledERK6ImVec2fji", nullptr},
        {"_ZN10ImDrawList12PushClipRectERK6ImVec2S2_b", nullptr},
        {"_ZN10ImDrawList11PopClipRectEv", nullptr},
        {"_ZN6ImFont13CalcTextSizeAEfffPKcS1_PS1_", nullptr},
        {"_ZN7ImGuiIO16AddMousePosEventEff", nullptr},
        {"_ZN7ImGuiIO19AddMouseButtonEventEib", nullptr},
        {"_ZN7ImGuiIO18AddMouseWheelEventEff", nullptr},
    };
    const size_t total = sizeof reqs / sizeof reqs[0];
    const size_t got = coff_resolve_batch(vk_mod, reqs, total);

    size_t i = 0;
    g_vk.fn_menu_tick = reinterpret_cast<decltype(g_vk.fn_menu_tick)>(reqs[i++].addr);
    g_vk.fn_menu_record = reinterpret_cast<decltype(g_vk.fn_menu_record)>(reqs[i++].addr);
    g_vk.fn_menu_record_image = reinterpret_cast<decltype(g_vk.fn_menu_record_image)>(reqs[i++].addr);
    g_vk.menu_is_open = reinterpret_cast<decltype(g_vk.menu_is_open)>(reqs[i++].addr);
    g_vk.prof_now = reinterpret_cast<decltype(g_vk.prof_now)>(reqs[i++].addr);
    g_vk.p_host = reinterpret_cast<decltype(g_vk.p_host)>(reqs[i++].addr);
    g_vk.p_editing = reinterpret_cast<decltype(g_vk.p_editing)>(reqs[i++].addr);
    g_vk.p_GImGui = reinterpret_cast<decltype(g_vk.p_GImGui)>(reqs[i++].addr);
    g_vk.fn_ui_begin = reinterpret_cast<decltype(g_vk.fn_ui_begin)>(reqs[i++].addr);
    g_vk.ui_fonts = reinterpret_cast<decltype(g_vk.ui_fonts)>(reqs[i++].addr);
    g_vk.fn_ui_record = reinterpret_cast<decltype(g_vk.fn_ui_record)>(reqs[i++].addr);
    g_vk.fn_ui_record_to = reinterpret_cast<decltype(g_vk.fn_ui_record_to)>(reqs[i++].addr);
    g_vk.rec_boundary = reinterpret_cast<decltype(g_vk.rec_boundary)>(reqs[i++].addr);
    g_vk.ig_GetIO = reinterpret_cast<decltype(g_vk.ig_GetIO)>(reqs[i++].addr);
    g_vk.ig_Begin = reinterpret_cast<decltype(g_vk.ig_Begin)>(reqs[i++].addr);
    g_vk.ig_End = reinterpret_cast<decltype(g_vk.ig_End)>(reqs[i++].addr);
    g_vk.ig_BeginChild = reinterpret_cast<decltype(g_vk.ig_BeginChild)>(reqs[i++].addr);
    g_vk.ig_EndChild = reinterpret_cast<decltype(g_vk.ig_EndChild)>(reqs[i++].addr);
    g_vk.ig_BeginTabBar = reinterpret_cast<decltype(g_vk.ig_BeginTabBar)>(reqs[i++].addr);
    g_vk.ig_EndTabBar = reinterpret_cast<decltype(g_vk.ig_EndTabBar)>(reqs[i++].addr);
    g_vk.ig_BeginTabItem = reinterpret_cast<decltype(g_vk.ig_BeginTabItem)>(reqs[i++].addr);
    g_vk.ig_EndTabItem = reinterpret_cast<decltype(g_vk.ig_EndTabItem)>(reqs[i++].addr);
    g_vk.ig_BeginTable = reinterpret_cast<decltype(g_vk.ig_BeginTable)>(reqs[i++].addr);
    g_vk.ig_EndTable = reinterpret_cast<decltype(g_vk.ig_EndTable)>(reqs[i++].addr);
    g_vk.ig_TableSetupColumn = reinterpret_cast<decltype(g_vk.ig_TableSetupColumn)>(reqs[i++].addr);
    g_vk.ig_TableHeadersRow = reinterpret_cast<decltype(g_vk.ig_TableHeadersRow)>(reqs[i++].addr);
    g_vk.ig_TableNextRow = reinterpret_cast<decltype(g_vk.ig_TableNextRow)>(reqs[i++].addr);
    g_vk.ig_TableSetColumnIndex = reinterpret_cast<decltype(g_vk.ig_TableSetColumnIndex)>(reqs[i++].addr);
    g_vk.ig_Button = reinterpret_cast<decltype(g_vk.ig_Button)>(reqs[i++].addr);
    g_vk.ig_InvisibleButton = reinterpret_cast<decltype(g_vk.ig_InvisibleButton)>(reqs[i++].addr);
    g_vk.ig_IsItemClicked = reinterpret_cast<decltype(g_vk.ig_IsItemClicked)>(reqs[i++].addr);
    g_vk.ig_Checkbox = reinterpret_cast<decltype(g_vk.ig_Checkbox)>(reqs[i++].addr);
    g_vk.ig_SliderFloat = reinterpret_cast<decltype(g_vk.ig_SliderFloat)>(reqs[i++].addr);
    g_vk.ig_ProgressBar = reinterpret_cast<decltype(g_vk.ig_ProgressBar)>(reqs[i++].addr);
    g_vk.ig_Text = reinterpret_cast<decltype(g_vk.ig_Text)>(reqs[i++].addr);
    g_vk.ig_TextWrapped = reinterpret_cast<decltype(g_vk.ig_TextWrapped)>(reqs[i++].addr);
    g_vk.ig_TextUnformatted = reinterpret_cast<decltype(g_vk.ig_TextUnformatted)>(reqs[i++].addr);
    g_vk.ig_TextDisabled = reinterpret_cast<decltype(g_vk.ig_TextDisabled)>(reqs[i++].addr);
    g_vk.ig_Separator = reinterpret_cast<decltype(g_vk.ig_Separator)>(reqs[i++].addr);
    g_vk.ig_SameLine = reinterpret_cast<decltype(g_vk.ig_SameLine)>(reqs[i++].addr);
    g_vk.ig_PushID_i = reinterpret_cast<decltype(g_vk.ig_PushID_i)>(reqs[i++].addr);
    g_vk.ig_PopID = reinterpret_cast<decltype(g_vk.ig_PopID)>(reqs[i++].addr);
    g_vk.ig_PushStyleColor_v = reinterpret_cast<decltype(g_vk.ig_PushStyleColor_v)>(reqs[i++].addr);
    g_vk.ig_PopStyleColor = reinterpret_cast<decltype(g_vk.ig_PopStyleColor)>(reqs[i++].addr);
    g_vk.ig_PushStyleVar_f = reinterpret_cast<decltype(g_vk.ig_PushStyleVar_f)>(reqs[i++].addr);
    g_vk.ig_PushStyleVar_v = reinterpret_cast<decltype(g_vk.ig_PushStyleVar_v)>(reqs[i++].addr);
    g_vk.ig_PopStyleVar = reinterpret_cast<decltype(g_vk.ig_PopStyleVar)>(reqs[i++].addr);
    g_vk.ig_PushFont = reinterpret_cast<decltype(g_vk.ig_PushFont)>(reqs[i++].addr);
    g_vk.ig_PopFont = reinterpret_cast<decltype(g_vk.ig_PopFont)>(reqs[i++].addr);
    g_vk.ig_SetNextWindowPos = reinterpret_cast<decltype(g_vk.ig_SetNextWindowPos)>(reqs[i++].addr);
    g_vk.ig_SetNextWindowSize = reinterpret_cast<decltype(g_vk.ig_SetNextWindowSize)>(reqs[i++].addr);
    g_vk.ig_GetFont = reinterpret_cast<decltype(g_vk.ig_GetFont)>(reqs[i++].addr);
    g_vk.ig_GetFontSize = reinterpret_cast<decltype(g_vk.ig_GetFontSize)>(reqs[i++].addr);
    g_vk.ig_GetFrameHeight = reinterpret_cast<decltype(g_vk.ig_GetFrameHeight)>(reqs[i++].addr);
    g_vk.ig_GetContentRegionAvail = reinterpret_cast<decltype(g_vk.ig_GetContentRegionAvail)>(reqs[i++].addr);
    g_vk.ig_GetCursorScreenPos = reinterpret_cast<decltype(g_vk.ig_GetCursorScreenPos)>(reqs[i++].addr);
    g_vk.ig_GetWindowDrawList = reinterpret_cast<decltype(g_vk.ig_GetWindowDrawList)>(reqs[i++].addr);
    g_vk.ig_GetBackgroundDrawList = reinterpret_cast<decltype(g_vk.ig_GetBackgroundDrawList)>(reqs[i++].addr);
    g_vk.ig_GetForegroundDrawList = reinterpret_cast<decltype(g_vk.ig_GetForegroundDrawList)>(reqs[i++].addr);
    g_vk.ig_SetScrollHereY = reinterpret_cast<decltype(g_vk.ig_SetScrollHereY)>(reqs[i++].addr);
    g_vk.ig_ColorConvertU32ToFloat4 = reinterpret_cast<decltype(g_vk.ig_ColorConvertU32ToFloat4)>(reqs[i++].addr);
    g_vk.dl_AddText = reinterpret_cast<decltype(g_vk.dl_AddText)>(reqs[i++].addr);
    g_vk.dl_AddRect = reinterpret_cast<decltype(g_vk.dl_AddRect)>(reqs[i++].addr);
    g_vk.dl_AddRectFilled = reinterpret_cast<decltype(g_vk.dl_AddRectFilled)>(reqs[i++].addr);
    g_vk.dl_AddLine = reinterpret_cast<decltype(g_vk.dl_AddLine)>(reqs[i++].addr);
    g_vk.dl_AddCircle = reinterpret_cast<decltype(g_vk.dl_AddCircle)>(reqs[i++].addr);
    g_vk.dl_AddCircleFilled = reinterpret_cast<decltype(g_vk.dl_AddCircleFilled)>(reqs[i++].addr);
    g_vk.dl_PushClipRect = reinterpret_cast<decltype(g_vk.dl_PushClipRect)>(reqs[i++].addr);
    g_vk.dl_PopClipRect = reinterpret_cast<decltype(g_vk.dl_PopClipRect)>(reqs[i++].addr);
    g_vk.font_CalcTextSizeA = reinterpret_cast<decltype(g_vk.font_CalcTextSizeA)>(reqs[i++].addr);
    g_vk.io_AddMousePosEvent = reinterpret_cast<decltype(g_vk.io_AddMousePosEvent)>(reqs[i++].addr);
    g_vk.io_AddMouseButtonEvent = reinterpret_cast<decltype(g_vk.io_AddMouseButtonEvent)>(reqs[i++].addr);
    g_vk.io_AddMouseWheelEvent = reinterpret_cast<decltype(g_vk.io_AddMouseWheelEvent)>(reqs[i++].addr);

    if (got != total) {
        if (g_exe.hos_log) {
            for (size_t k = 0; k < total; k++) {
                if (!reqs[k].addr)
                    g_exe.hos_log("longshot-js: WARNING missing vk symbol %s", reqs[k].name);
            }
        }
        return TRUE;
    }

    g_vk_ready = true;
    struct { const char* name; void* target; void* detour; void** tramp; } hooks[] = {
        {"menu_tick", (void*)g_vk.fn_menu_tick, (void*)hk_menu_tick, (void**)&orig_menu_tick},
        {"menu_record", (void*)g_vk.fn_menu_record, (void*)hk_menu_record, (void**)&orig_menu_record},
        {"menu_record_image", (void*)g_vk.fn_menu_record_image, (void*)hk_menu_record_image, (void**)&orig_menu_record_image},
        {"ui::begin", (void*)g_vk.fn_ui_begin, (void*)hk_ui_begin, (void**)&orig_ui_begin},
        {"ui::record", (void*)g_vk.fn_ui_record, (void*)hk_ui_record, (void**)&orig_ui_record},
        {"ui::record_to", (void*)g_vk.fn_ui_record_to, (void*)hk_ui_record_to, (void**)&orig_ui_record_to},
    };
    for (const auto& h : hooks) {
        if (MH_CreateHook(h.target, h.detour, h.tramp) == MH_OK) {
            MH_EnableHook(h.target);
        } else if (g_exe.hos_log) {
            g_exe.hos_log("longshot-js: vk hook failed for %s", h.name);
        }
    }
    if (g_exe.hos_log) {
        g_exe.hos_log("longshot-js: ImGui & F9 console attached to stock longshot_vk.dll (%zu/%zu symbols)", got, total);
    }
    return TRUE;
}

} // namespace

extern "C" void asi_hooks_vk_attach(void* vk_hmodule) {
    if (!vk_hmodule) return;
    InitOnceExecuteOnce(&g_vk_once, attach_once, vk_hmodule, nullptr);
}
