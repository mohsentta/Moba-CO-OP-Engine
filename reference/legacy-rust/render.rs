//! Software rasterizer for `.skn` meshes.
//!
//! No GPU crate is available in this offline build, so this is a small
//! z-buffered triangle rasterizer with smooth normals, a key light, and a
//! checkerboard floor. It writes a 24-bit BMP. The point is to prove the
//! asset pipeline end to end (WAD -> SKN -> pixels), not to reproduce Riot's
//! renderer.

use crate::skn::Skn;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::Path;

#[derive(Clone, Copy, Debug)]
struct V3 {
    x: f32,
    y: f32,
    z: f32,
}

impl V3 {
    fn new(x: f32, y: f32, z: f32) -> V3 {
        V3 { x, y, z }
    }
    fn sub(a: V3, b: V3) -> V3 {
        V3::new(a.x - b.x, a.y - b.y, a.z - b.z)
    }
    fn add(a: V3, b: V3) -> V3 {
        V3::new(a.x + b.x, a.y + b.y, a.z + b.z)
    }
    fn mul(a: V3, s: f32) -> V3 {
        V3::new(a.x * s, a.y * s, a.z * s)
    }
    fn dot(a: V3, b: V3) -> f32 {
        a.x * b.x + a.y * b.y + a.z * b.z
    }
    fn cross(a: V3, b: V3) -> V3 {
        V3::new(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        )
    }
    fn norm(a: V3) -> V3 {
        let l = (a.x * a.x + a.y * a.y + a.z * a.z).sqrt();
        if l > 1e-9 {
            V3::new(a.x / l, a.y / l, a.z / l)
        } else {
            V3::new(0.0, 1.0, 0.0)
        }
    }
}

/// Column-major 4x4 matrix.
type M4 = [[f32; 4]; 4];

fn mat_mul(a: M4, b: M4) -> M4 {
    let mut o = [[0f32; 4]; 4];
    for r in 0..4 {
        for c in 0..4 {
            let mut s = 0.0;
            for k in 0..4 {
                s += a[k][r] * b[c][k];
            }
            o[c][r] = s;
        }
    }
    o
}

fn transform(m: M4, p: [f32; 4]) -> [f32; 4] {
    let mut o = [0f32; 4];
    for r in 0..4 {
        o[r] = m[0][r] * p[0] + m[1][r] * p[1] + m[2][r] * p[2] + m[3][r] * p[3];
    }
    o
}

fn perspective(fov_y: f32, aspect: f32, near: f32, far: f32) -> M4 {
    let f = 1.0 / (fov_y / 2.0).tan();
    let mut m = [[0f32; 4]; 4];
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][2] = (far + near) / (near - far);
    m[2][3] = -1.0;
    m[3][2] = (2.0 * far * near) / (near - far);
    m
}

fn look_at(eye: V3, target: V3, up: V3) -> M4 {
    let f = V3::norm(V3::sub(target, eye));
    let s = V3::norm(V3::cross(f, up));
    let u = V3::cross(s, f);
    let mut m = [[0f32; 4]; 4];
    m[0][0] = s.x;
    m[1][0] = s.y;
    m[2][0] = s.z;
    m[0][1] = u.x;
    m[1][1] = u.y;
    m[2][1] = u.z;
    m[0][2] = -f.x;
    m[1][2] = -f.y;
    m[2][2] = -f.z;
    m[3][0] = -V3::dot(s, eye);
    m[3][1] = -V3::dot(u, eye);
    m[3][2] = V3::dot(f, eye);
    m[3][3] = 1.0;
    m
}

const MATERIAL_COLORS: [[f32; 3]; 10] = [
    [0.86, 0.62, 0.45], // skin
    [0.20, 0.22, 0.30], // cloth
    [0.72, 0.34, 0.55], // accent
    [0.90, 0.85, 0.75], // trim
    [0.30, 0.55, 0.65],
    [0.55, 0.30, 0.25],
    [0.75, 0.75, 0.80],
    [0.25, 0.40, 0.30],
    [0.95, 0.80, 0.35],
    [0.40, 0.30, 0.55],
];

