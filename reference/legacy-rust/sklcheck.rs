//! Determine the skeleton convention by checking which composition makes the
//! file's inverse-bind matrices cancel the bind pose.

#[path = "../skl.rs"]
mod skl;

fn local(t: [f32; 3], s: [f32; 3], q: [f32; 4], transpose: bool) -> [[f32; 4]; 4] {
    let (x, y, z, w) = (q[0], q[1], q[2], q[3]);
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, yy, zz) = (x * x2, y * y2, z * z2);
    let (xy, xz, yz) = (x * y2, x * z2, y * z2);
    let (wx, wy, wz) = (w * x2, w * y2, w * z2);
    let mut m = [
        [(1.0 - (yy + zz)), (xy - wz), (xz + wy)],
        [(xy + wz), (1.0 - (xx + zz)), (yz - wx)],
        [(xz - wy), (yz + wx), (1.0 - (xx + yy))],
    ];
    if transpose {
        m = [
            [m[0][0], m[1][0], m[2][0]],
            [m[0][1], m[1][1], m[2][1]],
            [m[0][2], m[1][2], m[2][2]],
        ];
    }
    [
        [m[0][0] * s[0], m[0][1] * s[0], m[0][2] * s[0], 0.0],
        [m[1][0] * s[1], m[1][1] * s[1], m[1][2] * s[1], 0.0],
        [m[2][0] * s[2], m[2][1] * s[2], m[2][2] * s[2], 0.0],
        [t[0], t[1], t[2], 1.0],
    ]
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let p = a.get(1).expect("sklcheck <file.skl>");
    let d = std::fs::read(p).expect("read");
    let rig = skl::Skl::parse(&d).expect("parse");
    println!("joints {}", rig.joints.len());
    for transpose in [false, true] {
        for parent_first in [false, true] {
            let n = rig.joints.len();
            let locals: Vec<_> = rig
                .joints
                .iter()
                .map(|j| local(j.t, j.s, j.q, transpose))
                .collect();
            let mut g = vec![skl::identity(); n];
            let mut done = vec![false; n];
            fn build(
                i: usize,
                rig: &skl::Skl,
                locals: &Vec<[[f32; 4]; 4]>,
                parent_first: bool,
                out: &mut Vec<[[f32; 4]; 4]>,
                done: &mut Vec<bool>,
            ) -> [[f32; 4]; 4] {
                if done[i] {
                    return out[i];
                }
                let p = rig.joints[i].parent;
                let local = locals[i];
                let gg = if p >= 0 && (p as usize) < rig.joints.len() {
                    let pg = build(p as usize, rig, locals, parent_first, out, done);
                    if parent_first {
                        skl::mul(pg, local)
                    } else {
                        skl::mul(local, pg)
                    }
                } else {
                    local
                };
                out[i] = gg;
                done[i] = true;
                gg
            }
            for i in 0..n {
                build(i, &rig, &locals, parent_first, &mut g, &mut done);
            }
            let mut err = 0.0f64;
            let mut worst = 0.0f64;
            for i in 0..n {
                let j = &rig.joints[i];
                let ib = local(j.ib_t, j.ib_s, j.ib_q, transpose);
                let m = skl::mul(ib, g[i]);
                let dt = ((m[3][0] * m[3][0] + m[3][1] * m[3][1] + m[3][2] * m[3][2]) as f64).sqrt();
                let dr = (m[0][0] - 1.0).abs()
                    + (m[1][1] - 1.0).abs()
                    + (m[2][2] - 1.0).abs();
                let e = dt + dr as f64;
                err += e;
                if e > worst {
                    worst = e;
                }
            }
            println!(
                "transpose={transpose} parent_first={parent_first} mean_err={:.4} worst={:.3}",
                err / n as f64,
                worst
            );
        }
    }
}
