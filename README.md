# Greenhook — JavaScript Scripting Engine for Longshot (`v3.0.0`)

**Greenhook** is a standalone ASI-based ES2023 JavaScript scripting runtime and mod SDK that attaches directly onto the **untouched stock release** of **Longshot `v3.0.0`** (*The Legend of Zelda: Ocarina of Time* Switch 2 recompilation). It lets mod authors write JavaScript mods controlling gameplay, Link's physics/health/inventory/age, flags & save state, world/time/teleports/events, camera & FOV, HID input, audio, Vulkan/graphics options & screenshots, virtual RomFS overrides, AArch64 guest memory & native function hooks, and live Dear ImGui HUD overlays & interactive windows.

Zero binary patching of `longshot.exe` or `longshot_vk.dll` is required: deployment drops a universal PE proxy `winmm.dll` ASI loader alongside `plugins/longshot_js.asi`.

Mods import from `greenhook`. Use `greenhook/data` for structured access to the installed game's RSDB tables, map placements, packs, and GameData definitions. Use `native.listFunctions()` for translated function addresses, `game.actors.getMethods()` for known virtual method slots, and `game.flags.list()` for live save flags. [API signatures and examples](docs/API_REFERENCE.md)

---

## Key Capabilities

- **Untouched Stock Injection**: `winmm.dll` proxies all 180 non-hooked `System32\winmm.dll` exports at exact original ordinals and hooks `timeBeginPeriod@137` outside the Windows loader lock to load `plugins\longshot_js.asi` (`LONGSHOT_JS_DISABLE=1` bypasses cleanly).
- **Modern ES2023 JavaScript Engine**: Powered by **QuickJS-NG** with ES module imports (`import { game, ui, memory, native, fs, events, Greenhook } from "greenhook"`), `Promise`, `async`/`await`, `BigInt`, `Uint8Array`, `TextEncoder`/`TextDecoder`, frame timers (`nextFrame()`, `waitFrames()`), and a watchdog interrupt guard against infinite loops.
- **Full Game & Recomp Hooking**:
  - Stock `longshot.exe` COFF symbols resolved at runtime (`asi/host_bridge.c`) with **29** MinHook detours (`asi/hooks_exe.c`).
  - **73** stock `longshot_vk.dll` COFF symbols resolved (`asi/hooks_vk.cpp`) with **6** Vulkan/ImGui detours + **2** `MkwVkHost` input detours, sharing Dear ImGui `1.92.9b` draw lists with exact Win64 ABI interop.
- **In-Game `F9` Console & `F8` Hot Reload**: Interactive Dear ImGui JS Console / REPL with command history, color-coded logs, live per-script toggles (`Scripts` tab), and instant `F8` hot-reload.
- **External Automation CLI & Named Pipe RPC**: User-only overlapped Windows named pipe server at `\\.\pipe\longshot-js` paired with a zero-dependency Node.js CLI (`scripts_sdk/longshot-js-cli.mjs`).

---

## Downloads

