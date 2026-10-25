# Format notes (verified against a live 2026 install)

Everything below was confirmed by reading a locally installed copy of the game
with `lolview`, not from memory or third-party dumps. Numbers are from one
specific patch; treat offsets as observations, not contracts.

## WAD v3 archives (`*.wad.client`)

* File starts with `RW`, then a small header and a 256-byte signature block.
* The table of contents is **not at a fixed offset** — it has moved between
  patches. It is located by scanning 16-byte-aligned candidates from 16 to 4096
  and scoring them: every entry must be structurally sane, all payload offsets
  must land after the TOC, and the strongest signal is the last payload ending
  exactly at end-of-file.
* Entries are 32 bytes: `xxh64` chunk key, `u32 offset`, `u32 compressed size`,
  `u32 uncompressed size`, `u8 compression`, `u8 duplicate`, 2 bytes padding,
  `u64` checksum (XXH3-64 of the stored bytes).
* **There are no filenames in the archive.** A chunk is addressed by
  `xxHash64(path.lower(), seed 0)`, so names must come from data files that
  reference them (see below) or from prior knowledge.

Compression codes seen in shipped data: `0` stored, `1` gzip, `2`/`3`/`4`
zstd, `0x24`/`0x34` zstd with flag bits. Many chunks use **`zstd-multi`**: a
concatenation of independent zstd frames that must be decoded in sequence.

Live check: `Ahri.wad.client` → 6,078 chunks, TOC at `0x110`, 187.3 MiB.

## Path conventions

```
DATA/Characters/<Champion>/<Champion>.bin            champion properties (PROP)
DATA/Characters/<Champion>/skins/skin<N>.bin        skin definition
DATA/Characters/<Champion>/animations/skin<N>.bin   VFX/SFX event table
ASSETS/Characters/<Champion>/Skins/Base/<Champion>_Base.skn   mesh
ASSETS/Characters/<Champion>/Skins/Base/<Champion>_Base.skl   skeleton
ASSETS/Characters/<Champion>/Skins/Base/<Champion>_Base_TX_CM.tex  texture
```

Skin 0 is `skins/skin0.bin` (no zero padding on the index). The skin file is
what names the mesh and skeleton; the champion `.bin` only names icons.

## Champion properties (`.bin`)

Starts with the magic `PROP`, then `u32` version `3`. Contains ability and icon
strings such as `ASSETS/Characters/Ahri/HUD/Icons2D/Icons_Ahri_Q.dds`. It does
**not** reference meshes.

## Skin definition (`skins/skin0.bin`)

Container that stores asset paths as plain strings, including the mesh and
skeleton for that skin, plus `DATA/Characters/.../Animations/...` references and
Wwise sound banks. This is the entry point for discovering a champion's models:

| Chunk | Compressed | Decoded |
|---|---|---|
| `data/characters/ahri/skins/skin0.bin` | 3,665 B | 20,361 B |
| → `ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn` | 452,884 B | 847,032 B |
| → `ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skl` | 8,831 B | 25,908 B |

## SKN (mesh)

Header begins `33 22 11 00` (`0x00112233`), then `u16 major`, `u16 minor`,
`u32 material_count`. Each material is a 64-byte name plus four `u32`s
(`start_vertex`, `num_vertices`, `start_index`, `num_indices`).

Version 4 inserts a metadata block before the buffers: `u32` unknown,
`u32 index_count`, `u32 vertex_count`, `u32 vertex_stride`, `u32` unknown,
`f32 bbox[10]`. Versions 1–3 go straight to the two counts and use a fixed
52-byte stride. Versions are tried in the order the version number implies.

Vertices are 52 bytes:

| Offset | Field |
|---|---|
| 0 | `f32 position[3]` |
| 12 | `u8 bone_indices[4]` |
| 16 | `f32 weights[4]` |
| 32 | `f32 normal[3]` |
| 44 | `f32 uv[2]` |

Positions are already in bind pose, so a static render needs no skinning.
A file whose element counts do not account for its size is rejected, which is
what makes the layout detection safe.

Verified on `Ahri_Base.skn` (version 4.1, stride 52, 847,032 bytes):

| Material | Vertices | Indices |
|---|---|---|
| Body | 9,721 | 45,162 |
| Eyes | 226 | 1,248 |
| Tails | 3,450 | 16,776 |
| Tail_Large | 383 | 1,848 |
| **total** | **13,780** | **65,034** (21,678 triangles) |

Bounds: `min (-157.48, 0.05, -196.42)`, `max (157.48, 205.94, 18.26)` — a
~206-unit-tall character with the tails spread wide and trailing behind, which
is the sanity check that the parse is right.

## TEX (texture)

