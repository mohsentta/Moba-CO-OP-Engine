//! Reader for Riot's `.wad.client` archives.
//!
//! The shipping archive is "WAD v3": a small header, a 256-byte RSA signature
//! block, a table of contents whose entries are 32 bytes, then the chunk data.
//! Chunk names are not stored; a chunk is addressed by the 64-bit xxHash64 of
//! its ASCII-lowercased path (seed 0), exactly as the game addresses it.
//!
//! The TOC location is *derived*, not assumed: the header/signature block has
//! changed shape between patches, so we scan for a candidate TOC whose last
//! entry ends exactly at end-of-file and whose entries are internally
//! consistent. That invariant held for every shipping archive tested.

use std::fs;
use std::path::{Path, PathBuf};

use twox_hash::{XxHash64, XxHash3_64};

pub const TOC_ENTRY_SIZE: usize = 32;

#[derive(Clone, Debug)]
pub struct WadEntry {
    pub hash: u64,
    pub offset: u32,
    pub compressed_size: u32,
    pub uncompressed_size: u32,
    /// 0 = stored, 1 = gzip, 2 = zstd, 3 = zstd multi-frame
    pub compression: u8,
    pub duplicate: u8,
    pub checksum: u64,
}

#[derive(Debug)]
pub enum Compression {
    None,
    Gzip,
    Zstd,
    ZstdMulti,
    Unknown(u8),
}

impl WadEntry {
    pub fn compression_name(&self) -> &'static str {
        match self.compression {
            0 => "stored",
            1 => "gzip",
            2 => "zstd",
            3 => "zstd-multi",
            0x24 => "zstd-f",
            0x34 => "zstd-f",
            _ => "?",
        }
    }
}

pub struct Wad {
    pub path: PathBuf,
    pub bytes: Vec<u8>,
    pub toc_offset: usize,
    pub entries: Vec<WadEntry>,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(a)
}

pub fn parse_entry(b: &[u8], o: usize) -> Option<WadEntry> {
    if o + TOC_ENTRY_SIZE > b.len() {
        return None;
    }
    Some(WadEntry {
        hash: u64_at(b, o),
        offset: u32_at(b, o + 8),
        compressed_size: u32_at(b, o + 12),
        uncompressed_size: u32_at(b, o + 16),
        compression: b[o + 20],
        duplicate: b[o + 21],
        // o+22..o+24 is padding in every archive observed
        checksum: u64_at(b, o + 24),
    })
}

/// Score a candidate TOC at `count_at` (the u32 count sits there, entries at +4).
/// Returns a confidence score; 0 means rejected.
fn score_toc(b: &[u8], count_at: usize) -> u32 {
    if count_at + 4 > b.len() {
        return 0;
    }
    let n = u32_at(b, count_at) as usize;
    if n == 0 || n > 1_000_000 {
        return 0;
    }
    let toc = count_at + 4;
    let end = match toc.checked_add(n * TOC_ENTRY_SIZE) {
        Some(e) if e <= b.len() => e,
        _ => return 0,
    };

    let mut score = 1u32;
    let mut prev_off = 0u64;
    for i in 0..n {
        let o = toc + i * TOC_ENTRY_SIZE;
        let e = match parse_entry(b, o) {
            Some(e) => e,
            None => return 0,
        };
        if e.compression > 3 {
            return 0;
        }
        if e.offset as u64 + e.compressed_size as u64 > b.len() as u64 {
            return 0;
        }
        // Uncompressed size should not be absurd (guards against false TOCs).
        if e.uncompressed_size as u64 > 512 * 1024 * 1024 {
            return 0;
        }
        // Chunk data must live at or after the TOC (duplicates may repeat an
        // earlier offset, so only a gross backward jump is rejected).
        if (e.offset as u64) + 64 < end as u64 || (i > 64 && (e.offset as u64) < prev_off / 2) {
            return 0;
        }
        prev_off = e.offset as u64;
        if score < 500 {
            score += 1;
        }
    }
    // The strongest invariant: the final chunk touches end-of-file.
    let last = parse_entry(b, toc + (n - 1) * TOC_ENTRY_SIZE).unwrap();
    let tail = last.offset as u64 + last.compressed_size as u64;
    if tail == b.len() as u64 {
        score += 1_000_000;
    } else if tail + 16 >= b.len() as u64 {
        score += 1000;
    }
    score
}

