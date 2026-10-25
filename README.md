# lolview

A standalone viewer and sandbox for League of Legends models and effects, written
in C++20. It reads assets directly from a League of Legends installation that
**you already own** — it ships none of them.

> Not affiliated with, endorsed by, or sponsored by Riot Games. League of Legends
> and all associated assets are trademarks and copyright of Riot Games, Inc.
> This project distributes **no** game content: no models, no textures, no
> archives, no binaries, no decompiled code. You point it at your own install;
> it reads that directory read-only. See [docs/LEGAL.md](docs/LEGAL.md).

## Status

**Goal:** recreate old co-op modes — **Doom Bots**, **Star Guardian Invasion**,
**Odyssey Extraction** — playable co-op, from the player's own install.

| Milestone | Scope | State |
|---|---|---|
| M0 | foundation: install/WAD/chunk hashing, payload decoding, SKN/SKL/ANM/TEX readers, GL renderer, sandbox | done |
| M0b | world content: mapgeo terrain (586 pieces, 343 textures, alpha), jungle pets, minions, scenario units | done |
| M0c | editor: free-fly camera, triangle picking, wireframe highlight, transforms, undo, scene save/load | done |
| M1 | **animation completeness** — `r3d2canm` parser done, evaluator outstanding | in progress |
| M2 | **terrain-aware movement + pathfinding** (height snap, collision, navmesh/A*) | planned |
| M3 | **combat core**: spell casting, cast times, projectiles, damage types, status effects | planned |
| M4 | entities & AI: minions, waves, spawners, aggro, leash | planned |
| M5 | **Lua scripting** runtime (`spawn`, `on_tick`, `on_cast`) + fixed-tick sim | planned |
| M6 | **co-op networking**: host-authoritative UDP, snapshots, asset agreement | planned |
| M7 | mode framework: waves, objectives, **modifiers** (Doom Bots), **augments** (Star Guardian / Odyssey) | planned |
| M8 | **recreate the co-op modes**: Star Guardian → Odyssey → Doom Bots | goal |