Choose **Player** to run mods or **Developer** for the same runtime plus declarations, the reference, examples and CLI. Both ZIPs include installation instructions. [GitHub Releases](https://github.com/yushaer/greenhook/releases)

## Quick Start

### 1. Deploy to Stock Longshot `v3.0.0`
Run from PowerShell (builds must exist under `asi\dist\`, or run `asi\build_asi.ps1` first):

```powershell
powershell -ExecutionPolicy Bypass -File scripts_sdk\install-to-v3.ps1 -GameDir "C:\Games\Longshot\v3.0.0"
```

Uninstall cleanly at any time (preserving user scripts in `<GameDir>\scripts\`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts_sdk\uninstall-from-v3.ps1 -GameDir "C:\Games\Longshot\v3.0.0"
```

### 2. Add or Run Scripts
- Drop `.js` files (or `<mod>\main.js` / `mod.json`) into `<GameDir>\scripts\`, or bundle scripts inside `<GameDir>\mods\<ModName>\scripts\`.
- Ready-to-use examples are copied into `<GameDir>\scripts\examples\` (`01-hud-telemetry.js` through `05-scene-editor.js`). `05-scene-editor.js` is an in-game scene editor: press `F7` for a free-fly camera, click objects to select them, and drag handles to move them. Edits are saved as virtual RomFS overrides.
- TypeScript autocomplete definitions live at `<GameDir>\scripts\greenhook.d.ts`.

### 3. Build & Run Unit Tests from Source
```powershell
powershell -ExecutionPolicy Bypass -File asi\build_asi.ps1
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

---

## Directory Layout

| Path | Description |
| :--- | :--- |
| `asi/` | ASI loader (`winmm_loader.c`, `gen_winmm_proxy.py`), PE COFF resolver (`coff_sym.*`), host bridge (`host_bridge.*`), `longshot.exe` MinHook detours (`hooks_exe.*`), `longshot_vk.dll` Dear ImGui overlay (`hooks_vk.*`), named-pipe RPC server (`js_pipe.*`), shared UI snapshot types (`js_ui_types.h`), build script (`build_asi.ps1`), and fallback headers (`compat_gen/`). |
| `host/` | QuickJS-NG engine core (`js_engine.c`, `js_memory.c`, `js_oot.c`, `js_io.c`, `js_ui.c`, `js_api.h`, `js_internal.h`) plus the three minimal stock host ABI headers (`hos.h`, `hos_paths.h`, `control.h`). |
| `gpu/` | Minimal stock Vulkan/ImGui/NVN ABI headers (`gpu/vk/` and `gpu/render/`) required for binary-compatible overlay compilation. |
| `third_party/` | Vendored **QuickJS-NG** (`quickjs/`), **MinHook** (`minhook/`), **a64-kit** (`a64-kit/rt/a64.h`), and **Dear ImGui `1.92.9b`** headers (`imgui/`). |
| `scripts_sdk/` | Mod author SDK: `longshot.d.ts`, `README.md`, 5 example scripts (`examples/`), Node.js CLI (`longshot-js-cli.mjs`), and deploy scripts (`install-to-v3.ps1`, `uninstall-from-v3.ps1`). |
| `tests/` | Standalone unit test harness (`test_js_engine.c` + `run_tests.ps1`) with mock guest memory & host stubs. |
| `docs/` | Technical architecture, API reference, build guide, and CLI/debugging guides. |

---

## Documentation

- **[Architecture & Hooking Internals](docs/ARCHITECTURE.md)** — How `winmm.dll`, PE COFF symbol resolution, MinHook detours, Dear ImGui overlay injection, and QuickJS-NG thread safety fit together.
- **[Greenhook API Reference](docs/API_REFERENCE.md)** — Complete module, hook, memory, UI, RomFS, filesystem, and gameplay scripting guide.
- **[Building & Testing Guide](docs/BUILDING.md)** — Toolchain setup, static linking requirements, unit testing, deployment, and environment switches.
- **[CLI, Pipe Protocol & Debugging](docs/CLI_AND_DEBUGGING.md)** — `F9` console usage, `\\.\pipe\longshot-js` JSON RPC schema, `longshot-js-cli.mjs`, and troubleshooting.
## First beta coverage

Greenhook targets Windows x64 and Longshot v3.0.0. The native function list gives guest addresses. Only a subset have documented signatures. Actor tracking observes the shared transform-update path. The engine does not yet provide authoritative spawn/despawn notifications, typed AI or animation controllers for every class, arbitrary quest progression commands, or a serializer for every game format. The SDK includes a BYML tree writer and map placement helpers. Mod authors can edit existing BYML fields, inspect RSDB records, watch live flags and call methods with verified signatures.

The first beta was exercised in Hyrule Field and Kokiri Forest with a disposable copy of a save. The checks covered clock structure returns, player statistics, reversible rupee edits, map travel and callbacks, live actor tracking, hot reload, and a virtual resource edit. The source tests cover the runtime and resource helpers. Run `tests/run_tests.ps1` and `node tests/test_game_data.mjs` (Node.js 22.15+ for the resource tests). Run `scripts_sdk/package-release.ps1` after a release build to produce both ZIPs.

Greenhook 0.1.0 Beta 3 adds named native exports, AI class vtable lookup, and typed AI method binding. On the supported stock build, 4,225 AI node names map to virtual methods, and 24,588 exported function entries resolve in the running game. The export count includes aliases and platform or media library functions. [Class and native discovery](docs/API_REFERENCE.md#discovering-game-classes-and-named-native-functions)
