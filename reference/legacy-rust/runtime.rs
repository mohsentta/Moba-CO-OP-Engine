//! The replay slice of the game loop, ported function-for-function.
//!
//! Decompile trail (RVAs):
//!
//! ```text
//! sub_9aa080   0x9aa080  replay-mode Update (called through the game-mode vtable)
//!   sub_6b8460 0x6b8460  sync: last_sync = clock
//!   sub_6b1580 0x6b1580  is_active gate (flags + speed + timers)
//!   sub_6b8260 0x6b8260  update path A
//!   sub_6bf600 0x6bf600  update path B
//!     sub_6be560 0x6be560  replay controller tick
//!       sub_6a8420 0x6a8420  per-object replay state
//!       Replay_ProcessEventsLoop2 0x6a98f0
//!         Replay_SeekTime        0x6bb2d0
//!         Replay_PeekEvent       0x6abca0
//!         Replay_SeekCursor      0x6ba930
//!         Replay_RunEventsUntilTime 0x69ac90
//!       sub_6a8fd0 0x6a8fd0  per-object pair interpolation / vision
//!   sub_6ac2a0            camera follow: obj[+0x15c/+0x160/+0x164] -> cam[+0x240..]
//! ```
//!
//! The object model itself is not ported here; the event consumer only counts
//! and routes records by netId. Everything that touches the `.rofl` file is
//! ported exactly (see `rofl.rs`, `block.rs`, `replay.rs`).

use crate::replay::Timeline;
use crate::rofl::Rofl;

/// One of the replay's event cursors (`+0x58`, `+0x70`, `+0x88`, `+0xa0`).
#[derive(Clone, Copy, Debug)]
pub struct Cursor {
    pub index: usize,
    pub cached_time: f32,
    /// Clock offset applied to this stream (`+0x194`, `+0x1d8`, `+0x500`).
    pub clock_offset: f32,
}

impl Cursor {
    fn new(clock_offset: f32) -> Cursor {
        Cursor {
            index: 0,
            cached_time: 0.0,
            clock_offset,
        }
    }
}

/// The replay object at `+0x58` inside the game's mode object.
pub struct ReplayMode {
    pub timeline: Timeline,
    /// +0x58, +0x70, +0x88, +0xa0
    pub cursors: [Cursor; 4],
    /// Game clock in seconds, updated by the engine (`sub_100a110`).
    pub clock: f32,
    /// +0x08: countdown until the next controller tick.
    pub countdown: f32,
    /// +0x101: replay mode active.
    pub active: bool,
    /// +0x2d0: loaded / ready.
    pub ready: bool,
    /// +0x2d4: earliest clock at which the gate may open.
    pub gate_time: f32,
    /// +0x2d8: last sync timestamp (`sub_6b8460`).
    pub last_sync: f32,
    /// +0x2dc: last dispatched event time.
    pub last_event_time: f32,
    /// Telemetry: how many records were handed to the (absent) object model.
    pub dispatched: u64,
    /// Controller tick interval in seconds; the game stores it at +0x218/+0x21c.
    pub tick_interval: f32,
}

impl ReplayMode {
    pub fn new(r: &Rofl) -> ReplayMode {
        ReplayMode {
            timeline: Timeline::from_rofl(r),
            cursors: [
                Cursor::new(0.0),
                Cursor::new(0.0),
                Cursor::new(0.0),
                Cursor::new(0.0),
            ],
            clock: 0.0,
            countdown: 0.0,
            active: false,
            ready: true,
            gate_time: 0.0,
            last_sync: 0.0,
            last_event_time: 0.0,
            dispatched: 0,
            tick_interval: 1.0 / 30.0,
        }
    }

    /// `sub_9aa080`: one replay-mode frame.
    pub fn frame(&mut self, dt: f32) {
        self.clock += dt;
        self.sync(); // sub_6b8460
        if !self.is_active() {
            return;
        }
        self.update_path_b(dt); // sub_6bf600 -> sub_6be560
    }

    /// `sub_6b8460`
    pub fn sync(&mut self) {
        self.last_sync = self.clock;
    }

    /// `sub_6b1580`: the gate. Simplified to the flags and timer the decompile
    /// exposes; the remaining conditions are anti-cheat/observer checks that
    /// have no bearing on playback.
    pub fn is_active(&self) -> bool {
        self.active && self.ready && self.clock >= self.gate_time
    }

    /// `sub_6be560`: controller tick. The game decrements a countdown and only
    /// does work when it reaches zero.
    pub fn update_path_b(&mut self, dt: f32) {
        self.countdown -= dt;
        if self.countdown > 0.0 {
            return;
        }
        self.countdown += self.tick_interval;

        // `for obj in world_objects { sub_6a8420(this, obj) }`
        self.per_object_state();

        self.process_events_loop2();

        // `for obj in world_objects { sub_6a8fd0(this, obj) }`
        self.per_object_pair_update();
    }

    /// `sub_6a8420`: resets the per-object accumulator. Requires the object
    /// model; kept as an explicit hook so the port stays structurally honest.
    fn per_object_state(&mut self) {
        // objects[obj.netid].accumulator = 0
    }

    /// `sub_6a8fd0`: pairwise interpolation/vision pass over world objects.
    /// Requires object positions at +0x15c/+0x160/+0x164.
    fn per_object_pair_update(&mut self) {
        // for a, b in pairs(objects) { if dist2 < radius2 { ... } }
    }

    /// `Replay_ProcessEventsLoop2` (0x6a98f0). The game walks four distinct
    /// event arrays (`+0x58`, `+0x70`, `+0x88`, `+0xa0`). Those arrays are
    /// filled per packet class while the file is parsed, and the class split
    /// is not recoverable without the packet schemas, so this port dispatches
    /// every record exactly once through stream 0 and keeps the other cursors
    /// for when the split is known.
    pub fn process_events_loop2(&mut self) {
        self.drain_stream(0);
    }

    fn drain_stream(&mut self, i: usize) {
        let limit = self.clock + self.cursors[i].clock_offset;
        let mut n = 0u64;
        loop {
            let e = match self.timeline.events.get(self.cursors[i].index) {
                Some(e) => e,
                None => break,
            };
            if e.time > limit {
                break;
            }
            self.last_event_time = e.time;
            self.cursors[i].cached_time = e.time;
            self.cursors[i].index += 1;
            n += 1;
        }
        self.dispatched += n;
    }

    /// `Replay_RunEventsUntilTime` for a caller that owns its own cursor index,
    /// mirroring the game's `SeekTime`-then-walk shape.
    pub fn run_until(&mut self, stream: usize, t: f32) -> usize {
        let mut count = 0;
        loop {
            let e = match self.timeline.events.get(self.cursors[stream].index) {
                Some(e) => e,
                None => break,
            };
            if e.time > t {
                break;
            }
            self.last_event_time = e.time;
            self.cursors[stream].index += 1;
            count += 1;
        }
        self.dispatched += count as u64;
        count
    }

    /// How far the fastest stream has read, in seconds.
    pub fn position(&self) -> f32 {
        self.last_event_time
    }

    pub fn finished(&self) -> bool {
        self.cursors[0].index >= self.timeline.len()
    }

    /// Seek every stream to `t` (the interactive equivalent of the game's
    /// `Replay_SeekTime` on each cursor).
    pub fn seek(&mut self, t: f32) {
        self.clock = t;
        let idx = self.timeline.seek_time(t);
        for c in &mut self.cursors {
            c.index = idx;
            c.cached_time = t;
        }
        self.last_event_time = t;
        self.countdown = 0.0;
    }
}
