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
```

For individual namespaces, use their default exports.

```js
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

## `game` method reference

Import `game` from `"greenhook"`. Each section below lists methods on the named object. Signatures use the SDK types available in VS Code. Guest addresses are represented as bigints.

Gameplay actions that run at the next player update return `true` when queued and `false` when the shared 64-entry queue is full. To check an operation in a live guest context, use `await game.defer(() => game.inventory.set(item, count))`. A paused or absent player can delay that promise.

### `game.player`

- `getProgress(): { life: number | null; maxLife: number | null; rupees: number | null; skulltulaTokens: number | null; heartPieces: number | null } | null` — Read the latest sample of life, maximum life, rupees, Skulltula tokens and heart pieces. Life uses quarter hearts. An unavailable sample or individual field is null.
- `getPos(): Vec3` — Read Link’s world coordinates. Falls back to the cached position, initially (0, 0, 0). Use getPointers().actor to check whether Link has been observed.
- `setPos(x: number, y: number, z: number): boolean` — Move Link to absolute world coordinates. Executes with a live player context or queues the move for a player update.
- `getFacing(): number` — Read Link’s heading in radians from his transform, falling back to the last sample (initially zero).
- `setFacing(radians: number): boolean` — Write the heading in radians. Returns true when the runtime has an actor to update.
- `getLife(): number` — Read current life in quarter hearts (4 is one heart). Falls back to the last sample, initially -1.
- `setLife(quarterHearts: number): boolean` — Set current life in quarter hearts, clamped to a minimum of zero. Returns true if either the game-data or live health write succeeds.
- `getMaxLife(): number` — Read heart capacity in quarter hearts, or -1 before the first successful read or sample.
- `setMaxLife(quarterHearts: number): boolean` — Set heart capacity in quarter hearts, with a minimum of 4. Returns whether the capacity flag was written.
- `getRupees(): number` — Read the wallet count, falling back to the last sample, initially -1.
- `setRupees(amount: number): boolean` — Write the wallet count, clamping negative amounts to zero. Returns false if the rupee flag cannot be written.
- `getAge(): "child" | "adult" | null` — Read the age flag or last observed age. Returns null before either is available.
- `setAge(age: "child" | "adult"): boolean` — Request the child or adult form. Runs immediately with a live actor and guest context or queues the change. Unsupported names throw RangeError.
- `getSpeedScale(): number` — Read the script movement-speed multiplier.
- `setSpeedScale(scale: number): void` — Set the movement multiplier. Values clamp to 0.05–10. Use 1 for normal speed.
- `getInvincible(): boolean | null` — Read the invincibility override. Null means the host controls it.
- `setInvincible(enabled: boolean | null): void` — Force invincibility on or off. Pass null to release script control.
- `getDamageMultiplier(): number` — Read the script damage multiplier.
- `setDamageMultiplier(mult: number): void` — Scale incoming damage by a factor from 0 to 100. Use 1 for normal damage.
- `getFlight(): boolean | null` — Read the flight override. Null means the host controls it.
- `setFlight(enabled: boolean | null): void` — Force flight on or off. Pass null to release script control.
- `getPrompt(): string` — Read the current player prompt string or its last sampled value. It can be empty.
- `getPointers(): PlayerPointers` — Read the last observed actor, player, components and controller guest addresses. An unavailable address is 0n. Refresh pointers after transitions.

### `game.inventory`

- `list(): InventoryItemEntry[]` — Enumerate known items with their display label, actor name, kind (toggle or quantity) and count. A count of -1 means unavailable.
- `get(itemName: string): number` — Read an item by actor name or recognized label. Returns its count, 0 or 1 for a recognized song flag, or -1 if unavailable.
- `has(itemName: string): boolean` — Return true when the reported count exceeds zero. Use get() to distinguish an unavailable count from zero.
- `add(itemName: string, amount?: number): boolean` — Add the specified amount (default 1). Outside a live guest context, true means the change was accepted into the queue.
- `remove(itemName: string, amount?: number): boolean` — Subtract the specified amount (default 1). Outside a live guest context, true means the change was accepted into the queue.
- `set(itemName: string, targetCount: number): boolean` — Set an absolute count. Negative targets throw RangeError. A queued edit computes the required change when it executes.

### `game.flags`

