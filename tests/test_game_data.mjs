import assert from "node:assert/strict";
import { zstdDecompressSync } from "node:zlib";
import { describeNativeLeaf, addActorPlacement, bindAiMethod, bindGameFunction, callActorMethod, editResource, openNativeModule, listPlacementFiles, openByml, removeActorPlacement, trackActors } from "../scripts_sdk/lib/greenhook-data.js";

// A one-element BYML array containing a signed integer.
for (const little of [false, true]) {
    const bytes = new Uint8Array(28);
    const view = new DataView(bytes.buffer);
    view.setUint16(0, little ? 0x5942 : 0x4259);
    view.setUint16(2, 7, little);
    view.setUint32(12, 16, little);
    bytes[16] = 0xc0;
    bytes[little ? 17 : 19] = 1;
    bytes[20] = 0xd1;
    const node = openByml(bytes);
    node.set(0, -123);
    assert.deepEqual([...node.entries()], [[0, -123]]);
    assert.throws(() => node.set(0, 2 ** 31), TypeError);
    assert.throws(() => node.set(1, 0), RangeError);
}

for (const length of [0, 1, 131072, 131073]) {
    let override;
    const source = new Uint8Array(length).fill(7);
    globalThis.game = { romfs: {
        readFile: () => new Uint8Array([0x28,0xb5,0x2f,0xfd,0x20]),
        decompressZstd: () => source.slice(),
        setVirtualFile: (_, bytes) => { override = bytes; return true; },
    }};
    assert.throws(() => editResource("test.zs", () => { throw Error("cancel"); }));
    assert.equal(override, undefined);
    assert.equal(editResource("test.zs", b => b.fill(42)), true);
    assert.deepEqual(new Uint8Array(zstdDecompressSync(override)), source.slice().fill(42));
}

const callbacks = {};
const changes = [];
globalThis.game = { graphics: { getFrame: () => 100 } };
for (const name of ["onActorUpdate", "onFrame", "onMapChange", "onUnload"]) {
    game[name] = fn => { callbacks[name] = fn; return () => delete callbacks[name]; };
}
const tracked = trackActors({maxIdleFrames: 2, onLeave: (_, why) => changes.push(why)});
callbacks.onActorUpdate({ptr: 123n, instanceId: 1});
callbacks.onActorUpdate({ptr: 123n, instanceId: 2});
assert.equal(tracked.list()[0].instanceId, 2);
callbacks.onFrame({frame: 102});
callbacks.onActorUpdate({ptr: 456n, instanceId: 3});
callbacks.onMapChange();
assert.deepEqual(changes, ["reused", "idle", "map"]);
callbacks.onUnload();
assert.deepEqual(Object.keys(callbacks), []);

// A Banc fixture with one actor, one u64 placement ID and a three-float translation.
for (const little of [true, false]) {
    const original = new Uint8Array(136), v = new DataView(original.buffer);
    const word = (at, value) => v.setUint32(at, value, little);
    function header(at, type, count) {
        original[at] = type;
        original[at + (little ? 1 : 3)] = count;
    }
    function field(at, key, type, offset) {
        original[at + (little ? 0 : 2)] = key;
        original[at + 3] = type;
        word(at + 4, offset);
    }
    v.setUint16(0, little ? 0x5942 : 0x4259);
    v.setUint16(2, 7, little);
    word(4, 16); word(12, 60);
    header(16, 0xc2, 3);
    [20, 27, 32, 42].forEach((n, i) => word(20 + 4 * i, n));
    original.set(new TextEncoder().encode("Actors\0Hash\0Translate\0"), 36);
    header(60, 0xc1, 1); field(64, 0, 0xc0, 72);
    header(72, 0xc0, 1); original[76] = 0xc1; word(80, 84);
    header(84, 0xc1, 2); field(88, 1, 0xd5, 104); field(96, 2, 0xc0, 112);
    v.setBigUint64(104, 35n, little);
    header(112, 0xc0, 3); original.fill(0xd2, 116, 119);
    let installed = original;
    globalThis.game = { romfs: {
        readFile: () => installed.slice(),
        listDir: () => ["B.bcett.byml.zs", "notes", "A.bcett.byml.zs"],
        setVirtualFile: (_, data) => { installed = data; return true; },
    }};
    assert.deepEqual(listPlacementFiles("Test"), ["Banc/Test/A.bcett.byml.zs", "Banc/Test/B.bcett.byml.zs"]);
    assert.throws(() => listPlacementFiles("../Test"));
    const pos = {x: -2, y: 4.5, z: 10}, id = (1n << 63n) + 99n;
    assert.equal(addActorPlacement("map", 35n, id, pos), true);
    const actors = openByml(installed).get("Actors");
    assert.equal(actors.length, 2);
    assert.equal(actors.get(1).get("Hash"), id);
    assert.deepEqual([...actors.get(1).get("Translate").entries()].map(([,n]) => n), Object.values(pos));
    assert.equal(actors.get(0).get("Translate").get(0), 0);
    const before = installed;
    assert.throws(() => addActorPlacement("map", 35n, id, pos), /already exists/);
    assert.equal(removeActorPlacement("map", 0n), false);
    assert.equal(installed, before);
    assert.equal(removeActorPlacement("map", 35n), true);
    assert.equal(openByml(installed).get("Actors").get(0).get("Hash"), id);
    assert.equal(removeActorPlacement("map", id), true);
    assert.equal(openByml(installed).get("Actors").length, 0);
}