/// Print the best few TOC candidates (diagnostics for a new patch).
pub fn toc_candidates(b: &[u8]) -> Vec<(u32, usize, u32)> {
    let mut v = Vec::new();
    for off in 0..8192.min(b.len().saturating_sub(4)) {
        let s = score_toc(b, off);
        if s > 0 {
            v.push((s, off, u32_at(b, off)));
        }
    }
    v.sort_by_key(|&(s, _, _)| std::cmp::Reverse(s));
    v.truncate(8);
    v
}

/// Walk a contiguous run of TOC entries starting at `start`.
///
/// An entry's data must live *after* the entry itself. That single rule makes
/// the walk self-terminating: the first non-entry (chunk data) breaks it.
pub fn walk_entries(b: &[u8], start: usize) -> Vec<WadEntry> {
    let mut v = Vec::new();
    let mut o = start;
    loop {
        let e = match parse_entry(b, o) {
            Some(e) => e,
            None => break,
        };
        // Compression codes observed in shipping WADs. `0x24`/`0x34` carry
        // flag bits above the low nibble but still hold a zstd frame.
        if !matches!(e.compression, 0 | 1 | 2 | 3 | 4 | 0x24 | 0x34) {
            break;
        }
        if e.compressed_size == 0 || e.uncompressed_size == 0 {
            break;
        }
        if e.offset as u64 + e.compressed_size as u64 > b.len() as u64 {
            break;
        }
        if (e.offset as usize) < o + TOC_ENTRY_SIZE {
            break;
        }
        v.push(e);
        o += TOC_ENTRY_SIZE;
        if v.len() > 4_000_000 {
            break;
        }
    }
    v
}

/// True when the TOC entry's stored bytes match the XXH3 checksum it carries.
pub fn entry_checksum_ok(b: &[u8], e: &WadEntry) -> bool {
    let s = e.offset as usize;
    let end = s + e.compressed_size as usize;
    if end > b.len() {
        return false;
    }
    checksum(&b[s..end]) == e.checksum
}

impl Wad {
    pub fn open(path: impl AsRef<Path>) -> Result<Wad, String> {
        let path = path.as_ref().to_path_buf();
        let bytes = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
        if bytes.len() < 16 || &bytes[0..2] != b"RW" {
            return Err(format!("{}: not a RW WAD", path.display()));
        }

        let mut best: Option<(usize, Vec<WadEntry>, u64)> = None;
        for start in (16..4096).step_by(16) {
            let mut entries = walk_entries(&bytes, start);
            if entries.is_empty() {
                continue;
            }
            // The structural walk can run past the real TOC into chunk data.
            // The defining invariant is that every chunk's data starts after
            // the TOC itself, so shrink the entry list until that holds for
            // the whole prefix.
            let mut n = entries.len();
            loop {
                if n == 0 {
                    break;
                }
                let min_off = entries[..n]
                    .iter()
                    .map(|e| e.offset as usize)
                    .min()
                    .unwrap_or(0);
                let allowed = min_off.saturating_sub(start) / TOC_ENTRY_SIZE;
                if allowed >= n {
                    break;
                }
                n = allowed;
            }
            entries.truncate(n);
            if entries.is_empty() {
                continue;
            }
            let toc_end = start + entries.len() * TOC_ENTRY_SIZE;
            if entries.iter().any(|e| (e.offset as usize) < toc_end) {
                continue;
            }
            let last = entries.last().unwrap();
            let tail = last.offset as u64 + last.compressed_size as u64;
            let score =
                entries.len() as u64 + if tail == bytes.len() as u64 { 1_000_000 } else { 0 };
            if best.as_ref().map(|(_, _e, s)| score > *s).unwrap_or(true) {
                best = Some((start, entries, score));
            }
        }
        let (toc_offset, entries, _) = match best {
            Some(x) => x,
            None => {
                return Err(format!(
                    "{}: no consistent TOC found (size {})",
                    path.display(),
                    bytes.len()
                ))
            }
        };
        Ok(Wad {
            path,
            bytes,
            toc_offset,
            entries,
        })
    }

