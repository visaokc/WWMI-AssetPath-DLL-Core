# DEV_LOG

## 2026-08-20 - ShapeKey capture diagnostics and atomic repair gate

- Runtime correction: the first UAV-role build was installed and the following
  F7 run left the YangYang INI byte-for-byte unchanged. Therefore that run did
  not prove an incorrect replacement; it proved the ShapeKey writer was never
  reached with a complete accepted pair. The previously stated ambiguity cause
  was not sufficiently verified.
- Diagnostics: local agent `STATUS` now exposes bounded `asset_capture` state:
  mode, selected VB/native vertex count, VB/ShapeKey observation counts, probe
  count, and the latest 16 ShapeKey hash/size/filter/slot records. This makes
  the next runtime check identify the exact missing stage without INI markers
  or broad frame dumps.
- Safety: when a selected VB family has ShapeKey roots, VB0, Component ranges,
  offsets, and scale are now one atomic repair unit. Missing or ambiguous
  offsets/scale returns the original document rather than committing a partial
  VB/Component update that collapses the model.
- Recovery: the live YangYang INI was manually returned to the known-good
  ShapeKey hashes and backed up at
  `D:\WWMI\Backups\ManualRecovery-20260820-111100`.
- Verification: native document, Draw Debug gate, unrestricted agent control,
  release INI contract, and `git diff --check` passed. `Release|x64` rebuilt
  with 212 pre-existing warnings and zero errors; DLL SHA256 is
  `DF32EF04B0CB4016E0B860233FB067A6DD9FBB0AECBE8ABD87580C7414803BBC`.
- Installation boundary: the game remains open, so the diagnostic/safety DLL
  is not installed. Runtime diagnosis remains pending after game exit.

## 2026-08-20 - ShapeKey UAV role correction

- Symptom: YangYang became valid immediately after manually replacing offsets
  `7fe8c94e -> 1a646636` and scale `81378bbb -> 6fe81411`, proving that the
  Component/VB repair was correct and the remaining failure was entirely in
  automatic ShapeKey selection.
- Root cause: the capture path enumerated every CS SRV and UAV bound to each
  WWMI ShapeKey loader/multiplier Dispatch, while the transformer selected the
  scale candidate primarily by byte width. Unrelated same-sized resources made
  the candidate count ambiguous, so the safe writer correctly refused the
  ShapeKey replacement. The prior unit fixture also modeled scale as an SRV on
  slot 2 instead of the observed runtime contract.
- Fix: capture now reads only the actual original Dispatch roles: offsets from
  UAV0 and scale from UAV1. The transformer requires offsets to be UAV0 and
  scale to be UAV1, each observed in both loader and multiplier stages with the
  selected model's exact native vertex-count size.
- Performance: each relevant Dispatch now queries two UAV slots instead of all
  128 CS SRV slots plus all 8 CS UAV slots. Capture OFF remains unchanged.
- Verification: the native document test now includes the real YangYang hashes,
  UAV roles, and an unrelated same-sized SRV that must not create ambiguity.
  Native document, Draw Debug gate, unrestricted agent control, release INI
  contract, and `git diff --check` passed. `Release|x64` rebuilt with 212
  pre-existing warnings and zero errors; DLL SHA256 is
  `0DF63533F2FA05F20652B8F767D5612111439CB3D3AA80148E4329D5D34AA9A0`.
- Installation: after confirming the game process was absent, the rebuilt DLL
  was copied to `D:\WWMI\d3d11.dll` and its SHA256 was verified. The previous
  DLL, `d3dx.ini`, and YangYang INI were backed up under
  `D:\WWMI\Backups\ShapeKeyUavRole-20260820-110423`; neither INI changed during
  installation. User-driven runtime acceptance remains pending.

## 2026-08-20 - First-frame ShapeKey observation retention

- Symptom: the current-model selector correctly updated YangYang VB0 to
  `efd45ce3` and repaired C4-C6 ranges, but left ShapeKey offsets/scale at
  their obsolete hashes. The Component overrides then redirected VB0 to an
  uninitialized `ResourceShapeKeyedPosition`, collapsing the visible model.
- Runtime proof: FrameAnalysis showed all seven updated
  `[TextureOverride\YY\Component*]` sections matching successfully, while the
  loader/multiplier resources were already bound as offsets `1a646636` and
  scale `6fe81411` before the first character draw selected the target INI.
  Their dispatch sizes also agree with the selected native count `65173`:
  offsets use `ceil(65173 * 24 / 256) = 6110` groups and scale uses
  `ceil(65173 / 64) = 1019` groups.
- Root cause: ShapeKey probing was gated on the target profile and native
  vertex count. Both become available only after draw-family selection, so
  ShapeKey Dispatches occurring earlier in the frame were discarded and could
  not participate in the first write.
- Fix: during any F7 capture mode, WWMI loader/multiplier resources are now
  retained once per `(resource hash, filter role)` from session start. The
  target-only transformer still applies the native vertex-count, stage, slot,
  UAV, uniqueness, and old-root association checks after model selection.
- Performance: capture OFF is unchanged. Capture ON adds only the already
  bounded and deduplicated loader/multiplier probes; no general compute-resource
  scan or cross-INI ShapeKey write was enabled.
- Verification: native document, Draw Debug gate, unrestricted agent control,
  release INI contract, and `git diff --check` passed. `Release|x64` rebuilt
  with 212 pre-existing warnings and zero errors; DLL SHA256 is
  `F414F008F2374D2AF2893AB9F57DEF807EAB8B7F03ACDB2BA724C9ADBDCD16A6`.
- Installation: after WuWa exited, the rebuilt DLL was copied to
  `D:\WWMI\d3d11.dll` and its SHA256 was verified. The previous DLL,
  `d3dx.ini`, and the damaged YangYang INI were backed up under
  `D:\WWMI\Backups\FirstFrameShapeKey-20260820-102621`. Installation did not
  modify either INI; user-driven F7 runtime acceptance remains pending.

