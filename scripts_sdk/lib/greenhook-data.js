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
            /** BYML type tag of an entry (0xd2 float, 0xc1 map, ...), or undefined when missing. */
            typeOf(k) {
                const i = slot(k);
                return i < 0 ? undefined : array ? bytes[at + 4 + i] : bytes[at + 7 + 8 * i];
            },
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

// ---- Whole-document BYML editing ----
// decodeByml returns plain values: strings, booleans, null, Uint8Array for binary data, arrays, plain
// objects for maps, and { type, value } for numbers so their exact BYML type survives a round trip
// (type is "i32", "f32", "u32", "i64", "u64" or "f64"). encodeByml writes such a tree back, including
// new entries, removed entries and new strings. Plain JS numbers encode as i32 when integral, else f32;
// plain bigints as u64 when nonnegative, else i64.
const NUMBER_TYPES = { i32: 0xd1, f32: 0xd2, u32: 0xd3, i64: 0xd4, u64: 0xd5, f64: 0xd6 };
const NUMBER_NAMES = { 0xd1: "i32", 0xd2: "f32", 0xd3: "u32", 0xd4: "i64", 0xd5: "u64", 0xd6: "f64" };

function isBymlNumber(v) {
    if (!v || typeof v !== "object" || Array.isArray(v) || v instanceof Uint8Array) return false;
    const keys = Object.keys(v);
    return keys.length === 2 && "type" in v && "value" in v && typeof v.type === "string" &&
        Object.prototype.hasOwnProperty.call(NUMBER_TYPES, v.type);
}

export const byml = {
    i32: value => ({ type: "i32", value }),
    f32: value => ({ type: "f32", value }),
    u32: value => ({ type: "u32", value }),
    i64: value => ({ type: "i64", value: BigInt(value) }),
    u64: value => ({ type: "u64", value: BigInt(value) }),
    f64: value => ({ type: "f64", value }),
    /** The number inside a decoded numeric value, or the value itself. */
    value: v => isBymlNumber(v) ? v.value : v,
    isNumber: isBymlNumber,
};

/** Deep copy of a decoded BYML value (the runtime has no structuredClone). */
export function cloneByml(v) {
    if (v instanceof Uint8Array) return v.slice();
    if (Array.isArray(v)) return v.map(cloneByml);
    if (v && typeof v === "object") {
        const out = {};
        for (const [k, x] of Object.entries(v)) out[k] = cloneByml(x);
        return out;
    }
    return v;
}

export function decodeByml(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const magic = view.getUint16(0);
    if (magic !== 0x5942 && magic !== 0x4259) throw new TypeError("Expected BYML");
    const le = magic === 0x5942;
    function walk(v, type) {
        if (type === 0xc0) return [...v.entries()].map(([i, x]) => walk(x, v.typeOf(i)));
        if (type === 0xc1) {
            const out = {};
            for (const [k, x] of v.entries()) out[k] = walk(x, v.typeOf(k));
            return out;
        }
        if (type === 0xa1) return v.slice();
        if (type in NUMBER_NAMES) return { type: NUMBER_NAMES[type], value: v };
        return v;
    }
    const root = openByml(bytes);
    return { version: view.getUint16(2, le), littleEndian: le, root: walk(root, root.kind === "array" ? 0xc0 : 0xc1) };
}

