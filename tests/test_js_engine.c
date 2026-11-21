/* Unit test suite for the Longshot JavaScript runtime (host/js_*.c + QuickJS-NG). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/js_api.h"
#include "../host/js_internal.h"

/* ---- Mock Guest & Host State ---- */
#define MOCK_GD_SLOT 0x84c9e180ULL
#define MOCK_GUEST_TOTAL (16u * 1024u * 1024u)
static uint8_t *g_mock_guest = NULL;
static uint64_t g_mock_sys_cursor = 0x800000ULL;
static int g_mark_count = 0;
static int g_inv_count = 0;
static int g_want_mouse = 0;
static int64_t g_frame = 0;
volatile int64_t hos_present_count = 0;

int js_player_stats(int values[5]) {
    for (int i = 0; i < 5; i++) values[i] = i;
    return 1;
}
int js_area_name(const char *map, char *out, size_t cap) {
    return snprintf(out, cap, "%s", map) > 0;
}
const uint32_t *js_lifted_functions(uint32_t *total) {
    static const uint32_t functions[] = {0x80123400};
    *total = 1;
    return functions;
}
uint64_t js_zstd_size(const void *src, size_t size) { (void)src; (void)size; return UINT64_MAX; }
int js_zstd_decode(void *dst, size_t cap, const void *src, size_t size, const void *dict, size_t ds) {
    (void)dst; (void)cap; (void)src; (void)size; (void)dict; (void)ds;
    return 0;
}

uint8_t *hos_guest_base(void) { return g_mock_guest; }
uint64_t hos_guest_size(void) { return MOCK_GUEST_TOTAL; }
int hos_mem_region(uint64_t addr, uint64_t size) {
    if (addr == MOCK_GD_SLOT && size == 8) return 1;
    if (addr < 0x100000ULL) return 0;
    if (size > MOCK_GUEST_TOTAL || addr > MOCK_GUEST_TOTAL - size) return 0;
    return 1;
}
uint64_t hos_sys_alloc(uint64_t size, uint64_t align) {
    if (align < 8) align = 8;
    uint64_t a = (g_mock_sys_cursor + (align - 1)) & ~(align - 1);
    if (a + size > MOCK_GUEST_TOTAL) return 0;
    g_mock_sys_cursor = a + size;
    return a;
}
uint64_t hos_guest_alloc(uint64_t size) { return hos_sys_alloc(size, 16); }
void hos_mark_written(const void *p, size_t n) { (void)p, (void)n; g_mark_count++; }

void hos_log(const char *fmt, ...) {
    (void)fmt;
}

int64_t hos_now_ns(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart * 1000000000.0) / (double)freq.QuadPart);
}

int hos_mods_count(void) { return 0; }
int hos_mod_scripts_dir(int index, char *out, size_t cap, const char **name_out) {
    (void)index, (void)out, (void)cap, (void)name_out;
    return 0;
}
int hos_text_focus(void) { return 0; }
void hos_display_js_want_mouse(int want) { g_want_mouse = want; }

static void mock_guest_fn(Ctx *A64_NOESC c) {
    if (c) {
        c->x[0] += 42;
        if (c->x[8]) js_mem_write(c->x[8], &c->x[0], 8);
    }
}
A64Fn a64_lookup(uint64_t addr) {
    return addr == 0x80123400ULL ? mock_guest_fn : NULL;
}
void a64_rt_invalidate_sites(uint64_t addr) { (void)addr; g_inv_count++; }

static int g_opts[16];
int rx_option(int id, int otherwise) {
    return (id >= 0 && id < 16 && g_opts[id]) ? g_opts[id] : otherwise;
}
void rx_option_set(int id, int value) {
    if (id >= 0 && id < 16) g_opts[id] = value;
}
int rx_control_frame_info(int64_t *p, int64_t *d, int *w, int *h) {
    if (p) *p = hos_present_count;
    if (d) *d = 1;
    if (w) *w = 1280;
    if (h) *h = 720;
    return 1;
}
int rx_control_shot(const char *path, int64_t after, void (*done)(void *, int64_t, int), void *ctx) {
    (void)after;
    if (done) done(ctx, hos_present_count + 1, path && *path ? 1 : 0);
    return 1;
}
int rx_control_setting(int index, HosControlSetting *out) {
    (void)index;
    if (out) memset(out, 0, sizeof *out);
    return 0;
}
int rx_control_setting_set(const char *key, int value, HosControlSetting *out) {
    (void)key, (void)value;
    if (out) memset(out, 0, sizeof *out);
    return 1;
}

