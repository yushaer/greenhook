// @ts-check
import { game, ui } from "greenhook";

/**
 * Example 01: Minimal, crisp HUD telemetry overlay using ui.draw.* & map change toasts.
 * Copy to <GameDir>\scripts\ to auto-load on launch, then press F8 after edits to hot-reload.
 */

game.onFrame(({ fps }) => {
    const pos = game.player.getPos();
    const map = game.world.getMap() || "Title / Menu";
    const time = game.world.getTime();
    const life = game.player.getLife();
    const maxLife = game.player.getMaxLife();
    const rupees = game.player.getRupees();

    const panelX = 18;
    const panelY = 18;
    const panelW = 330;
    const panelH = pos ? 108 : 54;

    ui.draw.rectFilled(panelX, panelY, panelW, panelH, { color: "#14100bc8", rounding: 6 });
    ui.draw.rect(panelX, panelY, panelW, panelH, { color: "#de982899", rounding: 6, thickness: 1.2 });

    const hdr = `${fps.toFixed(0)} FPS | ${map} | ${time ? time.formatted : "--:--"}`;
    ui.draw.text(panelX + 10, panelY + 8, hdr, { color: "#ffd866", size: 16 });

    if (pos) {
        const xyz = `XYZ: ${pos.x.toFixed(1)}, ${pos.y.toFixed(1)}, ${pos.z.toFixed(1)}`;
        ui.draw.text(panelX + 10, panelY + 32, xyz, { color: "#f0e8d6", size: 15 });

        const hpFrac = maxLife && maxLife > 0 ? Math.min(1, Math.max(0, (life ?? 0) / maxLife)) : 0;
        const hpLabel = life != null && maxLife != null
            ? `Hearts ${(life / 4).toFixed(2)}/${(maxLife / 4).toFixed(0)} | Rupees ${rupees ?? 0}`
            : "Health --";
        ui.draw.progressBar(panelX + 10, panelY + 60, panelW - 20, 22, hpFrac, {
            color: "#d94e4ee8",
            bgColor: "#261c18d0",
            text: hpLabel,
        });
    } else {
        ui.draw.text(panelX + 10, panelY + 30, "Waiting for gameplay...", { color: "#a89c88", size: 14 });
    }
});

game.onMapChange((map) => {
    const age = game.player.getAge();
    if (map) {
        ui.toast(`Map: ${game.world.getAreaName(map) ?? map} (${age ?? "?"})`, { color: "#de9828", durationMs: 2800 });
    }
});
