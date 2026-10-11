param(
    [string]$GameModule = 'build/game-module-clang-all/gRDHE78_recomp.dll',
    [string]$RuntimeBuild = 'build/runtime'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runtime = if ([IO.Path]::IsPathRooted($RuntimeBuild)) { $RuntimeBuild } else { Join-Path $PSScriptRoot $RuntimeBuild }
$package = Join-Path $root 'dist/flagship'
$launcher = Join-Path $PSScriptRoot 'build/flagship-launcher/BigWillyGuiLauncher.exe'
$modulePath = if ([IO.Path]::IsPathRooted($GameModule)) { $GameModule } else { Join-Path $PSScriptRoot $GameModule }
if (!(Test-Path -LiteralPath $launcher)) { throw 'Build workstation/build-launcher.cmd first.' }
if (!(Test-Path -LiteralPath (Join-Path $runtime 'dahbwu.exe'))) { throw "Build the runtime first: $runtime" }
if (!(Test-Path -LiteralPath $modulePath)) { throw 'Build workstation/build-hot-module.cmd --all first, or supply -GameModule for a deliberate fallback.' }
$active = Get-Process dahbwu -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $package 'dahbwu.exe') }
if ($active) { throw 'Close the flagship game before refreshing its package.' }
# A test runtime can also have the packaged module mapped. Check all existing
# binaries before copying any file, so that a test cannot leave a mixed package.
foreach ($binary in @((Join-Path $package 'dahbwu.exe'), (Join-Path $package 'gRDHE78_recomp.dll'), (Join-Path $root 'BigWillyUnleashed.exe'))) {
    if (Test-Path -LiteralPath $binary) {
        try {
            $probe = [IO.File]::Open($binary, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $probe.Dispose()
        } catch { throw "Close the game and test sessions before packaging: $binary is in use." }
    }
}
New-Item -ItemType Directory -Force $package | Out-Null
Copy-Item -LiteralPath (Join-Path $runtime 'dahbwu.exe') -Destination $package -Force
Copy-Item -LiteralPath $modulePath -Destination $package -Force
Copy-Item -LiteralPath (Join-Path $runtime 'Sys') -Destination $package -Recurse -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'config/KeyboardMouse.ini') -Destination (Join-Path $package 'WiimoteNew.ini') -Force
Copy-Item -LiteralPath $launcher -Destination (Join-Path $root 'BigWillyUnleashed.exe') -Force
Get-FileHash -LiteralPath (Join-Path $root 'BigWillyUnleashed.exe'),(Join-Path $package 'dahbwu.exe'),(Join-Path $package 'gRDHE78_recomp.dll') | Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'logs/flagship-hashes.json')
Write-Output (Join-Path $root 'BigWillyUnleashed.exe')