int hos_fs_host_path(const char *guest, char *out, size_t cap) {
    if (!strcmp(guest, "content:/rsdb/fixture")) {
        snprintf(out, cap, "%s/unit_mod.js", getenv("HOS_JS_DIR"));
        return 1;
    }
    if (out && cap) out[0] = 0;
    return 0;
}

static const struct { const char *c; const char *a; } g_dests[] = {
    {"Kokiri Forest (Link's House)", "Temple of Time (Pedestal)"},
    {"Hyrule Field", "Kakariko Village"},
};
int hos_oot_destination(int index, const char **child, const char **adult) {
    if (index < 0 || index >= 2) return 0;
    if (child) *child = g_dests[index].c;
    if (adult) *adult = g_dests[index].a;
    return 1;
}
int hos_oot_teleport_ask(int index) { (void)index; return 1; }
int hos_oot_age_ask(int adult) { (void)adult; return 1; }

int hos_oot_js_pouch_count(struct GuestContext *c, const char *name) { (void)c, (void)name; return 3; }
static int mock_item_count = 10;
int hos_oot_js_pouch_change(struct GuestContext *c, const char *name, int delta) { (void)c, (void)name; return mock_item_count += delta; }
int hos_oot_js_item_count(struct GuestContext *c, const char *name) { (void)c, (void)name; return mock_item_count; }
int hos_oot_js_gamedata_set_bool(struct GuestContext *c, uint32_t hash, int value) { (void)c, (void)hash, (void)value; return 1; }
void hos_oot_js_set_time_seconds(struct GuestContext *c, uint32_t seconds) { (void)c, (void)seconds; }
int hos_oot_js_set_player_pos(struct GuestContext *c, uint64_t actor, uint64_t components, float x, float y, float z) {
    (void)c, (void)actor, (void)components, (void)x, (void)y, (void)z;
    return 1;
}
int hos_oot_js_teleport_index(struct GuestContext *c, uint64_t actor, unsigned index) { (void)c, (void)actor, (void)index; return 1; }
int hos_oot_js_teleport_custom(struct GuestContext *c, uint64_t actor, const char *banc, const char *start_pos, const char *boot_event) {
    (void)c, (void)actor, (void)banc, (void)start_pos, (void)boot_event;
    return 1;
}
int hos_oot_js_trigger_event(struct GuestContext *c, const char *event_name, uint64_t actor) { (void)c, (void)event_name, (void)actor; return 1; }
int hos_oot_js_toggle_age(struct GuestContext *c, uint64_t actor, int target_adult) { (void)c, (void)actor, (void)target_adult; return 1; }
uint64_t hos_oot_js_gameplay_view(struct GuestContext *c) { (void)c; return 0; }
uint64_t hos_oot_js_player_controller(struct GuestContext *c, uint64_t components) { (void)c, (void)components; return 0; }

/* ---- Test Harness ---- */
static void step_frames(int n, double dt) {
    for (int i = 0; i < n; i++) {
        g_frame++;
        hos_present_count = g_frame;
        hos_js_on_frame(g_frame, dt);
    }
}

static void eval_must(const char *code, const char *expect_substr) {
    char out[65536];
    const int ok = hos_js_eval_json(code, "<test>", out, sizeof out);
    if (!ok || !strstr(out, "\"ok\":true")) {
        fprintf(stderr, "FAIL eval_must('%s') => %s\n", code, out);
        exit(1);
    }
    if (expect_substr && !strstr(out, expect_substr)) {
        fprintf(stderr, "FAIL eval_must('%s') expected '%s', got: %s\n", code, expect_substr, out);
        exit(1);
    }
}

