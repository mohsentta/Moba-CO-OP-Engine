# Skeleton + animation support (ports from LeagueToolkit specs)

Working code:

| file | role |
|---|---|
| `src/skl.rs` | `.skl` rig parser: 211 joints (flags, id, parent, radius, name hash, local TRS, inverse-bind TRS), influences table, hash→joint map |
| `src/anm.rs` | `.anm` parser: `r3d2anmd` v3/v4/v5, palette frames, 6-byte quantized quaternions (v5), slerp/lerp sampling |
| `src/bin/anim.rs` | loads SKL+SKN+ANM from a champion WAD, skins on the CPU, renders a PNG |
| `src/bin/sklcheck.rs` | convention solver |

## Formats

**SKL** (`token 0x22FD4FC3`): header `u32 size, u32 token, u32 version(0), u16 flags, u16 joint_count, u32 influences_count, i32 joints_off, joint_indices_off, influences_off, name_off, asset_name_off, bone_names_off, i32 ext[5]`.
Joint = 100 bytes: `u16 flags, i16 id, i16 parent, i16 pad, u32 name_hash, f32 radius, f32[3] t, f32[3] s, f32[4] q, f32[3] ib_t, f32[3] ib_s, f32[4] ib_q, i32 name_off` (name relative to the field).
Influences: `i16[influences_count]` mapping **SKN bone index → joint id**.

**ANM**: magic `r3d2anmd` (uncompressed) or `r3d2canm` (compressed, not parsed yet).
v5/v4 header after magic: `u32 size, u32 token, u32 version, u32 flags, u32 track_count, u32 frame_count, f32 frame_duration, i32 joint_hashes_off, asset_name_off, time_off, vector_palette_off, quat_palette_off, frames_off`; sections start at `offset + 12`.
v5 quats are 6 bytes (15-bit components + 2-bit omitted index); v4 quats are 16-byte floats.
Frames: v5 `u16 translation_id, u16 scale_id, u16 rotation_id` per track; v4 embeds `u32 joint_hash` per frame.

## Conventions (verified numerically with `sklcheck`)

- Rotation matrices must be **transposed** for row-vector math.
- Global = `local * parent_global` (row-vector order).
- `sklcheck` on the real rig: `transpose=true, parent_first=false` → mean error **0.0001**, i.e. the file's inverse-bind matrices cancel the bind pose exactly.
- Inverse bind is derived numerically from the bind pose (`invert(bind_global)`), so bind pose is exact by construction.

## Status

- Bind pose: correct (`assets/ahri/bind4.png`).
- Animated pose: coherent body pose (`assets/ahri/animInf.png`) but some accessory/weapon vertices are still mis-bound — the remaining issue is per-mesh bone binding, not the format or the math.
- Textures/materials: not yet (shading is normal + light).

## Commands

```
anim <wad> <skl path> <skn path> <out.png> [--bind] [--t=SECONDS]
sklcheck <file.skl>
```
