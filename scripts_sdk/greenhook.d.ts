/* Greenhook mod API for Longshot 3.0.0 */
interface Vec3 {
    x: number;
    y: number;
    z: number;
}

type Vec3Input = Vec3 | [number, number, number];
type AddressLike = bigint | number | string;
type ButtonName =
    | "A" | "B" | "X" | "Y" | "LS" | "RS" | "L" | "R"
    | "ZL" | "ZR" | "PLUS" | "MINUS" | "LEFT" | "UP" | "RIGHT" | "DOWN";
type ColorInput = string | number | [number, number, number] | [number, number, number, number];

interface ScriptMetadata {
    id: string;
    name: string;
    version: string;
    author: string;
    description: string;
    path: string;
    enabled: boolean;
    loaded: boolean;
    error: string;
}

interface RuntimeStats {
    memoryUsedBytes: number;
    mallocSize: number;
    objectCount: number;
    atomCount: number;
}

interface LongshotRuntime {
    readonly version: string;
    readonly gameBuild: string;
    readonly scriptsDir: string;
    readonly dataDir: string;
    reload(): boolean;
    scripts(): ScriptMetadata[];
    setScriptEnabled(id: string, enabled: boolean): boolean;
    stats(): RuntimeStats;
}

type Unsubscribable = () => void;

interface FrameEvent {
    readonly frame: number;
    readonly dt: number;
    readonly fps: number;
}

interface PlayerPointers {
    actor: bigint;
    player: bigint;
    components: bigint;
    controller: bigint;
}

interface PlayerUpdateEvent extends PlayerPointers {
    readonly pos: Vec3;
    readonly facing: number;
    readonly life: number;
    readonly maxLife: number;
    readonly rupees: number;
}

interface DamageEvent {
    readonly oldLife: number;
    damage: number;
    cancel: boolean;
}

interface CameraUpdateEvent {
    readonly view: bigint;
    readonly eye: Vec3;
    readonly target: Vec3;
}

interface GreenhookInputEvent {
    buttons: bigint;
    readonly rawButtons: bigint;
    lx: number;
    ly: number;
    rx: number;
    ry: number;
    blockGameInput: boolean;
}

interface EventRequestEvent {
    readonly name: string;
    readonly arg: bigint;
}

interface TimelineStartEvent {
    readonly name: string;
    readonly player: bigint;
    cancel: boolean;
}

interface SaveRequestEvent {
    readonly reason: number;
    readonly mode: number;
    readonly caller: bigint;
    readonly isTransition: boolean;
    cancel: boolean;
}

interface MapChangeEvent {
    readonly map: string;
    readonly prevMap: string;
    readonly age: "child" | "adult" | null;
}

interface MenuTickEvent {
    readonly screen: bigint;
    readonly vtable: bigint;
}

interface MenuFocusEvent {
    readonly screen: bigint;
    readonly pane: bigint;
    readonly lr: bigint;
}

interface BgmChangeEvent {
    readonly music: string;
    readonly prevMusic: string;
}

interface GamePlayerNamespace {
    /** Read the latest sample of life, maximum life, rupees, Skulltula tokens and heart pieces. Life uses quarter hearts. An unavailable sample or individual field is null. */
    getProgress(): { life: number | null; maxLife: number | null; rupees: number | null; skulltulaTokens: number | null; heartPieces: number | null } | null;
    /** Read Link’s world coordinates. Falls back to the cached position, initially (0, 0, 0). Use getPointers().actor to check whether Link has been observed. */
    getPos(): Vec3;
    /** Move Link to absolute world coordinates. Executes with a live player context or queues the move for a player update. */
    setPos(x: number, y: number, z: number): boolean;
    /** Read Link’s heading in radians from his transform, falling back to the last sample (initially zero). */
    getFacing(): number;
    /** Write the heading in radians. Returns true when the runtime has an actor to update. */
    setFacing(radians: number): boolean;
    /** Read current life in quarter hearts (4 is one heart). Falls back to the last sample, initially -1. */
    getLife(): number;
    /** Set current life in quarter hearts, clamped to a minimum of zero. Returns true if either the game-data or live health write succeeds. */
    setLife(quarterHearts: number): boolean;
    /** Read heart capacity in quarter hearts, or -1 before the first successful read or sample. */
    getMaxLife(): number;
    /** Set heart capacity in quarter hearts, with a minimum of 4. Returns whether the capacity flag was written. */
    setMaxLife(quarterHearts: number): boolean;
    /** Read the wallet count, falling back to the last sample, initially -1. */
    getRupees(): number;
    /** Write the wallet count, clamping negative amounts to zero. Returns false if the rupee flag cannot be written. */
    setRupees(amount: number): boolean;
    /** Read the age flag or last observed age. Returns null before either is available. */
    getAge(): "child" | "adult" | null;
    /** Request the child or adult form. Runs immediately with a live actor and guest context or queues the change. Unsupported names throw RangeError. */
    setAge(age: "child" | "adult"): boolean;
    /** Read the script movement-speed multiplier. */
    getSpeedScale(): number;
    /** Set the movement multiplier. Values clamp to 0.05–10. Use 1 for normal speed. */
    setSpeedScale(scale: number): void;
    /** Read the invincibility override. Null means the host controls it. */
    getInvincible(): boolean | null;
    /** Force invincibility on or off. Pass null to release script control. */
    setInvincible(enabled: boolean | null): void;
    /** Read the script damage multiplier. */
    getDamageMultiplier(): number;
    /** Scale incoming damage by a factor from 0 to 100. Use 1 for normal damage. */
    setDamageMultiplier(mult: number): void;
    /** Read the flight override. Null means the host controls it. */
    getFlight(): boolean | null;
    /** Force flight on or off. Pass null to release script control. */
    setFlight(enabled: boolean | null): void;
    /** Read the current player prompt string or its last sampled value. It can be empty. */
    getPrompt(): string;
    /** Read the last observed actor, player, components and controller guest addresses. An unavailable address is 0n. Refresh pointers after transitions. */
    getPointers(): PlayerPointers;
}

