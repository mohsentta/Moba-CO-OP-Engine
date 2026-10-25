//! Reorganise the Ghidra dump: classify every function, rename it, and merge
//! the 1,196 small files into per-category files (GUI/draw functions go to
//! `gui_*.c`, etc.). Writes a rename map so the transformation is reversible.
//!
//! This is a pure text pass over `src/*.c`; it does not pretend to understand
//! semantics, it uses strings, callees and body keywords, then propagates
//! labels along the call graph.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

#[derive(Clone)]
struct Meta {
    rva: u64,
    name: String,
    file: String,
    label: &'static str,
    callees: Vec<u64>,
    leaf: Option<String>,
    slug: String,
}

fn list_c(dir: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(dir)
        .expect("src dir")
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x == "c").unwrap_or(false))
        .collect();
    v.sort();
    v
}

/// Split on `/* ---- rva 0x... name ---- */` markers.
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

fn strings_of(body: &str) -> Vec<String> {
    let b = body.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'"' {
            let mut j = i + 1;
            let mut s = String::new();
            while j < b.len() && b[j] != b'"' {
                if b[j] == b'\\' {
                    j += 1;
                    if j >= b.len() {
                        break;
                    }
                }
                s.push(b[j] as char);
                j += 1;
            }
            if s.len() >= 4 {
                out.push(s);
            }
            i = j + 1;
        } else {
            i += 1;
        }
    }
    out
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

fn classify(body: &str, strings: &[String]) -> &'static str {
    let s = body.to_ascii_lowercase();
    let st = strings.join(" ").to_ascii_lowercase();
    let has = |k: &str| s.contains(k);
    let shas = |k: &str| st.contains(k);

    // GUI / draw first: these are the functions the user wants in one file.
    let gui_body = ["d3d", "dxgi", "swapchain", "hlsl", "id3d", "drawprimitive", "drawindexed",
                    "draw_primitive", "present(", "texture2d", "rendertexture", "blendstate",
                    "wm_paint", "wm_mouse", "wm_key", "setwindowpos", "createwindow",
                    "getcursorpos", "showcursor", "setcursor"];
    let gui_str = ["assets/ux", "ux/", "ui/", "fonts/", ".dds", "texture", "shader", "sprite",
                   "atlas", "widget", "panel", "button", "tooltip", "hud", "cursor", "menu",
                   "dialog", "icon", "font", "viewport", "drawtext", "bitmap", "fe/"];
    if gui_body.iter().any(|k| has(k)) || gui_str.iter().any(|k| shas(k)) {
        return "gui";
    }
    if has("fmod") || has("wwise") || shas("sound") || shas("audio") || shas(".bnk") || shas(".wpk") {
        return "audio";
    }
    if has("ws2_") || has("socket") || has("recv(") || has("send(") || shas("netid") {
        return "network";
    }
    if has("replay") || shas("replay") || shas("rofl") || has("keyframe") {
        return "replay";
    }
    if shas("characters/") || shas("spells/") || shas("skins/") || shas(".skn") || shas(".skl")
        || shas(".anm") || shas("particles/")
    {
        return "game";
    }
    if shas(".wad") || shas("data/") || shas("final/") {
        return "file";
    }
    // Memory only when the body is actually allocator-shaped.
    if (has("operator_new(") || has("malloc(")) && s.len() < 300 {
        return "memory";
    }
    if has("strlen(") || has("strcpy(") || has("memcpy(") || has("string_assign") {
        return "string";
    }
    if has("sqrt(") || has("sin(") || has("cos(") || has("matrix") || has("quat") || has("atan2") {
        return "math";
    }
    if has("collision") || has("raycast") || has("navmesh") || has("pathfind") {
        return "physics";
    }
    if has("createfile") || has("readfile") || has("fopen") || has("file_") {
        return "file";
    }
    if has("champion") || has("spell") || has("buff") || has("minion") || has("turret") {
        return "game";
    }
    "misc"
}
fn slug_of(strings: &[String]) -> String {
    for s in strings {
        let l = s.to_ascii_lowercase();
        if l.len() < 6 || l.starts_with("http") {
            continue;
        }
        if l.contains('%')
            || l.contains('(')
            || l.contains('=')
            || l.contains("param")
            || l.contains("lvar")
            || l.contains("cvar")
            || l.contains("uvar")
            || l.contains("stack")
        {
            continue;
        }
        if !(l.contains('/') || l.contains('.') || l.contains(' ')) {
            continue;
        }
        let mut out = String::new();
        for c in l.chars() {
            if c.is_ascii_alphanumeric() {
                out.push(c);
            } else if !out.ends_with('_') {
                out.push('_');
            }
            if out.len() >= 24 {
                break;
            }
        }
        let out = out.trim_matches('_').to_string();
        if out.len() >= 6 {
            return out;
        }
    }
    String::new()
}

