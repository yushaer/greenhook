# In-Game Console, Named Pipe CLI & Debugging

## In-Game Overlay Controls (`hooks_vk.cpp`)

- **`F9`** — Opens/closes the **Longshot JS Console & Script Manager** window (unlocked mouse pointer automatically via `WM_APP + 13` when `Console` or any `ui.window(...)` is open).
  - **Console / REPL tab**: Type any ES2023 JS expression or statement, press **Enter** (or click **Run**), navigate command history with **Up/Down** arrows, press **Esc** once to clear input or twice to close, and click **Clear Logs** to flush the ring buffer. Color-coded output differentiates logs, `=>` return values, warnings, and errors.
  - **Scripts tab**: Shows each discovered script's ID, name, version, enable/disable checkbox (saved to `<scriptsDir>\scripts.json`), and load status/error message.
- **`F8`** — Cleanly resets and hot-reloads all scripts, clearing active `ui.draw.*` primitives, `ui.window(...)` panels, timers, event listeners, `native.hook` detours, input/camera overrides, and RomFS overrides.

---

## Named Pipe JSON RPC (`\\.\pipe\longshot-js`)

`asi/js_pipe.c` runs an overlapped Windows named pipe server restricted to the current user's SID DACL (`ConvertStringSecurityDescriptorToSecurityDescriptorA`).

Every request and response is a single newline-delimited JSON object (`\n`).

### Supported Commands

| Command | Request Example | Notes |
| :--- | :--- | :--- |
| `status` | `{"id":1,"cmd":"status"}` | Returns `frame`, `ready`, `width`, `height`, `asi: true`. |
| `js_list` | `{"id":2,"cmd":"js_list"}` | Lists `scriptsDir` and all discovered scripts (`id`, `name`, `version`, `enabled`, `loaded`, `error`). |
| `js_eval` | `{"id":3,"cmd":"js_eval","code":"game.player.getLife()","filename":"<repl>"}` | Executes JS inside the runtime mutex + watchdog; returns `{ ok, text, value }` or `{ ok: false, error, stack }`. |
| `js_reload` | `{"id":4,"cmd":"js_reload"}` | Executes `hos_js_reload()` and returns updated script list. |
| `js_log` | `{"id":5,"cmd":"js_log","since":0}` | Returns new log ring entries with `seq > since`. |

---

## Node.js CLI (`scripts_sdk/longshot-js-cli.mjs`)

Requires Node.js (zero npm dependencies). Default pipe is `longshot-js` (override with `--pipe <name>`):

```bash
# Check runtime connection & display resolution
node scripts_sdk/longshot-js-cli.mjs status

# Evaluate live expressions or upload a local test file
node scripts_sdk/longshot-js-cli.mjs eval "game.player.setRupees(500)"
node scripts_sdk/longshot-js-cli.mjs file path/to/snippet.js

# Inspect loaded scripts & console output
node scripts_sdk/longshot-js-cli.mjs list
node scripts_sdk/longshot-js-cli.mjs log --since 0

# Hot-reload or open an interactive terminal REPL (.reload, .list, .exit)
node scripts_sdk/longshot-js-cli.mjs reload
node scripts_sdk/longshot-js-cli.mjs repl
```

---

## Booting Straight Into a Save (No Manual Input)

Stock Longshot `v3.0.0` can load a save at startup by itself. Nothing needs patching. Set these in `<GameDir>\longshot_options.ini`:

```ini
skip_title_screen=1   ; load the most recent save automatically
skip_opening=1        ; skip the intro
```

At boot the game scans `<GameDir>\user\save\<account>\slot_*` and picks the slot whose `progress.sav` has the newest modification time. It then presses A for you, starting about 20 s in. The log (`<GameDir>\logs\longshot_log.txt`) confirms each step:

```
skip title: ...\slot_05\progress.sav is the most recent save: pressing A from 20 s
skip title: loaded, the player is running
```

When there are no saves, it logs `skip title: no saved game yet: the title screen stays`. When nothing has loaded after about 120 s, it logs `skip title: gave up after ... s`.

**Booting a specific slot (e.g. a debug save):** the game picks the newest file, so before launching, set that slot's timestamps to now:

```powershell
$slot = Get-Item "<GameDir>\user\save\*\slot_03"
Get-ChildItem $slot -Filter *.sav | ForEach-Object { $_.LastWriteTime = Get-Date }
```

Saving in another slot makes that slot the newest one. Bump your slot again before the next launch.

Related stock `longshot.exe` symbols, which are not hooked yet: `hos_oot_autoload_input`, `hos_oot_autoload_reset`, `hos_title_input`.

---

## Troubleshooting

- **Plugin did not attach**: Verify `<GameDir>\bin\winmm.dll` and `<GameDir>\bin\plugins\longshot_js.asi` are both present and `LONGSHOT_JS_DISABLE` is not set to `1`. Check game logs for `[hos MainThread] longshot-js: ASI runtime active on stock v3.0.0 (bridge=complete, hooks=29)` and `ImGui & F9 console attached to stock longshot_vk.dll (73/73 symbols)`.
- **Infinite loop / script freeze**: Tight loops are interrupted automatically after ~1000 ms (`HOS_JS_TIMEOUT_MS`) with `InternalError: interrupted`. Check the `Scripts` tab or `list` CLI output for the error message.
- **`native.call()` throws "requires a live guest thread context"**: `native.call()` can only run inside guest CPU callbacks (`game.onPlayerUpdate(...)` or `game.defer(...)`), not directly inside top-level script load or `js_eval`.
- **`fs.*` path rejection**: Paths must be relative inside `scripts_data\` (`HOS_JS_DATA_DIR`); drive letters, leading slashes, and `..` segments are rejected.
