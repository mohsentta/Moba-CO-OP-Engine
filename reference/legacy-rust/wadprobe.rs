#![allow(dead_code)]
//! Diagnostic tool: locate the TOC in a shipping `.wad.client`, summarise it,
//! and find the model files by content rather than by assumed paths.

use std::collections::BTreeMap;
use std::env;

#[path = "../wad.rs"]
mod wad;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: wadprobe <file.wad.client> [--scan] [--findpath=<path>]");
        return;
    }

    // Extract a chunk by path to disk: --extract=<path>=<outfile>
    if args.iter().any(|a| a.starts_with("--extract=")) {
        let w = match wad::Wad::open(&args[1]) {
            Ok(w) => w,
            Err(e) => {
                eprintln!("{e}");
                return;
            }
        };
        for a in args.iter().filter(|a| a.starts_with("--extract=")) {
            let spec = a.trim_start_matches("--extract=");
            let (p, out) = spec.split_once('=').unwrap_or((spec, "out.bin"));
            match w.read_path(p) {
                Ok(d) => {
                    std::fs::write(out, &d).expect("write");
                    println!("  {p}: {} bytes -> {out}", d.len());
                }
                Err(e) => println!("  {p}: {e}"),
            }
        }
        return;
    }

    // Read and decompress a chunk by path.
    if args.iter().any(|a| a.starts_with("--read=")) {
        let w = match wad::Wad::open(&args[1]) {
            Ok(w) => w,
            Err(e) => {
                eprintln!("{e}");
                return;
            }
        };
        println!("toc 0x{:x}, {} chunks", w.toc_offset, w.entries.len());
        for a in args.iter().filter(|a| a.starts_with("--read=")) {
            let p = a.trim_start_matches("--read=");
            match w.read_path(p) {
                Ok(d) => {
                    let magic = if d.len() >= 4 {
                        String::from_utf8_lossy(&d[..4]).to_string()
                    } else {
                        String::new()
                    };
                    println!(
                        "  {p}: {} bytes, magic {:?} {}",
                        d.len(),
                        magic,
                        d.iter()
                            .take(16)
                            .map(|b| format!("{b:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                }
                Err(e) => println!("  {p}: {e}"),
            }
        }
        return;
    }

    if args.iter().any(|a| a == "--verify") {
        let bytes = std::fs::read(&args[1]).expect("read wad");
        let start = args
            .iter()
            .find_map(|a| a.strip_prefix("--verify="))
            .and_then(|s| usize::from_str_radix(s.trim_start_matches("0x"), 16).ok())
            .unwrap_or(0x110);
        let mut o = start;
        let mut good = 0usize;
        let mut miss = 0usize;
        let mut comps: BTreeMap<u8, usize> = BTreeMap::new();
        let mut last_good_end = 0u64;
        let mut first_bad = None;
        while o + 32 <= bytes.len() {
            let e = match wad::parse_entry(&bytes, o) {
                Some(e) => e,
                None => break,
            };
            let ok_bounds = e.compressed_size > 0
                && e.uncompressed_size > 0
                && (e.offset as u64 + e.compressed_size as u64) <= bytes.len() as u64;
            if !ok_bounds {
                miss += 1;
            } else {
                let s = e.offset as usize;
                let raw = &bytes[s..s + e.compressed_size as usize];
                let sum = wad::checksum(raw);
                if sum == e.checksum {
                    good += 1;
                    *comps.entry(e.compression).or_insert(0) += 1;
                    last_good_end = e.offset as u64 + e.compressed_size as u64;
                    o += 32;
                    miss = 0;
                    continue;
                }
            }
            miss += 1;
            if first_bad.is_none() {
                first_bad = Some((o, e.compression, e.compressed_size, e.uncompressed_size));
            }
            if miss >= 64 {
                break;
            }
            o += 32;
        }
        println!("verify from 0x{start:x}:");
        println!("  checksum-valid entries : {good}");
        println!("  comp histogram         : {comps:?}");
        println!("  last valid entry ends  : 0x{last_good_end:x} (file {})", bytes.len());
        println!("  first mismatch         : {first_bad:?}");
        return;
    }

    // Locate a known entry, then grow the sorted run around it to find the TOC.
    if args.iter().any(|a| a.starts_with("--run=")) {
        let bytes = std::fs::read(&args[1]).expect("read wad");
        for a in args.iter().filter(|a| a.starts_with("--run=")) {
            let p = a.trim_start_matches("--run=");
            let h = wad::path_hash(p);
            println!("path {p:?} -> hash {h:016x}");
            let pat = h.to_le_bytes();
            let mut hits = Vec::new();
            for i in 0..bytes.len().saturating_sub(8) {
                if bytes[i..i + 8] == pat {
                    hits.push(i);
                    if hits.len() > 4 {
                        break;
                    }
                }
            }
            println!("  hits: {hits:?}");
            for hit in hits {
                let ok = |o: usize| -> Option<wad::WadEntry> {
                    let e = wad::parse_entry(&bytes, o)?;
                    if e.compression > 3 || e.compressed_size == 0 || e.uncompressed_size == 0 {
                        return None;
                    }
                    if e.offset as u64 + e.compressed_size as u64 > bytes.len() as u64 {
                        return None;
                    }
                    Some(e)
                };
                if ok(hit).is_none() {
                    println!("  @0x{hit:x}: entry itself invalid");
                    continue;
                }
                let mut start = hit;
                while start >= 32 {
                    match (ok(start - 32), ok(start)) {
                        (Some(prev), Some(cur)) if prev.hash < cur.hash => start -= 32,
                        _ => break,
                    }
                }
                let mut end = hit;
                while end + 64 <= bytes.len() {
                    match (ok(end), ok(end + 32)) {
                        (Some(cur), Some(next)) if cur.hash < next.hash => end += 32,
                        _ => break,
                    }
                }
                let n = (end - start) / 32 + 1;
                println!(
                    "  @0x{hit:x}: run start=0x{start:x} end=0x{end:x} count={n}"
                );
                let first = ok(start).unwrap();
                let last = ok(end).unwrap();
                println!(
                    "    first hash={:016x} off=0x{:x} c={} u={}",
                    first.hash, first.offset, first.compressed_size, first.uncompressed_size
                );
                println!(
                    "    last  hash={:016x} off=0x{:x} c={} u={}",
                    last.hash, last.offset, last.compressed_size, last.uncompressed_size
                );
            }
        }
        return;
    }

    // Path-hash lookup works even when the TOC layout is not yet understood.
    if args.iter().any(|a| a.starts_with("--findpath=")) {
        let bytes = std::fs::read(&args[1]).expect("read wad");
        for a in args.iter().filter(|a| a.starts_with("--findpath=")) {
            let p = a.trim_start_matches("--findpath=");
            let h = wad::path_hash(p);
            println!("path {p:?} -> hash {h:016x}  (file {} bytes)", bytes.len());
            for (name, pat) in [("LE", h.to_le_bytes()), ("BE", h.to_be_bytes())] {
                let mut hits = Vec::new();
                for i in 0..bytes.len().saturating_sub(8) {
                    if bytes[i..i + 8] == pat {
                        hits.push(i);
                        if hits.len() > 20 {
                            break;
                        }
                    }
                }
                println!("  {name} hits: {:?}", hits);
                for &i in hits.iter().take(3) {
                    let s = i.saturating_sub(16);
                    let e = (i + 48).min(bytes.len());
                    println!(
                        "    @0x{i:x}: {}",
                        bytes[s..e]
                            .iter()
                            .map(|b| format!("{b:02x}"))
                            .collect::<Vec<_>>()
                            .join(" ")
                    );
                }
            }
        }
        return;
    }

    let w = match wad::Wad::open(&args[1]) {
        Ok(w) => w,
        Err(e) => {
            eprintln!("{e}");
            return;
        }
    };
    println!("file        : {}", w.path.display());
    println!("size        : {}", w.bytes.len());
    println!("toc offset  : 0x{:x}", w.toc_offset);
    println!("chunks      : {}", w.entries.len());
    println!(
        "head        : {}",
        w.bytes[..16.min(w.bytes.len())]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<Vec<_>>()
            .join(" ")
    );

    let mut comp = BTreeMap::new();
    let mut total_u = 0u64;
    for e in &w.entries {
        *comp.entry(e.compression).or_insert(0usize) += 1;
        total_u += e.uncompressed_size as u64;
    }
    println!("compression : {comp:?}");
    println!("uncompressed: {total_u}");

    for (i, e) in w.entries.iter().take(5).enumerate() {
        println!(
            "  [{i}] hash={:016x} off=0x{:x} c={} u={} {} check={:016x}",
            e.hash,
            e.offset,
            e.compressed_size,
            e.uncompressed_size,
            e.compression_name(),
            e.checksum
        );
    }

    if !args.iter().any(|a| a == "--scan") {
        // Extra: locate a chunk by path hash anywhere in the file.
        for a in args.iter().skip(2) {
            if let Some(p) = a.strip_prefix("--findpath=") {
                let h = wad::path_hash(p);
                println!("path {p:?} -> hash {h:016x}");
                for (name, pat) in [
                    ("LE", h.to_le_bytes()),
                    ("BE", h.to_be_bytes()),
                ] {
                    let mut hits = Vec::new();
                    for i in 0..w.bytes.len().saturating_sub(8) {
                        if w.bytes[i..i + 8] == pat {
                            hits.push(i);
                        }
                    }
                    println!("  {name} hits: {:?}", &hits[..hits.len().min(8)]);
                    for &i in hits.iter().take(2) {
                        let s = i.saturating_sub(8);
                        let e = (i + 40).min(w.bytes.len());
                        println!(
                            "    @0x{i:x}: {}",
                            w.bytes[s..e]
                                .iter()
                                .map(|b| format!("{b:02x}"))
                                .collect::<Vec<_>>()
                                .join(" ")
                        );
                    }
                }
            }
        }
        return;
    }

    // Walk every chunk, classify by magic, and look for model references.
    let mut magics: BTreeMap<String, (usize, u64)> = BTreeMap::new();
    let mut model_refs: Vec<(u64, String)> = Vec::new();
    let mut failed = 0usize;
    for e in &w.entries {
        let data = match w.read_chunk(e) {
            Ok(d) => d,
            Err(_) => {
                failed += 1;
                continue;
            }
        };
        let magic = if data.len() >= 4 {
            String::from_utf8_lossy(&data[..4]).to_string()
        } else {
            String::new()
        };
        let key = magic
            .chars()
            .map(|c| if c.is_ascii_graphic() { c } else { '.' })
            .collect::<String>();
        let slot = magics.entry(key).or_insert((0, 0));
        slot.0 += 1;
        slot.1 += data.len() as u64;

        if data.len() >= 4 && (data[0] == b'P' && data[1] == b'R' && data[2] == b'O' && data[3] == b'P')
        {
            for s in find_strings(&data) {
                let l = s.to_ascii_lowercase();
                if l.ends_with(".skn") || l.ends_with(".skl") || l.ends_with(".anm") {
                    model_refs.push((e.hash, s));
                }
            }
        }
    }
    println!("chunk magics (top 20):");
    let mut v: Vec<_> = magics.into_iter().collect();
    v.sort_by_key(|(_, (n, _))| std::cmp::Reverse(*n));
    for (k, (n, sz)) in v.into_iter().take(20) {
        println!("  {k:?} x{n} ({sz} bytes)");
    }
    if failed > 0 {
        println!("failed to decompress: {failed}");
    }
    model_refs.sort();
    model_refs.dedup();
    println!("model refs found inside PROP chunks: {}", model_refs.len());
    for (h, s) in model_refs.iter().take(40) {
        println!("  {h:016x} -> {s}");
    }
}

fn find_strings(d: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    let mut start = None;
    for (i, &b) in d.iter().enumerate() {
        let printable = (0x20..0x7f).contains(&b);
        match (printable, start) {
            (true, None) => start = Some(i),
            (false, Some(s)) => {
                if i - s >= 4 {
                    out.push(String::from_utf8_lossy(&d[s..i]).to_string());
                }
                start = None;
            }
            _ => {}
        }
    }
    if let Some(s) = start {
        if d.len() - s >= 4 {
            out.push(String::from_utf8_lossy(&d[s..]).to_string());
        }
    }
    out
}
