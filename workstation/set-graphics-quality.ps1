param([ValidateSet('Ultra4K', 'Extreme8K')][string]$Preset = 'Ultra4K')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $root 'dist/flagship'
$active = Get-Process -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and $_.Path.StartsWith($runtime + '\', [StringComparison]::OrdinalIgnoreCase)
}
if ($active) { throw 'Close the game before changing its graphics preset.' }
$name = if ($Preset -eq 'Extreme8K') { 'extreme8k' } else { 'ultra4k' }
$resolution = if ($Preset -eq 'Extreme8K') { '7680x4320' } else { '3840x2160' }
$config = Join-Path $PSScriptRoot 'run-user/config.ini'
$text = Get-Content -LiteralPath $config -Raw
if ($text -notmatch '(?m)^resolution=') { throw 'Missing resolution entry in config.ini' }
$text = $text -replace '(?m)^resolution=[^\r\n]*', "resolution=$resolution"
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "config/graphics-$name.ini") -Destination (Join-Path $PSScriptRoot 'run-user/Config/GFX.ini') -Force
Set-Content -LiteralPath $config -Value $text -NoNewline
Write-Output "$Preset enabled for the next launch. Model detail and distance use bigwilly.conf."
