# What the 16.21 decompile does to "play" a replay

All addresses are RVAs in `League of Legends.exe` (image base `0x140000000`). The
names in backticks are the ones recorded in `old/decompiled/function_names.json`
and `renames.tsv`; unnamed ones are left as their `sub_XXXXXXXX` symbol.

The short version: **the replay system is not a renderer.** It is a file loader
plus a clocked cursor over a sorted array of packet records. Every packet is
dispatched into the *live game object model* through the same registry the
network path uses; the normal game renderer then draws whatever those objects
became. There is no replay-specific drawing code to port.

---

## 1. Load: file → chunks → records

```
sub_6b1b60                       replay file load
 └─ Replay_LoadChunks  0x6b2890
     └─ Replay_DecompressChunk 0x6b6040
         ├─ ZSTD_decompress     0xf05a00
         ├─ ZSTD_isError        0xf0f760
         └─ ZSTD_getErrorName   0xf096d0
```

### `sub_6b1b60` — read the container

- Slurps the file (`File_GetSize` 0x155000, `File_Read_All` 0x15c580).
- Reads 6 bytes: `"RIOT"` + `u16 format`. `format == 2` takes the modern path.
  Anything else falls into the old path (a 0x100-byte signature block, then a
  separate decompressor) which is not implemented here.
- Calls `Replay_LoadChunks(this, data + 6, end)`.

### `Replay_LoadChunks` 0x6b2890 — walk the chain

The code establishes the whole layout backwards from the end of the file:

- `u32 footer_len` at `end - 4`.
- `footer_json[footer_len]` ends at `end - 4`.
- `0x100`-byte signature sits immediately before the JSON.
- Between the 6-byte header and the signature: 8 bytes, a length-prefixed
  version string, then `0x12`-byte chunk headers.

For each chunk it builds a `0x48`-byte record and pushes it into one of two
vectors on the replay object: keyframes at `+0x18`, game chunks at `+0x8`
(`kind == 2` is a keyframe). The final chunk must end exactly on the signature;
that equality is the loader's integrity check.

Chunk header (18 bytes on 16.1x+ builds, 17 before — the extra byte is a tag):

| off | type | meaning |
|---:|---|---|
| 0 | u32 | chunk id (1..N, kind-dependent) |
| 4 | u32 | sequence number written by the recorder (not a clock) |
| 8 | u8 | kind (1, 2 = keyframe, 3, 4) |
| 9 | u32 | uncompressed size |
| 13 | u32 | compressed size (0 = stored) |
| 17 | u8 | tag, newer builds only |

### `Replay_DecompressChunk` 0x6b6040

`csize == 0` → copy `usize` bytes verbatim. Otherwise `ZSTD_decompress` into a
buffer of exactly `usize` bytes, and treat a short/long result or any
`ZSTD_isError` as fatal (it logs `"Error decompressing replay data: %s"`).

---

## 2. Chunks → packets: `Replay_ParseBlock` 0x6af080

Each chunk's decompressed payload is a run of marker-byte-prefixed blocks.
Field order matters: **time, size, id, param, payload**.

| marker bit | meaning |
|---|---|
| 7 | time is a one-byte delta in **milliseconds**; otherwise an absolute `f32` in seconds |
| 6 | keep the previous packet id; otherwise read `u16` |
| 5 | `param` is a signed one-byte delta; otherwise `u32` |
| 4 | payload length is one byte; otherwise `u32` |
| 0-3 | record kind (stored as the second byte of the record) |

`Replay_BuildRecord` 0x6b2e90 loops this parser over a chunk and allocates the
record list. It special-cases packet id `0x2D8` (728), keeping a heap copy of
that payload.

The delta scale is not a guess: with `1/1000` the last event time of every
replay equals the footer's `gameLength` to the millisecond. `1/16` overshoots
by ~70%.

---

## 3. The event array the clock walks

Once parsed, events live in a contiguous array of `0x18`-byte records:

| off | type | meaning |
|---:|---|---|
| 0x00 | f32 | time in seconds |
| 0x08 | u32 | packet param |
| 0x0C | u32 | netId (object id) |
| 0x10 | ptr | refcounted payload buffer |

The array is sorted by time.

### `Replay_SeekTime` 0x6bb2d0
Binary search for the first record whose time is `>= t` (stride `0x18`,
compares the f32 at `index * 0x18`). Updates the cursor and caches the
timestamp at `cursor+0x10`.

### `Replay_SeekCursor` 0x6ba930
Sets the cursor index (clamped to the element count) and caches that record's
time. Both this and `SeekTime` take a mutex at `0x141cd57d8`.

### `Replay_PeekEvent` 0x6abca0
Returns the record at the cursor without advancing: copies `time`, the `u64`
at `+0x08`, and the payload pointer (with a refcount bump).

### `Replay_RunEventsUntilTime` 0x69ac90
The per-frame firehose:

```
now = game_time(0x100a110) + this->clock_offset(+0x194)
seek_time(this->cursor, now)
while peek_event(cursor, &rec) && rec.time <= now:
    obj = Registry_FindObjectById(0x141c88e60, rec.netId)
    if obj: dispatch into obj's vtable / state
    seek_cursor(cursor, cursor + 1)
```

`Registry_FindObjectById` 0x531740 is a red-black-tree lookup over
`map<u32, void*>`; the object's type is at `+0x158`. Two nearby reads of the
object, `+0x15c` and `+0x164`, are used as a 2-D distance check
(`dx*dx + dz*dz < radius²`), which shows the objects carry world positions —
but those positions are produced by the packet handlers, not stored in the
replay as plain coordinates.

### `Replay_ProcessEventsLoop` 0x6a92f0 / `Replay_ProcessEventsLoop2` 0x6a98f0
Variants of the same cursor loop. `Loop` transforms floats at `event+0x28` /
`+0x2c` before dispatch. `Loop2` drives three cursors (`+0x58`, `+0x70`,
`+0x88`, `+0xa0`) and calls `Replay_RunEventsUntilTime` on two of them.

### `sub_6be560` — the controller tick

```
countdown -= delta
if countdown <= 0:
    for obj in world_objects: sub_6a8420(this, obj)
    Replay_ProcessEventsLoop2(this)
    for obj in world_objects: sub_6a8fd0(this, obj)   // interpolate / visibility
```

That is the whole "playback": a timer, the cursor dispatch, and the ordinary
game simulation. Rendering happens later in the normal frame.

---

## 4. What this means for a Rust port

Portable, and ported in `src/`:

| game function | Rust |
|---|---|
| `sub_6b1b60` + `Replay_LoadChunks` 0x6b2890 | `rofl.rs` (`Rofl::parse`, `walk_chunks`) |
| `Replay_DecompressChunk` 0x6b6040 | `rofl.rs::zstd_all` |
| `Replay_ParseBlock` 0x6af080 | `block.rs` (`BlockReader::next_block`) |
| event array + `SeekTime` / `SeekCursor` / `PeekEvent` / `RunEventsUntilTime` | `replay.rs` (`Timeline`) |
| controller tick | `main.rs::cmd_play`, `cmd_frame` |
| `Registry_FindObjectById` + object vtables | **not portable** — see below |

Not portable with the replay alone:

- **Packet payload semantics.** The payload is a bit-packed body whose field
  layout lives in the game's type registry (1306 registered types, described by
  `schema_all_types.json`), read through `Payload_ReadVarint` 0xffbe00 and
  per-type field helpers. The replay stores packet ids and raw payloads; it
  never stores "x=1234, z=5678".
- **Object model / renderer.** Models, skeletons, animations, materials,
  shaders and the scene graph are engine state and game assets. The replay path
  never touches a model file.

So: the *playback machinery* is fully reproducible (this program does it), and
the *assets* can be read and drawn (this program does that too), but "render the
replay exactly like the game" additionally requires decoding the payload
schemas and reimplementing the renderer. Neither is present in the replay.