    pub fn chunk_count(&self) -> usize {
        self.entries.len()
    }

    pub fn find_hash(&self, hash: u64) -> Option<&WadEntry> {
        self.entries.iter().find(|e| e.hash == hash)
    }

    pub fn raw(&self, e: &WadEntry) -> &[u8] {
        let s = e.offset as usize;
        &self.bytes[s..s + e.compressed_size as usize]
    }

    pub fn read_chunk(&self, e: &WadEntry) -> Result<Vec<u8>, String> {
        let raw = self.raw(e);
        let want = e.uncompressed_size as usize;

        // Dispatch on the stored magic where possible: shipped archives use
        // several compression codes for the same zstd payload.
        if raw.starts_with(&[0x28, 0xb5, 0x2f, 0xfd]) {
            return zstd_decompress(raw, want);
        }
        if raw.starts_with(&[0x1f, 0x8b]) {
            return inflate_gzip(raw, want);
        }
        if e.compression == 0 {
            if raw.len() != want {
                return Err(format!(
                    "stored chunk {} has {} bytes, toc says {}",
                    e.hash,
                    raw.len(),
                    want
                ));
            }
            return Ok(raw.to_vec());
        }
        // Last resort: try zstd if the sizes imply compression.
        if e.compressed_size != e.uncompressed_size {
            return zstd_decompress(raw, want);
        }
        if raw.len() != want {
            return Err(format!(
                "chunk {}: {} bytes, toc says {} (comp {})",
                e.hash,
                raw.len(),
                want,
                e.compression
            ));
        }
        Ok(raw.to_vec())
    }

    /// Look up by path (case-insensitive, forward slashes) and decompress.
    pub fn read_path(&self, path: &str) -> Result<Vec<u8>, String> {
        let h = path_hash(path);
        let e = self
            .find_hash(h)
            .ok_or_else(|| format!("no chunk for {path:?} (hash {h:016x})"))?;
        self.read_chunk(e)
    }
}

/// xxHash64 of the ASCII-lowercased path, seed 0. This is the game's chunk key.
pub fn path_hash(path: &str) -> u64 {
    let lower = path.replace('\\', "/").to_ascii_lowercase();
    XxHash64::oneshot(0, lower.as_bytes())
}

/// XXH3-64 of stored bytes, seed 0 (the shipping WAD checksum).
pub fn checksum(data: &[u8]) -> u64 {
    XxHash3_64::oneshot(data)
}

fn zstd_decompress(raw: &[u8], want: usize) -> Result<Vec<u8>, String> {
    use std::io::{Cursor, Read};
    let mut out = Vec::with_capacity(want);
    let mut pos = 0usize;
    // A `ZstdMulti` chunk is a run of independent frames.
    while pos < raw.len() && out.len() < want {
        let mut cur = Cursor::new(&raw[pos..]);
        let mut dec = ruzstd::decoding::StreamingDecoder::new(&mut cur)
            .map_err(|e| format!("zstd frame: {e:?}"))?;
        let before = out.len();
        dec.read_to_end(&mut out)
            .map_err(|e| format!("zstd decode: {e}"))?;
        let consumed = cur.position() as usize;
        if consumed == 0 || out.len() == before {
            return Err("zstd frame consumed no input".into());
        }
        pos += consumed;
    }
    if out.len() != want {
        return Err(format!("zstd produced {} bytes, want {}", out.len(), want));
    }
    Ok(out)
}

