//! Classify WAD chunks by their 8-byte header (size + magic).

#[path = "../wad.rs"]
mod wad;

use std::collections::HashMap;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let path = a.get(1).expect("wadclass <wad> [limit]");
    let limit: usize = a.get(2).map(|s| s.parse().unwrap_or(usize::MAX)).unwrap_or(usize::MAX);
    let w = wad::Wad::open(path).expect("open wad");
    let mut hist: HashMap<u32, usize> = HashMap::new();
    let mut sample: HashMap<u32, (u64, usize, u32)> = HashMap::new();
    let n = w.entries.len().min(limit);
    for (i, e) in w.entries.iter().take(n).enumerate() {
        let d = match w.read_chunk(e) {
            Ok(d) => d,
            Err(_) => continue,
        };
        if d.len() < 8 {
            continue;
        }
        let size = u32::from_le_bytes([d[0], d[1], d[2], d[3]]);
        let magic = u32::from_le_bytes([d[4], d[5], d[6], d[7]]);
        *hist.entry(magic).or_insert(0) += 1;
        sample
            .entry(magic)
            .or_insert((e.hash, d.len(), size));
        if i % 1000 == 0 {
            eprintln!("  {i}/{n}");
        }
    }
    let mut v: Vec<(&u32, &usize)> = hist.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1));
    println!("chunks scanned: {n}");
    for (magic, count) in v.iter().take(20) {
        let (hash, len, size) = sample.get(magic).unwrap();
        println!(
            "  magic 0x{magic:08x} count={count:<6} sample hash=0x{hash:016x} len={len} hdr_size={size}"
        );
    }
}
