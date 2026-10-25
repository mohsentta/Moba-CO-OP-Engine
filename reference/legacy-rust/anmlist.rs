//! List animations in a WAD (hash, version, frames, duration, tracks).

#[path = "../wad.rs"]
mod wad;
#[path = "../anm.rs"]
mod anm;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let wadp = a.get(1).expect("anmlist <wad> [out.tsv]");
    let out = a.get(2);
    let w = wad::Wad::open(wadp).expect("open wad");
    let mut rows: Vec<String> = Vec::new();
    for e in &w.entries {
        if let Ok(d) = w.read_chunk(e) {
            if d.len() >= 12 && (&d[0..8] == b"r3d2anmd" || &d[0..8] == b"r3d2canm") {
                if let Ok(x) = anm::Anm::parse(&d) {
                    rows.push(format!(
                        "{:016x}\t{}\t{}\t{:.3}\t{}",
                        e.hash, x.version, x.frame_count, x.duration, x.tracks.len()
                    ));
                }
            }
        }
    }
    let text = rows.join("\n");
    if let Some(p) = out {
        std::fs::write(p, text + "\n").expect("write");
        println!("wrote {} animations to {p}", rows.len());
    } else {
        println!("{text}");
    }
}
