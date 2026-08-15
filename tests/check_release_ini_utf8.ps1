param(
    [string]$IniPath
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $IniPath) {
    $IniPath = Join-Path $repoRoot "Dependencies\d3dx.ini"
}
$bytes = [System.IO.File]::ReadAllBytes($IniPath)

if ($bytes.Length -ge 3 -and
    $bytes[0] -eq 0xEF -and
    $bytes[1] -eq 0xBB -and
    $bytes[2] -eq 0xBF) {
    throw "Release d3dx.ini must use UTF-8 without BOM"
}

$strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
$text = $strictUtf8.GetString($bytes)

function Assert-SingleActiveLine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Pattern,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $matches = [regex]::Matches(
        $text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline -bor
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    if ($matches.Count -ne 1) {
        throw "Release d3dx.ini must contain exactly one active $Description; found $($matches.Count)"
    }
}

Assert-SingleActiveLine '^\s*target\s*=\s*Client-Win64-Shipping\.exe\s*$' 'WWMI target'
Assert-SingleActiveLine '^\s*include\s*=\s*Core\\WWMI\\WuWa-Model-Importer\.ini\s*$' 'WWMI Core include'
Assert-SingleActiveLine '^\s*include_recursive\s*=\s*Mods\s*$' 'Mods include'
Assert-SingleActiveLine '^\s*toggle_asset_hash_capture\s*=\s*no_modifiers\s+VK_F7\s*$' 'F7 binding'
Assert-SingleActiveLine '^\s*toggle_aggressive_asset_hash_capture\s*=\s*shift\s+no_ctrl\s+no_alt\s+no_lwin\s+no_rwin\s+VK_F7\s*$' 'Shift+F7 binding'
Assert-SingleActiveLine '^\s*toggle_asset_hash_path_conversion\s*=\s*ctrl\s+no_shift\s+no_alt\s+no_lwin\s+no_rwin\s+VK_F7\s*$' 'Ctrl+F7 binding'
Assert-SingleActiveLine '^\s*toggle_asset_hash_clean_path_conversion\s*=\s*alt\s+no_shift\s+no_ctrl\s+no_lwin\s+no_rwin\s+VK_F7\s*$' 'Alt+F7 binding'

$drawDebugMatch = [regex]::Match(
    $text,
    '(?ms)^\s*\[DrawDebug\]\s*$.*?(?=^\s*\[|\z)'
)
if (-not $drawDebugMatch.Success) {
    throw 'Release d3dx.ini must contain [DrawDebug]'
}
if ($drawDebugMatch.Value -notmatch '(?mi)^\s*enabled\s*=\s*true\s*$') {
    throw 'Release d3dx.ini must enable the lazy Draw Debug lifecycle by default'
}

if ($text -match '(?mi)^\s*(?:target|include|include_recursive)\s*=\s*(?:Game\.exe|Core\\EFMI\\|Core\\ZZMI\\)') {
    throw 'Release d3dx.ini contains a non-WWMI loader target or Core include'
}

Write-Output "release_ini_contract: PASS"