interface InventoryItemEntry {
    label: string;
    name: string;
    kind: "toggle" | "quantity";
    count: number;
}

interface GameInventoryNamespace {
    /** Enumerate known items with their display label, actor name, kind (toggle or quantity) and count. A count of -1 means unavailable. */
    list(): InventoryItemEntry[];
    /** Read an item by actor name or recognized label. Returns its count, 0 or 1 for a recognized song flag, or -1 if unavailable. */
    get(itemName: string): number;
    /** Return true when the reported count exceeds zero. Use get() to distinguish an unavailable count from zero. */
    has(itemName: string): boolean;
    /** Add the specified amount (default 1). Outside a live guest context, true means the change was accepted into the queue. */
    add(itemName: string, amount?: number): boolean;
    /** Subtract the specified amount (default 1). Outside a live guest context, true means the change was accepted into the queue. */
    remove(itemName: string, amount?: number): boolean;
    /** Set an absolute count. Negative targets throw RangeError. A queued edit computes the required change when it executes. */
    set(itemName: string, targetCount: number): boolean;
}

interface GameFlagsNamespace {
    /** Scan up to 128 table slots per call for bool, int, uint, float, enum or string. Start at cursor 0 and pass back next, even after an empty page. A null next ends the scan. A null page means unavailable. */
    list(kind: "bool" | "int" | "float" | "enum" | "string" | "uint", cursor?: number): { hashes: number[]; next: number | null } | null;
    /** Compute the unsigned MurmurHash3 x86-32 key for a flag name using seed zero. */
    hash(name: string): number;
    /** Read a named or hashed flag across supported 32-bit-key tables. Returns a boolean, number, string or null when absent. */
    get(nameOrHash: string | number): boolean | number | string | null;
    /** Read an existing Boolean flag. Null means missing or unavailable. */
    getBool(nameOrHash: string | number): boolean | null;
    /** Write a Boolean. With persist enabled (default), also invoke or queue the game’s persistent setter. Pass false as the third argument for a direct in-memory write. */
    setBool(nameOrHash: string | number, value: boolean, persist?: boolean): boolean;
    /** Read an existing signed 32-bit flag, or null. */
    getInt(nameOrHash: string | number): number | null;
    /** Write an existing signed 32-bit entry. Returns whether the write succeeded. */
    setInt(nameOrHash: string | number, value: number): boolean;
    /** Read an existing unsigned 32-bit flag, or null. */
    getUInt(nameOrHash: string | number): number | null;
    /** Write an existing unsigned 32-bit entry. Returns whether the write succeeded. */
    setUInt(nameOrHash: string | number, value: number): boolean;
    /** Read a single-precision game flag as a JavaScript number, or null. */
    getFloat(nameOrHash: string | number): number | null;
    /** Write an existing float flag with single-precision rounding. Returns whether the write succeeded. */
    setFloat(nameOrHash: string | number, value: number): boolean;
    /** Read the numeric enum value hash, or null when the entry is unavailable. */
    getEnum(nameOrHash: string | number): number | null;
    /** Write an enum value using its string name (hashed) or numeric hash. Returns whether the existing entry was written. */
    setEnum(nameOrHash: string | number, value: string | number): boolean;
    /** Read an existing game string from its 64-byte buffer, or null. */
    getString(nameOrHash: string | number): string | null;
    /** Update an existing string buffer. At most 63 UTF-8 bytes fit before the terminator. Keep replacements within that limit to preserve whole characters. */
    setString(nameOrHash: string | number, value: string): boolean;
    /** Read a placement Boolean by its full 64-bit ID. Pass a bigint for exact integer precision. Returns null if absent. */
    getBool64(placementId: AddressLike): boolean | null;
    /** Change an existing placement Boolean by ID. Returns false if its entry cannot be found or written. */
    setBool64(placementId: AddressLike, value: boolean): boolean;
    /** Observe changes after the first readable sample establishes a baseline. The callback receives new value, previous value and original key. Call the returned function to stop. */
    watch(
        nameOrHash: string | number,
        callback: (newVal: boolean | number | string | null, oldVal: boolean | number | string | null, key: string | number) => void
    ): Unsubscribable;
}

interface WorldTime {
    totalSeconds: bigint;
    seconds: number;
    hour: number;
    minute: number;
    formatted: string;
}

interface DestinationEntry {
    index: number;
    child: string | null;
    adult: string | null;
}

interface SingletonsInfo {
    gameDataMgr: bigint;
    eventMgr: bigint;
    worldMgr: bigint;
    cameraMgr: bigint;
    actorMgr: bigint;
    pouchMgr: bigint;
}

