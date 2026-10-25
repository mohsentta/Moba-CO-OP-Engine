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
| Champion mesh, skeleton, colour map | Ahri 13,780 verts / 211 joints; Jinx 9,998 verts / 88 joints |
| Champion naming variants | candidate paths per asset (mesh, skeleton, texture, clips) |
| Animation | uncompressed clips animate; `r3d2canm` clips parse but are not yet evaluated |
| Weapon families | Jinx `minigun` / `rlauncher` clip sets, `Q` swaps the active set |
| Ability casts with real VFX | Q/W/R from the game's VFX bin; E procedural |
| League camera | 55° pitch, follow, wheel zoom, `Y` unlock, `-`/`+` 45° steps |
| Movement | right-click order, `S` stop, Run/Idle switching |
| HUD | real ability icons (per-weapon Q art), cooldowns, health/mana, target bar |
| Jungle pets | `durian_*` from Map11.wad |
| Minions | melee minion mesh + texture from Map12.wad |
| Scenario units | `scripts/scenario.txt`: `unit <model> <hp> <dx> <dz> [team]` |
| Rift terrain | 586 pieces, 2.04 M verts / 910 k tris, 343 textures |
| Transparency | cutout for foliage/decals, blended surfaces |
| Map editor (`F2`) | free-fly camera, searchable piece list, click-select, click-to-cycle, red wireframe highlight, move/rotate/scale, 200-step undo, save/load |
| Picking | triangle-accurate ray test, `--pick-test` probe |

## Stated goals and where they live

The end goal is recreating the old co-op modes — **Doom Bots**, **Star Guardian
Invasion**, **Odyssey Extraction** — playable co-op. Everything below is ordered
so each step is usable on its own; see `README.md#roadmap` for the full
milestones.

| Goal | Milestone | Notes |
|---|---|---|
| Play every animation a champion ships | M1 | `r3d2canm` parser done; evaluator outstanding |
| Pathfinding + walkable ground | M2 | navmesh/grid from map data, A*, collision, ground snap |
| Spell casting (cast times, projectiles, damage) | M3 | data-driven spells, status effects, auto-attacks |
| Minions, waves, AI | M4 | spawners, aggro, leash, lane waypoints |
| Scriptable rules (Lua) | M5 | `spawn`, `on_tick`, `on_cast`, timers |
| Network co-op | M6 | host-authoritative UDP, snapshots, asset agreement |
| Mode framework (waves, objectives, modifiers, augments) | M7 | modifiers = Doom Bots, augments = Star Guardian / Odyssey |
| Recreate the co-op modes | M8 | Star Guardian → Odyssey → Doom Bots |

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
