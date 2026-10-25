//! `.rofl` (ROFL2) container reader.
//!
//! Ported from `sub_6b1b60` (replay load) and `Replay_LoadChunks` at
//! `0x6b2890` in `League of Legends.exe` 16.21:
//!
//! ```text
//! char  magic[4] = "RIOT"
//! u16   format            (2 for the shipping format)
//! u64   file_time         (only for format 2)
//! u8    version_len
//! char  version[version_len]   e.g. "16.21.829.8620"
//! chunk[..]:
//!     u32 id
//!     u32 time            (ms)
//!     u8  kind            1/2/3/4, 2 = keyframe
//!     u32 uncompressed_size
//!     u32 compressed_size  (0 = stored)
//!     u8  tag
//!     u8  payload[compressed_size or uncompressed_size]
//! u8    signature[256]
//! char  footer_json[footer_len]
//! u32   footer_len        (last 4 bytes)
//! ```
//!
//! The walk must land exactly on the signature; anything else is a corrupt
//! file, which is the same invariant the game relies on.

use std::fs;
use std::path::{Path, PathBuf};

pub const SIGNATURE_LEN: usize = 256;
pub const CHUNK_HEADER_LEN: usize = 18;

#[derive(Clone)]
pub struct Chunk {
    pub id: u32,
    pub time: u32,
    pub kind: u8,
    pub uncompressed_size: u32,
    pub compressed_size: u32,
    pub tag: u8,
    pub data: Vec<u8>,
}

impl Chunk {
    pub fn is_keyframe(&self) -> bool {
        self.kind == 2
    }
}

pub struct Rofl {
    pub path: PathBuf,
    pub format: u16,
    pub file_time: u64,
    pub version: String,
    pub chunks: Vec<Chunk>,
    pub signature: Vec<u8>,
    pub footer_json: String,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn zstd_all(raw: &[u8], want: usize) -> Result<Vec<u8>, String> {
    use std::io::{Cursor, Read};
    let mut out = Vec::with_capacity(want);
    let mut pos = 0usize;
    while pos < raw.len() && out.len() < want {
        let mut cur = Cursor::new(&raw[pos..]);
        let mut dec = ruzstd::decoding::StreamingDecoder::new(&mut cur)
            .map_err(|e| format!("zstd: {e:?}"))?;
        dec.read_to_end(&mut out).map_err(|e| format!("zstd: {e}"))?;
        let n = cur.position() as usize;
        if n == 0 {
            break;
        }
        pos += n;
    }
    Ok(out)
}

/// One chunk located by the structural walk, before decompression.
struct RawChunk {
    id: u32,
    time: u32,
    kind: u8,
    usize: u32,
    csize: u32,
    tag: u8,
    payload_start: usize,
    stored: usize,
}

/// Walk chunk headers of `header_len` bytes from `start`. Returns None unless
/// the walk lands exactly on the signature.
fn walk_chunks(b: &[u8], start: usize, sig_start: usize, header_len: usize) -> Option<Vec<RawChunk>> {
    let mut pos = start;
    let mut out = Vec::new();
    while pos + header_len <= sig_start {
        let id = u32_at(b, pos);
        let time = u32_at(b, pos + 4);
        let kind = b[pos + 8];
        let usize_ = u32_at(b, pos + 9);
        let csize = u32_at(b, pos + 13);
        let tag = if header_len >= 18 { b[pos + 17] } else { 0 };
        let payload_start = pos + header_len;
        let stored = if csize == 0 {
            usize_ as usize
        } else {
            csize as usize
        };
        if payload_start + stored > sig_start {
            return None;
        }
        out.push(RawChunk {
            id,
            time,
            kind,
            usize: usize_,
            csize,
            tag,
            payload_start,
            stored,
        });
        pos = payload_start + stored;
    }
    if pos == sig_start && !out.is_empty() {
        Some(out)
    } else {
        None
    }
}

impl Rofl {
    pub fn open(path: impl AsRef<Path>) -> Result<Rofl, String> {
        let path = path.as_ref().to_path_buf();
        let b = fs::read(&path).map_err(|e| format!("read {}: {e}", path.display()))?;
        Self::parse(path, &b)
    }