## 2026-08-20 - Native VB identity correction

- Symptom: all four F7 modes generated Path state but never selected the
  current model, so VB0, Component ranges, and ShapeKey hashes were not sent
  through the target-only writer.
- Runtime proof: an agent-only frame dump in the user-selected stable scene
  found YangYang VB0 `efd45ce3`, stride `12`, and ByteWidth `782076`, which is
  `65173` native vertices. The INI's `global $mesh_vertex_count = 78034`
  describes the exported Mod Position buffer, not the native host VB. The
  previous equality gate could therefore never select this INI.
- Fix: F7 now builds a compact inverted index of loaded INI
  `(match_first_index, match_index_count)` signatures. Each live VB hash is
  scored independently; two signatures are required for a two-Component
  family and three for larger families, and the winning source must be unique.
  Only that source is fully parsed and eligible for pathless VB/Component and
  ShapeKey repair. Exported mesh vertex count is no longer an identity gate;
  the selected native VB count remains available only for ShapeKey buffer-size
  correlation.
- Performance: capture OFF remains one atomic flag read. During F7, INIs are
  parsed once into a compact inverted index, each `(VB hash, draw signature)`
  is scored once, and Buffer size is queried once per unseen VB hash until a
  target is selected. Full Component parsing and writes remain target-only.
- Validation: the live YangYang draw family matched only
  `D:\WWMI\Mods\YangYang\mod.ini` across all current `D:\WWMI\Mods` INIs
  (four unchanged signatures out of seven). Native document, Draw Debug gate,
  unrestricted agent control, release INI contract, and `git diff --check`
  passed. `Release|x64` rebuilt with 212 pre-existing warnings and zero errors;
  DLL SHA256 is
  `CFAEE5E019B8F64BBF7A780616CF96F9E669914F99F6464184B70104BEED3C07`.
- Installation: after WuWa exited, the rebuilt DLL was installed and read back
  at `D:\WWMI\d3d11.dll` with the build SHA256. The previous DLL and unchanged
  live INIs were backed up under
  `D:\WWMI\Backups\NativeVbIdentity-20260820-093550`; live `d3dx.ini` and
  YangYang `mod.ini` remained byte-identical. User-driven F7 runtime acceptance
  remains pending.

## 2026-08-20 - Current-model pathless VB family selection

- Purpose: remove Path and agent dependencies from normal VB0, Component, and
  ShapeKey repair while preventing one character INI from being inferred from
  another character's Component structure.
- Target selection: each F7 mode reads only the lightweight
  `global $mesh_vertex_count` identity from loaded INIs. A live VB0 selects a
  target only when its vertex count maps to exactly one INI. Only then does the
  writer parse that selected document's VB family; ambiguous counts do not
  write VB or ShapeKey changes.
- Repair behavior: the selected document accepts a pathless live draw family,
  so one unique structural match can replace VB0 and current
  `match_first_index` / `match_index_count` values. ShapeKey observations are
  restricted to buffers sized from the selected model's vertex count and the
  existing WWMI loader/multiplier roles. Agent capture remains diagnostic-only.
- Performance: capture OFF retains the atomic fast path. During F7, each unseen
  VB hash queries its Buffer size once until the model is selected; selected
  draw signatures and compute resources remain session-deduplicated. Other
  character INIs are never parsed for Component comparison.
- Verification: native document tests cover mesh-count extraction, pathless
  target-only repair, and non-target rejection. Native tests, Draw Debug human
  gate, unrestricted agent control, release INI contract, and
  `git diff --check` passed. `Release|x64` rebuilt with 212 pre-existing
  warnings and zero errors. DLL SHA256:
  `A3590A289DF691733A8BE50280BDC40A0008E7ACA62AFBCC6F47D52FBAEC1389`.
- Live boundary: loaded Mod metadata currently maps vertex count `78034`
  uniquely to `D:\WWMI\Mods\YangYang\mod.ini`. That INI remained unchanged at
  SHA256 `AE8EFC71F286540BF8B599B909D36F2F6DAEC01AC9BB3AF596421BE0592A5B12`.
  After WuWa exited, the DLL was installed and read back at `D:\WWMI\d3d11.dll`
  with the build SHA256. The previous DLL and unchanged live `d3dx.ini` were
  backed up under
  `D:\WWMI\Backups\CurrentModelPathless-20260820-065140`; `d3dx.ini` and the
  YangYang INI remained byte-identical. User-driven in-game F7 acceptance
  remains pending; no Mod INI was repaired by this round.

## 2026-08-20 - Unrestricted agent frame-resource and shader dump control

- Purpose: make the local agent channel sufficient for future diagnostics
  without repeatedly extending the DLL or enabling the human Hunting UI.
- Behavior: the named pipe now queues generic `DUMP` work for the render
  thread. `DUMP FRAME` accepts arbitrary standard FrameAnalysis options,
  including CB/VB/IB/texture/RT/depth payload and output-format flags.
  `DUMP SHADER` exports any loaded shader by hash and optional stage as original
  assembly, DXBC, or both; `DUMP SHADERS TARGET` exports shaders learned by an
  armed target capture. Shader files are isolated under `AgentDumps\Shaders`
  and cannot be picked up as `ShaderFixes` overrides.
- Human boundary: keyboard F8/F11 behavior is unchanged and still requires
  both the configured feature and Hunting. Only the local named-pipe agent path
  bypasses both gates, so it remains available even if `[DrawDebug] enabled`
  is false.
- Client/protocol: `tools/wwmi_draw_debug_client.py` adds `dump-frame`,
  `dump-shader`, `dump-target-shaders`, and `dump-raw`; `STATUS` reports queue,
  completion, output path, and error state.
