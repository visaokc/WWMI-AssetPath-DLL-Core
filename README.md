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
5. Open XXMI Launcher, go to `Settings` > `Advanced`, and enable
   `Unsafe Mode`. This is required for the Launcher to start the game with
   the custom replacement DLL.
6. Start the game through XXMI Launcher.

## Runtime usage

| Key | Mode | Behavior |
| --- | --- | --- |
| `F7` | Backup | Starts backup mode from OFF. Generated INIs are written as `mod.ini.hashcache`; the loaded `mod.ini` is not modified. Pressing `F7` while either mode is active turns capture OFF. |
| `Shift+F7` | Aggressive | Starts aggressive mode from OFF or switches directly from Backup. The loaded `mod.ini` is updated in place through an atomic replacement. Pressing `Shift+F7` again turns aggressive mode OFF. |
| `Ctrl+F7` | Path conversion | Converts currently validated Hash overrides to one active `match_asset_path` override in place. Unverified Hashes and their Mip metadata remain inside the same `asset-hash-stream` block for debugging and later automatic repair. |
| `Alt+F7` | Path cleanup | Uses the same runtime Path validation as Ctrl+F7, but removes every stored Hash section from a validated `asset-hash-stream` block and leaves only its active Path section. |
| `F10` | Reload | Reloads mod configuration after an INI has been updated or replaced. |

The active mode is shown at the top centre of the game window:

```text
Asset Hash Capture: OFF
Asset Hash Capture: ON (BACKUP)
Asset Hash Capture: ON (AGGRESSIVE)
Asset Hash Capture: ON (PATH CONVERSION)
Asset Hash Capture: ON (PATH CLEANUP)
```

### Short asset identity aliases

TextureOverride sections accept `path` and `name` as author-friendly aliases:

```ini
[TextureOverrideNum]
name = T_R2T1DaniyaMd10011Bangs02_D
handling = skip

[TextureOverridePath]
path = /Game/Aki/Character/Example/T_Example_D.T_Example_D
this = ResourceExample
```

They are active runtime match fields even if no F7 mode is ever used. `path`
is equivalent to `match_asset_path`; `name` is equivalent to
`match_asset_name`. The aliases apply only inside TextureOverride sections.
Do not combine a short alias with its own full spelling. Path, Name, and
Hash/fuzzy fallback identities may coexist in one section. Runtime matching
uses Path first, then Name, then Hash/fuzzy fallback, so a stale Hash does not
block a valid asset identity. Authors are responsible for making the fields
describe the same intended resource.

Starting any F7 mode detects short aliases and schedules a write even before a
matching texture is observed. The rewritten form always uses the full
`match_asset_path` or `match_asset_name` spelling; other section commands are
preserved.

## Draw Debug

Draw Debug is an opt-in, bounded FrameAnalysis profile for investigating all
draws involved in a modded character, including otherwise separate effect,
parallax, shadow, depth, and motion-vector passes.

Enable it in `d3dx.ini` before starting the game:

```ini
[DrawDebug]
enabled = true
toggle = no_modifiers VK_F11
long_press_ms = 1000
max_queue_records = 65536
options = dump_cb buf txt desc deferred_ctx_accurate share_dupes asset_path
```

Draw Debug now follows the same safety boundary as F8 Frame Analysis: first
enable Hunting mode with the configured `toggle_hunting` key (the supplied
configuration uses `Numpad0`). Outside active Hunting mode, F11 is ignored.
Press and release `F11` in less than one second to capture one complete heavy
FrameAnalysis frame. Hold `F11` for at least one second to start the lightweight
continuous draw stream; release it to stop. Heavy output is written to a
timestamped `FrameAnalysis-*` directory. Continuous output is written to
`DrawDebug-*\stream.jsonl` beside the loaded DLL.