- `list(kind: "bool" | "int" | "float" | "enum" | "string" | "uint", cursor?: number): { hashes: number[]; next: number | null } | null` — Scan up to 128 table slots per call for bool, int, uint, float, enum or string. Start at cursor 0 and pass back next, even after an empty page. A null next ends the scan. A null page means unavailable.
- `hash(name: string): number` — Compute the unsigned MurmurHash3 x86-32 key for a flag name using seed zero.
- `get(nameOrHash: string | number): boolean | number | string | null` — Read a named or hashed flag across supported 32-bit-key tables. Returns a boolean, number, string or null when absent.
- `getBool(nameOrHash: string | number): boolean | null` — Read an existing Boolean flag. Null means missing or unavailable.
- `setBool(nameOrHash: string | number, value: boolean, persist?: boolean): boolean` — Write a Boolean. With persist enabled (default), also invoke or queue the game’s persistent setter. Pass false as the third argument for a direct in-memory write.
- `getInt(nameOrHash: string | number): number | null` — Read an existing signed 32-bit flag, or null.
- `setInt(nameOrHash: string | number, value: number): boolean` — Write an existing signed 32-bit entry. Returns whether the write succeeded.
- `getUInt(nameOrHash: string | number): number | null` — Read an existing unsigned 32-bit flag, or null.
- `setUInt(nameOrHash: string | number, value: number): boolean` — Write an existing unsigned 32-bit entry. Returns whether the write succeeded.
- `getFloat(nameOrHash: string | number): number | null` — Read a single-precision game flag as a JavaScript number, or null.
- `setFloat(nameOrHash: string | number, value: number): boolean` — Write an existing float flag with single-precision rounding. Returns whether the write succeeded.
- `getEnum(nameOrHash: string | number): number | null` — Read the numeric enum value hash, or null when the entry is unavailable.
- `setEnum(nameOrHash: string | number, value: string | number): boolean` — Write an enum value using its string name (hashed) or numeric hash. Returns whether the existing entry was written.
- `getString(nameOrHash: string | number): string | null` — Read an existing game string from its 64-byte buffer, or null.
- `setString(nameOrHash: string | number, value: string): boolean` — Update an existing string buffer. At most 63 UTF-8 bytes fit before the terminator. Keep replacements within that limit to preserve whole characters.
- `getBool64(placementId: AddressLike): boolean | null` — Read a placement Boolean by its full 64-bit ID. Pass a bigint for exact integer precision. Returns null if absent.
- `setBool64(placementId: AddressLike, value: boolean): boolean` — Change an existing placement Boolean by ID. Returns false if its entry cannot be found or written.
- `watch(nameOrHash: string | number, callback: (newVal: boolean | number | string | null, oldVal: boolean | number | string | null, key: string | number) => void): Unsubscribable` — Observe changes after the first readable sample establishes a baseline. The callback receives new value, previous value and original key. Call the returned function to stop.

### `game.world`

- `getMap(): string | null` — Read the current map’s internal ID, or null when unavailable.
- `getAreaName(map?: string): string | null` — Look up the English area name for a map ID. Omit the ID to use the current map. Returns null when unresolved.
- `isLoading(): boolean` — Read the loading flag. Returns false when the flag is unavailable.
- `isEventPlaying(): boolean` — Test whether the event manager has a current event. Returns false if the manager cannot be read.
- `getMusic(): string` — Read the current BGM identifier from game data or its cached sample. The string may be empty.
- `getTime(): WorldTime | null` — Read totalSeconds as a bigint and seconds within the day as a number. Also returns hour, minute and formatted HH:MM. Returns null without a readable clock.
- `setTime(hourOrSeconds: number, minute?: number): boolean` — With two arguments, set hours and minutes. With one, values 0 through 24 are hours (fractions allowed), all others are seconds. The result wraps at 24 hours. May queue until a player update.
- `destinations(): DestinationEntry[]` — List teleport presets and their child/adult map IDs. Either age-specific ID can be null. Use index for an unambiguous preset.
- `teleport(target: number | string | { banc: string; startPos?: string; bootEvent?: string }): boolean` — Request a map change by index, first matching name or custom destination. Full names match case-insensitively, substrings case-sensitively. Custom defaults are Entrance and Dm_SY_BancChangeArrive. Observe onMapChange to detect arrival.
- `triggerEvent(eventName: string, starterActorPtr?: AddressLike): boolean` — Request a named game event using the supplied starter actor or the most recently sampled Link actor. May queue until a player update.
- `getSingletons(): SingletonsInfo` — Read the six game manager addresses as bigints. Check for 0n before using a manager and obtain a fresh address after transitions.

### `game.camera`

- `getPose(): CameraPose | null` — Read the active gameplay view’s eye, target, 12-number view matrix and last sampled camera angles. Returns null when the view is unavailable.
- `setOverride(pose: CameraOverridePose | null): void` — Supply a 12-number row-major view matrix, eye and target, or eye with yaw and pitch in radians. Both angles are needed in the angle form. Pass null to release the view.
- `setPlayerAngles(yawDeg: number, pitchDeg: number): void` — Override the player camera’s yaw and pitch in degrees.
- `clearPlayerAngles(): void` — Release the player camera angle override.
- `setFovScale(scale: number): void` — Set the field-of-view multiplier, clamped to 0.25–2.5.
- `getFovScale(): number` — Read the current field-of-view multiplier.
- `setSpeedScale(scale: number): void` — Set the camera-speed multiplier, clamped to 0.1–8.
- `getSpeedScale(): number` — Read the current camera-speed multiplier.
- `setDisableCentering(disable: boolean | null): void` — Pass true to suppress camera centering, false to allow it or null to use the host setting.
- `worldToScreen(x: number, y: number, z: number, fovDeg?: number): ScreenPoint | null` — Project a world position to `ui.draw` pixels (the overlay image, which can differ from the window or render size) using a vertical FOV in degrees (default 50). Returns null without a view. Positive depth is forward. The visible test allows a 200-pixel margin, so check window bounds for strict clipping.