- Isolation: implemented in worktree
  `D:\MOD\BlenderAddonProjects\.worktrees\wwmi-agent-unrestricted-dump` on
  branch `agent/unrestricted-dump`, leaving the concurrent main-worktree DLL
  changes untouched until commits `fbe8957` and `5dae25a` were ready. Both were
  then merged without a source conflict; the only manual merge was preserving
  both sets of `DEV_LOG.md` entries.
- Verification: static agent/Hunting contracts, Python syntax/client CLI,
  native Asset Hash document tests, release INI checks, and `git diff --check`
  passed. Final `Release|x64` rebuild completed with 212 pre-existing warnings
  and zero errors. DLL SHA256:
  `6AB021DC35D831427BFE6A77E4996E3545A42D3F0EC04FF1086993AAD5A82DE2`.
- Installation: after WuWa exited, the combined DLL was installed and read
  back at `D:\WWMI\d3d11.dll` with the same SHA256. The prior DLL and unchanged
  live INI were backed up under
  `D:\WWMI\Backups\AgentUnrestrictedDump-20260820-020409`.
## 2026-08-20 - First-pass Path-to-VB observation retention

- Symptom: `D:\WWMI\Mods\YangYang\mod.ini` gained valid generated Path
  blocks in Aggressive F7 mode, but VB0, Component ranges, and ShapeKey Hashes
  all remained unchanged.
- Root cause: `ObserveVbHashForAuthoring()` accepted only Paths already present
  in `watched_identities` when F7 started. On a legacy-Hash-only INI, the draw
  probe ran before the writer generated its Path, rejected the VB observation,
  and then deduplicated that draw signature for the rest of the session.
- Fix: retain bounded Path/VB draw observations during the active F7 session.
  `TransformVbHashIniDocument()` still consumes only Paths present in the
  transformed target document, so unrelated runtime Paths cannot create an INI
  replacement. The existing 65,536-observation cap, per-signature probe
  deduplication, ambiguity rejection, and capture-off atomic fast path remain.
- Regression: added a first-pass integration test that starts from a legacy
  texture Hash section, generates a Path, and repairs the linked VB family in
  the same transform pass. Native document, Draw Debug gate, and release INI
  contract tests pass. `Release|x64` rebuilt with 212 pre-existing warnings and
  zero errors; DLL SHA256 is
  `8950D153D5A533D1EC64C8C29B47BDB2BE15BAF92EB90664433150DC79AC2992`.
- Real fixture check: the YangYang document exposes one linked VB family
  (`15fb50a9`), and an offline replay with the reported current Component and
  ShapeKey observations updated all requested fields. The prior assumption
  that `if $object_detected` was not recognized was disproved; the existing
  variable scanner already treats it as the consumer of
  `$object_detected = 1`.
- Files: `DirectX11/AssetHashCapture.cpp`,
  `tests/asset_hash_ini_document_tests.cpp`, and `README.md`.
- Installation: after WuWa exited, the rebuilt DLL was installed at
  `D:\WWMI\d3d11.dll` and read back with the same SHA256. The previous DLL,
  live `d3dx.ini`, and current YangYang `mod.ini` were backed up under
  `D:\WWMI\Backups\FirstPassVbRepair-20260820-014819`; neither INI was edited
  during installation. Runtime acceptance remains pending until YangYang is
  tested again.

## 2026-08-20 - Path-gated ShapeKey and Component range repair

- Purpose: extend all four F7 authoring modes beyond VB0 replacement so a
  trusted Path-linked host can also repair changed `match_first_index`,
  `match_index_count`, ShapeKey offsets Hash, and ShapeKey scale Hash.
- Component behavior: exact draw-signature matching remains the first route.
  Changed ranges are accepted only when one current VB0 produces one unique
  contiguous Component sequence and no same-variable old family has the same
  structural shape. Hash-unchanged range updates are supported; split, merged,
  reordered, partial, or multi-candidate families remain unchanged.
- ShapeKey behavior: the DLL samples bound CS buffers only for WWMI's
  `3381.3333` loader and `3381.4444` multiplier callbacks. A ShapeKey pair is
  associated through the resolved Path/VB family and current vertex count;
  offsets require the unique UAV0 buffer seen at both stages, while scale
  requires one unique four-byte-per-vertex buffer. Alternate sections whose
  names carry an eight-digit host VB Hash are associated with that host family.
  Every active ShapeKey TextureOverride reference to a resolved old Hash is
  updated without adding INI comments or markers.
- Performance boundary: capture OFF uses a lock-free atomic flag read. During
  capture, stored VB0/draw signatures remain the fast path; unknown ranges are
  probed once, and ShapeKey correlation arms one deduplicated PS/CS resource
  scan per candidate rather than continuous per-frame scanning.
- Compatibility boundary: implemented incrementally on top of commits
  `a6d2460` and `07476f8`; the adjacent Hunting-independent agent Draw Debug
  changes were retained. No checkout, reset, or wholesale DLL source
  replacement was performed.
- Key files: `DirectX11/AssetHashCapture.cpp/.h`,
  `DirectX11/AssetHashIniDocument.cpp/.h`, `DirectX11/HackerContext.cpp`,
  `tests/asset_hash_ini_document_tests.cpp`, and `README.md`.
- Verification: native Asset Hash document tests cover changed ranges,
  unchanged-VB0 ranges, complete ShapeKey reference replacement, and VB/scale
  ambiguity rejection; Draw Debug hunting-gate regression passed;
  `git diff --check` passed. Full `Release|x64` rebuild completed with 212
  pre-existing warnings and zero errors. DLL SHA256:
  `68EEB289CC5CF33B96DCE6291E277E612F8AF0517C0C957B728F69694C731D2F`.
- Installation: with WuWa closed, the combined DLL was installed at
  `D:\WWMI\d3d11.dll`; the previously installed adjacent-window DLL and live
  INI were backed up under
  `D:\WWMI\Backups\ExtendedF7Repair-20260820-010421`. The live `d3dx.ini`
  remained byte-identical with SHA256
  `1E14D07F4ECDA067DA018793BD3CC40725D4E45064533BDE9710937ED31AB547`.