The default profile records the full draw/state log, shader and resource
identity, resource descriptors, Asset Path identity, and deduplicated constant
buffer contents. It intentionally avoids texture, render-target, VB, and IB
payloads. Those can be enabled later through `options` without rebuilding the
DLL, using the standard FrameAnalysis flags such as `dump_tex`, `dump_rt`,
`dump_depth`, `dump_vb`, and `dump_ib`.

D3D11 has no native concept of "the character that owns this mod." The capture
therefore records the complete frame. Matched TextureOverride/command-list
entries identify known mod draws in the log, while shared shaders, buffers,
resources, passes, and transforms allow related effect and parallax draws to be
correlated offline without excluding them prematurely.

When `enabled = false`, no FrameAnalysisContext is requested by Draw Debug and
there is no Draw Debug runtime cost. With `enabled = true` but no capture active,
the context remains ready in soft-disabled hunting mode; this has a small fixed
wrapper cost, while the intentional GPU readback and disk-write cost is limited
to captured frames.

### Agent control and targeted capture

When Draw Debug is enabled, the local-only named pipe
`\\.\pipe\wwmi-draw-debug` accepts `PING`, `STATUS`, `START`, `STOP`,
`SNAPSHOT`, `MARK <label>`, `FILTER CLEAR`, `FILTER DRAW <count> <first>`, and
`ARM`. Pipe commands only queue state changes; D3D11 work remains on the render
thread. Continuous records use a bounded non-blocking queue and an asynchronous
JSONL writer. Queue contention or overflow increments `dropped` instead of
stalling the game. `START`, `ARM`, and `SNAPSHOT` are rejected unless Hunting
mode is currently enabled; `STATUS` reports `control_allowed`, while `STOP`
remains available so an existing stream can always be terminated.

Use the included zero-dependency client:

```powershell
python .\tools\wwmi_draw_debug_client.py status
python .\tools\wwmi_draw_debug_client.py start
python .\tools\wwmi_draw_debug_client.py mark problem_visible
python .\tools\wwmi_draw_debug_client.py snapshot
python .\tools\wwmi_draw_debug_client.py stop
python .\tools\wwmi_draw_debug_client.py tail --follow
```

Targeted mode derives component draw signatures from a mod INI, records only
matching draws and later draws sharing learned VS/PS identities, and queues one
automatic heavy snapshot when the first target appears:

```powershell
python .\tools\wwmi_draw_debug_client.py arm D:\WWMI\Mods\Daniya7\mod.ini
```

This is deliberately evidence-driven rather than a claim that D3D11 exposes a
native character owner. Independent effects with no shared draw signature or
shader remain discoverable through an unfiltered stream and subsequent heavy
snapshot.

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

#### Path conversion mode

1. Press `Ctrl+F7`, then load every relevant scene, distance, LOD, graphics
   setting, and appearance state.
2. A normal Hash-only override is converted only after its Hash resolves
   uniquely to a current runtime Asset Path.
3. A previously generated multi-Mip block does not copy its commented Path
   directly. The DLL confirms that an exact-Path resource is currently alive.
   No stored Hash needs to remain valid or match the live resource.
4. Hashes currently associated with the validated Path are absorbed into one
   active `match_asset_path` override. Unverified Hashes remain beside it inside
   the same `asset-hash-stream` block, including their Mip dimensions and
   multiplicity, making update candidates easy to inspect and repair later.
5. Press `F10` to load the Path form. After reviewing or removing residual
   Hashes, `F7` or `Shift+F7` can convert the marked Path block back to the
   streamed multi-Mip Hash form.

Path conversion writes the loaded INI in place through the same atomic file
replacement used by aggressive mode. The generated Path block uses the shared
`asset-hash-stream` comment markers and omits `match_priority = 0`. A residual
Hash means "not validated in this capture", not necessarily "definitely
obsolete"; an unloaded LOD or Mip can also leave a residual Hash. On a later
F7 or Shift+F7 pass, residuals participate in the same Mip group: incomplete
captures remain additive, while a complete new group replaces the old group.

