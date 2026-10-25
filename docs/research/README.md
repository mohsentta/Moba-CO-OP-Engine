# replay player 2

A Rust rewrite of the League of Legends **replay playback path** recovered from
the 16.21 decompile, plus a reader for the game's WAD/SKN assets that loads a
champion's mesh from the local install and rasterizes it.

Two independent results:

1. **The replay decodes exactly.** All three sample replays produce an event
   count and an end time that match the footer's `gameLength` to the
   millisecond (see table below).
2. **The game's models load and render.** A real `.wad.client` is parsed, the
   zstd-compressed `.skn` is decompressed, its vertices/indices/materials are
   decoded, and the mesh is drawn — no GPU crate, no C toolchain, no network.

```
cargo build --release --offline
./replay_player_2 viewer <file.rofl> --champ=Ahri      # the app
./replay_player_2 info  <file.rofl>
./replay_player_2 run   <file.rofl>                    # ported game-mode loop
./replay_player_2 play  <file.rofl> --speed=4
./replay_player_2 model Ahri out.png --size=900x1200
./replay_player_2 frame <file.rofl> Ahri 1800 out.png --size=1100x750
```

The viewer opens a native Win32 window (no GPU crates offline): space pauses,
left/right seek 10 s, up/down change speed, Esc quits. It runs the ported
replay-mode update at 30 Hz and shows the clock, the champion mesh and the
timeline. Unit positions on the map are **not** drawn yet: they require the
bit-packed packet schemas (see [`port/REPLAY_SCOPE.md`](port/REPLAY_SCOPE.md)
and the note at the end of this file).


Assets are read from:

```
H:\Riot Games\League of Legends\Game\DATA\FINAL
```

Override with `--assets=<dir>`. Nothing is copied out of the install and no
asset bytes are committed here.

---

## Verified against the real files

| replay | format | chunks | events | last event | footer `gameLength` |
|---|---|---|---|---|---|
| `EUW1-7996988963.rofl` | 16.19.821.7343 | 80 (26 KF) | 1,396,994 | 1558.75 s | 1558.746 s |
| `EUW1-8000271609.rofl` | 16.19.823.0722 | 79 (26 KF) | 1,308,035 | 1513.43 s | 1513.425 s |
| `EUW1-8020814588.rofl` | 16.21.829.8620 | 134 (44 KF) | 2,340,307 | 2611.16 s | 2611.164 s |

The 2,340,307 figure also matches the event count previously established for
that match. Both chunk-header widths (17-byte pre-16.1x and 18-byte 16.1x+)
are handled; the loader picks whichever walk lands exactly on the 256-byte
signature, which is the same invariant the game uses.

Rendered samples are in `samples/`:

| file | what it is |
|---|---|
| `ahri_base.png` | Ahri base mesh, SKN v4.1 |
| `jinx_base.png` | Jinx base mesh, SKN v2.1 |
| `yasuo_base.png` | Yasuo base mesh, SKN v2.1 |
| `lux_skin15.png` | Lux mesh found by archive scan (path fallback) |
| `ahri_t1800.png` | replay frame at t = 1800 s, with the playback bar |

![Ahri base](samples/ahri_base.png)

---

## The decompile, in one paragraph

`sub_6b1b60` reads `"RIOT"`, walks the chunk chain (`Replay_LoadChunks`
0x6b2890), decompresses each chunk with zstd (`Replay_DecompressChunk`
0x6b6040), and parses marker-byte blocks (`Replay_ParseBlock` 0x6af080) into a
time-sorted array of 0x18-byte records `{f32 time, u32 param, u32 netId,
payload*}`. Each frame, `sub_6be560` advances a countdown, runs
`Replay_ProcessEventsLoop2` 0x6a98f0 → `Replay_SeekTime` 0x6bb2d0 /
`Replay_PeekEvent` 0x6abca0 / `Replay_SeekCursor` 0x6ba930, and
`Replay_RunEventsUntilTime` 0x69ac90 looks up each record's netId with
`Registry_FindObjectById` 0x531740 and feeds it to the live game object. The
game then renders normally. Full detail, including the field orders and the
timing constant, is in [`DECOMPILE.md`](DECOMPILE.md).

The crucial timing fact: relative block times are **milliseconds**
(`1/1000`), absolute times are `f32` seconds. That is what makes the event
timeline land exactly on `gameLength`; `1/16` overshoots by ~70%.

---

## Asset pipeline

`src/wad.rs` — shipping WAD v3:

- `"RW"`, version, a 256-byte RSA signature block, then a table of 32-byte
  entries: `u64 xxh64(path)`, `u32 offset`, `u32 compressed_size`,
  `u32 uncompressed_size`, `u8 compression`, `u8 duplicate`, `u16 pad`,
  `u64 checksum`.