/// Rename identifiers outside strings/comments.
fn rename_idents(s: &str, map: &HashMap<String, String>, by_rva: &HashMap<u64, String>) -> String {
    let b = s.as_bytes();
    let mut out = String::with_capacity(s.len() + 64);
    let mut i = 0;
    while i < b.len() {
        let c = b[i] as char;
        // comments
        if c == '/' && i + 1 < b.len() && b[i + 1] == b'/' {
            while i < b.len() && b[i] != b'\n' {
                out.push(b[i] as char);
                i += 1;
            }
            continue;
        }
        if c == '/' && i + 1 < b.len() && b[i + 1] == b'*' {
            out.push_str("/*");
            i += 2;
            while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                out.push(b[i] as char);
                i += 1;
            }
            if i + 1 < b.len() {
                out.push_str("*/");
                i += 2;
            }
            continue;
        }
        if c == '"' {
            out.push('"');
            i += 1;
            while i < b.len() {
                let ch = b[i] as char;
                out.push(ch);
                if b[i] == b'\\' && i + 1 < b.len() {
                    i += 1;
                    out.push(b[i] as char);
                } else if b[i] == b'"' {
                    i += 1;
                    break;
                }
                i += 1;
            }
            continue;
        }
        if c == '\'' {
            out.push('\'');
            i += 1;
            while i < b.len() {
                out.push(b[i] as char);
                if b[i] == b'\\' && i + 1 < b.len() {
                    i += 1;
                    out.push(b[i] as char);
                } else if b[i] == b'\'' {
                    i += 1;
                    break;
                }
                i += 1;
            }
            continue;
        }
        if c.is_ascii_alphabetic() || c == '_' {
            let start = i;
            while i < b.len() && ((b[i] as char).is_ascii_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            let ident = &s[start..i];
            if let Some(hex) = ident.strip_prefix("sub_") {
                if let Ok(rva) = u64::from_str_radix(hex, 16) {
                    if let Some(n) = by_rva.get(&rva) {
                        out.push_str(n);
                        continue;
                    }
                }
            }
            match map.get(ident) {
                Some(n) => out.push_str(n),
                None => out.push_str(ident),
            }
            continue;
        }
        out.push(c);
        i += 1;
    }
    out
}

fn seed_label(n: &str) -> Option<&'static str> {
    if n.starts_with("Replay_") {
        Some("replay")
    } else if n.starts_with("Mem_") || n.starts_with("operator_") {
        Some("memory")
    } else if n.starts_with("String_") || n == "strlen" || n == "memcpy" || n == "memset" {
        Some("string")
    } else if n.starts_with("ZSTD_") {
        Some("compress")
    } else if n.starts_with("File_") {
        Some("file")
    } else if n.starts_with("Registry_") || n.starts_with("TypeRegistry_") {
        Some("registry")
    } else if n.starts_with("Bits_") {
        Some("math")
    } else if n.starts_with("TLS_") || n.starts_with("__security") {
        Some("system")
    } else {
        None
    }
}

