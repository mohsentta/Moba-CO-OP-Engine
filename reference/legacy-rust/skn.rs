//! `.skn` (skinned mesh) reader.
//!
//! Layout recovered from `ASSETS/Characters/Ahri/Skins/Base/Ahri_Base.skn`
//! (version 4.1) and cross-checked against the file size:
//!
//! ```text
//! u32 magic = 0x00112233
//! u16 major, u16 minor
//! u32 material_count
//! material[count]:
//!     char name[64]
//!     u32 start_vertex
//!     u32 num_vertices
//!     u32 start_index
//!     u32 num_indices
//! u32 unk0
//! u32 index_count
//! u32 vertex_count
//! u32 vertex_stride      (52 in this build)
//! u32 unk1
//! f32 bbox[10]
//! u16 indices[index_count]
//! vertex[vertex_count] (vertex_stride bytes each):
//!     f32 position[3]
//!     u8  bone_indices[4]
//!     f32 weights[4]
//!     f32 normal[3]
//!     f32 uv[2]
//! ```
//!
//! The vertex positions are already in bind pose, so a static render needs no
//! skinning (the `.skl` skeleton is only needed to animate).

pub const MAGIC: u32 = 0x0011_2233;
pub const VERTEX_STRIDE: usize = 52;

#[derive(Clone, Debug)]
pub struct Material {
    pub name: String,
    pub start_vertex: u32,
    pub num_vertices: u32,
    pub start_index: u32,
    pub num_indices: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Vertex {
    pub position: [f32; 3],
    pub bone_indices: [u8; 4],
    pub weights: [f32; 4],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
}

#[derive(Debug)]
pub struct Skn {
    pub major: u16,
    pub minor: u16,
    pub materials: Vec<Material>,
    pub indices: Vec<u16>,
    pub vertices: Vec<Vertex>,
    pub vertex_stride: u32,
    pub bbox: [f32; 10],
}

struct Cursor<'a> {
    d: &'a [u8],
    o: usize,
}

impl<'a> Cursor<'a> {
    fn u32(&mut self) -> Result<u32, String> {
        if self.o + 4 > self.d.len() {
            return Err(format!("skn: eof at {} reading u32", self.o));
        }
        let v = u32::from_le_bytes(self.d[self.o..self.o + 4].try_into().unwrap());
        self.o += 4;
        Ok(v)
    }
    fn u16(&mut self) -> Result<u16, String> {
        if self.o + 2 > self.d.len() {
            return Err(format!("skn: eof at {} reading u16", self.o));
        }
        let v = u16::from_le_bytes(self.d[self.o..self.o + 2].try_into().unwrap());
        self.o += 2;
        Ok(v)
    }
    fn f32(&mut self) -> Result<f32, String> {
        Ok(f32::from_bits(self.u32()?))
    }
    fn bytes(&mut self, n: usize) -> Result<&'a [u8], String> {
        if self.o + n > self.d.len() {
            return Err(format!("skn: eof at {} reading {} bytes", self.o, n));
        }
        let s = &self.d[self.o..self.o + n];
        self.o += n;
        Ok(s)
    }
    fn cstr(&mut self, n: usize) -> String {
        let s = self.bytes(n).unwrap_or(&[]);
        let end = s.iter().position(|&b| b == 0).unwrap_or(s.len());
        String::from_utf8_lossy(&s[..end]).to_string()
    }
}

impl Skn {
    pub fn parse(d: &[u8]) -> Result<Skn, String> {
        if d.len() < 12 {
            return Err("skn: too small".into());
        }
        let major = u16::from_le_bytes([d[4], d[5]]);
        // Version 4 inserts a block of metadata (stride + bbox) between the
        // materials and the index buffer. Versions 1/2/3 go straight to the
        // counts. Try the layout the version implies, then the other one.
        let order: [u8; 2] = if major >= 4 { [4, 1] } else { [1, 4] };
        let mut last_err = String::new();
        for layout in order {
            match parse_layout(d, layout) {
                Ok(s) => return Ok(s),
                Err(e) => last_err = e,
            }
        }
        Err(last_err)
    }