Generated Path sections receive the Unreal object name as their section-name
suffix, for example `[TextureOverride_T_Example_D]`. The generic `_Texture`
part is removed. The name suffix is part
of the INI section identity, not a matcher. It prevents different object names
that originally shared a generic base such as `TextureOverride_Texture` from
collapsing into one duplicate section and remains stable across Path -> Hash ->
Path round trips. Authors should use full Path identities directly when their
mod contains same-named assets from different packages. An older generated
active-Path block without a name suffix is repaired the next time an F7 mode
writes the INI.

#### Path cleanup mode

`Alt+F7` follows the same exact-Path liveness boundary as Ctrl+F7. Once the
runtime observes a resource carrying the generated block's exact Path, the
complete stored Hash list in that block is discarded and one active Path
section is written. No old Hash needs to match; every stored Hash may already
be obsolete. A generated block whose Path has not been observed alive is left
unchanged. Ordinary Hash-only sections still require a unique current
Hash-to-Path resolution before conversion.

### Capture notes

- Starting F7, Shift+F7, Ctrl+F7, or Alt+F7 before the target character is loaded is
  supported.
- Each transition from OFF to any F7 mode starts a fresh capture session.
  Switching directly between writing modes keeps the same session.
- Only texture states that the game actually loads can be observed. The DLL
  does not force the game to load missing LODs or streamed mips.
- For a normal mip, a hash observed in the current session replaces older
  stored hashes at the same dimensions, allowing game-version updates without
  retaining every historical hash. Mips not observed in the session remain
  unchanged.
- If one session observes multiple distinct hashes at the same dimensions, the
  generated block is marked with `; asset_hash_mip_multiplicity = N`. On later
  sessions, that mip is replaced only after at least `N` hashes are observed
  together. An incomplete session adds its observed hash or hashes without
  deleting the marked set; a later complete session replaces the accumulated
  set atomically and refreshes `N`.
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
5. 打开 XXMI Launcher，进入`设置` > `高级`，启用`不安全模式`。这是让
   Launcher 使用自定义替换 DLL 启动游戏的必要条件。
6. 通过 XXMI Launcher 启动游戏。

### 快捷键

- `F7`：开启安全备份模式，输出 `mod.ini.hashcache`，不直接修改原
  `mod.ini`；任意捕获模式开启时按 `F7` 都会关闭捕获。
- `Shift+F7`：开启激进模式，原子替换当前加载的 `mod.ini`；在备份模式
  下可直接切换为激进模式，再按一次 `Shift+F7` 可关闭。
- `Ctrl+F7`：开启 Path 转换调试模式；只把当前游戏内重新验证成功的 Hash
  归一化为一条 active `match_asset_path` 写法，并原子替换原 `mod.ini`。未能
  验证的 Hash 连同 Mip 标记保留在同一个 `asset-hash-stream` 块内，方便排查
  候选过时 Hash，并供后续 F7 自动修复。
- `Alt+F7`：开启 Path 清理模式；验证 Path 有效后，删除对应
  `asset-hash-stream` 块内全部 Hash section，只留一条 active Path section。
  未通过当前运行时 Path 验证的生成块保持原样。
- `F10`：重新加载修改或替换后的 INI。

### Path/Name 简写

TextureOverride 内可以直接手写：

```ini
[TextureOverrideNum]
name = T_R2T1DaniyaMd10011Bangs02_D
handling = skip
```

`name` 与 `match_asset_name` 完全等价，`path` 与 `match_asset_path` 完全
等价；即使从不使用 F7，也会直接参与运行时匹配。简写只在
TextureOverride section 内生效，不能同时写简写和它自身对应的完整字段。
同一节允许同时写 Path、Name 以及
Hash/fuzzy 回退身份；运行时优先级为 Path → Name → Hash/fuzzy，因此失效
Hash 不会阻断有效的资产身份。各字段是否确实指向同一目标资源由作者保证。

