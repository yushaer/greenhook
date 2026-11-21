#!/usr/bin/env node
/**
 * Zero-dependency CLI & interactive REPL for the Longshot JS scripting runtime
 * over the Windows named pipe (default: \\.\pipe\longshot-js).
 *
 * Usage:
 *   node longshot-js-cli.mjs [--pipe <name>] status
 *   node longshot-js-cli.mjs [--pipe <name>] eval "<js expression>"
 *   node longshot-js-cli.mjs [--pipe <name>] file <script.js>
 *   node longshot-js-cli.mjs [--pipe <name>] reload
 *   node longshot-js-cli.mjs [--pipe <name>] list
 *   node longshot-js-cli.mjs [--pipe <name>] log [--since 0]
 *   node longshot-js-cli.mjs [--pipe <name>] repl
 */
import net from "node:net";
import fs from "node:fs";
import readline from "node:readline";
import path from "node:path";

function parseArgs(argv) {
    let pipeName = "longshot-js";
    const rest = [];
    for (let i = 0; i < argv.length; i++) {
        if ((argv[i] === "--pipe" || argv[i] === "-p") && i + 1 < argv.length) {
            pipeName = argv[++i];
        } else {
            rest.push(argv[i]);
        }
    }
    const pipePath = pipeName.startsWith("\\\\.\\pipe\\") ? pipeName : `\\\\.\\pipe\\${pipeName}`;
    return { pipePath, rest };
}

function sendRpc(pipePath, payload) {
    return new Promise((resolve, reject) => {
        const sock = net.createConnection(pipePath, () => {
            sock.write(JSON.stringify(payload) + "\n");
        });
        let buf = "";
        sock.setEncoding("utf8");
        sock.on("data", (chunk) => {
            buf += chunk;
            const nl = buf.indexOf("\n");
            if (nl >= 0) {
                const line = buf.slice(0, nl).trim();
                sock.destroy();
                try {
                    resolve(JSON.parse(line));
                } catch (e) {
                    reject(new Error(`Invalid JSON reply: ${line} (${e.message})`));
                }
            }
        });
        sock.on("error", (err) => reject(err));
    });
}

function printHelp() {
    console.log(`Longshot JS Control CLI
Commands:
  status                    Query runtime & frame status
  eval <code...>            Evaluate JS expression/statement in live game context
  file <path.js>            Read and evaluate a local JS file in live game context
  reload                    Hot-reload all scripts (F8 equivalent)
  list                      List discovered scripts and status/errors
  log [--since N]           Fetch recent console log ring lines
  repl                      Start interactive stdin JS REPL
Options:
  --pipe <name>             Named pipe (default: longshot-js)
  -h, --help                Show this help`);
}

async function main() {
    const { pipePath, rest } = parseArgs(process.argv.slice(2));
    if (rest.length === 0 || rest[0] === "-h" || rest[0] === "--help") {
        printHelp();
        return;
    }

    const cmd = rest[0];
    let id = 1;

    if (cmd === "status") {
        console.log(JSON.stringify(await sendRpc(pipePath, { id: id++, cmd: "status" }), null, 2));
    } else if (cmd === "list") {
        console.log(JSON.stringify(await sendRpc(pipePath, { id: id++, cmd: "js_list" }), null, 2));
    } else if (cmd === "reload") {
        console.log(JSON.stringify(await sendRpc(pipePath, { id: id++, cmd: "js_reload" }), null, 2));
    } else if (cmd === "log") {
        let since = 0;
        const si = rest.indexOf("--since");
        if (si >= 0 && si + 1 < rest.length) since = parseInt(rest[si + 1], 10) || 0;
        const r = await sendRpc(pipePath, { id: id++, cmd: "js_log", since });
        const lines = r?.result?.lines ?? [];
        for (const l of lines) {
            console.log(`[${l.seq}][${l.source}] ${l.text}`);
        }
        if (lines.length === 0) console.log("(no new log lines)");
    } else if (cmd === "eval") {
        const code = rest.slice(1).join(" ");
        if (!code) throw new Error("eval requires JS code");
        const r = await sendRpc(pipePath, { id: id++, cmd: "js_eval", code, filename: "<cli>" });
        console.log(JSON.stringify(r, null, 2));
        if (r?.result && r.result.ok === false) process.exitCode = 1;
    } else if (cmd === "file") {
        const filePath = rest[1];
        if (!filePath) throw new Error("file requires a script path");
        const code = fs.readFileSync(filePath, "utf8");
        const r = await sendRpc(pipePath, {
            id: id++,
            cmd: "js_eval",
            code,
            filename: path.basename(filePath),
        });
        console.log(JSON.stringify(r, null, 2));
        if (r?.result && r.result.ok === false) process.exitCode = 1;
    } else if (cmd === "repl") {
        console.log(`Connected target: ${pipePath} (.reload, .list, .exit)`);
        const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: "ls> " });
        rl.prompt();
        for await (const raw of rl) {
            const line = raw.trim();
            if (!line) { rl.prompt(); continue; }
            if (line === ".exit" || line === ".quit") break;
            try {
                if (line === ".reload") {
                    console.log(JSON.stringify(await sendRpc(pipePath, { id: id++, cmd: "js_reload" })));
                } else if (line === ".list") {
                    console.log(JSON.stringify(await sendRpc(pipePath, { id: id++, cmd: "js_list" }), null, 2));
                } else {
                    const r = await sendRpc(pipePath, { id: id++, cmd: "js_eval", code: line, filename: "<repl>" });
                    console.log(JSON.stringify(r.result ?? r));
                }
            } catch (e) {
                console.error("Pipe error:", e.message);
            }
            rl.prompt();
        }
        rl.close();
    } else {
        throw new Error(`Unknown command '${cmd}'. Run --help for usage.`);
    }
}

main().catch((err) => {
    console.error(err.message || err);
    process.exit(1);
});
