// @ts-check
import { game, ui, fs } from "greenhook";
import {
    readResource, decodeByml, encodeByml, cloneByml, byml, writeResource, installSavedOverrides,
    removeSavedOverride, resourceSize, setResourceSizes, trackActors,
} from "greenhook/data";

/**
 * Example 05: Scene editor with a free-fly camera, spawning and deleting.
 *
 * F7 enters/leaves the editor. Link freezes and the camera detaches.
 *
 *   Camera   Hold right mouse + move to look (or arrow keys). WASD fly, Q/E down/up,
 *            Shift fast, Ctrl slow. F frames the selection.
 *   Select   Left-click a marker. Esc deselects.
 *   Move     Drag the centre handle (Alt: up/down) or the X/Y/Z arrows. Ctrl snaps.
 *   Rotate   R turns 15 degrees (Shift+R the other way).
 *   Spawn    Pick a type on the Spawn tab, then press N to place it under the cursor.
 *   Delete   Delete key removes the selection (spawned or authored).
 *   History  Ctrl+Z undo, Ctrl+Y redo, Backspace resets the selection.
 *   Commit   Enter writes the area files and reloads the area.
 *
 * How it works: edits are saved in scripts_data/map-editor/edits.json. Applying rebuilds each
 * edited .bcett.byml placement file from the stock file plus your edits and serves it as a file
 * override that is re-registered at every launch. The game sizes each file from its resource size
 * table, which it reads at startup, so the first launch with this script saves an enlarged table
 * and asks for one restart; after that every area has room for roughly 150 new objects per file.
 *
 * Limits: collision for most scenery and props is pre-built per area (Phive static compound
 * bodies) and keyed by the original object's ID, so spawned copies of those types have no
 * collision. The Spawn tab marks each type. Physics-driven objects cannot be dragged live; their
 * new position shows after Enter. Areas without a teleport destination must be re-entered by hand.
 */

// ---------------------------------------------------------------------------
// Boot: re-register saved file overrides before the game reads them.

const reinstalled = installSavedOverrides();
console.info(`Scene editor: re-registered ${reinstalled} saved file override(s)`);

const EDITS_PATH = "map-editor/edits.json";
const SETTINGS_PATH = "map-editor/settings.json";
const SLACK_MARK = "map-editor/slack.json";
const SLACK_BYTES = 16 * 1024;
const BUDGET_OVERHEAD = 288;   // the size table entry is the file size plus a small fixed overhead
const TOGGLE_KEY = "F7";
const MAX_MARKERS = 300;
const PICK_RADIUS = 14;
const HANDLE_RADIUS = 9;
const ROT_STEP_DEG = 15;
const SNAP = 0.5;
const MATCH_RADIUS = 1.0;
const PAGE = 10;

const FIELD_NAMES = ({ translate: "Translate", rotate: "Rotate", scale: "Scale" });
const AXES = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
const AXIS_COLORS = ["#ff5050", "#5cff6a", "#5aa0ff"];

/** @typedef {number[]} V3 */
/** @typedef {{ translate?: number[], rotate?: number[], scale?: number[] }} Pose */
/** @typedef {{ hash: string, srt: number, gyaml: string, template: string, translate: number[], rotate?: number[], scale?: number[] }} Spawn */
/** @typedef {{ moved: Record<string, Pose>, deleted: string[], spawned: Spawn[] }} FileEdits */
/** @typedef {{ path: string, file: string, dynamic: boolean, gyaml: string, hash: string, fields: Record<string, number[]>, spawn?: Spawn }} Placement */

// ---------------------------------------------------------------------------
// Persistence

/** @param {string} path @param {any} fallback */
function readJson(path, fallback) {
    try {
        const v = fs.exists(path) ? fs.readJson(path) : null;
        return v && typeof v === "object" ? v : fallback;
    } catch { return fallback; }
}
/** @param {string} path @param {any} value */
function writeJson(path, value) {
    try { fs.mkdir("map-editor"); fs.writeJson(path, value); }
    catch (err) { ui.toast("Scene editor: could not save (" + String(err) + ")", { color: "#ff6b6b" }); }
}

/** @returns {Record<string, FileEdits>} */
function loadEdits() {
    const raw = readJson(EDITS_PATH, {});
    if (raw.version === 2 && raw.files && typeof raw.files === "object") return raw.files;
    // Version 1 stored path -> hash -> pose.
    /** @type {Record<string, FileEdits>} */
    const files = {};
    for (const [path, poses] of Object.entries(raw)) {
        if (path.startsWith("Banc/") && poses && typeof poses === "object") files[path] = { moved: poses, deleted: [], spawned: [] };
    }
    return files;
}
const edits = loadEdits();
const saveEdits = () => writeJson(EDITS_PATH, { version: 2, files: edits });
const settings = Object.assign(
    { speed: 8, fov: 50, sensitivity: 0.25, markers: true, labels: true, degrees: false, reloadOnSpawn: true },
    readJson(SETTINGS_PATH, {}));
const saveSettings = () => writeJson(SETTINGS_PATH, settings);

/** @param {string} path */
const fileEdits = path => (edits[path] ??= { moved: {}, deleted: [], spawned: [] });
/** @param {FileEdits | undefined} f */
const isEmpty = f => !f || (!Object.keys(f.moved).length && !f.deleted.length && !f.spawned.length);