/// Classify a function that makes no direct calls by its body shape.
fn leaf_tag(body: &str) -> Option<String> {
    let t = body.trim();
    if t.contains("(**") || t.contains("swi(") || t.contains("__guard") {
        return None; // indirect call or trap: not a real leaf
    }
    if t.is_empty() {
        return Some("empty".into());
    }
    let stmt = t.trim_end_matches(';').trim();
    if stmt == "return" {
        return Some("empty".into());
    }
    let semi = t.matches(';').count();
    if semi > 6 || t.lines().count() > 14 {
        // Large terminal routine: guess from the mix of operations.
        if t.contains("auVar") || t.contains("undefined (*") {
            return Some("simd".into());
        }
        let floats = t.matches("float").count() + t.matches("double").count();
        let loops = t.matches("while (").count() + t.matches("for (").count();
        let shifts = t.matches("<<").count() + t.matches(">>").count();
        if floats >= 6 {
            return Some("fmath".into());
        }
        if shifts >= 4 && t.matches("<<").count() > 0 && t.matches(">>").count() > 0 {
            return Some("bittrick".into());
        }
        if loops > 0 && t.contains("++") {
            return Some("loop".into());
        }
        if t.contains(" % ") {
            return Some("mod".into());
        }
        if (t.contains("DAT_") || t.contains("UNK_")) && t.contains('[') {
            return Some("table".into());
        }
        if (t.contains("DAT_") || t.contains("UNK_")) && t.contains(") =") {
            return Some("init_globals".into());
        }
        if floats > 0 {
            return Some("fmath".into());
        }
        return Some("algo".into());
    }
    let offset = |s: &str| -> Option<String> {
        let i = s.find(" + ")?;
        let h = &s[i + 3..];
        if let Some(hex) = h.strip_prefix("0x") {
            let e = hex.find(|c: char| !c.is_ascii_hexdigit()).unwrap_or(hex.len());
            if e > 0 {
                return Some(hex[..e].to_string());
            }
        }
        let e = h.find(|c: char| !c.is_ascii_digit()).unwrap_or(h.len());
        if e > 0 {
            Some(format!("d{}", &h[..e]))
        } else {
            None
        }
    };
    if let Some(rest) = stmt.strip_prefix("return ") {
        let r = rest.trim_end_matches(';').trim();
        if r == "0" || r == "1" || r.starts_with("0x") || r.parse::<i64>().is_ok() {
            return Some(format!("ret_{}", r.trim_start_matches("0x")));
        }
        if let Some(p) = r.strip_prefix("param_") {
            return Some(format!("id_{p}"));
        }
        if r.starts_with("DAT_") || r.starts_with("UNK_") {
            return Some("get_global".into());
        }
        if let Some(o) = offset(r) {
            return Some(format!("get_{o}"));
        }
    }
    if stmt.starts_with("*(") || stmt.contains(") =") {
        if let Some(o) = offset(stmt) {
            return Some(format!("set_{o}"));
        }
        if stmt.starts_with("DAT_") || stmt.starts_with("UNK_") {
            return Some("set_global".into());
        }
    }
    // Small expression leaves.
    if stmt.contains(" ? ") {
        return Some("select".into());
    }
    if stmt.contains('!') && stmt.contains("return") {
        return Some("not".into());
    }
    for (sym, tag) in [
        ("<<", "shl"),
        (">>", "shr"),
        ("==", "eq"),
        ("!=", "ne"),
        ("<", "lt"),
        (">", "gt"),
        ("%", "mod"),
        ("+", "add"),
        ("-", "sub"),
        ("/", "div"),
        ("*", "mul"),
        ("&", "and"),
        ("|", "or"),
        ("^", "xor"),
    ] {
        if stmt.contains(sym) {
            return Some(tag.to_string());
        }
    }
    Some("op".into())
}

fn load_names(path: &str) -> HashMap<u64, String> {
    let mut m = HashMap::new();
    if let Ok(txt) = fs::read_to_string(path) {
        for line in txt.lines() {
            let mut it = line.split('\t');
            let (Some(r), Some(n)) = (it.next(), it.next()) else {
                continue;
            };
            if let Some(h) = r.trim().strip_prefix("0x") {
                if let Ok(rva) = u64::from_str_radix(h, 16) {
                    m.insert(rva, n.trim().to_string());
                }
            }
        }
    }
    m
}