### `game.actors`

- `getPlayer(): ActorSnapshot | null` — Read Link’s actor snapshot, or null before an actor has been observed.
- `getMethods(actor: AddressLike, firstSlot: number, count: number): (bigint | null)[] | null` — Resolve 1–128 consecutive vtable slots starting at a nonnegative integer slot. An unreadable range returns null. Individual untranslated entries are null. Slot signatures depend on the actor class and build.
- `inspect(ptr: AddressLike): (ActorSnapshot & { instanceId: number; statusCode: number; name: string }) | null` — Read a snapshot for a current actor address, or null if the header cannot be read. Obtain addresses from actor callbacks and refresh them after map changes.
- `setPos(ptr: AddressLike, x: number, y: number, z: number): boolean` — Write world coordinates to a live actor. Link uses the player movement path. Other actors receive a direct transform write which their own update can overwrite.
- `getFacing(ptr: AddressLike): number | null` — Read a live actor’s yaw in radians from its transform rotation, or null if it cannot be read. `inspect()` and `onActorUpdate` snapshots also include `facing`.
- `setFacing(ptr: AddressLike, radians: number): boolean` — Turn a live actor to a yaw in radians. The rotation becomes a pure yaw: pitch and roll are dropped and each axis keeps its scale. Link uses the player facing path. As with `setPos`, the actor’s own update can overwrite it.

### `game.input`

Use `game.input.Buttons` for numeric bitmasks. Its keys are A, B, X, Y, LS, RS, L, R, ZL, ZR, PLUS, MINUS, LEFT, UP, RIGHT and DOWN.
- `getState(): PadStateSnapshot` — Read sampled buttons, rawButtons, justPressed, justReleased and four integer stick axes. Button masks here are numbers sampled before script overrides.
- `isDown(button: ButtonName | number): boolean` — Test a button name or bitmask. For a combined mask, every selected button must be held.
- `justPressed(button: ButtonName | number): boolean` — Test whether any selected button changed from up to down between the two latest input samples.
- `justReleased(button: ButtonName | number): boolean` — Test whether any selected button changed from down to up between the two latest input samples.
- `isKeyDown(key: string | number): boolean` — Test a supported key name or Windows virtual-key code. Returns false while the game process lacks foreground focus.
- `getMouse(): MouseSnapshot` — Read cursor coordinates, window size and button bits (left 1, right 2, middle 4). Coordinates are client-relative when the game is foreground, otherwise screen-relative. `overlayX`/`overlayY` give the cursor in `ui.draw` pixels (`overlayWidth` × `overlayHeight`) for hit-testing overlay drawings, and `overUi` is true while the cursor is over a Dear ImGui window or a widget holds the mouse.
- `setOverride(spec: InputOverrideSpec | null): void` — Replace selected controller bits and stick axes on successive input polls. The button mask defaults to 0xffff. Use mask 0 for a sticks-only override. Axes clamp to ±32767. Passing null releases buttons and sticks but preserves the block setting.
- `clearOverride(): void` — Release controller button and stick overrides and clear the persistent input block.
- `setBlockGameInput(block: boolean): void` — Zero game input before script overrides on each poll while enabled. Pass false to release the block.

### `game.audio`

- `getMusic(): string` — Read the most recently observed background music identifier.
- `setVolumeScale(scale: number): void` — Set the volume multiplier, clamped to 0–4. Use 1 for normal volume.
- `getVolumeScale(): number` — Read the current volume multiplier.
- `setPitchScale(scale: number): void` — Set the pitch multiplier, clamped to 0.25–4. Use 1 for normal pitch.
- `getPitchScale(): number` — Read the current pitch multiplier.
- `setMute(muted: boolean): void` — Enable or disable the script mute switch.
- `isMuted(): boolean` — Return the script mute switch state.

### `game.graphics`

