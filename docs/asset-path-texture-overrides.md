# Asset Path Texture Overrides

Asset path matching identifies Unreal Engine textures independently of their
runtime resource hash. The resolver is signature-driven and fails closed when
the required runtime functions cannot be identified. It does not use fixed
RVAs or executable-specific fallback profiles.

## TextureOverride syntax

Use the Unreal object name for the concise form:

```ini
[TextureOverrideExample]
match_asset_name = T_Example_D
if $object_detected
    this = ResourceExample
endif

[ShaderOverrideExample]
hash = 0123456789abcdef
checktextureoverride = ps-t0
```

Use the full Unreal object path when an explicit identity is required:

```ini
[TextureOverrideExample]
match_asset_path = /Game/Example/Textures/T_Example_D.T_Example_D
this = ResourceExample
```

`match_asset_name` and `match_asset_path` are mutually exclusive. Asset
identity matching also cannot be combined with `hash` or fuzzy texture match
options in the same section.

The short name is the suffix after the last `.` in the full object path. Name
matching is disabled for the remainder of the process if the same short name
is observed with more than one full path. Use `match_asset_path` when that
happens.

Asset path sections use the standard `checktextureoverride` command path.
Their pre commands run before the checked Draw or Dispatch and their post
commands run afterwards. They do not replace a resource merely because the
game bound it. This allows a shared game texture to use different replacements
for different objects by gating the TextureOverride with variables such as
`$object_detected`.

## Frame analysis

Add `asset_path` to the default frame analysis options to include captured
identities in frame analysis output:

```ini
[Hunting]
analyse_options = deferred_ctx_accurate share_dupes symlink asset_path
```

An F8 dump then writes `TextureAssetManifest.jsonl` in the frame analysis
directory. Each record contains the dump name, resource hashes, full asset
path, dimensions, mip count, and DXGI format.

Runtime asset path capture is always enabled for eligible shader-resource
Texture2D objects. This allows an asset identity TextureOverride introduced by
a config reload to match resources that were created before the override was
loaded. The `asset_path` frame analysis option only controls whether captured
identities are written to `TextureAssetManifest.jsonl`.

## Streaming hash authoring

The optional authoring mode is disabled by default. Configure and press F7 to
toggle it:

```ini
[Hunting]
toggle_asset_hash_capture = no_modifiers VK_F7
```

The current state is displayed at the top centre of the screen. All resolved
Texture2D Path/hash observations first enter a bounded recent cache, regardless
of whether a matching mod INI is already loaded. F10 promotes observations
matching newly loaded `match_asset_path` or unambiguous `match_asset_name`
identities into the authoring history. This prevents a mod added after the
initial texture upload from depending on the resource still being live.
F7 starts backup mode and writes `.hashcache` files. Shift+F7 starts
aggressive mode and atomically replaces the original loaded INI. File parsing
and writes run on a background worker rather than the texture creation path.
The modes are mutually exclusive. Shift+F7 can switch backup mode directly to
aggressive mode. Shift+F7 toggles aggressive mode off when it is already
active. F7 also stops either active mode; a subsequent F7 starts backup mode.

The recent cache is bounded to 32,768 Path identities and 131,072 hash
observations. Live resources remain eligible for an F10 rescan; after a
resource is released, its recent record is retained for five minutes and then
removed. Capacity pressure evicts the oldest released records first. Records
promoted for currently loaded INIs use a separate authoring-history limit of
8,192 identities, 32 hashes per identity, and 32,768 observations in total.
F10/source refresh removes promoted histories no longer referenced by loaded
mod INIs. A Path/Name longer than 2,048 characters is ignored.

For each source `mod.ini`, backup mode creates or updates
`mod.ini.hashcache`. The `.hashcache` suffix prevents the generated copy from
being loaded as a second mod INI. Aggressive mode applies the same transform
directly to `mod.ini` through a temporary file and atomic replacement.
The generated layout keeps the Resource section first, converts the original
asset matcher to a versioned comment marker, and emits one hash section per
observed streamed texture size:

```ini
[Resource_Texture]
filename = Texture.dds

; <asset-hash-stream>
; match_asset_name = T_Example_D
; asset_hash_compiler_version = Ver1.0
; game_version = 3.5.13
; 2048x2048
[TextureOverride_Texture_12345678]
hash = 12345678
this = Resource_Texture

; 1024x1024
[TextureOverride_Texture_23456789]
hash = 23456789
this = Resource_Texture

; </asset-hash-stream>
```

Sizes are sorted from the largest mip dimension to the smallest and hashes are
deduplicated. Every generated section preserves the original TextureOverride
body, so all streamed hashes keep pointing to the same Resource and retain the
same conditional commands.

`game_version` records the client version used for the capture. It is
informational metadata, not a refresh gate: enabling F7 always lets current
runtime observations update the corresponding generated mip dimensions even
when the recorded version is unchanged. The writer reads the active
`Resource/<version>/...` entries from the game's mounted
`MountResource.txt`; it does not use the executable file version, which
reports the Unreal Engine build instead of the client version. If the mounted
resource version cannot be resolved, the generated metadata uses `unknown`
rather than a stale hard-coded client version.

`asset_hash_compiler_version` identifies this authoring feature's output
contract. The current contract is `Ver1.0`. It is advanced only after a
feature/format update has been validated, rather than on every DLL build.
Older numeric markers remain readable and are migrated when rewritten.

F7 can also migrate a legacy TextureOverride containing only one active
`hash`. The hash must be observed as a Texture2D in the current recent cache
and uniquely resolve to one Asset Path. The original override body is cloned
to every observed streamed hash and the resolved Path becomes the commented
identity marker. Multiple legacy sections are merged into one streamed region
when every hash uniquely resolves to the same Path and their override bodies
are identical after trimming surrounding blank lines. If any hash is
unobserved, ambiguous, resolves to another Path, or the bodies differ, the
affected legacy sections are left unchanged.

When F7 refreshes its source set, live Texture2D resources referenced by pure
legacy hashes are scanned in addition to resources already carrying Path/Name
markers. This lets a currently valid legacy hash acquire its unique Path even
when the texture was created before F7 was enabled.

Generated regions retain the deduplicated union of hashes from the source INI,
the previous `.hashcache`, and current runtime observations. Multiple hashes
with the same width and height remain valid because graphics-detail changes or
different resource states can use distinct hashes at the same dimensions.
Texture2D hash changes caused by resource updates and copies are recorded when
the resource already has an Asset Path, so a transient state can survive after
the resource is released and still be written by F7.

Hash identity remains fail-closed across Asset Paths. If one observed hash maps
to more than one resolved full Path, the writer does not emit competing hash
overrides. Ambiguous hashes are removed individually while the same texture's
unique hashes remain generated. Only when no unique hash remains is the section
restored to its active `match_asset_path` or `match_asset_name`, preserving the
original body and conditions. This keeps shared low mip hashes from applying an
unrelated texture without discarding valid higher mip hashes.
Existing path/name comment markers and previously generated regions are
recognised, so repeated capture sessions do not nest or duplicate generated
blocks. Writes use a same-directory temporary file, `FlushFileBuffers`, and
an atomic replace.

## Runtime compatibility

The resolver signatures are independent of image layout and therefore do not
depend on fixed executable offsets. They can still require maintenance if a
game update changes the compiled function shapes or relevant Unreal Engine
object layouts. Unsupported builds remain on the original textures and report
`mode=failed-closed` in `AssetPathResolver.log`.