Definitions of done and per-milestone prerequisites: see [Roadmap](#roadmap).

Working today, verified against a live install:

* `AssetSource` resolves an install, refuses anything that is not one, and has
  no bundled-asset fallback by construction.
* `Wad` derives the table of contents by structural scan (it moves between
  patches) and lists 6,078 chunks in `Ahri.wad.client` in ~0.2 s.
* `--lookup` / `--extract` resolve `xxh64` path keys and decode stored, gzip,
  zstd and multi-frame zstd payloads, enforcing the TOC size.
* `--mesh` parses a real `.skn`: Ahri's base mesh is 4 materials, 13,780
  vertices, 21,678 triangles.
* `--tex` decodes a `.tex` to PNG (12-byte header, BC1/BC3, mip 0 last).
* `--skeleton` / `--anim` / `--time` skin the mesh with a real animation:
  211 joints, 177 influences, 237 animation tracks at 30 fps.
* `--render` draws any of that to a PNG through a hidden SDL3 window with an
  OpenGL 4.6 core context, an FBO and a `std140` bone buffer.
* `lolview_sandbox` runs the practice tool: animated champion, free camera,
  ability casts with effects, dummy target, ImGui panel, and a headless
  capture mode (`--capture`, `--capture-after`, `--auto-cast`).
* `--find` bulk-tests candidate virtual paths against an archive by hash.
* `--effect q|w|e|r` spawns a deterministic particle effect at `--time`
  (additive point sprites, procedurally generated sprite).
* Findings and path conventions: [docs/FORMATS.md](docs/FORMATS.md).

### A note on effects

Effects now come from the game: `lolview` parses PROP v3 bins, decodes
`VfxSystemDefinitionData` entries into emitter definitions (rate, lifetime,
spawn offset, birth velocity/colour, animated colour curves, blend mode, world
scale) and renders them with the game's own `.tex` particle textures. The sandbox
falls back to built-in procedural effects when no bin is supplied. Details:
[docs/FORMATS.md](docs/FORMATS.md).

## Build

Requires CMake ≥ 3.25 and a C++20 compiler (MSVC 2022, Clang 16+, GCC 13+).
zstd and zlib are fetched and built automatically, so there are no system
dependencies; the first configure needs network access.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To use pre-installed libraries instead: `-DLOLVIEW_FETCH_DEPS=OFF`.

## Practice sandbox

`lolview_sandbox` is the practice tool, built to feel like the game: a locked
isometric camera that follows the champion, right-click movement, cursor-aimed
casts, and a League-style HUD with the champion's own ability icons.

```powershell
.\build\lolview_sandbox.exe --game-dir "C:\Riot Games\League of Legends" --champion Ahri `
    --vfx-bin aa1b62a07d7b6785
```

| Input | Action |
|---|---|
| right click | move the champion (plays `Run`, returns to `Idle1`) |
| `Q` `W` `E` `R` | cast at the cursor (spell clip + real VFX, cooldowns apply) |
| mouse wheel | zoom |
| `Y` | toggle camera lock |
| `SPACE` | re-centre and lock |
| `-` / `+` | rotate the camera 45° per press |
| `F2` | editor mode: free-fly camera (WASD, Space/Ctrl, Shift boost, right-drag look) plus the editor window |
| `Ctrl+Z` | undo the last piece transform (editor) |
| `ESC` | quit |

The HUD draws the real ability icons, cooldown timers, champion health/mana,
the target dummy's health bar and the champion's selection ring. Casting locks
the spell clip until it finishes, then returns to movement/idle.

Camera feel can be tuned without a rebuild: `--pitch 40 --distance 880`.

### Map editor

With `--map`, the whole Rift is loaded as 586 individually editable pieces
(textured from the map archive, cached on the GPU). The "Map editor" window lets
you:

- pick a piece from the searchable list, or click it in the world (clicking only
  selects while editor mode is active)
- move it (`move`), rotate it (`yaw`) and scale it (`scale`) — rotation and
  scaling pivot around the piece's own centre
- hide pieces, reset one piece or everything
- **Ctrl+Z** undoes transform changes (200-step history)
- **Save scene** to a text file (`--scene path`, default `scenes/map.scene`)
  with one line per piece: `index px py pz yaw_degrees scale visible`

The saved scene is applied automatically on the next launch, so an assembled
layout persists. `--save-scene` writes the current layout at startup.

```powershell
.\build\lolview_sandbox.exe --game-dir "..." --champion Ahri --map `
    --scene scenes\my_layout.scene
```

Headless capture (used for regression screenshots):

```powershell
.\build\lolview_sandbox.exe --game-dir "..." --champion Ahri `
    --auto-cast q --capture shot.png --capture-after 0.45 --width 1600 --height 900
```

## Usage

```powershell
$game = "C:\Riot Games\League of Legends"
# what did we find?
lolview --game-dir $game --verify

# chunk index for one archive
lolview --game-dir $game --list --archive Ahri.wad --limit 30

# resolve and decode a real asset
lolview --game-dir $game --archive Ahri.wad --lookup "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn"
lolview --game-dir $game --archive Ahri.wad --extract "data/characters/ahri/skins/skin0.bin" --out skin0.bin

# structure of a real mesh
lolview --game-dir $game --archive Ahri.wad --mesh "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn"

# fully posed and textured render: mesh + colour map + skeleton + idle clip
lolview --game-dir $game --archive Ahri.wad `
        --mesh     "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn" `
        --texture  "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base_TX_CM.tex" `
        --skeleton "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skl" `
        --anim     "ASSETS/Characters/Ahri/Skins/Base/Animations/Idle1.anm" `
        --time 2.5 --render --size 800x1000 --yaw 25 --out ahri.png

# chunk key for a path (no install needed)
lolview --hash "ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn"
```

`--extract` writes only what you ask for, to the path you name; nothing is ever
written inside the installation.

## Layout

```
include/lolview/   public headers (asset source, WAD reader, hashing, decompress)
src/core/          install resolution + hashing
src/formats/       WAD reader, payload decoding, SKN mesh parser
src/main.cpp       CLI
tests/             dependency-free test runner, synthetic fixtures only
reference/         the earlier Rust parsers, kept as a porting reference
docs/research/     notes on the formats and the replay container
docs/FORMATS.md    verified format observations
docs/LEGAL.md      the rules this project holds itself to
scripts/           maintenance helpers (not part of the build)
```

`reference/` and `docs/research/` are source material written against the
formats; they contain no game data.

## Roadmap

**End goal:** a co-op engine that can rebuild *old* League of Legends co-op modes
— Doom Bots, Star Guardian Invasion, Odyssey Extraction — from data and scripts,
on a locally installed client's assets, in a multiplayer session.

Everything is ordered by dependency. Each milestone lists what must be true
before it can start, and what "done" means.

### M0 — Asset sandbox *(done)*
Champion mesh/skeleton/animation, textures, VFX, the whole Rift with per-material
textures and alpha, jungle pets and minions, map editor with saved transforms,
scene-driven unit spawning.

### M1 — Complete animation coverage *(next)*
**Needs:** M0. **Done when:** every clip a champion ships plays.
- Parse the compressed animation container (`r3d2canm`, versions 1–3): header,
  per-joint frame records, jump-cache evaluation.
- Feed decoded tracks into the existing pose pipeline, so nothing downstream
  changes.
- Unlocks Jinx's two walk cycles, all attacks per weapon, and spells.

### M2 — Terrain-aware movement
**Needs:** M1. **Done when:** units follow the ground and cannot walk through walls.
- Sample the map's height (mapgeo triangles) for ground snapping.
- Collision against level geometry; simple slide response.
- Pathfinding: navmesh (or a coarse grid baked from the same map data) plus
  A*; straight-line smoothing.
- Agent movement: arrive / face / turn-rate, so units stop and turn believably.

### M3 — Combat core
**Needs:** M1, M2. **Done when:** two units can fight and one dies.
- Data-driven spells: cast time, cooldown, cost, range, targeting shape
  (line / circle / cone / self).
- Projectiles with travel, collision and on-hit callbacks.
- Damage packets: raw/physical/magic, armour/MR, shields, death.
- Status effects: stun, slow, knockback — the minimum for boss abilities.
- Auto-attacks: wind-up, missile, attack-move.

### M4 — Entities and AI
**Needs:** M2, M3. **Done when:** minion waves march, fight and retreat.
- Minion/camp/turret entity types, spawners, lane waypoints.
- Behaviour layer: acquire target, chase, attack, leash, retreat.
- Team logic, aggro rules, wave timing.

### M5 — Scripting runtime
**Needs:** M3. **Done when:** a mode's rules live entirely in a script.
- Embedded Lua with a small world API: `spawn`, `on_tick`, `on_cast`,
  `damage`, `set_hp`, timers, waves.
- Fixed-tick simulation (e.g. 30 Hz) so scripts are deterministic and netcode
  has a stable clock.
- Hot reload for iteration.

### M6 — Co-op networking
**Needs:** M5. **Done when:** two clients share one session.
- Host-authoritative UDP: `HELLO` / `WELCOME` / intent / snapshot.
- Clients send intents (move, cast), host simulates, ~20 Hz snapshots,
  client interpolation; reconnect and late join.
- Asset agreement check: peers must load matching content, with a clear error.
- Lobby and player slots (4–5 players, as the modes expect).

### M7 — Mode framework
**Needs:** M5, M6. **Done when:** a new mode is a script + data, not C++.
- Mode contract: waves, spawn tables, objectives, win/lose, modifiers.
- Modifier system for enemies (extra health, custom abilities, on-death
  effects) — the backbone of Doom Bots.
- Augment/upgrade system with per-player choices — needed for Star Guardian
  and Odyssey.
- Shared HUD for objectives and wave state.

### M8 — Recreate the modes
**Needs:** M7. One script package per mode, in this order of difficulty:
1. **Star Guardian Invasion** — wave defence, augments, objective phases.
2. **Odyssey Extraction** — 2-player escalation, augments, boss stages.
3. **Doom Bots** — modifier-heavy enemy scripting, custom bot abilities.

### Beyond M8
- **Tooling:** timeline/ability editor, wave editor, replay of recorded sessions.
- **Presentation:** audio (BKHD/banks are unparsed), damage numbers, better HUD.
- **Performance:** instancing for map pieces, frustum culling, fewer draw calls.
- **Robustness:** fuzz the format parsers; tolerate missing assets gracefully.

### Principles
- Engine stays generic; game content arrives through providers and scripts.
- No game assets are distributed — everything is read from the user's install.
- Any format we implement gets a reference note in `docs/FORMATS.md`.

## License

Source code: MIT (see `LICENSE`). Game assets are not covered by that license
because none are distributed.