- `getFrame(): number` — Read the number of presented frames. Use differences for frame counts.
- `getFps(): number` — Read the recent measured presentation rate in frames per second. It starts at zero before sufficient samples arrive.
- `getWindowSize(): { width: number; height: number }` — Read render window width and height in pixels. Dimensions can be zero before frame information is available.
- `getOption(keyOrId: string | number): number | null` — Read an integer option value. A missing string key returns null. A numeric ID uses the host lookup with fallback -1.
- `setOption(keyOrId: string | number, value: number): boolean` — Set an integer option by key or numeric ID. A string key reports host success. A numeric ID returns true after dispatch. Use listOptions() for discoverable keys and ranges.
- `listOptions(): ControlOptionEntry[]` — Enumerate host settings with key, display name, text, tab, numeric value, min, max and step.
- `setTextSpeedScale(scale: number): void` — Set the text-speed multiplier, clamped to 0.1–100.
- `getTextSpeedScale(): number` — Read the script text-speed multiplier.
- `setTextSizeScale(scale: number): void` — Set the text-size multiplier, clamped to 0.25–4.
- `getTextSizeScale(): number` — Read the script text-size multiplier.
- `setResolutionScale(percent: number): boolean` — Set the resolution_scale option as an integer percentage. Returns whether the host accepted it.
- `takeScreenshot(relPath?: string): Promise<ScreenshotResult>` — Request a capture under scripts_data, default screenshots/shot.png. Await the result for ok, present and path. An invalid path or a second pending request throws synchronously. A rejected capture request can reject the promise.

### `game.romfs`

- `redirect(romPath: string, hostFilePath: string): boolean` — Serve a game resource from a host file on future opens. Returns whether the redirect was registered.
- `setVirtualFile(romPath: string, content: string | Uint8Array | ArrayBuffer): boolean` — Serve supplied text or bytes at a game resource path on future opens. Returns false when no override slot is available.
- `clearOverride(romPath: string): boolean` — Remove the virtual file or redirect registered at this path. Returns whether a matching entry was removed.
- `clearAllOverrides(): void` — Remove all scripts’ in-memory resource overrides and redirects. Use clearOverride(path) when cleaning up one mod.
- `exists(romPath: string): boolean` — Test for a virtual entry or a file or directory at the resolved game resource path.
- `decompressZstd(bytes: Uint8Array, dictionary?: Uint8Array): Uint8Array` — Decode one Zstandard frame to fresh bytes, optionally with its dictionary. An unknown size or output above 32 MiB throws RangeError. An invalid frame or wrong dictionary throws TypeError.
- `readFile(romPath: string): Uint8Array | null` — Read a fresh byte array, preferring a virtual file, then the resolved host file. Returns null on a missing file or an unreadable or oversized disk file (64 MiB limit).
- `listDir(romDir?: string): string[]` — List immediate child names from the resolved disk directory, or an empty array if unavailable. Omit the path for the root. Virtual-only files are absent from this listing.

### `game` lifecycle and scheduling

Every `on…` method returns a zero-argument unsubscribe function. Call it to remove that particular listener. Set mutable event fields before the callback returns.
- `defer<T>(fn: () => T): Promise<T>` — Run the callback immediately with a live guest context or at the next player update. Await its return value. A thrown callback exception rejects the promise.
- `onFrame(cb: (ev: FrameEvent) => void): Unsubscribable` — Receive frame number, elapsed seconds (dt) and measured fps on presentation. Use for overlays and frame-based timers.
- `onActorUpdate(cb: (actor: ActorSnapshot & {instanceId: number; statusCode: number}) => void): Unsubscribable` — Observe snapshots from the shared actor transform hook, including instanceId and statusCode. An actor can appear repeatedly. This hook observes updates and gives no guarantee of seeing every spawn or destruction.
- `onPlayerUpdate(cb: (ev: PlayerUpdateEvent) => void): Unsubscribable` — Receive Link’s pointers, position, facing, life, maxLife and rupees during a player update. Guest calls can run synchronously inside this callback.
- `onDamage(cb: (ev: DamageEvent) => void): Unsubscribable` — Inspect oldLife and incoming damage in quarter hearts. Assign a nonnegative integer to damage or set cancel to true before returning.
- `onCameraUpdate(cb: (ev: CameraUpdateEvent) => void): Unsubscribable` — Receive the guest view address and eye and target positions when the gameplay camera updates.
- `onInput(cb: (ev: GreenhookInputEvent) => void): Unsubscribable` — Receive raw and effective button masks as bigints and four stick axes. Modify buttons and axes synchronously to change the sample. To suppress this sample, set buttons to 0n and all four axes to zero.
- `onEventRequest(cb: (ev: EventRequestEvent) => void): Unsubscribable` — Receive the requested game event name and guest argument address. Inspect the argument during the callback.
- `onTimelineStart(cb: (ev: TimelineStartEvent) => void): Unsubscribable` — Receive the timeline name and player address. Set cancel to true synchronously to veto the start.
- `onSaveRequest(cb: (ev: SaveRequestEvent) => void): Unsubscribable` — Inspect reason, mode, caller address and isTransition. Set cancel to true synchronously to veto this save request.
- `onMapChange(cb: (map: string, prevMap: string) => void): Unsubscribable` — Receive current and previous map IDs as two positional strings. Refresh map-specific state here.
- `onMenuTick(cb: (ev: MenuTickEvent) => void): Unsubscribable` — Observe the current guest menu screen and its vtable during a menu update.
- `onMenuFocus(cb: (ev: MenuFocusEvent) => void): Unsubscribable` — Observe the guest menu screen, focused pane and caller link register when focus changes.
- `onBgmChange(cb: (music: string, prevMusic: string) => void): Unsubscribable` — Receive current and previous background music identifiers as two positional strings.
- `onUnload(cb: () => void): Unsubscribable` — Run cleanup during script unload. Release subscriptions or overrides your mod owns here.

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
- Layout scopes: `beginTabBar(id)` … `endTabBar()` holds tabs. `tab(id, label)` … `endTab()` shows its widgets only while that tab is selected. `beginChild(id, height?)` … `endChild()` makes a bordered region that scrolls (`height` in pixels, 0 fills the remaining space). Scopes nest. Unclosed scopes are closed when the window finishes drawing.
- A window holds up to 128 widgets, counting `separator()`, `sameLine()` and scope markers.

