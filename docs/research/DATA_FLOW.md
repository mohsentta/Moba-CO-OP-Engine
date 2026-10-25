# From unreadable bytes to usable data: the full path we can prove

This is the end-to-end trace of a `.rofl` file through the game, hop by hop,
with what is verified and what is still unknown. Every hop names the function
and the RVA so it can be checked.

---

## Hop 1 — file → memory (verified)

`Replay_Open` (`0x6b1b60`)

- reads the whole file into one buffer,
- checks `"RIOT"` + `u16 format`,
- `format == 2` → `Replay_LoadChunks`, else the legacy inline parser.

Output: an unparsed byte buffer plus a format decision.

## Hop 2 — container → chunk list (verified)

`Replay_LoadChunks` (`0x6b2890`)

- footer length from `EOF-4`, JSON footer, 256-byte signature before it,
- signature verified over the chunk region; mismatch aborts,
- 8-byte preamble (`u64`), length-prefixed version string,
- loops 18-byte chunk headers: `id, time, kind, usize, csize, tag`,
- per chunk: `Replay_DecompressChunk` (stored copy or one zstd frame),
- per chunk: `Replay_BuildRecord`, then append a **0x48-byte record** to
  either the keyframe vector (`+0x18`) or the game vector (`+0x08`).

Output: two vectors of 0x48-byte records, plus counts and timestamps.

## Hop 3 — chunk payload → blocks (verified)

`Replay_BuildRecord` (`0x6b2e90`)

For each block in the chunk:

```
Replay_ParseBlock(record, &id, &time, &size, &param)
if first block: record[+0x20] = time        // chunk start time
record[+0x24] = max(record[+0x24], time)    // running max
```

`Replay_ParseBlock` (`0x6af080`) is the marker-bit decoder:

```
marker byte
  bit7: 0 -> u8 ms delta      1 -> f32 seconds
  bit6: 0 -> u16 id           1 -> keep previous id
  bit5: 0 -> i8 param delta   1 -> u32 param
  bit4: 0 -> u16 size         1 -> u32 size
```

It produces `(time, id, param, payload[0..size])` per block. This is the first
point where the file becomes **structured**: id + time + bytes.

## Hop 4 — one special packet is extracted (verified, and surprising)

Inside `Replay_BuildRecord`, when the replay's `+0x48` list is empty, it reads
a varint from the block payload and checks it against **`0x2d8` (728)**:

```
if replay[+0x48] == 0:
    Payload_ReadVarint(payload, &v)
    if v == 0x2d8:
        node = alloc(0x20)
        node.time = block.time
        node.kind = ...
        node.data = copy(payload, block.size)
        push(replay[+0x48], node)
```

So packet **728** gets its payload copied into a separate list during load.
This matches the earlier scan result: the only 728 packet in the file is a
single ~16 KB snapshot. Everything else stays inside the chunk records.

## Hop 5 — records sit in memory (verified), and then... (unknown)

At this point the verified chain stops. The two vectors hold 0x48-byte
records. But the playback machinery reads **0x18-byte events**:

`Replay_PeekEvent` (`0x6abca0`) shows the event layout and the cursor:

```
cursor:  +0x00 u32 index
         +0x08 pointer to a vector object
vector:  +0x08 begin, +0x10 end, element stride 0x18
event:   +0x00 f32/u32 time
         +0x08 u32
         +0x0C u32  netId
         +0x10 pointer payload (refcount at payload+8)
```

`Replay_SeekCursor` (`0x6ba930`) caches `event.time` at `cursor[4]`;
`Replay_SeekTime` (`0x6bb2d0`) binary-searches the same 0x18 array.

**So a conversion from 0x48 records to 0x18 events must exist.** Targeted
searches failed to find it:

- it does not use the known vector helpers (`sub_6ab1a0` / `sub_6a16a0`);
  those are only called by the loaders,
- the two vectors are only appended to by the loaders,
- offset scans for `* 0x18` stride hit only the readers (Seek/Peek).

Conclusion: the converter is either inlined into a caller, or the 0x18 arrays
are built by a different container type that doesn't use those helpers. This
is gap #1.

## Hop 6 — events → objects (verified in outline)

`Replay_RunEventsUntilTime` (`0x69ac90`) and `Replay_ProcessEventsLoop2`
(`0x6a98f0`):

