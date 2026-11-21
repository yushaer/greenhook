param([string]$Version = "0.1.0-beta.2")
$ErrorActionPreference = "Stop"
$Repo = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Repo "work/releases"
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Common = @("LICENSE", "NOTICE.md", "asi/dist/winmm.dll", "asi/dist/plugins/longshot_js.asi",
    "scripts_sdk/install-to-v3.ps1", "scripts_sdk/uninstall-from-v3.ps1", "scripts_sdk/lib/greenhook-data.js",
    "third_party/quickjs/LICENSE", "third_party/minhook/LICENSE.txt", "third_party/imgui/LICENSE.txt",
    "third_party/a64-kit/NOTICE.TOTKPort.md", "third_party/a64-kit/PROVENANCE.md")
foreach ($Edition in @("Player", "Developer")) {
    $Files = $Common
    if ($Edition -eq "Developer") {
        $Files += @("docs/API_REFERENCE.md", "docs/CLI_AND_DEBUGGING.md", "scripts_sdk/README.md",
            "scripts_sdk/jsconfig.json", "scripts_sdk/greenhook-host.d.ts", "scripts_sdk/greenhook.d.ts", "scripts_sdk/longshot.d.ts", "scripts_sdk/longshot-js-cli.mjs")
        $Files += Get-ChildItem "$PSScriptRoot/examples/*.js" | ForEach-Object { "scripts_sdk/examples/" + $_.Name }
    }
    $Dest = Join-Path $Out "Greenhook-$Version-$Edition-win64.zip"
    $Stream = [IO.File]::Create($Dest)
    $Zip = [IO.Compression.ZipArchive]::new($Stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($File in $Files) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Zip, (Join-Path $Repo $File), $File) | Out-Null
        }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Zip, "$PSScriptRoot/QUICKSTART.md", "START-HERE.md") | Out-Null
    } finally { $Zip.Dispose(); $Stream.Dispose() }
    Get-FileHash $Dest
}
