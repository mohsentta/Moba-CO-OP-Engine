//! `.skl` skeleton parser (Riot `r3d2` rig).
//!
//! Layout ported from LeagueToolkit (`ltk_anim::rig::read` and
//! `ltk_anim::joint::read`):
//!
//! ```text
//! u32 file_size
//! u32 format_token  0x22FD4FC3
//! u32 version       (0)
//! u16 flags
//! u16 joint_count
//! u32 influences_count
//! i32 joints_off, joint_indices_off, influences_off,
//!     name_off, asset_name_off, bone_names_off
//! i32 extension[5]
//!
//! joint (100 bytes):
//!   u16 flags, i16 id, i16 parent, i16 pad, u32 name_hash, f32 radius
//!   f32[3] local_translation, f32[3] local_scale, f32[4] local_rotation
//!   f32[3] bind_translation, f32[3] bind_scale, f32[4] bind_rotation
//!   i32 name_off (relative to this field)
//! ```

use std::collections::HashMap;

pub const TOKEN: u32 = 0x22FD_4FC3;

#[derive(Clone, Debug)]
pub struct Joint {
    pub name: String,
    pub flags: u16,
    pub id: i16,
    pub parent: i16,
    pub radius: f32,
    pub hash: u32,
    pub t: [f32; 3],
    pub s: [f32; 3],
    pub q: [f32; 4],
    pub ib_t: [f32; 3],
    pub ib_s: [f32; 3],
    pub ib_q: [f32; 4],
}

#[derive(Debug)]
pub struct Skl {
    pub name: String,
    pub asset_name: String,
    pub joints: Vec<Joint>,
    pub influences: Vec<i16>,
    pub hash_to_joint: HashMap<u32, usize>,
}