// Enlarge the size table once RomFS is readable; it takes effect from the next launch.
let needsRestart = false;
(async () => {
    for (let i = 0; i < 36000; i++) {
        try { if (game.romfs.listDir("System/Resource").length) break; } catch { /* not mounted yet */ }
        await nextFrame();
    }
    const table = game.romfs.listDir("System/Resource").find(n => n.endsWith(".rsizetable.zs"));
    const tablePath = table ? "System/Resource/" + table : null;
    if (readJson(SLACK_MARK, {}).extra === SLACK_BYTES && tablePath && game.romfs.exists(tablePath) &&
        readJson("romfs-overrides/manifest.json", {})[tablePath]) return;
    // Measure from the stock table so the slack is added once.
    if (tablePath) removeSavedOverride(tablePath);
    /** @type {Record<string, number>} */
    const sizes = {};
    for (const dir of game.romfs.listDir("Banc")) {
        let names = [];
        try { names = game.romfs.listDir("Banc/" + dir); } catch { continue; }
        for (const n of names) {
            if (!n.endsWith(".bcett.byml.zs")) continue;
            const path = `Banc/${dir}/${n}`;
            game.romfs.clearOverride(path);
            const size = resourceSize(path);
            if (size !== null) sizes[path] = size + SLACK_BYTES;
        }
    }
    const r = setResourceSizes(sizes);
    writeJson(SLACK_MARK, { extra: SLACK_BYTES, files: r.updated.length });
    needsRestart = true;
    // Files re-read above lost their overrides for this session; put them back.
    for (const path of Object.keys(edits)) applyFile(path);
    ui.toast("Scene editor installed. Restart the game once so areas have room for new objects.", { color: "#ffd866", durationMs: 9000 });
})().catch(e => console.error("Scene editor slack:", String(e)));

// ---------------------------------------------------------------------------
// Vectors and camera

