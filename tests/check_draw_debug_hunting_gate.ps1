$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$hunting = Get-Content (Join-Path $root 'DirectX11\Hunting.cpp') -Raw
$stream = Get-Content (Join-Path $root 'DirectX11\DrawDebugStream.cpp') -Raw

$requiredHuntingContracts = @(
    'if (!agent_control && G->hunting != HUNTING_MODE_ENABLED)',
    '(!agent_control && G->hunting != HUNTING_MODE_ENABLED)',
    'AnalyseFrameInternal(device, false);',
    'StartLightDrawDebug(false);',
    'StartHeavyDrawDebug(device, false);'
)
foreach ($contract in $requiredHuntingContracts) {
    if (-not $hunting.Contains($contract)) {
        throw "Missing Draw Debug hunting gate: $contract"
    }
}

if ($stream.Contains('ERROR hunting mode required')) {
    throw 'Agent START, ARM, and SNAPSHOT must bypass the human Hunting gate.'
}
$requiredAgentContracts = @(
    'SetDrawDebugControlAllowed(draw_debug_enabled);',
    'StartLightDrawDebug(true);',
    'StartHeavyDrawDebug(device, true);',
    '\"agent_hunting_required\":false',
    '\"control_allowed\":%s'
)
foreach ($contract in $requiredAgentContracts) {
    if (-not ($hunting.Contains($contract) -or $stream.Contains($contract))) {
        throw "Missing Hunting-independent agent control contract: $contract"
    }
}

$configureBody = [regex]::Match(
    $stream,
    '(?s)void ConfigureDrawDebugStream\([^)]*\)\s*\{(.*?)\n\}'
)
if (-not $configureBody.Success) {
    throw 'ConfigureDrawDebugStream implementation was not found.'
}
if ($configureBody.Groups[1].Value.Contains('EnsureInitialized()') -or
    $configureBody.Groups[1].Value.Contains('CreateThread(')) {
    throw 'Parsing [DrawDebug] must not create background threads.'
}

$requiredLifecycleContracts = @(
    'writer_stop_event',
    'pipe_stop_event',
    'StartDrawDebugControlServer',
    'StopDrawDebugControlServer',
    'FILE_FLAG_OVERLAPPED',
    'CancelIoEx(',
    'WaitForSingleObject(writer_thread',
    'CloseHandle(output_file)'
)
foreach ($contract in $requiredLifecycleContracts) {
    if (-not $stream.Contains($contract)) {
        throw "Missing Draw Debug lifecycle contract: $contract"
    }
}

Write-Host 'draw_debug_hunting_gate: PASS'
