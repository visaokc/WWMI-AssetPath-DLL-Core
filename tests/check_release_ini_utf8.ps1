$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$iniPath = Join-Path $repoRoot "Dependencies\d3dx.ini"
$bytes = [System.IO.File]::ReadAllBytes($iniPath)

if ($bytes.Length -ge 3 -and
    $bytes[0] -eq 0xEF -and
    $bytes[1] -eq 0xBB -and
    $bytes[2] -eq 0xBF) {
    throw "Release d3dx.ini must use UTF-8 without BOM"
}

$strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
[void]$strictUtf8.GetString($bytes)

Write-Output "release_ini_utf8: PASS"
