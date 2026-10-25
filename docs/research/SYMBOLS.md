# Replay symbols

The decompiler gives no names: functions are `sub_XXXXXX`, fields are
`param_1 + 0xNN`. This file is the naming layer we apply on top.

## Functions

| name | RVA | what it is | confidence |
|---|---|---|---|
| `Replay_Open` | `0x6b1b60` | opens the file, sniffs `RIOT`/format, forks to the container parser | high |
| `Replay_LoadChunks` | `0x6b2890` | footer/signature/preamble/version, walks the 18-byte chunk chain | high |
| `Replay_DecompressChunk` | `0x6b6040` | stored copy or one zstd frame per chunk | high |
| `Replay_ParseBlock` | `0x6af080` | marker-bit block parser (time, size, id, param) | high |
| `Replay_BuildRecord` | `0x6b2e90` | turns a chunk's blocks into a 0x48-byte record | medium |
| `Replay_SeekTime` | `0x6bb2d0` | binary search over the 0x18-byte event array | high |
| `Replay_SeekCursor` | `0x6ba930` | sets/clamps a cursor, caches the timestamp | high |
| `Replay_PeekEvent` | `0x6abca0` | reads the record at the cursor, bumps payload refcount | high |
| `Replay_RunEventsUntilTime` | `0x69ac90` | dispatches events up to the clock | high |
| `Replay_ProcessEventsLoop` | `0x6a92f0` | peek-loop variant with float/netId handling | medium |
| `Replay_ProcessEventsLoop2` | `0x6a98f0` | three-cursor event walk | medium |
| `Replay_ActiveGate` | `0x6b1580` | active/ready/sync flags, speed checks | medium |
| `Replay_Sync` | `0x6b8460` | `last_sync = clock` | high |
| `Replay_UpdateA` / `Replay_UpdateB` | `0x6b8260` / `0x6bf600` | the two update entry points that call the director tick | medium |
| `Replay_ModeUpdate` | `0x9aa080` | game-mode update: sync, gate, updates, camera follow | high |
| `Replay_CameraDirector_Tick` | `0x6be560` | countdown, event priming, visibility scoring, camera target pick | high |
| `Replay_ResetObjectAccumulators` | `0x6a8420` | per-object replay accumulator reset | medium |
| `Replay_ScoreObjectVisibility` | `0x6a8fd0` | per-object visibility/interest scoring | medium |
| `Replay_ScoreMap_FindOrInsert` | `0x275330` | RB-tree lookup/insert of per-object scores | medium |
| `Replay_VisibilityMap_Test` | `0x6a5180` | "does A see B" lookup | medium |
| `Registry_FindObjectById` | `0x531740` | netId → object | high |
| `Game_Clock_Read` | `0x100a110` | frame delta / clock read | high |
| `Game_Clock_IsPaused` | `0x100a6f0` | pause check | high |

## Replay object (base = the pointer passed to `Replay_Open`)

| offset | name | meaning |
|---|---|---|
| `+0x08` | `game_records` | vector of 0x48-byte game chunk records |
| `+0x10` | `game_count` | count for the above |
| `+0x18` | `keyframe_records` | vector of keyframe chunk records |
| `+0x20` | `keyframe_count` | count for the above |
| `+0x40` | `end_time` | time of the last game record |
| `+0x50` | `boundary_id_3` | chunk id of kind 3 |
| `+0x54` | `boundary_id_4` | chunk id of kind 4 |
| `+0x68` | `version_string` | e.g. `16.21.829.8620` |
| `+0x88` | `preamble` | 8 bytes copied from the file (also used as a cursor base later) |

Cursor structs are 0x18 bytes each; the playback path addresses them at
`+0x58`, `+0x70`, `+0x88`, `+0xa0`. Note the overlap with `+0x88` above — the
loader and the playback path disagree about that slot, which is one of the
unresolved points.

## Game object (transform / identity)

| offset | name | meaning |
|---|---|---|
| `+0x20` | `net_id` | object id used by the event walk |
| `+0x158` | `team` | byte indexing the team table |
| `+0x15c` | `pos_x` | float |
| `+0x160` | `pos_y` | float — ignored by 2D distance math |
| `+0x164` | `pos_z` | float |
| `+0x140` | vtable flag | "is a hero / can be spectated" |
| `+0x1a8` | sub-object | visibility/status component |

The director computes `dx = a.pos_x - b.pos_x`, `dz = a.pos_z - b.pos_z`,
`d2 = dx*dx + dz*dz` — height never enters.

## Camera director state

These are offsets **relative to the director's own `param_1` base**
(`float*`), not the loader's replay base — the two bases are not yet proven
equal:

| offset | name | meaning |
|---|---|---|
| `+0x04` | `countdown` | pre-game countdown / tick accumulator |
| `+0x08` | `camera_target` | current target id |
| `+0x0c` | `previous_target` | target before the switch |
| `+0x10` | `switch_timer` | decays toward `+0x234` |
| `+0x14` | `hold_timer` | decays toward `+0x238` |
| `+0x104` | `countdown_base` | reset value for `+0x04` |
| `+0x108` | `countdown_seed` | used when a switch is accepted |
| `+0x10c` / `+0x110` | `seed_a` / `seed_b` | alternate countdown seeds |
| `+0x114` | `switch_seed` | reset value for `+0x10` |
| `+0x118` | `switch_decay` | per-second decay rate |
| `+0x11c` | `hold_seed` | reset value for `+0x14` |
| `+0x120` | `hold_decay` | per-second decay rate |
| `+0x144` | `score_divisor` | divides distance before scoring |
| `+0x148` | `radius_sq` | squared visibility radius |
| `+0x1f0` | `enemy_score_mult` | multiplier when teams differ |
| `+0x22c` | `switch_cooldown` | minimum time between switches |
| `+0x230` | `min_score` | below this, revert to the previous target |
| `+0x234` / `+0x238` | `switch_floor` / `hold_floor` | timer lower bounds |
| `+0x14c` / `+0x150` | `score_same_team` / `score_enemy` | per-team factors |

## Still unnamed

- The writer of `pos_x` / `pos_z` during replay playback — the movement
  applier. Without it, positions stay wherever spawn put them.
- The `0x48`-byte chunk record → `0x18`-byte event record converter.
- The exact relationship between the loader base and the director base.

## Global symbols (renamed in the dump)

| old | new | evidence |
|---|---|---|
| `_DAT_141c89020` | `g_ReplayObject` | `sub_6b8260` tests `+0x48` for emptiness — the same 728-packet list `Replay_BuildRecord` builds at `param_1+0x48`. This is the replay singleton. |
| `_DAT_141c88fb0` | `g_ReplayObjectList` | vector of replay objects: begin at `+8`, count at `+0x10` (used by the camera director). |
| `_DAT_141c88e60` | `g_ObjectRegistry` | first argument to `Registry_FindObjectById`. |
| `_DAT_141c88e58` | `g_TeamTable` | indexed by the team byte at object `+0x158`, at `+0x20 + team*4`. |
| `_DAT_141c88eb0` | `g_GameClock` | clock context passed to `Game_Clock_Read` / `IsPaused`. |

Fields of `g_ReplayObject` seen so far: `+0x08/+0x10` game records and
count, `+0x18/+0x20` keyframes and count, `+0x40` end time, `+0x48` packet-728
list, `+0x50/+0x54` boundary ids, `+0x58..+0xa0` event cursors, `+0x68`
version string, `+0x88` preamble.

Note: several fields (`+0x58`, `+0x68`, `+0x70`) are read through rotate/xor
expressions in parts of the code. Either Ghidra mis-decoded overlapping
instructions there, or those slots hold an encoded value in some contexts.
Marked as unresolved rather than renamed.

## Payload decoding: findings (round 2)

- Block payloads are **not** raw structs. They are bit-packed packet bodies; the
  first byte varies per message (`0x25`, `0x29`, `0x2a`, `0x3f`, ...) and the
  remainder is packed.
- The packet handler registry is `g_PacketHandlers` (`0x141cd5800`), a
  `std::map<u16, Handler*>`. `sub_6aac50` shows the flow: read a varint from
  the payload, look it up, call `handler->vtable[1](buf, end)`.
- The field *writers* `sub_c2a160` / `sub_c25630` / `sub_c24b70` (for object
  `+0x15c/+0x160/+0x164`) convert the float and emit each byte through a
  **bijective byte scramble** (rotate + bit-shuffle + xor + add). The inverse
  exists and is implemented in `main.rs` (`fx_x`, `fx_y`, `inv_map`), but
  applying it to replay payloads does not yield floats — the replay stores
  values after transport decoding, so the scramble is not present here.
- A 3-byte little-endian fixed-point read at `offset/1000` produces map-range
  numbers for many messages, and `origin` finds pairs near the fountains at
  t=0 (e.g. `13564.2,13553.4` for several units), but random bytes also land
  in range, so this is not proof of the field layout.
- `pos24scan` ranks `(id, netId, offset)` series by movement consistency;
  the best smooth series still show two alternating values, i.e. the offset
  is picking two different fields across message subtypes.

## What is still missing

The **wire layout** (bit positions, field order, delta encoding) per message
id. That is generated in the game's pack/unpack code and keyed by the runtime
handler registry; it is not present in `schema_all_types.json` (which only
carries decoded struct sizes/offsets). Porting it requires either locating the
per-id unpack function or recovering the schema at runtime.
