//! Animated Ahri: parse SKL + SKN + ANM from the game WAD, skin on the CPU,
//! render a frame.
//!
//! `anim <wad> <skl path> <skn path> <out.png> [--bind] [--t=SECONDS] [--anm=INDEX]`

#[path = "../wad.rs"]
mod wad;
#[path = "../skn.rs"]
mod skn;
#[path = "../skl.rs"]
mod skl;
#[path = "../anm.rs"]
mod anm;
#[path = "../render.rs"]
mod render;

use skl::{mul, trs, xform, xform_dir};

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("usage: anim <wad> <skl> <skn> <out.png> [--bind] [--t=S] [--anm=N]");
        std::process::exit(2);
    }
    let w = wad::Wad::open(&a[1]).expect("open wad");
    let skl_d = w.read_path(&a[2]).expect("read skl");
    let skn_d = w.read_path(&a[3]).expect("read skn");
    let rig = skl::Skl::parse(&skl_d).expect("parse skl");
    let mesh = skn::Skn::parse(&skn_d).expect("parse skn");
    println!(
        "rig '{}' joints={} mesh verts={} tris={}",
        rig.name,
        rig.joints.len(),
        mesh.vertices.len(),
        mesh.indices.len() / 3
    );

    let bind_only = a.iter().any(|s| s == "--bind");
    let time: f32 = a
        .iter()
        .find_map(|s| s.strip_prefix("--t="))
        .map(|v| v.parse().unwrap_or(0.0))
        .unwrap_or(0.0);
    let anm_index: usize = a
        .iter()
        .find_map(|s| s.strip_prefix("--anm="))
        .map(|v| v.parse().unwrap_or(0))
        .unwrap_or(0);

    // Pick the animation whose joint hashes overlap the rig the most.
    let mut picked: Option<(u64, Vec<u8>, anm::Anm)> = None;
    let mut seen = 0usize;
    let mut best_match = 0usize;
    if !bind_only {
        for e in &w.entries {
            if let Ok(d) = w.read_chunk(e) {
                if d.len() >= 12 && &d[0..8] == b"r3d2anmd" {
                    seen += 1;
                    if let Ok(x) = anm::Anm::parse(&d) {
                        let m = x
                            .tracks
                            .keys()
                            .filter(|h| rig.hash_to_joint.contains_key(h))
                            .count();
                        if m > best_match {
                            best_match = m;
                            picked = Some((e.hash, d.clone(), x));
                        }
                    }
                }
            }
        }
        println!("animations: {seen}, best joint match: {best_match}/{}", rig.joints.len());
    }
    let anim = if let Some((hash, d, x)) = picked {
        println!(
            "using anm hash=0x{hash:016x} size={} version={}",
            d.len(),
            x.version
        );
        println!(
            "  fps={:.1} frames={} duration={:.2}s joints={}",
            x.fps,
            x.frame_count,
            x.duration,
            x.tracks.len()
        );
        Some(x)
    } else {
        None
    };

    // Local transforms: animation when available, bind TRS otherwise.
    let n = rig.joints.len();
    let eval = anim.as_ref().map(|x| x.evaluate(time));
    if let Some(m) = &eval {
        let mut shown = 0;
        for (h, (r, tt, ss)) in m.iter() {
            println!("  track 0x{h:08x} q={:?} t={:?} s={:?}", r, tt, ss);
            shown += 1;
            if shown >= 4 { break; }
        }
    }
    let mut globals = vec![skl::identity(); n];
    let mut done = vec![false; n];
    fn build(
        i: usize,
        rig: &skl::Skl,
        eval: &Option<std::collections::HashMap<u32, ([f32; 4], [f32; 3], [f32; 3])>>,
        out: &mut Vec<[[f32; 4]; 4]>,
        done: &mut Vec<bool>,
    ) -> [[f32; 4]; 4] {
        if done[i] {
            return out[i];
        }
        let j = &rig.joints[i];
        let (t, s, q) = match eval.as_ref().and_then(|m| m.get(&j.hash)) {
            Some((r, tt, ss)) => (*tt, *ss, *r),
            None => (j.t, j.s, j.q),
        };
        let local = trs(t, s, q);
        let g = if j.parent >= 0 && (j.parent as usize) < rig.joints.len() {
            let pg = build(j.parent as usize, rig, eval, out, done);
            mul(local, pg)
        } else {
            local
        };
        out[i] = g;
        done[i] = true;
        g
    }
    for i in 0..n {
        build(i, &rig, &eval, &mut globals, &mut done);
    }

    // skin[j] = inverse_bind * global
    let mut skin = vec![skl::identity(); n];
    let bind_g = rig.bind_globals();
    for i in 0..n {
        skin[i] = mul(skl::invert(bind_g[i]), globals[i]);
    }

    let mut posed = skn::Skn {
        major: mesh.major,
        minor: mesh.minor,
        materials: mesh.materials.clone(),
        indices: mesh.indices.clone(),
        vertices: mesh.vertices.clone(),
        vertex_stride: mesh.vertex_stride,
        bbox: mesh.bbox,
    };
    // SKN bone indices index the SKL influences table, whose values are joint ids.
    let mut id_to_joint = std::collections::HashMap::new();
    for (i, j) in rig.joints.iter().enumerate() {
        id_to_joint.insert(j.id, i);
    }
    // SKN bone indices index the SKL influences table; its values are joint ids.
    let influences_dbg = rig.influences.len();
    println!("influences: {influences_dbg}");
    let mut remap: Vec<usize> = rig
        .influences
        .iter()
        .map(|id| id_to_joint.get(id).copied().unwrap_or(usize::MAX))
        .collect();
    remap.resize(256, usize::MAX);
    let hard = a.iter().any(|s| s == "--hard");
    for (vi, v) in posed.vertices.iter().enumerate() {
        if vi < 3 {
            println!("v{vi} pos={:?} bones={:?} w={:?}", v.position, v.bone_indices, v.weights);
        }
    }
    for v in &mut posed.vertices {
        let mut p = [0.0f32; 3];
        let mut nrm = [0.0f32; 3];
        let mut wsum = 0.0f32;
        for k in 0..4 {
            let wgt = v.weights[k];
            if wgt <= 0.0 {
                continue;
            }
            let bi = v.bone_indices[k] as usize;
            let idx = *remap.get(bi).unwrap_or(&usize::MAX);
            if idx >= n {
                continue;
            }
            let sp = xform(skin[idx], v.position);
            p[0] += sp[0] * wgt;
            p[1] += sp[1] * wgt;
            p[2] += sp[2] * wgt;
            let sn = xform_dir(skin[idx], v.normal);
            nrm[0] += sn[0] * wgt;
            nrm[1] += sn[1] * wgt;
            nrm[2] += sn[2] * wgt;
            wsum += wgt;
        }
        if wsum > 1e-6 {
            v.position = [p[0] / wsum, p[1] / wsum, p[2] / wsum];
            let l = (nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]).sqrt();
            if l > 1e-6 {
                v.normal = [nrm[0] / l, nrm[1] / l, nrm[2] / l];
            }
        }
    }

    let mut far = 0usize; let mut maxt = 0.0f32;
    for i in 0..n {
        let s = skin[i];
        let d = (s[3][0]*s[3][0] + s[3][1]*s[3][1] + s[3][2]*s[3][2]).sqrt();
        if d > 1.0 { far += 1; }
        if d > maxt { maxt = d; }
    }
    println!("skin translation: max={maxt:.1} joints_over_1={far}/{n}");
    for i in 0..3.min(n) {
        let s = skin[i];
        println!("skin[{i}] t=({:.2},{:.2},{:.2}) m00={:.3} m11={:.3} m22={:.3}", s[3][0], s[3][1], s[3][2], s[0][0], s[1][1], s[2][2]);
    }
    render::render_mesh(&posed, &a[4], 900, 1100, None).expect("render");
    println!("wrote {}", a[4]);
}