interface GameWorldNamespace {
    /** Read the current map’s internal ID, or null when unavailable. */
    getMap(): string | null;
    /** Look up the English area name for a map ID. Omit the ID to use the current map. Returns null when unresolved. */
    getAreaName(map?: string): string | null;
    /** Read the loading flag. Returns false when the flag is unavailable. */
    isLoading(): boolean;
    /** Test whether the event manager has a current event. Returns false if the manager cannot be read. */
    isEventPlaying(): boolean;
    /** Read the current BGM identifier from game data or its cached sample. The string may be empty. */
    getMusic(): string;
    /** Read totalSeconds as a bigint and seconds within the day as a number. Also returns hour, minute and formatted HH:MM. Returns null without a readable clock. */
    getTime(): WorldTime | null;
    /** With two arguments, set hours and minutes. With one, values 0 through 24 are hours (fractions allowed), all others are seconds. The result wraps at 24 hours. May queue until a player update. */
    setTime(hourOrSeconds: number, minute?: number): boolean;
    /** List teleport presets and their child/adult map IDs. Either age-specific ID can be null. Use index for an unambiguous preset. */
    destinations(): DestinationEntry[];
    /** Request a map change by index, first matching name or custom destination. Full names match case-insensitively, substrings case-sensitively. Custom defaults are Entrance and Dm_SY_BancChangeArrive. Observe onMapChange to detect arrival. */
    teleport(target: number | string | { banc: string; startPos?: string; bootEvent?: string }): boolean;
    /** Request a named game event using the supplied starter actor or the most recently sampled Link actor. May queue until a player update. */
    triggerEvent(eventName: string, starterActorPtr?: AddressLike): boolean;
    /** Read the six game manager addresses as bigints. Check for 0n before using a manager and obtain a fresh address after transitions. */
    getSingletons(): SingletonsInfo;
}

interface CameraPose {
    eye: Vec3;
    target: Vec3;
    matrix: number[];
    yaw: number;
    pitch: number;
}

interface CameraOverridePose {
    matrix?: number[];
    eye?: Vec3Input;
    target?: Vec3Input;
    yaw?: number;
    pitch?: number;
}

interface ScreenPoint {
    x: number;
    y: number;
    depth: number;
    visible: boolean;
}

interface GameCameraNamespace {
    /** Read the active gameplay view’s eye, target, 12-number view matrix and last sampled camera angles. Returns null when the view is unavailable. */
    getPose(): CameraPose | null;
    /** Supply a 12-number row-major view matrix, eye and target, or eye with yaw and pitch in radians. Both angles are needed in the angle form. Pass null to release the view. */
    setOverride(pose: CameraOverridePose | null): void;
    /** Override the player camera’s yaw and pitch in degrees. */
    setPlayerAngles(yawDeg: number, pitchDeg: number): void;
    /** Release the player camera angle override. */
    clearPlayerAngles(): void;
    /** Set the field-of-view multiplier, clamped to 0.25–2.5. */
    setFovScale(scale: number): void;
    /** Read the current field-of-view multiplier. */
    getFovScale(): number;
    /** Set the camera-speed multiplier, clamped to 0.1–8. */
    setSpeedScale(scale: number): void;
    /** Read the current camera-speed multiplier. */
    getSpeedScale(): number;
    /** Pass true to suppress camera centering, false to allow it or null to use the host setting. */
    setDisableCentering(disable: boolean | null): void;
    /** Project a world position to window pixels using a vertical FOV in degrees (default 50). Returns null without a view. Positive depth is forward. The visible test allows a 200-pixel margin, so check window bounds for strict clipping. */
    worldToScreen(x: number, y: number, z: number, fovDeg?: number): ScreenPoint | null;
}

interface ActorSnapshot {
    ptr: bigint;
    vtable: bigint;
    name?: string;
    components: bigint;
    pos: Vec3;
    facing?: number;
}

interface GameActorsNamespace {
    /** Read Link’s actor snapshot, or null before an actor has been observed. */
    getPlayer(): ActorSnapshot | null;
    /** Resolve 1–128 consecutive vtable slots starting at a nonnegative integer slot. An unreadable range returns null. Individual untranslated entries are null. Slot signatures depend on the actor class and build. */
    getMethods(actor: AddressLike, firstSlot: number, count: number): (bigint | null)[] | null;
    /** Read a snapshot for a current actor address, or null if the header cannot be read. Obtain addresses from actor callbacks and refresh them after map changes. */
    inspect(ptr: AddressLike): (ActorSnapshot & { instanceId: number; statusCode: number; name: string }) | null;
    /** Write world coordinates to a live actor. Link uses the player movement path. Other actors receive a direct transform write which their own update can overwrite. */
    setPos(ptr: AddressLike, x: number, y: number, z: number): boolean;
    /** Read a live actor's yaw in radians from its transform, or null if it cannot be read. */
    getFacing(ptr: AddressLike): number | null;
    /** Turn a live actor to a yaw in radians. The rotation becomes a pure yaw: pitch and roll are dropped, per-axis scale is kept. Link uses the player facing path. Like setPos, the actor's own update can overwrite it. */
    setFacing(ptr: AddressLike, radians: number): boolean;
}

interface PadStateSnapshot {
    buttons: number;
    rawButtons: number;
    justPressed: number;
    justReleased: number;
    lx: number;
    ly: number;
    rx: number;
    ry: number;
}

interface MouseSnapshot {
    /** Cursor in window client pixels. */
    x: number;
    y: number;
    /** Render frame size. */
    width: number;
    height: number;
    buttons: number;
    /** Cursor in ui.draw / worldToScreen pixels. Use these to hit-test overlay drawings. */
    overlayX: number;
    overlayY: number;
    /** Size of the ui.draw coordinate space. */
    overlayWidth: number;
    overlayHeight: number;
    /** True while the cursor is over a Dear ImGui window or a widget holds the mouse. */
    overUi: boolean;
}

