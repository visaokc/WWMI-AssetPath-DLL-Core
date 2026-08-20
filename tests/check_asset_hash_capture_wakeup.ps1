$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Get-Content (Join-Path $root 'DirectX11\AssetHashCapture.cpp') -Raw
$begin = $source.IndexOf('bool AssetHashCaptureNeedsVbObservation(')
$end = $source.IndexOf(
    'bool AssetHashCaptureNeedsCurrentModelVertexCount(',
    $begin
)
if ($begin -lt 0 -or $end -le $begin) {
    throw 'Current-model target selection implementation was not found.'
}
$body = $source.Substring($begin, $end - $begin)
$contracts = @(
    'capture_dirty = true;',
    'signal_writer = true;',
    'if (signal_writer)',
    'SignalWriter();'
)
foreach ($contract in $contracts) {
    if (-not $body.Contains($contract)) {
        throw "Missing target-selection writer wakeup contract: $contract"
    }
}
if ($body.IndexOf('ReleaseSRWLockExclusive(&capture_lock);') -gt
        $body.LastIndexOf('SignalWriter();')) {
    throw 'The writer must be signaled after releasing the capture lock.'
}

Write-Host 'asset_hash_capture_wakeup: PASS'
