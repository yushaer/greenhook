// @ts-check
import { game, memory, native, ui } from "greenhook";

/**
 * Example 04: Low-level memory access, AArch64 native function call & hook (MinHook), and virtual RomFS.
 */

// 1. Verify known OoT Build e0f3ccfa symbol availability
const getTimeAddr = native.addresses.GetTime;
console.info(`GetTime (0x${getTimeAddr.toString(16)}) available: ${native.hasFunction(getTimeAddr)}`);

// 2. Allocate guest memory scratch buffer, write/read CString, and free cleanly
const scratch = memory.alloc(64);
if (scratch && memory.isMapped(scratch, 64)) {
    memory.writeCString(scratch, "Greenhook Arena OK");
    console.log("Arena round-trip:", memory.readCString(scratch));
    memory.free(scratch);
}

// 3. Install a virtual RomFS file readable via nn::fs (and game.romfs.readFile)
const virtualRomPath = "longshot_js/hello.json";
game.romfs.setVirtualFile(virtualRomPath, JSON.stringify({ engine: "quickjs-ng", ok: true }));
const virtualBytes = game.romfs.readFile(virtualRomPath);
if (virtualBytes) {
    const decoded = new TextDecoder().decode(virtualBytes);
    console.log("Virtual RomFS content:", decoded);
}

// 4. Hook native AArch64 function cleanly and count calls (before/after ctx inspection)
let calls = 0;
let lastSec = 0;
const hookId = native.hasFunction(getTimeAddr)
    ? native.hook(getTimeAddr, {
        after() {
            calls++;
            lastSec = game.world.getTime()?.seconds ?? 0;
        },
    })
    : 0;

if (hookId) {
    console.info(`Installed MinHook #${hookId} on GetTime`);
}

game.onFrame(({ frame }) => {
    if (frame % 180 === 0 && hookId) {
        ui.draw.text(18, 136, `GetTime calls: ${calls} (last=${lastSec})`, { color: "#80e8ff", size: 14 });
    }
});

game.onUnload(() => {
    if (hookId) native.unhook(hookId);
    console.info("04-native-and-romfs unloaded cleanly.");
});
