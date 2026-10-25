#include <lolview/skn.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

std::string num(std::size_t value) { return std::to_string(value); }

// Little-endian cursor over the file, with bounds checks on every read.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint32_t u32() {
        require(4, "u32");
        std::uint32_t value = 0;
        std::memcpy(&value, data_.data() + offset_, sizeof(value));
        offset_ += 4;
        return value;
    }

    std::uint16_t u16() {
        require(2, "u16");
        std::uint16_t value = 0;
        std::memcpy(&value, data_.data() + offset_, sizeof(value));
        offset_ += 2;
        return value;
    }

    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::span<const std::uint8_t> bytes(std::size_t n) {
        require(n, "bytes");
        const auto view = data_.subspan(offset_, n);
        offset_ += n;
        return view;
    }

    std::string cstr(std::size_t n) {
        const auto raw = bytes(n);
        std::size_t end = 0;
        while (end < raw.size() && raw[end] != 0) ++end;
        return std::string(reinterpret_cast<const char*>(raw.data()), end);
    }

    std::size_t offset() const noexcept { return offset_; }
    std::size_t size() const noexcept { return data_.size(); }
    void seek(std::size_t value) { offset_ = value; }

private:
    void require(std::size_t n, const char* what) const {
        if (offset_ + n > data_.size()) {
            throw std::runtime_error("skn: unexpected end of file at offset " +
                                     num(offset_) + " reading " + what);
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
};

}  // namespace

Skn Skn::parse_layout(std::span<const std::uint8_t> bytes, int layout) {
    Cursor cursor(bytes);

    const std::uint32_t magic = cursor.u32();
    if (magic != kMagic) {
        throw std::runtime_error("skn: bad magic " + num(magic));
    }

    const std::uint16_t version_major = cursor.u16();
    const std::uint16_t version_minor = cursor.u16();
    const std::uint32_t material_count = cursor.u32();
    if (material_count > 4096) {
        throw std::runtime_error("skn: implausible material count " + num(material_count));
    }

    std::vector<SknMaterial> materials;
    materials.reserve(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i) {
        SknMaterial material;
        material.name = cursor.cstr(64);
        material.start_vertex = cursor.u32();
        material.num_vertices = cursor.u32();
        material.start_index = cursor.u32();
        material.num_indices = cursor.u32();
        materials.push_back(std::move(material));
    }

    std::size_t index_count = 0;
    std::size_t vertex_count = 0;
    std::uint32_t vertex_stride = kBaseVertexStride;
    std::array<float, 10> bbox{};

    if (layout == 4) {
        (void)cursor.u32();  // unknown, constant in observed files
        index_count = cursor.u32();
        vertex_count = cursor.u32();
        vertex_stride = cursor.u32();
        (void)cursor.u32();  // unknown, constant in observed files
        for (float& value : bbox) value = cursor.f32();
    } else {
        index_count = cursor.u32();
        vertex_count = cursor.u32();
    }

    if (vertex_stride < kBaseVertexStride || vertex_stride > 256) {
        throw std::runtime_error("skn: implausible vertex stride " + num(vertex_stride));
    }
    if (index_count > 200'000'000ULL || vertex_count > 50'000'000ULL) {
        throw std::runtime_error("skn: implausible element counts");
    }

    const std::size_t needed = index_count * sizeof(std::uint16_t) +
                               vertex_count * static_cast<std::size_t>(vertex_stride);
    if (cursor.offset() + needed > bytes.size()) {
        throw std::runtime_error("skn: counts need " + num(needed) + " bytes, only " +
                                 num(bytes.size() - cursor.offset()) + " remain");
    }

    std::vector<std::uint16_t> indices;
    indices.reserve(index_count);
    for (std::size_t i = 0; i < index_count; ++i) {
        indices.push_back(cursor.u16());
    }

    std::vector<SknVertex> vertices;
    vertices.reserve(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const std::size_t base = cursor.offset();

        SknVertex vertex;
        vertex.position = {cursor.f32(), cursor.f32(), cursor.f32()};

        const auto bones = cursor.bytes(4);
        vertex.bone_indices = {bones[0], bones[1], bones[2], bones[3]};

        vertex.weights = {cursor.f32(), cursor.f32(), cursor.f32(), cursor.f32()};
        vertex.normal = {cursor.f32(), cursor.f32(), cursor.f32()};
        vertex.uv = {cursor.f32(), cursor.f32()};

        vertices.push_back(vertex);
        cursor.seek(base + vertex_stride);
    }

    // The mesh should account for the file, modulo small alignment padding.
    const std::size_t left = bytes.size() - cursor.offset();
    if (left > 64) {
        throw std::runtime_error("skn: " + num(left) +
                                 " bytes left after the vertex buffer (wrong layout?)");
    }

    Skn result;
    result.major_ = version_major;
    result.minor_ = version_minor;
    result.materials_ = std::move(materials);
    result.indices_ = std::move(indices);
    result.vertices_ = std::move(vertices);
    result.vertex_stride_ = vertex_stride;
    result.bbox_ = bbox;
    return result;
}