```
u32 magic "TEX\0"
u16 width
u16 height
u8  version (1)
u8  format code (0x0a = BC1, 0x0c = BC3)
u16 flags
--- BCn mip chain, mip 0 LAST (smallest mip first)
```

Two traps, both confirmed against real files:

1. **The mip chain is stored smallest-first**, so mip 0 sits at
   `payload.size() - mip0_size`. Decoding from the front yields a tile of the
   tiny mips — an easy way to think the decode is broken when it is not.
2. **The format is not reliably identified by the header byte alone.** The
   payload size decides: a 1024×1024 BC1 chain is 699,064 bytes, the BC3 chain
   is 1,398,128. `Ahri_Base_TX_CM.tex` is 1,398,140 bytes = 12-byte header +
   BC3 chain; `Jinx_Base_TX_CM.tex` is 349,556 = 12 + BC1 chain at 1024×512.

Colour maps are atlases (face, hair, dress, hands all in one image). UVs in the
`.skn` are already normalised and **do not need a V flip** when the mesh and
texture are used together.

Verified: `Ahri_Base_TX_CM.tex` decodes to a clean 1024×1024 BC3 atlas;
`Ahri_Base.skn` renders fully textured with correct skin, hair, dress and boot
mapping.

## Animation paths

Animations live beside the skin, not in a champion-wide folder:

```
ASSETS/Characters/<Champion>/Skins/Base/Animations/Idle1.anm
ASSETS/Characters/<Champion>/Skins/Base/Animations/Run.anm
ASSETS/Characters/<Champion>/Skins/Base/Animations/Spell1.anm
ASSETS/Characters/<Champion>/Skins/Base/Animations/attack1.anm
```

Names are inconsistent (`Idle1` capitalised, `attack1` lower case), and there is
no directory listing, so discovery is by hash search — `lolview --find` tests a
candidate list in one pass. Files are large (Idle1 is 219,790 → 382,984 bytes).

## SKL (skeleton)

Header: `u32 file_size`, `u32 token 0x22FD4FC3`, `u32 version 0`, `u16 flags`,
`u16 joint_count`, `u32 influences_count`, then six `i32` section offsets. Each
joint is 100 bytes: `u16 flags, i16 id, i16 parent, i16 pad, u32 hash, f32 radius`,
local TRS, bind TRS, and an `i32` name offset *relative to that field*.

Two things that are easy to get wrong:

1. **The mesh does not index joints.** `Ahri_Base.skn` stores bone indices
   0..176 while the skeleton has 211 joints — the mesh indexes the **influence
   table** (`influences_count` = 177 entries of `i16` joint ids). Using the
   indices directly against joints produces the classic "character explodes into
   spikes" render.
2. Animation translations are **local**, not world: 177 of 211 joints match
   their bind-local translation exactly at frame 0. Only the root-level
   `C_Buffbone_Glb_*` attachments carry world-scale values.

Verified: 211 joints, 177 influences; bind pose skinning is an exact identity.

## ANM (animation)

`r3d2anmd`, versions 3/4/5. v5 (current) layout: `u32 track_count` at 0x1C,
`u32 frame_count` at 0x20, `f32 frame_duration` at 0x24, section offsets at
0x28/0x34/0x38/0x3C. Every section is preceded by a 12-byte header. Frames are
6 bytes (`u16` translation index, scale index, rotation index), laid out
frame-major then track-minor. Joint hashes sit in their own section (v5) or in
each frame (v4).

Rotations are 6-byte quantised quaternions (three 15-bit components plus a 2-bit
"largest component" selector). Scale indices are always 0 and palette entry 0 is
`(1,1,1)`, so scale can be substituted without decoding.

Verified: Idle1 is v5, 237 tracks, 120 frames, 4.00 s at 30 fps; the sampled
pose changes over time and stays anatomically coherent.

## VFX (partially reversed)

Earlier sweeps failed because the naming assumption was wrong. With a community
hash dictionary (`CommunityDragon/Data`, `hashes/lol/hashes.game.txt.*`) the real
paths appear, and they are confirmed present in `Ahri.wad.client`:

```
assets/characters/ahri/skins/base/particles/ahri_base_q_sphere_01.scb
assets/characters/ahri/skins/base/particles/ahri_base_e_heart_01.scb
assets/characters/ahri/skins/base/particles/ahri_base_r_blastwave.scb
assets/characters/ahri/skins/base/particles/ahri_base_w_swirls_01.scb
assets/characters/ahri/skins/base/particles/common_magicball.scb
```

18 of 55 dictionary candidates resolve. Two facts explain the earlier dead end:

1. **`.scb` is the particle mesh**, not the system: the file starts with
   `r3d2Mesh`. Particle *geometry* is a mesh, as the `r3d2Mesh` histogram entry
   suggested.