interface InputOverrideSpec {
    buttons?: number | bigint | ButtonName[];
    mask?: number | bigint;
    blockGameInput?: boolean;
    lx?: number;
    ly?: number;
    rx?: number;
    ry?: number;
}

interface GameInputNamespace {
    readonly Buttons: Record<ButtonName, number>;
    /** Read sampled buttons, rawButtons, justPressed, justReleased and four integer stick axes. Button masks here are numbers sampled before script overrides. */
    getState(): PadStateSnapshot;
    /** Test a button name or bitmask. For a combined mask, every selected button must be held. */
    isDown(button: ButtonName | number): boolean;
    /** Test whether any selected button changed from up to down between the two latest input samples. */
    justPressed(button: ButtonName | number): boolean;
    /** Test whether any selected button changed from down to up between the two latest input samples. */
    justReleased(button: ButtonName | number): boolean;
    /** Test a supported key name or Windows virtual-key code. Returns false while the game process lacks foreground focus. */
    isKeyDown(key: string | number): boolean;
    /** Read cursor coordinates, window size and button bits (left 1, right 2, middle 4). Coordinates are client-relative when the game is foreground, otherwise screen-relative. */
    getMouse(): MouseSnapshot;
    /** Replace selected controller bits and stick axes on successive input polls. The button mask defaults to 0xffff. Use mask 0 for a sticks-only override. Axes clamp to ±32767. Passing null releases buttons and sticks but preserves the block setting. */
    setOverride(spec: InputOverrideSpec | null): void;
    /** Release controller button and stick overrides and clear the persistent input block. */
    clearOverride(): void;
    /** Zero game input before script overrides on each poll while enabled. Pass false to release the block. */
    setBlockGameInput(block: boolean): void;
}

interface GameAudioNamespace {
    /** Read the most recently observed background music identifier. */
    getMusic(): string;
    /** Set the volume multiplier, clamped to 0–4. Use 1 for normal volume. */
    setVolumeScale(scale: number): void;
    /** Read the current volume multiplier. */
    getVolumeScale(): number;
    /** Set the pitch multiplier, clamped to 0.25–4. Use 1 for normal pitch. */
    setPitchScale(scale: number): void;
    /** Read the current pitch multiplier. */
    getPitchScale(): number;
    /** Enable or disable the script mute switch. */
    setMute(muted: boolean): void;
    /** Return the script mute switch state. */
    isMuted(): boolean;
}

interface ControlOptionEntry {
    key: string;
    name: string;
    text: string;
    tab: string;
    value: number;
    min: number;
    max: number;
    step: number;
}

interface ScreenshotResult {
    ok: boolean;
    present: number;
    path: string;
}

interface GameGraphicsNamespace {
    /** Read the number of presented frames. Use differences for frame counts. */
    getFrame(): number;
    /** Read the recent measured presentation rate in frames per second. It starts at zero before sufficient samples arrive. */
    getFps(): number;
    /** Read render window width and height in pixels. Dimensions can be zero before frame information is available. */
    getWindowSize(): { width: number; height: number };
    /** Read an integer option value. A missing string key returns null. A numeric ID uses the host lookup with fallback -1. */
    getOption(keyOrId: string | number): number | null;
    /** Set an integer option by key or numeric ID. A string key reports host success. A numeric ID returns true after dispatch. Use listOptions() for discoverable keys and ranges. */
    setOption(keyOrId: string | number, value: number): boolean;
    /** Enumerate host settings with key, display name, text, tab, numeric value, min, max and step. */
    listOptions(): ControlOptionEntry[];
    /** Set the text-speed multiplier, clamped to 0.1–100. */
    setTextSpeedScale(scale: number): void;
    /** Read the script text-speed multiplier. */
    getTextSpeedScale(): number;
    /** Set the text-size multiplier, clamped to 0.25–4. */
    setTextSizeScale(scale: number): void;
    /** Read the script text-size multiplier. */
    getTextSizeScale(): number;
    /** Set the resolution_scale option as an integer percentage. Returns whether the host accepted it. */
    setResolutionScale(percent: number): boolean;
    /** Request a capture under scripts_data, default screenshots/shot.png. Await the result for ok, present and path. An invalid path or a second pending request throws synchronously. A rejected capture request can reject the promise. */
    takeScreenshot(relPath?: string): Promise<ScreenshotResult>;
}

interface GameRomfsNamespace {
    /** Serve a game resource from a host file on future opens. Returns whether the redirect was registered. */
    redirect(romPath: string, hostFilePath: string): boolean;
    /** Serve supplied text or bytes at a game resource path on future opens. Returns false when no override slot is available. */
    setVirtualFile(romPath: string, content: string | Uint8Array | ArrayBuffer): boolean;
    /** Remove the virtual file or redirect registered at this path. Returns whether a matching entry was removed. */
    clearOverride(romPath: string): boolean;
    /** Remove all scripts’ in-memory resource overrides and redirects. Use clearOverride(path) when cleaning up one mod. */
    clearAllOverrides(): void;
    /** Test for a virtual entry or a file or directory at the resolved game resource path. */
    exists(romPath: string): boolean;
    /** Decode one Zstandard frame to fresh bytes, optionally with its dictionary. An unknown size or output above 32 MiB throws RangeError. An invalid frame or wrong dictionary throws TypeError. */
    decompressZstd(bytes: Uint8Array, dictionary?: Uint8Array): Uint8Array;
    /** Read a fresh byte array, preferring a virtual file, then the resolved host file. Returns null on a missing file or an unreadable or oversized disk file (64 MiB limit). */
    readFile(romPath: string): Uint8Array | null;
    /** List immediate child names from the resolved disk directory, or an empty array if unavailable. Omit the path for the root. Virtual-only files are absent from this listing. */
    listDir(romDir?: string): string[];
}