- Runtime acceptance: static/native/build/install validation is complete.
  Current WuWa ShapeKey slot identity and a real updated Component family still
  require an in-game F7 pass before this is considered end-to-end accepted.

## 2026-08-20 - Hunting-independent agent Draw Debug control

- Purpose: let the local named-pipe agent start continuous, targeted, and
  heavy Draw Debug captures without enabling Hunting, while preserving the
  Hunting gate for human F11/F8 input.
- Behavior: `[DrawDebug]` parsing remains thread-free. The render-thread update
  lazily starts `\\.\pipe\wwmi-draw-debug` whenever Draw Debug is configured.
  Agent `START`, `ARM`, and `SNAPSHOT` bypass Hunting; keyboard paths still call
  the gated implementations, and an agent-owned stream is not stopped merely
  because Hunting is off.
- Compatibility boundary: the implementation was added on top of commit
  `a6d2460` and retains its Path-gated lazy VB0 repair. No checkout, reset, or
  replacement of the prior Asset Hash/VB0 files was performed.
- Key files: `DirectX11/DrawDebugStream.cpp`, `DirectX11/Hunting.cpp`,
  `tests/check_draw_debug_hunting_gate.ps1`, and `README.md`.
- Verification: the Draw Debug agent/human gate contract and Asset Hash/VB0
  native tests passed; `git diff --check` passed; `Release|x64` rebuilt with
  212 pre-existing warnings and zero errors. DLL SHA256:
  `75563B526427F7618304A1E69A0C6827EBBFA9856BC0A138C91DC6372F0EDF10`.
- Installation: with WuWa closed, the combined DLL was installed at
  `D:\WWMI\d3d11.dll`. The previous DLL and unchanged live INI were backed up
  under `D:\WWMI\Backups\AgentDirectDrawDebug-20260820-001502`.
- Runtime acceptance: build/static validation is complete; direct agent-pipe
  capture and live VB0 repair still require a new WuWa run for end-to-end
  acceptance.

## 2026-08-19 - Path-gated VB0 repair with lazy observation

- Purpose: add automatic VB0 hash repair to all four F7 capture modes while
  keeping old texture/VB0 hashes out of identity decisions.
- Behavior: trusted `match_asset_path` identities gate observation; VB0
  candidates are identified from existing Path-linked object variables plus
  `match_first_index`/`match_index_count`. A replacement is written only when
  one complete new hash maps to the same draw-signature family; ambiguity is
  left unchanged. No new INI comments or markers are emitted.
- Performance boundary: current VB0 hash is cached on vertex-buffer binding.
  Normal draws whose VB0 remains a stored candidate pay only a set lookup.
  PS texture-path probing runs once per unseen VB0/draw signature, so repair
  work is limited to stale-path cases rather than every F7 draw.
- Key files: `DirectX11/AssetHashCapture.cpp/.h`,
  `DirectX11/AssetHashIniDocument.cpp/.h`, `DirectX11/HackerContext.cpp`,
  `DirectX11/ResourceHash.cpp/.h`, and `tests/asset_hash_ini_document_tests.cpp`.
- Verification: native Asset Hash document tests passed, Draw Debug hunting
  gate passed, and `Release|x64` rebuilt with zero errors. Build output DLL
  SHA256: `C0504E2B3E37560A5D51962DD866A69C0579C49F8008031F63097BFA292A3F0E`.
- Runtime/game acceptance: not run in WuWa; live Daniya multi-VB0 acceptance
  remains required before installing or packaging a public build.

## 2026-08-15 - v1.0.3 Draw Debug lifecycle and corrected WWMI package

- Purpose: keep the custom F11/agent Draw Debug feature available without
  creating background threads while the game is loading. The previous
  `enabled = true` path created both writer and named-pipe threads directly
  from INI parsing and never provided a complete thread/file shutdown path.
- Lifecycle behavior: parsing `[DrawDebug]` now records configuration only.
  Entering Hunting starts the cancellable overlapped named-pipe server; leaving
  Hunting stops and joins it. Lightweight capture creates its writer thread and
  output file only when capture actually starts, then drains the queue, joins
  the writer, flushes, and closes the file when capture stops.
- Safety behavior: named-pipe connect/read/write operations wait on a dedicated
  stop event and use `CancelIoEx`, preventing shutdown from hanging on a blocked
  pipe client. Output-path access is synchronized with the existing queue lock.
- Verification: the lifecycle contract test first failed against eager startup,
  then passed after the fix. Asset Hash document tests and release INI contract
  also pass. Full `Release|x64` rebuild completed with 212 pre-existing warnings
  and zero errors; DLL SHA256:
  `8F46A23E834A25E5170B9141EBBC2994FA5C6D8F25C66274DCFEA8D4A3DFBC03`.
- Live A/B: with the game closed, the rebuilt DLL was installed and live
  `[DrawDebug] enabled = true` restored. The previous live DLL/INI are backed up
  at `D:\WWMI\Backups\DrawDebugLazyLifecycle-20260815-082739`. Repeated game
  startup/exit plus Hunting and F11 lifecycle testing passed without the prior
  startup crash, so the verified live INI is packaged byte-for-byte with Draw
  Debug enabled by default.
- Release package: `_archive/zip-packages/WWMI-AssetPath-DLL-v1.0.3.zip`
  contains only the live-tested DLL and INI. Extracted artifacts match their
  source hashes and pass the complete release contract. ZIP SHA256:
  `4DAE4D02AB22165C19A7CDDEC805003B25301C6C569F323009E888E5219CD545`.

## 2026-08-15 - v1.0.2 release INI root-cause correction (local only)

