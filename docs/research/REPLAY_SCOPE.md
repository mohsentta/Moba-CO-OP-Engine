# Scope: the game loop and the `.rofl` readers

This is the slice that was actually rebuilt — nothing else.

## 1. "The main game function"

There is no single `main()` that reads events. Replay playback is a **game mode**
whose `Update` is installed in a vtable and called once per frame by the
engine's frame loop. The engine loop itself is generic (present, simulate,
render) and is not replay-specific.

The replay-mode update is `sub_9aa080` (RVA `0x9aa080`). It has no direct call
sites in the decompile, which is what a vtable-only entry point looks like:

```c
sub_9aa080(mode):
    param_1 += 0x58                       // replay object
    sub_6b8460(param_1)                   // sync clock
    if (sub_6b1580(param_1)) {            // active gate
        sub_6b8260(param_1)               // update path A
        sub_6bf600(param_1)               // update path B
        sub_6ac2a0(param_1, &cam)         // camera follow
        if (cam.changed) copy camera position
    }
```

`sub_6b8260` and `sub_6bf600` are near-identical per-frame updates; both end in
the replay controller. The controller is `sub_6be560` (RVA `0x6be560`):

```c
sub_6be560(replay, out_time):
    countdown = replay[+0x08] - delta
    if countdown <= 0:
        for obj in world_objects: sub_6a8420(replay, obj)
        Replay_ProcessEventsLoop2(replay)         // reads the events
        for obj in world_objects: sub_6a8fd0(replay, obj)
        replay[+0x08] = tick_interval
```

So "the function that runs the game and reads the events" is the pair
`sub_9aa080` (frame) and `sub_6be560` (tick), with
`Replay_ProcessEventsLoop2` doing the actual event walk.

### Event walk

```c
Replay_ProcessEventsLoop2(replay):
    Replay_SeekTime(replay+0x58, clock)                       // 0x6bb2d0
    Replay_PeekEvent(replay+0x58, &ev)                        // 0x6abca0
    while (ev.time < clock + replay[+0x1d8]):
        dispatch(ev); Replay_SeekCursor(cursor+1)             // 0x6ba930
    Replay_RunEventsUntilTime(replay, replay+0x70)            // 0x69ac90
    Replay_RunEventsUntilTime(replay, replay+0x88)
    ... fourth stream at replay+0xa0, clock offset +0x500 ...
```

Each record is 0x18 bytes: `f32 time`, `u32 param`, `u32 netId` at `+0x0C`,
payload pointer at `+0x10`.

## 2. Replay object layout (offsets used by the port)

| offset | meaning |
|---|---|
| `+0x08` | tick countdown |
| `+0x58`, `+0x70`, `+0x88`, `+0xa0` | event cursors (array index + cached time) |
| `+0x101` | active flag |
| `+0x194` | clock offset for `RunEventsUntilTime` |
| `+0x1d8`, `+0x1dc` | clock offset / rate for `ProcessEventsLoop2` |
| `+0x2d0` | ready flag |
| `+0x2d4` | gate timestamp |
| `+0x2d8` | last sync time |
| `+0x2dc` | last dispatched event time |
| `+0x500` | third-stream clock offset |

## 3. Every function that reads a `.rofl`

| RVA | function | reads |
|---|---|---|
| `0x6b1b60` | replay file load | whole file, `"RIOT"`, format, calls chunk loader |
| `0x6b2890` | `Replay_LoadChunks` | footer length at EOF, 256-byte signature, version string, chunk chain, 0x12-byte headers |
| `0x6b6040` | `Replay_DecompressChunk` | chunk payload (stored or zstd) |
| `0x6af080` | `Replay_ParseBlock` | block marker + fields + payload |
| `0x6b2e90` | `Replay_BuildRecord` | iterates blocks into records |
| `0x6bc750/6bc756/6bc75e` | replay constructors | allocate the replay object and call `sub_6b1b60` with the path |

Those are the only readers. Nothing else in the 122k functions touches the
file; the rest consume the parsed events.

## 4. What was rebuilt here

| game function | Rust |
|---|---|
| `sub_6b1b60` + `Replay_LoadChunks` | `rofl.rs::Rofl::parse`, `walk_chunks` |
| `Replay_DecompressChunk` | `rofl.rs::zstd_all` |
| `Replay_ParseBlock` | `block.rs::BlockReader::next_block` |
| `Replay_SeekTime` | `replay.rs::Timeline::seek_time` |
| `Replay_SeekCursor` | `replay.rs::Timeline::seek_cursor` |
| `Replay_PeekEvent` | `replay.rs::Timeline::peek` |
| `Replay_RunEventsUntilTime` | `replay.rs::Timeline::run_until` |
| `sub_6be560` controller tick | `runtime.rs::ReplayMode::update_path_b` |
| `Replay_ProcessEventsLoop2` | `runtime.rs::ReplayMode::process_events_loop2` |
| `sub_9aa080` mode update | `runtime.rs::ReplayMode::frame` |
| `sub_6b8460` sync | `runtime.rs::ReplayMode::sync` |
| `sub_6b1580` gate | `runtime.rs::ReplayMode::is_active` |

Stubbed on purpose (they need the object model, not the file):

- `sub_6a8420` per-object replay state
- `sub_6a8fd0` pairwise interpolation/vision
- `sub_6ac2a0` camera follow

## 5. Verified

`replay_player_2 run <file.rofl>` drives the ported mode update at 30 Hz and
dispatches **every record exactly once**:

| replay | events dispatched | clock at end |
|---|---|---|
| `EUW1-8020814588.rofl` | **2,340,307** | 2611.17 s |

The game walks up to four event arrays; without the packet-class split the
port uses one stream, which is why the count is exact rather than 4x.
