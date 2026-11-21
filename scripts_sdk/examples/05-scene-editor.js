// @ts-check
import { game, ui, fs } from "greenhook";
import { readResource, editResource, openByml, trackActors } from "greenhook/data";

/**
 * Example 05: Scene editor with a free-fly camera.
 *
 * F7 enters/leaves the editor. Link freezes and the camera detaches.
 *
 *   Camera   Hold right mouse + move to look (or arrow keys). WASD fly, Q/E down/up,
 *            Shift fast, Ctrl slow. F frames the selection.
 *   Select   Left-click an object. Blue/orange dots are authored placements, pink diamonds are
 *            live actors. The hovered object gets a white outline, the selection a yellow one.
 *            Esc deselects.
 *   Move     Drag the centre handle to slide on the ground plane (hold Alt for up/down).
 *            Drag a red/green/blue arrow to move along X/Y/Z. Hold Ctrl while dragging to snap.
 *   Rotate   R turns a placement 15 degrees (Shift+R the other way).
 *   History  Ctrl+Z undo, Ctrl+Y redo, Backspace resets the selection.
 *   Commit   Enter applies all placement edits and reloads the area.
 *
 * Placements are the area's authored .bcett.byml entries. Their edits are saved to
 * scripts_data/map-editor/edits.json and served as virtual RomFS overrides (existing fields only;
 * the BYML layer cannot add or delete objects). Live actors move immediately but only for this
 * visit; "Keep" writes a moved actor's position into its placement when one matches.
 * Outlines are boxes around the object's origin, not mesh silhouettes.
 */

const EDITS_PATH = "map-editor/edits.json";
const SETTINGS_PATH = "map-editor/settings.json";
const TOGGLE_KEY = "F7";
const MAX_PLACEMENT_MARKERS = 300;  // the overlay holds 1024 primitives per frame
const MAX_ACTOR_MARKERS = 150;
const MAX_HELD_ACTORS = 16;
const PICK_RADIUS = 14;             // px
const HANDLE_RADIUS = 9;            // px
const ROT_STEP_DEG = 15;
const SNAP = 0.5;
const MATCH_RADIUS = 1.0;           // m, live actor <-> placement origin

const FIELD_NAMES = /** @type {const} */ ({ translate: "Translate", rotate: "Rotate", scale: "Scale" });
const AXES = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
const AXIS_COLORS = ["#ff5050", "#5cff6a", "#5aa0ff"];

/** @typedef {number[]} V3 */
/** @typedef {{ translate?: number[], rotate?: number[], scale?: number[] }} PlacementEdit */
/** @typedef {{ path: string, file: string, dynamic: boolean, index: number, gyaml: string, hash: string, fields: Record<string, number[]> }} Placement */
/** @typedef {{ kind: "placement", key: string, p: Placement } | { kind: "actor", key: string, ptr: bigint, instanceId: number, name: string }} Target */

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

/** @type {Record<string, Record<string, PlacementEdit>>} path -> hash -> absolute values */
const edits = readJson(EDITS_PATH, {});
const settings = Object.assign(
    { speed: 8, fov: 50, sensitivity: 0.25, placements: true, actors: true, labels: true, degrees: false, holdActors: true, outline: 1 },
    readJson(SETTINGS_PATH, {}));
const saveEdits = () => writeJson(EDITS_PATH, edits);
const saveSettings = () => writeJson(SETTINGS_PATH, settings);

// ---------------------------------------------------------------------------
// Vector helpers

