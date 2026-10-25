//! Call-graph inference around the functions we already understand.
//!
//! Seeds are the replay-path functions identified from the decompile. We walk
//! callers and callees two hops out, and print what each neighbour most likely
//! is, with the evidence (who calls it / what it calls).

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::PathBuf;

struct F {
    rva: u64,
    name: String,
    file: String,
    callees: Vec<u64>,
}

fn list_c(dir: &PathBuf) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(dir)
        .expect("src")
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x == "c").unwrap_or(false))
        .collect();
    v.sort();
    v
}

fn slices(text: &str) -> Vec<(u64, String, &str)> {
    let mut marks: Vec<(usize, u64, String)> = Vec::new();
    for line in text.split_inclusive('\n') {
        let start = line.as_ptr() as usize - text.as_ptr() as usize;
        let t = line.trim();
        if let Some(rest) = t.strip_prefix("/* ---- rva ") {
            let mut it = rest.split_whitespace();
            let rva_s = it.next().unwrap_or("");
            let name = it.next().unwrap_or("").trim_end_matches("----").trim();
            if let Some(hex) = rva_s.strip_prefix("0x") {
                if let Ok(rva) = u64::from_str_radix(hex, 16) {
                    marks.push((start, rva, name.to_string()));
                }
            }
        }
    }
    let mut out = Vec::new();
    for (i, (pos, rva, name)) in marks.iter().enumerate() {
        let end = marks.get(i + 1).map(|m| m.0).unwrap_or(text.len());
        out.push((*rva, name.clone(), &text[*pos..end]));
    }
    out
}

fn body_of(slice: &str) -> &str {
    match (slice.find('{'), slice.rfind('}')) {
        (Some(a), Some(b)) if b > a => &slice[a + 1..b],
        _ => "",
    }
}

