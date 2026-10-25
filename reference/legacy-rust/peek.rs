//! Peek a packet payload as u24 fixed-point pairs.

#[path = "../rofl.rs"]
mod rofl;
#[path = "../block.rs"]
mod block;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let path = a.get(1).expect("peek <rofl> <id> <t> <off> <count> [stride] [size]");
    let id: u16 = a.get(2).expect("id").parse().expect("id");
    let t: f64 = a.get(3).expect("t").parse().expect("t");
    let off: usize = a.get(4).expect("off").parse().expect("off");
    let count: usize = a.get(5).expect("count").parse().expect("count");
    let stride: usize = a.get(6).map(|s| s.parse().unwrap_or(6)).unwrap_or(6);
    let want_size: usize = a.get(7).map(|s| s.parse().unwrap_or(0)).unwrap_or(0);
    let r = rofl::Rofl::open(path).expect("open");
    for c in &r.chunks {
        for b in block::parse_blocks(&c.data) {
            if b.id != id || (b.time as f64 - t).abs() > 0.5 {
                continue;
            }
            if want_size != 0 && b.payload.len() != want_size {
                continue;
            }
            let p = &b.payload;
            println!(
                "t={:.3} id={} net=0x{:x} size={}",
                b.time,
                b.id,
                b.param as u32,
                p.len()
            );
            for k in 0..count {
                let o = off + k * stride;
                if o + 6 > p.len() {
                    break;
                }
                let x = (p[o] as u32 | (p[o + 1] as u32) << 8 | (p[o + 2] as u32) << 16) as f64
                    / 1000.0;
                let z = (p[o + 3] as u32
                    | (p[o + 4] as u32) << 8
                    | (p[o + 5] as u32) << 16) as f64
                    / 1000.0;
                println!("  [{k}] o={o} x={x:.1} z={z:.1}");
            }
            return;
        }
    }
    println!("not found");
}
