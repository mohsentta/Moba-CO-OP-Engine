#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lolview {

// Walkable area authored in the in-game path editor (F3) and saved to a file.
// Points are in the same space as movement intents: spawn-relative.
//
// Two kinds of polygon:
//   path     walkable region (positive)
//   obstacle blocked region (negative, drawn red) - always wins
struct WalkArea {
    using Polygon = std::vector<std::pair<float, float>>;

    std::vector<Polygon> polygons;   // walkable
    std::vector<Polygon> obstacles;  // blocked

    bool empty() const { return polygons.empty(); }

    // True when the point can be walked. No walkable polygons means "no
    // restriction" unless the point sits inside an obstacle.
    bool contains(float x, float z) const;

    bool load(const std::string& path);
    void parse(const std::string& text);
};

// Uniform grid over the walkable area, with A* on top. The host builds one from
// the area and uses it to route units around obstacles.
class NavGrid {
public:
    void build(const WalkArea& area, float cell_size = 100.0f);

    bool valid() const { return width_ > 0 && height_ > 0; }
    bool cell_walkable(int cx, int cz) const;
    int width() const { return width_; }
    int height() const { return height_; }
    float cell_size() const { return cell_; }

    // True when the world point is walkable in the grid.
    bool point_walkable(float x, float z) const;

    // A* from one world point to another. Returns false when either end is not
    // walkable or no route exists. The result excludes the start point.
    bool find_path(float start_x, float start_z, float goal_x, float goal_z,
                   std::vector<std::pair<float, float>>& out) const;

    // Nearest walkable cell centre to a world point (used to pull units that end
    // up outside the area back inside). False when no cell is walkable.
    bool nearest_walkable(float x, float z, float& out_x, float& out_z) const;

    // True when a straight segment stays inside the walkable area. Used to
    // smooth grid paths into direct lines.
    bool line_clear(float x0, float z0, float x1, float z1) const;

private:
    std::pair<int, int> cell_of(float x, float z) const;
    std::pair<float, float> centre_of(int cx, int cz) const;
    std::size_t index_of(int cx, int cz) const { return static_cast<std::size_t>(cz) * width_ + cx; }

    float cell_ = 100.0f;
    float min_x_ = 0.0f;
    float min_z_ = 0.0f;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> walkable_;
};

}  // namespace lolview
