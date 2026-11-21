# Safely install Longshot JS Scripting Engine (winmm.dll + plugins/longshot_js.asi)
# onto an untouched stock Longshot v3.0.0 release directory.
param(
    [Parameter(Mandatory)][string]$GameDir,
    [string]$DistDir = ""
)
$ErrorActionPreference = "Stop"

$Repo = Split-Path -Parent $PSScriptRoot
if (-not $DistDir) { $DistDir = "$Repo\asi\dist" }

$WinmmSrc = "$DistDir\winmm.dll"
$AsiSrc   = "$DistDir\plugins\longshot_js.asi"
if (-not (Test-Path $WinmmSrc)) { throw "Missing $WinmmSrc (run asi/build_asi.ps1 first)" }
if (-not (Test-Path $AsiSrc))   { throw "Missing $AsiSrc (run asi/build_asi.ps1 first)" }

$BinDir = Join-Path $GameDir "bin"
if (-not (Test-Path (Join-Path $BinDir "longshot.exe"))) {
    throw "Target directory does not contain bin\longshot.exe: $GameDir"
}

$VersionFile = Join-Path $GameDir "longshot_version.txt"
if (!(Test-Path $VersionFile) -or !(Select-String -LiteralPath $VersionFile -Pattern '^version=v3\.0\.0$' -Quiet)) {
    throw "Greenhook 0.1 supports Longshot v3.0.0. Select that installation folder."
}
$PluginsDir = Join-Path $BinDir "plugins"
$ScriptsDir = Join-Path $GameDir "scripts"
$ExamplesDir = Join-Path $ScriptsDir "examples"
New-Item -ItemType Directory -Force -Path $PluginsDir, $ScriptsDir, $ExamplesDir | Out-Null

$Loader = Join-Path $BinDir "winmm.dll"
$Receipt = Join-Path $PluginsDir "greenhook-loader.sha256"
$Owned = (Test-Path $Loader) -and (Test-Path $Receipt) -and
    ((Get-FileHash $Loader).Hash -eq (Get-Content $Receipt -Raw).Trim())
if (!(Test-Path $Loader) -or $Owned -or (Get-FileHash $Loader).Hash -eq (Get-FileHash $WinmmSrc).Hash) {
    Copy-Item $WinmmSrc $Loader -Force
    (Get-FileHash $Loader).Hash | Set-Content -LiteralPath $Receipt
} else {
    Write-Host "Using your existing winmm.dll. It must support loading plugins/longshot_js.asi."
}
Copy-Item $AsiSrc   (Join-Path $PluginsDir "longshot_js.asi") -Force

$LibDir = Join-Path $ScriptsDir "lib"
New-Item -ItemType Directory -Force -Path $LibDir | Out-Null
Copy-Item "$Repo\scripts_sdk\lib\greenhook-data.js" $LibDir -Force
if (Test-Path "$Repo\scripts_sdk\greenhook.d.ts") {
    if (!(Test-Path "$ScriptsDir\jsconfig.json") -and !(Test-Path "$ScriptsDir\tsconfig.json")) {
        Copy-Item "$Repo\scripts_sdk\jsconfig.json" $ScriptsDir
    }
    Copy-Item "$Repo\scripts_sdk\greenhook-host.d.ts" $ScriptsDir -Force
    Copy-Item "$Repo\scripts_sdk\greenhook.d.ts" $ScriptsDir -Force
    Copy-Item "$Repo\docs\API_REFERENCE.md" $ScriptsDir -Force
    Copy-Item "$Repo\scripts_sdk\longshot.d.ts" (Join-Path $ScriptsDir "longshot.d.ts") -Force
    Copy-Item "$Repo\scripts_sdk\README.md"     (Join-Path $ScriptsDir "README.md") -Force
    Copy-Item "$Repo\scripts_sdk\examples\*.js" $ExamplesDir -Force
}

Write-Host "Installed Longshot JS Scripting API onto stock installation:"
Write-Host ("  ASI Loader: {0}" -f (Join-Path $BinDir "winmm.dll"))
Write-Host ("  JS Runtime: {0}" -f (Join-Path $PluginsDir "longshot_js.asi"))
Write-Host ("  Scripts SDK: {0}" -f $ScriptsDir)
Write-Host ""
Write-Host "Stock bin\longshot.exe and bin\longshot_vk.dll were NOT modified."
Write-Host "Drop .js scripts into $ScriptsDir (or copy from $ExamplesDir) and press F9 for console / F8 to reload."
