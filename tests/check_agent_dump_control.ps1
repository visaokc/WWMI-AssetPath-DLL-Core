$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$hunting = Get-Content (Join-Path $root 'DirectX11\Hunting.cpp') -Raw
$stream = Get-Content (Join-Path $root 'DirectX11\DrawDebugStream.cpp') -Raw
$client = Get-Content (Join-Path $root 'tools\wwmi_draw_debug_client.py') -Raw

$streamContracts = @(
    'else if (!_strnicmp(command, "DUMP ", 5))',
    'ConsumeAgentDumpRequest',
    'DrawDebugStreamIsLearnedShader',
    'SetAgentDumpResult',
    '\"completed_dumps\":%llu',
    '\"agent_hunting_required\":false'
)
foreach ($contract in $streamContracts) {
    if (-not $stream.Contains($contract)) {
        throw "Missing agent dump pipe contract: $contract"
    }
}

$huntingContracts = @(
    'HandleAgentDumpRequest',
    'DUMP SHADER [stage] hash [asm|bin|both]',
    'DUMP SHADERS TARGET [asm|bin|both]',
    'parse_enum_option_string<wchar_t *, FrameAnalysisOptions>',
    'BinaryToAsmText(',
    'AgentDumps',
    'StartHeavyDrawDebug(device, true, parsed);',
    'SetDrawDebugControlAllowed(true);',
    'ConfigureDrawDebugStream(true,'
)
foreach ($contract in $huntingContracts) {
    if (-not $hunting.Contains($contract)) {
        throw "Missing render-thread agent dump contract: $contract"
    }
}

if ($hunting.Contains('if (G->hunting != HUNTING_MODE_ENABLED)' + [Environment]::NewLine +
        "`t`tHandleAgentDumpRequest")) {
    throw 'Agent dump handling must not be nested under the human Hunting gate.'
}

$clientContracts = @(
    'dump-frame',
    'dump-shader',
    'dump-target-shaders',
    'dump-raw',
    'completed_dumps'
)
foreach ($contract in $clientContracts) {
    if (-not $client.Contains($contract)) {
        throw "Missing agent dump client contract: $contract"
    }
}

Write-Host 'agent_dump_control: PASS'
