$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = Join-Path $root 'tests\build'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found: $vswhere"
}

$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools were not found.'
}

$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$executable = Join-Path $build 'asset_hash_ini_document_tests.exe'

New-Item -ItemType Directory -Force -Path $build | Out-Null
try {
    $command = @(
        'call', ('"{0}"' -f $vcvars), '>nul', '&&',
        'cd', '/d', ('"{0}"' -f $root), '&&',
        'cl.exe', '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
        '/Fotests\build\',
        '/Fetests\build\asset_hash_ini_document_tests.exe',
        'tests\asset_hash_ini_document_tests.cpp',
        'DirectX11\AssetHashIniDocument.cpp'
    ) -join ' '
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    & $executable
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Remove-Item -LiteralPath $build -Recurse -Force -ErrorAction SilentlyContinue
}