- read events up to the clock,
- `Registry_FindObjectById(netId)` (`0x531740`) to get the object,
- call type predicates on it (`sub_2dfbc0` / `sub_2dfc40`: compare a type id),
- use per-object maps/strings (e.g. `sub_2036b0` string assign, `sub_275330`
  score map).

This is where a netId becomes an object reference. What it does not do — in
the code we traced — is write a position.

## Hop 7 — positions (read side verified, write side open)

`Replay_CameraDirector_Tick` (`0x6be560`) reads:

```c
dx = *(float*)(a + 0x15c) - *(float*)(b + 0x15c);
dz = *(float*)(a + 0x164) - *(float*)(b + 0x164);
d2 = dx*dx + dz*dz;      // y (+0x160) ignored
```

So by tick time, the position floats **already exist** on the object. They are
maintained by something else. We found the outbound side but not the inbound:

- `sub_cebcb0` (`0xcebcb0`) is a **serializer**: it reads `+0x15c/+0x160/
  +0x164` and writes them to a packet buffer with a change-bitmask, one helper
  per field (`sub_c2a160`, `sub_c25630`, `sub_c24b70`).
- Direct assignments to `+0x15c`: only `sub_1465f80` (`0x1465f80`, case
  `0x18` of a field-id switch) — but `+0x160` is a byte there, so it is a
  different struct layout.
- 22 functions copy into `+0x15c`/`+0x164` from another buffer/object
  (`sub_272e87`, `sub_32210a`, the `sub_156c2e0` family). None confirmed as
  the packet applier.

This is gap #2, and it is the one that matters: without the writer, nothing
moves.

## Dead ends (checked, documented so we don't repeat them)

- **`_DAT_141c89020`** looked like the replay singleton for a moment, but its
  fields are read through rotate/xor expressions (`>> 0x32 | << 0xe ^
  0x80104765`), i.e. obfuscated pointers, and it is used by anti-tamper code.
  It is not a plain replay object base.
- **Registration table at file offset `0x1f35ed0`** holding `0xcebcb0` as a
  u32 RVA: entries look like `{name RVA, fn RVA, ...}`, but the name strings
  at `0x1bb4cc8` etc. are encrypted at rest (high entropy). Same `.rdata`
  wall as the rest of the string table.
- **`sub_6b5420`** (`0x6b5420`) writes `+0x15c/+0x160/+0x164` — but it is a
  **settings initializer** that fills the camera director's config fields
  (radius, seeds, team factors) from named settings; the offset overlap is
  coincidental.

## What the wire format looks like (from the serializer)

The serializer reveals the position encoding order even without the setter:

```
mask byte
  for each set bit, in fixed order:
      if value != 0: write f32 value
```

So replay payloads for a position update are **mask + conditional floats**,
not a fixed `x,y,z` triplet. That is why the earlier raw float scans over the
whole file found nothing usable — the values are conditional and packed
between other fields.

## The two gaps, in priority order

1. **Who writes `+0x15c/+0x164` from a decoded packet?** Find the vtable slot
   next to `sub_cebcb0` (the deserializer mirror). That is the position
   applier and it unlocks real movement.
2. **0x48 records → 0x18 events.** Needed to know which payload belongs to
   which netId at which time — though the events themselves already carry
   netId, so if the applier is found first, this may become optional.

## Summary picture

```
.rofl bytes
   │  Replay_Open            RIOT + format
   ▼
chunk region
   │  Replay_LoadChunks      footer, signature, preamble, version
   ▼
18-byte chunk headers
   │  Replay_DecompressChunk  stored / zstd
   ▼
block stream
   │  Replay_BuildRecord → Replay_ParseBlock   (time, id, param, payload)
   ├── packet 728 snapshot → replay+0x48 list
   ▼
0x48-byte records  (game vector +0x08, keyframe vector +0x18)
   │  ???  ← gap #1
   ▼
0x18-byte events  (cursor +0x58/+0x70/+0x88/+0xa0)
   │  SeekTime / PeekEvent / RunEventsUntilTime
   ▼
netId → object  (Registry_FindObjectById)
   │  ???  ← gap #2 (packet field applier / position writer)
   ▼
object fields: +0x15c x, +0x160 y, +0x164 z, +0x158 team
   │
   ▼
camera director reads distance, picks target, renders
```

The file becomes *structured* at hop 3 (blocks) and *identified* at hop 6
(netId → object). The two `???` are where structure becomes game state. Both
are behind virtual dispatch, which is why static call-graph work keeps
stopping there.