struct Framebuffer {
    w: usize,
    h: usize,
    color: Vec<[f32; 3]>,
    depth: Vec<f32>,
}

impl Framebuffer {
    fn new(w: usize, h: usize) -> Framebuffer {
        Framebuffer {
            w,
            h,
            color: vec![[0.0, 0.0, 0.0]; w * h],
            depth: vec![f32::INFINITY; w * h],
        }
    }

    fn shade(&mut self, x: usize, y: usize, z: f32, c: [f32; 3]) {
        let i = y * self.w + x;
        if z <= self.depth[i] {
            self.depth[i] = z;
            self.color[i] = c;
        }
    }
}

/// Render `mesh` to `out.bmp`/`out.png`.
pub fn render_frame(mesh: &Skn, width: u32, height: u32, hud: Option<f32>, ss: usize) -> Result<Vec<u8>, String> {
    let w = (width as usize).max(16) * ss;
    let h = (height as usize).max(16) * ss;
    let mut fb = Framebuffer::new(w, h);

    // Background: vertical gradient, so the silhouette reads.
    for y in 0..h {
        let t = y as f32 / h as f32;
        let c = [
            0.055 * (1.0 - t) + 0.012 * t,
            0.070 * (1.0 - t) + 0.016 * t,
            0.105 * (1.0 - t) + 0.028 * t,
        ];
        for x in 0..w {
            fb.color[y * w + x] = c;
        }
    }

    let (mn, mx) = mesh.bounds();
    let center = V3::new(
        (mn[0] + mx[0]) * 0.5,
        (mn[1] + mx[1]) * 0.5,
        (mn[2] + mx[2]) * 0.5,
    );
    let ext = V3::new(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
    let radius = ext.x.max(ext.y).max(ext.z).max(1.0) * 0.5;
    println!(
        "  extent {:.1} x {:.1} x {:.1} (radius {:.1})",
        ext.x, ext.y, ext.z, radius
    );

    let scale = 1.0 / radius;
    let floor_y = (mn[1] - center.y) * scale - 0.002;
    // Orbit around the model: 3/4 front view, slightly above.
    let (yaw, pitch) = (0.55f32, 0.20f32);
    let dist = 3.1f32;
    let eye = V3::new(
        0.0,
        dist * pitch.sin(),
        dist * pitch.cos(),
    );
    let target = V3::new(0.0, 0.0, 0.0);

    // Rotate the model and move it to the origin.
    let (sy, cy) = (yaw.sin(), yaw.cos());
    let model = |p: [f32; 3]| -> V3 {
        let x = (p[0] - center.x) * scale;
        let y = (p[1] - center.y) * scale;
        let z = (p[2] - center.z) * scale;
        V3::new(x * cy + z * sy, y, -x * sy + z * cy)
    };

    let view = look_at(eye, target, V3::new(0.0, 1.0, 0.0));
    let proj = perspective(0.70, w as f32 / h as f32, 0.05, 100.0);
    let vp = mat_mul(proj, view);

    // Floor quad at the model's feet.
    let quad = [
        V3::new(-9.0, floor_y, -9.0),
        V3::new(9.0, floor_y, -9.0),
        V3::new(9.0, floor_y, 9.0),
        V3::new(-9.0, floor_y, 9.0),
    ];
    let light = V3::norm(V3::new(-0.45, 0.8, 0.55));

    // --- floor (checkerboard) ---
    for tri in [[0usize, 1, 2], [0, 2, 3]] {
        let p = [quad[tri[0]], quad[tri[1]], quad[tri[2]]];
        raster_tri(
            &mut fb,
            &vp,
            &p,
            &p,
            [1.0; 3],
            0.0,
            &light,
            Some((0.20, 0.23, 0.30)),
        );
    }

    let mesh_tris = mesh.triangles();
    println!("  triangles {}", mesh_tris.len());
    let mut pos = Vec::with_capacity(mesh.vertices.len());
    let mut nrm = Vec::with_capacity(mesh.vertices.len());
    let mut col = Vec::with_capacity(mesh.vertices.len());
    for v in &mesh.vertices {
        pos.push(model(v.position));
        let n = V3::new(v.normal[0], v.normal[1], v.normal[2]);
        let n = if n.x * n.x + n.y * n.y + n.z * n.z > 1e-12 {
            V3::norm(V3::new(n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy))
        } else {
            V3::new(0.0, 1.0, 0.0)
        };
        nrm.push(n);
    }
    // Material colour per index: use the material ranges.
    let mut mat_of = vec![0usize; mesh.vertices.len()];
    for (mi, m) in mesh.materials.iter().enumerate() {
        let s = m.start_vertex as usize;
        for k in s..(s + m.num_vertices as usize).min(mesh.vertices.len()) {
            mat_of[k] = mi;
        }
    }
    for &m in &mat_of {
        col.push(MATERIAL_COLORS[m % MATERIAL_COLORS.len()]);
    }

    for t in &mesh_tris {
        let p = [pos[t[0]], pos[t[1]], pos[t[2]]];
        let n = [nrm[t[0]], nrm[t[1]], nrm[t[2]]];
        let c = [
            (col[t[0]][0] + col[t[1]][0] + col[t[2]][0]) / 3.0,
            (col[t[0]][1] + col[t[1]][1] + col[t[2]][1]) / 3.0,
            (col[t[0]][2] + col[t[1]][2] + col[t[2]][2]) / 3.0,
        ];
        raster_tri(&mut fb, &vp, &p, &n, c, 0.0, &light, None);
    }

    // --- HUD: replay timeline bar ---
    if let Some(frac) = hud {
        let frac = frac.clamp(0.0, 1.0);
        let pad = 24 * ss;
        let y0 = h - 20 * ss;
        let bar_h = 4 * ss;
        for y in y0 - 2 * ss..y0 + bar_h + 2 * ss {
            for x in pad..w - pad {
                fb.color[y * w + x] = [0.06, 0.07, 0.09];
            }
        }
        let fill_end = pad + ((w - 2 * pad) as f32 * frac) as usize;
        for y in y0..y0 + bar_h {
            for x in pad..fill_end {
                fb.color[y * w + x] = [0.80, 0.55, 0.20];
            }
        }
        // Playhead tick.
        for y in y0 - 2 * ss..y0 + bar_h + 2 * ss {
            for x in fill_end.saturating_sub(ss)..(fill_end + ss).min(w - pad) {
                fb.color[y * w + x] = [0.95, 0.85, 0.55];
            }
        }
    }

    // Downsample and encode.
    let (ow, oh) = (w / ss, h / ss);
    let mut rgb = vec![0u8; ow * oh * 3];
    for y in 0..oh {
        for x in 0..ow {
            let mut acc = [0f32; 3];
            for dy in 0..ss {
                for dx in 0..ss {
                    let c = fb.color[(y * ss + dy) * w + (x * ss + dx)];
                    acc[0] += c[0];
                    acc[1] += c[1];
                    acc[2] += c[2];
                }
            }
            let n = (ss * ss) as f32;
            let i = (y * ow + x) * 3;
            rgb[i] = to_srgb(acc[0] / n);
            rgb[i + 1] = to_srgb(acc[1] / n);
            rgb[i + 2] = to_srgb(acc[2] / n);
        }
    }
    Ok(rgb)
}

/// Render and write a still image.
pub fn render_mesh(
    mesh: &Skn,
    out: &str,
    width: u32,
    height: u32,
    hud: Option<f32>,
) -> Result<(), String> {
    let rgb = render_frame(mesh, width, height, hud, 2)?;
    write_image(out, width, height, &rgb)
}

fn crc32(data: &[u8]) -> u32 {
    let mut c = 0xffff_ffffu32;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xedb8_8320 } else { c >> 1 };
        }
    }
    !c
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in data {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

/// Minimal PNG encoder: one zlib stream made of stored (uncompressed) deflate
/// blocks. Larger than a real compression, but exact and dependency-free.
fn write_png(path: &str, w: u32, h: u32, rgb: &[u8]) -> Result<(), String> {
    let mut raw = Vec::with_capacity((w as usize * 3 + 1) * h as usize);
    for y in 0..h as usize {
        raw.push(0); // filter: none
        raw.extend_from_slice(&rgb[y * w as usize * 3..(y + 1) * w as usize * 3]);
    }
    let mut z = vec![0x78, 0x01];
    let mut i = 0usize;
    while i < raw.len() {
        let n = (raw.len() - i).min(65535);
        let last = i + n >= raw.len();
        z.push(if last { 1 } else { 0 });
        z.extend_from_slice(&(n as u16).to_le_bytes());
        z.extend_from_slice(&(!(n as u16)).to_le_bytes());
        z.extend_from_slice(&raw[i..i + n]);
        i += n;
    }
    z.extend_from_slice(&adler32(&raw).to_be_bytes());

    let mut out = Vec::new();
    out.extend_from_slice(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]);
    let push_chunk = |out: &mut Vec<u8>, ty: &[u8; 4], data: &[u8]| {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        out.extend_from_slice(ty);
        out.extend_from_slice(data);
        let mut crc_in = Vec::with_capacity(4 + data.len());
        crc_in.extend_from_slice(ty);
        crc_in.extend_from_slice(data);
        out.extend_from_slice(&crc32(&crc_in).to_be_bytes());
    };
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]); // 8-bit RGB
    push_chunk(&mut out, b"IHDR", &ihdr);
    push_chunk(&mut out, b"IDAT", &z);
    push_chunk(&mut out, b"IEND", &[]);
    std::fs::write(path, out).map_err(|e| format!("write {path}: {e}"))?;
    Ok(())
}

