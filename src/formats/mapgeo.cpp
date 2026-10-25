#include <lolview/mapgeo.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace lolview {
namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    void need(std::size_t n) const {
        if (pos_ + n > data_.size()) {
            throw std::runtime_error("mapgeo: unexpected end of file at " +
                                     std::to_string(pos_));
        }
    }

    std::uint8_t u8() {
        need(1);
        return data_[pos_++];
    }
    std::uint16_t u16() {
        need(2);
        const std::uint16_t v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        need(4);
        const std::uint32_t v = static_cast<std::uint32_t>(data_[pos_]) |
                                (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                                (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                                (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string string() {
        const std::uint32_t length = u32();
        need(length);
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), length);
        pos_ += length;
        return s;
    }
    void skip(std::size_t n) {
        need(n);
        pos_ += n;
    }
    void seek(std::size_t position) { pos_ = position; }
    std::size_t position() const noexcept { return pos_; }
    std::size_t size() const noexcept { return data_.size(); }
    std::span<const std::uint8_t> span(std::size_t at, std::size_t n) const {
        return data_.subspan(at, n);
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

struct Element {
    std::uint32_t name = 0;
    std::uint32_t format = 0;
    std::uint32_t size = 0;
};

struct Declaration {
    std::vector<Element> elements;
    std::uint32_t stride = 0;
};

std::uint32_t element_size(std::uint32_t format) {
    switch (format) {
        case 0: return 4;   // X_Float32
        case 1: return 8;   // XY_Float32
        case 2: return 12;  // XYZ_Float32
        case 3: return 16;  // XYZW_Float32
        case 4: return 4;   // BGRA_Packed8888
        case 5: return 4;   // ZYXW_Packed8888
        case 6: return 4;   // RGBA_Packed8888
        case 7: return 4;   // XY_Packed1616
        case 8: return 8;   // XYZ_Packed161616
        case 9: return 8;   // XYZW_Packed16161616
        case 10: return 2;  // XY_Packed88
        case 11: return 3;  // XYZ_Packed888
        case 12: return 4;  // XYZW_Packed8888
        default: return 0;
    }
}

struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
};

Vec3 read_vec3(const std::uint8_t* p) {
    float v[3];
    std::memcpy(v, p, sizeof(v));
    return Vec3{v[0], v[1], v[2]};
}

Float2 read_vec2(const std::uint8_t* p) {
    float v[2];
    std::memcpy(v, p, sizeof(v));
    return Float2{v[0], v[1]};
}

struct MeshRecord {
    std::uint32_t vertex_count = 0;
    std::uint32_t base_declaration = 0;
    std::vector<std::uint32_t> vertex_buffer_ids;
    std::uint32_t index_count = 0;
    std::uint32_t index_buffer_id = 0;
    std::vector<MapgeoSubmesh> submeshes;
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

}  // namespace

MapgeoAsset load_mapgeo(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "OEGM", 4) != 0) {
        throw std::runtime_error("mapgeo: bad magic (expected 'OEGM')");
    }
    reader.skip(4);

    const std::uint32_t version = reader.u32();
    if (version < 13 || version > 18) {
        throw std::runtime_error("mapgeo: unsupported version " + std::to_string(version));
    }
    const bool has_visibility_controller = version >= 15;
    const bool has_texture_overrides = version >= 17;
    const bool has_u16_render_flags = version >= 16;

    // Shader texture overrides (v17+): global sampler -> texture path.
    const std::uint32_t override_count = reader.u32();
    for (std::uint32_t i = 0; i < override_count; ++i) {
        reader.skip(4);       // sampler index
        (void)reader.string();  // texture path
    }

    // Vertex declarations.
    const std::uint32_t declaration_count = reader.u32();
    std::vector<Declaration> declarations;
    declarations.reserve(declaration_count);
    for (std::uint32_t i = 0; i < declaration_count; ++i) {
        reader.skip(4);  // usage
        const std::uint32_t element_count = reader.u32();
        if (element_count > 15) {
            throw std::runtime_error("mapgeo: implausible vertex element count");
        }
        Declaration declaration;
        for (std::uint32_t e = 0; e < element_count; ++e) {
            Element element;
            element.name = reader.u32();
            element.format = reader.u32();
            element.size = element_size(element.format);
            if (element.size == 0) {
                throw std::runtime_error("mapgeo: unknown vertex element format " +
                                         std::to_string(element.format));
            }
            declaration.stride += element.size;
            declaration.elements.push_back(element);
        }
        reader.skip(static_cast<std::size_t>(15 - element_count) * 8);
        declarations.push_back(std::move(declaration));
    }

    // Vertex buffers: record where each blob lives, read lazily.
    const std::uint32_t vertex_buffer_count = reader.u32();
    struct BufferRef {
        std::size_t offset = 0;
        std::size_t size = 0;
    };
    std::vector<BufferRef> vertex_buffers(vertex_buffer_count);
    for (auto& buffer : vertex_buffers) {
        reader.skip(1);  // visibility flags (v13+)
        buffer.size = reader.u32();
        buffer.offset = reader.position();
        reader.skip(buffer.size);
    }

    // Index buffers.
    const std::uint32_t index_buffer_count = reader.u32();
    std::vector<std::pair<std::size_t, std::size_t>> index_buffers(index_buffer_count);
    for (auto& buffer : index_buffers) {
        reader.skip(1);  // visibility flags (v13+)
        buffer.second = static_cast<std::size_t>(reader.i32());
        buffer.first = reader.position();
        reader.skip(buffer.second);
    }

    // Meshes. Version 18 slightly postdates the reference parser, so a record
    // that runs past the end is treated as the end of the mesh table rather
    // than a hard failure: everything parsed up to that point stays usable.
    const std::uint32_t mesh_count = reader.u32();
    std::vector<MeshRecord> records(mesh_count);
    for (auto& record : records) {
        try {
        record.vertex_count = static_cast<std::uint32_t>(reader.i32());
        const std::uint32_t declaration_streams = reader.u32();
        record.base_declaration = static_cast<std::uint32_t>(reader.i32());
        record.vertex_buffer_ids.resize(declaration_streams);
        for (auto& id : record.vertex_buffer_ids) {
            id = static_cast<std::uint32_t>(reader.i32());
        }
        record.index_count = reader.u32();
        record.index_buffer_id = static_cast<std::uint32_t>(reader.i32());

        reader.skip(1);  // visibility (v13+)
        if (has_visibility_controller) reader.skip(4);
        if (version >= 18) reader.skip(4);  // extra field added in v18

        const std::uint32_t submesh_count = reader.u32();
        record.submeshes.reserve(submesh_count);
        for (std::uint32_t s = 0; s < submesh_count; ++s) {
            MapgeoSubmesh submesh;
            reader.skip(4);  // material hash
            submesh.material = reader.string();
            submesh.start_index = static_cast<std::uint32_t>(reader.i32());
            submesh.index_count = static_cast<std::uint32_t>(reader.i32());
            reader.skip(8);  // min/max vertex
            record.submeshes.push_back(std::move(submesh));
        }

        reader.skip(1);                 // disable backface culling
        reader.skip(6 * 4);             // bounding box
        for (float& value : record.transform) value = reader.f32();
        reader.skip(1);                 // quality
        reader.skip(1);                 // layer transition behavior (v14+)
        reader.skip(has_u16_render_flags ? 2 : 1);  // render flags

        // Baked light and stationary light channels: string + 2x vec2.
        for (int channel = 0; channel < 2; ++channel) {
            (void)reader.string();
            reader.skip(16);
        }

        if (has_texture_overrides) {
            const std::uint32_t mesh_override_count = reader.u32();
            for (std::uint32_t i = 0; i < mesh_override_count; ++i) {
                reader.skip(4);
                (void)reader.string();
            }
            reader.skip(16);  // baked paint scale + bias
        }
        } catch (const std::exception&) {
            records.resize(static_cast<std::size_t>(&record - records.data()));
            break;
        }
    }

    // Decode the geometry we can use.
    MapgeoAsset asset;
    asset.meshes.reserve(records.size());
    for (std::size_t mesh_index = 0; mesh_index < records.size(); ++mesh_index) {
        const MeshRecord& record = records[mesh_index];
        if (record.vertex_buffer_ids.empty() || record.index_buffer_id >= index_buffers.size()) {
            continue;
        }
        const std::uint32_t buffer_id = record.vertex_buffer_ids.front();
        if (buffer_id >= vertex_buffers.size()) continue;
        if (record.base_declaration >= declarations.size()) continue;

        const Declaration& declaration = declarations[record.base_declaration];
        const BufferRef& buffer = vertex_buffers[buffer_id];
        if (declaration.stride == 0) continue;

        MapgeoMesh mesh;
        mesh.submeshes = record.submeshes;
        // Row-major in the file, column-major for the renderer.
        mesh.transform = Mat4::identity();
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                mesh.transform.m[c * 4 + r] = record.transform[r * 4 + c];
            }
        }

        mesh.vertices.resize(record.vertex_count);
        for (std::uint32_t v = 0; v < record.vertex_count; ++v) {
            const std::uint8_t* base = bytes.data() + buffer.offset +
                                       static_cast<std::size_t>(v) * declaration.stride;
            MapgeoVertex vertex;
            std::size_t offset = 0;
            for (const Element& element : declaration.elements) {
                if (element.name == 0 && element.format == 2) {
                    const Vec3 position = read_vec3(base + offset);
                    vertex.px = position.x;
                    vertex.py = position.y;
                    vertex.pz = position.z;
                } else if (element.name == 2 && element.format == 2) {
                    const Vec3 normal = read_vec3(base + offset);
                    vertex.nx = normal.x;
                    vertex.ny = normal.y;
                    vertex.nz = normal.z;
                } else if ((element.name == 7 || element.name == 8) && element.format == 1) {
                    const Float2 uv = read_vec2(base + offset);
                    vertex.u = uv.x;
                    vertex.v = uv.y;
                }
                offset += element.size;
            }
            mesh.vertices[v] = vertex;
        }

        const auto& index_buffer = index_buffers[record.index_buffer_id];
        const std::size_t index_total =
            std::min<std::size_t>(record.index_count, index_buffer.second / 2);
        mesh.indices.resize(index_total);
        std::memcpy(mesh.indices.data(), bytes.data() + index_buffer.first,
                    index_total * sizeof(std::uint16_t));

        asset.total_vertices += mesh.vertices.size();
        asset.total_triangles += mesh.indices.size() / 3;
        asset.meshes.push_back(std::move(mesh));
    }
    return asset;
}

}  // namespace lolview