interface GameNamespace {
    readonly player: GamePlayerNamespace;
    readonly inventory: GameInventoryNamespace;
    readonly flags: GameFlagsNamespace;
    readonly world: GameWorldNamespace;
    readonly camera: GameCameraNamespace;
    readonly actors: GameActorsNamespace;
    readonly input: GameInputNamespace;
    readonly audio: GameAudioNamespace;
    readonly graphics: GameGraphicsNamespace;
    readonly romfs: GameRomfsNamespace;

    /** Run the callback immediately with a live guest context or at the next player update. Await its return value. A thrown callback exception rejects the promise. */
    defer<T>(fn: () => T): Promise<T>;
    /** Receive frame number, elapsed seconds (dt) and measured fps on presentation. Use for overlays and frame-based timers. */
    onFrame(cb: (ev: FrameEvent) => void): Unsubscribable;
    /** Observe snapshots from the shared actor transform hook, including instanceId and statusCode. An actor can appear repeatedly. This hook observes updates and gives no guarantee of seeing every spawn or destruction. */
    onActorUpdate(cb: (actor: ActorSnapshot & {instanceId: number; statusCode: number}) => void): Unsubscribable;
    /** Receive Link’s pointers, position, facing, life, maxLife and rupees during a player update. Guest calls can run synchronously inside this callback. */
    onPlayerUpdate(cb: (ev: PlayerUpdateEvent) => void): Unsubscribable;
    /** Inspect oldLife and incoming damage in quarter hearts. Assign a nonnegative integer to damage or set cancel to true before returning. */
    onDamage(cb: (ev: DamageEvent) => void): Unsubscribable;
    /** Receive the guest view address and eye and target positions when the gameplay camera updates. */
    onCameraUpdate(cb: (ev: CameraUpdateEvent) => void): Unsubscribable;
    /** Receive raw and effective button masks as bigints and four stick axes. Modify buttons and axes synchronously to change the sample. To suppress this sample, set buttons to 0n and all four axes to zero. */
    onInput(cb: (ev: GreenhookInputEvent) => void): Unsubscribable;
    /** Receive the requested game event name and guest argument address. Inspect the argument during the callback. */
    onEventRequest(cb: (ev: EventRequestEvent) => void): Unsubscribable;
    /** Receive the timeline name and player address. Set cancel to true synchronously to veto the start. */
    onTimelineStart(cb: (ev: TimelineStartEvent) => void): Unsubscribable;
    /** Inspect reason, mode, caller address and isTransition. Set cancel to true synchronously to veto this save request. */
    onSaveRequest(cb: (ev: SaveRequestEvent) => void): Unsubscribable;
    /** Receive current and previous map IDs as two positional strings. Refresh map-specific state here. */
    onMapChange(cb: (map: string, prevMap: string) => void): Unsubscribable;
    /** Observe the current guest menu screen and its vtable during a menu update. */
    onMenuTick(cb: (ev: MenuTickEvent) => void): Unsubscribable;
    /** Observe the guest menu screen, focused pane and caller link register when focus changes. */
    onMenuFocus(cb: (ev: MenuFocusEvent) => void): Unsubscribable;
    /** Receive current and previous background music identifiers as two positional strings. */
    onBgmChange(cb: (music: string, prevMusic: string) => void): Unsubscribable;
    /** Run cleanup during script unload. Release subscriptions or overrides your mod owns here. */
    onUnload(cb: () => void): Unsubscribable;
}

interface UiWindowHandle {
    readonly id: string;
    setOpen(open: boolean): this;
    isOpen(): boolean;
    close(): this;
    clear(): this;
    label(text: string, color?: ColorInput): this;
    separator(): this;
    sameLine(): this;
    button(id: string, label: string, onClick?: () => void): this;
    checkbox(id: string, label: string, initial: boolean, onChange?: (checked: boolean) => void): this;
    slider(id: string, label: string, min: number, max: number, initial: number, onChange?: (value: number) => void): this;
    progressBar(fraction: number, label?: string): this;
    /** Start a tab bar. Add tabs with tab()/endTab(), then close it with endTabBar(). */
    beginTabBar(id: string): this;
    /** Start a tab inside a tab bar. Widgets until endTab() show only while it is selected. */
    tab(id: string, label: string): this;
    endTab(): this;
    endTabBar(): this;
    /** Start a scrollable, bordered region. height in pixels (0 fills the remaining space). Close with endChild(). */
    beginChild(id: string, height?: number): this;
    endChild(): this;
}