    pub fn parse(path: PathBuf, b: &[u8]) -> Result<Rofl, String> {
        if b.len() < 32 || &b[0..4] != b"RIOT" {
            return Err(format!("{}: not a RIOT replay", path.display()));
        }
        let format = u16::from_le_bytes([b[4], b[5]]);
        if format != 2 {
            return Err(format!(
                "{}: ROFL format {format} is not supported (the 16.x path is format 2)",
                path.display()
            ));
        }
        let file_time = u64::from_le_bytes(b[6..14].try_into().unwrap());
        let vlen = b[14] as usize;
        if 15 + vlen > b.len() {
            return Err("truncated version string".into());
        }
        let version = String::from_utf8_lossy(&b[15..15 + vlen]).to_string();

        // Footer: u32 length, JSON, then the 256-byte signature before it.
        if b.len() < 4 + SIGNATURE_LEN {
            return Err("file too small for footer".into());
        }
        let footer_len = u32_at(b, b.len() - 4) as usize;
        let footer_start = b
            .len()
            .checked_sub(4 + footer_len)
            .ok_or("footer length out of range")?;
        if footer_start < SIGNATURE_LEN + 15 + vlen {
            return Err("footer overlaps header".into());
        }
        let sig_start = footer_start - SIGNATURE_LEN;
        let signature = b[sig_start..footer_start].to_vec();
        let footer_json = String::from_utf8_lossy(&b[footer_start..b.len() - 4]).to_string();

        let pos = 15 + vlen;
        // Two header widths exist in the wild: 18 bytes on 16.1x+ builds
        // (extra tag byte) and 17 bytes before that. The game's loader tries
        // both and keeps whichever walk lands exactly on the signature.
        let mut chosen: Option<(usize, Vec<RawChunk>)> = None;
        for hdr in [CHUNK_HEADER_LEN, CHUNK_HEADER_LEN - 1] {
            if let Some(raws) = walk_chunks(b, pos, sig_start, hdr) {
                // Validate a compressed chunk so a coincidental walk cannot win.
                let sample = raws.iter().find(|r| r.csize != 0 && r.usize != 0);
                if let Some(s) = sample {
                    let payload = &b[s.payload_start..s.payload_start + s.csize as usize];
                    if zstd_all(payload, s.usize as usize)
                        .map(|d| d.len() == s.usize as usize)
                        .unwrap_or(false)
                    {
                        chosen = Some((hdr, raws));
                        break;
                    }
                } else {
                    chosen = Some((hdr, raws));
                    break;
                }
            }
        }
        let (header_len, raws) = chosen.ok_or_else(|| {
            format!(
                "{}: no chunk walk lands on the signature (0x{sig_start:x})",
                path.display()
            )
        })?;
        let mut chunks = Vec::with_capacity(raws.len());
        for rc in raws {
            let raw = &b[rc.payload_start..rc.payload_start + rc.stored];
            let data = if rc.csize == 0 {
                raw.to_vec()
            } else {
                let d = zstd_all(raw, rc.usize as usize)?;
                if d.len() != rc.usize as usize {
                    return Err(format!(
                        "chunk {}: decompressed {} bytes, header says {}",
                        rc.id,
                        d.len(),
                        rc.usize
                    ));
                }
                d
            };
            chunks.push(Chunk {
                id: rc.id,
                time: rc.time,
                kind: rc.kind,
                uncompressed_size: rc.usize,
                compressed_size: rc.csize,
                tag: rc.tag,
                data,
            });
        }
        let _ = header_len;

        Ok(Rofl {
            path,
            format,
            file_time,
            version,
            chunks,
            signature,
            footer_json,
        })
    }

    pub fn total_events_hint(&self) -> u64 {
        self.chunks.iter().map(|c| c.data.len() as u64).sum()
    }

    pub fn footer_field(&self, key: &str) -> Option<String> {
        // A tiny, dependency-free scan for `"key":value` in the footer.
        let pat = format!("\"{key}\":");
        let i = self.footer_json.find(&pat)? + pat.len();
        let rest = &self.footer_json[i..];
        let mut out = String::new();
        let mut in_str = false;
        let mut depth = 0i32;
        for ch in rest.chars() {
            if in_str {
                out.push(ch);
                if ch == '\\' {
                    // keep escapes as-is; this is only used for display
                }
                if ch == '"' && !out.ends_with("\\\"") {
                    in_str = false;
                }
                continue;
            }
            match ch {
                '"' => {
                    in_str = true;
                    out.push(ch);
                }
                '{' | '[' => {
                    depth += 1;
                    out.push(ch);
                }
                '}' | ']' => {
                    depth -= 1;
                    out.push(ch);
                    if depth <= 0 && ch == '}' {
                        return Some(out);
                    }
                }
                ',' if depth == 0 => return Some(out.trim().to_string()),
                _ => out.push(ch),
            }
        }
        Some(out.trim().to_string())
    }
}
