# Compile and run Greenhook unit tests (tests/test_js_engine.c).
param(
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

$WorkDir = "$Repo\work"
$ObjDir = "$WorkDir\test_obj"
New-Item -ItemType Directory -Force -Path $WorkDir, $ObjDir | Out-Null

$CCommon = @(
    "-O2", "-std=c11", "-D_GNU_SOURCE",
    "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX"
)
$QjsFlags = $CCommon + @("-DCONFIG_VERSION=`"quickjs-ng`"", "-I$Repo\third_party\quickjs")
$QjsObjs = @()
foreach ($rel in (Get-Content "$Repo\third_party\quickjs\sources.txt")) {
    $rel = $rel.Trim()
    if (-not $rel) { continue }
    $src = "$Repo\third_party\quickjs\$rel"
    $obj = "$ObjDir\qjs_" + ($rel -replace "[\\/]", "_").Replace(".c", ".o")
    if (-not (Test-Path $obj) -or (Get-Item $src).LastWriteTimeUtc -gt (Get-Item $obj).LastWriteTimeUtc) {
        & $CC @QjsFlags -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { throw "Failed compiling $rel" }
    }
    $QjsObjs += $obj
}

$TestFlags = $CCommon + @(
    "-Wall", "-Wextra",
    "-DCONFIG_VERSION=`"quickjs-ng`"",
    "-I$Repo\host",
    "-I$Repo\third_party\quickjs",
    "-I$Repo\third_party\a64-kit\rt",
    "-I$Repo\asi\compat_gen"
)
$ExeOut = "$WorkDir\test_js_engine.exe"
& $CC @TestFlags @QjsObjs `
    "$Repo\host\js_engine.c" `
    "$Repo\host\js_memory.c" `
    "$Repo\host\js_oot.c" `
    "$Repo\host\js_io.c" `
    "$Repo\host\js_ui.c" `
    "$Repo\tests\test_js_engine.c" `
    -o $ExeOut -luser32 -lkernel32 -lm
if ($LASTEXITCODE -ne 0) { throw "Failed compiling tests/test_js_engine.c" }

& $ExeOut
exit $LASTEXITCODE
