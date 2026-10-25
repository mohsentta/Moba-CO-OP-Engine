# Milestone 1 — playable Rift sandbox

Tag: `v0.1.0`

A local League of Legends practice sandbox that reads everything from the
user's own installation. No game bytes are stored in this repository; every
asset is resolved by path hash at runtime and decoded in memory.

## What runs

```powershell
.\build\lolview_sandbox.exe --game-dir "C:\Riot Games\League of Legends" `
    --champion Ahri --pet durian_miner --map `
    --vfx-bin aa1b62a07d7b6785
```

| Feature | State |
|---|---|
| Champion mesh, skeleton, colour map | 13,780 verts / 21,678 tris, 211 joints |
| Animation (Idle1, Run, Spell1/2/4, attack1) | CPU pose + GPU skinning |
| Ability casts with real VFX | Q/W/R from the game's VFX bin; E procedural |
| League camera | 55° pitch, follow, wheel zoom, `Y` unlock |
| Movement | right-click order, `S` stop, Run/Idle switching |
| HUD | real ability icons, cooldowns, health/mana, target bar |
| Jungle pets | `durian_*` from Map11.wad, bind pose |
| Rift terrain | 586 pieces, 2.04 M verts / 910 k tris, 343 textures |
| Transparency | cutout for foliage/decals, blended surfaces |
| Map editor | per-piece move/rotate/scale, selection brackets, save/load |

## Verified numbers

- WAD: `Ahri.wad` 6,078 chunks, `Map11.wad` 28,461 chunks (2.4 GB, read in
  chunks).
- Terrain: `base_srx.mapgeo` → 586 meshes, 2,042,809 vertices, 910,783
  triangles, 183 materials, 10 unresolved textures.
- Scene start-up ~6 s; ~200 fps at 1600x900 with the whole map.

## Controls

| Input | Action |
|---|---|
| right click | move champion |
| `S` | stop |
| `Q` `W` `E` `R` | cast at cursor |
| wheel | zoom |
| `Y` / `SPACE` | toggle camera lock / re-centre |
| `F2` | free-fly camera (WASD, Space/Ctrl, Shift boost, right-drag look) |
| `ESC` | quit |

Editor: pick a piece from the list or click it in the world; `move`, `yaw`,
`scale`, `visible`, `Save scene` (writes `scenes/map.scene`, applied on launch).

## Known limits

- No ground collision: the champion walks on a flat plane, so she floats or
  sinks on slopes.
- Pets have no animation data in the dictionary and stand in bind pose.
- 10 materials (vertex-deform props, decals) resolve no texture.
- Water is blended but not refracted/reflected.
- Editor has no undo, multi-select or grid snapping.
- No minions, turrets, or audio.

## Formats implemented

WAD v3, SKN, SKL, ANM, TEX, DDS, PROP/VFX bins, and MAPGEO (OEGM v13–18,
including the v18 four-byte quirk). See `docs/FORMATS.md` and `docs/MAP.md`.

## Legal

The tool ships no Riot content: assets are read from the user's own install,
never redistributed. See `docs/LEGAL.md`.