- Confirmed root cause: the published v1.0.2 ZIP used the repository's generic
  XXMI `d3dx.ini` instead of the user's working WWMI configuration. It targeted
  `Game.exe` and omitted `include = Core\WWMI\WuWa-Model-Importer.ini`, so the
  game could start while WWMI Core and all Mods remained unloaded. Replacing
  only the INI with `D:\WWMI\d3dx.ini` restored Mod loading with the same DLL.
- Release-source rule: future packages must derive `d3dx.ini` from the user's
  known-working live `D:\WWMI\d3dx.ini`, not from a generic XXMI template.
- Regression boundary: `tests/check_release_ini_utf8.ps1` now validates the
  complete release contract: strict UTF-8 without BOM, the exact WWMI target,
  WWMI Core include, Mods include, all four F7 bindings, and default-enabled
  lazy-lifecycle Draw
  Debug. The published v1.0.2 INI fails this new test at the WWMI target check;
  the corrected INI passes it. A package that fails any item must not be
  released.
- Local validation package:
  `_archive/zip-packages/WWMI-AssetPath-DLL-v1.0.2-ini-fix-local-test.zip`
  contains only `d3d11.dll` and the corrected `d3dx.ini`. Extracted files pass
  the release contract and match their source hashes. ZIP SHA256:
  `F52D364146C49A44FBA5AFA104BF58961BF28FBA8900664691D78B202359AB51`.
- Remote boundary: this correction remains local. Do not push, tag, or modify a
  GitHub Release without explicit user permission in the current turn.

## 2026-08-15 - v1.0.2 canonical naming and UTF-8 release fix

- Purpose: make every generated TextureOverride name independent of an
  author's custom section suffix and fix the Launcher UTF-8 decode failure in
  the v1.0.1 GitHub package.
- Naming behavior: Path output now always uses
  `[TextureOverride_<object-name>]`; Hash output always uses
  `[TextureOverride_Texture_<hash>]`. Both conversions discard all prior text
  after the TextureOverride prefix. Generated Hash bodies add
  `match_priority = 0` when no active priority is already present.
- Encoding diagnosis: the repository and v1.0.1 ZIP `d3dx.ini` both contained
  one CP1252 smart-apostrophe byte `0x92` at byte offset 34589, exactly matching
  the Launcher error reported by affected users. The release template now uses
  strict UTF-8 without BOM, and a dedicated regression test rejects future
  invalid UTF-8 or BOM output.
- Key files: `DirectX11/AssetHashIniDocument.cpp`,
  `tests/asset_hash_ini_document_tests.cpp`,
  `tests/check_release_ini_utf8.ps1`, `Dependencies/d3dx.ini`, `README.md`,
  `docs/asset-path-texture-overrides.md`, and `DEV_LOG.md`.
- Verification: Asset Hash document tests, release INI UTF-8 validation, and
  Draw Debug Hunting-gate tests passed. Full `Release|x64` rebuild completed
  with zero errors. The installed DLL matches SHA256
  `2F782BE0823495428746F1F6203EC5C23A0FCA4CFA2156F94C3E13408D7FF56B`;
  the previous live DLL/INI are backed up at
  `D:\WWMI\Backups\CanonicalAssetSections-20260815-074843`.
- Package: `WWMI-AssetPath-DLL-v1.0.2.zip` contains only `d3d11.dll` and the
  corrected `d3dx.ini`. ZIP SHA256:
  `6B283E53EFB887180775E996CE1955270E884F990D7247BB259B2789A131DF25`.

## 2026-08-15 - v1.0.1 release

- Purpose: publish the complete authoring/debug update accumulated since the
  original v1.0.0 stable Asset Path package.
- Release scope: Mip-aware F7/Shift+F7 replacement, Ctrl+F7 diagnostic Path
  conversion with in-stream residuals, Alt+F7 validated Path cleanup, runtime
  `path`/`name` aliases and identity priority, readable Path section names,
  compiler marker `Ver1.1`, and Hunting-gated F11/agent Draw Debug control.
- Documentation: `README.md` now identifies v1.0.1 as the current release and
  includes English and Chinese update briefs plus release checksums.
- Package: `_archive/zip-packages/WWMI-AssetPath-DLL-v1.0.1.zip` contains only
  `d3d11.dll` and the release-template `d3dx.ini` at the ZIP root. SHA256:
  `7637FAF78B8978B9AC3A49B0163FB7FCED60F7872733FDE39C1BFADD49A37496`.
  DLL SHA256:
  `9A206DB7BCB4FCD2E43F5905821CBE754DFB6D3AEB2B652BA53A0DDFB4EEB595`.
  INI SHA256:
  `14C5F24A4D9A0963002C33EC405CD78D5A16047F7AD7DE2643A80457B66E4E1F`.
- Verification: native Asset Hash document tests and the Draw Debug Hunting
  gate test passed; the final `Release|x64` build completed with zero errors.
  The previously installed live DLL already matches the packaged DLL.

## 2026-08-15 - Unique readable Path section names

- Purpose: fix pure-Path texture scrambling caused by Ctrl+F7/Alt+F7 reducing
  multiple generated overrides to the duplicate section name
  `[TextureOverride_Texture]`.
- Output behavior: a generated Path override now uses its Unreal object name as
  the section suffix and removes the redundant generic `_Texture` base, for
  example `[TextureOverride_T_R2T1DaniyaMd10011Hair01_D]`. The section name is
  stable across Path -> Hash -> Path round trips. Same-named assets from
  different packages still require distinct author-chosen section bases.
- Repair behavior: generated active-Path blocks using the old generic section
  name request canonicalization on any F7 mode. The repair needs only the
  active Path identity and does not require a currently valid stored Hash.
  Residual Hash sections remain in the same stream and inherit the readable
  Path section base.
- Key files: `DirectX11/AssetHashIniDocument.cpp/.h`,
  `DirectX11/AssetHashCapture.cpp`, `tests/asset_hash_ini_document_tests.cpp`,
  `README.md`, `docs/asset-path-texture-overrides.md`, and `DEV_LOG.md`.
