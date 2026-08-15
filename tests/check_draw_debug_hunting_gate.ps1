$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$hunting = Get-Content (Join-Path $root 'DirectX11\Hunting.cpp') -Raw
$stream = Get-Content (Join-Path $root 'DirectX11\DrawDebugStream.cpp') -Raw

$requiredHuntingContracts = @(
    'if (!draw_debug_enabled || G->hunting != HUNTING_MODE_ENABLED)',
    'G->hunting != HUNTING_MODE_ENABLED)',
    'SetDrawDebugControlAllowed(draw_debug_enabled && hunting_enabled);'
)
foreach ($contract in $requiredHuntingContracts) {
    if (-not $hunting.Contains($contract)) {
        throw "Missing Draw Debug hunting gate: $contract"
    }
}

if (([regex]::Matches($stream, 'ERROR hunting mode required')).Count -ne 3) {
    throw 'START, ARM, and SNAPSHOT must all reject control outside hunting mode.'
}
if (-not $stream.Contains('\"control_allowed\":%s')) {
    throw 'Draw Debug STATUS must expose the hunting gate state.'
}

Write-Host 'draw_debug_hunting_gate: PASS'
