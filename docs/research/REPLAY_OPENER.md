# What happens to a replay file when the game opens it

This is the story of one `.rofl` file, from a path string to two in-memory
arrays of chunk records. Everything below is the behaviour of the 16.21
binary: `sub_6b1b60` (file load) and `Replay_LoadChunks` (0x6b2890).

The short version: the game reads the whole file into memory, checks it is a
RIOT replay, looks at a 16-bit format number, and then takes one of two very
different paths. Modern replays (format 2) delegate to a chunk walker that
reads backwards from the end of the file, verifies a 256-byte signature,
reads a version string, and then walks a chain of 18-byte chunk headers,
decompressing each payload and turning it into records. Old replays take an
inline legacy path with a different header, a zlib-inflated block and an
index table. At the end, the game holds keyframes in one vector and game
chunks in another, plus a handful of timestamps.

---

## Stage 0 — the file becomes memory

`sub_6b1b60(replay, path)` sets the replay state to `6` ("loading"), opens the
path, asks the OS for its size, allocates a buffer of exactly that size, reads
the whole thing in one go, and moves the state to `7` ("loaded, parsing").
There is no streaming and no memory mapping: the entire file lives in RAM from
this moment on.

Every failure here is the same shape — log an error and stop:

```
if open(path) fails            -> error, state stays 6
if size == 0                   -> error
if read(size) != size          -> error
if size < 6                    -> error "too small"
```

Then it copies the first six bytes into a small local struct and looks at them.

---

## Stage 1 — the six-byte header decides everything

```
bytes 0..4  "RIOT"
bytes 4..6  u16 format
```

The magic is checked first. If it is not `RIOT`, the file is rejected
immediately. The format word is the fork in the road:

```
if format == 2:      Replay_LoadChunks(replay, file + 6, end)
else:                legacy path (inlined, below)
```

From here on, `param_2` is the first byte after the six-byte header, and
`param_3` is the end of the file.

---

## Stage 2A — modern replays (format 2)

### 2A.1 Read the footer backwards

The modern container is written from both ends towards the middle. The
loader starts at the end:

```
if (end - data) < 4:                    error "too small"
footer_len = u32 at (end - 4)
if footer_len > (end - data):           error
footer_start = end - 4 - footer_len
if (footer_start - data) < 0x100:       error
sig_start = footer_start - 0x100
```

So the last four bytes are a length, the bytes before them are a JSON footer
(match id, game length, keyframe ids, player stats), and immediately before
the footer sit exactly 256 bytes of signature.

### 2A.2 Verify the signature

The game hashes the region from `data` up to `sig_start` — that is, every
chunk, but not the signature or the footer — using a keyed hash object
(`sub_15b3b60` to create it, `sub_15b0840` to absorb the data, `sub_15b61a0`
to compare against the 256-byte block). The result is one boolean:

```
if signature does not match:
    log "replay signature invalid" with the hash error text
    abort  (nothing is parsed)
```

This is the integrity gate. A truncated or modified chunk makes the hash
differ and the whole replay is refused.

### 2A.3 The preamble and the version string

With the signature accepted, the loader reads forward from `data`:

```
copy 8 bytes from data            -> replay[+0x88]      // u64: file time
if sig_start != data + 8:
    len = u8 at data+8
    copy len bytes at data+9      -> replay[+0x68]      // version string, e.g. "16.21.829.8620"
    cursor = data + 9 + len
else:
    cursor = data + 8                                   // no string at all
```

Two branches, because a replay with no version string is legal — older
writers omitted it. The version string is what the client shows and what
tools use to pick a parser.

### 2A.4 Walk the chunk chain

Now the actual payload. The remainder of the file is a flat sequence of
chunks, each with an 18-byte header:

```
u32 id
u32 time
u8  kind            (2 = keyframe, 3/4 = boundary markers, otherwise game data)
u32 uncompressed_size
u32 compressed_size (0 = stored verbatim)
u8  tag
```

The loop is a single cursor with a size check before every read:

```
while cursor + 0x11 < sig_start:
    if (sig_start - cursor) < 0x12:
        error "truncated chunk header"     // and stop
    header = read 18 bytes at cursor
    cursor += 18
    if cursor >= sig_start: break          // header was the last thing
    ... build a 0x48-byte chunk record ...
```

The record carries the header fields plus a flag (`param = 1`) and scratch
fields used by the decompressor. Before decompressing, the loader updates a
range map keyed by chunk id:

```
range = map_lookup(replay[+0x58], id, kind)
if kind == 2:                       // keyframe
    range.min = min(range.min, id)
    range.max = max(range.max, id)
else:
    range.max = max(range.max, id)
```

That map is what later answers "which chunk covers time T" when seeking.

Then the branch that matters: keyframe or not.

```
if kind == 2:                                            // KEYFRAME
    ok = Replay_DecompressChunk(replay, record, &cursor, sig_start)
    if not ok: error "keyframe decompression failed"; free; stop
    Replay_BuildRecord(replay, record)                   // blocks -> event records
    grow vector replay[+0x18]; append record; replay[+0x20]++
else:                                                    // GAME DATA
    ok = Replay_DecompressChunk(replay, record, &cursor, sig_start)
    if not ok: error "chunk decompression failed"; free; stop
    if kind == 3: replay[+0x50] = id                     // boundary timestamps
    if kind == 4: replay[+0x54] = id
    Replay_BuildRecord(replay, record)
    grow vector replay[+0x8]; append record; replay[+0x10]++
```

