#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lolview {

struct SknMaterial {
    std::string name;
    std::uint32_t start_vertex = 0;
    std::uint32_t num_vertices = 0;
    std::uint32_t start_index = 0;
    std::uint32_t num_indices = 0;
};

// Bind-pose vertex. Positions are already in bind pose, so a static render
// needs no skinning; the skeleton is only required for animation.
struct SknVertex {
    std::array<float, 3> position{};
    std::array<std::uint8_t, 4> bone_indices{};
    std::array<float, 4> weights{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
};

struct SknBounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

// `.skn` (skinned mesh) reader. Version 4 inserts a metadata block (vertex
// stride + bounding box) between the material list and the index buffer;
// versions 1-3 go straight to the counts. The parser tries the layout the
// version implies and then the other one, and rejects a file whose counts do
// not account for its size (the strongest mistake detector).
class Skn {
public:
    static constexpr std::uint32_t kMagic = 0x00112233;
    static constexpr std::uint32_t kBaseVertexStride = 52;

    static Skn parse(std::span<const std::uint8_t> bytes);

    // Procedural cube used for sandbox props (dummy targets). Not game data.
    static Skn make_cube(float half_extent);

    // Horizontal quad on y = 0, used for the sandbox ground.
    static Skn make_plane(float half_extent);

    // Build a mesh from raw parts (map geometry chunks, procedural props).
    static Skn from_parts(std::vector<SknMaterial> materials,
                          std::vector<SknVertex> vertices,
                          std::vector<std::uint16_t> indices);

    std::uint16_t major_version() const noexcept { return major_; }
    std::uint16_t minor_version() const noexcept { return minor_; }
    const std::vector<SknMaterial>& materials() const noexcept { return materials_; }
    const std::vector<SknVertex>& vertices() const noexcept { return vertices_; }
    const std::vector<std::uint16_t>& indices() const noexcept { return indices_; }
    std::uint32_t vertex_stride() const noexcept { return vertex_stride_; }
    const std::array<float, 10>& bbox() const noexcept { return bbox_; }

    std::size_t triangle_count() const noexcept;
    SknBounds compute_bounds() const noexcept;

private:
    static Skn parse_layout(std::span<const std::uint8_t> bytes, int layout);

    std::uint16_t major_ = 0;
    std::uint16_t minor_ = 0;
    std::vector<SknMaterial> materials_;
    std::vector<std::uint16_t> indices_;
    std::vector<SknVertex> vertices_;
    std::uint32_t vertex_stride_ = 0;
    std::array<float, 10> bbox_{};
};

}  // namespace lolview
