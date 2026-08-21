$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Get-Content (Join-Path $root 'DirectX11\AssetHashCapture.cpp') -Raw

$refreshBegin = $source.IndexOf('void RefreshAssetHashCaptureSources()')
$refreshEnd = $source.IndexOf(
    'void ObserveAssetHashForAuthoring(',
    $refreshBegin
)
if ($refreshBegin -lt 0 -or $refreshEnd -le $refreshBegin) {
    throw 'Asset Hash source refresh implementation was not found.'
}
$refresh = $source.Substring($refreshBegin, $refreshEnd - $refreshBegin)
if ($refresh -notmatch
        'if\s*\(capture_mode == CaptureMode::Off\)\s*ResetCaptureSessionLocked\(\);') {
    throw 'Workspace reset must occur only during an off-mode config refresh.'
}
if (-not $refresh.Contains('!target_source_file.empty()')) {
    throw 'An active F10 or F7 mode change must rewrite the locked workspace.'
}

$toggleBegin = $source.IndexOf('void ToggleAssetHashCapture(')
$toggleEnd = $refreshBegin
if ($toggleBegin -lt 0 -or $toggleEnd -le $toggleBegin) {
    throw 'Asset Hash toggle implementations were not found.'
}
$toggles = $source.Substring($toggleBegin, $toggleEnd - $toggleBegin)
if ($toggles.Contains('ResetCaptureSessionLocked();')) {
    throw 'F7 mode toggles must preserve the current workspace until off-mode F10.'
}

$writerBegin = $source.IndexOf('void WriteSnapshot(')
$writerEnd = $source.IndexOf('DWORD WINAPI CaptureWriterThread(', $writerBegin)
if ($writerBegin -lt 0 -or $writerEnd -le $writerBegin) {
    throw 'Asset Hash writer implementation was not found.'
}
$writer = $source.Substring($writerBegin, $writerEnd - $writerBegin)
foreach ($contract in @(
        'if (target_source.empty())',
        'if (_wcsicmp(source_path.c_str(), target_source.c_str()))'
    )) {
    if (-not $writer.Contains($contract)) {
        throw "Missing workspace-scoped writer contract: $contract"
    }
}
if ($writer.IndexOf('if (target_source.empty())') -gt
        $writer.IndexOf('TransformAssetHashIniDocumentToPaths(')) {
    throw 'The writer must reject an unlocked workspace before transforming textures.'
}
if ($writer.IndexOf(
        'if (_wcsicmp(source_path.c_str(), target_source.c_str()))') -gt
        $writer.IndexOf('TransformAssetHashIniDocumentToPaths(')) {
    throw 'The writer must reject non-workspace INIs before transforming textures.'
}

$diagnosticsBegin = $source.IndexOf('std::string AssetHashCaptureDiagnosticsJson()')
$diagnostics = $source.Substring($diagnosticsBegin)
foreach ($contract in @('\"workspace\":%llu', '\"workspace_locked\":%s')) {
    if (-not $diagnostics.Contains($contract)) {
        throw "Missing workspace diagnostic contract: $contract"
    }
}

Write-Host 'asset_hash_workspace_lifecycle: PASS'
