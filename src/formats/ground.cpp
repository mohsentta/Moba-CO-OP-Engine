#include <lolview/skn.hpp>

namespace lolview {

Skn Skn::make_plane(float half_extent) {
    Skn plane;
    plane.major_ = 4;
    plane.minor_ = 0;
    plane.vertex_stride_ = kBaseVertexStride;

    SknMaterial material;
    material.name = "Ground";
    material.num_vertices = 4;
    material.num_indices = 6;
    plane.materials_.push_back(material);

    const float h = half_extent;
    const auto push = [&](float x, float y, float z, float u, float v) {
        SknVertex vertex{};
        vertex.position = {x, y, z};
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.uv = {u, v};
        vertex.bone_indices = {0, 0, 0, 0};
        vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        plane.vertices_.push_back(vertex);
    };

    push(-h, 0.0f, -h, 0.0f, 0.0f);
    push(h, 0.0f, -h, 1.0f, 0.0f);
    push(h, 0.0f, h, 1.0f, 1.0f);
    push(-h, 0.0f, h, 0.0f, 1.0f);

    plane.indices_ = {0, 1, 2, 0, 2, 3};
    return plane;
}

Skn Skn::from_parts(std::vector<SknMaterial> materials, std::vector<SknVertex> vertices,
                    std::vector<std::uint16_t> indices) {
    Skn mesh;
    mesh.major_ = 4;
    mesh.minor_ = 0;
    mesh.vertex_stride_ = kBaseVertexStride;
    mesh.materials_ = std::move(materials);
    mesh.vertices_ = std::move(vertices);
    mesh.indices_ = std::move(indices);
    return mesh;
}

}  // namespace lolview