interface UiNamespace {
    readonly draw: {
        text(x: number, y: number, text: string, opts?: { color?: ColorInput; bgColor?: ColorInput; size?: number; scale?: number }): void;
        rect(x: number, y: number, w: number, h: number, opts?: { color?: ColorInput; rounding?: number; thickness?: number }): void;
        rectFilled(x: number, y: number, w: number, h: number, opts?: { color?: ColorInput; rounding?: number }): void;
        line(x1: number, y1: number, x2: number, y2: number, opts?: { color?: ColorInput; thickness?: number }): void;
        circle(cx: number, cy: number, radius: number, opts?: { color?: ColorInput; thickness?: number }): void;
        circleFilled(cx: number, cy: number, radius: number, opts?: { color?: ColorInput }): void;
        progressBar(x: number, y: number, w: number, h: number, fraction: number, opts?: { color?: ColorInput; bgColor?: ColorInput; text?: string }): void;
        clear(): void;
    };
    toast(text: string, opts?: { color?: ColorInput; durationMs?: number }): void;
    window(id: string, title: string, opts?: { x?: number; y?: number; w?: number; width?: number; h?: number; height?: number; open?: boolean; onClose?: () => void }): UiWindowHandle;
    readonly console: {
        open(): void;
        close(): void;
        toggle(): boolean;
        isOpen(): boolean;
        clear(): void;
    };
}

interface MemoryNamespace {
    readU8(addr: AddressLike): number;
    readS8(addr: AddressLike): number;
    readU16(addr: AddressLike): number;
    readS16(addr: AddressLike): number;
    readU32(addr: AddressLike): number;
    readS32(addr: AddressLike): number;
    readU64(addr: AddressLike): bigint;
    readS64(addr: AddressLike): bigint;
    readF32(addr: AddressLike): number;
    readF64(addr: AddressLike): number;
    writeU8(addr: AddressLike, val: number): void;
    writeS8(addr: AddressLike, val: number): void;
    writeU16(addr: AddressLike, val: number): void;
    writeS16(addr: AddressLike, val: number): void;
    writeU32(addr: AddressLike, val: number): void;
    writeS32(addr: AddressLike, val: number): void;
    writeU64(addr: AddressLike, val: number | bigint): void;
    writeS64(addr: AddressLike, val: number | bigint): void;
    writeF32(addr: AddressLike, val: number): void;
    writeF64(addr: AddressLike, val: number): void;
    readBytes(addr: AddressLike, len: number): Uint8Array;
    writeBytes(addr: AddressLike, data: Uint8Array | ArrayBuffer | number[]): void;
    readCString(addr: AddressLike, maxLen?: number): string;
    writeCString(addr: AddressLike, str: string): void;
    isMapped(addr: AddressLike, size?: number): boolean;
    scan(startAddr: AddressLike, len: number, patternHex: string): bigint[];
    alloc(bytes: number): bigint;
    free(addr: AddressLike): boolean;
}

interface HookCpuContext {
    x0: bigint; x1: bigint; x2: bigint; x3: bigint;
    x4: bigint; x5: bigint; x6: bigint; x7: bigint;
    lr: bigint; sp: bigint;
    getX(regIndex: number): bigint;
    setX(regIndex: number, value: number | bigint): void;
    getF(regIndex: number): number;
    setF(regIndex: number, value: number): void;
    getD(regIndex: number): number;
    setD(regIndex: number, value: number): void;
}

interface NativeNamespace {
    /** Page through the installed executable's translated guest methods. Limits are 1â€“1024. */
    listFunctions(start?: number, limit?: number): { addresses: bigint[]; total: number; next: number | null } | null;
    /** Resolve AI node vtables by exact class name. Entries contain native addresses in zero-based virtual slot order. */
    getAiClass(name: string): { vtable: bigint; methods: (bigint | null)[] }[];
    /** Decode an Itanium C++ symbol to a qualified name and parameters. Returns null for undecodable names. */
    demangleSymbol(symbol: string): string | null;
    hasFunction(addr: AddressLike): boolean;
    /** Invoke a translated function inside game.defer or onPlayerUpdate. indirectResult supplies x8 for structure returns. */
    call(addr: AddressLike, args?: { indirectResult?: AddressLike; x?: (number | bigint)[]; f?: number[]; d?: number[] }): {
        x0: bigint; x1: bigint; x2: bigint; x3: bigint; f0: number; d0: number;
    };
    hook(addr: AddressLike, handlers: {
        before?: (ctx: HookCpuContext) => boolean | void;
        after?: (ctx: HookCpuContext) => void;
    }): number;
    unhook(handleOrAddr: number | AddressLike): boolean;
    createCallback(fn: (...args: bigint[]) => bigint | number | boolean | void): bigint;
    readonly addresses: {
        /** Runs the event graph query for an inventory count. Its receiver is a game-owned query object. */
        readonly PouchCountQuery: bigint;
        /** Reads parameters of a Banc change event from its game-owned request context. */
        readonly BancChangeEventArgQuery: bigint;
        /** Starts a timeline. game.onTimelineStart exposes the name and cancellation flag. */
        readonly TimelineStart: bigint;
        /** Evaluates the graph query for the most recent event skip. */
        readonly MostRecentEventSkipQuery: bigint;
        /** Receives the time manager in x0, seconds in w1, 1 in w2 and 0xffffffff in w3 from the world SetTime method. */
        readonly TimeManagerSetTime: bigint;
        /** Updates the player camera. This is the update Longshot patches for camera speed. */
        readonly PlayerCameraUpdate: bigint;
        /** Advances the letter animation on a game text pane. */
        readonly TextPaneAdvance: bigint;
        /** Starts the ocarina note-wait action and reports it to Longshot. */
        readonly OcarinaNoteWaitStart: bigint;
        /** Ticks the ocarina action and evaluates its input gates. */
        readonly OcarinaNoteWaitTick: bigint;
        /** Updates the voice resource using the saved voice-language option. */
        readonly VoiceResourceUpdate: bigint;
        /** Updates the movie player frame count used by timelines. */
        readonly MoviePlayerUpdate: bigint;
        /** Advances animation entry clocks using their speed and start/end range. */
        readonly AnimationEntryAdvance: bigint;
        /** Writes the pose time used to draw a skeleton. */
        readonly SkeletonPoseTimeUpdate: bigint;
        /** Updates the frame step that advances the game clock. */
        readonly FrameStepUpdate: bigint;
        /** Updates the global step read by other game systems. */
        readonly GlobalStepUpdate: bigint;
        /** Returns the current Longshot system tick in x0. Takes no arguments. */
        readonly GetSystemTick: bigint;
        /** Returns ticks per second in x0. Takes no arguments. */
        readonly GetSystemTickFrequency: bigint;
        readonly GameDataMgrSlot: bigint;
        readonly EventMgrSlot: bigint;
        readonly WorldMgrSlot: bigint;
        readonly CameraMgrSlot: bigint;
        readonly ActorMgrSlot: bigint;
        readonly PouchMgrSlot: bigint;
        readonly PouchSingletonSlot: bigint;
        readonly GetBool: bigint;
        readonly SetBoolPersist: bigint;
        readonly GetString64: bigint;
        readonly RequestEvent: bigint;
        readonly EventRequestArgCtor: bigint;
        readonly ActorLinkSet: bigint;
        readonly ActorLinkRelease: bigint;
        readonly SetTime: bigint;
        readonly GetTime: bigint;
        readonly PouchAddRemove: bigint;
        readonly PouchCount: bigint;
        readonly ChangePosition: bigint;
    };
}

