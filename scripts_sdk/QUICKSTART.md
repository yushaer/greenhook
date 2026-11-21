# Greenhook for Longshot 3.0.0 on Windows x64

Extract the entire ZIP into a folder. Close Longshot. Open PowerShell in the extracted folder and run the following command, replacing the path with the folder containing your Longshot installation's `longshot_version.txt`.

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts_sdk\install-to-v3.ps1 -GameDir 'C:\Games\Longshot\v3.0.0'
```

Place a trusted mod's `.js` file in the game's `scripts` folder. Folder mods go under `scripts\ModName\main.js`. Start Longshot and press F9 to check the Scripts tab. Press F8 after editing a script. Restart the game after upgrading Greenhook itself. Mods with native or memory access have control of the running game. Keep a backup of saves you use for testing.

The developer download includes the same runtime plus working examples, editor declarations, a CLI and the full API reference. Copy an example from `scripts\examples` into `scripts` to enable it. Start with the HUD example, then open `scripts\API_REFERENCE.md`. New mods can import `game`, `ui`, `native` and other core namespaces from `"greenhook"`.

To uninstall, close Longshot and run the uninstall script with the same `-GameDir`. It keeps your scripts and mod data.
For VS Code type hints, install the Developer ZIP and open the installed `scripts` folder in VS Code. Type `game.player.` after importing `game` and press Ctrl+Space. See the installed `scripts/README.md` for separate mod repositories and existing editor configurations.
