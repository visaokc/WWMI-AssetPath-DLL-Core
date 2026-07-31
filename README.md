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

## Stable Asset Path build

The second commit adds the currently validated WWMI Asset Path implementation:

- always-on Asset Path identity capture
- streaming texture hash collection
- F7 backup-copy INI migration mode
- Shift+F7 aggressive in-place INI migration mode
- legacy hash-to-Asset-Path migration
- bounded observation history and ambiguity-safe hash emission

Validated stable `d3d11.dll` SHA256:

```text
ED8E4AF3FEB95914E6F90DD80B49BEA3166660670A6149620F180536E6C9E0B7
```

## Download

Download the ready-to-use package from
[Releases](https://github.com/visaokc/WWMI-AssetPath-DLL-Core/releases/latest).
The ZIP contains `d3d11.dll` and `d3dx.ini`.

## Installation

1. Close the game and XXMI Launcher.
2. Open the WWMI root directory: the directory that already contains
   `d3d11.dll` and `d3dx.ini`.
3. Back up the existing `d3d11.dll` and `d3dx.ini`.
4. Extract both files from the release ZIP into the WWMI root directory and
   replace the existing files.
5. Start the game through XXMI Launcher.

## Runtime usage

| Key | Mode | Behavior |
| --- | --- | --- |
| `F7` | Backup | Starts backup mode from OFF. Generated INIs are written as `mod.ini.hashcache`; the loaded `mod.ini` is not modified. Pressing `F7` while either mode is active turns capture OFF. |
| `Shift+F7` | Aggressive | Starts aggressive mode from OFF or switches directly from Backup. The loaded `mod.ini` is updated in place through an atomic replacement. Pressing `Shift+F7` again turns aggressive mode OFF. |
| `F10` | Reload | Reloads mod configuration after an INI has been updated or replaced. |

The active mode is shown at the top centre of the game window:

```text
Asset Hash Capture: OFF
Asset Hash Capture: ON (BACKUP)
Asset Hash Capture: ON (AGGRESSIVE)
```

### Recommended workflow

#### Aggressive mode

1. Press `Shift+F7` before loading the target character or texture.
2. Load the character and visit the required scenes, distances, LOD states,
   graphics-detail settings, and appearance variants.
3. The DLL updates matching `mod.ini` files as new texture hashes are
   observed.
4. Press `F10` to reload the updated INIs.
5. If the current view still uses an old resource state, refresh the scene or
   reload the character after `F10`.
6. Press `Shift+F7` again, or press `F7`, to turn capture OFF.

#### Backup mode

1. Press `F7` before loading the target content.
2. Load all required texture states.
3. Find the generated `mod.ini.hashcache` beside the original `mod.ini`.
4. Review it, back up the original `mod.ini`, then replace `mod.ini` with the
   generated file. The `.hashcache` file itself is deliberately not loaded as
   a second mod INI.
5. Press `F10`, then refresh the scene if necessary.
6. Press `F7` to turn capture OFF.

### Capture notes

- Starting F7 or Shift+F7 before the target character is loaded is supported.
- Only texture states that the game actually loads can be observed. The DLL
  does not force the game to load missing LODs or streamed mips.
- Multiple hashes at the same resolution are retained because graphics-detail
  changes or different resource states can use different hashes.
- Existing `match_asset_path`, `match_asset_name`, generated comment markers,
  and previously generated hash regions are recognised on later runs.
- Legacy hash-only TextureOverrides can be migrated when the current hash is
  observed and resolves uniquely to one Asset Path.
- If one hash resolves to multiple Asset Paths, that ambiguous hash is omitted
  instead of risking an unrelated texture replacement.

## 中文使用说明

### 安装

1. 完全退出游戏和 XXMI Launcher。
2. 打开已有 `d3d11.dll`、`d3dx.ini` 的 WWMI 根目录。
3. 备份原来的 `d3d11.dll` 和 `d3dx.ini`。
4. 从 [Releases](https://github.com/visaokc/WWMI-AssetPath-DLL-Core/releases/latest)
   下载 ZIP，将其中两个文件解压到 WWMI 根目录并覆盖。
5. 通过 XXMI Launcher 启动游戏。

### 快捷键

- `F7`：开启安全备份模式，输出 `mod.ini.hashcache`，不直接修改原
  `mod.ini`；任意捕获模式开启时按 `F7` 都会关闭捕获。
- `Shift+F7`：开启激进模式，原子替换当前加载的 `mod.ini`；在备份模式
  下可直接切换为激进模式，再按一次 `Shift+F7` 可关闭。
- `F10`：重新加载修改或替换后的 INI。

### 推荐操作

激进模式最方便：在角色加载前按 `Shift+F7`，再加载角色并切换需要覆盖的
场景、距离、LOD、画面细节和外观状态。INI 更新时间变化后按 `F10`。如果
当前画面没有立刻恢复，再刷新场景或重新加载角色。完成后再次按
`Shift+F7` 或按 `F7` 关闭。

备份模式更安全：按 `F7` 后加载所需状态，找到生成的
`mod.ini.hashcache`，检查并备份原 `mod.ini`，再用生成文件替换原 INI，
最后按 `F10`。`.hashcache` 不会被当作第二份 mod INI 自动加载。

DLL 只能记录游戏实际加载过的贴图状态，不能强制游戏加载尚未出现的 LOD
或流式 Mip；因此需要主动展示对应角色、距离和画面细节。若同分辨率存在
多个有效 Hash，它们会全部保留。

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
