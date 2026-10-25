//! Named constants for the replay slice, so the port doesn't traffic in magic
//! numbers. Mirrors `port/SYMBOLS.md`.

#![allow(dead_code)]

/// Function RVAs (image base 0x140000000).
pub mod func {
    pub const REPLAY_OPEN: u64 = 0x6b1b60;
    pub const REPLAY_LOAD_CHUNKS: u64 = 0x6b2890;
    pub const REPLAY_DECOMPRESS_CHUNK: u64 = 0x6b6040;
    pub const REPLAY_PARSE_BLOCK: u64 = 0x6af080;
    pub const REPLAY_BUILD_RECORD: u64 = 0x6b2e90;
    pub const REPLAY_SEEK_TIME: u64 = 0x6bb2d0;
    pub const REPLAY_SEEK_CURSOR: u64 = 0x6ba930;
    pub const REPLAY_PEEK_EVENT: u64 = 0x6abca0;
    pub const REPLAY_RUN_EVENTS_UNTIL_TIME: u64 = 0x69ac90;
    pub const REPLAY_PROCESS_EVENTS_LOOP: u64 = 0x6a92f0;
    pub const REPLAY_PROCESS_EVENTS_LOOP2: u64 = 0x6a98f0;
    pub const REPLAY_ACTIVE_GATE: u64 = 0x6b1580;
    pub const REPLAY_SYNC: u64 = 0x6b8460;
    pub const REPLAY_UPDATE_A: u64 = 0x6b8260;
    pub const REPLAY_UPDATE_B: u64 = 0x6bf600;
    pub const REPLAY_MODE_UPDATE: u64 = 0x9aa080;
    pub const REPLAY_CAMERA_DIRECTOR_TICK: u64 = 0x6be560;
    pub const REPLAY_RESET_OBJECT_ACCUMULATORS: u64 = 0x6a8420;
    pub const REPLAY_SCORE_OBJECT_VISIBILITY: u64 = 0x6a8fd0;
    pub const REPLAY_SCORE_MAP_FIND_OR_INSERT: u64 = 0x275330;
    pub const REPLAY_VISIBILITY_MAP_TEST: u64 = 0x6a5180;
    pub const REGISTRY_FIND_OBJECT_BY_ID: u64 = 0x531740;
    pub const GAME_CLOCK_READ: u64 = 0x100a110;
    pub const GAME_CLOCK_IS_PAUSED: u64 = 0x100a6f0;
}

/// Replay object fields (base = pointer passed to `Replay_Open`).
pub mod replay {
    pub const GAME_RECORDS: u64 = 0x08;
    pub const GAME_COUNT: u64 = 0x10;
    pub const KEYFRAME_RECORDS: u64 = 0x18;
    pub const KEYFRAME_COUNT: u64 = 0x20;
    pub const END_TIME: u64 = 0x40;
    pub const BOUNDARY_ID_3: u64 = 0x50;
    pub const BOUNDARY_ID_4: u64 = 0x54;
    pub const VERSION_STRING: u64 = 0x68;
    pub const PREAMBLE: u64 = 0x88;
    /// Cursor structs, 0x18 bytes each.
    pub const CURSOR_A: u64 = 0x58;
    pub const CURSOR_B: u64 = 0x70;
    pub const CURSOR_C: u64 = 0x88;
    pub const CURSOR_D: u64 = 0xa0;
}

/// Game object fields.
pub mod obj {
    pub const NET_ID: u64 = 0x20;
    pub const TEAM: u64 = 0x158;
    pub const POS_X: u64 = 0x15c;
    pub const POS_Y: u64 = 0x160; // ignored by 2D distance math
    pub const POS_Z: u64 = 0x164;
    pub const VISIBILITY_COMPONENT: u64 = 0x1a8;
}

/// Camera director state, relative to the director's own base.
pub mod director {
    pub const COUNTDOWN: u64 = 0x04;
    pub const CAMERA_TARGET: u64 = 0x08;
    pub const PREVIOUS_TARGET: u64 = 0x0c;
    pub const SWITCH_TIMER: u64 = 0x10;
    pub const HOLD_TIMER: u64 = 0x14;
    pub const COUNTDOWN_BASE: u64 = 0x104;
    pub const COUNTDOWN_SEED: u64 = 0x108;
    pub const SEED_A: u64 = 0x10c;
    pub const SEED_B: u64 = 0x110;
    pub const SWITCH_SEED: u64 = 0x114;
    pub const SWITCH_DECAY: u64 = 0x118;
    pub const HOLD_SEED: u64 = 0x11c;
    pub const HOLD_DECAY: u64 = 0x120;
    pub const SCORE_DIVISOR: u64 = 0x144;
    pub const RADIUS_SQ: u64 = 0x148;
    pub const ENEMY_SCORE_MULT: u64 = 0x1f0;
    pub const SWITCH_COOLDOWN: u64 = 0x22c;
    pub const MIN_SCORE: u64 = 0x230;
    pub const SWITCH_FLOOR: u64 = 0x234;
    pub const HOLD_FLOOR: u64 = 0x238;
    pub const SCORE_SAME_TEAM: u64 = 0x14c;
    pub const SCORE_ENEMY: u64 = 0x150;
}
