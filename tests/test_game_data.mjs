import assert from "node:assert/strict";
import { zstdDecompressSync } from "node:zlib";
import { editResource, openByml, trackActors } from "../scripts_sdk/lib/greenhook-data.js";

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
console.log("Game data and actor tracking tests passed");