const actor = {ptr: 55n, instanceId: 4, vtable: 1000n};
let current = {...actor}, method = 99n, calls = 0;
globalThis.game = {defer: cb => Promise.resolve().then(cb), actors: {
    inspect: () => current, getMethods: () => [method],
}};
globalThis.native = {call(address, args) {
    calls++;
    assert.equal(address, 99n);
    assert.deepEqual(args.x, [55n, 3n]);
    return {x0: 42n};
}};
assert.equal((await callActorMethod(actor, 2, {x: [3n]})).x0, 42n);
const pending = callActorMethod(actor, 2);
current = {...actor, instanceId: 5};
await assert.rejects(pending, /identity/);
current = actor; method = null;
await assert.rejects(callActorMethod(actor, 2), /Unresolved/);
assert.throws(() => callActorMethod(actor, 2, {x: Array(8).fill(0)}), /Too many/);
assert.equal(calls, 1);

globalThis.native = {hasFunction: address => address === 55n, call(addr, regs) {
    assert.equal(addr, 55n);
    assert.deepEqual(regs.x, [55n, 4294967295, 1]);
    assert.deepEqual(regs.f, [1.25]);
    assert.equal(regs.d[0], undefined);
    assert.equal(regs.d[1], 3.5);
    return {x0: 0xffffffffn};
}};
const bind = bindGameFunction(55n, ["ptr", "f32", "i32", "f64", "bool"], "i32");
assert.equal(await bind(55n, 1.25, -1, 3.5, true), -1);
assert.throws(() => bind(55n, 1, 2 ** 31, 2, false), RangeError);
assert.throws(() => bindGameFunction(55n, Array(9).fill("ptr"), "void"), /Stack/);
assert.throws(() => bindGameFunction(99n, [], "void"), /Unknown/);

const table = new Uint8Array(72), strings = new TextEncoder().encode("\0example\0");
const view = new DataView(table.buffer);
view.setUint32(24, 1, true);
view.setUint8(28, 0x12);
view.setUint16(30, 1, true);
view.setBigUint64(32, 0x100n, true);
globalThis.Greenhook = {gameBuild: "e0f3ccfa"};
globalThis.memory = {isMapped: () => true, readBytes: addr => addr === 0x800030c0n ? table : strings};
globalThis.native = {hasFunction: addr => addr === 0x80000100n, demangleSymbol: () => null};
const mod = openNativeModule("rtld");
assert.deepEqual(mod.page(0, 1), {symbols: [], next: 1});
assert.deepEqual(mod.page(1, 2), {symbols: [{symbol: "example", name: "example", address: 0x80000100n}], next: null});
assert.throws(() => mod.page(0, 257), RangeError);

const live = 1000n, vtable = 1500n, address = 2000n;
globalThis.native = {
    hasFunction: a => a === address,
    getAiClass: () => [{vtable, methods: [address]}],
    call: (a, args) => { assert.equal(a, address); assert.deepEqual(args.x, [live, 12]); return {x0: 9n}; },
};
globalThis.memory = {isMapped: a => a === live, readU64: () => vtable};
const aiMethod = bindAiMethod("Test", 0, ["u32"], "u32");
assert.equal(await aiMethod(() => live, 12), 9);
await assert.rejects(aiMethod(() => 0n, 12), /unavailable/);
globalThis.memory.readU64 = () => vtable + 8n;
await assert.rejects(aiMethod(() => live, 12), /different vtable/);
assert.throws(() => bindAiMethod("Test", 1, [], "void"), /Unresolved/);

const leafAddress = 0x82000200n;
const leaf = (first, second = 0xd65f03c0, third = 0) => {
    const bytes = new Uint8Array(12), view = new DataView(bytes.buffer);
    [first, second, third].forEach((op, i) => view.setUint32(i * 4, op, true));
    globalThis.memory = {isMapped: () => true, readBytes: () => bytes};
    return describeNativeLeaf(leafAddress);
};
globalThis.native = {hasFunction: addr => addr === leafAddress};
assert.match(leaf(0x91098000), /plus 608 bytes/);
assert.match(leaf(0xf9400800), /8-byte load from input x0 \+ 16/);
assert.match(leaf(0xbd400400), /s0 returns a 4-byte/);
assert.equal(leaf(0xd2800020), "x0 returns constant 1.");
assert.match(leaf(0x90000000, 0x91004000, 0xd65f03c0), /0x82000010/);
assert.match(leaf(0xf0ffffe0, 0x91004000, 0xd65f03c0), /0x81fff010/);
assert.match(leaf(0xd65f03c0), /without writing/);
assert.equal(leaf(0xf9400801), null); // writes x1, not the return register
assert.equal(leaf(0x91098000, 0xd503201f), null); // an instruction follows the add
assert.equal(describeNativeLeaf(0n), null);
console.log("Game data, AI method and native symbol tests passed");
