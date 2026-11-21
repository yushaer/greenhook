// @ts-check
import { game, ui, events } from "greenhook";

/**
 * Example 03: Event hooks (onDamage, onSaveRequest, onInput combo), flag watcher, and inter-mod event bus.
 */

let savedPos = null;
let comboCooldown = 0;

// 1. Reduce lethal hits to quarter-heart survival ("Focus Sash" mechanic)
game.onDamage((ev) => {
    if (ev.oldLife > 1 && ev.damage >= ev.oldLife) {
        ev.damage = ev.oldLife - 1;
        ui.toast("Endured lethal hit with 1/4 heart!", { color: "#ff8844" });
        events.emit("sandbox:enduredHit", ev.oldLife);
    }
});

// 2. Log save requests cleanly
game.onSaveRequest(({ reason, mode, isTransition }) => {
    console.info(`Save requested (reason=${reason}, mode=${mode}, transition=${isTransition})`);
});

// 3. L + R + D-Pad Up saves Link's position; L + R + D-Pad Down recalls Link there
const { L, R, UP, DOWN } = game.input.Buttons;
const COMBO_SAVE = L | R | UP;
const COMBO_LOAD = L | R | DOWN;

game.onInput((ev) => {
    if (comboCooldown > 0) { comboCooldown--; return; }
    const held = Number(ev.rawButtons & 0xFFFFn);
    if ((held & COMBO_SAVE) === COMBO_SAVE) {
        const pos = game.player.getPos();
        if (pos) {
            savedPos = pos;
            ui.toast(`Marked pos (${pos.x.toFixed(1)}, ${pos.y.toFixed(1)}, ${pos.z.toFixed(1)})`);
            comboCooldown = 20;
        }
    } else if ((held & COMBO_LOAD) === COMBO_LOAD && savedPos) {
        game.player.setPos(savedPos.x, savedPos.y, savedPos.z);
        ui.toast("Recalled marked position!");
        comboCooldown = 20;
    }
});

// 4. Watch PlayerStatus.Life flag directly
game.flags.watch("PlayerStatus.Life", (next, prev) => {
    if (prev != null && next !== prev) {
        console.log(`Life changed: ${prev} -> ${next}`);
    }
});

// 5. Subscribe to custom inter-mod event bus
events.on("sandbox:enduredHit", (oldLife) => {
    console.warn(`Inter-mod event: player survived lethal hit from ${oldLife}`);
});