启动任意 F7 模式后，即使尚未观察到匹配贴图，也会安排一次规范化写入；输出
统一恢复为 `match_asset_path`/`match_asset_name` 完整写法，其余命令保持不变。

### 推荐操作

激进模式最方便：在角色加载前按 `Shift+F7`，再加载角色并切换需要覆盖的
场景、距离、LOD、画面细节和外观状态。INI 更新时间变化后按 `F10`。如果
当前画面没有立刻恢复，再刷新场景或重新加载角色。完成后再次按
`Shift+F7` 或按 `F7` 关闭。

备份模式更安全：按 `F7` 后加载所需状态，找到生成的
`mod.ini.hashcache`，检查并备份原 `mod.ini`，再用生成文件替换原 INI，
最后按 `F10`。`.hashcache` 不会被当作第二份 mod INI 自动加载。

Path 转换调试：按 `Ctrl+F7` 后遍历所有相关场景、距离、LOD、画质和外观。
普通 Hash 写法必须先由当前 Hash 反查出唯一 Path 才会转换；已经由 F7 生成
的多 Mip 块也不会直接照抄注释 Path，而会用这条 Path 向当前游戏查询它现在
关联的 Hash。查询成功后，当前 Path 返回的 Hash 会被吸收到一条 active
`match_asset_path` section；无法确认的 Hash 原地保留。Path section 不写
`match_priority = 0`，但会和 Hash 形式一样保留完整的 `asset-hash-stream`
注释标记。残留 Hash、Mip 尺寸和 multiplicity 也保留在同一块内；之后用普通
`F7` 或 `Shift+F7` 转回 Hash 时，未收齐新 Hash 就增量保留，收齐后按 Mip
整组覆盖旧 Hash。

生成的 Path section 名直接使用 Unreal object name 作为后缀，例如
`[TextureOverride_T_R2T1DaniyaMd10011Hair01_D]`，并去掉原本冗余的 `_Texture`。
该后缀只用于保证 INI section 名更容易区分，不参与资源匹配；Path -> Hash ->
Path 往返会保留同一 name 后缀。如果同一 mod 存在不同 package 下完全同名的资产，
仍需要作者为它们保留不同的 section base name。旧版生成且没有 name 后缀的
active Path 块，会在下一次任意 F7 模式写入时自动规范化修复。

残留 Hash 的准确含义是“本次捕获未验证”，不是无条件证明它已经过时；如果
对应 LOD/Mip 没有实际加载，它同样会残留。完整遍历状态后仍稳定残留的 Hash
才是最有价值的版本更新排错目标。

Path 清理：`Alt+F7` 使用与 `Ctrl+F7` 相同的精确 Path 存活确认。只要当前
运行时观察到携带该 Path 的资源，就删除该块内全部新旧 Hash，只保留 active
`match_asset_path` section；不要求任何旧 Hash 仍然正确，也不要求新旧 Hash
存在交集。没有确认 Path 存活的生成块不会被清空。普通 Hash-only section
仍须由当前 Hash 唯一反查到 Path 后才会转换。

DLL 只能记录游戏实际加载过的贴图状态，不能强制游戏加载尚未出现的 LOD
或流式 Mip；因此需要主动展示对应角色、距离和画面细节。从 OFF 开启一次
`F7` 或 `Shift+F7` 会建立一个全新的捕获会话；从备份模式直接切换到激进
模式仍属于同一个会话。

普通 Mip 在本次会话捕获到新 Hash 后，会覆盖同尺寸的旧 Hash；本次没有
捕获到的其它 Mip 保持不变。若同一会话在同尺寸下确实捕获到多个不同 Hash，
生成块会写入 `; asset_hash_mip_multiplicity = N` 特殊注释。以后更新这个 Mip
时，只有单次会话完整捕获到至少 `N` 个同尺寸 Hash 才会替换旧集合；若只
捕获到不足 `N` 个，则只增量加入、不删除旧集合，直到后续某次会话完整捕获
后再整体替换并刷新 `N`。

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
