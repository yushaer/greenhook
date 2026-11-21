// Run with the SDK CLI's file command while Longshot is presenting frames.
(async () => {
    const {openAiDefinitions, openNativeModule, describeNativeLeaf} = await import("greenhook/data");
    const ai = openAiDefinitions();
    if (!ai) throw new Error("The AI database is unavailable. Open the game and retry.");

    const dir = "method-reference/" + Greenhook.gameBuild + "-" + Date.now();
    const hex = n => "0x" + n.toString(16);
    const leaves = new Map();
    const known = new Map(Object.entries(native.addresses).map(([name, addr]) => [addr, name]));
    const save = (file, text) => {
        if (!fs.writeText(dir + "/" + file, text)) throw new Error("Could not write " + file);
    };
    const index = ["# Greenhook method reference", "",
        "Generated with Greenhook " + Greenhook.version + " for build " + Greenhook.gameBuild + ".",
        "", "Open this folder in VS Code and use Ctrl+Shift+F to search across files. " +
        "AI pages give graph inputs, defaults, vtables and zero-based virtual slots. Guest body notes describe recognized instructions in the stock image. Host replacements may behave differently. A blank entry means the body has not been characterized. Native pages give demangled names, raw symbols and guest addresses. " +
        "Overloads and aliases have separate entries. Only slots with verified identities are labeled. " +
        "Graph inputs describe fields of the running graph instance. They cannot be passed as positional native arguments.",
        "", "## AI graph nodes", ""];
    const field = v => {
        if (typeof v === "bigint") return String(v) + "n";
        if (v instanceof Uint8Array) return "[" + v.length + " bytes]";
        if (v && typeof v === "object" && "kind" in v) {
            const entries = [...v.entries()];
            return v.kind === "array" ? "[" + entries.map(([,x]) => field(x)).join(", ") + "]" :
                "{" + entries.map(([k,x]) => k + ": " + field(x)).join(", ") + "}";
        }
        return JSON.stringify(v) ?? "";
    };
    /** @param {unknown} v @returns {v is import("greenhook/data").ResourceNode} */
    const isNode = v => v !== null && typeof v === "object" && Object.prototype.hasOwnProperty.call(v, "kind");
    let page = [], first = "", last = "", nodes = 0, matched = 0, tables = 0, slots = 0;
    function flushAi() {
        const file = "ai-" + String(Math.ceil(nodes / 48)).padStart(3, "0") + ".md";
        save(file, "# AI nodes\n\n" + page.join("\n").trimEnd() + "\n");
        index.push("- [" + first + " through " + last + "](" + file + ")");
        page = [];
    }
    for (const [key, record] of ai.entries()) {
        if (typeof key !== "string" || !isNode(record)) throw new Error("Unexpected AI definition");
        if (!page.length) first = key;
        last = key;
        nodes++;
        page.push("## " + key, "");
        const tags = record.get("Tags"), inputs = record.get("Inputs");
        if (isNode(tags)) page.push("Tags: " + [...tags.entries()].map(([,v]) => v).join(", "), "");
        const params = [];
        if (isNode(inputs)) for (const [type, fields] of inputs.entries()) {
            if (!isNode(fields)) continue;
            for (const [name, meta] of fields.entries()) {
                const initial = isNode(meta) ? meta.get("default") : undefined;
                params.push("- `" + type + " " + name + "`" + (initial === undefined ? "" : " (default `" + field(initial) + "`)"));
            }
        }
        page.push(...(params.length ? ["Graph inputs", "", ...params] : ["No graph inputs listed."]), "");
        const classes = native.getAiClass(key);
        if (!classes.length) page.push("Native vtable unresolved.", "");
        else matched++;
        for (const cls of classes) {
            tables++;
            slots += cls.methods.length;
            page.push("Vtable `" + hex(cls.vtable) + "`", "", "| Slot | Guest address | Verified identity | Guest body |", "| ---: | --- | --- | --- |");
            cls.methods.forEach((address, slot) => {
                if (address !== null && !leaves.has(address)) leaves.set(address, describeNativeLeaf(address));
                page.push("| " + slot + " | " + (address === null ? "Unresolved" : "`" + hex(address) + "`") + " | " +
                    (slot === 19 ? "Class name getter" : address === null ? "" : known.get(address) ?? "") + " | " +
                    (leaves.get(address) ?? "") + " |");
            });
            page.push("");
        }
        if (nodes % 48 === 0) flushAi();
        if (nodes % 8 === 0) await nextFrame();
    }
    if (page.length) flushAi();
    index.push("", "## Named native exports", "");
    let exports = 0;
    /** @type {("rtld"|"main"|"subsdk0"|"sdk")[]} */
    const modules = ["rtld", "main", "subsdk0", "sdk"];
    for (const module of modules) {
        const lib = openNativeModule(module);
        if (!lib) throw new Error("Could not open " + module);
        let cursor = 0, batch = [], part = 0, count = 0;
        function flushNative() {
            const file = module + "-" + String(++part).padStart(3, "0") + ".tsv";
            save(file, "address\tname\tsymbol\n" + batch.join("\n") + "\n");
            index.push("- [" + file + "](" + file + ")");
            batch = [];
        }
        for (;;) {
            const result = lib.page(cursor, 128);
            for (const s of result.symbols) {
                batch.push([hex(s.address), s.name, s.symbol].join("\t"));
                count++;
                if (batch.length === 512) flushNative();
            }
            if (result.next === null) break;
            cursor = result.next;
            await nextFrame();
        }
        if (batch.length) flushNative();
        index.push("", module + " export entries: " + count, "");
        exports += count;
    }
    index.splice(3, 0, "", nodes + " AI definitions, " + matched + " resolved names, " + tables +
        " vtables, " + slots + " virtual slot entries, " + exports + " named native export entries.");
    save("README.md", index.join("\n").trimEnd() + "\n");
    console.info("Method reference saved in " + Greenhook.dataDir + "/" + dir);
})().catch(error => console.error("Method reference export failed", String(error)));