const add = (/** @type {V3} */ a, /** @type {V3} */ b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (/** @type {V3} */ a, /** @type {V3} */ b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul = (/** @type {V3} */ a, /** @type {number} */ s) => [a[0] * s, a[1] * s, a[2] * s];
const dot = (/** @type {V3} */ a, /** @type {V3} */ b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const len = (/** @type {V3} */ a) => Math.hypot(a[0], a[1], a[2]);
const norm = (/** @type {V3} */ a) => mul(a, 1 / (len(a) || 1));
const dist = (/** @type {V3} */ a, /** @type {V3} */ b) => len(sub(a, b));
const vec = (/** @type {{x:number,y:number,z:number}} */ v) => [v.x, v.y, v.z];

// ---------------------------------------------------------------------------
// Free camera. Basis matches game.camera.setOverride({ eye, yaw, pitch }).

const cam = { eye: [0, 0, 0], yaw: 0, pitch: 0 };

function basis() {
    const sy = Math.sin(cam.yaw), cy = Math.cos(cam.yaw), sp = Math.sin(cam.pitch), cp = Math.cos(cam.pitch);
    return { right: [cy, 0, -sy], up: [-sp * sy, cp, -sp * cy], forward: [-cp * sy, -sp, -cp * cy] };
}
/** @typedef {ReturnType<typeof basis>} Basis */
/** @typedef {{ w: number, h: number }} Screen */

/** Mouse and screen in ui.draw pixels (overlayX/Y), falling back to client pixels on older runtimes. */
function readMouse() {
    const m = /** @type {any} */ (game.input.getMouse());
    const overlay = typeof m.overlayX === "number" && m.overlayWidth > 0;
    const win = game.graphics.getWindowSize();
    return {
        x: overlay ? m.overlayX : m.x,
        y: overlay ? m.overlayY : m.y,
        left: (m.buttons & 1) !== 0,
        right: (m.buttons & 2) !== 0,
        overUi: Boolean(m.overUi),
        scr: overlay ? { w: m.overlayWidth, h: m.overlayHeight } : { w: win.width || 1280, h: win.height || 720 },
    };
}
/** @typedef {ReturnType<typeof readMouse>} Mouse */

/** @param {V3} p @param {Basis} b @param {Screen} scr */
function project(p, b, scr) {
    const d = sub(p, cam.eye);
    const depth = dot(d, b.forward);
    if (depth < 0.05) return null;
    const t = Math.tan(settings.fov * Math.PI / 360), aspect = scr.w / scr.h;
    const nx = dot(d, b.right) / (depth * t * aspect), ny = dot(d, b.up) / (depth * t);
    return { x: (nx * 0.5 + 0.5) * scr.w, y: (-ny * 0.5 + 0.5) * scr.h, depth };
}

/** @param {number} sx @param {number} sy @param {Basis} b @param {Screen} scr */
function mouseRay(sx, sy, b, scr) {
    const t = Math.tan(settings.fov * Math.PI / 360), aspect = scr.w / scr.h;
    const nx = 2 * sx / scr.w - 1, ny = 1 - 2 * sy / scr.h;
    return norm(add(b.forward, add(mul(b.right, nx * t * aspect), mul(b.up, ny * t))));
}

function startCameraFromGame() {
    const pose = game.camera.getPose();
    const pos = game.player.getPos();
    if (pose) {
        cam.eye = vec(pose.eye);
        const f = norm(sub(vec(pose.target), cam.eye));
        cam.pitch = Math.asin(Math.max(-1, Math.min(1, -f[1])));
        cam.yaw = Math.atan2(-f[0], -f[2]);
    } else if (pos) {
        cam.eye = [pos.x, pos.y + 3, pos.z + 6];
        cam.yaw = 0; cam.pitch = 0.3;
    }
}

function pushCamera() {
    game.camera.setOverride({ eye: { x: cam.eye[0], y: cam.eye[1], z: cam.eye[2] }, yaw: cam.yaw, pitch: cam.pitch });
}

// ---------------------------------------------------------------------------
// Placements (authored data)

/** @type {Placement[]} */
let placements = [];
let mapId = /** @type {string | null} */ (null);
let loadError = "";

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
        for (const n of names) if (/\.bcett\.byml(\.zs)?$/.test(n)) files.push(`Banc/${dir}/${n}`);
    }
    const own = files.filter(f => (f.split("/").pop() ?? "").startsWith(map));
    return own.length ? own : files;
}

/** @param {any} node */
function readVec(node) {
    if (!node || node.kind !== "array") return null;
    const out = [];
    for (const [, v] of node.entries()) { if (typeof v !== "number") return null; out.push(v); }
    return out;
}

/** @param {string} path */
function readPlacements(path) {
    const bytes = readResource(path);
    if (!bytes) return [];
    const actors = openByml(bytes).get("Actors");
    if (!actors || actors.kind !== "array") return [];
    const file = path.split("/").pop() ?? path;
    /** @type {Placement[]} */
    const out = [];
    let index = 0;
    for (const [, a] of actors.entries()) {
        const i = index++;
        if (!a || a.kind !== "map") continue;
        const hash = a.get("Hash");
        if (hash === undefined || hash === null) continue;
        /** @type {Record<string, number[]>} */
        const fields = {};
        for (const [key, name] of Object.entries(FIELD_NAMES)) { const v = readVec(a.get(name)); if (v) fields[key] = v; }
        if (!fields.translate || fields.translate.length < 3) continue;
        const gyaml = a.get("Gyaml");
        out.push({ path, file, dynamic: /Dynamic/i.test(file), index: i,
                   gyaml: typeof gyaml === "string" ? gyaml : "(unnamed)", hash: String(hash), fields });
    }
    return out;
}

function loadScene() {
    mapId = game.world.getMap();
    placements = [];
    loadError = "";
    selected = null;
    if (!mapId) return;
    for (const path of findPlacementFiles(mapId)) {
        // Read the stock file so originals stay original, then re-install the override.
        game.romfs.clearOverride(path);
        try { placements.push(...readPlacements(path)); }
        catch (err) { loadError = `${path.split("/").pop()}: ${String(err)}`; }
        if (edits[path]) dirtyFiles.add(path);
    }
    applyDirty();
}

/** @param {Placement} p @param {"translate"|"rotate"|"scale"} key @returns {number[] | undefined} */
function value(p, key) {
    const e = /** @type {any} */ (edits[p.path]?.[p.hash]);
    return (e && e[key]) || p.fields[key];
}
/** @param {Placement} p */
const pos3 = p => /** @type {V3} */ (value(p, "translate"));
/** @param {Placement} p */
const isEdited = p => Boolean(edits[p.path]?.[p.hash]);

/** @type {Set<string>} */
const dirtyFiles = new Set();

/** @param {Placement} p */
const snapshot = p => { const e = edits[p.path]?.[p.hash]; return e ? JSON.parse(JSON.stringify(e)) : undefined; };

/** @param {string} path @param {string} hash @param {PlacementEdit | undefined} e */
function writeEntry(path, hash, e) {
    if (e) (edits[path] ??= {})[hash] = e;
    else if (edits[path]) { delete edits[path][hash]; if (!Object.keys(edits[path]).length) delete edits[path]; }
    dirtyFiles.add(path);
}

/** @param {Placement} p @param {"translate"|"rotate"|"scale"} key @param {number[]} v */
function setValue(p, key, v) {
    const e = snapshot(p) ?? {};
    e[key] = v.map(n => Math.fround(n));
    writeEntry(p.path, p.hash, e);
}

/** Writes absolute values into a virtual override; re-applying is idempotent. @param {string} path */
function applyFile(path) {
    game.romfs.clearOverride(path);
    const fileEdits = edits[path];
    if (!fileEdits) return true;
    return editResource(path, bytes => {
        const actors = openByml(bytes).get("Actors");
        if (!actors || actors.kind !== "array") throw new Error("No Actors array");
        for (const [, a] of actors.entries()) {
            const e = /** @type {any} */ (fileEdits[String(a?.get?.("Hash"))]);
            if (!e) continue;
            for (const [key, name] of Object.entries(FIELD_NAMES)) {
                const node = e[key] && a.get(name);
                if (node) e[key].forEach((/** @type {number} */ n, /** @type {number} */ i) => node.set(i, n));
            }
        }
    });
}

function applyDirty() {
    for (const path of dirtyFiles) {
        try { if (!applyFile(path)) ui.toast("Could not override " + path, { color: "#ff6b6b" }); }
        catch (err) { ui.toast("Apply failed: " + String(err), { color: "#ff6b6b", durationMs: 4000 }); }
    }
    dirtyFiles.clear();
}

function reloadArea() {
    if (!mapId) return;
    applyDirty();
    if (game.world.teleport({ banc: mapId })) ui.toast("Reloading area with your edits...", { color: "#5ce67a" });
    else ui.toast("Reload request rejected", { color: "#ff6b6b" });
}

// ---------------------------------------------------------------------------
// Live actors

const tracker = trackActors({ maxIdleFrames: 240 });
/** @type {Map<bigint, V3>} where each actor was first observed */
const actorOrigin = new Map();
/** @type {Map<bigint, V3>} actors moved by the editor, held at these positions */
const heldActors = new Map();

function liveActors() {
    const player = game.actors.getPlayer()?.ptr;
    return tracker.list().filter(a => a.ptr !== player && a.pos);
}

/** @param {bigint} ptr */
function actorPos(ptr) {
    const held = heldActors.get(ptr);
    if (held) return held;
    const a = tracker.list().find(x => x.ptr === ptr);
    return a ? vec(a.pos) : null;
}

/** @param {bigint} ptr @param {V3} v */
function moveActor(ptr, v) {
    if (!actorOrigin.has(ptr)) { const p = actorPos(ptr); if (p) actorOrigin.set(ptr, p); }
    heldActors.set(ptr, v);
    if (heldActors.size > MAX_HELD_ACTORS) heldActors.delete(heldActors.keys().next().value);
    game.actors.setPos(ptr, v[0], v[1], v[2]);
}

/** Placement that spawned this actor: the one whose stored position is the actor's first observed spot. @param {bigint} ptr */
function placementForActor(ptr) {
    const origin = actorOrigin.get(ptr) ?? actorPos(ptr);
    if (!origin) return null;
    let best = null, bestD = MATCH_RADIUS;
    for (const p of placements) {
        const d = dist(p.fields.translate, origin);
        if (d < bestD) { best = p; bestD = d; }
    }
    return best;
}

/** Live actor sitting on a placement's original spot, for previewing placement moves. @param {Placement} p */
function actorForPlacement(p) {
    let best = null, bestD = MATCH_RADIUS;
    for (const a of liveActors()) {
        const d = dist(actorOrigin.get(a.ptr) ?? vec(a.pos), p.fields.translate);
        if (d < bestD) { best = a; bestD = d; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Targets: one interface over placements and actors

/** @param {Placement} p @returns {Target} */
const placementTarget = p => ({ kind: "placement", key: "p:" + p.path + "|" + p.hash, p });
/** @param {{ ptr: bigint, instanceId: number, name?: string }} a @returns {Target} */
const actorTarget = a => ({ kind: "actor", key: "a:" + a.ptr + ":" + a.instanceId, ptr: a.ptr, instanceId: a.instanceId, name: a.name || "actor" });

/** @param {Target} t */
const targetName = t => t.kind === "placement" ? t.p.gyaml : t.name;
/** @param {Target} t @returns {V3 | null} */
const targetPos = t => t.kind === "placement" ? pos3(t.p) : actorPos(t.ptr);

/** @param {Target} t */
function targetAlive(t) {
    return t.kind === "placement" || tracker.list().some(a => a.ptr === t.ptr && a.instanceId === t.instanceId);
}

/** @typedef {{ kind: "placement", path: string, hash: string, before: PlacementEdit | undefined, after: PlacementEdit | undefined } | { kind: "actor", ptr: bigint, instanceId: number, before: V3, after: V3 }} Step */
/** @param {Target} t @returns {any} */
const capture = t => t.kind === "placement" ? snapshot(t.p) : targetPos(t);

/** @param {Target} t @param {V3} v */
function setTargetPos(t, v) {
    if (t.kind === "placement") {
        setValue(t.p, "translate", v);
        const live = actorForPlacement(t.p);
        if (live) moveActor(live.ptr, v);
    } else moveActor(t.ptr, v);
}

/** @type {Step[]} */
const undoStack = [];
/** @type {Step[]} */
const redoStack = [];

/** @param {Target} t @param {any} before */
function commit(t, before) {
    const after = capture(t);
    if (JSON.stringify(before) === JSON.stringify(after)) return;
    /** @type {Step} */
    const step = t.kind === "placement"
        ? { kind: "placement", path: t.p.path, hash: t.p.hash, before, after }
        : { kind: "actor", ptr: t.ptr, instanceId: t.instanceId, before, after };
    undoStack.push(step);
    if (undoStack.length > 200) undoStack.shift();
    redoStack.length = 0;
    if (t.kind === "placement") { saveEdits(); applyDirty(); }
    rebuildPanel();
}

/** @param {boolean} redo */
function history(redo) {
    const from = redo ? redoStack : undoStack, to = redo ? undoStack : redoStack;
    const step = from.pop();
    if (!step) return;
    to.push(step);
    if (step.kind === "placement") {
        writeEntry(step.path, step.hash, redo ? step.after : step.before);
        saveEdits();
        applyDirty();
        const p = placements.find(q => q.path === step.path && q.hash === step.hash);
        if (p) {
            selected = placementTarget(p);
            const live = actorForPlacement(p);
            if (live) moveActor(live.ptr, pos3(p));
        }
    } else {
        const v = redo ? step.after : step.before;
        if (v) moveActor(step.ptr, v);
        const a = tracker.list().find(x => x.ptr === step.ptr);
        if (a) selected = actorTarget(a);
    }
    ui.toast(redo ? "Redo" : "Undo", { durationMs: 800 });
    rebuildPanel();
}

// ---------------------------------------------------------------------------
// Input

/** @type {Record<string, boolean>} */
const prevKeys = {};
/** @param {string} k */
const down = k => game.input.isKeyDown(k);
/** @param {string} k */
function pressed(k) {
    const d = down(k), was = prevKeys[k];
    prevKeys[k] = d;
    return d && !was;
}

// ---------------------------------------------------------------------------
// Editor state and frame loop

let active = false;
/** @type {Target | null} */
let selected = null;
/** @type {Target | null} */
let hovered = null;
/** @type {null | { mode: "plane" | "axis" | "vertical", axis: number, t: Target, start: V3, m0: {x:number,y:number}, grab: V3, before: any, screenAxis: number[], worldLen: number }} */
let drag = null;
/** @type {Mouse | null} */
let lastMouse = null;
let lastTime = now();
let toggleHeld = false;
let tick = 0;

function enter() {
    if (game.world.isLoading()) return;
    active = true;
    loadScene();
    startCameraFromGame();
    pushCamera();
    game.input.setBlockGameInput(true);
    panel.setOpen(true);
    rebuildPanel();
    ui.toast(`Scene editor: ${placements.length} placements, ${liveActors().length} live actors`, { color: "#ffd866" });
}

function exit() {
    if (!active) return;
    active = false;
    drag = null;
    applyDirty();
    game.camera.setOverride(null);
    game.input.setBlockGameInput(false);
    panel.setOpen(false);
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
    if (!ctrl || drag) { // Ctrl+Z / Ctrl+Y should not fly
        if (down("W")) move = add(move, b.forward);
        if (down("S")) move = sub(move, b.forward);
        if (down("D")) move = add(move, b.right);
        if (down("A")) move = sub(move, b.right);
    }
    if (down("E")) move = add(move, [0, 1, 0]);
    if (down("Q")) move = sub(move, [0, 1, 0]);
    if (len(move) > 0) cam.eye = add(cam.eye, mul(norm(move), speed));
}

/** @param {Target} t @param {Basis} b @param {Screen} scr */
function gizmo(t, b, scr) {
    const p = targetPos(t);
    if (!p) return null;
    const c = project(p, b, scr);
    if (!c) return null;
    const worldLen = Math.max(0.2, c.depth * 0.12);
    return { p, c, worldLen, ends: AXES.map(a => project(add(p, mul(a, worldLen)), b, scr)) };
}

/** @param {{x:number,y:number}} m @param {number} y @param {Basis} b @param {Screen} scr @returns {V3 | null} */
function groundHit(m, y, b, scr) {
    const r = mouseRay(m.x, m.y, b, scr);
    if (Math.abs(r[1]) < 1e-4) return null;
    const k = (y - cam.eye[1]) / r[1];
    return k > 0 ? add(cam.eye, mul(r, k)) : null;
}

/** @param {Mouse} m @param {Basis} b */
function beginDrag(m, b) {
    if (selected) {
        const g = gizmo(selected, b, m.scr);
        if (g) {
            const base = { t: selected, start: g.p, m0: { x: m.x, y: m.y }, before: capture(selected), worldLen: g.worldLen };
            for (let i = 0; i < 3; i++) {
                const e = g.ends[i];
                if (e && Math.hypot(e.x - m.x, e.y - m.y) <= HANDLE_RADIUS + 4) {
                    drag = { ...base, mode: "axis", axis: i, grab: [0, 0, 0], screenAxis: [e.x - g.c.x, e.y - g.c.y] };
                    return;
                }
            }
            if (Math.hypot(g.c.x - m.x, g.c.y - m.y) <= HANDLE_RADIUS + 4) {
                const up = g.ends[1];
                drag = { ...base, mode: down("Alt") ? "vertical" : "plane", axis: 1,
                         grab: groundHit(m, g.p[1], b, m.scr) ?? g.p,
                         screenAxis: up ? [up.x - g.c.x, up.y - g.c.y] : [0, -1] };
                return;
            }
        }
    }
    if (hovered) { selected = hovered; rebuildPanel(); }
}

const snap = (/** @type {number} */ v) => down("Ctrl") ? Math.round(v / SNAP) * SNAP : v;

/** @param {Mouse} m @param {Basis} b */
function updateDrag(m, b) {
    if (!drag) return;
    const d = drag;
    let next;
    if (d.mode === "plane") {
        const hit = groundHit(m, d.start[1], b, m.scr);
        if (!hit) return;
        const moved = sub(hit, d.grab);
        next = [snap(d.start[0] + moved[0]), d.start[1], snap(d.start[2] + moved[2])];
    } else {
        const sa = d.screenAxis, l2 = sa[0] * sa[0] + sa[1] * sa[1];
        if (l2 < 4) return; // axis points straight at the camera
        const along = ((m.x - d.m0.x) * sa[0] + (m.y - d.m0.y) * sa[1]) / l2 * d.worldLen;
        next = [...d.start];
        next[d.axis] = snap(d.start[d.axis] + along);
    }
    setTargetPos(d.t, next);
    if (++tick % 8 === 0) rebuildPanel();
}

/** @param {number} sign */
function rotateSelected(sign) {
    if (selected?.kind !== "placement") { if (selected) ui.toast("Rotation is available for placements"); return; }
    const p = selected.p, r = value(p, "rotate");
    if (!r) { ui.toast("This placement has no rotation field"); return; }
    const before = capture(selected);
    const step = settings.degrees ? ROT_STEP_DEG : ROT_STEP_DEG * Math.PI / 180;
    const next = [...r];
    next[1] += sign * step;
    setValue(p, "rotate", next);
    commit(selected, before);
}

function resetSelected() {
    if (!selected) return;
    const before = capture(selected);
    if (selected.kind === "placement") {
        if (!isEdited(selected.p)) return;
        writeEntry(selected.p.path, selected.p.hash, undefined);
        const live = actorForPlacement(selected.p);
        if (live) moveActor(live.ptr, selected.p.fields.translate);
    } else {
        const origin = actorOrigin.get(selected.ptr);
        if (!origin) return;
        moveActor(selected.ptr, origin);
    }
    commit(selected, before);
}

/** Writes a moved actor's position into the placement it came from. */
function keepActor() {
    if (selected?.kind !== "actor") return;
    const p = placementForActor(selected.ptr), v = actorPos(selected.ptr);
    if (!p || !v) { ui.toast("No authored placement matches this actor", { color: "#ff9b6b" }); return; }
    const t = placementTarget(p), before = capture(t);
    setValue(p, "translate", v);
    commit(t, before);
    ui.toast(`Saved ${p.gyaml} at its new position`, { color: "#5ce67a" });
}

function frameSelected() {
    const p = selected && targetPos(selected);
    if (p) cam.eye = sub(p, mul(basis().forward, 6));
}

game.onFrame(() => {
    if (down(TOGGLE_KEY)) {
        if (!toggleHeld) { toggleHeld = true; active ? exit() : enter(); }
    } else toggleHeld = false;

    // Hold moved actors in place against their own movement, a few at a time.
    if (settings.holdActors && heldActors.size && tick % 4 === 0) {
        const alive = new Set(tracker.list().map(a => a.ptr));
        for (const [ptr, v] of heldActors) {
            if (!alive.has(ptr)) heldActors.delete(ptr);
            else if (!(drag?.t.kind === "actor" && drag.t.ptr === ptr)) game.actors.setPos(ptr, v[0], v[1], v[2]);
        }
    }
    tick++;
    if (!active) return;

    const t = now(), dt = Math.min(0.1, Math.max(0, (t - lastTime) / 1000));
    lastTime = t;
    if (game.world.isLoading()) return;

    const m = readMouse(), b0 = basis();
    if (selected && !targetAlive(selected)) { selected = null; rebuildPanel(); }

    const ctrl = down("Ctrl");
    if (pressed("Z") && ctrl) history(false);
    if (pressed("Y") && ctrl) history(true);
    if (pressed("R")) rotateSelected(down("Shift") ? -1 : 1);
    if (pressed("F")) frameSelected();
    if (pressed("Esc")) { selected = null; rebuildPanel(); }
    if (pressed("Backspace")) resetSelected();
    if (pressed("Enter")) reloadArea();

    flyCamera(dt, m);
    pushCamera();
    const b = basis();

    /** @type {{ t: Target, s: {x:number,y:number,depth:number}, edited: boolean, dynamic: boolean }[]} */
    const shown = [];
    if (settings.placements) {
        for (const p of placements) {
            const s = project(pos3(p), b, m.scr);
            if (s && s.x > -20 && s.y > -20 && s.x < m.scr.w + 20 && s.y < m.scr.h + 20)
                shown.push({ t: placementTarget(p), s, edited: isEdited(p), dynamic: p.dynamic });
        }
        shown.sort((x, y) => x.s.depth - y.s.depth);
        shown.length = Math.min(shown.length, MAX_PLACEMENT_MARKERS);
    }
    const shownActors = [];
    if (settings.actors) {
        for (const a of liveActors()) {
            const s = project(actorPos(a.ptr) ?? vec(a.pos), b, m.scr);
            if (s && s.x > -20 && s.y > -20 && s.x < m.scr.w + 20 && s.y < m.scr.h + 20)
                shownActors.push({ t: actorTarget(a), s, edited: heldActors.has(a.ptr), dynamic: false });
        }
        shownActors.sort((x, y) => x.s.depth - y.s.depth);
        shownActors.length = Math.min(shownActors.length, MAX_ACTOR_MARKERS);
    }

    // Hover: nearest marker to the cursor; actors win ties so a live object is easy to grab.
    hovered = null;
    if (!m.overUi && !drag) {
        let best = PICK_RADIUS;
        for (const list of [shownActors, shown]) {
            for (const { t: target, s } of list) {
                const d = Math.hypot(s.x - m.x, s.y - m.y);
                if (d < best - (list === shownActors ? 0 : 2)) { best = d; hovered = target; }
            }
        }
    }

    const wasLeft = lastMouse?.left ?? false;
    if (m.left && !wasLeft && !m.overUi) beginDrag(m, b0);
    else if (m.left && drag) updateDrag(m, b);
    else if (!m.left && drag) { const d = drag; drag = null; commit(d.t, d.before); }
    lastMouse = m;

    draw(shown, shownActors, b, m);
});

// ---------------------------------------------------------------------------
// Drawing

/** Box outline around a target, scaled and turned like the placement. @param {Target} t @param {Basis} b @param {Screen} scr @param {string} color @param {number} thickness */
function outline(t, b, scr, color, thickness) {
    const p = targetPos(t);
    if (!p) return;
    let sx = 1, sy = 1, sz = 1, yaw = 0;
    if (t.kind === "placement") {
        const s = value(t.p, "scale"), r = value(t.p, "rotate");
        if (s) [sx, sy, sz] = s;
        if (r) yaw = settings.degrees ? r[1] * Math.PI / 180 : r[1];
    }
    const hx = 0.6 * settings.outline * sx, hz = 0.6 * settings.outline * sz, h = 1.8 * settings.outline * sy;
    const c = Math.cos(yaw), s = Math.sin(yaw);
    const corners = [];
    for (const y of [0, h]) for (const [x, z] of [[-hx, -hz], [hx, -hz], [hx, hz], [-hx, hz]])
        corners.push(project([p[0] + x * c + z * s, p[1] + y, p[2] - x * s + z * c], b, scr));
    const edges = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];
    for (const [i, j] of edges) {
        const a = corners[i], e = corners[j];
        if (a && e) ui.draw.line(a.x, a.y, e.x, e.y, { color, thickness });
    }
}

/** @param {any[]} shown @param {any[]} shownActors @param {Basis} b @param {Mouse} m */
function draw(shown, shownActors, b, m) {
    const scr = m.scr;
    const isSel = (/** @type {Target} */ t) => selected?.key === t.key;
    const isHov = (/** @type {Target} */ t) => hovered?.key === t.key;

    for (const { t, s, edited, dynamic } of shown) {
        if (isSel(t)) continue;
        const r = Math.max(2.5, Math.min(6, 40 / s.depth)) + (isHov(t) ? 2 : 0);
        const color = isHov(t) ? "#ffffff" : edited ? "#5ce67acc" : dynamic ? "#ffad5ccc" : "#66d9ffcc";
        ui.draw.circleFilled(s.x, s.y, r, { color });
    }
    for (const { t, s, edited } of shownActors) {
        if (isSel(t)) continue;
        const r = Math.max(4, Math.min(8, 50 / s.depth)) + (isHov(t) ? 2 : 0);
        const color = isHov(t) ? "#ffffff" : edited ? "#5ce67a" : "#ff6ad5";
        ui.draw.line(s.x - r, s.y, s.x, s.y - r, { color, thickness: 2 });
        ui.draw.line(s.x, s.y - r, s.x + r, s.y, { color, thickness: 2 });
        ui.draw.line(s.x + r, s.y, s.x, s.y + r, { color, thickness: 2 });
        ui.draw.line(s.x, s.y + r, s.x - r, s.y, { color, thickness: 2 });
    }

    if (hovered && !isSel(hovered)) {
        outline(hovered, b, scr, "#ffffffc0", 1.5);
        const p = targetPos(hovered), s = p && project(p, b, scr);
        if (s && settings.labels) ui.draw.text(s.x + 12, s.y - 9, targetName(hovered), { color: "#ffffff", bgColor: "#000000b0", size: 14 });
    }

    if (selected) {
        outline(selected, b, scr, "#000000a0", 4.5);
        outline(selected, b, scr, "#ffd866", 2);
        const g = gizmo(selected, b, scr);
        if (g) {
            const hot = drag?.t.key === selected.key ? drag.mode : null;
            g.ends.forEach((e, i) => {
                if (!e) return;
                const on = hot === "axis" && drag?.axis === i;
                ui.draw.line(g.c.x, g.c.y, e.x, e.y, { color: AXIS_COLORS[i], thickness: on ? 4 : 2.5 });
                ui.draw.circleFilled(e.x, e.y, HANDLE_RADIUS * (on ? 1.2 : 0.8), { color: AXIS_COLORS[i] });
            });
            ui.draw.circleFilled(g.c.x, g.c.y, HANDLE_RADIUS, { color: hot === "plane" || hot === "vertical" ? "#ffffff" : "#ffd866" });
            ui.draw.circle(g.c.x, g.c.y, HANDLE_RADIUS + 3, { color: "#000000aa", thickness: 1.5 });
            if (settings.labels) {
                ui.draw.text(g.c.x + 14, g.c.y + 8, `${targetName(selected)}\n${g.p.map(n => n.toFixed(2)).join(", ")}`,
                             { color: "#ffd866", bgColor: "#000000b0", size: 14 });
            }
        }
    }

    if (!m.overUi) ui.draw.circle(m.x, m.y, 4, { color: "#ffffffaa", thickness: 1 });
    ui.draw.text(16, scr.h - 28,
        "RMB+mouse look | WASD/QE fly | Shift fast | click select | drag handles | R rotate | Ctrl+Z undo | Enter apply+reload | F7 exit",
        { color: "#ffffffd0", bgColor: "#00000090", size: 14 });
}

// ---------------------------------------------------------------------------
// Side panel

const panel = ui.window("scene_editor", "Scene Editor", { x: 24, y: 90, w: 380, open: false, onClose: () => exit() });

const fmt = (/** @type {number[] | null | undefined} */ v, deg = false) =>
    v ? v.map(n => (deg ? n * 180 / Math.PI : n).toFixed(2)).join(", ") : "-";

function rebuildPanel() {
    if (!active) return;
    panel.clear();
    panel.label(`${mapId ?? "(no map)"}  |  ${placements.length} placements, ${liveActors().length} actors`, "#ffd866");
    if (loadError) panel.label(loadError, "#ff6b6b");

    panel.separator();
    if (selected?.kind === "placement") {
        const p = selected.p;
        panel.label(`${p.gyaml}  (placement)`, isEdited(p) ? "#5ce67a" : "#66d9ff")
             .label(`${p.file}  #${p.index}`, "#a0a0a0")
             .label(`Position  ${fmt(value(p, "translate"))}`)
             .label(`Rotate${settings.degrees ? "" : " (deg)"}  ${fmt(value(p, "rotate"), !settings.degrees)}`)
             .label(`Scale  ${fmt(value(p, "scale"))}`)
             .button("rot_l", "Rotate -15", () => rotateSelected(-1)).sameLine()
             .button("rot_r", "Rotate +15", () => rotateSelected(1)).sameLine()
             .button("focus", "Focus (F)", frameSelected)
             .button("to_link", "Move to Link", () => {
                 const pos = game.player.getPos();
                 if (!pos || !selected) return;
                 const before = capture(selected);
                 setTargetPos(selected, vec(pos));
                 commit(selected, before);
             }).sameLine()
             .button("reset", "Reset", resetSelected);
        const scale = value(p, "scale");
        if (scale) {
            panel.slider(`scale_${p.hash}`, "Uniform scale", 0.1, 5, scale[0], v => {
                if (selected?.kind !== "placement") return;
                const before = capture(selected);
                setValue(selected.p, "scale", [v, v, v]);
                commit(selected, before);
            });
        }
    } else if (selected?.kind === "actor") {
        const match = placementForActor(selected.ptr);
        panel.label(`${selected.name}  (live actor)`, "#ff6ad5")
             .label(`Position  ${fmt(actorPos(selected.ptr))}`)
             .label(match ? `Spawned by ${match.gyaml} in ${match.file}` : "No authored placement matches this actor.", "#a0a0a0")
             .label("Actor moves last until the area reloads.", "#808080")
             .button("keep", "Keep (save to placement)", keepActor).sameLine()
             .button("focus", "Focus (F)", frameSelected)
             .button("to_link", "Move to Link", () => {
                 const pos = game.player.getPos();
                 if (!pos || !selected) return;
                 const before = capture(selected);
                 setTargetPos(selected, vec(pos));
                 commit(selected, before);
             }).sameLine()
             .button("reset", "Reset", resetSelected);
    } else {
        panel.label("Click an object in the world to select it.", "#a0a0a0")
             .label("Dots: placements (blue static, orange dynamic). Diamonds: live actors. Green: edited.", "#808080");
    }

    panel.separator()
         .button("undo", `Undo (${undoStack.length})`, () => history(false)).sameLine()
         .button("redo", `Redo (${redoStack.length})`, () => history(true)).sameLine()
         .button("apply", "Apply + reload", reloadArea)
         .button("link_here", "Bring Link to camera", () => game.player.setPos(cam.eye[0], cam.eye[1], cam.eye[2])).sameLine()
         .button("revert_area", "Revert area", () => {
             for (const p of placements) if (isEdited(p)) writeEntry(p.path, p.hash, undefined);
             undoStack.length = 0; redoStack.length = 0;
             heldActors.clear();
             saveEdits(); applyDirty(); rebuildPanel();
             ui.toast("Reverted every edit in this area");
         })
         .separator()
         .slider("speed", "Fly speed", 1, 60, settings.speed, v => { settings.speed = v; saveSettings(); })
         .slider("sens", "Mouse sensitivity", 0.05, 1, settings.sensitivity, v => { settings.sensitivity = v; saveSettings(); })
         .slider("fov", "Marker FOV", 30, 90, settings.fov, v => { settings.fov = v; saveSettings(); })
         .slider("outline", "Outline size", 0.25, 6, settings.outline, v => { settings.outline = v; saveSettings(); })
         .checkbox("show_p", "Placements", settings.placements, v => { settings.placements = v; saveSettings(); }).sameLine()
         .checkbox("show_a", "Actors", settings.actors, v => { settings.actors = v; saveSettings(); }).sameLine()
         .checkbox("labels", "Labels", settings.labels, v => { settings.labels = v; saveSettings(); })
         .checkbox("hold", "Hold moved actors", settings.holdActors, v => { settings.holdActors = v; saveSettings(); }).sameLine()
         .checkbox("degrees", "Rotation in degrees", settings.degrees, v => { settings.degrees = v; saveSettings(); rebuildPanel(); })
         .label("If markers drift from objects as you turn, adjust Marker FOV.", "#808080");
}

// ---------------------------------------------------------------------------
// Lifecycle

for (const path of Object.keys(edits)) dirtyFiles.add(path);
applyDirty();

const offMap = game.onMapChange(() => {
    actorOrigin.clear();
    heldActors.clear();
    undoStack.length = 0; redoStack.length = 0;
    if (!active) return;
    (async () => {
        while (game.world.isLoading()) await nextFrame();
        if (!active) return;
        loadScene();
        rebuildPanel();
    })();
});
const offActors = game.onActorUpdate(a => {
    if (!actorOrigin.has(a.ptr) && a.pos) actorOrigin.set(a.ptr, vec(a.pos));
});
game.onUnload(() => { offMap(); offActors(); tracker.stop(); exit(); });
console.info(`Scene editor loaded. Press ${TOGGLE_KEY} in game to open it.`);
