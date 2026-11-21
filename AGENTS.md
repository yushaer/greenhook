# Notes for Coding Agents

Start with [README.md](README.md) for the layout. [docs/](docs/) has the architecture, API, build and debugging guides.

## Build, test, deploy

```powershell
powershell -ExecutionPolicy Bypass -File asi\build_asi.ps1
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
node tests/test_game_data.mjs
powershell -ExecutionPolicy Bypass -File scripts_sdk\install-to-v3.ps1 -GameDir "<GameDir>"
```

## Testing in the running game

- **Skip the title screen.** Put `skip_title_screen=1` and `skip_opening=1` in `<GameDir>\longshot_options.ini`. The stock game then loads the most recently written save at boot, with no input. To pick a slot, set that slot's `*.sav` timestamps to now before launching. Full details: [docs/CLI_AND_DEBUGGING.md](docs/CLI_AND_DEBUGGING.md#booting-straight-into-a-save-no-manual-input).
- **Know when it has loaded.** Wait for `skip title: loaded, the player is running` in `<GameDir>\logs\longshot_log.txt`. After that, scripts that depend on the player are safe to use.
- **Drive the game.** Use the named-pipe CLI: `node scripts_sdk/longshot-js-cli.mjs eval|run|log|reload`. See [docs/CLI_AND_DEBUGGING.md](docs/CLI_AND_DEBUGGING.md).
- **Check that the plugin attached.** The log should contain `longshot-js: ASI runtime active on stock v3.0.0 (bridge=complete, hooks=29)`.
- **Do not damage saves.** The game writes to `<GameDir>\user\save\`. Make a backup before running tests that change progress.
