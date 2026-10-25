//! Minimal Win32 window + software renderer + replay clock.
//!
//! No GPU or windowing crates are available offline, so this talks to user32
//! and gdi32 directly and blits a 32-bit DIB.

use crate::render;
use crate::runtime::ReplayMode;
use crate::skn::Skn;

use std::ffi::c_void;

type Hwnd = isize;
type Hdc = isize;

#[repr(C)]
struct WndClassW {
    style: u32,
    lpfn_wnd_proc: usize,
    cb_cls_extra: i32,
    cb_wnd_extra: i32,
    h_instance: isize,
    h_icon: isize,
    h_cursor: isize,
    hbr_background: isize,
    lpsz_menu_name: *const u16,
    lpsz_class_name: *const u16,
}

#[repr(C)]
struct Point {
    x: i32,
    y: i32,
}

#[repr(C)]
struct Msg {
    hwnd: Hwnd,
    message: u32,
    w_param: usize,
    l_param: isize,
    time: u32,
    pt: Point,
    l_private: u32,
}

#[repr(C)]
struct BitmapInfoHeader {
    bi_size: u32,
    bi_width: i32,
    bi_height: i32,
    bi_planes: u16,
    bi_bit_count: u16,
    bi_compression: u32,
    bi_size_image: u32,
    bi_x_pels_per_meter: i32,
    bi_y_pels_per_meter: i32,
    bi_clr_used: u32,
    bi_clr_important: u32,
}

#[repr(C)]
struct RgbQuad {
    b: u8,
    g: u8,
    r: u8,
    a: u8,
}

#[repr(C)]
struct BitmapInfo {
    header: BitmapInfoHeader,
    colors: [RgbQuad; 1],
}

const WM_DESTROY: u32 = 0x0002;
const WM_CLOSE: u32 = 0x0010;
const PM_REMOVE: u32 = 0x0001;
const DIB_RGB_COLORS: u32 = 0;
const SRCCOPY: u32 = 0x00CC_0020;
const WS_OVERLAPPEDWINDOW: u32 = 0x00CF_0000;
const SW_SHOW: i32 = 5;

#[link(name = "user32")]
extern "system" {
    fn RegisterClassW(class: *const WndClassW) -> u16;
    fn CreateWindowExW(
        ex_style: u32,
        class_name: *const u16,
        window_name: *const u16,
        style: u32,
        x: i32,
        y: i32,
        w: i32,
        h: i32,
        parent: isize,
        menu: isize,
        instance: isize,
        param: *const c_void,
    ) -> Hwnd;
    fn DefWindowProcW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> isize;
    fn ShowWindow(hwnd: Hwnd, cmd: i32) -> i32;
    fn UpdateWindow(hwnd: Hwnd) -> i32;
    fn PeekMessageW(msg: *mut Msg, hwnd: Hwnd, min: u32, max: u32, remove: u32) -> i32;
    fn TranslateMessage(msg: *const Msg) -> i32;
    fn DispatchMessageW(msg: *const Msg) -> isize;
    fn PostQuitMessage(code: i32);
    fn SetWindowTextW(hwnd: Hwnd, text: *const u16) -> i32;
    fn GetDC(hwnd: Hwnd) -> Hdc;
    fn ReleaseDC(hwnd: Hwnd, dc: Hdc) -> i32;
    fn GetAsyncKeyState(key: i32) -> i16;
    fn DestroyWindow(hwnd: Hwnd) -> i32;
}

#[link(name = "gdi32")]
extern "system" {
    fn CreateDIBSection(
        dc: Hdc,
        info: *const BitmapInfo,
        usage: u32,
        bits: *mut *mut c_void,
        section: isize,
        offset: u32,
    ) -> isize;
    fn StretchDIBits(
        dc: Hdc,
        x_dest: i32,
        y_dest: i32,
        w_dest: i32,
        h_dest: i32,
        x_src: i32,
        y_src: i32,
        w_src: i32,
        h_src: i32,
        bits: *const c_void,
        info: *const BitmapInfo,
        usage: u32,
        rop: u32,
    ) -> i32;
    fn DeleteObject(obj: isize) -> i32;
}

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(name: *const u16) -> isize;
}

