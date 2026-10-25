#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lolview/math.hpp>

namespace lolview {

struct MapgeoVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float u = 0.0f, v = 0.0f;
};

struct MapgeoSubmesh {
    std::string material;
    std::uint32_t start_index = 0;
    std::uint32_t index_count = 0;
};

struct MapgeoMesh {
    std::string name;
    std::vector<MapgeoSubmesh> submeshes;
    std::vector<MapgeoVertex> vertices;
    std::vector<std::uint16_t> indices;
    Mat4 transform = Mat4::identity();
};

struct MapgeoAsset {
    std::vector<MapgeoMesh> meshes;
    std::size_t total_vertices = 0;
    std::size_t total_triangles = 0;
};

// Riot environment geometry (`.mapgeo`, magic "OEGM"). Layout ported from the
// `ltk_mapgeo` reference implementation; versions 13-18 are handled, which
// covers current builds.
MapgeoAsset load_mapgeo(std::span<const std::uint8_t> bytes);

}  // namespace lolview