fn callees_of(body: &str) -> Vec<u64> {
    let b = body.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i + 4 < b.len() {
        if b[i] == b's' && i + 4 <= b.len() && &b[i..i + 4] == b"sub_" {
            let mut j = i + 4;
            while j < b.len() && b[j].is_ascii_hexdigit() {
                j += 1;
            }
            if j > i + 4 {
                if let Ok(v) = u64::from_str_radix(&body[i + 4..j], 16) {
                    out.push(v);
                }
            }
            i = j;
        } else if b[i] == b'f' && i + 7 <= b.len() && &b[i..i + 7] == b"func_0x" {
            let mut j = i + 7;
            while j < b.len() && b[j].is_ascii_hexdigit() {
                j += 1;
            }
            if j > i + 7 {
                if let Ok(v) = u64::from_str_radix(&body[i + 7..j], 16) {
                    out.push(if v >= 0x1_4000_0000 { v - 0x1_4000_0000 } else { v });
                }
            }
            i = j;
        } else {
            i += 1;
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

fn seed_table() -> Vec<(u64, &'static str)> {
    vec![
        (0x6b1b60, "replay file loader: reads RIOT header and calls Replay_LoadChunks"),
        (0x6b2890, "Replay_LoadChunks: walks footer/signature/chunk chain, fills chunk vectors"),
        (0x6b6040, "Replay_DecompressChunk: stored copy or ZSTD_decompress"),
        (0x6af080, "Replay_ParseBlock: marker-bit block parser (time, size, id, param)"),
        (0x6b2e90, "Replay_BuildRecord: iterates blocks into record objects"),
        (0x6bb2d0, "Replay_SeekTime: binary search over 0x18-byte event array"),
        (0x6ba930, "Replay_SeekCursor: set/clamp cursor, cache timestamp"),
        (0x6abca0, "Replay_PeekEvent: read record at cursor, bump payload refcount"),
        (0x69ac90, "Replay_RunEventsUntilTime: dispatch events up to the clock"),
        (0x6a92f0, "Replay_ProcessEventsLoop: peek-loop variant with float/netId fields"),
        (0x6a98f0, "Replay_ProcessEventsLoop2: three-cursor event walk"),
        (0x6be560, "replay controller tick: countdown, per-object state, event walk"),
        (0x9aa080, "replay-mode Update: sync, gate, update A/B, camera follow"),
        (0x6b1580, "replay active gate: flags, timer, speed checks"),
        (0x6b8460, "replay sync: last_sync = clock"),
        (0x6b8260, "replay update path A"),
        (0x6bf600, "replay update path B"),
        (0x6a8420, "per-object replay accumulator reset"),
        (0x6a8fd0, "per-object replay scoring/vision pass"),
        (0x6a8c70, "score accumulation for status flags"),
        (0x531740, "CONTEXT Registry_FindObjectById: netId -> object"),
        (0x100a110, "CONTEXT game clock getter"),
        (0x275330, "per-object score map lookup/insert"),
        (0x6a4eb0, "chunk record range map lookup"),
        (0x6ab1a0, "vector grow for chunk records"),
        (0x6a16a0, "vector append for chunk records"),
        (0xf33c30, "CONTEXT Mem_Alloc"),
        (0xf2ded0, "CONTEXT operator_delete"),
        (0xf05a00, "CONTEXT ZSTD_decompress"),
        (0xf0f760, "CONTEXT ZSTD_isError"),
        (0xf096d0, "CONTEXT ZSTD_getErrorName"),
        (0xffbe00, "CONTEXT Payload_ReadVarint"),
        (0x2036b0, "CONTEXT std::string assign/copy (SSO buffer)"),
        (0x2035f0, "CONTEXT string/object construct helper"),
        (0x23cbc0, "CONTEXT payload buffer grow/copy (Mem_Alloc + memcpy)"),
        (0x23e870, "CONTEXT vector resize helper"),
        (0x2dfbc0, "CONTEXT object type predicate A (type id compare)"),
        (0x2dfc40, "CONTEXT object type predicate B (type id compare)"),
        (0x2474b0, "CONTEXT CRT fatal: range-check failure"),
        (0x6b1580, "replay gate"),
    ]
}

fn main() {
    let src = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("H:/projects/LOL/src"));
    let out = std::env::args()
        .nth(2)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("port/INFERRED.md"));

    let mut funcs: Vec<F> = Vec::new();
    for path in list_c(&src) {
        let text = fs::read_to_string(&path).unwrap_or_default();
        let file = path.file_name().unwrap().to_string_lossy().to_string();
        for (rva, name, slice) in slices(&text) {
            funcs.push(F {
                rva,
                name,
                file: file.clone(),
                callees: callees_of(body_of(slice)),
            });
        }
    }
    funcs.sort_by_key(|f| f.rva);
    funcs.dedup_by_key(|f| f.rva);
    let idx: HashMap<u64, usize> = funcs.iter().enumerate().map(|(i, f)| (f.rva, i)).collect();

    // Reverse edges.
    let mut callers: HashMap<u64, Vec<u64>> = HashMap::new();
    for f in &funcs {
        for c in &f.callees {
            callers.entry(*c).or_default().push(f.rva);
        }
    }

    let seeds = seed_table();
    let seed_role: HashMap<u64, &'static str> = seeds.iter().copied().collect();

    // BFS 2 hops, collect evidence.
    let mut dist: HashMap<u64, u8> = HashMap::new();
    let mut evidence: HashMap<u64, Vec<String>> = HashMap::new();
    for &(s, role) in seeds.iter().filter(|(_, r)| !r.starts_with("CONTEXT")) {
        dist.insert(s, 0);
        evidence.entry(s).or_default().push(format!("KNOWN: {role}"));
        // callees
        if let Some(&i) = idx.get(&s) {
            for &c in &funcs[i].callees {
                dist.entry(c).or_insert(1);
                evidence
                    .entry(c)
                    .or_default()
                    .push(format!("called by 0x{s:x} ({role})"));
            }
        }
        // callers
        if let Some(cs) = callers.get(&s) {
            for &c in cs {
                dist.entry(c).or_insert(1);
                let caller_role = seed_role.get(&c).map(|r| *r).unwrap_or("unknown");
                evidence
                    .entry(c)
                    .or_default()
                    .push(format!("calls 0x{s:x} ({role}); caller itself: {caller_role}"));
            }
        }
    }
    // second hop from depth-1 nodes
    let first: Vec<(u64, u8)> = dist.iter().filter(|(_, &d)| d == 1).map(|(&r, &d)| (r, d)).collect();
    for (r, _) in first {
        if let Some(&i) = idx.get(&r) {
            for &c in &funcs[i].callees {
                if !dist.contains_key(&c) {
                    dist.insert(c, 2);
                    evidence
                        .entry(c)
                        .or_default()
                        .push(format!("called by 0x{r:x} (2 hops from a known replay function)"));
                }
            }
        }
        if let Some(cs) = callers.get(&r) {
            for &c in cs {
                if !dist.contains_key(&c) {
                    dist.insert(c, 2);
                    evidence
                        .entry(c)
                        .or_default()
                        .push(format!("calls 0x{r:x} (2 hops from a known replay function)"));
                }
            }
        }
    }

    // Guess a role from evidence.
    let guess = |rva: u64, ev: &[String]| -> String {
        if let Some(role) = seed_role.get(&rva) {
            return (*role).to_string();
        }
        let j = ev.join(" | ");
        let by = |s: &str| j.contains(&format!("called by {s}"));
        let to = |s: &str| j.contains(&format!("calls {s}"));
        if by("0x6b2890") {
            return "chunk-chain helper called by Replay_LoadChunks".to_string();
        }
        if by("0x6b1b60") {
            return "replay header/file helper called by the loader".to_string();
        }
        if by("0x6af080") {
            return "payload reader called by Replay_ParseBlock".to_string();
        }
        if by("0x6b2e90") {
            return "block/record helper called by Replay_BuildRecord".to_string();
        }
        if by("0x6bb2d0") || by("0x6ba930") || by("0x6abca0") {
            return "event-cursor/seek helper".to_string();
        }
        if by("0x69ac90") || by("0x6a98f0") {
            return "event dispatch helper (called from the event walk)".to_string();
        }
        if by("0x6be560") {
            return "per-object replay update helper".to_string();
        }
        if by("0x6b8260") || by("0x6bf600") {
            return "helper inside the replay update path".to_string();
        }
        if by("0x9aa080") {
            return "replay-mode update helper".to_string();
        }
        if to("0x6af080") {
            return "block consumer / record producer".to_string();
        }
        if to("0x69ac90") || to("0x6a98f0") || to("0x6be560") {
            return "driver/owner of the replay event walk".to_string();
        }
        if to("0x6b2890") || to("0x6b1b60") {
            return "code that launches replay loading".to_string();
        }
        if to("0x531740") {
            return "object lookup user (netId-based)".to_string();
        }
        "adjacent to the replay path (role unknown)".to_string()
    };

    let mut rows: Vec<(u8, u64, String, String, String, String)> = Vec::new();
    for (&rva, &d) in &dist {
        if d > 1 {
            continue;
        }
        let f = match idx.get(&rva) {
            Some(&i) => &funcs[i],
            None => continue,
        };
        let ev = evidence.get(&rva).cloned().unwrap_or_default();
        let first_ev = ev.first().cloned().unwrap_or_default();
        rows.push((d, rva, f.name.clone(), f.file.clone(), guess(rva, &ev), first_ev));
    }
    rows.sort_by(|a, b| (a.0, a.1).cmp(&(b.0, b.1)));

    let mut md = String::new();
    md.push_str("# Inferred roles around the known replay path\n\n");
    md.push_str("Seeded from the functions identified in the decompile. Distance 1 meansn");
    md.push_str("direct caller/callee; distance 2 is context only.\n\n");
    md.push_str("| d | rva | function | likely role | evidence |\n");
    md.push_str("|---|---|---|---|---|\n");
    for (d, rva, name, _file, role, ev) in &rows {
        md.push_str(&format!(
            "| {d} | `0x{rva:x}` | `{name}` | {role} | {} |\n",
            ev.replace('|', "/")
        ));
    }
    md.push_str(&format!("\nTotal functions in the 2-hop replay neighbourhood: {}\n", rows.len()));
    fs::write(&out, md).ok();

    let known = rows.iter().filter(|r| r.0 == 0).count();
    let d1 = rows.iter().filter(|r| r.0 == 1).count();
    let d2 = dist.values().filter(|&&d| d == 2).count();
    println!("seeds {} | hops: 1={} 2={} | wrote {}", known, d1, d2, out.display());
}





