// Resource records are lazy so inspecting one actor does not decode the entire database.
const utf8 = new TextDecoder();

export function openByml(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const magic = view.getUint16(0);
    if (magic !== 0x5942 && magic !== 0x4259) throw new TypeError("Expected BYML");
    const le = magic === 0x5942;
    const u32 = at => view.getUint32(at, le);
    function u24(at) {
        return le ? view.getUint16(at, true) + view.getUint8(at + 2) * 65536
                  : view.getUint8(at) * 65536 + view.getUint16(at + 1, false);
    }
    function string(table, index) {
        if (!table || view.getUint8(table) !== 0xc2 || index >= u24(table + 1))
            throw new RangeError("Invalid BYML string index");
        const offset = table + 4 + 4 * index;
        const begin = table + u32(offset), end = table + u32(offset + 4);
        if (end <= begin || end > bytes.length || bytes[end - 1] !== 0)
            throw new RangeError("Invalid BYML string extent");
        return utf8.decode(bytes.subarray(begin, end - 1));
    }
    function value(type, at) {
        const word = u32(at);
        switch (type) {
        case 0xa0: return string(u32(8), word);
        case 0xa1: {
            const end = word + 4 + u32(word);
            if (end > bytes.length) throw new RangeError("Invalid BYML blob");
            return bytes.subarray(word + 4, end);
        }
        case 0xc0: case 0xc1: return node(word);
        case 0xd0: return Boolean(word);
        case 0xd1: return view.getInt32(at, le);
        case 0xd2: return view.getFloat32(at, le);
        case 0xd3: return word;
        case 0xd4: return view.getBigInt64(word, le);
        case 0xd5: return view.getBigUint64(word, le);
        case 0xd6: return view.getFloat64(word, le);
        case 0xff: return null;
        default: throw new TypeError("Unsupported BYML type 0x" + type.toString(16));
        }
    }
    function node(at) {
        const tag = view.getUint8(at), length = u24(at + 1);
        if (tag !== 0xc0 && tag !== 0xc1) throw new TypeError("Expected BYML array or map");
        const array = tag === 0xc0;
        const values = at + 4 + Math.ceil(length / 4) * 4;
        if ((array ? values + length * 4 : at + 4 + length * 8) > bytes.length)
            throw new RangeError("Invalid BYML container extent");
        function key(i) { return array ? i : string(u32(4), u24(at + 4 + 8 * i)); }
        function item(i) {
            return array ? value(bytes[at + 4 + i], values + 4 * i)
                         : value(bytes[at + 7 + 8 * i], at + 8 + 8 * i);
        }
        function slot(k) {
            if (array) return Number.isInteger(k) && k >= 0 && k < length ? k : -1;
            for (let i = 0; i < length; i++) if (key(i) === k) return i;
            return -1;
        }
        return {
            kind: array ? "array" : "map", length,
            get(k) {
                const i = slot(k);
                return i < 0 ? undefined : item(i);
            },
            set(k, v) {
                const i = slot(k);
                if (i < 0) throw new RangeError("Unknown BYML field");
                const type = array ? bytes[at + 4 + i] : bytes[at + 7 + 8 * i];
                const pos = array ? values + 4 * i : at + 8 + 8 * i;
                if (type === 0xd0 && typeof v === "boolean") view.setUint32(pos, Number(v), le);
                else if (type === 0xd1 && Number.isInteger(v) && v >= -2147483648 && v <= 2147483647) view.setInt32(pos, v, le);
                else if (type === 0xd3 && Number.isInteger(v) && v >= 0 && v <= 4294967295) view.setUint32(pos, v, le);
                else if (type === 0xd2 && Number.isFinite(v) && Number.isFinite(Math.fround(v))) view.setFloat32(pos, v, le);
                else if (type === 0xd6 && Number.isFinite(v)) view.setFloat64(u32(pos), v, le);
                else if (type === 0xd4 && typeof v === "bigint" && BigInt.asIntN(64,v) === v) view.setBigInt64(u32(pos), v, le);
                else if (type === 0xd5 && typeof v === "bigint" && BigInt.asUintN(64,v) === v) view.setBigUint64(u32(pos), v, le);
                else if (type === 0xa0 && typeof v === "string") {
                    const table = u32(8);
                    for (let j = 0; j < u24(table + 1); j++) {
                        if (string(table, j) === v) { view.setUint32(pos, j, le); return; }
                    }
                    throw new RangeError("String absent from this resource's string table");
                } else throw new TypeError("Value must fit the existing BYML field type");
            },
            *entries() { for (let i = 0; i < length; i++) yield [key(i), item(i)]; },
        };
    }
    return node(u32(12));
}

export function openPack(bytes) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (v.getUint32(0) !== 0x53415243) throw new TypeError("Expected SARC");
    const le = v.getUint16(6) === 0xfffe;
    const u16 = at => v.getUint16(at, le), u32 = at => v.getUint32(at, le);
    const fat = u16(4), count = u16(fat + 6), nodes = fat + u16(fat + 4);
    const fnt = nodes + 16 * count, names = fnt + u16(fnt + 4), base = u32(12);
    if (v.getUint32(fat) !== 0x53464154 || v.getUint32(fnt) !== 0x53464e54 ||
        names > base || base > bytes.length) throw new RangeError("Invalid SARC tables");
    const files = new Map();
    for (let i = 0; i < count; i++) {
        const at = nodes + 16 * i, attr = u32(at + 4);
        const begin = base + u32(at + 8), end = base + u32(at + 12);
        if (end < begin || end > bytes.length) throw new RangeError("Invalid SARC entry");
        let key = u32(at);
        if (attr >>> 24) {
            const off = names + (attr & 0xffffff) * 4;
            if (off >= base) throw new RangeError("Invalid SARC name");
            const nul = bytes.subarray(off, base).indexOf(0);
            if (nul < 0) throw new RangeError("Unterminated SARC name");
            key = utf8.decode(bytes.subarray(off, off + nul));
        }
        files.set(key, bytes.subarray(begin, end));
    }
    return files;
}