fn write_image(path: &str, w: u32, h: u32, rgb: &[u8]) -> Result<(), String> {
    if path.to_ascii_lowercase().ends_with(".png") {
        write_png(path, w, h, rgb)
    } else {
        write_bmp(path, w, h, rgb)
    }
}

fn to_srgb(v: f32) -> u8 {
    let v = v.clamp(0.0, 1.0).powf(1.0 / 2.2);
    (v * 255.0 + 0.5) as u8
}

#[allow(clippy::too_many_arguments)]
fn raster_tri(
    fb: &mut Framebuffer,
    vp: &M4,
    p: &[V3; 3],
    n: &[V3; 3],
    base: [f32; 3],
    _unused: f32,
    light: &V3,
    flat: Option<(f32, f32, f32)>,
) {
    let mut clip = [[0f32; 4]; 3];
    for i in 0..3 {
        clip[i] = transform(*vp, [p[i].x, p[i].y, p[i].z, 1.0]);
        if clip[i][3] <= 0.001 {
            return;
        }
    }
    let mut sp = [[0f32; 3]; 3];
    for i in 0..3 {
        let inv = 1.0 / clip[i][3];
        sp[i][0] = (clip[i][0] * inv * 0.5 + 0.5) * fb.w as f32;
        sp[i][1] = (1.0 - (clip[i][1] * inv * 0.5 + 0.5)) * fb.h as f32;
        sp[i][2] = clip[i][2] * inv;
    }
    let area = (sp[1][0] - sp[0][0]) * (sp[2][1] - sp[0][1])
        - (sp[2][0] - sp[0][0]) * (sp[1][1] - sp[0][1]);
    if area.abs() < 1e-9 {
        return;
    }
    let minx = sp.iter().map(|s| s[0]).fold(f32::INFINITY, f32::min).floor().max(0.0) as usize;
    let maxx = (sp.iter().map(|s| s[0]).fold(f32::NEG_INFINITY, f32::max).ceil() as i64)
        .min(fb.w as i64 - 1)
        .max(0) as usize;
    let miny = sp.iter().map(|s| s[1]).fold(f32::INFINITY, f32::min).floor().max(0.0) as usize;
    let maxy = (sp.iter().map(|s| s[1]).fold(f32::NEG_INFINITY, f32::max).ceil() as i64)
        .min(fb.h as i64 - 1)
        .max(0) as usize;
    if minx > maxx || miny > maxy {
        return;
    }

    let inv_area = 1.0 / area;
    for y in miny..=maxy {
        for x in minx..=maxx {
            let px = x as f32 + 0.5;
            let py = y as f32 + 0.5;
            let w0 = ((sp[1][0] - px) * (sp[2][1] - py) - (sp[2][0] - px) * (sp[1][1] - py)) * inv_area;
            let w1 = ((sp[2][0] - px) * (sp[0][1] - py) - (sp[0][0] - px) * (sp[2][1] - py)) * inv_area;
            let w2 = 1.0 - w0 - w1;
            if w0 < 0.0 || w1 < 0.0 || w2 < 0.0 {
                continue;
            }
            let z = w0 * sp[0][2] + w1 * sp[1][2] + w2 * sp[2][2];
            let mut color = if let Some(c) = flat {
                // Checkerboard on the floor, in world space.
                let wx = w0 * p[0].x + w1 * p[1].x + w2 * p[2].x;
                let wz = w0 * p[0].z + w1 * p[1].z + w2 * p[2].z;
                let checker = ((wx * 1.6).floor() as i32 + (wz * 1.6).floor() as i32) & 1;
                let k = if checker == 0 { 1.0 } else { 0.72 };
                [c.0 * k, c.1 * k, c.2 * k]
            } else {
                let nn = V3::norm(V3::add(
                    V3::add(V3::mul(n[0], w0), V3::mul(n[1], w1)),
                    V3::mul(n[2], w2),
                ));
                let diff = V3::dot(nn, *light).max(0.0);
                let rim = (1.0 - V3::dot(nn, V3::new(0.0, 0.0, 1.0)).abs()).powf(2.0) * 0.15;
                let k = 0.28 + 0.85 * diff + rim;
                [base[0] * k, base[1] * k, base[2] * k]
            };
            // Slight distance fog toward the background.
            let fog = (z * 0.08).clamp(0.0, 0.45);
            color[0] = color[0] * (1.0 - fog) + 0.03 * fog;
            color[1] = color[1] * (1.0 - fog) + 0.04 * fog;
            color[2] = color[2] * (1.0 - fog) + 0.06 * fog;
            fb.shade(x, y, z, color);
        }
    }
}