extern "system" fn wnd_proc(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> isize {
    match msg {
        WM_CLOSE => {
            unsafe {
                DestroyWindow(hwnd);
            }
            0
        }
        WM_DESTROY => {
            unsafe {
                PostQuitMessage(0);
            }
            0
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn key_down(vk: i32) -> bool {
    unsafe { (GetAsyncKeyState(vk) as u16 & 0x8000) != 0 }
}

pub struct Controls {
    pub paused: bool,
    pub speed: f32,
    prev_space: bool,
    prev_left: bool,
    prev_right: bool,
    prev_up: bool,
    prev_down: bool,
}

impl Controls {
    fn new() -> Controls {
        Controls {
            paused: false,
            speed: 1.0,
            prev_space: false,
            prev_left: false,
            prev_right: false,
            prev_up: false,
            prev_down: false,
        }
    }

    /// Returns an optional seek target (seconds).
    fn poll(&mut self) -> Option<f32> {
        let mut seek = None;
        let space = key_down(0x20);
        if space && !self.prev_space {
            self.paused = !self.paused;
        }
        self.prev_space = space;

        let left = key_down(0x25);
        if left && !self.prev_left {
            seek = Some(-10.0);
        }
        self.prev_left = left;

        let right = key_down(0x27);
        if right && !self.prev_right {
            seek = Some(10.0);
        }
        self.prev_right = right;

        let up = key_down(0x26);
        if up && !self.prev_up {
            self.speed = (self.speed * 2.0).min(32.0);
        }
        self.prev_up = up;

        let down = key_down(0x28);
        if down && !self.prev_down {
            self.speed = (self.speed / 2.0).max(0.25);
        }
        self.prev_down = down;
        seek
    }
}

/// Open the viewer and run until the window closes.
pub fn run(
    mesh: &Skn,
    replay: &mut ReplayMode,
    width: u32,
    height: u32,
    label: &str,
) -> Result<(), String> {
    unsafe {
        let instance = GetModuleHandleW(std::ptr::null());
        let class_name = wide("ReplayPlayer2Window");
        let class = WndClassW {
            style: 0,
            lpfn_wnd_proc: wnd_proc as usize,
            cb_cls_extra: 0,
            cb_wnd_extra: 0,
            h_instance: instance,
            h_icon: 0,
            h_cursor: 0,
            hbr_background: 0,
            lpsz_menu_name: std::ptr::null(),
            lpsz_class_name: class_name.as_ptr(),
        };
        if RegisterClassW(&class) == 0 {
            return Err("RegisterClassW failed".into());
        }
        let title = wide(&format!("replay player 2 - {label}"));
        let hwnd = CreateWindowExW(
            0,
            class_name.as_ptr(),
            title.as_ptr(),
            WS_OVERLAPPEDWINDOW,
            80,
            60,
            width as i32,
            height as i32,
            0,
            0,
            instance,
            std::ptr::null(),
        );
        if hwnd == 0 {
            return Err("CreateWindowExW failed".into());
        }
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        let dc = GetDC(hwnd);
        if dc == 0 {
            return Err("GetDC failed".into());
        }
        let mut bmi = BitmapInfo {
            header: BitmapInfoHeader {
                bi_size: std::mem::size_of::<BitmapInfoHeader>() as u32,
                bi_width: width as i32,
                bi_height: -(height as i32),
                bi_planes: 1,
                bi_bit_count: 32,
                bi_compression: 0,
                bi_size_image: 0,
                bi_x_pels_per_meter: 0,
                bi_y_pels_per_meter: 0,
                bi_clr_used: 0,
                bi_clr_important: 0,
            },
            colors: [RgbQuad { b: 0, g: 0, r: 0, a: 0 }],
        };
        let mut bits: *mut c_void = std::ptr::null_mut();
        let dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &mut bits, 0, 0);
        if dib == 0 || bits.is_null() {
            return Err("CreateDIBSection failed".into());
        }

        let mut controls = Controls::new();
        let mut last = std::time::Instant::now();
        let mut msg: Msg = std::mem::zeroed();
        let mut last_title = String::new();
        let frame_time = std::time::Duration::from_millis(33);

        loop {
            while PeekMessageW(&mut msg, 0, 0, 0, PM_REMOVE) != 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if key_down(0x1B) {
                break;
            }
            if let Some(delta) = controls.poll() {
                let t = (replay.clock + delta).clamp(0.0, replay.timeline.duration());
                replay.seek(t);
            }

            let now = std::time::Instant::now();
            let dt = now.duration_since(last).as_secs_f32().min(0.1);
            last = now;
            if !controls.paused {
                replay.frame(dt * controls.speed);
            }

            let frac = if replay.timeline.duration() > 0.0 {
                replay.last_event_time / replay.timeline.duration()
            } else {
                0.0
            };
            let rgb = render::render_frame(mesh, width, height, Some(frac), 1)?;
            let dst = std::slice::from_raw_parts_mut(bits as *mut u8, (width * height * 4) as usize);
            for i in 0..(width * height) as usize {
                dst[i * 4] = rgb[i * 3 + 2];
                dst[i * 4 + 1] = rgb[i * 3 + 1];
                dst[i * 4 + 2] = rgb[i * 3];
                dst[i * 4 + 3] = 0;
            }
            StretchDIBits(
                dc,
                0,
                0,
                width as i32,
                height as i32,
                0,
                0,
                width as i32,
                height as i32,
                bits,
                &bmi,
                DIB_RGB_COLORS,
                SRCCOPY,
            );

            let title = format!(
                "replay player 2 - {label} - {:02}:{:02} / {:02}:{:02}  [{}x]{}",
                (replay.last_event_time as u32) / 60,
                (replay.last_event_time as u32) % 60,
                (replay.timeline.duration() as u32) / 60,
                (replay.timeline.duration() as u32) % 60,
                controls.speed,
                if controls.paused { "  PAUSED" } else { "" }
            );
            if title != last_title {
                let w = wide(&title);
                SetWindowTextW(hwnd, w.as_ptr());
                last_title = title;
            }

            let elapsed = now.elapsed();
            if elapsed < frame_time {
                std::thread::sleep(frame_time - elapsed);
            }
        }

        ReleaseDC(hwnd, dc);
        DeleteObject(dib);
    }
    Ok(())
}
