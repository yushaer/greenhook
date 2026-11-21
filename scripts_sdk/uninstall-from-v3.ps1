# Cleanly uninstall Longshot JS Scripting Engine from a stock Longshot v3.0.0 directory.
# Preserves user scripts in <GameDir>\scripts\.
param(
    [Parameter(Mandatory)][string]$GameDir
)
$ErrorActionPreference = "Stop"

$BinDir = Join-Path $GameDir "bin"
$WinmmDst = Join-Path $BinDir "winmm.dll"
$AsiDst   = Join-Path $BinDir "plugins\longshot_js.asi"

$Removed = $false
$Receipt = Join-Path $BinDir "plugins/greenhook-loader.sha256"
if ((Test-Path $WinmmDst) -and (Test-Path $Receipt) -and
    ((Get-FileHash $WinmmDst).Hash -eq (Get-Content $Receipt -Raw).Trim())) {
    Remove-Item $WinmmDst -Force
    Write-Host "Removed $WinmmDst"
    $Removed = $true
}
if (Test-Path $Receipt) { Remove-Item -LiteralPath $Receipt }
if (Test-Path $AsiDst) {
    Remove-Item $AsiDst -Force
    Write-Host "Removed $AsiDst"
    $Removed = $true
}

if (-not $Removed) {
    Write-Host "Nothing to remove in $BinDir."
} else {
    Write-Host "Uninstalled cleanly. User scripts in $(Join-Path $GameDir 'scripts') were preserved."
}
