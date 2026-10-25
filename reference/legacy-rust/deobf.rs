//! Inverse of the payload byte substitution used on netId/position fields.
//!
//! Recovered from `Replay_RunEventsUntilTime` (0x69ac90), which decodes a
//! 4-byte field in place before handing it to `Registry_FindObjectById`:
//!
//! ```text
//! x   = b ^ 0x5f
//! s   = (x << 1) & 0xff
//! idx = (s >> 4) | (((b >> 7) | s) << 4)
//! out = ror5(table[idx]) ^ 0xd4          // table at RVA 0x18badf0
//! ```

pub static TABLE: [u8; 256] = *include_bytes!("subst.bin");

#[inline]
pub fn deobf_byte(b: u8) -> u8 {
    let x = b ^ 0x5f;
    let s = x.wrapping_shl(1);
    let idx = (s >> 4) | ((b >> 7 | s).wrapping_shl(4));
    let t = TABLE[idx as usize];
    ((t >> 5) | t.wrapping_shl(3)) ^ 0xd4
}

pub fn deobf(d: &[u8]) -> Vec<u8> {
    d.iter().map(|&b| deobf_byte(b)).collect()
}