2. **The emitter graphs live inside the PROP bins**, not in separate
   `.preload`/`.troy`/`.sco` files in this build (`.sco` dictionary entries do
   not resolve in the archive). A VFX bin decodes to strings such as:

```
FireCards_FIXED
ASSETS/Characters/Ahri/Skins/Base/Particles/Ahri_Base_Wall_Erosion.tex
ASSETS/Characters/Ahri/Skins/Base/Particles/common_bigradialhalo.tex
ASSETS/Characters/Ahri/Skins/Base/Particles/common_magicBall.scb
Ahri_Taunt8
Characters/Ahri/Skins/Skin0/Particles/Ahri_Taunt
```

That gives every ingredient for real effects: the system name, its particle
meshes and its textures. PROP v3 begins `PROP`, `u32 version = 3`, `u32` string
table offset (12 in observed files), then `u16`-length-prefixed strings; the
object tree follows. Field keys are hashes, so naming them needs
`hashes.binfields.txt` from the same dictionary.

Status: **decoded and implemented in C++** (`src/formats/bin.cpp`,
`src/formats/vfx_def.cpp`), verified by rendering `Ahri_Base_Q_mis` from the real
bin with its own textures in the sandbox.

### PROP v3 layout

```
u32  magic "PROP"
u32  version (3)
u32  linked_file_count, then that many strings (u16 length + bytes)
u32  entry_count
u32  entry_type_hash[entry_count]
entry:
  u32 length          (covers path hash + field count + fields)
  u32 path_hash
  u16 field_count
  field[field_count]
field:
  u32 name_hash (FNV-1a 32 of the name)
  u8  type
  value
```

Types: `0` empty, `1` bool, `2..10` integers/float, `11..13` vec2/3/4,
`14` mat4, `15` rgba, `16` string, `17` hash, `18` path (xxh64), `0x80/0x81` list,
`0x82` struct, `0x83` embedded, `0x84` link, `0x85` option, `0x86` map, `0x87` flag.
Lists are `u8 value_type, u32, u32 count`; structs are `u32 type, [u32, u16 count]`
then fields. Hashes are FNV-1a 32, so field names can be derived without a
dictionary — the community `hashes.binfields.txt` is only needed for discovery.

### VFX system entries

An entry of type `VfxSystemDefinitionData` carries `particleName`, `particlePath`
and `complexEmitterDefinitionData`: a list of `VfxEmitterDefinitionData`, one per
emitter. Fields observed and used:

| Field | Meaning |
|---|---|
| `rate` | particles per second (constant or animated) |
| `particleLifetime` | lifetime in seconds |
| `birthVelocity` / `birthDrag` | initial velocity and drag |
| `birthColor` | RGBA at birth |
| `Color` | animated RGBA over `times`/`values` |
| `SpawnShape.emitOffset` | spawn offset relative to the caster |
| `primitive` | e.g. `VfxPrimitiveArbitraryQuad` (camera-facing quad) |
| `blendMode` | 1 = alpha, 4 = additive (observed) |
| `emitterName` | human-readable emitter label |

`Ahri_Base_Q_mis` (bin chunk `0xaa1b62a07d7b6785`) is 16 emitters; the same bin
holds `Ahri_Base_R_mis` and the three `R_Indicator` systems. The bin itself has
no path in the archive, so it is addressed by hash; find bins by scanning decoded
chunks for a `particleName` string.

Base-skin VFX bin: chunk `0xaa1b62a07d7b6785`, 13 entries.

## Skinning

`skin[i] = animated_global * inverse(bind_global)` in column-vector form, with
the result re-indexed through the influence table. 211 joints is too many
matrices for default vertex uniforms (1024 constant registers), so bone matrices
go in a **uniform buffer** (`std140`, 256 mat4 = 16 KiB).

## Reproducing these numbers

```powershell
lolview --game-dir "H:\Riot Games\League of Legends" --archive Ahri.wad --list --limit 5
lolview --game-dir "..." --archive Ahri.wad --lookup "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn"
lolview --game-dir "..." --archive Ahri.wad --extract "data/characters/ahri/skins/skin0.bin" --out skin0.bin
python scripts/asset_strings.py skin0.bin --grep "\.(skn|skl)$"
lolview --game-dir "..." --archive Ahri.wad --mesh "ASSETS/.../Ahri_Base.skn" `
        --texture "ASSETS/.../Ahri_Base_TX_CM.tex" `
        --skeleton "ASSETS/.../Ahri_Base.skl" `
        --anim "ASSETS/.../Animations/Idle1.anm" --time 2.5 `
        --render --size 800x1000 --out ahri.png
```
