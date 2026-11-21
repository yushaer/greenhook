# Build winmm.dll (ASI loader) and plugins/longshot_js.asi for untouched stock Longshot v3.0.0.
param(
    [string]$OutDir = "",
    [string]$ToolchainBin = ""
)
$ErrorActionPreference = "Stop"

$Repo = Split-Path -Parent $PSScriptRoot
$Tools = "$env:LOCALAPPDATA\LongshotLauncher\toolchains\v3.0.0\LongshotBuilder\tools"
if (-not $ToolchainBin) {
    $DefaultBin = "$Tools\llvm-mingw\bin"
    if (Test-Path "$DefaultBin\clang.exe") {
        $ToolchainBin = $DefaultBin
    } else {
        $ClangCmd = Get-Command clang.exe -ErrorAction SilentlyContinue
        if (-not $ClangCmd) { throw "Could not find clang.exe (install llvm-mingw or pass -ToolchainBin)" }
        $ToolchainBin = Split-Path -Parent $ClangCmd.Source
    }
}
$CC = Join-Path $ToolchainBin "clang.exe"
$CXX = Join-Path $ToolchainBin "clang++.exe"
$ImGuiInc = if (Test-Path "$Repo\third_party\imgui\imgui.h") { "$Repo\third_party\imgui" } else { "$Tools\third_party\imgui" }
$GenDir = "$Repo\asi\compat_gen"

if (-not $OutDir) { $OutDir = "$Repo\asi\dist" }
$ObjDir = "$Repo\asi\.obj"
New-Item -ItemType Directory -Force -Path $OutDir, "$OutDir\plugins", $ObjDir | Out-Null

Write-Host "[1/3] Generating winmm.dll export forwarding stubs..."
python "$Repo\asi\gen_winmm_proxy.py"
if ($LASTEXITCODE -ne 0) { throw "gen_winmm_proxy.py failed" }

Write-Host "[2/3] Building winmm.dll ASI loader..."
& $CC -O2 -Wall -Wextra -shared `
    "$Repo\asi\winmm_loader.c" `
    "$Repo\asi\winmm_stubs.S" `
    "$Repo\asi\winmm.def" `
    -o "$OutDir\winmm.dll" `
    -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "winmm.dll build failed" }

Write-Host "[3/3] Compiling longshot_js.asi..."
$CCommon = @(
    "-O2", "-std=c11", "-D_GNU_SOURCE",
    "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX"
)
$QjsFlags = $CCommon + @("-DCONFIG_VERSION=`"quickjs-ng`"", "-I$Repo\third_party\quickjs")
$HostFlags = $CCommon + @(
    "-DLONGSHOT_ASI",
    "-DCONFIG_VERSION=`"quickjs-ng`"",
    "-I$Repo\host",
    "-I$Repo\third_party\quickjs",
    "-I$Repo\third_party\a64-kit\rt",
    "-I$GenDir",
    "-I$Repo\third_party\minhook"
)
$CppFlags = @(
    "-O2", "-std=c++20", "-Wall", "-Wextra",
    "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
    "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
    "-DIMGUI_DISABLE_WIN32_FUNCTIONS",
    "-DIMGUI_DISABLE_DEMO_WINDOWS",
    "-DIMGUI_DISABLE_DEBUG_TOOLS",
    "-I$ImGuiInc",
    "-I$Repo\asi",
    "-I$Repo\third_party\a64-kit\rt",
    "-I$GenDir"
)

$Objs = @()

foreach ($rel in (Get-Content "$Repo\third_party\quickjs\sources.txt")) {
    $rel = $rel.Trim()
    if (-not $rel) { continue }
    $src = "$Repo\third_party\quickjs\$rel"
    $obj = "$ObjDir\qjs_" + ($rel -replace "[\\/]", "_").Replace(".c", ".o")
    if (-not (Test-Path $obj) -or (Get-Item $src).LastWriteTimeUtc -gt (Get-Item $obj).LastWriteTimeUtc) {
        & $CC @QjsFlags -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { throw "Failed compiling $rel" }
    }
    $Objs += $obj
}

foreach ($rel in @("buffer.c", "hook.c", "trampoline.c", "hde/hde64.c")) {
    $src = "$Repo\third_party\minhook\src\$rel"
    $obj = "$ObjDir\mh_" + ($rel -replace "[\\/]", "_").Replace(".c", ".o")
    if (-not (Test-Path $obj) -or (Get-Item $src).LastWriteTimeUtc -gt (Get-Item $obj).LastWriteTimeUtc) {
        & $CC -O2 -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { throw "Failed compiling minhook/$rel" }
    }
    $Objs += $obj
}

foreach ($name in @("js_engine.c", "js_memory.c", "js_oot.c", "js_io.c", "js_ui.c")) {
    $src = "$Repo\host\$name"
    $obj = "$ObjDir\host_" + $name.Replace(".c", ".o")
    & $CC @HostFlags -Wall -Wextra -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "Failed compiling host/$name" }
    $Objs += $obj
}

foreach ($name in @("coff_sym.c", "host_bridge.c", "hooks_exe.c", "js_pipe.c", "dllmain.c")) {
    $src = "$Repo\asi\$name"
    $obj = "$ObjDir\asi_" + $name.Replace(".c", ".o")
    & $CC @HostFlags -Wall -Wextra -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "Failed compiling asi/$name" }
    $Objs += $obj
}

$VkObj = "$ObjDir\asi_hooks_vk.o"
& $CXX @CppFlags -c "$Repo\asi\hooks_vk.cpp" -o $VkObj
if ($LASTEXITCODE -ne 0) { throw "Failed compiling asi/hooks_vk.cpp" }
$Objs += $VkObj

& $CXX -shared -static @Objs -o "$OutDir\plugins\longshot_js.asi" -luser32 -ladvapi32 -lkernel32 -lm
if ($LASTEXITCODE -ne 0) { throw "Linking longshot_js.asi failed" }

$WSize = (Get-Item "$OutDir\winmm.dll").Length
$ASize = (Get-Item "$OutDir\plugins\longshot_js.asi").Length
Write-Host "Built successfully:"
Write-Host ("  winmm.dll:       {0} ({1:N0} bytes)" -f "$OutDir\winmm.dll", $WSize)
Write-Host ("  longshot_js.asi: {0} ({1:N0} bytes)" -f "$OutDir\plugins\longshot_js.asi", $ASize)
