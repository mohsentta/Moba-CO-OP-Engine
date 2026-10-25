# Where positions are (and where they are not)

## Answer

The replay does **not** store unit positions as readable coordinates. The block
payloads are a **bit-packed serialization** whose field layout lives in the
game's type registry, and several fields are additionally **byte-obfuscated**
with a fixed substitution table. We recovered the obfuscation and confirmed it
by decoding netIds; the positions themselves need the per-type bit layouts,
which are not present in the replay.

In practice League derives movement from commands (target + speed) and
simulates positions in the object model. The object does carry a position at
`+0x15c/+0x160/+0x164` (used by `sub_6be560` for distance checks), but that
value is produced by the engine, not serialized as three floats.

## What was proven

### 1. Payload fields are byte-obfuscated

`Replay_RunEventsUntilTime` (0x69ac90) decodes a 4-byte field in place before
`Registry_FindObjectById`:

```c
x   = b ^ 0x5f;
s   = (x << 1) & 0xff;
idx = (s >> 4) | (((b >> 7) | s) << 4);
out = ror5(TABLE[idx]) ^ 0xd4;          // TABLE at RVA 0x18badf0
```

The 256-byte table was extracted from `.rdata` (file offset `0x18badf0`) and
is committed as `src/subst.bin`. `src/deobf.rs` implements the inverse.

Proof: after applying it, `0x4000_xxxx` netIds appear in the payloads, e.g.
packet 822 at decoded offsets 7/19/41, packet 28 at +158, packet 115 at +3.
Before the transform those 4-byte fields look like noise.

### 2. Fields are bit-packed and unaligned

Decoded netIds land at **odd** offsets (7, 19, 41, 158), and payload bytes look
like packed nibbles (`c3 c3 c3`, `cd`, `cf`) rather than structs. That matches
the registry-driven reader (`Payload_ReadVarint` 0xffbe00 + per-field helpers),
not a C struct.

### 3. No coordinate encoding survives a brute-force search

Searched all 2,340,307 events for:

| candidate encoding | result |
|---|---|
| `f32[3]` at every byte offset, map corner `[0,15000]` | none (only constant grids) |
| `f32[3]` at every byte offset, centered `[-8000,8000]` | none |
| same, after de-obfuscating the payload | none |
| `u16[3] × 0.25` / `i16[3]` at every offset | false positives only |
| per-entity smoothness scoring (delta > 0.5 and < 600 units) | only constants/repeats |

The best "smooth" candidates were examined by hand:

- packet **522**, offset 1, `u16×0.25`: 92% smooth, but the values are
  constant per match (`13095.8 / 11115.2 / 6857.5`) and `param == 0`. Not units.
- packet **1031** (18 bytes, 89k events): no decoded netId and no plausible
  float triple at any offset; likely a packed status/attribute record.
- packet **360**: 1,263-byte payload, literally `0x44` repeated (`f32 785.06665`)
  — a grid/visibility array.
- packet **1022**: 6 bytes, identical at t=0 and t=60 — static per-entity data.

## What would unlock positions

1. **Type registry decode.** `schema_all_types.json` has 1,306 types with field
   hashes and offsets; the field helper functions define the value encoding.
   Missing link: packet id → type hash. Recovering that table and the bit widths
   is the next reverse-engineering step.
2. **Movement command reconstruction.** If movement is command-based, decoding
   the move packet gives a destination + speed; positions then come from
   simulating movement along the path, not from reading them.

Both are bounded RE tasks, but neither is a field extraction that can be done
from the file alone. That is why the viewer currently has a clock, events and
models, but no units on the map.

## Diagnostics added for this work

| command | purpose |
|---|---|
| `descan <rofl>` | de-obfuscate every payload; list netId fields and float triples |
| `findpos <rofl>` | score every id/offset/format for per-entity smooth motion |
| `trace <rofl> <id> <off> <scale>` | print decoded triples over time |
| `probe <rofl> <id>` | raw + de-obfuscated hex, netId/position offsets per event |
| `idstat <rofl>` | packet id frequency and average payload size |