export function encodeByml(doc) {
    const le = doc.littleEndian !== false, version = doc.version ?? 7, root = doc.root;
    if (!root || typeof root !== "object" || isBymlNumber(root) || root instanceof Uint8Array)
        throw new TypeError("BYML root must be an array or map");
    const enc = new TextEncoder();
    const keySet = new Set(), strSet = new Set();
    function typeOf(v) {
        if (v === null || v === undefined) return 0xff;
        if (typeof v === "string") return 0xa0;
        if (typeof v === "boolean") return 0xd0;
        if (typeof v === "number") {
            if (!Number.isFinite(v)) throw new TypeError("BYML numbers must be finite");
            return Number.isInteger(v) && v >= -2147483648 && v <= 2147483647 ? 0xd1 : 0xd2;
        }
        if (typeof v === "bigint") return v >= 0n ? 0xd5 : 0xd4;
        if (v instanceof Uint8Array) return 0xa1;
        if (Array.isArray(v)) return 0xc0;
        if (isBymlNumber(v)) return NUMBER_TYPES[v.type];
        if (typeof v === "object") return 0xc1;
        throw new TypeError("Unsupported BYML value: " + typeof v);
    }
    (function collect(v) {
        const t = typeOf(v);
        if (t === 0xa0) strSet.add(v);
        else if (t === 0xc0) v.forEach(collect);
        else if (t === 0xc1) for (const [k, x] of Object.entries(v)) { keySet.add(k); collect(x); }
    })(root);
    // Keys and strings are sorted by UTF-8 bytes; readers binary-search the key table.
    const byteOrder = (a, b) => {
        const x = enc.encode(a), y = enc.encode(b);
        for (let i = 0; i < Math.min(x.length, y.length); i++) if (x[i] !== y[i]) return x[i] - y[i];
        return x.length - y.length;
    };
    const keys = [...keySet].sort(byteOrder), strs = [...strSet].sort(byteOrder);
    const keyIndex = new Map(keys.map((k, i) => [k, i])), strIndex = new Map(strs.map((s, i) => [s, i]));

    let buf = new Uint8Array(1 << 16), view = new DataView(buf.buffer), pos = 0;
    function reserve(n) {
        if (pos + n <= buf.length) return;
        let size = buf.length;
        while (size < pos + n) size *= 2;
        const next = new Uint8Array(size);
        next.set(buf);
        buf = next;
        view = new DataView(buf.buffer);
    }
    const align = n => { const p = Math.ceil(pos / n) * n; reserve(p - pos); pos = p; };
    function u24(at, v) {
        if (v > 0xffffff) throw new RangeError("BYML container too large");
        if (le) { view.setUint16(at, v & 0xffff, true); view.setUint8(at + 2, v >>> 16); }
        else { view.setUint8(at, v >>> 16); view.setUint16(at + 1, v & 0xffff, false); }
    }
    function stringTable(list) {
        if (!list.length) return 0;
        align(4);
        const at = pos, head = 4 + 4 * (list.length + 1);
        reserve(head);
        pos += head;
        view.setUint8(at, 0xc2);
        u24(at + 1, list.length);
        list.forEach((s, i) => {
            const b = enc.encode(s);
            view.setUint32(at + 4 + 4 * i, pos - at, le);
            reserve(b.length + 1);
            buf.set(b, pos);
            buf[pos + b.length] = 0;
            pos += b.length + 1;
        });
        view.setUint32(at + 4 + 4 * list.length, pos - at, le);
        align(4);
        return at;
    }
    // Identical containers and 64-bit values are written once and shared, as the game's own files
    // do. The game reserves memory per file from its resource size table, so size matters.
    const canonIds = new Map(), memo = new WeakMap(), written = new Map();
    const bits = new DataView(new ArrayBuffer(8));
    function canon(v, t) {
        switch (t) {
        case 0xa0: return "s" + strIndex.get(v);
        case 0xd0: return v ? "T" : "F";
        case 0xff: return "N";
        case 0xd1: case 0xd2: case 0xd3: {
            const n = isBymlNumber(v) ? v.value : v;
            if (t === 0xd2) { bits.setFloat32(0, n); return "f" + bits.getUint32(0); }
            return (t === 0xd1 ? "i" : "u") + n;
        }
        case 0xa1: return null; // binary blobs are rare; never shared
        }
        if (v && typeof v === "object" && memo.has(v)) return memo.get(v);
        let key;
        if (t === 0xc0) key = "A" + v.map(x => { const xt = typeOf(x); return canon(x, xt) ?? "?"; }).join(",");
        else if (t === 0xc1) key = "M" + Object.entries(v).sort((a, b) => keyIndex.get(a[0]) - keyIndex.get(b[0]))
            .map(([k, x]) => { const xt = typeOf(x); return keyIndex.get(k) + "=" + (canon(x, xt) ?? "?"); }).join(",");
        else {
            const n = isBymlNumber(v) ? v.value : v;
            if (t === 0xd6) { bits.setFloat64(0, Number(n)); key = "D" + bits.getBigUint64(0); }
            else key = (t === 0xd4 ? "L" : "U") + BigInt(n);
        }
        if (key.includes("?")) key = null; // contains an unshareable blob
        let id = key === null ? null : canonIds.get(key);
        if (key !== null && id === undefined) { id = "#" + canonIds.size; canonIds.set(key, id); }
        if (v && typeof v === "object") memo.set(v, id);
        return id;
    }
    // Returns the parent's 32-bit word for a value, or null for inline numbers.
    function word(v, t) {
        if (t === 0xd1 || t === 0xd2 || t === 0xd3) return null;
        if (t === 0xa0 || t === 0xd0 || t === 0xff) return writeOut(v, t);
        const id = canon(v, t);
        if (id !== null && written.has(id)) return written.get(id);
        const at = writeOut(v, t);
        if (id !== null) written.set(id, at);
        return at;
    }
    // Writes the out-of-line part of a value.
    function writeOut(v, t) {
        switch (t) {
        case 0xa0: return strIndex.get(v);
        case 0xd0: return v ? 1 : 0;
        case 0xd1: case 0xd2: case 0xd3: return null;
        case 0xff: return 0;
        case 0xd4: case 0xd5: case 0xd6: {
            align(4); // the game's own files align 64-bit values to 4 bytes
            const at = pos, n = isBymlNumber(v) ? v.value : v;
            reserve(8);
            pos += 8;
            if (t === 0xd6) view.setFloat64(at, Number(n), le);
            else if (t === 0xd4) view.setBigInt64(at, BigInt.asIntN(64, BigInt(n)), le);
            else view.setBigUint64(at, BigInt.asUintN(64, BigInt(n)), le);
            return at;
        }
        case 0xa1: {
            align(4);
            const at = pos;
            reserve(4 + v.length);
            view.setUint32(at, v.length, le);
            buf.set(v, at + 4);
            pos += 4 + v.length;
            return at;
        }
        default: return node(v, t);
        }
    }
    function putInline(at, v, t) {
        const n = isBymlNumber(v) ? v.value : v;
        if (typeof n !== "number" || !Number.isFinite(n)) throw new TypeError("BYML 32-bit numbers must be finite");
        if (t === 0xd1) view.setInt32(at, n, le);
        else if (t === 0xd2) view.setFloat32(at, n, le);
        else view.setUint32(at, n >>> 0, le);
    }
    function node(v, t) {
        align(4);
        const at = pos;
        if (t === 0xc0) {
            const n = v.length, values = 4 + Math.ceil(n / 4) * 4;
            reserve(values + 4 * n);
            pos += values + 4 * n;
            view.setUint8(at, 0xc0);
            u24(at + 1, n);
            v.forEach((x, i) => {
                const xt = typeOf(x);
                buf[at + 4 + i] = xt;
                const w = word(x, xt);
                if (w === null) putInline(at + values + 4 * i, x, xt);
                else view.setUint32(at + values + 4 * i, w, le);
            });
        } else {
            const entries = Object.entries(v).sort((a, b) => keyIndex.get(a[0]) - keyIndex.get(b[0]));
            reserve(4 + 8 * entries.length);
            pos += 4 + 8 * entries.length;
            view.setUint8(at, 0xc1);
            u24(at + 1, entries.length);
            entries.forEach(([k, x], i) => {
                const xt = typeOf(x), e = at + 4 + 8 * i;
                u24(e, keyIndex.get(k));
                buf[e + 3] = xt;
                const w = word(x, xt);
                if (w === null) putInline(e + 4, x, xt);
                else view.setUint32(e + 4, w, le);
            });
        }
        return at;
    }

    reserve(16);
    pos = 16;
    buf.set(le ? [0x59, 0x42] : [0x42, 0x59]);
    view.setUint16(2, version, le);
    // Compute each offset before writing it: writing can grow (replace) the buffer and its view.
    const keyTable = stringTable(keys);
    const strTable = stringTable(strs);
    // The game's writer stores every 64-bit value in one pool between the string table and the
    // node tree, in first-use order; the game does not load files laid out otherwise.
    (function pool(v) {
        const t = typeOf(v);
        if (t === 0xc0) v.forEach(pool);
        else if (t === 0xc1) Object.entries(v).sort((a, b) => keyIndex.get(a[0]) - keyIndex.get(b[0])).forEach(([, x]) => pool(x));
        else if (t === 0xd4 || t === 0xd5 || t === 0xd6) word(v, t);
    })(root);
    const rootAt = node(root, typeOf(root));
    view.setUint32(4, keyTable, le);
    view.setUint32(8, strTable, le);
    view.setUint32(12, rootAt, le);
    return buf.slice(0, pos);
}

