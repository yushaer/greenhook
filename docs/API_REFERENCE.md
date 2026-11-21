# Greenhook JavaScript API Reference

This reference mirrors [`scripts_sdk/greenhook.d.ts`](../scripts_sdk/greenhook.d.ts) and the underlying C runtime implementation in [`host/`](../host/).

---

## Script Discovery & Modules

### Locations & Load Order
1. **Top-level scripts** in `<GameDir>\scripts\*.js` (or directory overridden by `HOS_JS_DIR`). Files starting with `.`, `_`, or `-` are skipped; subfolders `lib`, `shared`, and `node_modules` are ignored as libraries.
2. **Folder mods** in `<GameDir>\scripts\<mod>\`:
   - Optional `mod.json`:
     ```json
     {
       "name": "My Mod",
       "version": "1.0.0",
       "author": "Author",
       "description": "What it does",
       "entry": "main.js",
       "enabled": true
     }
     ```
   - If `"entry"` is omitted, looks for `main.js`, then `index.js`.
3. **Packaged RomFS/content mods** under `<GameDir>\mods\<ModName>\scripts\` (loaded automatically unless `<ModName>\.disabled` exists; script IDs are prefixed `mod:<ModName>/`).
4. Disabled state toggles in the `F9` UI persist to `<scriptsDir>\scripts.json`.

### Importing Namespaces (ES2023 Modules + Globals)
The core namespaces are available as globals and as ES module exports. `Greenhook` is the scripting runtime. Existing `Longshot` globals and `longshot` imports remain available.

```js
import { Greenhook, game, ui, memory, native, fs, events } from "greenhook";
// Or import individual namespaces:
import game from "greenhook/game";
import ui from "greenhook/ui";
```

Relative `.js` / `.mjs` imports and `.json` imports are allowed **strictly inside** `scripts/` and active `mods/*/scripts/` folders (directory traversal outside script roots is blocked).

---

## Global Runtime & Async Helpers

### `Greenhook`
- `Greenhook.version` (`string`), `Greenhook.gameBuild` (`string`)
- `Greenhook.scriptsDir` (`string`), `Greenhook.dataDir` (`string`)
- `Greenhook.reload(): boolean` — Schedules a clean hot-reload after current JS execution returns.
- `Greenhook.scripts(): ScriptMetadata[]` — Returns `{ id, name, version, author, description, path, enabled, loaded, error }[]`.
- `Greenhook.setScriptEnabled(id, enabled): boolean`
- `Greenhook.stats(): RuntimeStats` — `{ memoryUsedBytes, mallocSize, objectCount, atomCount }`.

### Timers, Promises & Encoding
- `setTimeout(fn, delayMs?)`, `clearTimeout(id)`
- `setInterval(fn, intervalMs)`, `clearInterval(id)`
- `nextFrame(): Promise<FrameEvent>` — Resolves on the next `hos_vi_native_presented` tick with `{ frame, dt, fps }`.
- `waitFrames(count: number): Promise<void>`
- `sleep(ms: number): Promise<void>`
- `now(): number` — Monotonic milliseconds.
- `new TextEncoder()`, `new TextDecoder()`

---

## `game` Namespace

### `game.player`
- Position & facing: `getPos(): Vec3 | null`, `setPos(x, y, z): boolean`, `getFacing(): number | null`, `setFacing(radians): boolean`
- Vitals & age: `getLife(): number`, `setLife(quarterHearts)`, `getMaxLife()`, `setMaxLife(quarterHearts)`, `getRupees()`, `setRupees(amount)`, `getAge(): "child" | "adult" | null`, `setAge("child" | "adult")`
- Physics & combat cheats (pass `null` to restore host option default):
  - `getSpeedScale()` / `setSpeedScale(scale)`
  - `getInvincible()` / `setInvincible(enabled | null)`
  - `getDamageMultiplier()` / `setDamageMultiplier(mult)`
  - `getFlight()` / `setFlight(enabled | null)`
- Inspection: `getPrompt(): string`, `getPointers(): { actor, player, components, controller }` (`bigint` addresses).

### `game.inventory`
- `list(): InventoryItemEntry[]` (`{ label, name, kind, count }`)
- `get(itemName): number`, `has(itemName): boolean`
- `add(itemName, amount = 1): boolean`, `remove(itemName, amount = 1): boolean`, `set(itemName, targetCount): boolean`

### `game.flags`
Operates on OoT GameDataMgr flags by name (hashed automatically with **MurmurHash3 x86-32, seed 0**) or numeric hash:
- `hash(name): number`
- Typed readers/writers (`nameOrHash: string | number`):
  - `get(nameOrHash)`
  - `getBool` / `setBool(nameOrHash, value, persist = true)`
  - `getInt` / `setInt`, `getUInt` / `setUInt`, `getFloat` / `setFloat`, `getEnum` / `setEnum`
  - `getString` / `setString`; the separate `getBool64(id)` / `setBool64(id, value)` take a placement ID (use a bigint literal for full precision).
- Reactive watcher: `watch(nameOrHash, (newVal, oldVal, key) => void): Unsubscribable`

### `game.world`
- `getMap(): string | null`, `isLoading(): boolean`, `isEventPlaying(): boolean`, `getMusic(): string`
- `getTime(): { totalSeconds, seconds, hour, minute, formatted } | null`, `setTime(hourOrSeconds, minute?)` (2-arg form treats args as `(hour, minute)`).
- `destinations(): { index, child, adult }[]`
- `teleport(target)` — Accepts preset destination `index`, substring/name match, or `{ banc, startPos?, bootEvent? }`.
- `triggerEvent(eventName, starterActorPtr?): boolean`
- `getSingletons(): { gameDataMgr, eventMgr, worldMgr, cameraMgr, actorMgr, pouchMgr }`

### `game.camera`
- `getPose(): { eye, target, matrix, yaw, pitch } | null`
- `setOverride(pose | null)` — `{ matrix? }` or `{ eye?, target?, yaw?, pitch? }`.
- `setPlayerAngles(yawDeg, pitchDeg)`, `clearPlayerAngles()`
- `getFovScale()` / `setFovScale(scale)`, `getSpeedScale()` / `setSpeedScale(scale)`, `setDisableCentering(disable | null)`
- `worldToScreen(x, y, z, fovDeg?): { x, y, depth, visible } | null`

### `game.actors`
- `getPlayer(): ActorSnapshot | null`
- `inspect(ptr: AddressLike): ActorSnapshot | null` (`{ ptr, vtable, name?, components, pos, facing? }`)
- `setPos(ptr: AddressLike, x, y, z): boolean`

### `game.input`
- `game.input.Buttons` — Enum bitmasks for `"A"`, `"B"`, `"X"`, `"Y"`, `"LS"`, `"RS"`, `"L"`, `"R"`, `"ZL"`, `"ZR"`, `"PLUS"`, `"MINUS"`, `"LEFT"`, `"UP"`, `"RIGHT"`, `"DOWN"`.
- `getState(): { buttons, rawButtons, justPressed, justReleased, lx, ly, rx, ry }`
- `isDown(btn)`, `justPressed(btn)`, `justReleased(btn)`, `isKeyDown(keyOrVk)`, `getMouse(): { x, y, width, height, buttons }`
- `setOverride({ buttons?, mask?, blockGameInput?, lx?, ly?, rx?, ry? } | null)`, `clearOverride()`, `setBlockGameInput(block)`

### `game.audio` & `game.graphics`
- Audio: `getMusic()`, `setVolumeScale(scale)` / `getVolumeScale()`, `setPitchScale(scale)` / `getPitchScale()`, `setMute(muted)` / `isMuted()`.
- Graphics: `getFrame()`, `getFps()`, `getWindowSize()`, `getOption(keyOrId)`, `setOption(key, value)`, `listOptions()`, `setTextSpeedScale(scale)` / `getTextSpeedScale()`, `setTextSizeScale(scale)` / `getTextSizeScale()`, `setResolutionScale(percent)`, `takeScreenshot(relPath?): Promise<{ ok, present, path }>`.

### `game.romfs`
- `setVirtualFile(romPath, stringOrBytes)` — Serves an in-memory virtual file (`ReadFile`, `GetFileSize`, `GetEntryType`, `CloseFile`) over `rom:/<romPath>`.
- `redirect(romPath, hostFilePath)` — Redirects `host_path` resolution to any host file.
- `clearOverride(romPath)`, `clearAllOverrides()`, `exists(romPath)`, `readFile(romPath): Uint8Array | null`, `listDir(romDir?)`.

### `game.on*` Lifecycle Events
Each registration returns a function. Save it and call it to unsubscribe.
- `game.onFrame(({ frame, dt, fps }) => ...)`
- `game.onPlayerUpdate((ev) => ...)` — Runs on Link's update with live `{ actor, player, components, controller, pos, facing, life, maxLife, rupees }` + guest CPU `Ctx` active (`native.call()` allowed!).
- `game.onDamage((ev) => { ev.damage = ...; ev.cancel = true; })`
- `game.onCameraUpdate(({ view, eye, target }) => ...)`
- `game.onInput((ev) => { ev.buttons = ...; ev.lx = ...; ev.blockGameInput = true; })`
- `game.onEventRequest(({ name, arg }) => ...)`
- `game.onTimelineStart((ev) => { ev.cancel = true; })`
- `game.onSaveRequest((ev) => { ev.cancel = true; })`
- `game.onMapChange((map, prevMap) => ...)`
- `game.onMenuTick(({ screen, vtable }) => ...)`
- `game.onMenuFocus(({ screen, pane, lr }) => ...)`
- `game.onBgmChange((music, prevMusic) => ...)`
- `game.onUnload(() => ...)`
- `game.defer(fn)` — Returns a promise for the result of `fn()`. It runs immediately in a live guest context, otherwise on the next player update. Callback exceptions reject the promise.

---

## `ui` Namespace (Dear ImGui HUD, Toasts & Mod Windows)

Color inputs accept `"#RGB"`, `"#RGBA"`, `"#RRGGBB"`, `"#RRGGBBAA"`, `"rgb(...)"`, `"rgba(...)"`, packed numbers, or `[r, g, b]` / `[r, g, b, a]` arrays.

### `ui.draw` & `ui.toast`
- `ui.draw.text(x, y, text, { color?, bgColor?, size?, scale? })`
- `ui.draw.rect(x, y, w, h, { color?, rounding?, thickness? })`
- `ui.draw.rectFilled(x, y, w, h, { color?, rounding? })`
- `ui.draw.line(x1, y1, x2, y2, { color?, thickness? })`
- `ui.draw.circle(cx, cy, radius, { color?, thickness? })`
- `ui.draw.circleFilled(cx, cy, radius, { color? })`
- `ui.draw.progressBar(x, y, w, h, fraction, { color?, bgColor?, text? })`
- `ui.draw.clear()`
- `ui.toast(text, { color?, durationMs? })`

### `ui.window(id, title, opts?)`
Creates persistent interactive Dear ImGui panels (automatically captures mouse cursor while open):
- Chainable handle methods: `setOpen(bool)`, `isOpen()`, `close()`, `clear()`, `label(text, color?)`, `separator()`, `sameLine()`, `button(id, label, onClick?)`, `checkbox(id, label, initial, onChange?)`, `slider(id, label, min, max, initial, onChange?)`, `progressBar(fraction, label?)`.

### `ui.console`
- `ui.console.open()`, `close()`, `toggle(): boolean`, `isOpen(): boolean`, `clear()`.

---

## `memory`, `native`, `fs`, and `events`

### `memory`
Every access checks `addr >= 0x100000` and `hos_mem_region(addr, size)` and notifies renderer write-tracking (`hos_mark_written`):
- Typed accessors: `readU8/S8/U16/S16/U32/S32/U64/S64/F32/F64` and matching `write*`. (`U64`/`S64` return/accept `bigint`).
- Strings & buffers: `readBytes(addr, len): Uint8Array`, `writeBytes(addr, data)`, `readCString(addr, maxLen?)`, `writeCString(addr, str)`.
- Utilities: `isMapped(addr, size?)`, `scan(startAddr, len, patternHex): bigint[]` (e.g. `"DE AD ?? EF"`), `alloc(bytes): bigint` (4 MB scratch guest arena), `free(addr)`.

### `native`
- `native.hasFunction(addr): boolean`
- `native.call(addr, { x?, f?, d? })` — Must run during a live `GuestContext` (such as `game.onPlayerUpdate` or `game.defer`). Returns `{ x0, x1, x2, x3, f0, d0 }`.
- `native.hook(addr, { before?: (ctx) => boolean | void, after?: (ctx) => void }): number` — Returning `false` from `before` skips invoking the original function. `ctx` exposes `x0..x7`, `lr`, `sp`, and `getX`/`setX`/`getF`/`setF`/`getD`/`setD`.
- `native.unhook(handleOrAddr): boolean`
- `native.createCallback(fn): bigint` — Allocates up to 32 synthetic guest function addresses (`0xFF00000000+`) dispatchable via `a64_lookup`.
- `native.addresses` — Built-in constants for key OoT addresses (`GameDataMgrSlot`, `EventMgrSlot`, `WorldMgrSlot`, `CameraMgrSlot`, `ActorMgrSlot`, `PouchMgrSlot`, `PouchSingletonSlot`, `GetBool`, `SetBoolPersist`, `GetString64`, `RequestEvent`, `EventRequestArgCtor`, `ActorLinkSet`, `ActorLinkRelease`, `SetTime`, `GetTime`, `PouchAddRemove`, `PouchCount`, `ChangePosition`).

### `fs` (Sandboxed Mod Storage)
Restricted strictly to `scripts_data/` (`HOS_JS_DATA_DIR`). Paths containing `..`, drive letters, or escaping `scripts_data/` throw `RangeError`:
- `readText`, `writeText`, `readBytes`, `writeBytes`, `readJson`, `writeJson`, `exists`, `remove`, `mkdir`, `listDir`.

### `events`
Inter-mod publish/subscribe bus: `on(name, cb)`, `once(name, cb)`, `off(name, cb)`, `emit(name, ...args)`.

## Looking up gameplay state

| Method | When to use it | Result |
| --- | --- | --- |
| `game.player.getProgress()` | Read Longshot's latest player-stat sample | `{life, maxLife, rupees, skulltulaTokens, heartPieces}`, or `null` before a sample. Individual unknown values are `null`. Life is measured in quarter hearts. Heart pieces count all pieces found. |
| `game.world.getAreaName(map?)` | Turn a map identifier into an English display name | String or `null`. Omitting the argument uses the current map. Use the original ID for teleports and comparisons. |
| `game.flags.list(kind, cursor = 0)` | Discover the hashes available in the active save's typed flag table | `{hashes: number[], next: number \| null}` or `null` when unavailable. Kinds are `bool`, `int`, `uint`, `float`, `enum`, and `string`. Each page scans up to 128 hash slots and can be empty. Continue until `next === null`. Restart after loading a different save. |
| `game.actors.getMethods(actor, firstSlot, count)` | Inspect a **known** virtual-table span on a live actor | Array of guest addresses, one per slot, with `null` for unresolved entries. Returns `null` for unreadable storage. Request 1–128 slots. Obtain the class layout before choosing the range. Adjacent tables can contain callable addresses too. |

```js
import { game } from "greenhook";

const stop = game.flags.watch("PlayerStatus.Life", (life, previous) => {
    console.log("Quarter hearts", previous, "→", life);
});
game.onUnload(stop);
```

The watcher establishes its baseline on a player update. Numeric hashes from `list()` can go straight into typed getters, setters, and watchers. For a Boolean identified by a placed actor's 64-bit ID, use `getBool64(id)`. Ordinary named flags use a separate 32-bit key space. Typed getters return `null` when a flag is unavailable. Direct setters change the running game. Normal game save timing determines when changes reach disk.

Calls such as `setPos`, `setAge`, `inventory.add/remove/set`, `setTime`, `teleport`, and `triggerEvent` may enqueue work for a player update. The shared queue holds 64 requests. A queued call returns `true` on acceptance and `false` when full. To observe an immediate game-side result, await `game.defer(() => game.inventory.set(item, count))`. Inventory counts supplied to `set` are absolute. If no player update occurs, deferred work waits.

## Reading game databases, maps, and packs

Import the following functions from `greenhook/data`. The installer puts this SDK module in `scripts/lib/greenhook-data.js`. It reads the current installation's RomFS, including file overrides. No extracted game database ships with Greenhook.

| Method | Result |
| --- | --- |
| `listTables()` | Sorted RSDB filenames. Pick by table prefix to avoid assuming a product revision. |
| `openTable(filename)` | Root BYML node for an RSDB filename, or `null` for a missing file. ActorInfo, GameActorInfo, PouchActorInfo, Challenge, DungeonParam, GameBancInfo, Location, EventSetting, and UIScreen are examples. |
| `readResource(path)` | `Uint8Array` or `null`. Reads plain files directly. For `.zs`, it uses the frame's dictionary ID to select the dictionary from `Pack/ZsDic.pack.zs`. |
| `openByml(bytes)` | Lazy array or map node with `kind`, `length`, `get(key)`, and `entries()`. Missing keys return `undefined`. Values include nested nodes, strings, numbers, booleans, bigint for 64-bit integers, binary views, and null. Both byte orders work. Unsupported types and malformed extents throw. |
| `openPack(bytes)` | Map of SARC entry names to byte views. Unnamed entries use numeric hashes. Use `openByml` on a BYML entry to inspect it. |
| `game.romfs.decompressZstd(bytes, dictionary?)` | Decode one frame with declared size up to 32 MiB. Malformed input, unknown sizes, memory limits, or a mismatched dictionary raise exceptions. |

```js
import { listTables, openTable, readResource, openByml } from "greenhook/data";

const filename = listTables().find(name => name.startsWith("ActorInfo."));
if (filename) {
    const firstActor = openTable(filename)?.get(0);
    if (firstActor) console.log(firstActor.get("__RowId"), firstActor.get("ClassName"));
}

const bytes = readResource("Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs");
if (bytes) {
    const placements = openByml(bytes).get("Actors");
    for (const [, placement] of placements.entries()) {
        console.log(placement.get("Gyaml"), placement.get("Hash").toString(16));
        break;
    }
}
```

Use `game.romfs.listDir("GameData")` to find the installed GameDataList, then open it with `openByml(readResource(path))`. Its root exposes `Data` and `MetaData`. Map placements describe authored content. Use `onPlayerUpdate` for live pointers and `game.actors.inspect` to inspect a known live instance. Resource views retain their decompressed bytes. Open a table once per task and keep frame callbacks small.

## Calling translated methods

`native.listFunctions(start = 0, limit = 128)` reads the sorted guest function address table from the current stock executable. It returns `{addresses: bigint[], total: number, next: number | null}`, or `null` if the table is unavailable. The limit is 1–1024. Indexes are table positions. The values in `addresses` are the guest addresses accepted by `native.hasFunction`, `native.call`, and `native.hook`. Format an address as `"f_" + address.toString(16)` to find its translated C function.

```js
import { game } from "greenhook";

const stop = game.onPlayerUpdate(({ actor }) => {
    // Inspect the first slot only. Access further slots using the class layout.
    const first = game.actors.getMethods(actor, 0, 1)?.[0];
    if (first) console.log("First virtual method", "f_" + first.toString(16));
    stop();
});
```

Before invoking a discovered method, determine its receiver, argument registers, pointer lifetimes, and return type from its translated implementation and callers. Integer and pointer arguments go into `x`, single-precision floats into `f`, and doubles into `d`. A member method commonly expects the receiver in `x[0]`. Use `game.defer` to obtain a live guest context. The dispatch table contains no C++ names, prototypes, or ownership information. Passing an incorrect receiver or argument can crash the game.

### Verified native calling conventions in the 3.0.0 game image

| Address and method | Guest inputs | Result or effect |
| --- | --- | --- |
| `0x83472280 GetTime` | World manager in x0, writable 24-byte result in x8 | Total seconds are a u64 at result + 8. Use `game.world.getTime().totalSeconds` for ordinary reads. |
| `0x834722d0 SetTime` | World manager in x0, seconds in w1 | The function loads the time manager at world + 0x510 and delegates the change. Use `game.world.setTime(hour, minute)` to schedule it from a frame callback. |
| `0x805b63e0 PouchCount` | Pouch manager in x0, address of a guest pointer to the item name in x1, zero in x2 and x3 for an ordinary count | Count in w0. `game.inventory.get(name)` handles the guest string, song flags and supported aliases. |
| `0x806a2b20 SetBoolPersist` | GameData manager in x0, Boolean in w1, flag hash in w2 | Updates the specified Boolean. Use `game.flags.setBool(name, value, true)`. |
| `0x806d2530 ChangePosition` | Player controller in x0 and pointer to three f32 coordinates in x1 | Moves Link through the controller. Use `game.player.setPos(x, y, z)`. |
| `0x80d9a820 RequestEvent` | Event manager in x0 and constructed event-request object in x1 | Submits the request. `game.world.triggerEvent(name, actor?)` handles construction and actor-link lifetime. |

`native.call(addr, {x, f, d, indirectResult})` accepts up to eight positional values per register bank. Omitted registers retain the live context value. `indirectResult` supplies the x8 guest address for structure returns. Allocate the size and alignment required by the specific method. Greenhook requires a mapped 24-byte buffer for GetTime. The native call restores registers afterward. Writes to the guest result buffer and game state persist.

```js
import { game, memory, native } from "greenhook";

async function readClock() {
    return game.defer(() => {
        const world = game.world.getSingletons().worldMgr;
        if (!world || !game.world.getTime()) return null;
        const out = memory.alloc(24);
        try {
            native.call(native.addresses.GetTime, { x: [world], indirectResult: out });
            return memory.readU64(out + 8n);
        } finally {
            memory.free(out);
        }
    });
}
```

## Observing active actors

`game.onActorUpdate(callback)` runs at Longshot's shared actor transform update, with a live guest context. The snapshot includes `ptr`, `instanceId`, `statusCode`, `name`, `vtable`, `components`, and `pos`. The identity and raw status fields are the actor fields that the translated ActorLinkSet reads at offsets 24 and 44. Keep callbacks short. Store observations for display and reacquire live pointers before mutation.

```js
import { trackActors } from "greenhook/data";
const actors = trackActors({
    maxIdleFrames: 120,
    onEnter(actor) { console.log("Observed", actor.name, actor.instanceId); },
    onLeave(actor, reason) { console.log("Left observation", actor.instanceId, reason); }
});
// actors.list() returns the latest snapshot for each observed instance.
// actors.stop() removes its listeners. Unload also stops it.
```

An entry leaves on a map change, pointer reuse with a different instance ID, or an idle timeout in presented frames. A sleeping actor can time out and later reappear. These are observation events. A spawn request, engine destructor, offscreen actor or paused scene has different timing.

## Editing installed resources in memory

`editResource(path, edit)` reads a private copy, calls the synchronous edit function with the decompressed bytes, then registers a virtual override. It returns false when the input is missing or virtual slots are full. An exception before registration preserves the previous override. For a .zs input it emits a valid Zstandard frame with raw blocks and the same decompressed size. Already-loaded game objects may require a scene or resource reload before they reflect a change. `game.romfs.clearOverride(path)` removes the override for future opens.

`node.set(key, value)` edits an existing numeric, Boolean or string field in place. Use bigint for 64-bit integers. A string replacement must already appear in that file's string table. `openPack` returns mutable byte views, so editing a BYML member also changes its parent pack's bytes. Avoid editing aliased BYML data unless all references should see the change. Container insertion, new strings and arbitrary resizing require an asset authoring tool.

```js
import { editResource, openByml } from "greenhook/data";

const path = "Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs";
function movePlacement(id, x) {
    return editResource(path, bytes => {
        for (const [, actor] of openByml(bytes).get("Actors").entries()) {
            if (actor.get("Hash") === id) {
                actor.get("Translate").set(0, x);
                return;
            }
        }
        throw new Error("Placement ID missing in this map");
    });
}
```

## Looking up a gameplay record by ID

`findRecord(table, rowId)` from `greenhook/data` searches a row array ordered by string `__RowId` in logarithmic time. Open the table once and reuse it. The result is a lazy row or `null`. Use `entries()` for unindexed tables, for modified tables whose order changed, or to inspect unfamiliar fields.

| System | Read this table | Useful fields in the installed dump |
| --- | --- | --- |
| Actors | ActorInfo | ClassName, CalcRadius, LoadRadius, DisplayRadius, ELinkUserName |
| Inventory | PouchActorInfo | PouchCategory, PouchTabCategory, BundleActorNum, ActionGuideType |
| Quests | Challenge | Steps contains each step's Name, Participants, DestinationPoint and ForceProgressGameData. An empty flag name is possible. |
| Dungeons | DungeonParam | RoomParam and TargetGameData |
| Maps | GameBancInfo | BancMain and SoundSceneryName |
| Events | EventSetting | EntryPointName, FlowchartFileName, EventBaseSetting and EventSoundSetting |

```js
import { listTables, openTable, findRecord } from "greenhook/data";
const file = listTables().find(s => s.startsWith("DungeonParam."));
if (file) {
    const dungeon = findRecord(openTable(file), "BottomOfTheWell");
    const hash = dungeon?.get("TargetGameData");
    if (typeof hash === "number") console.log(game.flags.get(hash));
}
```
## VS Code type hints

Install the Developer ZIP and open the installed `scripts` folder. The SDK README section **Type hints in Visual Studio Code** covers completion, separate projects, and troubleshooting.
