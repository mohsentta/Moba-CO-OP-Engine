//! Bulk extractor for the Ghidra dump.
//!
//! Walks every `src/*.c`, pulls each function's RVA, name, C signature and
//! coarse body metrics, and writes a TSV plus a summary. This is the
//! "0.3 s per function" pass - it is intentionally mechanical.

use std::collections::BTreeMap;
use std::fs;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

#[derive(Default)]
struct FnRow {
    rva: u64,
    name: String,
    file: String,
    ret: String,
    params: String,
    lines: usize,
    calls: usize,
    branches: usize,
    shape: String,
    trivial: bool,
    leaf: bool,
}

fn read_dir_c(path: &Path) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(path)
        .expect("read src dir")
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x == "c").unwrap_or(false))
        .collect();
    v.sort();
    v
}

/// Split the file into function slices using the `rva 0x... name` markers.
fn slices<'a>(text: &'a str) -> Vec<(u64, String, &'a str)> {
    let mut marks: Vec<(usize, u64, String)> = Vec::new();
    for line in text.split_inclusive('\n') {
        let start = line.as_ptr() as usize - text.as_ptr() as usize;
        let t = line.trim();
        let body = if let Some(rest) = t.strip_prefix("/* ---- rva ") {
            rest
        } else if let Some(rest) = t.strip_prefix("     rva ") {
            // index line: "     rva 0x...  name"
            rest
        } else {
            continue;
        };
        let mut it = body.split_whitespace();
        let rva_s = it.next().unwrap_or("");
        let name = it.next().unwrap_or("").trim_end_matches('*').trim();
        if let Some(hex) = rva_s.strip_prefix("0x") {
            if let Ok(rva) = u64::from_str_radix(hex, 16) {
                // index lines have no "---- */"; they are followed by the body
                // anyway, so dedupe by rva later is unnecessary: we only take
                // marker lines that contain the trailing dashes.
                if t.contains("----") {
                    marks.push((start, rva, name.trim_end_matches(" ----").to_string()));
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

/// Find `ret name(params)` before the body brace.
fn signature(slice: &str) -> (String, String) {
    let mut lines: Vec<&str> = Vec::new();
    for line in slice.lines().skip(1) {
        let t = line.trim();
        if t.is_empty() || t.starts_with("//") || t.starts_with("/*") || t.starts_with('*') {
            continue;
        }
        if t == "{" {
            break;
        }
        lines.push(t);
        if t.ends_with(')') {
            // signature may continue on the next line only if it has no ")"; a
            // closing paren finishes it.
            break;
        }
    }
    let sig = lines.join(" ");
    let open = match sig.find('(') {
        Some(i) => i,
        None => return (sig, String::new()),
    };
    let close = sig.rfind(')').unwrap_or(sig.len());
    let head = sig[..open].trim();
    let params = sig[open + 1..close].trim().to_string();
    // return type is everything before the function name
    let ret = head.rsplit_once(' ').map(|(r, _)| r.trim().to_string()).unwrap_or_default();
    (ret, params)
}

fn metrics(slice: &str) -> (usize, usize, usize, bool, bool, String) {
    let lines = slice.lines().count();
    let mut calls = 0usize;
    let mut branches = 0usize;
    for line in slice.lines() {
        if line.contains("if (") || line.contains("if(") || line.contains("while (") || line.contains("switch (") {
            branches += 1;
        }
        // call-ish: identifier immediately followed by '('
        let bytes = line.as_bytes();
        for i in 0..bytes.len() {
            if bytes[i] == b'(' && i > 0 {
                let mut j = i;
                while j > 0 && (bytes[j - 1].is_ascii_alphanumeric() || bytes[j - 1] == b'_') {
                    j -= 1;
                }
                if j < i {
                    let ident = &line[j..i];
                    if !matches!(
                        ident,
                        "if" | "while" | "for" | "switch" | "sizeof" | "return" | "cast"
                    ) {
                        calls += 1;
                    }
                }
            }
        }
    }
    let body: String = match (slice.find('{'), slice.rfind('}')) {
        (Some(a), Some(b)) if b > a => &slice[a + 1..b],
        _ => "",
    }
    .lines()
    .map(|l| l.trim())
    .filter(|l| {
        !l.is_empty() && !l.starts_with("//") && !l.starts_with("/*") && !l.starts_with('*')
    })
    .collect::<Vec<_>>()
    .join("\n");
    let trivial = body.is_empty()
        || body == "return;"
        || (body.starts_with("return") && !body.contains(';') == false && body.matches(';').count() <= 1 && !body.contains('('))
        || body.matches(';').count() <= 1 && !body.contains("if") && !body.contains("for") && !body.contains("while");
    let shape = shape_of(&body);
    (lines, calls, branches, trivial, calls == 0, shape)
}

fn shape_of(body: &str) -> String {
    if body.is_empty() {
        return "empty".into();
    }
    let semi = body.matches(';').count();
    let t = body.trim().trim_end_matches(';').trim();
    if let Some(rest) = t.strip_prefix("return ") {
        let r = rest.trim();
        if !r.contains('(') && !r.contains('[') && r.len() < 32 {
            return format!("const:{r}");
        }
        if r.ends_with(')') && semi == 1 {
            let target = r.split('(').next().unwrap_or("").trim();
            return format!("thunk:{target}");
        }
    }
    if semi == 1 && t.contains(") =") {
        return "setter".into();
    }
    if semi == 1 && t.ends_with(')') && !t.starts_with("return") {
        return "call".into();
    }
    "other".into()
}

fn sanitize_ident(s: &str) -> String {
    let mut o = String::new();
    for c in s.chars() {
        if c.is_ascii_alphanumeric() || c == '_' {
            o.push(c);
        } else {
            o.push('_');
        }
    }
    if o.chars().next().map(|c| c.is_ascii_digit()).unwrap_or(true) {
        o.insert(0, '_');
    }
    o
}

fn rust_type(t: &str) -> String {
    let t = t.trim();
    let is_ptr = t.contains('*');
    let base = t.trim_end_matches('*').trim();
    let b = match base {
        "void" => {
            if is_ptr {
                "u8"
            } else {
                "()"
            }
        }
        "longlong" => "i64",
        "ulonglong" => "u64",
        "undefined8" => "u64",
        "undefined4" => "u32",
        "undefined2" => "u16",
        "undefined1" | "undefined" => "u8",
        "uint" => "u32",
        "int" => "i32",
        "ushort" => "u16",
        "short" => "i16",
        "byte" | "uchar" | "char" => "i8",
        "float" => "f32",
        "double" => "f64",
        "bool" => "bool",
        "code" => "u8",
        _ => "i64",
    };
    if is_ptr {
        format!("*mut {b}")
    } else {
        b.to_string()
    }
}

fn split_params(p: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut cur = String::new();
    for c in p.chars() {
        match c {
            '(' | '[' => {
                depth += 1;
                cur.push(c);
            }
            ')' | ']' => {
                depth -= 1;
                cur.push(c);
            }
            ',' if depth == 0 => {
                out.push(cur.trim().to_string());
                cur.clear();
            }
            _ => cur.push(c),
        }
    }
    if !cur.trim().is_empty() {
        out.push(cur.trim().to_string());
    }
    out
}

fn emit_stubs(rows: &[FnRow], outdir: &Path) -> (usize, usize, usize) {
    let dir = outdir.join("stubs");
    fs::create_dir_all(&dir).ok();
    let mut by_file: BTreeMap<&str, Vec<&FnRow>> = BTreeMap::new();
    for r in rows {
        by_file.entry(r.file.as_str()).or_default().push(r);
    }
    let mut mods = String::from("// generated by portgen - one module per decompiled .c\n");
    let (mut written, mut autobodies, mut skipped) = (0usize, 0usize, 0usize);
    for (file, list) in by_file {
        let stem = sanitize_ident(file.trim_end_matches(".c"));
        mods.push_str(&format!("pub mod {stem};\n"));
        let mut out = String::new();
        out.push_str(&format!("//! port of {file} ({} functions)\n#![allow(dead_code, unused_variables, non_snake_case)]\n\n", list.len()));
        for r in list {
            let name = sanitize_ident(&r.name);
            let ret = rust_type(&r.ret);
            if r.params.contains("...") {
                out.push_str(&format!(
                    "// skipped (variadic) rva 0x{:x} {} :: {}\n\n",
                    r.rva, r.name, r.params
                ));
                skipped += 1;
                continue;
            }
            let mut params = Vec::new();
            for (i, p) in split_params(&r.params).iter().enumerate() {
                if p.is_empty() || p == "void" {
                    continue;
                }
                let toks: Vec<&str> = p.split_whitespace().collect();
                let mut name = toks.last().copied().unwrap_or("").to_string();
                let mut ty = toks[..toks.len().saturating_sub(1)].join(" ");
                while name.starts_with('*') {
                    ty.push('*');
                    name.remove(0);
                }
                if ty.is_empty() {
                    ty = name.clone();
                    name = format!("p{i}");
                }
                let ty = rust_type(&ty);
                let mut pname = name.trim_matches('*');
                if pname.is_empty() || pname.chars().all(|c| c.is_ascii_digit()) {
                    pname = "p";
                }
                let pname = sanitize_ident(pname);
                let pname = if matches!(
                    pname.as_str(),
                    "type" | "in" | "fn" | "let" | "move" | "ref" | "box" | "loop" | "match"
                ) {
                    format!("p_{pname}")
                } else {
                    pname
                };
                params.push(format!("{pname}: {ty}"));
            }
            out.push_str(&format!(
                "/// rva 0x{:x} - shape {} - C: `{} {}({})`\n",
                r.rva, r.shape, r.ret, r.name, r.params
            ));
            out.push_str(&format!(
                "pub fn {name}({}) -> {ret} {{\n",
                params.join(", ")
            ));
            let default = match ret.as_str() {
                "()" => "()".to_string(),
                "bool" => "false".to_string(),
                "f32" | "f64" => "0.0".to_string(),
                t if t.starts_with("*mut") => "core::ptr::null_mut()".to_string(),
                _ => "0".to_string(),
            };
            if let Some(c) = r.shape.strip_prefix("const:") {
                if !c.starts_with('"') && !c.contains("->") {
                    out.push_str(&format!("    {c} // auto: const body\n}}\n\n"));
                    autobodies += 1;
                    written += 1;
                    continue;
                }
            }
            if r.shape == "empty" {
                out.push_str(&format!("    {default} // auto: {}\n}}\n\n", r.shape));
                autobodies += 1;
            } else {
                out.push_str("    unimplemented!(\"not ported yet\")\n}\n\n");
            }
            written += 1;
        }
        fs::write(dir.join(format!("{stem}.rs")), out).ok();
    }
    fs::write(dir.join("mod.rs"), mods).ok();
    (written, autobodies, skipped)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let src = args.get(1).map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"H:\projects\LOL\src"));
    let outdir = args.get(2).map(PathBuf::from).unwrap_or_else(|| PathBuf::from("port"));
    let stubs = args.iter().any(|a| a == "--stubs");
    fs::create_dir_all(&outdir).ok();

    let files = read_dir_c(&src);
    let mut rows: Vec<FnRow> = Vec::new();
    let mut per_file: BTreeMap<String, usize> = BTreeMap::new();
    let t0 = std::time::Instant::now();

    for path in &files {
        let text = match fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("skip {}: {e}", path.display());
                continue;
            }
        };
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        for (rva, fname, slice) in slices(&text) {
            let (ret, params) = signature(slice);
            let (lines, calls, branches, trivial, leaf, shape) = metrics(slice);
            rows.push(FnRow {
                rva,
                name: fname,
                file: name.clone(),
                ret,
                params,
                lines,
                calls,
                branches,
                shape,
                trivial,
                leaf,
            });
            *per_file.entry(name.clone()).or_insert(0) += 1;
        }
    }

    rows.sort_by_key(|r| r.rva);
    rows.dedup_by_key(|r| r.rva);

    let tsv = outdir.join("functions.tsv");
    let f = fs::File::create(&tsv).expect("create functions.tsv");
    let mut w = BufWriter::new(f);
    writeln!(w, "rva\tname\tfile\tret\tparams\tlines\tcalls\tbranches\ttrivial\tleaf\tshape").unwrap();
    for r in &rows {
        writeln!(
            w,
            "0x{:x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            r.rva,
            r.name,
            r.file,
            r.ret.replace('\t', " "),
            r.params.replace('\t', " "),
            r.lines,
            r.calls,
            r.branches,
            r.trivial as u8,
            r.leaf as u8,
            r.shape
        )
        .unwrap();
    }
    w.flush().unwrap();

    let total = rows.len();
    let trivial = rows.iter().filter(|r| r.trivial).count();
    let leaf = rows.iter().filter(|r| r.leaf).count();
    let both = rows.iter().filter(|r| r.trivial && r.leaf).count();
    let big = rows.iter().filter(|r| r.lines > 400).count();
    let named = rows.iter().filter(|r| !r.name.starts_with("sub_")).count();
    let callers: u64 = rows.iter().map(|r| r.calls as u64).sum();

    println!("files            : {}", files.len());
    println!("functions        : {total}");
    println!("named            : {named}");
    println!("trivial bodies   : {trivial}");
    println!("leaf (no calls)  : {leaf}");
    println!("trivial + leaf   : {both}");
    println!("large (>400 ln)  : {big}");
    println!("call edges (approx): {callers}");
    println!("elapsed          : {:?}", t0.elapsed());
    println!("wrote {}", tsv.display());

    let report = outdir.join("STATS.md");
    let mut r = String::new();
    r.push_str("# Mechanical pass over the decompile\n\n");
    r.push_str(&format!("- files: {}\n- functions: **{total}**\n", files.len()));
    r.push_str(&format!("- already named: {named}\n"));
    r.push_str(&format!("- trivial bodies: {trivial}\n"));
    r.push_str(&format!("- leaf functions: {leaf}\n"));
    r.push_str(&format!("- trivial and leaf: {both}\n"));
    r.push_str(&format!("- larger than 400 lines: {big}\n"));
    r.push_str(&format!("- approximate call sites: {callers}\n\n"));
    r.push_str("Full table: `functions.tsv` (rva, name, file, ret, params, lines, calls, branches, trivial, leaf).\n");
    fs::write(report, r).ok();

    if stubs {
        let (written, autobodies, skipped) = emit_stubs(&rows, &outdir);
        println!("stubs written    : {written}");
        println!("auto-bodied      : {autobodies}");
        println!("skipped variadic : {skipped}");
    }

    // Top files by function count.
    let mut v: Vec<_> = per_file.into_iter().collect();
    v.sort_by_key(|(_, n)| std::cmp::Reverse(*n));
    println!("top files:");
    for (f, n) in v.iter().take(8) {
        println!("  {n:5}  {f}");
    }
}
fn classify(body: &str) -> &'static str {
    if body.is_empty() {
        return "empty";
    }
    let semi = body.matches(';').count();
    if semi == 1 {
        let t = body.trim().trim_end_matches(';').trim();
        if let Some(rest) = t.strip_prefix("return ") {
            let r = rest.trim();
            if !r.contains('(') {
                return "const";
            }
            if r.ends_with(')') && !r.contains("?") {
                return "thunk";
            }
        }
        if t.starts_with('*') && t.contains(") =") {
            return "setter";
        }
        if t.ends_with(')') && !t.starts_with("return") && !t.contains(" = ") {
            return "call";
        }
    }
    "other"
}

