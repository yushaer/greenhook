/* Longshot JavaScript Scripting API (QuickJS-NG runtime, host integration). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GuestContext;

/* Lifecycle (hos_main.c) */
void hos_js_init(void);
void hos_js_shutdown(void);
int hos_js_reload(void);
int hos_js_is_ready(void);

/* Game / host lifecycle hooks */
void hos_js_on_frame(int64_t frame_num, double dt_sec);
void hos_js_on_actor_update(struct GuestContext *c, uint64_t actor);
void hos_js_on_player_update(struct GuestContext *c, uint64_t actor, uint64_t components, uint64_t player);
int hos_js_on_damage(uint32_t old_life, uint32_t *in_out_hit);
int hos_js_on_camera_view(struct GuestContext *c, uint64_t view);
int hos_js_on_mouse_camera(uint64_t cam, float *yaw, float *pitch);
void hos_js_on_input(uint64_t *buttons, int32_t stick[4], uint64_t raw_buttons, const int32_t raw_stick[4]);
void hos_js_on_event_request(struct GuestContext *c, const char *name, uint64_t arg);
int hos_js_on_timeline_start(struct GuestContext *c, const char *name, uint64_t player);
int hos_js_on_save_request(struct GuestContext *c, uint32_t reason, uint32_t mode, uint64_t caller, int is_transition);
void hos_js_on_menu_tick(struct GuestContext *c, uint64_t screen);
void hos_js_on_menu_focus(struct GuestContext *c, uint64_t screen, uint64_t pane, uint64_t lr);

/* Fast atomic modifier getters (hot-path overrides; -1 = unset for toggles, 1.0f = neutral for scales) */
int hos_js_player_invincible(void);
int hos_js_player_flight(void);
float hos_js_player_speed_scale(void);
float hos_js_damage_multiplier(void);
float hos_js_fov_scale(void);
float hos_js_text_speed_scale(void);
float hos_js_text_size_scale(void);
float hos_js_camera_speed_scale(void);
int hos_js_camera_disable_centering(void);
float hos_js_audio_volume_scale(void);
float hos_js_audio_pitch_scale(void);
int hos_js_audio_muted(void);

/* RomFS overrides (nn_fs.c) */
int hos_js_romfs_redirect(const char *rel, char *out, size_t cap);
int hos_js_romfs_virtual_copy(const char *rel, uint8_t **out_buf, uint64_t *out_len);
int hos_js_romfs_virtual_exists(const char *rel, uint64_t *out_len);

/* Control bridge JSON helpers (control.c) */
int hos_js_eval_json(const char *code, const char *filename, char *out_json, size_t cap);
int hos_js_list_json(char *out_json, size_t cap);
int hos_js_log_json(int since_seq, char *out_json, size_t cap);

/* Native AArch64 hook lookup (a64_rt.c) */
void (*a64_js_lookup_hook(uint64_t addr))(struct GuestContext *);
void a64_rt_invalidate_sites(uint64_t addr);

/* Dear ImGui overlay / window / console UI bridge (rx_backend.c, display.c, vk_js_overlay.cpp) */
int hos_js_ui_active(void);
int hos_js_ui_snapshot_vk(void *out_snap);
void hos_js_ui_event_vk(const void *ev);
void hos_display_js_want_mouse(int want);
/* Overlay image size and Dear ImGui's mouse capture, recorded by the overlay each frame so scripts can
   draw and hit-test in the same pixels as ui.draw (hooks_vk.cpp -> js_io.c). */
void hos_js_set_overlay_state(int width, int height, int ui_wants_mouse);

/* Thin C wrappers from hos_main.c for high-level OoT scripting */
int hos_oot_js_pouch_count(struct GuestContext *c, const char *name);
int hos_oot_js_pouch_change(struct GuestContext *c, const char *name, int delta);
int hos_oot_js_item_count(struct GuestContext *c, const char *name);
int hos_oot_js_gamedata_set_bool(struct GuestContext *c, uint32_t hash, int value);
void hos_oot_js_set_time_seconds(struct GuestContext *c, uint32_t seconds);
int hos_oot_js_set_player_pos(struct GuestContext *c, uint64_t actor, uint64_t components, float x, float y, float z);
int hos_oot_js_teleport_index(struct GuestContext *c, uint64_t actor, unsigned index);
int hos_oot_js_teleport_custom(struct GuestContext *c, uint64_t actor, const char *banc, const char *start_pos, const char *boot_event);
int hos_oot_js_trigger_event(struct GuestContext *c, const char *event_name, uint64_t actor);
int hos_oot_js_toggle_age(struct GuestContext *c, uint64_t actor, int target_adult);
uint64_t hos_oot_js_gameplay_view(struct GuestContext *c);
uint64_t hos_oot_js_player_controller(struct GuestContext *c, uint64_t components);

#ifdef __cplusplus
}
#endif