const add = (/** @type {V3} */ a, /** @type {V3} */ b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (/** @type {V3} */ a, /** @type {V3} */ b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul = (/** @type {V3} */ a, /** @type {number} */ s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (/** @type {V3} */ a, /** @type {V3} */ b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const len = (/** @type {V3} */ a) => Math.hypot(a[0], a[1], a[2]);
const norm = (/** @type {V3} */ a) => mul(a, 1 / (len(a) || 1));
const dist = (/** @type {V3} */ a, /** @type {V3} */ b) => len(sub(a, b));
const vec = (/** @type {{x:number,y:number,z:number}} */ v) => [v.x, v.y, v.z];

const cam = { eye: [0, 0, 0], yaw: 0, pitch: 0 };
function basis() {
    const sy = Math.sin(cam.yaw), cy = Math.cos(cam.yaw), sp = Math.sin(cam.pitch), cp = Math.cos(cam.pitch);
    return { right: [cy, 0, -sy], up: [-sp * sy, cp, -sp * cy], forward: [-cp * sy, -sp, -cp * cy] };
}
/** @typedef {ReturnType<typeof basis>} Basis */
/** @typedef {{ w: number, h: number }} Screen */

function readMouse() {
    const m = /** @type {any} */ (game.input.getMouse());
    const overlay = typeof m.overlayX === "number" && m.overlayWidth > 0;
    const win = game.graphics.getWindowSize();
    return {
        x: overlay ? m.overlayX : m.x, y: overlay ? m.overlayY : m.y,
        left: (m.buttons & 1) !== 0, right: (m.buttons & 2) !== 0, overUi: Boolean(m.overUi),
        scr: overlay ? { w: m.overlayWidth, h: m.overlayHeight } : { w: win.width || 1280, h: win.height || 720 },
    };
}
/** @typedef {ReturnType<typeof readMouse>} Mouse */

/** @param {V3} p @param {Basis} b @param {Screen} scr */
function project(p, b, scr) {
    const d = sub(p, cam.eye), depth = dot(d, b.forward);
    if (depth < 0.05) return null;
    const t = Math.tan(settings.fov * Math.PI / 360), aspect = scr.w / scr.h;
    const nx = dot(d, b.right) / (depth * t * aspect), ny = dot(d, b.up) / (depth * t);
    return { x: (nx * 0.5 + 0.5) * scr.w, y: (-ny * 0.5 + 0.5) * scr.h, depth };
}
/** @param {number} sx @param {number} sy @param {Basis} b @param {Screen} scr */
function mouseRay(sx, sy, b, scr) {
    const t = Math.tan(settings.fov * Math.PI / 360), aspect = scr.w / scr.h;
    return norm(add(b.forward, add(mul(b.right, (2 * sx / scr.w - 1) * t * aspect), mul(b.up, (1 - 2 * sy / scr.h) * t))));
}
function startCameraFromGame() {
    const pose = game.camera.getPose(), pos = game.player.getPos();
    if (pose) {
        cam.eye = vec(pose.eye);
        const f = norm(sub(vec(pose.target), cam.eye));
        cam.pitch = Math.asin(Math.max(-1, Math.min(1, -f[1])));
        cam.yaw = Math.atan2(-f[0], -f[2]);
    } else if (pos) { cam.eye = [pos.x, pos.y + 3, pos.z + 6]; cam.yaw = 0; cam.pitch = 0.3; }
}
const pushCamera = () => game.camera.setOverride({ eye: { x: cam.eye[0], y: cam.eye[1], z: cam.eye[2] }, yaw: cam.yaw, pitch: cam.pitch });

// ---------------------------------------------------------------------------
// Area files

/** @type {Placement[]} */
let placements = [];
let mapId = /** @type {string | null} */ (null);
let loadError = "";
/** @type {Map<string, any[]>} stock Actors per path, for templates */
const stockActors = new Map();

/** @param {string} map */
function findPlacementFiles(map) {
    const dirs = [];
    try {
        const all = game.romfs.listDir("Banc");
        if (all.includes(map)) dirs.push(map);
        for (const d of all) if (d !== map && map.startsWith(d)) dirs.push(d);
    } catch { /* fall back to the map ID */ }
    if (!dirs.length) dirs.push(map);
    const files = [];
    for (const dir of dirs) {
        let names = [];
        try { names = game.romfs.listDir("Banc/" + dir); } catch { continue; }
        for (const n of names) if (/\.bcett\.byml\.zs$/.test(n)) files.push(`Banc/${dir}/${n}`);
    }
    const own = files.filter(f => (f.split("/").pop() ?? "").startsWith(map));
    return own.length ? own : files;
}

/** Stock Actors array of a placement file (reads past any override). @param {string} path */
function stock(path) {
    let actors = stockActors.get(path);
    if (!actors) {
        game.romfs.clearOverride(path);
        const bytes = readResource(path);
        const root = bytes && decodeByml(bytes).root;
        actors = root && !Array.isArray(root) && Array.isArray(root.Actors) ? root.Actors : [];
        stockActors.set(path, actors);
        if (!isEmpty(edits[path])) applyFile(path); // restore the override for the game
    }
    return actors;
}

const hashOf = (/** @type {any} */ a) => String(byml.value(a?.Hash));
/** @param {any} node */
const nums = node => Array.isArray(node) ? node.map(v => Number(byml.value(v))) : null;

function loadScene() {
    mapId = game.world.getMap();
    placements = [];
    loadError = "";
    selected = null;
    stockActors.clear();
    if (!mapId) return;
    for (const path of findPlacementFiles(mapId)) {
        const file = path.split("/").pop() ?? path, dynamic = /Dynamic/i.test(file);
        let actors = [];
        try { actors = stock(path); } catch (err) { loadError = `${file}: ${String(err)}`; continue; }
        const fe = edits[path];
        const deleted = new Set(fe?.deleted ?? []);
        for (const a of actors) {
            const hash = hashOf(a), t = nums(a.Translate);
            if (!t || t.length < 3 || deleted.has(hash) || typeof a.Gyaml !== "string") continue;
            /** @type {Record<string, number[]>} */
            const fields = { translate: t };
            const r = nums(a.Rotate), s = nums(a.Scale);
            if (r) fields.rotate = r;
            if (s) fields.scale = s;
            placements.push({ path, file, dynamic, gyaml: a.Gyaml, hash, fields });
        }
        for (const sp of fe?.spawned ?? []) {
            const fields = { translate: sp.translate, ...(sp.rotate ? { rotate: sp.rotate } : {}), ...(sp.scale ? { scale: sp.scale } : {}) };
            placements.push({ path, file, dynamic, gyaml: sp.gyaml, hash: sp.hash, fields, spawn: sp });
        }
    }
    collisionCache.clear();
    bakedIds = undefined;
}

/** @param {Placement} p @param {"translate"|"rotate"|"scale"} key @returns {number[] | undefined} */
function value(p, key) {
    if (p.spawn) return p.spawn[key];
    return /** @type {any} */ (edits[p.path]?.moved[p.hash])?.[key] ?? p.fields[key];
}
const pos3 = (/** @type {Placement} */ p) => /** @type {V3} */ (value(p, "translate"));
const isEdited = (/** @type {Placement} */ p) => Boolean(p.spawn || edits[p.path]?.moved[p.hash]);

/** @param {Placement} p @param {"translate"|"rotate"|"scale"} key @param {number[]} v */
function setValue(p, key, v) {
    const f = v.map(n => Math.fround(n));
    if (p.spawn) { p.spawn[key] = f; return; }
    const moved = fileEdits(p.path).moved;
    (moved[p.hash] ??= {})[key] = f;
    dirty.add(p.path);
}

// Undo works on whole-file snapshots of the edit record, which also covers spawn and delete.
/** @type {{ path: string, before: string, after: string, hash: string }[]} */
const undoStack = [];
/** @type {typeof undoStack} */
const redoStack = [];
/** @type {Set<string>} files with edits not yet applied to the game */
const dirty = new Set();
const snap = (/** @type {string} */ path) => JSON.stringify(edits[path] ?? null);
/** @param {string} path @param {string} json */
function restore(path, json) {
    const v = JSON.parse(json);
    if (v) edits[path] = v; else delete edits[path];
}
/** @param {string} path @param {string} before @param {string} hash */
function commit(path, before, hash) {
    if (isEmpty(edits[path])) delete edits[path];
    const after = snap(path);
    if (before === after) return;
    undoStack.push({ path, before, after, hash });
    if (undoStack.length > 200) undoStack.shift();
    redoStack.length = 0;
    dirty.add(path);
    saveEdits();
    rebuildPanel();
}
/** @param {boolean} redo */
function history(redo) {
    const step = (redo ? redoStack : undoStack).pop();
    if (!step) return;
    (redo ? undoStack : redoStack).push(step);
    restore(step.path, redo ? step.after : step.before);
    dirty.add(step.path);
    saveEdits();
    const keep = step.hash;
    loadScene();
    selected = placements.find(p => p.hash === keep) ?? null;
    ui.toast((redo ? "Redo" : "Undo") + " (press Enter to apply)", { durationMs: 1200 });
    rebuildPanel();
}

// ---------------------------------------------------------------------------
// Writing area files

/** @param {any[] | undefined} node @param {number[]} values */
const typed = (node, values) => values.map((v, i) => ({ type: node?.[i]?.type ?? "f32", value: v }));

/** Rebuilds one placement file from stock + edits and serves it. @param {string} path */
function applyFile(path) {
    const fe = edits[path];
    if (isEmpty(fe)) { removeSavedOverride(path); return { ok: true }; }
    game.romfs.clearOverride(path);
    const bytes = readResource(path);
    if (!bytes) return { ok: false, reason: "missing file" };
    const doc = decodeByml(bytes);
    if (Array.isArray(doc.root) || !Array.isArray(doc.root.Actors)) return { ok: false, reason: "missing Actors array" };
    const actors = /** @type {any[]} */ (doc.root.Actors);
    const byHash = new Map(actors.map(a => [hashOf(a), a]));
    for (const [hash, pose] of Object.entries(fe.moved)) {
        const a = byHash.get(hash);
        if (!a) continue;
        for (const [key, name] of Object.entries(FIELD_NAMES)) {
            const v = /** @type {any} */ (pose)[key];
            if (v && Array.isArray(a[name])) a[name] = typed(a[name], v);
        }
    }
    const deleted = new Set(fe.deleted);
    const out = actors.filter(a => !deleted.has(hashOf(a)));
    let skipped = 0;
    for (const sp of fe.spawned) {
        const tpl = byHash.get(sp.template) ?? actors.find(a => a.Gyaml === sp.gyaml);
        if (!tpl) { skipped++; continue; }
        const c = cloneByml(tpl);
        c.Hash = byml.u64(BigInt(sp.hash));
        c.Phive = { Placement: { ID: byml.u64(BigInt(sp.hash)) } };
        c.SRTHash = byml.u32(sp.srt);
        delete c.Links; delete c.Rails; delete c.Presence;
        c.Translate = typed(tpl.Translate, sp.translate);
        if (sp.rotate) c.Rotate = typed(tpl.Rotate, sp.rotate);
        if (sp.scale) c.Scale = typed(tpl.Scale, sp.scale);
        out.push(c);
    }
    doc.root.Actors = out;
    const encoded = encodeByml(doc);
    const budget = resourceSize(path);
    if (budget !== null && encoded.length + BUDGET_OVERHEAD > budget) {
        installSavedOverrides(); // keep serving the last version that fit
        return { ok: false, reason: needsRestart ? "restart the game once to make room" : "no room left in this area file" };
    }
    writeResource(path, encoded);
    return { ok: true, skipped };
}

/** Applies pending edits and reloads the area through its teleport destination. */
function applyAndReload() {
    if (!mapId) return false;
    const failures = [];
    for (const path of new Set([...dirty, ...placements.map(p => p.path)])) {
        if (!dirty.has(path)) continue;
        const r = applyFile(path);
        if (!r.ok) failures.push(`${path.split("/").pop()}: ${r.reason}`);
    }
    dirty.clear();
    if (failures.length) { ui.toast("Not applied: " + failures.join("; "), { color: "#ff6b6b", durationMs: 7000 }); return false; }
    const dest = game.world.destinations().find(d => d.child === mapId || d.adult === mapId);
    if (!dest) { ui.toast("Saved. This area has no teleport destination: leave and re-enter to see changes.", { color: "#ffd866", durationMs: 6000 }); return false; }
    const back = game.player.getPos();
    reloadReturn = back ? vec(back) : null;
    if (!game.world.teleport(dest.index)) { ui.toast("Reload request rejected", { color: "#ff6b6b" }); return false; }
    ui.toast("Reloading area...", { color: "#5ce67a" });
    return true;
}
/** @type {V3 | null} */
let reloadReturn = null;

// ---------------------------------------------------------------------------
// Spawning and collision info

/** @type {Map<string, "yes" | "no" | "?">} */
const collisionCache = new Map();
/** Placement IDs found in the area's static compound bodies, or null when the area is too large to scan. @type {Set<string> | null | undefined} */
let bakedIds;
/**
 * Collision for most scenery is pre-built in Phive/StaticCompoundBody/<area>/*.bphsc and refers to
 * each placement by its 64-bit ID. One pass over those files (IDs sit at 4-byte offsets) finds which
 * of this area's placements are baked. Spawned copies of baked types get no collision.
 */
function scanBakedIds() {
    if (bakedIds !== undefined) return bakedIds;
    const dir = (mapId ?? "").split("_")[0];
    let names = [];
    try { names = game.romfs.listDir("Phive/StaticCompoundBody/" + dir).filter(n => n.startsWith(mapId ?? "") || !/_(Child|Adult)/.test(n)); }
    catch { bakedIds = new Set(); return bakedIds; }
    if (names.length > 4) { bakedIds = null; return bakedIds; }
    /** @type {Map<number, number[]>} low word -> high words of this area's placement IDs */
    const wanted = new Map();
    for (const p of placements) {
        if (p.spawn) continue;
        const h = BigInt(p.hash), lo = Number(h & 0xffffffffn), hi = Number(h >> 32n);
        const list = wanted.get(lo);
        if (list) list.push(hi); else wanted.set(lo, [hi]);
    }
    bakedIds = new Set();
    for (const n of names) {
        const b = readResource(`Phive/StaticCompoundBody/${dir}/${n}`);
        if (!b) continue;
        const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
        for (let i = 0; i + 8 <= b.length; i += 4) {
            const his = wanted.get(v.getUint32(i, true));
            if (his && his.includes(v.getUint32(i + 4, true)))
                bakedIds.add(String((BigInt(v.getUint32(i + 4, true)) << 32n) | BigInt(v.getUint32(i, true))));
        }
    }
    return bakedIds;
}
/** @param {string} gyaml */
function collisionOf(gyaml) {
    const cached = collisionCache.get(gyaml);
    if (cached) return cached;
    const ids = scanBakedIds(), samples = placements.filter(p => p.gyaml === gyaml && !p.spawn);
    const result = /** @type {"yes" | "no" | "?"} */ (!ids || !samples.length ? "?" : samples.some(p => ids.has(p.hash)) ? "no" : "yes");
    collisionCache.set(gyaml, result);
    return result;
}

/** @type {{ gyaml: string, path: string, count: number }[]} */
let spawnTypes = [];
let spawnPage = 0;
let spawnType = /** @type {string | null} */ (null);
function buildSpawnTypes() {
    /** @type {Map<string, { gyaml: string, path: string, count: number }>} */
    const m = new Map();
    for (const p of placements) {
        if (p.spawn || /^(Area|.*Area(Box)?|.*Anchor|StartPos|.*EventRequestActor|BGM_.*|NavMesh.*|.*AirWall.*)$/.test(p.gyaml)) continue;
        const e = m.get(p.gyaml) ?? { gyaml: p.gyaml, path: p.path, count: 0 };
        e.count++;
        m.set(p.gyaml, e);
    }
    spawnTypes = [...m.values()].sort((a, b) => a.gyaml.localeCompare(b.gyaml));
}

function newHash(/** @type {string} */ path) {
    const used = new Set(stock(path).map(hashOf));
    for (const f of Object.values(edits)) for (const s of f.spawned) used.add(s.hash);
    for (;;) {
        const h = (1n << 62n) | (BigInt(Math.floor(Math.random() * 2 ** 30)) << 32n) | BigInt(Math.floor(Math.random() * 2 ** 32));
        if (!used.has(String(h))) return String(h);
    }
}

/** Spawns a copy of `gyaml` at `at`. Returns the new placement. @param {string} gyaml @param {V3} at */
function spawn(gyaml, at) {
    const tpl = placements.find(p => p.gyaml === gyaml && !p.spawn);
    if (!tpl) { ui.toast("No template for " + gyaml + " in this area", { color: "#ff6b6b" }); return null; }
    const before = snap(tpl.path);
    /** @type {Spawn} */
    const sp = {
        hash: newHash(tpl.path), srt: Math.floor(Math.random() * 2 ** 32), gyaml, template: tpl.hash,
        translate: at.map(n => Math.fround(n)),
        ...(tpl.fields.rotate ? { rotate: [...tpl.fields.rotate] } : {}),
        ...(tpl.fields.scale ? { scale: [...tpl.fields.scale] } : {}),
    };
    fileEdits(tpl.path).spawned.push(sp);
    commit(tpl.path, before, sp.hash);
    loadScene();
    const p = placements.find(q => q.hash === sp.hash) ?? null;
    selected = p;
    const col = collisionOf(gyaml);
    ui.toast(`Spawned ${gyaml}${col === "no" ? " (no collision for this type)" : ""}`, { color: "#5ce67a" });
    if (settings.reloadOnSpawn) applyAndReload();
    rebuildPanel();
    return p;
}

/** @param {Placement} p */
function deletePlacement(p) {
    const before = snap(p.path);
    const fe = fileEdits(p.path);
    if (p.spawn) fe.spawned = fe.spawned.filter(s => s !== p.spawn);
    else { fe.deleted.push(p.hash); delete fe.moved[p.hash]; }
    commit(p.path, before, p.hash);
    loadScene();
    ui.toast(`Deleted ${p.gyaml} (Enter to apply)`, { durationMs: 1500 });
}

// ---------------------------------------------------------------------------
// Live actors (for preview of actors that accept position writes)

const tracker = trackActors({ maxIdleFrames: 240 });
/** @type {Map<bigint, V3>} */
const actorOrigin = new Map();
/** @param {Placement} p */
function previewLive(p) {
    const o = p.spawn ? null : p.fields.translate;
    if (!o) return;
    let best = null, bd = MATCH_RADIUS;
    for (const a of tracker.list()) {
        const d = dist(actorOrigin.get(a.ptr) ?? vec(a.pos), o);
        if (d < bd) { best = a; bd = d; }
    }
    if (best) { const t = pos3(p); game.actors.setPos(best.ptr, t[0], t[1], t[2]); }
}

// ---------------------------------------------------------------------------
// Input and frame loop

/** @type {Record<string, boolean>} */
const prevKeys = {};
const down = (/** @type {string} */ k) => game.input.isKeyDown(k);
function pressed(/** @type {string} */ k) { const d = down(k), was = prevKeys[k]; prevKeys[k] = d; return d && !was; }

let active = false;
/** @type {Placement | null} */
let selected = null;
/** @type {Placement | null} */
let hovered = null;
/** @type {null | { mode: "plane" | "axis" | "vertical", axis: number, p: Placement, start: V3, m0: {x:number,y:number}, grab: V3, before: string, screenAxis: number[], worldLen: number }} */
let drag = null;
/** @type {Mouse | null} */
let lastMouse = null;
let lastTime = now(), toggleHeld = false, tick = 0;

function enter() {
    if (game.world.isLoading()) return;
    active = true;
    loadScene();
    buildSpawnTypes();
    startCameraFromGame();
    pushCamera();
    game.input.setBlockGameInput(true);
    panel.setOpen(true);
    rebuildPanel();
    if (needsRestart) ui.toast("Restart the game once before spawning so areas have room.", { color: "#ffd866", durationMs: 6000 });
}
function exit() {
    if (!active) return;
    active = false;
    drag = null;
    game.camera.setOverride(null);
    game.input.setBlockGameInput(false);
    panel.setOpen(false);
    if (dirty.size) ui.toast(`${dirty.size} area file(s) have unapplied edits. Press Enter in the editor to apply.`, { color: "#ffd866" });
}

/** @param {number} dt @param {Mouse} m */
function flyCamera(dt, m) {
    const b = basis();
    if (m.right && lastMouse?.right) {
        const k = settings.sensitivity * 0.01;
        cam.yaw -= (m.x - lastMouse.x) * k;
        cam.pitch += (m.y - lastMouse.y) * k;
    }
    const look = 1.8 * dt;
    if (down("Left")) cam.yaw += look;
    if (down("Right")) cam.yaw -= look;
    if (down("Up")) cam.pitch -= look;
    if (down("Down")) cam.pitch += look;
    cam.pitch = Math.max(-1.55, Math.min(1.55, cam.pitch));
    const ctrl = down("Ctrl");
    const speed = settings.speed * (down("Shift") ? 4 : 1) * (ctrl ? 0.25 : 1) * dt;
    let move = [0, 0, 0];
    if (!ctrl || drag) {
        if (down("W")) move = add(move, b.forward);
        if (down("S")) move = sub(move, b.forward);
        if (down("D")) move = add(move, b.right);
        if (down("A")) move = sub(move, b.right);
    }
    if (down("E")) move = add(move, [0, 1, 0]);
    if (down("Q")) move = sub(move, [0, 1, 0]);
    if (len(move) > 0) cam.eye = add(cam.eye, mul(norm(move), speed));
}

/** @param {Placement} p @param {Basis} b @param {Screen} scr */
function gizmo(p, b, scr) {
    const t = pos3(p), c = project(t, b, scr);
    if (!c) return null;
    const worldLen = Math.max(0.2, c.depth * 0.12);
    return { t, c, worldLen, ends: AXES.map(a => project(add(t, mul(a, worldLen)), b, scr)) };
}
/** @returns {V3 | null} */
function groundHit(/** @type {{x:number,y:number}} */ m, /** @type {number} */ y, /** @type {Basis} */ b, /** @type {Screen} */ scr) {
    const r = mouseRay(m.x, m.y, b, scr);
    if (Math.abs(r[1]) < 1e-4) return null;
    const k = (y - cam.eye[1]) / r[1];
    return k > 0 ? add(cam.eye, mul(r, k)) : null;
}
/** Where N places a new object: on the plane of the hovered object, else Link's height, else 6 m ahead. @param {Mouse} m */
function spawnPoint(m) {
    const b = basis();
    const y = hovered ? pos3(hovered)[1] : game.player.getPos()?.y ?? cam.eye[1] - 2;
    const hit = groundHit(m, y, b, m.scr);
    return hit && dist(hit, cam.eye) < 200 ? hit : add(cam.eye, mul(b.forward, 6));
}

/** @param {Mouse} m @param {Basis} b */
function beginDrag(m, b) {
    if (selected) {
        const g = gizmo(selected, b, m.scr);
        if (g) {
            const base = { p: selected, start: g.t, m0: { x: m.x, y: m.y }, before: snap(selected.path), worldLen: g.worldLen };
            for (let i = 0; i < 3; i++) {
                const e = g.ends[i];
                if (e && Math.hypot(e.x - m.x, e.y - m.y) <= HANDLE_RADIUS + 4) {
                    drag = { ...base, mode: "axis", axis: i, grab: [0, 0, 0], screenAxis: [e.x - g.c.x, e.y - g.c.y] };
                    return;
                }
            }
            if (Math.hypot(g.c.x - m.x, g.c.y - m.y) <= HANDLE_RADIUS + 4) {
                const up = g.ends[1];
                drag = { ...base, mode: down("Alt") ? "vertical" : "plane", axis: 1, grab: groundHit(m, g.t[1], b, m.scr) ?? g.t,
                         screenAxis: up ? [up.x - g.c.x, up.y - g.c.y] : [0, -1] };
                return;
            }
        }
    }
    if (hovered) { selected = hovered; rebuildPanel(); }
}
const snapTo = (/** @type {number} */ v) => down("Ctrl") ? Math.round(v / SNAP) * SNAP : v;
/** @param {Mouse} m @param {Basis} b */
function updateDrag(m, b) {
    if (!drag) return;
    const d = drag;
    let next;
    if (d.mode === "plane") {
        const hit = groundHit(m, d.start[1], b, m.scr);
        if (!hit) return;
        const mv = sub(hit, d.grab);
        next = [snapTo(d.start[0] + mv[0]), d.start[1], snapTo(d.start[2] + mv[2])];
    } else {
        const sa = d.screenAxis, l2 = sa[0] * sa[0] + sa[1] * sa[1];
        if (l2 < 4) return;
        next = [...d.start];
        next[d.axis] = snapTo(d.start[d.axis] + ((m.x - d.m0.x) * sa[0] + (m.y - d.m0.y) * sa[1]) / l2 * d.worldLen);
    }
    setValue(d.p, "translate", next);
    if (++tick % 6 === 0) { previewLive(d.p); rebuildPanel(); }
}
function rotateSelected(/** @type {number} */ sign) {
    if (!selected) return;
    const r = value(selected, "rotate") ?? [0, 0, 0];
    const before = snap(selected.path);
    const next = [...r];
    next[1] += sign * (settings.degrees ? ROT_STEP_DEG : ROT_STEP_DEG * Math.PI / 180);
    setValue(selected, "rotate", next);
    commit(selected.path, before, selected.hash);
}
function resetSelected() {
    if (!selected || selected.spawn) return;
    const before = snap(selected.path);
    delete fileEdits(selected.path).moved[selected.hash];
    commit(selected.path, before, selected.hash);
}
function frameSelected() { if (selected) cam.eye = sub(pos3(selected), mul(basis().forward, 6)); }

game.onFrame(() => {
    if (down(TOGGLE_KEY)) { if (!toggleHeld) { toggleHeld = true; active ? exit() : enter(); } } else toggleHeld = false;
    tick++;
    if (!active) return;
    const t = now(), dt = Math.min(0.1, Math.max(0, (t - lastTime) / 1000));
    lastTime = t;
    if (game.world.isLoading()) return;

    const m = readMouse(), b0 = basis(), ctrl = down("Ctrl");
    if (pressed("Z") && ctrl) history(false);
    if (pressed("Y") && ctrl) history(true);
    if (pressed("R")) rotateSelected(down("Shift") ? -1 : 1);
    if (pressed("F")) frameSelected();
    if (pressed("Esc")) { selected = null; rebuildPanel(); }
    if (pressed("Backspace")) resetSelected();
    if (pressed("Delete") && selected) deletePlacement(selected);
    if (pressed("N") && spawnType) spawn(spawnType, spawnPoint(m));
    if (pressed("Enter")) applyAndReload();

    flyCamera(dt, m);
    pushCamera();
    const b = basis();

    const shown = [];
    if (settings.markers) {
        for (const p of placements) {
            const s = project(pos3(p), b, m.scr);
            if (s && s.x > -20 && s.y > -20 && s.x < m.scr.w + 20 && s.y < m.scr.h + 20) shown.push({ p, s });
        }
        shown.sort((x, y) => x.s.depth - y.s.depth);
        shown.length = Math.min(shown.length, MAX_MARKERS);
    }
    hovered = null;
    if (!m.overUi && !drag) {
        let best = PICK_RADIUS;
        for (const { p, s } of shown) { const d = Math.hypot(s.x - m.x, s.y - m.y); if (d < best) { best = d; hovered = p; } }
    }
    if (m.left && !(lastMouse?.left ?? false) && !m.overUi) beginDrag(m, b0);
    else if (m.left && drag) updateDrag(m, b);
    else if (!m.left && drag) { const d = drag; drag = null; previewLive(d.p); commit(d.p.path, d.before, d.p.hash); }
    lastMouse = m;
    draw(shown, b, m);
});

// After our reload finishes, put Link back where he was so the area streams around the edit.
game.onMapChange(() => { liveReset(); });
function liveReset() {
    actorOrigin.clear();
    (async () => {
        while (game.world.isLoading() || !game.world.getMap()) await nextFrame();
        await waitFrames(30);
        if (reloadReturn) { const r = reloadReturn; reloadReturn = null; game.player.setPos(r[0], r[1], r[2]); }
        if (active) { const keep = selected?.hash; loadScene(); buildSpawnTypes(); selected = placements.find(p => p.hash === keep) ?? null; rebuildPanel(); }
    })();
}
// Teleporting to the same area may not report a map change; watch the loading flag too.
let wasLoading = false;
game.onFrame(() => {
    const l = game.world.isLoading();
    if (wasLoading && !l && reloadReturn) liveReset();
    wasLoading = l;
});
const offActors = game.onActorUpdate(a => { if (!actorOrigin.has(a.ptr) && a.pos) actorOrigin.set(a.ptr, vec(a.pos)); });

// ---------------------------------------------------------------------------
// Drawing

/** @param {Placement} p @param {Basis} b @param {Screen} scr @param {string} color @param {number} thickness */
function outline(p, b, scr, color, thickness) {
    const t = pos3(p), s = value(p, "scale") ?? [1, 1, 1], r = value(p, "rotate");
    const yaw = r ? (settings.degrees ? r[1] * Math.PI / 180 : r[1]) : 0;
    const hx = 0.6 * s[0], hz = 0.6 * s[2], h = 1.8 * s[1], c = Math.cos(yaw), sn = Math.sin(yaw);
    const corners = [];
    for (const y of [0, h]) for (const [x, z] of [[-hx, -hz], [hx, -hz], [hx, hz], [-hx, hz]])
        corners.push(project([t[0] + x * c + z * sn, t[1] + y, t[2] - x * sn + z * c], b, scr));
    for (const [i, j] of [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]]) {
        const a = corners[i], e = corners[j];
        if (a && e) ui.draw.line(a.x, a.y, e.x, e.y, { color, thickness });
    }
}
/** @param {{p: Placement, s: {x:number,y:number,depth:number}}[]} shown @param {Basis} b @param {Mouse} m */
function draw(shown, b, m) {
    for (const { p, s } of shown) {
        if (p === selected) continue;
        const r = Math.max(2.5, Math.min(6, 40 / s.depth)) + (p === hovered ? 2 : 0);
        const color = p === hovered ? "#ffffff" : p.spawn ? "#ff6ad5" : isEdited(p) ? "#5ce67acc" : p.dynamic ? "#ffad5ccc" : "#66d9ffcc";
        ui.draw.circleFilled(s.x, s.y, r, { color });
    }
    if (hovered && hovered !== selected) {
        outline(hovered, b, m.scr, "#ffffffc0", 1.5);
        const s = project(pos3(hovered), b, m.scr);
        if (s && settings.labels) ui.draw.text(s.x + 12, s.y - 9, hovered.gyaml, { color: "#ffffff", bgColor: "#000000b0", size: 14 });
    }
    if (selected) {
        outline(selected, b, m.scr, "#000000a0", 4.5);
        outline(selected, b, m.scr, "#ffd866", 2);
        const g = gizmo(selected, b, m.scr);
        if (g) {
            const hot = drag?.p === selected ? drag.mode : null;
            g.ends.forEach((e, i) => {
                if (!e) return;
                const on = hot === "axis" && drag?.axis === i;
                ui.draw.line(g.c.x, g.c.y, e.x, e.y, { color: AXIS_COLORS[i], thickness: on ? 4 : 2.5 });
                ui.draw.circleFilled(e.x, e.y, HANDLE_RADIUS * (on ? 1.2 : 0.8), { color: AXIS_COLORS[i] });
            });
            ui.draw.circleFilled(g.c.x, g.c.y, HANDLE_RADIUS, { color: hot === "plane" || hot === "vertical" ? "#ffffff" : "#ffd866" });
            if (settings.labels) ui.draw.text(g.c.x + 14, g.c.y + 8, `${selected.gyaml}\n${g.t.map(n => n.toFixed(2)).join(", ")}`, { color: "#ffd866", bgColor: "#000000b0", size: 14 });
        }
    }
    if (spawnType && !m.overUi) {
        const at = spawnPoint(m), s = project(at, b, m.scr);
        if (s) { ui.draw.circle(s.x, s.y, 8, { color: "#ff6ad5", thickness: 2 }); ui.draw.text(s.x + 10, s.y + 6, "N: " + spawnType, { color: "#ff6ad5", bgColor: "#00000090", size: 13 }); }
    }
    if (!m.overUi) ui.draw.circle(m.x, m.y, 4, { color: "#ffffffaa", thickness: 1 });
    const pending = dirty.size ? ` | ${dirty.size} file(s) pending: Enter` : "";
    ui.draw.text(16, m.scr.h - 28, `RMB look | WASD/QE fly | click select | drag handles | R rotate | N spawn | Del delete | Ctrl+Z undo | Enter apply+reload | F7 exit${pending}`,
        { color: "#ffffffd0", bgColor: "#00000090", size: 14 });
}

// ---------------------------------------------------------------------------
// Panel

const panel = ui.window("scene_editor", "Scene Editor", { x: 24, y: 90, w: 400, open: false, onClose: () => exit() });
const fmt = (/** @type {number[] | undefined} */ v, deg = false) => v ? v.map(n => (deg ? n * 180 / Math.PI : n).toFixed(2)).join(", ") : "-";

function rebuildPanel() {
    if (!active) return;
    panel.clear();
    panel.label(`${mapId ?? "(no map)"}  |  ${placements.length} objects${dirty.size ? `  |  ${dirty.size} file(s) pending` : ""}`, "#ffd866");
    if (needsRestart) panel.label("Restart the game once to give areas room for new objects.", "#ff9b6b");
    if (loadError) panel.label(loadError, "#ff6b6b");
    panel.beginTabBar("tabs");

    panel.tab("edit", "Edit");
    if (selected) {
        const p = selected;
        panel.label(`${p.gyaml}${p.spawn ? "  (spawned)" : ""}`, p.spawn ? "#ff6ad5" : isEdited(p) ? "#5ce67a" : "#66d9ff")
             .label(`${p.file}`, "#a0a0a0")
             .label(`Position  ${fmt(value(p, "translate"))}`)
             .label(`Rotate${settings.degrees ? "" : " (deg)"}  ${fmt(value(p, "rotate"), !settings.degrees)}`)
             .label(`Scale  ${fmt(value(p, "scale"))}`)
             .button("rot_l", "Rotate -15", () => rotateSelected(-1)).sameLine()
             .button("rot_r", "Rotate +15", () => rotateSelected(1)).sameLine()
             .button("focus", "Focus (F)", frameSelected)
             .button("to_link", "Move to Link", () => {
                 const pos = game.player.getPos();
                 if (!pos || !selected) return;
                 const before = snap(selected.path);
                 setValue(selected, "translate", vec(pos));
                 commit(selected.path, before, selected.hash);
             }).sameLine()
             .button("reset", "Reset", resetSelected).sameLine()
             .button("delete", "Delete (Del)", () => { if (selected) deletePlacement(selected); });
        const scale = value(p, "scale");
        if (scale) panel.slider(`scale_${p.hash}`, "Uniform scale", 0.1, 5, scale[0], v => {
            if (!selected) return;
            const before = snap(selected.path);
            setValue(selected, "scale", [v, v, v]);
            commit(selected.path, before, selected.hash);
        });
    } else {
        panel.label("Click a marker to select it. Pink = spawned, green = edited.", "#a0a0a0");
    }
    panel.endTab();

    panel.tab("spawn", "Spawn");
    const pages = Math.max(1, Math.ceil(spawnTypes.length / PAGE));
    spawnPage = Math.min(spawnPage, pages - 1);
    panel.label(spawnType ? `Placing: ${spawnType}  (N at the cursor)` : "Choose a type, then press N to place it at the cursor.", spawnType ? "#ff6ad5" : "#a0a0a0")
         .button("sp_prev", "< Prev", () => { spawnPage = (spawnPage + pages - 1) % pages; rebuildPanel(); }).sameLine()
         .button("sp_next", "Next >", () => { spawnPage = (spawnPage + 1) % pages; rebuildPanel(); }).sameLine()
         .label(`page ${spawnPage + 1}/${pages}`, "#a0a0a0")
         .beginChild("types", 260);
    for (const ty of spawnTypes.slice(spawnPage * PAGE, (spawnPage + 1) * PAGE)) {
        const col = collisionOf(ty.gyaml);
        const tag = col === "yes" ? "" : col === "no" ? "  [no collision]" : "  [collision ?]";
        panel.button("ty_" + ty.gyaml.slice(0, 40), `${ty.gyaml === spawnType ? "> " : ""}${ty.gyaml} x${ty.count}${tag}`, () => { spawnType = ty.gyaml; rebuildPanel(); });
    }
    panel.endChild()
         .checkbox("reload_spawn", "Apply + reload right after each spawn", settings.reloadOnSpawn, v => { settings.reloadOnSpawn = v; saveSettings(); })
         .label("Copies of types marked [no collision] have no collision: theirs is pre-built for the original objects.", "#808080");
    panel.endTab();

    panel.tab("settings", "Settings");
    panel.slider("speed", "Fly speed", 1, 60, settings.speed, v => { settings.speed = v; saveSettings(); })
         .slider("sens", "Mouse sensitivity", 0.05, 1, settings.sensitivity, v => { settings.sensitivity = v; saveSettings(); })
         .slider("fov", "Marker FOV", 30, 90, settings.fov, v => { settings.fov = v; saveSettings(); })
         .checkbox("markers", "Markers", settings.markers, v => { settings.markers = v; saveSettings(); }).sameLine()
         .checkbox("labels", "Labels", settings.labels, v => { settings.labels = v; saveSettings(); }).sameLine()
         .checkbox("degrees", "Rotation in degrees", settings.degrees, v => { settings.degrees = v; saveSettings(); rebuildPanel(); })
         .button("revert_area", "Revert this area", () => {
             for (const path of new Set(placements.map(p => p.path))) { delete edits[path]; dirty.add(path); }
             undoStack.length = 0; redoStack.length = 0;
             saveEdits(); loadScene(); rebuildPanel();
             ui.toast("Reverted this area. Press Enter to apply.");
         });
    panel.endTab();
    panel.endTabBar();

    panel.separator()
         .button("undo", `Undo (${undoStack.length})`, () => history(false)).sameLine()
         .button("redo", `Redo (${redoStack.length})`, () => history(true)).sameLine()
         .button("apply", "Apply + reload (Enter)", applyAndReload);
}

// ---------------------------------------------------------------------------
// Scripting API for other mods and automation.

globalThis.SceneEditor = {
    enter, exit, applyAndReload,
    isActive: () => active,
    map: () => mapId,
    types: () => { if (!placements.length) { loadScene(); buildSpawnTypes(); } return spawnTypes.map(t => ({ ...t, collision: collisionOf(t.gyaml) })); },
    /** @param {string} gyaml @param {{x:number,y:number,z:number}} at */
    spawn: (gyaml, at) => { if (!placements.length) loadScene(); const p = spawn(gyaml, vec(at)); return p ? p.hash : null; },
    /** @param {string} hash @param {{x:number,y:number,z:number}} at */
    move: (hash, at) => {
        if (!placements.length) loadScene();
        const p = placements.find(q => q.hash === hash);
        if (!p) return false;
        const before = snap(p.path);
        setValue(p, "translate", vec(at));
        commit(p.path, before, p.hash);
        return true;
    },
    /** @param {string} hash */
    remove: hash => { if (!placements.length) loadScene(); const p = placements.find(q => q.hash === hash); if (p) deletePlacement(p); return Boolean(p); },
    revertArea: () => { if (!placements.length) loadScene(); for (const path of new Set(placements.map(p => p.path))) { delete edits[path]; dirty.add(path); } saveEdits(); loadScene(); },
    pending: () => [...dirty],
    /** Objects in the loaded area: hash, type, position, spawned flag. */
    list: () => { if (!placements.length) loadScene(); return placements.map(p => ({ hash: p.hash, gyaml: p.gyaml, pos: value(p, "translate"), spawned: Boolean(p.spawn) })); },
    /** @param {string | null} hash */
    select: hash => { selected = hash ? placements.find(p => p.hash === hash) ?? null : null; rebuildPanel(); return Boolean(selected); },
    /** @param {{eye:{x:number,y:number,z:number}, yaw?:number, pitch?:number}} pose */
    setCamera: pose => { cam.eye = vec(pose.eye); if (pose.yaw !== undefined) cam.yaw = pose.yaw; if (pose.pitch !== undefined) cam.pitch = pose.pitch; if (active) pushCamera(); },
    getCamera: () => ({ eye: [...cam.eye], yaw: cam.yaw, pitch: cam.pitch }),
    /** @param {string | null} gyaml */
    setSpawnType: gyaml => {
        spawnType = gyaml;
        const i = spawnTypes.findIndex(t => t.gyaml === gyaml);
        if (i >= 0) spawnPage = Math.floor(i / PAGE);
        rebuildPanel();
    },
};

game.onUnload(() => { offActors(); tracker.stop(); exit(); });
console.info(`Scene editor loaded. Press ${TOGGLE_KEY} in game to open it.`);
