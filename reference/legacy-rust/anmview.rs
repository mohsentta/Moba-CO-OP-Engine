//! Interactive animation viewer: plays `.anm` clips on a champion model.
//!
//! anmview <wad> <skl> <skn> <anims.tsv>
//! Keys: space pause, left/right previous/next clip, up/down speed, esc quit.

#[path = "../wad.rs"]
mod wad;
#[path = "../skn.rs"]
mod skn;
#[path = "../skl.rs"]
mod skl;
#[path = "../anm.rs"]
mod anm;
#[path = "../render.rs"]
mod render;

use skl::{mul, trs, xform, xform_dir};
use std::collections::HashMap;

type Hwnd = *mut core::ffi::c_void;
type Hdc = *mut core::ffi::c_void;

#[repr(C)]
struct Msg {
    hwnd: Hwnd,
    message: u32,
    wparam: usize,
    lparam: isize,
    time: u32,
    pt_x: i32,
    pt_y: i32,
}

#[repr(C)]
struct WndClassW {
    style: u32,
    wnd_proc: extern "system" fn(Hwnd, u32, usize, isize) -> isize,
    cls_extra: i32,
    wnd_extra: i32,
    instance: *mut core::ffi::c_void,
    icon: *mut core::ffi::c_void,
    cursor: *mut core::ffi::c_void,
    background: *mut core::ffi::c_void,
    menu_name: *const u16,
    class_name: *const u16,
}

#[repr(C)]
#[repr(C)]
#[derive(Default)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

struct BitmapInfoHeader {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    size_image: u32,
    x_ppm: i32,
    y_ppm: i32,
    clr_used: u32,
    clr_important: u32,
}

#[repr(C)]
struct BitmapInfo {
    header: BitmapInfoHeader,
    colors: [u32; 256],
}