/// Minimal DEFLATE/gzip decoder (WAD compression type 1).
///
/// RFC 1951 inflate, written here so the tool needs no C toolchain.
fn inflate_gzip(raw: &[u8], want: usize) -> Result<Vec<u8>, String> {
    // Skip the gzip member header when present.
    let mut s = 0usize;
    if raw.len() >= 2 && raw[0] == 0x1f && raw[1] == 0x8b {
        if raw.len() < 10 || raw[2] != 8 {
            return Err("bad gzip header".into());
        }
        let flg = raw[3];
        s = 10;
        if flg & 4 != 0 {
            if s + 2 > raw.len() {
                return Err("truncated gzip extra".into());
            }
            let xlen = u16::from_le_bytes([raw[s], raw[s + 1]]) as usize;
            s += 2 + xlen;
        }
        if flg & 8 != 0 {
            while s < raw.len() && raw[s] != 0 {
                s += 1;
            }
            s += 1;
        }
        if flg & 16 != 0 {
            while s < raw.len() && raw[s] != 0 {
                s += 1;
            }
            s += 1;
        }
        if flg & 2 != 0 {
            s += 2;
        }
        if s >= raw.len() {
            return Err("truncated gzip".into());
        }
    }
    let mut inf = Inflater::new(&raw[s..]);
    let out = inf.run()?;
    if out.len() != want {
        return Err(format!("inflate produced {} bytes, want {}", out.len(), want));
    }
    Ok(out)
}

struct BitReader<'a> {
    d: &'a [u8],
    pos: usize,
    bit: u32,
}

impl<'a> BitReader<'a> {
    fn new(d: &'a [u8]) -> Self {
        BitReader { d, pos: 0, bit: 0 }
    }
    fn bits(&mut self, n: u32) -> Result<u32, String> {
        let mut v = 0u32;
        for i in 0..n {
            if self.pos >= self.d.len() {
                return Err("out of bits".into());
            }
            let b = (self.d[self.pos] >> self.bit) & 1;
            v |= (b as u32) << i;
            self.bit += 1;
            if self.bit == 8 {
                self.bit = 0;
                self.pos += 1;
            }
        }
        Ok(v)
    }
    fn align(&mut self) {
        if self.bit != 0 {
            self.bit = 0;
            self.pos += 1;
        }
    }
}

struct Huffman {
    // counts per bit length, and symbols ordered by code
    counts: [u16; 16],
    symbols: Vec<u16>,
}

impl Huffman {
    fn from_lengths(lengths: &[u8]) -> Huffman {
        let mut counts = [0u16; 16];
        for &l in lengths {
            counts[l as usize] += 1;
        }
        counts[0] = 0;
        let mut offs = [0u16; 16];
        for i in 1..16 {
            offs[i] = offs[i - 1] + counts[i - 1];
        }
        let mut symbols = vec![0u16; lengths.iter().filter(|&&l| l != 0).count()];
        for (sym, &l) in lengths.iter().enumerate() {
            if l != 0 {
                symbols[offs[l as usize] as usize] = sym as u16;
                offs[l as usize] += 1;
            }
        }
        Huffman { counts, symbols }
    }

    fn decode(&self, br: &mut BitReader) -> Result<u16, String> {
        let mut code = 0i32;
        let mut first = 0i32;
        let mut index = 0i32;
        for len in 1..16 {
            code |= br.bits(1)? as i32;
            let count = self.counts[len] as i32;
            if code - first < count {
                return Ok(self.symbols[(index + code - first) as usize]);
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        Err("bad huffman code".into())
    }
}

struct Inflater<'a> {
    br: BitReader<'a>,
    out: Vec<u8>,
}

impl<'a> Inflater<'a> {
    fn new(d: &'a [u8]) -> Self {
        Inflater {
            br: BitReader::new(d),
            out: Vec::new(),
        }
    }

    fn run(&mut self) -> Result<Vec<u8>, String> {
        loop {
            let last = self.br.bits(1)?;
            let ty = self.br.bits(2)?;
            match ty {
                0 => self.stored()?,
                1 => self.fixed()?,
                2 => self.dynamic()?,
                _ => return Err("bad block type".into()),
            }
            if last == 1 {
                break;
            }
        }
        let mut out = std::mem::take(&mut self.out);
        // gzip trailer is crc32+isize; some chunks are zlib-wrapped instead.
        out.truncate(out.len());
        Ok(out)
    }