Skn Skn::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12) {
        throw std::runtime_error("skn: file is too small");
    }

    const std::uint16_t major = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8));
    const int first = major >= 4 ? 4 : 1;
    const int second = (first == 4) ? 1 : 4;

    std::string first_error;
    try {
        return parse_layout(bytes, first);
    } catch (const std::exception& e) {
        first_error = e.what();
    }

    try {
        return parse_layout(bytes, second);
    } catch (const std::exception&) {
        throw std::runtime_error(first_error);
    }
}

Skn Skn::make_cube(float half_extent) {
    Skn cube;
    cube.major_ = 4;
    cube.minor_ = 0;
    cube.vertex_stride_ = kBaseVertexStride;

    SknMaterial material;
    material.name = "Box";
    material.num_vertices = 24;
    material.num_indices = 36;
    cube.materials_.push_back(material);

    const float h = half_extent;
    const auto push = [&](float x, float y, float z, float nx, float ny, float nz) {
        SknVertex vertex{};
        vertex.position = {x, y, z};
        vertex.normal = {nx, ny, nz};
        vertex.uv = {0.5f, 0.5f};
        vertex.bone_indices = {0, 0, 0, 0};
        vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        cube.vertices_.push_back(vertex);
    };

    push(h, -h, -h, 1, 0, 0);  push(h, h, -h, 1, 0, 0);
    push(h, h, h, 1, 0, 0);    push(h, -h, h, 1, 0, 0);
    push(-h, -h, h, -1, 0, 0); push(-h, h, h, -1, 0, 0);
    push(-h, h, -h, -1, 0, 0); push(-h, -h, -h, -1, 0, 0);
    push(-h, h, -h, 0, 1, 0);  push(h, h, -h, 0, 1, 0);
    push(h, h, h, 0, 1, 0);    push(-h, h, h, 0, 1, 0);
    push(-h, -h, h, 0, -1, 0); push(h, -h, h, 0, -1, 0);
    push(h, -h, -h, 0, -1, 0); push(-h, -h, -h, 0, -1, 0);
    push(-h, -h, h, 0, 0, 1);  push(h, -h, h, 0, 0, 1);
    push(h, h, h, 0, 0, 1);    push(-h, h, h, 0, 0, 1);
    push(h, -h, -h, 0, 0, -1); push(-h, -h, -h, 0, 0, -1);
    push(-h, h, -h, 0, 0, -1); push(h, h, -h, 0, 0, -1);

    for (std::uint16_t face = 0; face < 6; ++face) {
        const std::uint16_t base = static_cast<std::uint16_t>(face * 4);
        cube.indices_.insert(cube.indices_.end(),
                             {base, static_cast<std::uint16_t>(base + 1),
                              static_cast<std::uint16_t>(base + 2), base,
                              static_cast<std::uint16_t>(base + 2),
                              static_cast<std::uint16_t>(base + 3)});
    }
    return cube;
}

std::size_t Skn::triangle_count() const noexcept {
    std::size_t triangles = 0;
    bool any_material_range = false;

    for (const auto& material : materials_) {
        if (material.num_indices == 0) continue;
        any_material_range = true;
        const std::size_t start = material.start_index;
        const std::size_t end =
            std::min<std::size_t>(start + material.num_indices, indices_.size());
        if (end > start) triangles += (end - start) / 3;
    }

    if (!any_material_range) triangles = indices_.size() / 3;
    return triangles;
}

SknBounds Skn::compute_bounds() const noexcept {
    SknBounds bounds;
    bounds.min = {std::numeric_limits<float>::infinity(),
                  std::numeric_limits<float>::infinity(),
                  std::numeric_limits<float>::infinity()};
    bounds.max = {-std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity()};

    for (const auto& vertex : vertices_) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    }
    return bounds;
}

}  // namespace lolview