    /// Draw only triangles whose material range is non-empty, using the
    pub fn triangles(&self) -> Vec<[usize; 3]> {
        let mut out = Vec::new();
        let mut ranges: Vec<(usize, usize)> = self
            .materials
            .iter()
            .filter(|m| m.num_indices > 0)
            .map(|m| (m.start_index as usize, m.num_indices as usize))
            .collect();
        if ranges.is_empty() {
            ranges.push((0, self.indices.len()));
        }
        for (start, count) in ranges {
            let end = (start + count).min(self.indices.len());
            let mut i = start;
            while i + 2 < end {
                let (a, b, cc) = (
                    self.indices[i] as usize,
                    self.indices[i + 1] as usize,
                    self.indices[i + 2] as usize,
                );
                if a < self.vertices.len() && b < self.vertices.len() && cc < self.vertices.len() {
                    out.push([a, b, cc]);
                }
                i += 3;
            }
        }
        out
    }

    pub fn bounds(&self) -> ([f32; 3], [f32; 3]) {
        let mut mn = [f32::INFINITY; 3];
        let mut mx = [f32::NEG_INFINITY; 3];
        for v in &self.vertices {
            for k in 0..3 {
                mn[k] = mn[k].min(v.position[k]);
                mx[k] = mx[k].max(v.position[k]);
            }
        }
        (mn, mx)
    }
}

fn parse_layout(d: &[u8], layout: u8) -> Result<Skn, String> {
    let mut c = Cursor { d, o: 0 };
    let magic = c.u32()?;
    if magic != MAGIC {
        return Err(format!("skn: bad magic {magic:08x}"));
    }
    let major = c.u16()?;
    let minor = c.u16()?;
    let mat_count = c.u32()? as usize;
    if mat_count > 4096 {
        return Err(format!("skn: implausible material count {mat_count}"));
    }
    let mut materials = Vec::with_capacity(mat_count);
    for _ in 0..mat_count {
        materials.push(Material {
            name: c.cstr(64),
            start_vertex: c.u32()?,
            num_vertices: c.u32()?,
            start_index: c.u32()?,
            num_indices: c.u32()?,
        });
    }
    let (index_count, vertex_count, vertex_stride, bbox);
    if layout == 4 {
        let _unk0 = c.u32()?;
        index_count = c.u32()? as usize;
        vertex_count = c.u32()? as usize;
        vertex_stride = c.u32()?;
        let _unk1 = c.u32()?;
        let mut bb = [0f32; 10];
        for b in &mut bb {
            *b = c.f32()?;
        }
        bbox = bb;
    } else {
        index_count = c.u32()? as usize;
        vertex_count = c.u32()? as usize;
        vertex_stride = VERTEX_STRIDE as u32;
        bbox = [0f32; 10];
    }
    if vertex_stride < VERTEX_STRIDE as u32 || vertex_stride > 256 {
        return Err(format!("skn: implausible vertex stride {vertex_stride}"));
    }
    if index_count > 200_000_000 || vertex_count > 50_000_000 {
        return Err("skn: implausible counts".into());
    }
    let need = index_count * 2 + vertex_count * vertex_stride as usize;
    if c.o + need > d.len() {
        return Err(format!(
            "skn: counts need {} bytes, only {} remain",
            need,
            d.len() - c.o
        ));
    }

    let mut indices = Vec::with_capacity(index_count);
    for _ in 0..index_count {
        indices.push(c.u16()?);
    }
    let mut vertices = Vec::with_capacity(vertex_count);
    for _ in 0..vertex_count {
        let base = c.o;
        let position = [c.f32()?, c.f32()?, c.f32()?];
        let bi = c.bytes(4)?;
        let bone_indices = [bi[0], bi[1], bi[2], bi[3]];
        let weights = [c.f32()?, c.f32()?, c.f32()?, c.f32()?];
        let normal = [c.f32()?, c.f32()?, c.f32()?];
        let uv = [c.f32()?, c.f32()?];
        vertices.push(Vertex {
            position,
            bone_indices,
            weights,
            normal,
            uv,
        });
        c.o = base + vertex_stride as usize;
    }
    // The mesh should account for the file, modulo small alignment padding.
    let left = d.len() - c.o;
    if left > 64 {
        return Err(format!("skn: {left} bytes left after vertices (wrong layout)"));
    }
    Ok(Skn {
        major,
        minor,
        materials,
        indices,
        vertices,
        vertex_stride,
        bbox,
    })
}