// ---- Resource size table ----
// The game allocates each resource from System/Resource/*.rsizetable.zs (CRC32 of the path without
// ".zs" -> bytes). An entry is the stock decompressed size plus a small fixed overhead, so a file that
// grows is truncated on load. The table is read when the game starts: patch it from a script's top
// level and restart once before serving larger files.
const CRC_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
})();
function crc32(s) {
    let c = 0xffffffff;
    for (const b of new TextEncoder().encode(s)) c = CRC_TABLE[(c ^ b) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}

function sizeTablePath() {
    const name = globalThis.game.romfs.listDir("System/Resource").find(n => n.endsWith(".rsizetable.zs"));
    if (!name) throw new Error("No resource size table in System/Resource");
    return "System/Resource/" + name;
}

function parseSizeTable(bytes) {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (new TextDecoder().decode(bytes.subarray(0, 6)) !== "RESTBL") throw new TypeError("Expected RESTBL");
    const strSize = v.getUint32(10, true), crcCount = v.getUint32(14, true), nameCount = v.getUint32(18, true);
    const names = 22 + 8 * crcCount;
    if (names + nameCount * (strSize + 4) > bytes.length) throw new RangeError("Invalid RESTBL extents");
    // Returns the byte offset of the size word for a RomFS path, or -1.
    function slot(path) {
        const key = path.replace(/\.zs$/, "");
        for (let i = 0; i < nameCount; i++) {
            const at = names + i * (strSize + 4);
            const end = bytes.subarray(at, at + strSize).indexOf(0);
            if (new TextDecoder().decode(bytes.subarray(at, end < 0 ? at + strSize : at + end)) === key) return at + strSize;
        }
        const h = crc32(key);
        let lo = 0, hi = crcCount;
        while (lo < hi) {
            const mid = (lo + hi) >>> 1, c = v.getUint32(22 + 8 * mid, true);
            if (c === h) return 22 + 8 * mid + 4;
            if (c < h) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    return { view: v, slot };
}

/** Allocation size the game uses for a RomFS path, or null when the table has no entry. */
export function resourceSize(path) {
    const t = parseSizeTable(readResource(sizeTablePath()));
    const at = t.slot(path);
    return at < 0 ? null : t.view.getUint32(at, true);
}

/**
 * Raises allocation sizes: `sizes` maps RomFS paths to minimum byte counts. Serves the patched table
 * through writeResource and returns { path, updated, missing }. Takes effect for a table the game reads
 * afterwards, which is normally at startup.
 */
export function setResourceSizes(sizes) {
    const path = sizeTablePath();
    globalThis.game.romfs.clearOverride(path);
    const bytes = readResource(path).slice();
    const t = parseSizeTable(bytes);
    const updated = [], missing = [];
    for (const [p, size] of sizes instanceof Map ? sizes : Object.entries(sizes)) {
        if (!Number.isSafeInteger(size) || size < 0 || size > 0xffffffff) throw new RangeError("Invalid size for " + p);
        const at = t.slot(p);
        if (at < 0) { missing.push(p); continue; }
        if (t.view.getUint32(at, true) < size) { t.view.setUint32(at, size, true); updated.push(p); }
    }
    if (!writeResource(path, bytes)) throw new Error("Could not override " + path);
    return { path, updated, missing };
}

/**
 * Serves `bytes` (decompressed content) for RomFS `path`. By default the data is written to
 * scripts_data/romfs-overrides/<path> and the game path is redirected to that host file, so every
 * loader thread opens a real file (the asynchronous area loader rejects in-memory handles).
 * Pass { memory: true } for a setVirtualFile override instead.
 */
export function writeResource(path, bytes, { memory = false } = {}) {
    if (typeof path !== "string" || !path || /(^|[\\/])\.\.([\\/]|$)|^[\\/]|:/.test(path))
        throw new TypeError("Expected a relative RomFS path");
    const { romfs } = globalThis.game;
    const data = path.endsWith(".zs") ? storedFrame(bytes) : bytes;
    if (memory) return romfs.setVirtualFile(path, data);
    const { fs, Greenhook } = globalThis;
    const rel = "romfs-overrides/" + path.replace(/\\/g, "/");
    fs.mkdir(rel.slice(0, rel.lastIndexOf("/")));
    fs.writeBytes(rel, data);
    romfs.clearOverride(path);
    return romfs.redirect(path, Greenhook.dataDir.replace(/[\\/]+$/, "") + "\\" + rel.replace(/\//g, "\\"));
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
