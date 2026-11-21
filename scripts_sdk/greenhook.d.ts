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
    /** Latest sampled Link statistics. Life and maxLife use quarter hearts. Missing fields are null. */
    getProgress(): { life: number | null; maxLife: number | null; rupees: number | null; skulltulaTokens: number | null; heartPieces: number | null } | null;
    getPos(): Vec3 | null;
    setPos(x: number, y: number, z: number): boolean;
    getFacing(): number | null;
    setFacing(radians: number): boolean;
    /** Quarter hearts, or -1 before the first sample. */
    getLife(): number;
    setLife(quarterHearts: number): boolean;
    /** Maximum life in quarter hearts, or -1 before the first sample. */
    getMaxLife(): number;
    setMaxLife(quarterHearts: number): boolean;
    /** Rupee count, or -1 before the first sample. */
    getRupees(): number;
    setRupees(amount: number): boolean;
    getAge(): "child" | "adult" | null;
    setAge(age: "child" | "adult"): boolean;
    getSpeedScale(): number;
    setSpeedScale(scale: number): void;
    getInvincible(): boolean | null;
    setInvincible(enabled: boolean | null): void;
    getDamageMultiplier(): number;
    setDamageMultiplier(mult: number): void;
    getFlight(): boolean | null;
    setFlight(enabled: boolean | null): void;
    getPrompt(): string;
    getPointers(): PlayerPointers;
}

interface InventoryItemEntry {
    label: string;
    name: string;
    kind: "toggle" | "quantity";
    count: number;
}

interface GameInventoryNamespace {
    list(): InventoryItemEntry[];
    get(itemName: string): number;
    has(itemName: string): boolean;
    add(itemName: string, amount?: number): boolean;
    remove(itemName: string, amount?: number): boolean;
    set(itemName: string, targetCount: number): boolean;
}

interface GameFlagsNamespace {
    /** Scan up to 128 slots. Pass next as the next cursor until it is null. A null page means unavailable. */
    list(kind: "bool" | "int" | "float" | "enum" | "string" | "uint", cursor?: number): { hashes: number[]; next: number | null } | null;
    hash(name: string): number;
    get(nameOrHash: string | number): boolean | number | string | null;
    getBool(nameOrHash: string | number): boolean | null;
    setBool(nameOrHash: string | number, value: boolean, persist?: boolean): boolean;
    getInt(nameOrHash: string | number): number | null;
    setInt(nameOrHash: string | number, value: number): boolean;
    getUInt(nameOrHash: string | number): number | null;
    setUInt(nameOrHash: string | number, value: number): boolean;
    getFloat(nameOrHash: string | number): number | null;
    setFloat(nameOrHash: string | number, value: number): boolean;
    getEnum(nameOrHash: string | number): number | null;
    setEnum(nameOrHash: string | number, value: string | number): boolean;
    getString(nameOrHash: string | number): string | null;
    setString(nameOrHash: string | number, value: string): boolean;
    getBool64(placementId: AddressLike): boolean | null;
    setBool64(placementId: AddressLike, value: boolean): boolean;
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
    getMap(): string | null;
    /** English display name for a map ID. With no argument, looks up the current map. */
    getAreaName(map?: string): string | null;
    isLoading(): boolean;
    isEventPlaying(): boolean;
    getMusic(): string;
    getTime(): WorldTime | null;
    setTime(hourOrSeconds: number, minute?: number): boolean;
    destinations(): DestinationEntry[];
    teleport(target: number | string | { banc: string; startPos?: string; bootEvent?: string }): boolean;
    triggerEvent(eventName: string, starterActorPtr?: AddressLike): boolean;
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
    getPose(): CameraPose | null;
    setOverride(pose: CameraOverridePose | null): void;
    setPlayerAngles(yawDeg: number, pitchDeg: number): void;
    clearPlayerAngles(): void;
    setFovScale(scale: number): void;
    getFovScale(): number;
    setSpeedScale(scale: number): void;
    getSpeedScale(): number;
    setDisableCentering(disable: boolean | null): void;
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
    getPlayer(): ActorSnapshot | null;
    /** Resolve a known span of actor virtual method slots. Each entry is a guest address or null. */
    getMethods(actor: AddressLike, firstSlot: number, count: number): (bigint | null)[] | null;
    inspect(ptr: AddressLike): ActorSnapshot | null;
    setPos(ptr: AddressLike, x: number, y: number, z: number): boolean;
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
    x: number;
    y: number;
    width: number;
    height: number;
    buttons: number;
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
    getState(): PadStateSnapshot;
    isDown(button: ButtonName | number): boolean;
    justPressed(button: ButtonName | number): boolean;
    justReleased(button: ButtonName | number): boolean;
    isKeyDown(key: string | number): boolean;
    getMouse(): MouseSnapshot;
    setOverride(spec: InputOverrideSpec | null): void;
    clearOverride(): void;
    setBlockGameInput(block: boolean): void;
}