#[link(name = "user32")]
#[link(name = "gdi32")]
extern "system" {
    fn GetModuleHandleW(name: *const u16) -> *mut core::ffi::c_void;
    fn RegisterClassW(class: *const WndClassW) -> u16;
    fn CreateWindowExW(
        ex: u32,
        class: *const u16,
        title: *const u16,
        style: u32,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        parent: Hwnd,
        menu: *mut core::ffi::c_void,
        inst: *mut core::ffi::c_void,
        param: *mut core::ffi::c_void,
    ) -> Hwnd;
    fn DefWindowProcW(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> isize;
    fn DestroyWindow(hwnd: Hwnd) -> i32;
    fn PostQuitMessage(code: i32);
    fn PeekMessageW(msg: *mut Msg, hwnd: Hwnd, min: u32, max: u32, remove: u32) -> i32;
    fn TranslateMessage(msg: *const Msg) -> i32;
    fn DispatchMessageW(msg: *const Msg) -> isize;
    fn ShowWindow(hwnd: Hwnd, cmd: i32) -> i32;
    fn GetDC(hwnd: Hwnd) -> Hdc;
    fn ReleaseDC(hwnd: Hwnd, dc: Hdc) -> i32;
    fn StretchDIBits(
        dc: Hdc,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        sx: i32,
        sy: i32,
        sw: i32,
        sh: i32,
        bits: *const u8,
        info: *const BitmapInfo,
        usage: u32,
        rop: u32,
    ) -> i32;
    fn GetAsyncKeyState(vk: i32) -> i16;
    fn SetWindowTextW(hwnd: Hwnd, text: *const u16) -> i32;
    fn InvalidateRect(hwnd: Hwnd, rect: *const core::ffi::c_void, erase: i32) -> i32;
    fn UpdateWindow(hwnd: Hwnd) -> i32;
    fn GetClientRect(hwnd: Hwnd, rect: *mut Rect) -> i32;
}

static mut WND: Hwnd = core::ptr::null_mut();

extern "system" fn wnd_proc(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> isize {
    unsafe {
        match msg {
            0x0010 | 0x0002 => {
                // WM_CLOSE / WM_DESTROY
                DestroyWindow(hwnd);
                PostQuitMessage(0);
                0
            }
            0x0005 => {
                // WM_PAINT: validated in the render loop
                let _ = InvalidateRect(hwnd, core::ptr::null(), 0);
                0
            }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

struct Clip {
    hash: u64,
    anm: anm::Anm,
    name: String,
}

fn load_anm(w: &wad::Wad, hash: u64) -> Option<anm::Anm> {
    let e = w.find_hash(hash)?;
    let d = w.read_chunk(e).ok()?;
    anm::Anm::parse(&d).ok()
}

fn skin(
    rig: &skl::Skl,
    mesh: &skn::Skn,
    ev: &HashMap<u32, ([f32; 4], [f32; 3], [f32; 3])>,
    remap: &[usize],
) -> skn::Skn {
    let n = rig.joints.len();
    let mut globals = vec![skl::identity(); n];
    let mut done = vec![false; n];
    fn build(
        i: usize,
        rig: &skl::Skl,
        ev: &HashMap<u32, ([f32; 4], [f32; 3], [f32; 3])>,
        out: &mut Vec<[[f32; 4]; 4]>,
        done: &mut Vec<bool>,
    ) -> [[f32; 4]; 4] {
        if done[i] {
            return out[i];
        }
        let j = &rig.joints[i];
        let (t, s, q) = match ev.get(&j.hash) {
            Some((r, tt, ss)) => (*tt, *ss, *r),
            None => (j.t, j.s, j.q),
        };
        let local = trs(t, s, q);
        let g = if j.parent >= 0 && (j.parent as usize) < rig.joints.len() {
            let pg = build(j.parent as usize, rig, ev, out, done);
            mul(local, pg)
        } else {
            local
        };
        out[i] = g;
        done[i] = true;
        g
    }
    for i in 0..n {
        build(i, rig, ev, &mut globals, &mut done);
    }
    let bind = rig.bind_globals();
    let mut skins = vec![skl::identity(); n];
    for i in 0..n {
        skins[i] = mul(skl::invert(bind[i]), globals[i]);
    }
    let mut out = skn::Skn {
        major: mesh.major,
        minor: mesh.minor,
        materials: mesh.materials.clone(),
        indices: mesh.indices.clone(),
        vertices: mesh.vertices.clone(),
        vertex_stride: mesh.vertex_stride,
        bbox: mesh.bbox,
    };
    for v in &mut out.vertices {
        let mut p = [0.0f32; 3];
        let mut nrm = [0.0f32; 3];
        let mut ws = 0.0f32;
        for k in 0..4 {
            let wgt = v.weights[k];
            if wgt <= 0.0 {
                continue;
            }
            let idx = *remap.get(v.bone_indices[k] as usize).unwrap_or(&usize::MAX);
            if idx >= n {
                continue;
            }
            let sp = xform(skins[idx], v.position);
            p[0] += sp[0] * wgt;
            p[1] += sp[1] * wgt;
            p[2] += sp[2] * wgt;
            let sn = xform_dir(skins[idx], v.normal);
            nrm[0] += sn[0] * wgt;
            nrm[1] += sn[1] * wgt;
            nrm[2] += sn[2] * wgt;
            ws += wgt;
        }
        if ws > 1e-6 {
            v.position = [p[0] / ws, p[1] / ws, p[2] / ws];
            let l = (nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]).sqrt();
            if l > 1e-6 {
                v.normal = [nrm[0] / l, nrm[1] / l, nrm[2] / l];
            }
        }
    }
    out
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("anmview <wad> <skl> <skn> <anims.tsv>");
        std::process::exit(2);
    }
    let w = wad::Wad::open(&a[1]).expect("open wad");
    let rig = skl::Skl::parse(&w.read_path(&a[2]).expect("skl")).expect("parse skl");
    let mesh = skn::Skn::parse(&w.read_path(&a[3]).expect("skn")).expect("parse skn");

    let mut id_to_joint: HashMap<i16, usize> = HashMap::new();
    for (i, j) in rig.joints.iter().enumerate() {
        id_to_joint.insert(j.id, i);
    }
    let mut remap: Vec<usize> = rig
        .influences
        .iter()
        .map(|id| id_to_joint.get(id).copied().unwrap_or(usize::MAX))
        .collect();
    remap.resize(256, usize::MAX);

    let list = std::fs::read_to_string(&a[4]).expect("anims.tsv");
    let mut clips: Vec<(u64, f32, usize)> = Vec::new();
    for line in list.lines() {
        let c: Vec<&str> = line.split('\t').collect();
        if c.len() < 5 {
            continue;
        }
        let hash = u64::from_str_radix(c[0], 16).unwrap_or(0);
        let dur: f32 = c[3].parse().unwrap_or(0.0);
        let tracks: usize = c[4].parse().unwrap_or(0);
        if dur >= 0.3 && tracks >= 100 {
            clips.push((hash, dur, tracks));
        }
    }
    clips.sort_by_key(|c| c.0);
    println!("clips: {}", clips.len());
    if clips.is_empty() {
        return;
    }

    let (cw, ch) = (520i32, 660i32);
    unsafe {
        let hinst = GetModuleHandleW(core::ptr::null());
        let class = wide("anmview");
        let wc = WndClassW {
            style: 0x0020,
            wnd_proc,
            cls_extra: 0,
            wnd_extra: 0,
            instance: hinst,
            icon: core::ptr::null_mut(),
            cursor: core::ptr::null_mut(),
            background: core::ptr::null_mut(),
            menu_name: core::ptr::null(),
            class_name: class.as_ptr(),
        };
        if RegisterClassW(&wc) == 0 {
            eprintln!("RegisterClass failed");
            return;
        }
        let title = wide("Ahri animation viewer");
        let hwnd = CreateWindowExW(
            0,
            class.as_ptr(),
            title.as_ptr(),
            0x10CF_0000,
            100,
            60,
            cw,
            ch,
            core::ptr::null_mut(),
            core::ptr::null_mut(),
            hinst,
            core::ptr::null_mut(),
        );
        if hwnd.is_null() {
            eprintln!("CreateWindow failed");
            return;
        }
        WND = hwnd;
        ShowWindow(hwnd, 1);
        UpdateWindow(hwnd);

        let mut bmi = BitmapInfo {
            header: BitmapInfoHeader {
                size: core::mem::size_of::<BitmapInfoHeader>() as u32,
                width: cw,
                height: -ch,
                planes: 1,
                bit_count: 32,
                compression: 0,
                size_image: (cw * ch * 4) as u32,
                x_ppm: 0,
                y_ppm: 0,
                clr_used: 0,
                clr_important: 0,
            },
            colors: [0; 256],
        };

        let mut idx = 0usize;
        let mut t = 0.0f32;
        let mut speed = 1.0f32;
        let mut paused = false;
        let mut clip: Option<Clip> = None;
        let mut prev_keys: [bool; 6] = [false; 6];
        let mut msg = Msg {
            hwnd: core::ptr::null_mut(),
            message: 0,
            wparam: 0,
            lparam: 0,
            time: 0,
            pt_x: 0,
            pt_y: 0,
        };
        let mut last = std::time::Instant::now();
        let mut frames = 0u32;
        let mut fps_t = 0.0f32;

        loop {
            while PeekMessageW(&mut msg, core::ptr::null_mut(), 0, 0, 1) != 0 {
                if msg.message == 0x0012 {
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            let keys = [
                GetAsyncKeyState(0x1B) < 0,
                GetAsyncKeyState(0x20) < 0,
                GetAsyncKeyState(0x27) < 0,
                GetAsyncKeyState(0x25) < 0,
                GetAsyncKeyState(0x26) < 0,
                GetAsyncKeyState(0x28) < 0,
            ];
            if keys[0] {
                return;
            }
            if keys[1] && !prev_keys[1] {
                paused = !paused;
            }
            if keys[2] && !prev_keys[2] {
                idx = (idx + 1) % clips.len();
                clip = None;
            }
            if keys[3] && !prev_keys[3] {
                idx = (idx + clips.len() - 1) % clips.len();
                clip = None;
            }
            if keys[4] {
                speed = (speed * 1.02).min(4.0);
            }
            if keys[5] {
                speed = (speed / 1.02).max(0.05);
            }
            prev_keys = keys;

            if clip.is_none() {
                if let Some(x) = load_anm(&w, clips[idx].0) {
                    clip = Some(Clip {
                        hash: clips[idx].0,
                        anm: x,
                        name: format!("#{idx}"),
                    });
                    t = 0.0;
                } else {
                    idx = (idx + 1) % clips.len();
                }
            }

            let dt = last.elapsed().as_secs_f32();
            last = std::time::Instant::now();
            if let Some(c) = &clip {
                if !paused {
                    t = (t + dt * speed) % c.anm.duration.max(0.001);
                }
                let ev = c.anm.evaluate(t);
                let posed = skin(&rig, &mesh, &ev, &remap);
                let rgba = render::render_frame(&posed, cw as u32, ch as u32, Some(t / c.anm.duration.max(0.001)), 1)
                    .unwrap_or_else(|_| vec![0u8; (cw * ch * 4) as usize]);
                let mut bgra = vec![0u8; (cw * ch * 4) as usize];
                let npix = (cw * ch) as usize;
                for i in 0..npix {
                    bgra[i * 4] = rgba[i * 3 + 2];
                    bgra[i * 4 + 1] = rgba[i * 3 + 1];
                    bgra[i * 4 + 2] = rgba[i * 3];
                    bgra[i * 4 + 3] = 255;
                }
                let mut rc = Rect::default();
                GetClientRect(hwnd, &mut rc);
                let dc = GetDC(hwnd);
                StretchDIBits(
                    dc,
                    0,
                    0,
                    rc.right - rc.left,
                    rc.bottom - rc.top,
                    0,
                    0,
                    cw,
                    ch,
                    bgra.as_ptr(),
                    &bmi,
                    0,
                    0x00CC0020,
                );
                ReleaseDC(hwnd, dc);

                fps_t += dt;
                frames += 1;
                if fps_t > 0.5 {
                    let fps = frames as f32 / fps_t;
                    let title = format!(
                        "Ahri  clip {}  hash={:016x}  {:.2}s  t={:.2}  speed={:.2}  {:.0} fps{}",
                        c.name,
                        c.hash,
                        c.anm.duration,
                        t,
                        speed,
                        fps,
                        if paused { "  [paused]" } else { "" }
                    );
                    let wt = wide(&title);
                    SetWindowTextW(hwnd, wt.as_ptr());
                    fps_t = 0.0;
                    frames = 0;
                }
            }
            std::thread::sleep(std::time::Duration::from_millis(16));
        }
    }
}



