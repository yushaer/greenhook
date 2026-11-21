// @ts-check
import { game, ui, fs } from "greenhook";

/**
 * Example 02: Interactive Dear ImGui Sandbox Control Panel with JSON persistence.
 */

const CONFIG_PATH = "sandbox-panel.json";
const config = fs.readJson(CONFIG_PATH);
const saved = /** @type {Record<string, unknown>} */ (config && typeof config === "object" ? config : {});
const state = {
    speedScale: typeof saved.speedScale === "number" ? saved.speedScale : 1.0,
    fovScale: typeof saved.fovScale === "number" ? saved.fovScale : 1.0,
    damageMult: typeof saved.damageMult === "number" ? saved.damageMult : 1.0,
    audioPitch: typeof saved.audioPitch === "number" ? saved.audioPitch : 1.0,
    invincible: Boolean(saved.invincible),
    flight: Boolean(saved.flight),
    muted: Boolean(saved.muted),
};

function applyState() {
    game.player.setSpeedScale(state.speedScale);
    game.camera.setFovScale(state.fovScale);
    game.player.setDamageMultiplier(state.damageMult);
    game.audio.setPitchScale(state.audioPitch);
    game.player.setInvincible(state.invincible ? true : null);
    game.player.setFlight(state.flight ? true : null);
    game.audio.setMute(state.muted);
    fs.writeJson(CONFIG_PATH, state);
}

applyState();

const win = ui.window("sandbox_panel", "Greenhook Sandbox", { x: 42, y: 145, w: 340, open: true });

win.label("Physics & Combat")
   .slider("speed", "Move Speed", 0.25, 3.0, state.speedScale, (v) => { state.speedScale = v; applyState(); })
   .slider("fov", "FOV Scale", 0.6, 1.5, state.fovScale, (v) => { state.fovScale = v; applyState(); })
   .slider("dmg", "Damage Taken", 0.0, 4.0, state.damageMult, (v) => { state.damageMult = v; applyState(); })
   .checkbox("inv", "Invincible Hearts", state.invincible, (v) => { state.invincible = v; applyState(); })
   .sameLine()
   .checkbox("flt", "Free Flight", state.flight, (v) => { state.flight = v; applyState(); })
   .separator()
   .label("Audio")
   .slider("pitch", "Voice Pitch", 0.5, 2.0, state.audioPitch, (v) => { state.audioPitch = v; applyState(); })
   .checkbox("mute", "Mute Audio", state.muted, (v) => { state.muted = v; applyState(); })
   .separator()
   .label("Quick Actions")
   .button("heal", "Full Heal", () => {
       const max = game.player.getMaxLife();
       if (!max || max < 0 || !game.player.setLife(max)) return;
       ui.toast("Restored full hearts", { color: "#5ce67a" });
   })
   .sameLine()
   .button("rupees", "500 Rupees", () => {
       if (!game.player.setRupees(500)) return;
       ui.toast("Set Rupees to 500", { color: "#66d9ff" });
   })
   .button("noon", "Noon (12:00)", () => { game.world.setTime(12, 0); })
   .sameLine()
   .button("night", "Midnight (00:00)", () => { game.world.setTime(0, 0); })
   .button("age", "Toggle Child / Adult", () => {
       const next = game.player.getAge() === "child" ? "adult" : "child";
       game.player.setAge(next);
   })
   .button("reset", "Reset All Modifiers", () => {
       state.speedScale = 1.0;
       state.fovScale = 1.0;
       state.damageMult = 1.0;
       state.audioPitch = 1.0;
       state.invincible = false;
       state.flight = false;
       state.muted = false;
       applyState();
       ui.toast("Reset sandbox modifiers");
   });