fn write_bmp(path: &str, w: u32, h: u32, rgb: &[u8]) -> Result<(), String> {
    let row = (w as usize * 3).div_ceil(4) * 4;
    let size = 54 + row * h as usize;
    let f = File::create(Path::new(path)).map_err(|e| format!("create {path}: {e}"))?;
    let mut w_ = BufWriter::new(f);
    let mut hdr = [0u8; 54];
    hdr[0] = b'B';
    hdr[1] = b'M';
    hdr[2..6].copy_from_slice(&(size as u32).to_le_bytes());
    hdr[10..14].copy_from_slice(&54u32.to_le_bytes());
    hdr[14..18].copy_from_slice(&40u32.to_le_bytes());
    hdr[18..22].copy_from_slice(&w.to_le_bytes());
    hdr[22..26].copy_from_slice(&(h as i32).to_le_bytes());
    hdr[26..28].copy_from_slice(&1u16.to_le_bytes());
    hdr[28..30].copy_from_slice(&24u16.to_le_bytes());
    hdr[34..38].copy_from_slice(&((row * h as usize) as u32).to_le_bytes());
    w_.write_all(&hdr).map_err(|e| e.to_string())?;
    let pad = row - w as usize * 3;
    let zeros = [0u8; 3];
    for y in (0..h as usize).rev() {
        for x in 0..w as usize {
            let i = (y * w as usize + x) * 3;
            // BMP stores BGR
            w_.write_all(&[rgb[i + 2], rgb[i + 1], rgb[i]])
                .map_err(|e| e.to_string())?;
        }
        if pad > 0 {
            w_.write_all(&zeros[..pad]).map_err(|e| e.to_string())?;
        }
    }
    w_.flush().map_err(|e| e.to_string())?;
    Ok(())
}


