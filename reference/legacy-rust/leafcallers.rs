//! Profile functions that call leaf functions, and guess what the caller is
//! from the mix of leaf behaviours it uses.
//!
//! A function that only calls setters is an initializer; one that calls getters
//! is an accessor; one that calls SIMD/float leaves is a math kernel; etc.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::{BufWriter, Write};
use std::path::PathBuf;

struct F {
    rva: u64,
    name: String,
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

fn callees_of(body: &str) -> Vec<u64> {
    let b = body.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
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

fn role(prof: &BTreeMap<String, usize>) -> String {
    let total: usize = prof.values().sum();
    let setters: usize = prof
        .iter()
        .filter(|(k, _)| k.starts_with("set_"))
        .map(|(_, v)| *v)
        .sum();
    let getters: usize = prof
        .iter()
        .filter(|(k, _)| k.starts_with("get"))
        .map(|(_, v)| *v)
        .sum();
    if setters >= 3 && getters == 0 {
        return "struct initializer / writer".to_string();
    }
    if getters >= 2 && setters == 0 {
        return "accessor / reader".to_string();
    }
    let mut v: Vec<(&String, &usize)> = prof.iter().collect();
    v.sort_by_key(|(_, n)| std::cmp::Reverse(**n));
    let (dom, domn) = v[0];
    let share = *domn as f64 / total as f64;
    let label = match dom.as_str() {
        "bittrick" => "bit manipulation / codec",
        "table" => "table-driven lookup",
        "simd" => "SIMD kernel",
        "fmath" => "float math",
        "algo" => "generic algorithm",
        "op" => "small-operator algorithm",
        "init_globals" => "static initializer",
        "not" | "eq" | "ne" | "lt" => "predicate logic",
        "ret_0" | "ret_1" | "ret_2" => "constant/predicate factory",
        "empty" | "ret_stub" => "callback table filler",
        "mod" => "modular arithmetic",
        "add" | "sub" | "mul" => "arithmetic helper",
        _ => "leaf user",
    };
    if share >= 0.7 {
        label.to_string()
    } else {
        let second = v.get(1).map(|(s, _)| s.as_str()).unwrap_or("");
        format!("mixed {dom}+{second}")
    }
}

fn main() {
    let src = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("H:/projects/LOL/src"));
    let leaves = std::env::args()
        .nth(2)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("H:/projects/LOL/src_organized/leaves.tsv"));
    let out = std::env::args()
        .nth(3)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("port/LEAF_CALLERS.md"));

    // Leaf shapes.
    let mut shape: HashMap<u64, String> = HashMap::new();
    if let Ok(txt) = fs::read_to_string(&leaves) {
        for (i, line) in txt.lines().enumerate() {
            if i == 0 {
                continue;
            }
            let c: Vec<&str> = line.split('\t').collect();
            if c.len() >= 4 {
                if let Some(h) = c[0].strip_prefix("0x") {
                    if let Ok(rva) = u64::from_str_radix(h, 16) {
                        shape.insert(rva, c[3].to_string());
                    }
                }
            }
        }
    }

    let mut funcs: Vec<F> = Vec::new();
    for path in list_c(&src) {
        let text = fs::read_to_string(&path).unwrap_or_default();
        for (rva, name, slice) in slices(&text) {
            let body = match (slice.find('{'), slice.rfind('}')) {
                (Some(a), Some(b)) if b > a => &slice[a + 1..b],
                _ => "",
            };
            funcs.push(F {
                rva,
                name,
                callees: callees_of(body),
            });
        }
    }
    funcs.sort_by_key(|f| f.rva);
    funcs.dedup_by_key(|f| f.rva);

    let mut rows: Vec<(usize, u64, String, String, String)> = Vec::new();
    for f in &funcs {
        let mut prof: BTreeMap<String, usize> = BTreeMap::new();
        let mut called_leaves = 0usize;
        for c in &f.callees {
            if let Some(s) = shape.get(c) {
                *prof.entry(s.clone()).or_insert(0) += 1;
                called_leaves += 1;
            }
        }
        if called_leaves == 0 {
            continue;
        }
        let mut parts: Vec<String> = prof.iter().map(|(k, v)| format!("{k}x{v}")).collect();
        parts.sort();
        rows.push((
            called_leaves,
            f.rva,
            f.name.clone(),
            role(&prof),
            parts.join(" "),
        ));
    }
    rows.sort_by_key(|r| std::cmp::Reverse(r.0));

    let mut md = String::new();
    md.push_str("# Functions that call leaf functions\n\n");
    md.push_str("Role is inferred from the mix of leaf behaviours the function calls.\n\n");
    md.push_str("| leaf calls | rva | function | inferred role | leaf profile |\n|---|---|---|---|---|\n");
    for (n, rva, name, r, prof) in rows.iter().take(120) {
        md.push_str(&format!("| {n} | `0x{rva:x}` | `{name}` | {r} | {prof} |\n"));
    }
    md.push_str(&format!("\nTotal callers of leaves: {}\n", rows.len()));
    fs::write(&out, md).ok();

    // Full table.
    if let Some(dir) = out.parent() {
        let mut tsv = BufWriter::new(fs::File::create(dir.join("leaf_callers.tsv")).unwrap());
        writeln!(tsv, "rva\tname\trole\tleaf_calls\tprofile").unwrap();
        for (n, rva, name, r, prof) in &rows {
            writeln!(tsv, "0x{rva:x}\t{name}\t{r}\t{n}\t{prof}").unwrap();
        }
        tsv.flush().ok();
    }

    let mut role_hist: BTreeMap<&str, usize> = BTreeMap::new();
    for (_, _, _, r, _) in &rows {
        *role_hist.entry(r.as_str()).or_insert(0) += 1;
    }
    println!("callers of leaves: {}", rows.len());
    for (r, n) in &role_hist {
        println!("  {r:34} {n}");
    }
    println!("wrote {}", out.display());
}