interface FsNamespace {
    readText(relPath: string): string | null;
    writeText(relPath: string, content: string): boolean;
    readBytes(relPath: string): Uint8Array | null;
    writeBytes(relPath: string, data: Uint8Array | ArrayBuffer | number[]): boolean;
    readJson<T = unknown>(relPath: string): T | null;
    writeJson(relPath: string, data: unknown): boolean;
    exists(relPath: string): boolean;
    remove(relPath: string): boolean;
    mkdir(relPath: string): boolean;
    listDir(relDir?: string): string[];
}

interface EventsNamespace {
    on(eventName: string, cb: (...args: any[]) => void): Unsubscribable;
    once(eventName: string, cb: (...args: any[]) => void): Unsubscribable;
    off(eventName: string, cb: (...args: any[]) => void): boolean;
    emit(eventName: string, ...args: any[]): void;
}

declare const Greenhook: LongshotRuntime;
declare const Longshot: LongshotRuntime;
declare const game: GameNamespace;
declare const ui: UiNamespace;
declare const memory: MemoryNamespace;
declare const native: NativeNamespace;
declare const fs: FsNamespace;
declare const events: EventsNamespace;

declare function setTimeout(fn: () => void, delayMs?: number): number;
declare function clearTimeout(id: number): void;
declare function setInterval(fn: () => void, intervalMs: number): number;
declare function clearInterval(id: number): void;
declare function nextFrame(): Promise<FrameEvent>;
declare function waitFrames(count: number): Promise<void>;
declare function sleep(ms: number): Promise<void>;
declare function now(): number;

declare module "greenhook" {
    export const Greenhook: LongshotRuntime;
    export const Longshot: LongshotRuntime;
    export const game: GameNamespace;
    export const ui: UiNamespace;
    export const memory: MemoryNamespace;
    export const native: NativeNamespace;
    export const fs: FsNamespace;
    export const events: EventsNamespace;
    const api: { Greenhook: LongshotRuntime; Longshot: LongshotRuntime; game: GameNamespace; ui: UiNamespace; memory: MemoryNamespace; native: NativeNamespace; fs: FsNamespace; events: EventsNamespace };
    export default api;
}
declare module "greenhook/game" { export { game, game as default } from "greenhook"; }
declare module "greenhook/ui" { export { ui, ui as default } from "greenhook"; }
declare module "greenhook/memory" { export { memory, memory as default } from "greenhook"; }
declare module "greenhook/native" { export { native, native as default } from "greenhook"; }
declare module "greenhook/fs" { export { fs, fs as default } from "greenhook"; }
declare module "greenhook/events" { export { events, events as default } from "greenhook"; }
declare module "longshot" { export * from "greenhook"; export { default } from "greenhook"; }
declare module "longshot/game" { export * from "greenhook/game"; export { default } from "greenhook/game"; }
declare module "longshot/ui" { export * from "greenhook/ui"; export { default } from "greenhook/ui"; }
declare module "longshot/memory" { export * from "greenhook/memory"; export { default } from "greenhook/memory"; }
declare module "longshot/native" { export * from "greenhook/native"; export { default } from "greenhook/native"; }
declare module "longshot/fs" { export * from "greenhook/fs"; export { default } from "greenhook/fs"; }
declare module "longshot/events" { export * from "greenhook/events"; export { default } from "greenhook/events"; }