fn sym_name(rva: u64) -> Option<&'static str> {
    Some(match rva {
        0x6be560 => "Replay_CameraDirector_Tick",
        0x6a8fd0 => "Replay_ScoreObjectVisibility",
        0x6a8420 => "Replay_ResetObjectAccumulators",
        0x275330 => "Replay_ScoreMap_FindOrInsert",
        0x6a5180 => "Replay_VisibilityMap_Test",
        0x6b8260 => "Replay_UpdateA",
        0x6bf600 => "Replay_UpdateB",
        0x6b1580 => "Replay_ActiveGate",
        0x6b8460 => "Replay_Sync",
        0x9aa080 => "Replay_ModeUpdate",
        0x6a92f0 => "Replay_ProcessEventsLoop",
        0x6a98f0 => "Replay_ProcessEventsLoop2",
        0x69ac90 => "Replay_RunEventsUntilTime",
        0x6abca0 => "Replay_PeekEvent",
        0x6bb2d0 => "Replay_SeekTime",
        0x6ba930 => "Replay_SeekCursor",
        0x6af080 => "Replay_ParseBlock",
        0x6b2890 => "Replay_LoadChunks",
        0x6b6040 => "Replay_DecompressChunk",
        0x6b2e90 => "Replay_BuildRecord",
        0x6b1b60 => "Replay_Open",
        0x531740 => "Registry_FindObjectById",
        0x100a110 => "Game_Clock_Read",
        0x100a6f0 => "Game_Clock_IsPaused",
        _ => return None,
    })
}

fn is_epilogue(b: &[u8]) -> bool {
    if b.len() < 2 {
        return false;
    }
    let (b0, b1) = (b[0], b[1]);
    (b0 == 0x48 && (b1 == 0x83 || b1 == 0x81) && b.len() > 2 && b[2] == 0xC4)
        || matches!(b0, 0x5F | 0x5E | 0x5B | 0x5D)
        || (b0 == 0x41 && (0x58..=0x5F).contains(&b1))
        || (b0 == 0x48 && b1 == 0x8B)
        || (b0 == 0x4C && (b1 == 0x8B || b1 == 0x8D))
        || (b0 == 0x49 && b1 == 0x8B)
        || (b0 == 0x0F && b1 == 0x28)
        || (b0 == 0x44 && b1 == 0x0F)
}