```js
const panel = ui.window("tools", "Tools", { w: 360, h: 420 });
panel.beginTabBar("tabs")
     .tab("world", "World").button("noon", "Noon", () => game.world.setTime(12, 0)).endTab()
     .tab("log", "Log").beginChild("lines", 0);
for (const line of ["first", "second", "third"]) panel.label(line);
panel.endChild().endTab().endTabBar();
```

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
- `native.call(addr, { x?, f?, d?, indirectResult? })` — Must run during a live `GuestContext` (such as `game.onPlayerUpdate` or `game.defer`). Returns `{ x0, x1, x2, x3, f0, d0 }`.
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
    if (firstActor && typeof firstActor === "object" && "kind" in firstActor) console.log(firstActor.get("__RowId"), firstActor.get("ClassName"));
}

const bytes = readResource("Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs");
if (bytes) {
    const placements = openByml(bytes).get("Actors");
    if (placements && typeof placements === "object" && "kind" in placements) {
        for (const [, placement] of placements.entries()) {
            if (!placement || typeof placement !== "object" || !("kind" in placement)) continue;
            const id = placement.get("Hash");
            if (typeof id === "bigint") console.log(placement.get("Gyaml"), id.toString(16));
            break;
        }
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

### Binding a translated function with a known signature

`bindGameFunction(address, parameters, returns)` from `greenhook/data` creates a callable JavaScript wrapper and supplies parameter and return hints in VS Code. Each invocation uses `game.defer` and returns a promise. It supports `bool` (boolean), `i32`, `u32`, `f32`, `f64` (number), and `ptr`, `i64`, `u64` (bigint). A `void` result resolves to undefined. Integer/pointer and floating-point arguments each have eight register slots. Structure returns, stack arguments and variadic calls require separate ABI handling. Verify the signature against the translated implementation and callers before binding it.

For example, the named SetTime function takes the world manager pointer followed by seconds. Bind it once. Obtain the manager and call the wrapper inside the same guest callback to avoid retaining a pointer across a map transition.

```js
import { game, native } from "greenhook";
import { bindGameFunction } from "greenhook/data";

const setSeconds = bindGameFunction(native.addresses.SetTime, ["ptr", "u32"], "void");
async function setNoon() {
    await game.defer(() => {
        const world = game.world.getSingletons().worldMgr;
        if (world) return setSeconds(world, 12 * 3600);
    });
}
```

### Verified native calling conventions in the 3.0.0 game image

| Address and method | Guest inputs | Result or effect |
| --- | --- | --- |
| `0x83472280 GetTime` | World manager in x0, writable 24-byte result in x8 | Total seconds are a u64 at result + 8. Use `game.world.getTime().totalSeconds` for ordinary reads. |
| `0x834722d0 SetTime` | World manager in x0, seconds in w1 | The function loads the time manager at world + 0x510 and delegates the change. Use `game.world.setTime(hour, minute)` to schedule it from a frame callback. |
| `0x805b63e0 PouchCount` | Pouch manager in x0, address of a guest pointer to the item name in x1, zero in x2 and x3 for an ordinary count | Count in w0. `game.inventory.get(name)` handles the guest string, song flags and supported aliases. |
| `0x806a2b20 SetBoolPersist` | GameData manager in x0, Boolean in w1, flag hash in w2 | Updates the specified Boolean. Use `game.flags.setBool(name, value, true)`. |
| `0x806d2530 ChangePosition` | Player controller in x0 and pointer to three f32 coordinates in x1 | Moves Link through the controller. Use `game.player.setPos(x, y, z)`. |
| `0x80d9a820 RequestEvent` | Event manager in x0 and constructed event-request object in x1 | Submits the request. `game.world.triggerEvent(name, actor?)` handles construction and actor-link lifetime. |
| `0x8022d9c0 GetBool` | GameData manager in x0, writable output byte address in x1, flag hash in w2 | Bit 0 of w0 reports whether the lookup succeeded. Read bit 0 of the output byte for the value. Use `game.flags.getBool(key)` to distinguish false from missing. |
| `0x80cd0ea0 GetString64` | GameData manager in x0, writable guest pointer slot in x1, flag hash in w2 | Bit 0 of w0 reports success. The output slot receives a guest string pointer. Copy the string during the callback. `game.flags.getString(key)` returns the JavaScript string directly. |
| `0x80d99f10 EventRequestArgCtor` | Address of writable event-request storage in x0 | Initializes the request object in place. The 3.0.0 bridge reserves 0x1b20 bytes for it, then fills its event name and optional parameters before submission. Use `game.world.triggerEvent()` or `teleport()` for that sequence. |
| `0x802dc440 ActorLinkSet` | Initialized actor link in x0, live actor pointer in x1, zero in x2 for the event-request path | Attaches the actor to the request’s starter link. The link has a lifetime separate from the actor. |
| `0x8022b6f0 ActorLinkRelease` | Initialized actor link in x0 | Releases a link. The event bridge cleans up both embedded links after submitting the request. Passing the actor pointer itself would target the wrong object. |
| `0x828d4a70 PouchAddRemove` | Pointer to a populated event parameter block in x0 | Uses the item name and signed count delta referenced by that block. The host reads the top two bits of the word at block + 0x18 for status. Use `game.inventory.add()` or `remove()` for ordinary edits. |

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

`game.onActorUpdate(callback)` runs at Longshot's shared actor transform update, with a live guest context. The snapshot includes `ptr`, `instanceId`, `statusCode`, `name`, `vtable`, `components`, and `pos`. The identity and raw status fields are the actor fields that the translated ActorLinkSet reads at offsets 24 and 44. Keep callbacks short. Store observations for display and reacquire live pointers before mutation. Actor names are best-effort on build 3.0.0. Some transform callbacks expose an unreadable or non-text name. Match a known placement by its map data when you need an authored actor type.

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

## Adding and removing map actor placements

Import these helpers from `greenhook/data`. They edit a Banc map resource in memory. Apply the override before entering its scene. The game decides which placements to instantiate when it loads the map, including age, event and other conditions. Placement IDs are unsigned 64-bit hashes stored in the map. Live actor pointers and instance IDs belong to a different system.

| Function | Behavior |
| --- | --- |
| `listPlacementFiles(map)` | Takes one Banc directory name, such as `"KokiriForest"`. Returns sorted resource paths, including age, static, dynamic and cutscene variants present on disk. Select the variant your mod targets. |
| `addActorPlacement(path, templateId, newId, position)` | Copies an existing placement in that file. Supplies a new nonzero bigint ID and absolute world coordinates `{x,y,z}`. Keeps the original actor type, rotation, parameters, links and conditions. Sets the copied physics placement ID where present. Duplicate IDs within the file, invalid coordinates and edits that exceed a known map allocation throw before installation. Choose an ID unique across the placements your mod can load together. |
| `removeActorPlacement(path, id)` | Removes the matching placement from that file’s actor list for subsequent loads. References to that ID elsewhere in the map or in other files remain the mod author’s responsibility. |

Both edits return false when the requested file or source ID is missing or the override table is full. They build on the currently visible override. The SDK deep-copies the template, then re-encodes the map in its original byte order. For a larger map, reserve sufficient space with setResourceSizes and restart before applying it. Apply edits once during setup. Calling `game.romfs.clearOverride(path)` clears all edits to that path, including edits from other mods.

```js
import { listPlacementFiles, addActorPlacement } from "greenhook/data";

// templateId is a Hash from this file’s Actors array.
function placeInKokiriForest(templateId, newId, position) {
    const file = listPlacementFiles("KokiriForest")
        .find(path => path.endsWith("/KokiriForest_Child_Static.bcett.byml.zs"));
    return file ? addActorPlacement(file, templateId, newId, position) : false;
}
```

## Calling an actor’s virtual method

`callActorMethod(actor, slot, args?)` from `greenhook/data` schedules a call in a live guest context. Pass an observation from `game.onActorUpdate`, `trackActors().list()` or a successful `game.actors.inspect(ptr)`. Before dispatch it re-reads the pointer, instance ID and vtable, then resolves the slot on that actor. A mismatch or an unresolved entry rejects the promise. These checks cannot establish that the object is still alive if freed storage retains the same contents.

Supply the **zero-based slot and signature verified for the actor class**. The helper passes the actor in x0. The first element of `args.x` goes into x1, leaving room for seven integer or pointer arguments. `args.f` and `args.d` start at vector register 0 and share the same register bank. Use `indirectResult` for a caller-owned return buffer when the method requires it. The promise resolves to `{x0,x1,x2,x3,f0,d0}`. There is no universal spawn, destruction, AI or animation slot across actor classes.

## Editing installed resources in memory

`editResource(path, edit)` reads a private copy, calls the synchronous edit function with the decompressed bytes, then registers a virtual override. It returns false when the input is missing or virtual slots are full. An exception before registration preserves the previous override. For a .zs input it emits a valid Zstandard frame with raw blocks and the same decompressed size. Already-loaded game objects may require a scene or resource reload before they reflect a change. `game.romfs.clearOverride(path)` removes the override for future opens.

`node.set(key, value)` edits an existing numeric, Boolean or string field in place. Use bigint for 64-bit integers. A string replacement must already appear in that file's string table. `openPack` returns mutable byte views, so editing a BYML member also changes its parent pack's bytes. Avoid editing aliased BYML data unless all references should see the change. For new fields, strings and resized containers, use the tree writer below.

```js
import { editResource, openByml } from "greenhook/data";

const path = "Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs";
function movePlacement(id, x) {
    return editResource(path, bytes => {
        const actors = openByml(bytes).get("Actors");
        if (!actors || typeof actors !== "object" || !("kind" in actors)) throw new Error("Actors array missing");
        for (const [, actor] of actors.entries()) {
            if (!actor || typeof actor !== "object" || !("kind" in actor)) continue;
            if (actor.get("Hash") === id) {
                const position = actor.get("Translate");
                if (!position || typeof position !== "object" || !("kind" in position)) throw new Error("Translate array missing");
                position.set(0, x);
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
const table = file && openTable(file);
if (table) {
    const dungeon = findRecord(table, "BottomOfTheWell");
    const hash = dungeon?.get("TargetGameData");
    if (typeof hash === "number") console.log(game.flags.get(hash));
}
```
## VS Code type hints

Install the Developer ZIP and open the installed `scripts` folder. The SDK README section **Type hints in Visual Studio Code** covers completion, separate projects, and troubleshooting.

## Writing game resources and keeping edits across launches

| Export from `greenhook/data` | Usage |
| --- | --- |
| `decodeByml(bytes)` | Decode the whole file into an editable document with `root`, `version` and `littleEndian`. Maps become objects and arrays become JavaScript arrays. Numeric fields carry their original storage type. |
| `byml.value(field)`, `byml.isNumber(field)` | Unwrap a numeric field or test whether a value is a tagged number. Use `byml.i32`, `u32`, `f32`, `i64`, `u64` or `f64` to choose an explicit width for a replacement. Pass bigint values for exact 64-bit integers. |
| `cloneByml(value)`, `encodeByml(document)` | Deep-copy a subtree before editing it. Encode the whole document to bytes after adding or removing records. The original format version and byte order are preserved when using the decoded document. |
| `writeResource(path, bytes, options?)` | Accepts decompressed bytes and wraps them for a .zs destination. The default saves a file under scripts_data/romfs-overrides and registers a redirect. Pass `{memory:true}` for a session override. Check the returned boolean. |
| `installSavedOverrides()`, `removeSavedOverride(path)` | Restore saved redirects at script startup and return the number installed, or remove the saved override for one path. A scene may retain an already loaded resource until re-entry. |
| `resourceSize(path)`, `setResourceSizes(sizes)` | Read the resource allocation in bytes (null if unlisted), or raise listed minima using a Map or path-to-size object. The result lists updated and missing paths. The running game can retain its old size table. Restore the saved redirect on the next launch before loading larger resources. |

## Discovering game classes and named native functions

| Method | Result |
| --- | --- |
| `actorClassEntries()` from `greenhook/data` | Iterates installed ActorInfo definitions as `{actor, className, record}`. Several actors can use the same class. The record retains the original BYML metadata. Consume a large iterator incrementally across frames. |
| `openAiDefinitions()` from `greenhook/data` | Reads the installed AI node definition map, or returns null when absent. Use `get(nodeName)` for one definition or `entries()` to discover names, inputs and tags. The fields vary by node. These names identify nodes used inside game graphs. |
| `openNativeModule(module)` from `greenhook/data` | Snapshots the dynamic symbol and string tables from guest memory for rtld, main, subsdk0 or sdk on the supported 3.0.0 image. Returns null for unmapped tables. A module object holds up to about 2.6 MiB of data until released. |
| `module.page(start = 0, limit = 64)` | Scans 1–256 table slots and returns `{symbols, next}`. Each entry contains the raw symbol, a demangled name when available, and a callable guest address. Follow next even after an empty page. Null ends the scan. Overloads and aliases can have separate entries. |
| `native.demangleSymbol(symbol)` | Decodes a mangled C++ name, including encoded parameters, or returns null. Ordinary C exports keep their original name in module pages. |

The dynamic tables describe exported functions in the application, media library and platform SDK. Most internal gameplay routines have no exported symbol. `native.listFunctions()` still enumerates their translated addresses. Class records, AI node names and native exports come from different tables. The game does not provide a universal mapping between them. A demangled member name usually omits its return type and the implicit receiver.

```js
import { openNativeModule } from "greenhook/data";

// Search one page per presented frame so an overlay stays responsive.
async function findSdkMethods(fragment) {
    const sdk = openNativeModule("sdk");
    const matches = [];
    if (!sdk) return matches;
    let cursor = 0;
    for (;;) {
        const page = sdk.page(cursor, 128);
        matches.push(...page.symbols.filter(s => s.name.includes(fragment)));
        if (page.next === null) return matches;
        cursor = page.next;
        await nextFrame();
    }
}
```

### Resolving AI classes to native methods

`native.getAiClass(name)` searches the supported game image for vtables whose slot 19 returns the exact class name. It returns an array because multiple tables can share a name. A miss returns an empty array. Each result has a guest `vtable` address and a zero-based `methods` array. A null slot means the pointer has no translated function in the dispatch table. Look up a class once and keep the result. Class lookup scans the image and belongs outside frame callbacks.

The 3.0.0 dump matched 4,225 names from its 5,531 AI definitions using this layout. Some definitions have no matching native class. The slots correspond to native implementations, including inherited ones. The game’s graph input definitions describe graph parameters. A method taking a game-owned query object expects that object to exist and contain initialized graph state.

```js
import { native } from "greenhook";

const query = native.getAiClass("QueryGetPouchContentNumByActorName")
    .find(entry => entry.methods.includes(native.addresses.PouchCountQuery));
if (query) {
    const slot = query.methods.indexOf(native.addresses.PouchCountQuery);
    console.log("Pouch count query", slot, query.methods[slot]?.toString(16));
}
```

For this build the pouch query uses slot 39 at 0x82a3c260. The Banc change query has a different vtable and uses slot 39 at 0x829df380. The ocarina action's start and tick use separate slots. To observe a graph action, pass its verified executable address to `native.hook`. To make a direct call, supply the ABI and live receiver appropriate to that particular method. Actor pointers refer to a different object type.

Use `bindAiMethod(className, slot, parameterTypes, returnType)` from `greenhook/data` to create a typed callable for a known scalar method. The resulting function takes a receiver callback followed by the explicit method arguments. The SDK runs the receiver callback in a guest context, checks its current vtable against the class, supplies x0, then dispatches. An unmapped pointer, wrong vtable or unresolved slot fails the call. The mod is responsible for acquiring a live graph object and observing its lifetime. For the verified class-name getter, the signature is `bindAiMethod("QueryGetPouchContentNumByActorName", 19, [], "ptr")`. Read the returned guest C string to obtain the class name.

### Additional addresses in Longshot 3.0.0

Access each entry as `native.addresses.Name`. The first fifteen names below describe gameplay paths identified from Longshot’s translated call sites and patches. Use game-owned action objects for graph methods. Where the full register contract is unknown, observe an existing call before attempting direct invocation. For timekeeping, `bindGameFunction(native.addresses.GetSystemTick, [], "u64")` provides a no-argument wrapper. Divide a tick difference by the frequency to measure elapsed seconds.

| Name | Guest address | Behavior |
| --- | --- | --- |
| `PouchCountQuery` | `0x82a3c260` | Runs the event graph query for an inventory count. Its receiver is a game-owned query object. |
| `BancChangeEventArgQuery` | `0x829df380` | Reads parameters of a Banc change event from its game-owned request context. |
| `TimelineStart` | `0x83b5bfb0` | Starts a timeline. game.onTimelineStart exposes the name and cancellation flag. |
| `MostRecentEventSkipQuery` | `0x80de6cb0` | Evaluates the graph query for the most recent event skip. |
| `TimeManagerSetTime` | `0x80f0e640` | Receives the time manager in x0, seconds in w1, 1 in w2 and 0xffffffff in w3 from the world SetTime method. |
| `PlayerCameraUpdate` | `0x80644c80` | Updates the player camera. This is the update Longshot patches for camera speed. |
| `TextPaneAdvance` | `0x80b1e140` | Advances the letter animation on a game text pane. |
| `OcarinaNoteWaitStart` | `0x828a9190` | Starts the ocarina note-wait action and reports it to Longshot. |
| `OcarinaNoteWaitTick` | `0x828a9280` | Ticks the ocarina action and evaluates its input gates. |
| `VoiceResourceUpdate` | `0x80a9ca30` | Updates the voice resource using the saved voice-language option. |
| `MoviePlayerUpdate` | `0x83b7c710` | Updates the movie player frame count used by timelines. |
| `AnimationEntryAdvance` | `0x802cfca0` | Advances animation entry clocks using their speed and start/end range. |
| `SkeletonPoseTimeUpdate` | `0x80944aa0` | Writes the pose time used to draw a skeleton. |
| `FrameStepUpdate` | `0x80706be0` | Updates the frame step that advances the game clock. |
| `GlobalStepUpdate` | `0x83b6ea90` | Updates the global step read by other game systems. |
| `GetSystemTick` | `0x85106370` | Returns the current Longshot system tick in x0. Takes no arguments. |
| `GetSystemTickFrequency` | `0x851063a0` | Returns ticks per second in x0. Takes no arguments. |