`Replay_DecompressChunk` has its own two-way branch:

```
if compressed_size == 0:
    copy uncompressed_size bytes verbatim
else:
    ZSTD_decompress(compressed_size -> uncompressed_size)
    if ZSTD_isError or output size mismatch: fail
```

So each chunk is either stored or one zstd frame. A failure at any chunk
frees the current record, logs which side failed (keyframe or game chunk),
and leaves the replay partially loaded — the arrays keep whatever was
appended before the bad chunk.

The loop ends in one of three ways: the cursor reaches the signature, a
header is too short, or a payload decompression fails.

### 2A.5 Post-processing timestamps

After the loop, the loader derives the replay's end time from the last
record rather than from the footer:

```
if game_chunk_count != 0:
    last = game_vector[game_chunk_count-1]
    replay[+0x40] = last.time
```

The same idea is applied to the keyframe vector on the path just above. This
is why a replay whose footer says one thing and whose chunks say another
will still play to the chunk-derived end.

---

## Stage 2B — old replays (format != 2)

The `else` branch is the pre-2.0 container and is written inline, not as a
separate function. Its shape is:

```
copy 0x100-byte signature at data+6
create an inflate stream (window 0x126) over the rest
inflate a small block (0x14 bytes) and verify it
read 0x1a bytes at data+0x120: a small header, including a length
read that length; copy the version string
build a version object (0x1058 bytes)
read a 0x42-byte block:
    two 32-bit counts
    one time value scaled by 1/16
    a length byte, then that many bytes
build a 0x11-byte-per-entry chunk table from the counts
allocate two 1 MB buffers
for each table entry:
    read the 0x48-byte record, decompress, build blocks
```

Same destination, different packaging: an indexed table instead of a chain,
and zlib instead of zstd. None of the 16.x replays take this branch; it is
there for backwards compatibility.

---

## Epilogue — what the replay object contains

When the opener returns successfully, the object holds:

| field | meaning |
|---|---|
| `+0x08` | vector of game chunk records, `+0x10` count |
| `+0x18` | vector of keyframe chunk records, `+0x20` count |
| `+0x40` | end time derived from the last game chunk |
| `+0x50`, `+0x54` | boundary chunk ids (kinds 3 and 4) |
| `+0x58` | min/max chunk id ranges |
| `+0x68` | version string |
| `+0x88` | 8-byte preamble (file time) |

Each 0x48-byte chunk record holds the header fields and its decompressed
payload. `Replay_BuildRecord` has already parsed that payload into blocks;
`Replay_LoadChunks` itself never looks inside the bit stream.

Later, the playback controller (`sub_6be560`) sorts/uses those records:
`Replay_SeekTime` binary-searches them, `Replay_PeekEvent` reads one, and
`Replay_RunEventsUntilTime` dispatches every event up to the current clock.

---

## Branch table

| check | failure behaviour |
|---|---|
| magic != RIOT | reject |
| size < 6 / < 4 | error, no parse |
| format not 2 | legacy parser |
| footer_len > size | error |
| space before footer < 0x100 | error |
| signature mismatch | log + abort |
| space for preamble < 8 | error |
| string length runs past end | error |
| chunk header truncated (< 0x12) | error, stop |
| keyframe decompress fails | error, free, stop |
| game chunk decompress fails | error, free, stop |
| stored chunk size mismatch | error via decompressor |
| zstd error / size mismatch | error via decompressor |

The key architectural point: **the opener only unpacks and indexes**. It
verifies integrity, splits the file into keyframes and game chunks, and turns
each chunk's payload into records. It never interprets a packet, never moves
an object, and never draws anything — that starts when the playback
controller walks the records it left behind.

---

## Who reads these fields (traced)

The opener writes; something else reads. What the decompile actually shows:

| field | reader | status |
|---|---|---|
| `+0x08` / `+0x18` record vectors | playback cursors (`Replay_ProcessEventsLoop2` → `SeekTime` / `PeekEvent` / `RunEventsUntilTime`) | **real, but the 0x48→0x18 converter is not identified** |
| `+0x40` end time | controller `sub_6be560` **writes** 0 during a reset; no reader inside the replay module | consumer is outside (UI/timeline), inferred |
| `+0x50` / `+0x54` boundary ids | no reader found | unknown |
| `+0x58` ranges | written by the loader, later treated as a cursor base by the event walk | ambiguous; the same offset serves both during load and playback |
| `+0x68` version string | no read found in the game module | client/tool metadata |
| `+0x88` 8-byte preamble | `Replay_RunEventsUntilTime(param_1, param_1 + 0x88)` uses it as a cursor | overlaps the cursor storage; exact handoff unclear |

The important gap: the opener appends **0x48-byte chunk records**, while seek/peek operate on **0x18-byte event records**. The builder that converts one into the other has not been located. That is the dotted edge in the tree, and it is the same missing link that blocks packet-level decoding.
