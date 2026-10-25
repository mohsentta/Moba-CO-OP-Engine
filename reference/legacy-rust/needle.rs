//! Needle search: find a live (x,z) pair encoded inside replay payloads.

#[path = "../rofl.rs"]
mod rofl;
#[path = "../block.rs"]
mod block;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let path = a.get(1).expect("needle <rofl> <x> <z> [tol]");
    let tx: f64 = a.get(2).expect("x").parse().expect("x");
    let tz: f64 = a.get(3).expect("z").parse().expect("z");
    let tol: f64 = a.get(4).map(|s| s.parse().unwrap_or(3.0)).unwrap_or(3.0);
    let r = rofl::Rofl::open(path).expect("open");
    let scales = [1000.0f64, 4.0, 1.0];
    let mut hits = 0usize;
    for c in &r.chunks {
        for b in block::parse_blocks(&c.data) {
            let p = &b.payload;
            if p.len() < 6 {
                continue;
            }
            for off in 0..=(p.len() - 3) {
                let ux =
                    (p[off] as u32 | (p[off + 1] as u32) << 8 | (p[off + 2] as u32) << 16) as f64;
                for si in 0..3 {
                    let xv = ux / scales[si];
                    if (xv - tx).abs() > tol {
                        continue;
                    }
                    for dz in [3usize, 4] {
                        let zo = off + dz;
                        if zo + 3 > p.len() {
                            continue;
                        }
                        let uz = (p[zo] as u32
                            | (p[zo + 1] as u32) << 8
                            | (p[zo + 2] as u32) << 16) as f64;
                        for sj in 0..3 {
                            let zv = uz / scales[sj];
                            if (zv - tz).abs() <= tol {
                                println!(
                                    "t={:8.3} id={:<5} net=0x{:x} x@{} scale={} z@{} scale={} size={}",
                                    b.time,
                                    b.id,
                                    b.param as u32,
                                    off,
                                    scales[si],
                                    zo,
                                    scales[sj],
                                    p.len()
                                );
                                hits += 1;
                                if hits >= 30 {
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    println!("needle hits={hits}");
}