interface GameAudioNamespace {
    getMusic(): string;
    setVolumeScale(scale: number): void;
    getVolumeScale(): number;
    setPitchScale(scale: number): void;
    getPitchScale(): number;
    setMute(muted: boolean): void;
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
    getFrame(): number;
    getFps(): number;
    getWindowSize(): { width: number; height: number };
    getOption(keyOrId: string | number): ControlOptionEntry | number | null;
    setOption(key: string, value: number): ControlOptionEntry | null;
    listOptions(): ControlOptionEntry[];
    setTextSpeedScale(scale: number): void;
    getTextSpeedScale(): number;
    setTextSizeScale(scale: number): void;
    getTextSizeScale(): number;
    setResolutionScale(percent: number): ControlOptionEntry | null;
    takeScreenshot(relPath?: string): Promise<ScreenshotResult>;
}

interface GameRomfsNamespace {
    redirect(romPath: string, hostFilePath: string): boolean;
    setVirtualFile(romPath: string, content: string | Uint8Array | ArrayBuffer): boolean;
    clearOverride(romPath: string): boolean;
    clearAllOverrides(): void;
    exists(romPath: string): boolean;
    decompressZstd(bytes: Uint8Array, dictionary?: Uint8Array): Uint8Array;
    readFile(romPath: string): Uint8Array | null;
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

    /** Run with a live guest context. Resolves to the callback result or rejects with its exception. */
    defer<T>(fn: () => T): Promise<T>;
    /** Runs each presented frame. dt is seconds. Call the returned function to unsubscribe. */
    onFrame(cb: (ev: FrameEvent) => void): Unsubscribable;
    /** Observe an actor at the shared transform-update hook. Copy values needed after the callback. */
    onActorUpdate(cb: (actor: ActorSnapshot & {instanceId: number; statusCode: number}) => void): Unsubscribable;
    onPlayerUpdate(cb: (ev: PlayerUpdateEvent) => void): Unsubscribable;
    onDamage(cb: (ev: DamageEvent) => void): Unsubscribable;
    onCameraUpdate(cb: (ev: CameraUpdateEvent) => void): Unsubscribable;
    onInput(cb: (ev: GreenhookInputEvent) => void): Unsubscribable;
    onEventRequest(cb: (ev: EventRequestEvent) => void): Unsubscribable;
    onTimelineStart(cb: (ev: TimelineStartEvent) => void): Unsubscribable;
    onSaveRequest(cb: (ev: SaveRequestEvent) => void): Unsubscribable;
    /** Receives the current and previous map IDs as positional strings. Call the returned function to unsubscribe. */
    onMapChange(cb: (map: string, prevMap: string) => void): Unsubscribable;
    onMenuTick(cb: (ev: MenuTickEvent) => void): Unsubscribable;
    onMenuFocus(cb: (ev: MenuFocusEvent) => void): Unsubscribable;
    onBgmChange(cb: (music: string, prevMusic: string) => void): Unsubscribable;
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
    /** Page through the installed executable's translated guest methods. Limits are 1–1024. */
    listFunctions(start?: number, limit?: number): { addresses: bigint[]; total: number; next: number | null } | null;
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
        set(key: string | number, value: string | number | boolean | bigint): void;
        get(key: string | number): ResourceValue | undefined;
        entries(): IterableIterator<[string | number, ResourceValue]>;
    }
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
