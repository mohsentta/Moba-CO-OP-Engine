//! Bit-packed replay block reader.
//!
//! This is a line-for-line port of `Replay_ParseBlock` at `0x6af080`
//! (`Replay_ParseBlock` in `src/mod_0413_6a4e20.c`). The marker byte selects
//! each field's width:
//!
//! ```text
//! bit 7   time: one byte delta (units of 1/16 s), else absolute f32
//! bit 6   packet id: keep previous, else read u16
//! bit 5   param: one signed byte delta, else u32
//! bit 4   payload length: zero/one byte, else u32
//! low 4   record type
//! ```

pub const REL_TIME_SCALE: f32 = 1.0 / 1000.0;

#[derive(Clone, Debug)]
pub struct Block {
    pub time: f32,
    pub marker: u8,
    pub kind: u8,
    pub id: u16,
    pub param: i32,
    pub payload: Vec<u8>,
}

pub struct BlockReader<'a> {
    d: &'a [u8],
    pub pos: usize,
    pub time: f32,
    pub id: u16,
    pub param: i32,
}

impl<'a> BlockReader<'a> {
    pub fn new(d: &'a [u8]) -> Self {
        BlockReader {
            d,
            pos: 0,
            time: 0.0,
            id: 0xffff,
            param: 0,
        }
    }

    fn u8(&mut self) -> Option<u8> {
        let v = *self.d.get(self.pos)?;
        self.pos += 1;
        Some(v)
    }
    fn u16(&mut self) -> Option<u16> {
        let s = self.d.get(self.pos..self.pos + 2)?;
        self.pos += 2;
        Some(u16::from_le_bytes([s[0], s[1]]))
    }
    fn u32(&mut self) -> Option<u32> {
        let s = self.d.get(self.pos..self.pos + 4)?;
        self.pos += 4;
        Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn f32(&mut self) -> Option<f32> {
        Some(f32::from_bits(self.u32()?))
    }

    /// Returns the next block, or None at end of the chunk.
    pub fn next_block(&mut self) -> Option<Block> {
        if self.pos >= self.d.len() {
            return None;
        }
        let marker = self.u8()?;
        let kind = marker & 0x0f;

        // bit 7: relative byte, else absolute float
        if marker & 0x80 != 0 {
            let d = self.u8()?;
            self.time += d as f32 * REL_TIME_SCALE;
        } else {
            self.time = self.f32()?;
        }

        // bit 4: one-byte payload length; else a u32
        let size = if marker & 0x10 != 0 {
            self.u8()? as usize
        } else {
            self.u32()? as usize
        };

        // bit 6: packet id keeps the previous value; else a fresh u16
        if marker & 0x40 == 0 {
            self.id = self.u16()?;
        }

        // bit 5: param is a signed byte delta; else a u32
        if marker & 0x20 != 0 {
            let d = self.u8()? as i8;
            self.param = self.param.wrapping_add(d as i32);
        } else {
            self.param = self.u32()? as i32;
        }

        if self.pos + size > self.d.len() {
            return None;
        }
        let payload = self.d[self.pos..self.pos + size].to_vec();
        self.pos += size;
        Some(Block {
            time: self.time,
            marker,
            kind,
            id: self.id,
            param: self.param,
            payload,
        })
    }
}

/// Parse every block in a chunk.
pub fn parse_blocks(d: &[u8]) -> Vec<Block> {
    let mut r = BlockReader::new(d);
    let mut out = Vec::new();
    while let Some(b) = r.next_block() {
        out.push(b);
    }
    out
}
