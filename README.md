# WWMI Asset Path DLL Core

This repository is a minimized, buildable extraction of the source required
to produce the WWMI `d3d11.dll`.

It intentionally excludes unrelated projects from the full
`XXMI-Libs-Package` workspace, including injectors, legacy D3D compiler
projects, decompiler tools not referenced by `DirectX11`, tests, and release
automation.

## Upstream baseline

- Project: `SpectrumQT/XXMI-Libs-Package`
- Release: `v0.9.2`
- Source commit: `b94661f3abdbfc56aaf9e9f15c69b812573bd654`
- Official `d3d11.dll` SHA256:
  `CCBDD982B7CA4FD324198B68B83AEE23ED8B0CF0445255AA8309D021BA8AAD29`

The root commit contains only the upstream files needed by the `DirectX11`
project and its direct build dependencies.

## Build

Requirements:

- Visual Studio 2022 with Desktop development with C++
- Windows 10 or Windows 11 SDK

Run:

```powershell
.\build-d3d11.ps1
```

Output:

```text
x64\Release\d3d11.dll
```

MSVC and the linker do not produce byte-for-byte reproducible output for this
project. A rebuilt DLL can therefore have a different SHA256 despite identical
source.

