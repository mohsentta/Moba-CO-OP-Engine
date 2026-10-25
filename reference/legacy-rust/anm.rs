//! `.anm` animation parser (Riot `r3d2anmd`), versions 3/4/5.
//!
//! Ported from LeagueToolkit (`ltk_anim::asset::uncompressed`):
//!
//! - v5: joint hashes in their own section, 6-byte quantized quaternions
//! - v4: joint hash embedded per frame, 16-byte quaternions
//! - v3: 32-byte padded joint names (ELF hash), rotation + translation only
//!
//! Palette-based: every frame stores three u16 indices into a shared vector
//! palette (translation, scale) and a quaternion palette (rotation).

use std::collections::HashMap;

#[derive(Clone, Copy, Debug, Default)]
pub struct Frame {
    pub t: u16,
    pub s: u16,
    pub r: u16,
}

#[derive(Debug)]
pub struct Anm {
    pub version: u32,
    pub fps: f32,
    pub duration: f32,
    pub frame_count: usize,
    pub vec_pal: Vec<[f32; 3]>,
    pub quat_pal: Vec<[f32; 4]>,
    pub tracks: HashMap<u32, Vec<Frame>>,
}

fn u16_at(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
fn u32_at(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}
fn i32_at(d: &[u8], o: usize) -> i32 {
    i32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}
fn f32_at(d: &[u8], o: usize) -> f32 {
    f32::from_bits(u32_at(d, o))
}
fn vec3(d: &[u8], o: usize) -> [f32; 3] {
    [f32_at(d, o), f32_at(d, o + 4), f32_at(d, o + 8)]
}
fn quat(d: &[u8], o: usize) -> [f32; 4] {
    [
        f32_at(d, o),
        f32_at(d, o + 4),
        f32_at(d, o + 8),
        f32_at(d, o + 12),
    ]
}

pub fn is_anm(d: &[u8]) -> Option<bool> {
    if d.len() < 12 {
        return None;
    }
    match &d[0..8] {
        b"r3d2anmd" => Some(false),
        b"r3d2canm" => Some(true),
        _ => None,
    }
}

fn decompress_quat6(b: &[u8]) -> [f32; 4] {
    let first = b[0] as u64 | ((b[1] as u64) << 8);
    let second = b[2] as u64 | ((b[3] as u64) << 8);
    let third = b[4] as u64 | ((b[5] as u64) << 8);
    let bits = first | (second << 16) | (third << 32);
    let max_index = ((bits >> 45) & 3) as u8;
    let sqrt2 = std::f32::consts::SQRT_2;
    let one_div = sqrt2 / 2.0;
    let scale = sqrt2 / 32767.0;
    let a = ((bits >> 30) & 32767) as f32 * scale - one_div;
    let b2 = ((bits >> 15) & 32767) as f32 * scale - one_div;
    let c = (bits & 32767) as f32 * scale - one_div;
    let d = (1.0 - (a * a + b2 * b2 + c * c)).max(0.0).sqrt();
    match max_index {
        0 => [d, a, b2, c],
        1 => [a, d, b2, c],
        2 => [a, b2, d, c],
        _ => [a, b2, c, d],
    }
}

fn normalize(q: [f32; 4]) -> [f32; 4] {
    let l = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt();
    if l > 1e-8 {
        [q[0] / l, q[1] / l, q[2] / l, q[3] / l]
    } else {
        [0.0, 0.0, 0.0, 1.0]
    }
}

fn slerp(a: [f32; 4], b: [f32; 4], t: f32) -> [f32; 4] {
    let mut bb = b;
    let mut dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if dot < 0.0 {
        dot = -dot;
        bb = [-b[0], -b[1], -b[2], -b[3]];
    }
    if dot > 0.9995 {
        let r = [
            a[0] + t * (bb[0] - a[0]),
            a[1] + t * (bb[1] - a[1]),
            a[2] + t * (bb[2] - a[2]),
            a[3] + t * (bb[3] - a[3]),
        ];
        return normalize(r);
    }
    let theta = dot.clamp(-1.0, 1.0).acos();
    let sin = theta.sin();
    let s0 = ((1.0 - t) * theta).sin() / sin;
    let s1 = (t * theta).sin() / sin;
    [
        s0 * a[0] + s1 * bb[0],
        s0 * a[1] + s1 * bb[1],
        s0 * a[2] + s1 * bb[2],
        s0 * a[3] + s1 * bb[3],
    ]
}

fn lerp3(a: [f32; 3], b: [f32; 3], t: f32) -> [f32; 3] {
    [
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        a[2] + (b[2] - a[2]) * t,
    ]
}

impl Anm {
    pub fn parse(d: &[u8]) -> Result<Anm, String> {
        if d.len() < 12 || &d[0..8] != b"r3d2anmd" {
            return Err("anm: not r3d2anmd".into());
        }
        let version = u32_at(d, 8);
        match version {
            5 => Self::read_v5(d),
            4 => Self::read_v4(d),
            3 => Self::read_v3(d),
            v => Err(format!("anm: version {v}")),
        }
    }

    fn read_v5(d: &[u8]) -> Result<Anm, String> {
        let track_count = u32_at(d, 0x1c) as usize;
        let frame_count = u32_at(d, 0x20) as usize;
        let frame_duration = f32_at(d, 0x24);
        let joint_hashes_off = i32_at(d, 0x28);
        let vector_off = i32_at(d, 0x34);
        let quat_off = i32_at(d, 0x38);
        let frames_off = i32_at(d, 0x3c);
        if vector_off <= 0 || quat_off <= 0 || frames_off <= 0 || joint_hashes_off <= 0 {
            return Err("anm v5: missing section".into());
        }
        let fps = if frame_duration > 0.0 { 1.0 / frame_duration } else { 30.0 };
        let vec_count = ((quat_off - vector_off) / 12) as usize;
        let quat_count = ((joint_hashes_off - quat_off) / 6) as usize;
        let hash_count = ((frames_off - joint_hashes_off) / 4) as usize;

        let mut vec_pal = Vec::with_capacity(vec_count);
        for i in 0..vec_count {
            vec_pal.push(vec3(d, vector_off as usize + 12 + i * 12));
        }
        let mut quat_pal = Vec::with_capacity(quat_count);
        for i in 0..quat_count {
            let o = quat_off as usize + 12 + i * 6;
            quat_pal.push(normalize(decompress_quat6(&d[o..o + 6])));
        }
        let mut hashes = Vec::with_capacity(hash_count);
        for i in 0..hash_count {
            hashes.push(u32_at(d, joint_hashes_off as usize + 12 + i * 4));
        }

        let mut tracks: Vec<Vec<Frame>> = vec![Vec::with_capacity(frame_count); hashes.len().min(track_count)];
        let base = frames_off as usize + 12;
        for _f in 0..frame_count {
            for t in 0..track_count {
                let o = base + (_f * track_count + t) * 6;
                if o + 6 > d.len() {
                    return Err("anm v5: frames truncated".into());
                }
                if t < tracks.len() {
                    tracks[t].push(Frame {
                        t: u16_at(d, o),
                        s: u16_at(d, o + 2),
                        r: u16_at(d, o + 4),
                    });
                }
            }
        }
        let tracks = hashes.into_iter().zip(tracks).collect();
        Ok(Anm {
            version: 5,
            fps,
            duration: frame_count as f32 / fps,
            frame_count,
            vec_pal,
            quat_pal,
            tracks,
        })
    }

    fn read_v4(d: &[u8]) -> Result<Anm, String> {
        let track_count = u32_at(d, 0x1c) as usize;
        let frame_count = u32_at(d, 0x20) as usize;
        let frame_duration = f32_at(d, 0x24);
        let vector_off = i32_at(d, 0x34);
        let quat_off = i32_at(d, 0x38);
        let frames_off = i32_at(d, 0x3c);
        if vector_off <= 0 || quat_off <= 0 || frames_off <= 0 {
            return Err("anm v4: missing section".into());
        }
        let fps = if frame_duration > 0.0 { 1.0 / frame_duration } else { 30.0 };
        let vec_count = ((quat_off - vector_off) / 12) as usize;
        let quat_count = ((frames_off - quat_off) / 16) as usize;

        let mut vec_pal = Vec::with_capacity(vec_count);
        for i in 0..vec_count {
            vec_pal.push(vec3(d, vector_off as usize + 12 + i * 12));
        }
        let mut quat_pal = Vec::with_capacity(quat_count);
        for i in 0..quat_count {
            let o = quat_off as usize + 12 + i * 16;
            quat_pal.push(normalize(quat(d, o)));
        }

        let mut tracks: HashMap<u32, Vec<Frame>> = HashMap::new();
        let base = frames_off as usize + 12;
        for f in 0..frame_count {
            for t in 0..track_count {
                let o = base + (f * track_count + t) * 12;
                if o + 12 > d.len() {
                    return Err("anm v4: frames truncated".into());
                }
                let hash = u32_at(d, o);
                let fr = Frame {
                    t: u16_at(d, o + 4),
                    s: u16_at(d, o + 6),
                    r: u16_at(d, o + 8),
                };
                let e = tracks.entry(hash).or_default();
                while e.len() < f {
                    e.push(Frame::default());
                }
                e.push(fr);
            }
        }
        Ok(Anm {
            version: 4,
            fps,
            duration: frame_count as f32 / fps,
            frame_count,
            vec_pal,
            quat_pal,
            tracks,
        })
    }

    fn read_v3(d: &[u8]) -> Result<Anm, String> {
        let _skeleton_id = u32_at(d, 12);
        let track_count = u32_at(d, 16) as usize;
        let frame_count = u32_at(d, 20) as usize;
        let fps = u32_at(d, 24) as f32;
        let fps = if fps > 0.0 { fps } else { 30.0 };
        let mut vec_pal = vec![[1.0, 1.0, 1.0]];
        let mut quat_pal: Vec<[f32; 4]> = Vec::new();
        let mut tracks: HashMap<u32, Vec<Frame>> = HashMap::new();
        let mut o = 28;
        for _ in 0..track_count {
            if o + 36 > d.len() {
                break;
            }
            let mut name_end = o;
            while name_end < o + 32 && d[name_end] != 0 {
                name_end += 1;
            }
            let name = String::from_utf8_lossy(&d[o..name_end]).to_string();
            o += 32;
            let _flags = u32_at(d, o);
            o += 4;
            let hash = elf_hash(&name) as u32;
            let mut frames = Vec::with_capacity(frame_count);
            for _ in 0..frame_count {
                if o + 28 > d.len() {
                    break;
                }
                let q = quat(d, o);
                let t = vec3(d, o + 16);
                o += 28;
                let rid = quat_pal.len() as u16;
                quat_pal.push(q);
                let tid = vec_pal.len() as u16;
                vec_pal.push(t);
                frames.push(Frame { t: tid, s: 0, r: rid });
            }
            tracks.insert(hash, frames);
        }
        Ok(Anm {
            version: 3,
            fps,
            duration: frame_count as f32 / fps,
            frame_count,
            vec_pal,
            quat_pal,
            tracks,
        })
    }

    /// Per-joint (rotation, translation, scale) at time `t`, keyed by joint hash.
    pub fn evaluate(&self, t: f32) -> HashMap<u32, ([f32; 4], [f32; 3], [f32; 3])> {
        let mut out = HashMap::new();
        if self.frame_count == 0 {
            return out;
        }
        let t = t.clamp(0.0, self.duration.max(0.0));
        let pos = t * self.fps;
        let a = (pos.floor() as usize).min(self.frame_count - 1);
        let b = (a + 1).min(self.frame_count - 1);
        let f = pos.fract();
        for (hash, frames) in &self.tracks {
            let fa = match frames.get(a) {
                Some(v) => *v,
                None => continue,
            };
            let fb = frames.get(b).copied().unwrap_or(fa);
            let ra = self.quat_pal.get(fa.r as usize).copied().unwrap_or([0.0, 0.0, 0.0, 1.0]);
            let rb = self.quat_pal.get(fb.r as usize).copied().unwrap_or(ra);
            let ta = self.vec_pal.get(fa.t as usize).copied().unwrap_or([0.0; 3]);
            let tb = self.vec_pal.get(fb.t as usize).copied().unwrap_or(ta);
            let sa = self.vec_pal.get(fa.s as usize).copied().unwrap_or([1.0; 3]);
            let sb = self.vec_pal.get(fb.s as usize).copied().unwrap_or(sa);
            let r = if a == b || f < 1e-5 { ra } else { slerp(ra, rb, f) };
            let tr = if a == b || f < 1e-5 { ta } else { lerp3(ta, tb, f) };
            let sc = if a == b || f < 1e-5 { sa } else { lerp3(sa, sb, f) };
            out.insert(*hash, (r, tr, sc));
        }
        out
    }
}

fn elf_hash(s: &str) -> u32 {
    let mut h: u32 = 0;
    for b in s.bytes() {
        h = (h << 4).wrapping_add(b as u32);
        let g = h & 0xF000_0000;
        if g != 0 {
            h ^= g >> 24;
        }
        h &= !g;
    }
    h
}

