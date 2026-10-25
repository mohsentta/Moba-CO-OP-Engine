//! The replay playback cursor, mirroring the game's replay controller.
//!
//! In the 16.21 binary the event list is an array of 0x18-byte records:
//!
//! ```text
//! +0x00  f32  time (seconds)
//! +0x08  u64  packet header (low 32 = param, high 32 = netId)
//! +0x10  ptr  refcounted payload buffer
//! ```
//!
//! `Replay_SeekTime` (0x6bb2d0) binary-searches that array for the first
//! timestamp >= the requested time. `Replay_SeekCursor` (0x6ba930) sets the
//! index and caches the timestamp. `Replay_PeekEvent` (0x6abca0) returns the
//! record at the cursor without advancing. `Replay_RunEventsUntilTime`
//! (0x69ac90) walks the cursor while the event time is <= the game clock and
//! dispatches each record through the object registry.
//!
//! This type is that state machine, without the object registry (which lives
//! behind the game's ~1300 registered packet types).

use crate::block::{self, Block};
use crate::rofl::Rofl;

#[derive(Clone, Debug)]
pub struct Event {
    pub time: f32,
    pub kind: u8,
    pub id: u16,
    pub param: i32,
    pub payload: Vec<u8>,
}

impl From<Block> for Event {
    fn from(b: Block) -> Event {
        Event {
            time: b.time,
            kind: b.kind,
            id: b.id,
            param: b.param,
            payload: b.payload,
        }
    }
}

pub struct Timeline {
    /// Sorted exactly like the game's event array (by time).
    pub events: Vec<Event>,
    /// Current record index (`*param_1` in the decompile).
    pub cursor: usize,
    /// Cached timestamp of the cursor record (`param_1[4]`).
    pub cursor_time: f32,
}

impl Timeline {
    pub fn from_rofl(r: &Rofl) -> Timeline {
        let mut events = Vec::new();
        for c in &r.chunks {
            for b in block::parse_blocks(&c.data) {
                events.push(Event::from(b));
            }
        }
        events.sort_by(|a, b| a.time.total_cmp(&b.time));
        Timeline {
            events,
            cursor: 0,
            cursor_time: 0.0,
        }
    }

    pub fn len(&self) -> usize {
        self.events.len()
    }

    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }

    pub fn duration(&self) -> f32 {
        self.events.last().map(|e| e.time).unwrap_or(0.0)
    }

    /// `Replay_SeekTime`: index of the first event at or after `t`.
    pub fn seek_time(&self, t: f32) -> usize {
        // The game binary-searches; `partition_point` is the same search.
        self.events.partition_point(|e| e.time < t)
    }

    /// `Replay_SeekCursor`: set the index, clamp to the array, and cache the
    /// timestamp. Returns the effective index.
    pub fn seek_cursor(&mut self, index: usize) -> usize {
        let clamped = index.min(self.events.len());
        self.cursor = clamped;
        self.cursor_time = if clamped < self.events.len() {
            self.events[clamped].time
        } else {
            0.0
        };
        clamped
    }

    /// `Replay_PeekEvent`: the record at the cursor, without advancing.
    pub fn peek(&self) -> Option<&Event> {
        self.events.get(self.cursor)
    }

    /// Seek to `t`, then `Replay_RunEventsUntilTime`: dispatch every event with
    /// `time <= t`. This is the per-frame work the replay controller performs.
    pub fn run_until(&mut self, t: f32) -> Vec<usize> {
        // If the cursor is already past `t` (a backwards seek), reposition it
        // the way Replay_SeekTime does.
        if self.cursor < self.events.len() && self.events[self.cursor].time > t {
            self.seek_cursor(self.seek_time(t));
        }
        let mut fired = Vec::new();
        while self.cursor < self.events.len() && self.events[self.cursor].time <= t {
            fired.push(self.cursor);
            self.cursor += 1;
        }
        self.cursor_time = self
            .events
            .get(self.cursor)
            .map(|e| e.time)
            .unwrap_or(0.0);
        fired
    }
}