- Verification: native `/W4 /WX` tests cover two Paths sharing the former
  generic base, legacy-block repair, and stable Path -> Hash -> Path naming.
  Full `Release|x64` rebuild completed with zero errors and produced SHA256
  `9A206DB7BCB4FCD2E43F5905821CBE754DFB6D3AEB2B652BA53A0DDFB4EEB595`.
  After confirming the game and launcher were closed, the DLL was installed to
  `D:\WWMI\d3d11.dll`; source and installed SHA256 hashes match. The previous
  DLL and live INI were backed up to
  `D:\WWMI\Backups\AssetPathSectionNames-20260815-070514` before deployment.

## 2026-08-15 - Hunting-gated F11 and agent Draw Debug control

- Purpose: prevent accidental F11 FrameAnalysis/dump capture during normal
  gameplay and apply the same Hunting-mode safety boundary used by F8.
- Input behavior: F11 key-down, short heavy capture, and long-press lightweight
  streaming now require `G->hunting == HUNTING_MODE_ENABLED`. Draw Debug no
  longer enables or restores Hunting internally. Leaving Hunting mode clears a
  pending F11 hold and stops an active lightweight stream.
- Agent behavior: the local-only named pipe now rejects `START`, `ARM`, and
  `SNAPSHOT` with `ERROR hunting mode required` outside Hunting mode. `STOP`
  remains available, and `STATUS` exposes `hunting_required` plus
  `control_allowed`. The render-thread control path repeats the gate and drops
  stale start/snapshot requests for defense in depth.
- Audit boundary: the agent pipe provides only fixed capture/filter/mark
  commands, rejects remote pipe clients, uses bounded filter tables and a
  bounded telemetry queue, and cannot select arbitrary filesystem paths or
  execute processes. Heavy payload types remain explicitly controlled by the
  static `[DrawDebug] options` profile; the live profile dumps CB data but not
  textures, RT/depth, VB, or IB payloads.
- Key files: `DirectX11/Hunting.cpp`, `DirectX11/DrawDebugStream.cpp/.h`,
  `tests/check_draw_debug_hunting_gate.ps1`, `README.md`, and `DEV_LOG.md`.
- Verification: the dedicated hunting-gate contract test and Asset Hash document
  tests passed. Full `Release|x64` rebuild completed with zero errors. The
  installed DLL SHA256 is
  `F4F6F071C3A12C3E9DC5309E2D68D6080FBC272FAA9271D006B406C339695946`.
  The previous DLL and live INI were backed up to
  `D:\WWMI\Backups\DrawDebugHuntingGate-20260815-064113` before deployment to
  `D:\WWMI\d3d11.dll`.

## 2026-08-15 - Ctrl+F7 residual Mip continuity

- Purpose: keep Ctrl+F7 diagnostic residuals associated with their Path and
  Mip group so a later F7 or Shift+F7 pass can repair them automatically.
- Format behavior: unverified Hash sections now remain inside the same
  versioned `asset-hash-stream` block as the active `match_asset_path` section.
  Their resolution comments and `asset_hash_mip_multiplicity` marker are
  preserved instead of emitting the residual sections after the closing
  marker.
- Round-trip behavior: when the Path form returns to Hash form, an incomplete
  observed Mip set is merged additively with residuals. Once the observed set
  reaches the stored multiplicity, it replaces the complete old Mip group.
  Alt+F7 can now remove the Path block and all of its residual Hashes together.
- Key files: `DirectX11/AssetHashIniDocument.cpp`,
  `tests/asset_hash_ini_document_tests.cpp`, `README.md`,
  `docs/asset-path-texture-overrides.md`, and `DEV_LOG.md`.
- Verification: native tests cover residual placement before the stream end,
  multiplicity preservation, incomplete additive update, complete replacement,
  and Alt+F7 whole-block cleanup. Full `Release|x64` rebuild completed with zero
  errors. The installed DLL SHA256 is
  `8B94DA792C711BF0BF13543453A30554470644B6B42D2392D86833A970651111`.
  The previous DLL and live INI were backed up to
  `D:\WWMI\Backups\AssetResidualMipContinuity-20260815-063503` before
  deployment to `D:\WWMI\d3d11.dll`.

## 2026-08-15 - Alt+F7 validated Path cleanup

- Purpose: add a cleanup counterpart to Ctrl+F7 for authors who have finished
  version-update diagnosis and want to remove the complete stored Hash list
  from a generated block.
- Runtime behavior: `Alt+F7` enters `PATH CLEANUP`. Ordinary Hash-only sections
  retain the unique current Hash-to-Path conversion requirement. A generated
  `asset-hash-stream` block still uses its commented Path for exact runtime
  liveness confirmation. Once a resource carrying that Path is observed, the
  whole stored Hash list is removed and one active `match_asset_path` section
  remains. No stored Hash match or old/new Hash overlap is required. A
  generated block without current Path evidence is left unchanged.
- Boundary: Ctrl+F7 remains the diagnostic mode and preserves unverified Hashes;
  Alt+F7 deliberately removes them after Path validation. Both write the loaded
  INI through atomic replacement, omit `match_priority = 0` from the Path
  section, and retain the versioned `asset-hash-stream` marker.
- Key files: `DirectX11/AssetHashCapture.cpp/.h`,
  `DirectX11/AssetHashIniDocument.cpp/.h`, `DirectX11/Hunting.cpp`,
  `Dependencies/d3dx.ini`, `README.md`,
  `docs/asset-path-texture-overrides.md`, and
  `tests/asset_hash_ini_document_tests.cpp`.