static void rm_rf_dir(const char *dir) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char sub[MAX_PATH];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_rf_dir(sub);
            else DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

int main(void) {
    g_mock_guest = VirtualAlloc(NULL, MOCK_GD_SLOT + 8, MEM_RESERVE, PAGE_NOACCESS);
    assert(g_mock_guest && VirtualAlloc(g_mock_guest, MOCK_GUEST_TOTAL, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(g_mock_guest + MOCK_GD_SLOT, 8, MEM_COMMIT, PAGE_READWRITE));

    char tmp[MAX_PATH], base_dir[MAX_PATH], sdir[MAX_PATH], ddir[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(base_dir, sizeof base_dir, "%s\\longshot_js_unit_%lu", tmp, (unsigned long)GetCurrentProcessId());
    snprintf(sdir, sizeof sdir, "%s\\scripts", base_dir);
    snprintf(ddir, sizeof ddir, "%s\\scripts_data", base_dir);
    rm_rf_dir(base_dir);
    CreateDirectoryA(base_dir, NULL);
    CreateDirectoryA(sdir, NULL);
    CreateDirectoryA(ddir, NULL);

    _putenv_s("HOS_JS_DIR", sdir);
    _putenv_s("HOS_JS_DATA_DIR", ddir);

    /* Write an ES module mod script that imports "longshot" and registers handlers + UI + virtual RomFS */
    char script_path[MAX_PATH];
    snprintf(script_path, sizeof script_path, "%s\\unit_mod.js", sdir);
    FILE *sf = fopen(script_path, "wb");
    assert(sf != NULL);
    fputs(
        "import { game, memory, native, ui, fs, events, Longshot } from 'longshot';\n"
        "import { Greenhook } from 'greenhook'; import g from 'greenhook/game';\n"
        "if (Greenhook !== Longshot || g !== game) throw Error('module alias mismatch');\n"
        "globalThis.__unit = { loaded: true, timerOk: false, frameOk: false, waitOk: false, btnCount: 0, sliderVal: 0, unloaded: false };\n"
        "events.on('unload', () => { globalThis.__unit.unloaded = true; });\n"
        "setTimeout(() => { globalThis.__unit.timerOk = true; }, 10);\n"
        "(async () => {\n"
        "  await nextFrame();\n"
        "  globalThis.__unit.frameOk = true;\n"
        "  await waitFrames(3);\n"
        "  globalThis.__unit.waitOk = true;\n"
        "})();\n"
        "game.romfs.setVirtualFile('text/hello.txt', 'hyrule-virtual');\n"
        "game.romfs.redirect('actor/redirect.bin', 'C:\\\\mock\\\\alt.bin');\n"
        "ui.toast('Unit boot', 2000, '#44cc66');\n"
        "ui.draw.text(10, 20, 'HUD', '#ffffff', 18);\n"
        "const w = ui.window('win1', 'Test Panel', { x: 40, y: 50, width: 280, height: 180 });\n"
        "w.button('go_btn', 'Go', () => { globalThis.__unit.btnCount++; });\n"
        "w.slider('sc_sl', 'Scale', 0.5, 2.0, 1.0, (v) => { globalThis.__unit.sliderVal = v; });\n",
        sf
    );
    fclose(sf);

    hos_js_init();
    assert(hos_js_is_ready() == 1);

    /* 1. List discovered scripts */
    char buf[65536];
    assert(hos_js_list_json(buf, sizeof buf) == 1);
    if (!strstr(buf, "\"id\":\"unit_mod.js\"") || !strstr(buf, "\"loaded\":true")) {
        fprintf(stderr, "FAIL list_json: %s\n", buf);
        return 1;
    }

    /* 2. ES module globals + Promise / setTimeout / nextFrame / waitFrames */
    Sleep(25);
    step_frames(6, 1.0 / 60.0);
    eval_must("[typeof memory.readU32, typeof ui.window, __unit.timerOk, __unit.frameOk, __unit.waitOk].join(',')",
              "function,function,true,true,true");

    /* 3. Watchdog timeout protection on infinite loop */
    char wout[4096];
    const int wok = hos_js_eval_json("while (true) {}", "<watchdog>", wout, sizeof wout);
    assert(wok == 0);
    if (!strstr(wout, "\"ok\":false")) {
        fprintf(stderr, "FAIL watchdog expected error, got: %s\n", wout);
        return 1;
    }

    /* 4. Murmur3 flag hashes */
    eval_must("game.flags.hash('Sequence_CurrentBanc') >>> 0", "492931546"); /* 0x1d6189da */
    eval_must("game.flags.hash('PlayerStatus.Life') >>> 0", "833312128");     /* 0x31ab5580 */

    /* 5. Bounds-checked memory R/W, scan, alloc, unmapped rejection */
    eval_must("(()=>{ memory.writeU32(0x120000, 0xCAFEBABE); memory.writeCString(0x120100, 'Zelda'); "
              "return (memory.readU32(0x120000) >>> 0).toString(16) + ':' + memory.readCString(0x120100); })()",
              "cafebabe:Zelda");
    assert(g_mark_count > 0);
    eval_must("(()=>{ memory.writeBytes(0x130000, [0xDE, 0xAD, 0x42, 0xEF]); "
              "return memory.scan(0x130000, 0x200, 'DE AD ?? EF').map(a => Number(a)); })()",
              "1245184"); /* 0x130000 */
    eval_must("(()=>{ const p = memory.alloc(64); memory.writeU32(p, 777); return memory.readU32(p); })()", "777");
    eval_must("(()=>{ try { memory.readU32(0x1000); return 'no-throw'; } catch (e) { return 'threw'; } })()", "threw");
    eval_must("(()=>{ try { memory.readU32(0x50000000); return 'no-throw'; } catch (e) { return 'threw'; } })()", "threw");

    /* 6. Sandbox fs + path traversal rejection */
    eval_must("(()=>{ fs.writeJson('sub/cfg.json', { a: 1, b: 'ok' }); return fs.readJson('sub/cfg.json').b; })()", "ok");
    eval_must("(()=>{ try { fs.readText('../scripts/unit_mod.js'); return 'no-throw'; } catch (e) { return 'threw'; } })()", "threw");

    /* 7. RomFS virtual file + redirect lookup */
    assert(hos_js_romfs_virtual_exists("text/hello.txt", NULL) == 1);
    uint8_t *vbuf = NULL;
    uint64_t vlen = 0;
    assert(hos_js_romfs_virtual_copy("rom:/text/hello.txt", &vbuf, &vlen) == 1);
    assert(vlen == 14 && memcmp(vbuf, "hyrule-virtual", 14) == 0);
    free(vbuf);
    char redir[MAX_PATH] = "";
    assert(hos_js_romfs_redirect("actor/redirect.bin", redir, sizeof redir) == 1);
    assert(strstr(redir, "alt.bin") != NULL);

    /* 8. UI snapshot & event round-trip */
    MkwVkJsSnapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.struct_size = sizeof snap;
    assert(hos_js_ui_snapshot_vk(&snap) == 1);
    assert(snap.runtime_ready == 1);
    assert(snap.prim_count == 1);
    assert(snap.toast_count == 1);
    assert(snap.window_count == 1 && !strcmp(snap.windows[0].id, "win1"));
    assert(snap.windows[0].widget_count == 2);

    MkwVkJsEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = MKWVK_JS_EV_CLICK;
    snprintf(ev.target_id, sizeof ev.target_id, "win1");
    snprintf(ev.widget_id, sizeof ev.widget_id, "go_btn");
    hos_js_ui_event_vk(&ev);

    memset(&ev, 0, sizeof ev);
    ev.kind = MKWVK_JS_EV_SLIDER;
    snprintf(ev.target_id, sizeof ev.target_id, "win1");
    snprintf(ev.widget_id, sizeof ev.widget_id, "sc_sl");
    ev.fval = 1.75f;
    hos_js_ui_event_vk(&ev);

    step_frames(1, 1.0 / 60.0);
    eval_must("JSON.stringify([__unit.btnCount, __unit.sliderVal])", "1,1.75");
    (void)g_inv_count;
    (void)g_want_mouse;

    eval_must("(() => { const p = native.listFunctions(0, 1); return [p.total,p.next === null,p.addresses[0] === 0x80123400n].join(',') })()", "1,true,true");
    eval_must("(() => { let n=0; const old=events.on('old',()=>{}); old(); const off=events.on('new',()=>n++); old(); events.emit('new'); off(); return n===1?'pass':'fail' })()", "pass");
    eval_must("(() => { for(let i=0;i<63;i++) if(!game.world.setTime(12)) return 'early'; return [game.inventory.set('Arrow',5),game.world.setTime(12)].join(',') })()", "true,false");
    eval_must("game.onPlayerUpdate(ev=>{globalThis.__ctrl=ev.controller; globalThis.__called=native.call(0x80123400,{x:[1],indirectResult:0x125000}).x0})", NULL);
    eval_must("game.onActorUpdate(a=>globalThis.__actor=[a.instanceId,a.statusCode,a.ptr===0x120000n].join(','));"
              "memory.writeU32(0x120018,123);memory.writeU32(0x12002c,7)", NULL);
    Ctx guest = {0};
    hos_js_on_actor_update(&guest, 0x120000);
    eval_must("__actor","123,7,true");
    hos_js_on_player_update(&guest, 0x120000, 0, 0);
    eval_must("[__called === 43n,__ctrl === 0n,game.world.setTime(12)].join(',')", "true,true,true");
    assert(guest.x[8] == 0);
    eval_must("memory.readU64(0x125000) === 43n ? 'written' : 'missing'", "written");
    eval_must("game.romfs.exists('RSDB/fixture') ? 'mounted' : 'missing'", "mounted");
    assert(mock_item_count == 5);
    eval_must("memory.writeU64(0x120000,0x124000n);memory.writeU64(0x124008,0x80123400n);"
              "const methods=game.actors.getMethods(0x120000,0,2);"
              "[methods[0]===null,methods[1]===0x80123400n].join(',')","true,true");
    eval_must("[game.player.getProgress().skulltulaTokens, game.world.getAreaName('Village')].join(',')", "3,Village");
    eval_must("game.flags.list('int') === null ? 'unavailable' : 'wrong'", "unavailable");
    eval_must("memory.writeU64(0x84c9e180,0x100000n);memory.writeU64(0x100000,0x110000n);"
              "memory.writeU32(0x1100d0,1);memory.writeU64(0x1100e0,0x130000n);memory.writeU32(0x1100ec,129);"
              "memory.writeU32(0x130000,0);memory.writeU32(0x130400,1234);memory.writeU32(0x130404,0);"
              "const a=game.flags.list('int'),b=game.flags.list('int',a.next);"
              "[a.next,b.next===null,b.hashes.join(',')].join(',')","128,true,1234");

    /* 9. Clean reload */
    assert(hos_js_reload() == 1);
    step_frames(1, 1.0 / 60.0);
    eval_must("__unit.btnCount", "0");
    /* Overlay coordinate space: frame size until the overlay reports its image, then the overlay size. */
    hos_js_set_overlay_state(0, 0, 0);
    eval_must("const m0 = game.input.getMouse(); `${m0.overlayWidth}x${m0.overlayHeight}:${m0.overUi}`", "1280x720:false");
    hos_js_set_overlay_state(1920, 1080, 1);
    eval_must("const m1 = game.input.getMouse(); `${m1.overlayWidth}x${m1.overlayHeight}:${m1.overUi}:${typeof m1.overlayX}`", "1920x1080:true:number");
    hos_js_set_overlay_state(0, 0, 0);

    hos_js_shutdown();
    rm_rf_dir(base_dir);
    VirtualFree(g_mock_guest, 0, MEM_RELEASE);
    printf("ALL UNIT TESTS PASSED\n");
    return 0;
}
