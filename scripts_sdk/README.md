# Greenhook mod SDK

## Type hints in Visual Studio Code

1. Install the **Developer** ZIP. In VS Code, choose **File → Open Folder** and open the game's `scripts` directory. It should contain `jsconfig.json`, `greenhook.d.ts`, and `greenhook-host.d.ts`. VS Code's built-in JavaScript and TypeScript language service provides completion, hover documentation and parameter hints.
2. Open an existing mod or create `MyMod/main.js`. Type `import { game } from "greenhook";`. On the next line type `game.player.` and press **Ctrl+Space**. Hover over `getProgress` to see its return type. Inside `game.onActorUpdate(actor => { ... })`, completion on `actor.` shows its position, instance ID and vtable.
3. For a separate mod repository, copy those same three files from the SDK to the project root and open **that** folder. Deploy the JavaScript to Longshot. TypeScript projects can include the declaration files in their own tsconfig and emit ES modules targeting ES2020 or newer.
4. If the folder already has a jsconfig or tsconfig, the installer preserves it. Include the mod files and both declaration files under that project's `include` or `files`. The shipped config uses `lib: ["ES2020"]` and `types: []` to model the embedded runtime. Use a dedicated mod project when the surrounding application also depends on Node or browser globals.
5. If completion is missing, open the mod and run **JavaScript: Go to Project Configuration** in the Command Palette to check which config owns the file. After moving declarations, run **TypeScript: Restart TS Server**. The shipped config uses `checkJs` to report mistakes in the Problems panel and `noEmit` to keep your files intact.

No npm install or bundler is required for a plain JavaScript mod. [VS Code JavaScript project configuration](https://code.visualstudio.com/docs/languages/jsconfig)

## First mod

Save the following as `scripts/MyMod/main.js`, start Longshot and press F8. When Link changes maps, a toast shows the new area's display name. Press F9 to inspect load errors.

```js
import { game, ui } from "greenhook";

game.onMapChange((map, previous) => {
    if (map !== previous) ui.toast(game.world.getAreaName(map) ?? map);
});
```

## Gameplay and resource APIs

Import `game`, `ui`, `events`, `fs`, `native`, `memory` or `Greenhook` from `greenhook`. The existing examples cover HUD telemetry, a persistent control panel, gameplay events and native hooks. Copy an example to the top level of the game's scripts folder to activate it.

Import `openTable`, `findRecord`, `trackActors`, `readResource`, `openPack`, `openByml` and `editResource` from `greenhook/data` for game databases, map placements and in-memory resource edits. The installer places this shared library under `scripts/lib`.

Use `listPlacementFiles`, `addActorPlacement` and `removeActorPlacement` to edit actors for subsequent scene loads. Use `callActorMethod` with a verified virtual slot to dispatch a method on a current actor observation. All four are exported by `greenhook/data`.

The installed `API_REFERENCE.md` describes callback timing, method signatures, return values, gameplay units, unavailable data, register arguments, and structured resource editing. The Developer ZIP also includes `docs/CLI_AND_DEBUGGING.md` and the Node.js CLI under `scripts_sdk`.

For native discovery, start with `openAiDefinitions()` for graph node names, `native.getAiClass(name)` for the associated virtual methods, `bindAiMethod` for a known scalar class method, and `openNativeModule("sdk")` for named exports. The API reference gives working lookup examples and explains how to interpret missing methods.