- Verification: native document tests passed both the no-evidence no-op and
  whole-block removal with zero overlap between stored and current Hashes. Full
  `Release|x64` rebuild completed
  with zero errors. The installed DLL SHA256 is
  `1F5497FB160AD6612F22AA50AD962BED7992DE6FC6D5E11343151EDE1860B4BA`.
  The DLL and live `d3dx.ini` were backed up to
  `D:\WWMI\Backups\AssetPathCleanup-20260815-062834` before deployment; the
  live Alt+F7 binding was then added to `D:\WWMI\d3dx.ini`.

## 2026-08-15 - Runtime `path` and `name` identity aliases

- Purpose: make Asset Path TextureOverrides practical to hand-author without
  requiring the long `match_asset_path` and `match_asset_name` field names.
- Runtime behavior: inside TextureOverride sections, `path` is equivalent to
  `match_asset_path` and `name` is equivalent to `match_asset_name`, including
  ordinary command bodies such as `handling = skip`. The aliases are registered
  as known TextureOverride keys, so they work without enabling any F7 mode.
- Validation boundary: short and full spellings of the same identity cannot be
  combined, and `name` must remain a bare Unreal object name. Path, Name, and
  Hash/fuzzy fallback identities may coexist in one section. Runtime resolution
  is ordered Path, then Name, then Hash/fuzzy; authors remain responsible for
  making independently written fields describe the same intended resource.
- Authoring behavior: the F7 document parser canonicalizes both aliases to the
  full field names. Starting any F7 mode schedules canonicalization when an
  alias is present even if no matching resource has been observed. Multiple
  Path/Name identities are preserved and canonicalized when output remains
  identity-based. Alias detection is scoped to TextureOverride sections and
  ignores unrelated `name` fields elsewhere in an INI.
- Key files: `DirectX11/IniHandler.cpp`,
  `DirectX11/AssetHashIniDocument.cpp/.h`,
  `DirectX11/AssetHashCapture.cpp`, `DirectX11/ResourceHash.cpp`, `README.md`,
  `docs/asset-path-texture-overrides.md`, and
  `tests/asset_hash_ini_document_tests.cpp`.
- Verification: dedicated `/W4 /WX` document tests passed alias recognition,
  canonical identity keys, no-observation normalization, body preservation,
  and non-TextureOverride exclusion. Full `Release|x64` rebuild completed with
  zero errors. The installed DLL SHA256 is
  `89C011B85534D44F90D1AF8F1F626DA150B40C53281EA91313BC5D4A99FAB67D`;
  it was deployed to `D:\WWMI\d3d11.dll` after backing up the previous DLL and
  live INI to
  `D:\WWMI\Backups\AssetIdentityFallback-20260815-061917`.

## 2026-08-15 - Runtime-validated Ctrl+F7 Path conversion

- Purpose: add a game-version update diagnostic that converts currently valid
  Hash overrides back to one active Asset Path matcher while leaving
  unverified Hashes visible for author review.
- Key files: `DirectX11/AssetHashCapture.cpp/.h`,
  `DirectX11/AssetHashIniDocument.cpp/.h`, `DirectX11/Hunting.cpp`,
  `Dependencies/d3dx.ini`, `README.md`,
  `docs/asset-path-texture-overrides.md`, and
  `tests/asset_hash_ini_document_tests.cpp`.
- Input and write boundary: `Ctrl+F7` enters `PATH CONVERSION`; it uses the
  aggressive mode's temporary-file atomic replacement for the loaded INI.
  Starting from OFF resets the capture session, while switching directly from
  Backup or Aggressive preserves current observations.
- Conversion policy: ordinary Hash-only sections require a unique current
  Hash-to-Path resolution. Previously generated blocks do not trust their
  commented identity: that exact Path must be observed alive in the current
  runtime, without requiring any stored Hash match. Current Hashes associated
  with the live Path collapse into one active `match_asset_path` section;
  unverified Hashes remain inside the same generated block and are not silently
  deleted.
- Round-trip and format policy: Path and streamed-Hash forms both use the
  `asset-hash-stream` marker with `asset_hash_compiler_version = Ver1.1`.
  Active Path output omits `match_priority = 0`; a later F7 or Shift+F7 can
  consume the marked Path block without copying the active identity into a
  generated Hash section body.
- UX boundary: a residual Hash means it was not validated in the current
  capture. It is a strong stale-Hash candidate only after the author has
  exercised all relevant scenes, LODs, Mips, graphics settings, and variants.
- Verification: the dedicated `/W4 /WX` native transform suite passed legacy
  Hash conversion, marker output, Path-to-current-Hash validation, residual
  preservation, `match_priority = 0` removal, and
  Path-to-Hash round-trip. Full `Release|x64` rebuild completed with zero
  errors; SHA256
  `E542AF4848BCF1DD4DD90601E5F15C0D8310B7177DB85D48E06DD70467C6BF77`.
- Installation: after confirming the game and XXMI Launcher were not running,
  installed the verified DLL to `D:\WWMI\d3d11.dll` and added only the
  `toggle_asset_hash_path_conversion` binding to the existing `d3dx.ini`.
  Backup: `D:\WWMI\Backups\AssetHashPathConversion-20260815-054617`.

## 2026-08-02 - Session-scoped mip hash replacement and multiplicity markers

- Purpose: stop treating every observed streaming hash as permanent additive
  history while preserving genuine same-mip multi-hash resource states.
- Key files: `DirectX11/AssetHashCapture.cpp`,
  `DirectX11/AssetHashIniDocument.cpp`, `README.md`, and
  `tests/asset_hash_ini_document_tests.cpp`.
- Capture boundary: each OFF-to-F7 or OFF-to-Shift+F7 transition clears the
  in-memory observation set and starts a new session. A direct Backup-to-
  Aggressive switch keeps the current session.
- Replacement policy: an unmarked mip is replaced by the hashes observed at
  the same dimensions in the current session. Observing multiple hashes at one
  mip emits `; asset_hash_mip_multiplicity = N`. Later sessions replace that
  mip only after observing at least `N` hashes together; an incomplete capture
  remains additive until a complete capture can replace the accumulated set.
  Mips absent from the session remain unchanged.