fn u16_at(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
fn i16_at(d: &[u8], o: usize) -> i16 {
    i16::from_le_bytes([d[o], d[o + 1]])
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
fn cstr(d: &[u8], o: usize) -> String {
    if o == 0 || o >= d.len() {
        return String::new();
    }
    let mut end = o;
    while end < d.len() && d[end] != 0 {
        end += 1;
    }
    String::from_utf8_lossy(&d[o..end]).to_string()
}

impl Skl {
    pub fn parse(d: &[u8]) -> Result<Skl, String> {
        if d.len() < 0x40 {
            return Err("skl: too small".into());
        }
        if u32_at(d, 4) != TOKEN {
            return Err(format!("skl: bad token 0x{:08x}", u32_at(d, 4)));
        }
        let version = u32_at(d, 8);
        if version != 0 {
            return Err(format!("skl: version {version}"));
        }
        let joint_count = u16_at(d, 0x0e) as usize;
        let influences_count = u32_at(d, 0x10) as usize;
        let joints_off = i32_at(d, 0x14);
        let indices_off = i32_at(d, 0x18);
        let influences_off = i32_at(d, 0x1c);
        let name_off = i32_at(d, 0x20);
        let asset_off = i32_at(d, 0x24);

        let mut joints = Vec::with_capacity(joint_count);
        if joints_off > 0 {
            let base = joints_off as usize;
            for i in 0..joint_count {
                let o = base + i * 100;
                if o + 100 > d.len() {
                    return Err("skl: joint out of range".into());
                }
                let name_off_rel = i32_at(d, o + 96);
                let np = o as i64 + 96 + name_off_rel as i64;
                let name = if np >= 0 { cstr(d, np as usize) } else { String::new() };
                joints.push(Joint {
                    name,
                    flags: u16_at(d, o),
                    id: i16_at(d, o + 2),
                    parent: i16_at(d, o + 4),
                    radius: f32_at(d, o + 12),
                    hash: u32_at(d, o + 8),
                    t: vec3(d, o + 16),
                    s: vec3(d, o + 28),
                    q: quat(d, o + 40),
                    ib_t: vec3(d, o + 56),
                    ib_s: vec3(d, o + 68),
                    ib_q: quat(d, o + 80),
                });
            }
        }

        let mut hash_to_joint = HashMap::new();
        if indices_off > 0 {
            let base = indices_off as usize;
            for i in 0..joint_count {
                let o = base + i * 8;
                if o + 8 > d.len() {
                    break;
                }
                let id = i16_at(d, o);
                let hash = u32_at(d, o + 4);
                if id >= 0 && (id as usize) < joints.len() {
                    hash_to_joint.insert(hash, id as usize);
                }
            }
        }
        if hash_to_joint.is_empty() {
            for (i, j) in joints.iter().enumerate() {
                if j.hash != 0 {
                    hash_to_joint.insert(j.hash, i);
                }
            }
        }
        let mut influences = Vec::new();
        if influences_off > 0 {
            let base = influences_off as usize;
            for i in 0..influences_count {
                let o = base + i * 2;
                if o + 2 > d.len() {
                    break;
                }
                influences.push(i16_at(d, o));
            }
        }

        Ok(Skl {
            name: cstr(d, name_off as usize),
            asset_name: cstr(d, asset_off as usize),
            joints,
            influences,
            hash_to_joint,
        })
    }

    /// Global bind-space matrix of each joint (parent chain of local TRS).
    pub fn bind_globals(&self) -> Vec<[[f32; 4]; 4]> {
        let n = self.joints.len();
        let mut out = vec![identity(); n];
        let mut done = vec![false; n];
        fn build(
            i: usize,
            joints: &[Joint],
            out: &mut Vec<[[f32; 4]; 4]>,
            done: &mut Vec<bool>,
        ) -> [[f32; 4]; 4] {
            if done[i] {
                return out[i];
            }
            let j = &joints[i];
            let local = trs(j.t, j.s, j.q);
            let p = j.parent;
            let g = if p >= 0 && (p as usize) < joints.len() {
                let pg = build(p as usize, joints, out, done);
                mul(local, pg)
            } else {
                local
            };
            out[i] = g;
            done[i] = true;
            g
        }
        for i in 0..n {
            build(i, &self.joints, &mut out, &mut done);
        }
        out
    }
}

pub fn identity() -> [[f32; 4]; 4] {
    [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]
}

pub fn trs(t: [f32; 3], s: [f32; 3], q: [f32; 4]) -> [[f32; 4]; 4] {
    let (x, y, z, w) = (q[0], q[1], q[2], q[3]);
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, yy, zz) = (x * x2, y * y2, z * z2);
    let (xy, xz, yz) = (x * y2, x * z2, y * z2);
    let (wx, wy, wz) = (w * x2, w * y2, w * z2);
    [
        [(1.0 - (yy + zz)) * s[0], (xy + wz) * s[0], (xz - wy) * s[0], 0.0],
        [(xy - wz) * s[1], (1.0 - (xx + zz)) * s[1], (yz + wx) * s[1], 0.0],
        [(xz + wy) * s[2], (yz - wx) * s[2], (1.0 - (xx + yy)) * s[2], 0.0],
        [t[0], t[1], t[2], 1.0],
    ]
}

pub fn mul(a: [[f32; 4]; 4], b: [[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let mut m = [[0.0f32; 4]; 4];
    for r in 0..4 {
        for c in 0..4 {
            m[r][c] = (0..4).map(|k| a[r][k] * b[k][c]).sum();
        }
    }
    m
}

pub fn xform(m: [[f32; 4]; 4], p: [f32; 3]) -> [f32; 3] {
    [
        m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
        m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
        m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3],
    ]
}

/// Inverse of an affine row-vector matrix (translation in row 3).
pub fn invert(m: [[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let a = m[0][0];
    let b = m[0][1];
    let c = m[0][2];
    let d = m[1][0];
    let e = m[1][1];
    let f = m[1][2];
    let g = m[2][0];
    let h = m[2][1];
    let i = m[2][2];
    let det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    let inv = 1.0 / det;
    let r = [
        [(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv],
        [(f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv],
        [(d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv],
    ];
    let t = [m[3][0], m[3][1], m[3][2]];
    let ti = [
        -(t[0] * r[0][0] + t[1] * r[1][0] + t[2] * r[2][0]),
        -(t[0] * r[0][1] + t[1] * r[1][1] + t[2] * r[2][1]),
        -(t[0] * r[0][2] + t[1] * r[1][2] + t[2] * r[2][2]),
    ];
    [
        [r[0][0], r[0][1], r[0][2], 0.0],
        [r[1][0], r[1][1], r[1][2], 0.0],
        [r[2][0], r[2][1], r[2][2], 0.0],
        [ti[0], ti[1], ti[2], 1.0],
    ]
}

pub fn xform_dir(m: [[f32; 4]; 4], p: [f32; 3]) -> [f32; 3] {
    [
        m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2],
        m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2],
        m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2],
    ]
}







