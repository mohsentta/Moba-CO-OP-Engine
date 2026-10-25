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

| Milestone | Scope | State |
|---|---|---|
| M0 | install resolution, WAD index, chunk hashing | done |
| M1 | zstd/gzip payload decoding, `--extract`, path discovery | done |
| M2 | SKN mesh parsing (`--mesh`) | done |
| M2b | OpenGL 4.6 renderer, offscreen PNG output (`--render`) | done |
| M4 | TEX/BCn textures and materials | done |
| M3 | SKL skeleton + ANM animation playback with GPU skinning | done |
| M5 | particle system + ability effects (`--effect q|w|e|r`) | done |
| M6 | interactive practice sandbox (`lolview_sandbox`) | done |

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
| `F2` | editor mode: free-fly camera (WASD, Space/Ctrl, Shift boost, right-drag look) plus the editor window |
| `ESC` | quit |

The HUD draws the real ability icons, cooldown timers, champion health/mana,
the target dummy's health bar and the champion's selection ring. Casting locks
the spell clip until it finishes, then returns to movement/idle.

Camera feel can be tuned without a rebuild: `--pitch 40 --distance 880`.

### Map editor

With `--map`, the whole Rift is loaded as 586 individually editable pieces
(textured from the map archive, cached on the GPU). The "Map editor" window lets
you:

- pick a piece from the searchable list, or click it in the world
- move it (`move`), rotate it (`yaw`) and scale it (`scale`) — rotation and
  scaling pivot around the piece's own centre
- hide pieces, reset one piece or everything
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

## License

Source code: MIT (see `LICENSE`). Game assets are not covered by that license
because none are distributed.
