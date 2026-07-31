$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found: $vswhere"
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1

if (-not $msbuild) {
    throw 'MSBuild.exe was not found.'
}

& $msbuild `
    (Join-Path $root 'DirectX11\DirectX11.vcxproj') `
    /m `
    /t:Rebuild `
    /p:Configuration=Release `
    /p:Platform=x64 `
    "/p:SolutionDir=$root\"

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$dll = Join-Path $root 'x64\Release\d3d11.dll'
Get-Item -LiteralPath $dll
Get-FileHash -Algorithm SHA256 -LiteralPath $dll
