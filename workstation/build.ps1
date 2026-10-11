$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$mapping = @(& subst) | Where-Object { $_ -match '^R:' }
if ($mapping) {
    $target = ($mapping -split '=>', 2)[1].Trim().TrimEnd('\')
    if ($target -ne $root.TrimEnd('\')) { throw "R: already maps to $target; refusing to change it" }
} elseif (Test-Path R:\) {
    throw 'R: is already in use. A free build-drive alias is required.'
} else {
    & subst R: $root
    if ($LASTEXITCODE -ne 0) { throw 'Could not create workspace drive alias' }
}
Push-Location $root
try {
    & "$PSScriptRoot/build-runtime.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Runtime build failed' }
    python "$PSScriptRoot/prepare_symbols.py"
    if ($LASTEXITCODE -ne 0) { throw 'Symbol verification failed' }
    & "$PSScriptRoot/build-game.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Game module build failed' }
    & "$PSScriptRoot/build-hot-module.cmd" --all --jobs 8
    if ($LASTEXITCODE -ne 0) { throw 'Optimized game module build failed' }
    & "$PSScriptRoot/build-launcher.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Flagship launcher build failed' }
    & "$PSScriptRoot/package.ps1"
} finally { Pop-Location }