let dictionaries;
export function readResource(path) {
    const rom = globalThis.game.romfs;
    const bytes = rom.readFile(path);
    if (bytes === null || !path.endsWith(".zs")) return bytes;
    const size = [0, 1, 2, 4][bytes[4] & 3];
    let dict;
    if (size) {
        const start = bytes[4] & 32 ? 5 : 6;
        let id = 0;
        for (let i = 0; i < size; i++) id += bytes[start + i] * 256 ** i;
        if (id) {
            if (!dictionaries) {
                const archive = rom.readFile("Pack/ZsDic.pack.zs");
                if (!archive) throw new Error("Missing Pack/ZsDic.pack.zs");
                const found = new Map();
                for (const data of openPack(rom.decompressZstd(archive)).values()) {
                    const header = new DataView(data.buffer, data.byteOffset, data.byteLength);
                    if (data.length >= 8 && header.getUint32(0, true) === 0xec30a437)
                        found.set(header.getUint32(4, true), data);
                }
                dictionaries = found;
            }
            dict = dictionaries.get(id);
            if (!dict) throw new Error("Missing Zstd dictionary " + id);
        }
    }
    return rom.decompressZstd(bytes, dict);
}

export function listTables() {
    return globalThis.game.romfs.listDir("RSDB").filter(n => n.endsWith(".byml.zs")).sort();
}

export function openTable(filename) {
    if (!filename || /[/\\:]/.test(filename) || !filename.endsWith(".byml.zs"))
        throw new TypeError("Expected an RSDB filename from listTables()");
    const data = readResource("RSDB/" + filename);
    return data === null ? null : openByml(data);
}

// Zstd single-segment frame of raw blocks. Decoding preserves the exact resource size.
function storedFrame(bytes) {
    const block = 128 * 1024, count = Math.max(1, Math.ceil(bytes.length / block));
    const out = new Uint8Array(9 + bytes.length + count * 3);
    out.set([0x28, 0xb5, 0x2f, 0xfd, 0xa0]);
    new DataView(out.buffer).setUint32(5, bytes.length, true);
    let dst = 9;
    for (let i = 0; i < count; i++) {
        const part = bytes.subarray(i * block, (i + 1) * block);
        const head = part.length * 8 + Number(i + 1 === count);
        out.set([head & 255, (head >>> 8) & 255, head >>> 16], dst);
        out.set(part, dst + 3);
        dst += 3 + part.length;
    }
    return out;
}

export function editResource(path, edit) {
    const data = readResource(path);
    if (data === null) return false;
    const result = edit(data);
    if (result && typeof result.then === "function") throw new TypeError("editResource callback must be synchronous");
    return globalThis.game.romfs.setVirtualFile(path, path.endsWith(".zs") ? storedFrame(data) : data);
}

// These are observations from the shared transform update. A quiet actor can leave the set while still alive.
export function trackActors({ maxIdleFrames = 120, onEnter, onLeave } = {}) {
    if (!Number.isSafeInteger(maxIdleFrames) || maxIdleFrames < 1) throw new RangeError("Invalid maxIdleFrames");
    const live = new Map();
    const game = globalThis.game;
    let frame = game.graphics.getFrame(), stopped = false;
    function remove(ptr, actor, reason) {
        live.delete(ptr);
        if (onLeave) onLeave(actor, reason);
    }
    const offUpdate = game.onActorUpdate(actor => {
        if (stopped) return;
        const old = live.get(actor.ptr);
        if (old && old.instanceId !== actor.instanceId) remove(actor.ptr, old, "reused");
        if (stopped) return;
        actor = { ...actor, lastSeenFrame: frame };
        live.set(actor.ptr, actor);
        if ((!old || old.instanceId !== actor.instanceId) && onEnter) onEnter(actor);
    });
    let offFrame, offMap, offUnload;
    function stop() {
        stopped = true;
        offUpdate(); offFrame?.(); offMap?.(); offUnload?.();
        live.clear();
    }
    try {
        offFrame = game.onFrame(ev => {
            frame = ev.frame;
            for (const [ptr, actor] of live) {
                if (frame - actor.lastSeenFrame >= maxIdleFrames) remove(ptr, actor, "idle");
            }
        });
        offMap = game.onMapChange(() => {
            for (const [ptr, actor] of live) remove(ptr, actor, "map");
        });
        offUnload = game.onUnload(stop);
    } catch (err) { stop(); throw err; }
    return { list: () => [...live.values()], stop };
}

// The stock actor, pouch, challenge, event and map RSDBs are ordered by __RowId.
export function findRecord(table, rowId) {
    if (!table || table.kind !== "array" || typeof rowId !== "string")
        throw new TypeError("Expected an RSDB row array and a string row ID");
    let lo = 0, hi = table.length;
    while (lo < hi) {
        const mid = Math.floor((lo + hi) / 2);
        const row = table.get(mid), key = row.get("__RowId");
        if (typeof key !== "string") throw new TypeError("Table has no string __RowId index");
        if (key === rowId) return row;
        if (key < rowId) lo = mid + 1;
        else hi = mid;
    }
    return null;
}