- The TOC start varies by archive (0x110 in every archive tested: champion
  WADs, DATA, and Map11), so it is *found*, not assumed: walk 32-byte entries
  while offsets point past the entry and data stays in bounds, then keep only
  the prefix for which the TOC still ends before the first chunk's data.
  Trailing false entries disappear under that invariant. `wadprobe --verify`
  additionally checks entries against their XXH3 checksum (the shipping
  algorithm; older archives use a SHA-256 prefix, which the reader does not
  depend on).
- Chunk identity is `xxh64(ascii-lowercased path)` — there are no names in the
  archive. Compression codes seen: `0` (stored), `3`, `0x24`, `0x34`; all
  compressed ones are zstd frames, detected by magic `28 b5 2f fd`. A small
  gzip/inflate path exists for older WADs (pure Rust, no zlib dependency).
- The archive is memory-mapped by reading it whole, so the multi-gigabyte map
  WADs (Map11 is 2.5 GB) open but are impractical; a streaming reader is the
  obvious next step. Champion WADs (50–200 MB) are fine.

`src/skn.rs` — `.skn` meshes, both layouts in the shipping game:

- v4 (Ahri, 4.1): header, materials (64-byte name + four `u32` ranges), then a
  60-byte block (`index_count`, `vertex_count`, `stride = 52`, bbox), indices,
  then vertices.
- v1/v2/v3 (Jinx, Yasuo, 2.1): header, materials, counts, indices, vertices.

Vertex record (52 bytes): `f32 position[3]`, `u8 bone_indices[4]`,
`f32 weights[4]`, `f32 normal[3]`, `f32 uv[2]`. Positions are already in bind
pose, so a static render needs no skeleton. The parser validates the counts
against the remaining file size and rejects a layout that doesn't close.

`src/render.rs` — a small z-buffered software rasterizer: orbit camera,
perspective projection, smooth normals, one key light, checkerboard floor, 2×
supersampling, and a BMP/PNG writer (PNG uses stored deflate blocks, so the
encoder is dependency-free).

---

## Is it possible?

**The playback: yes, and it's done.** Container, chunking, zstd, block parsing,
the sorted event array, seek/peek/run semantics and the clock-driven dispatch
all reproduce the decompile and agree with the files to the millisecond.

**The models: yes, and it's done for the mesh.** WAD → SKN → pixels works for
v2 and v4 meshes across several champions.

**Rendering "the same way the game does": no, not from the replay alone.** The
replay does not contain models, terrain, materials, shaders, camera state, or
world positions as plain values. It contains packet ids and bit-packed payloads,
which the game feeds into the live object model; the normal renderer draws the
result. To match the game frame-for-frame you would additionally need:

1. **Payload schemas.** Field layouts live in the type registry (1306 types,
   `schema_all_types.json`) and are read through `Payload_ReadVarint` and
   per-type handlers. Decoding the movement/appearance packets is the single
   blocker for positions, health bars, animations and champion identity.
2. **A renderer and material system.** Shaders, lighting, fog, map terrain
   (`Map11.wad.client` is 2.5 GB), particles.
3. **Skeletons and animations** (`.skl` / `.anm`) if champions should move.

That is a bounded but large second project. What exists here is the honest
foundation: the replay is fully readable, and the game's own meshes can be
loaded and drawn from the install.

---

## Layout

```
Cargo.toml
src/main.rs        CLI: info / play / blocks / model / frame
src/rofl.rs        ROFL2 container, chunk walk, zstd
src/block.rs       Replay_ParseBlock port (marker bits, field order)
src/replay.rs      Timeline: SeekTime / SeekCursor / PeekEvent / RunEventsUntilTime
src/wad.rs         WAD v3 reader (TOC discovery, xxh64/xxh3, zstd, inflate)
src/skn.rs         SKN v2/v4 mesh reader
src/render.rs      software rasterizer + BMP/PNG writer
src/bin/wadprobe.rs  diagnostic: TOC discovery, path lookup, extract, scan
src/bin/portgen.rs   bulk extractor: all 122,572 functions -> TSV + Rust scaffold
DECOMPILE.md       function-by-function map of the playback path
port/REPLAY_SCOPE.md  the game-loop slice + every .rofl reader
port/INVENTORY.md     all 122,572 decompiled functions, and why 1:1 is out of reach
port/functions.tsv    one row per function: rva, name, file, signature, metrics
port/stubs/           generated Rust module per decompiled .c (122,572 fns)
samples/           rendered output
```

Only two crates are used, both pure Rust and available offline: `ruzstd`
(zstd) and `twox-hash` (xxh64/xxh3).
