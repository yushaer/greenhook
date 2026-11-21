# Building, Testing & Deploying Greenhook

Greenhook is self-contained under `D:\greenhook`: all third-party libraries (**QuickJS-NG**, **MinHook**, **a64-kit** header, and **Dear ImGui `1.92.9b`** headers) plus minimal stock host/Vulkan ABI headers are vendored in-tree.

---

## Prerequisites

- **Compiler Toolchain (llvm-mingw)**:
  - Default search order in `asi/build_asi.ps1` and `tests/run_tests.ps1`:
    1. Explicit `-ToolchainBin <dir>` argument
    2. `%LOCALAPPDATA%\LongshotLauncher\toolchains\v3.0.0\LongshotBuilder\tools\llvm-mingw\bin`
    3. `clang.exe` / `clang++.exe` on `PATH`
- **Python 3**: Needed during builds by [`asi/gen_winmm_proxy.py`](../asi/gen_winmm_proxy.py) to parse `%SystemRoot%\System32\winmm.dll`'s export table.
- **Node.js** *(optional)*: Only needed to run the zero-dependency CLI tool [`scripts_sdk/longshot-js-cli.mjs`](../scripts_sdk/longshot-js-cli.mjs).

---

## Build Steps

From `D:\greenhook`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File asi\build_asi.ps1
```

Outputs placed into `D:\greenhook\asi\dist\`:
- `winmm.dll` — x86-64 proxy DLL + ASI loader
- `plugins\longshot_js.asi` — QuickJS-NG + MinHook + ImGui runtime plugin

### Verifying Zero External Runtime Dependencies
`asi/build_asi.ps1` links `longshot_js.asi` statically (`-static`) so it does not depend on `libc++.dll` or `libunwind.dll`:

```powershell
& "$env:LOCALAPPDATA\LongshotLauncher\toolchains\v3.0.0\LongshotBuilder\tools\llvm-mingw\bin\llvm-objdump.exe" -p asi\dist\plugins\longshot_js.asi | Select-String "DLL Name:"
```

Only `KERNEL32.dll`, `USER32.dll`, `ADVAPI32.dll`, and Windows UCRT `api-ms-win-crt-*` DLLs should appear.

---

## Running Unit Tests

Run the C unit test harness [`tests/test_js_engine.c`](../tests/test_js_engine.c) against QuickJS-NG + `host/js_*.c` + mock guest/host stubs:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

Expect `ALL UNIT TESTS PASSED` and exit code `0`. Covered scenarios:
- ES module `"longshot"` loading and exports
- `Promise`, `setTimeout`, `nextFrame()`, and `waitFrames()` scheduling
- Watchdog timeout interruption on tight loops (`while (true) {}`)
- MurmurHash3 x86-32 hashes (`Sequence_CurrentBanc` = `0x1d6189da`, `PlayerStatus.Life` = `0x31ab5580`)
- Bounds-checked `memory` R/W, `scan()`, `alloc()`, and unmapped address rejection
- Sandboxed `fs` JSON/text I/O and `../` traversal rejection
- `game.romfs` virtual file copy/exists and path redirect resolution
- ImGui snapshot & widget click/slider event round-trip (`hos_js_ui_snapshot_vk` / `hos_js_ui_event_vk`)
- Clean `hos_js_reload()` teardown & script re-initialization

---

## Installing & Uninstalling on Longshot `v3.0.0`

Deploy compiled artifacts, `longshot.d.ts`, `README.md`, and `examples\*.js`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts_sdk\install-to-v3.ps1 -GameDir "C:\Games\Longshot\v3.0.0"
```

Verify stock game binaries (`bin\longshot.exe`, `bin\longshot_vk.dll`) remain untouched, or remove Greenhook at any time with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts_sdk\uninstall-from-v3.ps1 -GameDir "C:\Games\Longshot\v3.0.0"
```

---

## Runtime Environment Variables

| Variable | Default | Purpose |
| :--- | :--- | :--- |
| `LONGSHOT_JS_DISABLE` | unset | Set to `1` to skip loading all `plugins\*.asi` in `winmm.dll`. |
| `HOS_JS_DIR` | `<GameDir>\scripts` | Override primary scripts folder path. |
| `HOS_JS_DATA_DIR` | `<GameDir>\scripts_data` | Override sandboxed `fs.*` storage root path. |
| `HOS_JS_CONTROL` | `longshot-js` | Named pipe name (`\\.\pipe\<name>`), or `0` to disable pipe RPC. |
| `HOS_JS_TIMEOUT_MS` | `1000` | Watchdog execution timeout in milliseconds per JS entry. |