fn rename_globals(s: &str) -> String {
    s.replace("_DAT_141c89020", "ReplaySingleton")
        .replace("_DAT_141c88fb0", "g_ReplayObjectList")
        .replace("_DAT_141c88e60", "g_ObjectRegistry")
        .replace("_DAT_141cd5800", "g_PacketHandlers")
        .replace("_DAT_141c88e58", "g_TeamTable")
        .replace("_DAT_141c88eb0", "g_GameClock")
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let src = args.get(1).map(PathBuf::from).unwrap_or_else(|| PathBuf::from("src"));
    let names = load_names("port/replay_names.tsv");
    let outdir = args
        .get(2)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("src_organized"));
    fs::create_dir_all(&outdir).ok();

    let files = list_c(&src);
    let t0 = std::time::Instant::now();

    // Pass 1: metadata.
    let pe = Pe::load(r"H:\projects\LOL\League of Legends (decrypted).exe");
    let mut metas: Vec<Meta> = Vec::new();
    for path in &files {
        let text = fs::read_to_string(path).unwrap_or_default();
        let file = path.file_name().unwrap().to_string_lossy().to_string();
        for (rva, name, slice) in slices(&text) {
            let name = names.get(&rva).cloned().or_else(|| sym_name(rva).map(|s| s.to_string())).unwrap_or(name);
            let body = body_of(slice);
            let mut strings = strings_of(body);
            strings.extend(ref_strings(body, &pe));
            let label = seed_label(&name).unwrap_or_else(|| classify(body, &strings));
            let callees = callees_of(body);
            let failed = slice.contains("decompile failed");
            let mut leaf = if callees.is_empty() && failed {
                Some("undecoded".to_string())
            } else if callees.is_empty() {
                leaf_tag(body)
            } else {
                None
            };
            if leaf.as_deref() == Some("empty") {
                if let Some(b) = pe.bytes_at(rva as u32, 3) {
                    if b[0] == 0xC3 {
                        leaf = Some("ret_stub".to_string());
                    } else if is_epilogue(b) {
                        leaf = Some("tail_frag".to_string());
                    }
                }
            }
            metas.push(Meta {
                rva,
                name,
                file: file.clone(),
                label,
                callees,
                leaf,
                slug: slug_of(&strings),
            });
        }
    }
    metas.sort_by_key(|m| m.rva);
    metas.dedup_by_key(|m| m.rva);

    // Label propagation along the call graph (3 rounds).
    let mut by_rva: HashMap<u64, usize> = HashMap::new();
    for (i, m) in metas.iter().enumerate() {
        by_rva.insert(m.rva, i);
    }
    for _ in 0..3 {
        let labels: Vec<&'static str> = metas.iter().map(|m| m.label).collect();
        for i in 0..metas.len() {
            if metas[i].label != "misc" {
                continue;
            }
            let mut votes: BTreeMap<&'static str, usize> = BTreeMap::new();
            for c in &metas[i].callees {
                if let Some(&j) = by_rva.get(c) {
                    if labels[j] != "misc" {
                        *votes.entry(labels[j]).or_insert(0) += 1;
                    }
                }
            }
            if let Some((&lab, &n)) = votes.iter().max_by_key(|(_, n)| **n) {
                let total: usize = votes.values().sum();
                if n >= 2 && n * 2 >= total && !matches!(lab, "memory" | "string" | "misc") {
                    metas[i].label = lab;
                }
            }
        }
    }

    // Callers, for fragment attribution.
    let mut callers: HashMap<u64, Vec<String>> = HashMap::new();
    for m in &metas {
        for c in &m.callees {
            let e = callers.entry(*c).or_default();
            if e.len() < 3 && !e.contains(&m.name) {
                e.push(m.name.clone());
            }
        }
    }

    // Names.
    let mut map: HashMap<String, String> = HashMap::new();
    let mut by_rva_name: HashMap<u64, String> = HashMap::new();
    let mut rows: Vec<(u64, String, String, &'static str, String)> = Vec::new();
    let mut frags: Vec<(u64, String, u64, String, u64, String)> = Vec::new();
    for (i, m) in metas.iter().enumerate() {
        let new = if let Some(n) = names.get(&m.rva) {
            n.clone()
        } else if let Some(sym) = sym_name(m.rva) {
            sym.to_string()
        } else if m.leaf.as_deref() == Some("tail_frag") {
            // Attribute the fragment to the function that owns the preceding bytes.
            let (owner_rva, owner_name, gap) = if i > 0 {
                let o = &metas[i - 1];
                (o.rva, o.name.clone(), m.rva.saturating_sub(o.rva))
            } else {
                (0, String::new(), u64::MAX)
            };
            let owner_short = by_rva_name
                .get(&owner_rva)
                .cloned()
                .unwrap_or_else(|| {
                    owner_name
                        .trim_start_matches("sub_")
                        .chars()
                        .take(12)
                        .collect::<String>()
                });
            let name = format!("frag_of_{}_{:x}", owner_short, m.rva);
            let cs = callers.get(&m.rva).cloned().unwrap_or_default();
            frags.push((
                m.rva,
                name.clone(),
                owner_rva,
                owner_name.clone(),
                gap,
                cs.join(","),
            ));
            name
        } else if let Some(tag) = &m.leaf {
            format!("leaf_{}_{:x}", tag, m.rva)
        } else if m.slug.is_empty() {
            format!("{}_{:x}", m.label, m.rva)
        } else {
            format!("{}_{}_{:x}", m.label, m.slug, m.rva)
        };
        map.insert(m.name.clone(), new.clone());
        by_rva_name.insert(m.rva, new.clone());
        rows.push((m.rva, m.name.clone(), new, m.label, m.file.clone()));
    }

    // Leaf report: no direct calls, classified by body shape.
    let mut hist: BTreeMap<String, usize> = BTreeMap::new();
    for m in &metas {
        if let Some(tag) = &m.leaf {
            *hist.entry(tag.clone()).or_insert(0) += 1;
        }
    }
    let leaf_count: usize = hist.values().sum();
    {
        let mut tsv = BufWriter::new(fs::File::create(outdir.join("leaves.tsv")).unwrap());
        writeln!(tsv, "rva\told\tnew\tshape\tstatus").unwrap();
        for m in metas.iter().filter(|m| m.leaf.is_some()) {
            let tag = m.leaf.clone().unwrap();
            let new = by_rva_name.get(&m.rva).cloned().unwrap_or_default();
            let status = match tag.as_str() {
                "tail_frag" => "fragment_removed",
                "undecoded" => "failed_decompile",
                _ => "ok",
            };
            writeln!(tsv, "0x{:x}\t{}\t{}\t{}\t{}", m.rva, m.name, new, tag, status).unwrap();
        }
        tsv.flush().ok();

        // Fragment attribution: owner function, gap, and call sites.
        let mut ft = BufWriter::new(fs::File::create(outdir.join("fragments.tsv")).unwrap());
        writeln!(ft, "rva\tnew\towner_rva\towner_name\tgap\tcallers").unwrap();
        for (rva, new, orva, oname, gap, callers) in &frags {
            writeln!(
                ft,
                "0x{rva:x}\t{new}\t0x{orva:x}\t{oname}\t{gap}\t{callers}"
            )
            .unwrap();
        }
        ft.flush().ok();
    }

    // Pass 2: rewrite and merge.
    let mut writers: HashMap<(&'static str, usize), BufWriter<fs::File>> = HashMap::new();
    let mut sizes: HashMap<(&'static str, usize), u64> = HashMap::new();
    let mut counts: HashMap<&'static str, usize> = HashMap::new();
    let max_file = 8 * 1024 * 1024u64;

    let mut merged_fragments = 0usize;
    for path in &files {
        let text = fs::read_to_string(path).unwrap_or_default();
        for (rva, _name, slice) in slices(&text) {
            let idx = match by_rva.get(&rva) {
                Some(&i) => i,
                None => continue,
            };
            let m = &metas[idx];
            // Tail-epilogue fragments are reached by fall-through from their
            // owner, not by call: they are not standalone functions. Drop the
            // duplicate entry; the owner stays in the dump.
            if m.leaf.as_deref() == Some("tail_frag") {
                merged_fragments += 1;
                continue;
            }
            let (marker, rest) = match slice.find('\n') {
                Some(p) => (&slice[..p], &slice[p + 1..]),
                None => ("", slice),
            };
            let renamed = rename_globals(&rename_idents(rest, &map, &by_rva_name));
            let mut chunk = 0usize;
            loop {
                let key = (m.label, chunk);
                let sz = *sizes.get(&key).unwrap_or(&0);
                if !writers.contains_key(&key) || sz < max_file {
                    break;
                }
                chunk += 1;
            }
            let key = (m.label, chunk);
            if !writers.contains_key(&key) {
                let base = if m.label == "gui" { "guifunctions" } else { m.label };
                let path = outdir.join(format!("{base}_{chunk}.c"));
                let f = fs::File::create(&path).expect("create out");
                writers.insert(key, BufWriter::new(f));
                sizes.insert(key, 0);
                counts.entry(m.label).or_insert(0);
            }
            let w = writers.get_mut(&key).unwrap();
            writeln!(w, "{marker}").ok();
            writeln!(w, "/* original: {} */", m.name).ok();
            w.write_all(renamed.as_bytes()).ok();
            writeln!(w).ok();
            *sizes.get_mut(&key).unwrap() += (marker.len() + renamed.len() + 32) as u64;
            *counts.get_mut(&key.0).unwrap() += 1;
        }
    }

    let mut csv = BufWriter::new(fs::File::create(outdir.join("rename_map.tsv")).unwrap());
    writeln!(csv, "rva\told\tnew\tlabel\tfile").unwrap();
    for (rva, old, new, label, file) in &rows {
        writeln!(csv, "0x{rva:x}\t{old}\t{new}\t{label}\t{file}").unwrap();
    }
    csv.flush().ok();

    let mut label_counts: BTreeMap<&str, usize> = BTreeMap::new();
    for m in &metas {
        *label_counts.entry(m.label).or_insert(0) += 1;
    }
    println!("functions : {}", metas.len());
    println!("tail-fragment entries dropped: {merged_fragments}");
    println!("emitted functions: {}", metas.len() - merged_fragments);
    println!("leaves    : {leaf_count}");
    let mut hv: Vec<_> = hist.iter().collect();
    hv.sort_by_key(|(_, n)| std::cmp::Reverse(**n));
    for (tag, n) in hv.iter().take(12) {
        println!("  leaf {tag:10} {n}");
    }
    println!("elapsed   : {:?}", t0.elapsed());
    println!("output    : {}", outdir.display());
    for (l, n) in &label_counts {
        println!("  {l:8} {n:6}");
    }
    drop(writers);
    let out_files: Vec<_> = fs::read_dir(&outdir)
        .unwrap()
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().map(|x| x == "c").unwrap_or(false))
        .collect();
    let total: u64 = out_files.iter().map(|e| e.metadata().map(|m| m.len()).unwrap_or(0)).sum();
    println!("merged files: {} ({:.1} MB)", out_files.len(), total as f64 / 1048576.0);
}




struct Pe {
    data: Vec<u8>,
    secs: Vec<(u32, u32, u32)>, // va, raw_size, raw_ptr
}

impl Pe {
    fn load(path: &str) -> Pe {
        let data = std::fs::read(path).unwrap_or_default();
        let mut secs = Vec::new();
        if data.len() > 0x200 {
            let u32at = |o: usize| u32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
            let u16at = |o: usize| u16::from_le_bytes([data[o], data[o + 1]]);
            let pe = u32at(0x3c) as usize;
            if pe + 0x18 < data.len() {
                let n = u16at(pe + 6) as usize;
                let opt = u16at(pe + 20) as usize;
                let st = pe + 24 + opt;
                for i in 0..n {
                    let o = st + i * 40;
                    if o + 40 <= data.len() {
                        secs.push((u32at(o + 12), u32at(o + 16), u32at(o + 20)));
                    }
                }
            }
        }
        Pe { data, secs }
    }

    fn bytes_at(&self, rva: u32, n: usize) -> Option<&[u8]> {
        for &(va, raw, ptr) in &self.secs {
            if rva >= va && (rva as usize) < va as usize + raw as usize {
                let off = (rva - va + ptr) as usize;
                if off + n <= self.data.len() {
                    return Some(&self.data[off..off + n]);
                }
            }
        }
        None
    }

    fn cstr(&self, rva: u32) -> Option<String> {
        let mut off = None;
        for &(va, raw, ptr) in &self.secs {
            if rva >= va && rva < va + raw.max(0x1000) {
                off = Some((rva - va + ptr) as usize);
                break;
            }
        }
        let off = off?;
        if off >= self.data.len() {
            return None;
        }
        let mut s = Vec::new();
        let mut i = off;
        while i < self.data.len() && s.len() < 160 {
            let b = self.data[i];
            if b == 0 {
                break;
            }
            if !(0x20..0x7f).contains(&b) {
                return None;
            }
            s.push(b);
            i += 1;
        }
        if s.len() >= 4 {
            Some(String::from_utf8_lossy(&s).to_string())
        } else {
            None
        }
    }
}

fn ref_strings(body: &str, pe: &Pe) -> Vec<String> {
    let b = body.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i + 5 < b.len() {
        let rest = &body[i..];
        let tok = if rest.starts_with("&UNK_") {
            Some(5)
        } else if rest.starts_with("&DAT_") {
            Some(5)
        } else {
            None
        };
        if let Some(skip) = tok {
            let mut j = i + skip;
            while j < b.len() && b[j].is_ascii_hexdigit() {
                j += 1;
            }
            if j > i + skip {
                if let Ok(rva) = u32::from_str_radix(&body[i + skip..j], 16) {
                    if let Some(s) = pe.cstr(rva) {
                        out.push(s);
                    }
                }
            }
            i = j;
        } else {
            i += 1;
        }
    }
    out
}



