declare module "greenhook/data" {
    export type ResourceValue = boolean | string | number | bigint | null | Uint8Array | ResourceNode;
    export interface ResourceNode {
        readonly kind: "array" | "map";
        readonly length: number;
        /** Raw BYML tag of an existing field, or undefined when missing. */
        typeOf(key: string | number): number | undefined;
        set(key: string | number, value: string | number | boolean | bigint): void;
        get(key: string | number): ResourceValue | undefined;
        entries(): IterableIterator<[string | number, ResourceValue]>;
    }
    export interface NativeSymbol { symbol: string; name: string; address: bigint }
    /** Snapshot one image module's export tables. Page cursors count raw symbol slots. */
    export function openNativeModule(module: "rtld" | "main" | "subsdk0" | "sdk"): { slots: number; page(start?: number, limit?: number): { symbols: NativeSymbol[]; next: number | null } } | null;
    /** Read the installed AI node metadata keyed by graph node name. */
    export function openAiDefinitions(): ResourceNode | null;
    /** Iterate ActorInfo records that declare both an actor identifier and a class name. */
    export function actorClassEntries(): IterableIterator<{ actor: string; className: string; record: ResourceNode }>;
    export type BymlNumber = { type: "i32" | "u32" | "f32" | "f64"; value: number } | { type: "i64" | "u64"; value: bigint };
    export type BymlValue = null | boolean | string | number | bigint | Uint8Array | BymlNumber | BymlValue[] | { [key: string]: BymlValue };
    export interface BymlDocument { root: BymlValue[] | { [key: string]: BymlValue }; version: number; littleEndian: boolean }
    /** Decode an editable tree, preserving numeric tags and document byte order. */
    export function decodeByml(bytes: Uint8Array): BymlDocument;
    /** Serialize an edited map or array, including new strings and fields. */
    export function encodeByml(doc: Pick<BymlDocument, "root"> & Partial<BymlDocument>): Uint8Array;
    export function cloneByml<T extends BymlValue>(value: T): T;
    export const byml: {
        i32(value: number): BymlNumber; u32(value: number): BymlNumber; f32(value: number): BymlNumber; f64(value: number): BymlNumber;
        i64(value: bigint | number): BymlNumber; u64(value: bigint | number): BymlNumber;
        isNumber(value: unknown): value is BymlNumber;
        value<T>(value: T): T extends BymlNumber ? T["value"] : T;
    };
    /** Read this resource's allocation limit in bytes, or null when unlisted. */
    export function resourceSize(path: string): number | null;
    /** Raise size-table entries for subsequent loads of the resource size table. */
    export function setResourceSizes(sizes: Map<string, number> | Record<string, number>): { path: string; updated: string[]; missing: string[] };
    /** Install decompressed content as a saved redirect, or pass memory: true for a session override. */
    export function writeResource(path: string, bytes: Uint8Array, options?: { memory?: boolean }): boolean;
    export function installSavedOverrides(): number;
    export function removeSavedOverride(path: string): boolean;
    export type ScalarType = "bool" | "i32" | "u32" | "i64" | "u64" | "ptr" | "f32" | "f64";
    type ScalarValue<T> = T extends "void" ? void : T extends "bool" ? boolean : T extends "i64" | "u64" | "ptr" ? bigint : number;
    /** Bind a verified scalar register signature. Each invocation runs in game.defer and resolves to the decoded result. */
    export function bindGameFunction<P extends readonly ScalarType[], R extends ScalarType | "void">(address: AddressLike, parameters: readonly [...P], returns: R): (...values: { [I in keyof P]: ScalarValue<P[I]> }) => Promise<ScalarValue<R>>;
    /** Bind a scalar AI method by class and virtual slot. Resolve a live graph instance inside the guest callback. */
    export function bindAiMethod<P extends readonly ScalarType[], R extends ScalarType | "void">(className: string, slot: number, parameters: readonly [...P], returns: R): (getReceiver: () => bigint, ...values: { [I in keyof P]: ScalarValue<P[I]> }) => Promise<ScalarValue<R>>;
    /** Sorted resource paths for all installed placement variants in one Banc directory. */
    export function listPlacementFiles(map: string): string[];
    /** Clone a placed actor under a new placement ID and position for future loads. Returns false if the file, template or override slot is unavailable. */
    export function addActorPlacement(path: string, templateId: bigint, newId: bigint, position: Vec3): boolean;
    /** Omit the identified placement from future loads of this resource. Returns false if absent or the override cannot be installed. */
    export function removeActorPlacement(path: string, id: bigint): boolean;
    /** Invoke a verified virtual slot in a guest callback. The actor occupies x0. args.x starts at x1. Rejects on changed identity or an unresolved slot. */
    export function callActorMethod(actor: Pick<ActorSnapshot, "ptr" | "vtable"> & { instanceId: number }, slot: number, args?: Parameters<NativeNamespace["call"]>[1]): Promise<ReturnType<NativeNamespace["call"]>>;
    export interface ObservedActor extends ActorSnapshot { instanceId: number; statusCode: number; lastSeenFrame: number; }
    /** Track observed actor instances until map change, pointer reuse, idle expiry, or stop(). */
    export function trackActors(options?: {
        maxIdleFrames?: number;
        onEnter?: (actor: ObservedActor) => void;
        onLeave?: (actor: ObservedActor, reason: "reused" | "idle" | "map") => void;
    }): { list(): ObservedActor[]; stop(): void };
    /** Edit a private decompressed copy synchronously, then install an in-memory virtual override. */
    export function editResource(path: string, edit: (data: Uint8Array) => void): boolean;
    export function listTables(): string[];
    /** Binary search an RSDB array sorted by string __RowId. Returns null when the row is absent. */
    export function findRecord(table: ResourceNode, rowId: string): ResourceNode | null;
    export function openTable(filename: string): ResourceNode | null;
    export function openByml(bytes: Uint8Array): ResourceNode;
    export function openPack(bytes: Uint8Array): Map<string | number, Uint8Array>;
    export function readResource(path: string): Uint8Array | null;
}