- Safety boundary: ambiguous cross-Asset-Path observations are removed before
  replacement decisions, so an unsafe new hash cannot delete a stored safe
  hash. Generated blocks now identify the compiler as `Ver1.1`.
- Verification: the dedicated native `/W4 /WX` transform suite passed Backup,
  Aggressive, single-hash replacement, multi-hash discovery, incomplete and
  complete updates, multiplicity expansion, unknown-resolution round-tripping,
  unmarked-history cleanup, ambiguity handling, and idempotence. Full
  `Release|x64` rebuild completed with zero errors; SHA256
  `94081CF26D7FAEE2F8EC601729524668A2100ED5D8DC26B7CE7230C651B8D6DC`.
- Installation: after confirming no game or Launcher process was running,
  installed the verified DLL to `D:\WWMI\d3d11.dll`. Backup:
  `D:\WWMI\Backups\AssetHashMipMultiplicity-20260802-172743`. No process was
  started or stopped, and `d3dx.ini` was not modified.

## 2026-08-02 - Register DrawDebug as a known INI section

- Fixed the in-game `Unknown section type - [DrawDebug]` overlay by registering
  `DrawDebug` in the native INI section whitelist. The configuration and Agent
  stream were already functional; this was a parser diagnostics omission.
- Final `Release|x64` build completed with zero errors; SHA256
  `603B9A4A2AB4ABFDBC2CCAA917A3C65F9F6AA5121C9A7DC1931FF595991C8BCA`.
- After confirming the game and Launcher were closed, installed the corrected
  DLL to `D:\WWMI\d3d11.dll`. Backup:
  `D:\WWMI\Backups\DrawDebugSectionFix-20260802-082657`.

## 2026-08-02 - Continuous Agent stream and targeted automatic capture

- Purpose: cover short-lived visual states without continuously running heavy
  FrameAnalysis resource dumps.
- Added `DirectX11/DrawDebugStream.cpp/.h`: bounded non-blocking draw queue,
  asynchronous JSONL writer, local-only `\\.\pipe\wwmi-draw-debug` control,
  status/drop counters, markers, and targeted draw filters.
- Input behavior: short `F11` press retains the one-frame heavy snapshot; holding
  for `long_press_ms` starts the lightweight stream and releasing stops it.
- Agent behavior: `START`, `STOP`, `SNAPSHOT`, `STATUS`, `MARK`, and targeted
  `FILTER DRAW`/`ARM` commands. Render-thread D3D11 work is never performed by
  the pipe thread.
- Targeted Daniya workflow: `tools/wwmi_draw_debug_client.py arm
  D:\WWMI\Mods\Daniya7\mod.ini` extracts component index-count/first-index
  signatures, records direct matches plus learned VS/PS-related draws, and queues
  one automatic heavy snapshot on first detection.
- Performance boundary: continuous records contain compact draw arguments,
  shader hashes, and IB/VB0 hashes only. Full CB/resource payloads remain in the
  bounded heavy snapshot. Queue contention/overflow drops telemetry and reports
  a counter instead of blocking rendering.
- Verification: dedicated native smoke passed for the writer, JSONL record
  count, named-pipe status, sequential command reconnect, targeted filter,
  bounded non-match exclusion, and automatic snapshot request. The final
  `Release|x64` build completed with zero errors; SHA256
  `A7232A8DC867AC2FFD2D5B51F028B701A264B61BC0CE60C905A07CAA2D735977`.
- Installation: after confirming the game and Launcher were closed, installed
  the verified DLL to `D:\WWMI\d3d11.dll` and surgically added
  `long_press_ms`/`max_queue_records` to the existing enabled `[DrawDebug]`
  section. Backup: `D:\WWMI\Backups\DrawDebugStream-20260802-081955`.

## 2026-08-02 - Configurable bounded Draw Debug capture

- Purpose: add a reusable runtime diagnostic path for whole-frame character,
  effect, parallax, shadow, depth, and motion-vector draw investigation without
  repeatedly patching the DLL for new resource types.
- Key files: `DirectX11/Hunting.cpp`, `DirectX11/Hunting.h`,
  `DirectX11/HackerDXGI.cpp`, `Dependencies/d3dx.ini`, and `README.md`.
- Behavior: `[DrawDebug] enabled = true` prepares FrameAnalysis in
  soft-disabled hunting mode. The configurable `toggle` key starts one bounded
  complete-frame capture and automatically restores the previous hunting and
  FrameAnalysis options after Present. A second key press aborts an active
  capture.
- Default evidence: full native draw/state log, shader and resource identity,
  descriptors, Asset Path identity, and deduplicated CB binary/text payloads.
  Expensive texture/RT/VB/IB payloads remain opt-in through the existing
  FrameAnalysis option vocabulary.
- Boundary: D3D11 cannot directly identify a character owner, so the feature
  captures the complete frame and preserves TextureOverride/command-list
  evidence for offline correlation instead of filtering out independent effect
  or parallax draws.
- Performance: `enabled = false` retains the normal fast context. Enabled but
  idle uses soft-disabled hunting and only the fixed FrameAnalysisContext
  overhead; GPU readback and disk output occur only during a bounded capture.
- Verification: `build-d3d11.ps1` completed successfully for `Release|x64`;
  output `x64/Release/d3d11.dll` SHA256
  `C7B11A2C7C8332302CF7D0363C87498E6DECF1A759E446561D6419AAA7DD77FC`.
- Installation: copied the verified build to `D:\WWMI\d3d11.dll` and appended
  an enabled `[DrawDebug]` section to the existing `D:\WWMI\d3dx.ini` without
  replacing unrelated configuration. The previous DLL and INI were backed up
  under `D:\WWMI\Backups\DrawDebug-20260802-080351`. No game or Launcher
  process was started or stopped.