    fn stored(&mut self) -> Result<(), String> {
        self.br.align();
        let len = self.br.bits(16)? as usize;
        let nlen = self.br.bits(16)? as usize;
        if len != (!nlen & 0xffff) {
            return Err("stored block length mismatch".into());
        }
        for _ in 0..len {
            self.out.push(self.br.bits(8)? as u8);
        }
        Ok(())
    }

    fn fixed(&mut self) -> Result<(), String> {
        let mut lens = [0u8; 288];
        for i in 0..144 {
            lens[i] = 8;
        }
        for i in 144..256 {
            lens[i] = 9;
        }
        for i in 256..280 {
            lens[i] = 7;
        }
        for i in 280..288 {
            lens[i] = 8;
        }
        let lit = Huffman::from_lengths(&lens);
        let dist = Huffman::from_lengths(&[5u8; 30]);
        self.codes(&lit, &dist)
    }

    fn dynamic(&mut self) -> Result<(), String> {
        let hlit = self.br.bits(5)? as usize + 257;
        let hdist = self.br.bits(5)? as usize + 1;
        let hclen = self.br.bits(4)? as usize + 4;
        const ORDER: [usize; 19] = [
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
        ];
        let mut cl = [0u8; 19];
        for i in 0..hclen {
            cl[ORDER[i]] = self.br.bits(3)? as u8;
        }
        let clh = Huffman::from_lengths(&cl);
        let mut lens = Vec::with_capacity(hlit + hdist);
        while lens.len() < hlit + hdist {
            let sym = clh.decode(&mut self.br)?;
            match sym {
                0..=15 => lens.push(sym as u8),
                16 => {
                    let prev = *lens.last().ok_or("repeat with no previous")?;
                    let n = 3 + self.br.bits(2)? as usize;
                    lens.extend(std::iter::repeat(prev).take(n));
                }
                17 => {
                    let n = 3 + self.br.bits(3)? as usize;
                    lens.extend(std::iter::repeat(0).take(n));
                }
                18 => {
                    let n = 11 + self.br.bits(7)? as usize;
                    lens.extend(std::iter::repeat(0).take(n));
                }
                _ => return Err("bad code length symbol".into()),
            }
        }
        lens.truncate(hlit + hdist);
        let lit = Huffman::from_lengths(&lens[..hlit]);
        let dist = Huffman::from_lengths(&lens[hlit..]);
        self.codes(&lit, &dist)
    }

    fn codes(&mut self, lit: &Huffman, dist: &Huffman) -> Result<(), String> {
        const LBASE: [u16; 29] = [
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258,
        ];
        const LEXT: [u8; 29] = [
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
            0,
        ];
        const DBASE: [u16; 30] = [
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
            1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
        ];
        const DEXT: [u8; 30] = [
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
            12, 13, 13,
        ];
        loop {
            let sym = lit.decode(&mut self.br)? as usize;
            if sym < 256 {
                self.out.push(sym as u8);
            } else if sym == 256 {
                return Ok(());
            } else {
                let i = sym - 257;
                if i >= 29 {
                    return Err("bad length symbol".into());
                }
                let len = LBASE[i] as usize + self.br.bits(LEXT[i] as u32)? as usize;
                let ds = dist.decode(&mut self.br)? as usize;
                if ds >= 30 {
                    return Err("bad distance symbol".into());
                }
                let d = DBASE[ds] as usize + self.br.bits(DEXT[ds] as u32)? as usize;
                if d > self.out.len() {
                    return Err("distance past start".into());
                }
                let start = self.out.len() - d;
                for k in 0..len {
                    let b = self.out[start + k];
                    self.out.push(b);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_matches_known() {
        // xxh64("hello", 0)
        assert_eq!(XxHash64::oneshot(0, b"hello"), 0x26c7827d889f6da3);
    }

    #[test]
    fn hash_is_case_insensitive() {
        assert_eq!(path_hash("DATA/A.bin"), path_hash("data/a.bin"));
    }
}
