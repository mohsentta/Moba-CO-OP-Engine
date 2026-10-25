#include <lolview/walkable.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <queue>
#include <sstream>

namespace lolview {
namespace {

bool point_in_polygon(const WalkArea::Polygon& polygon, float x, float z) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const float xi = polygon[i].first;
        const float zi = polygon[i].second;
        const float xj = polygon[j].first;
        const float zj = polygon[j].second;
        const bool crosses = (zi > z) != (zj > z);
        if (crosses && zj != zi) {
            const float intersection = (xj - xi) * (z - zi) / (zj - zi) + xi;
            if (x < intersection) inside = !inside;
        }
    }
    return inside;
}

}  // namespace

bool WalkArea::contains(float x, float z) const {
    // Obstacles win over walkable regions.
    for (const auto& obstacle : obstacles) {
        if (point_in_polygon(obstacle, x, z)) return false;
    }
    if (polygons.empty()) return true;
    for (const auto& polygon : polygons) {
        if (point_in_polygon(polygon, x, z)) return true;
    }
    return false;
}

void WalkArea::parse(const std::string& text) {
    std::istringstream stream(text);
    Polygon current;
    bool into_obstacles = false;
    std::string token;
    while (stream >> token) {
        if (token == "path" || token == "obstacle") {
            if (current.size() >= 3) {
                (into_obstacles ? obstacles : polygons).push_back(current);
            }
            current.clear();
            into_obstacles = (token == "obstacle");
            std::string name;
            stream >> name;
        } else if (token == "point") {
            float x = 0.0f, z = 0.0f;
            if (!(stream >> x >> z)) break;
            current.push_back({x, z});
        } else if (token == "end") {
            if (current.size() >= 3) {
                (into_obstacles ? obstacles : polygons).push_back(current);
            }
            current.clear();
        }
    }
    if (current.size() >= 3) {
        (into_obstacles ? obstacles : polygons).push_back(current);
    }
}

bool WalkArea::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    parse(text.str());
    return true;
}

// ---- navigation grid ---------------------------------------------------

void NavGrid::build(const WalkArea& area, float cell_size) {
    cell_ = cell_size > 1.0f ? cell_size : 1.0f;
    walkable_.clear();
    width_ = height_ = 0;
    if (area.polygons.empty() && area.obstacles.empty()) return;

    float max_x = -std::numeric_limits<float>::max();
    float max_z = -std::numeric_limits<float>::max();
    min_x_ = min_z_ = std::numeric_limits<float>::max();
    const auto extend = [&](const WalkArea::Polygon& polygon) {
        for (const auto& point : polygon) {
            min_x_ = std::min(min_x_, point.first);
            min_z_ = std::min(min_z_, point.second);
            max_x = std::max(max_x, point.first);
            max_z = std::max(max_z, point.second);
        }
    };
    for (const auto& polygon : area.polygons) extend(polygon);
    for (const auto& polygon : area.obstacles) extend(polygon);

    width_ = static_cast<int>(std::ceil((max_x - min_x_) / cell_)) + 1;
    height_ = static_cast<int>(std::ceil((max_z - min_z_) / cell_)) + 1;
    walkable_.assign(static_cast<std::size_t>(width_) * height_, 0);

    for (int cz = 0; cz < height_; ++cz) {
        for (int cx = 0; cx < width_; ++cx) {
            const auto centre = centre_of(cx, cz);
            walkable_[index_of(cx, cz)] = area.contains(centre.first, centre.second) ? 1 : 0;
        }
    }
}

bool NavGrid::cell_walkable(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= width_ || cz >= height_) return false;
    return walkable_[index_of(cx, cz)] != 0;
}

std::pair<int, int> NavGrid::cell_of(float x, float z) const {
    return {static_cast<int>(std::floor((x - min_x_) / cell_)),
            static_cast<int>(std::floor((z - min_z_) / cell_))};
}

std::pair<float, float> NavGrid::centre_of(int cx, int cz) const {
    return {min_x_ + (static_cast<float>(cx) + 0.5f) * cell_,
            min_z_ + (static_cast<float>(cz) + 0.5f) * cell_};
}

bool NavGrid::point_walkable(float x, float z) const {
    const auto cell = cell_of(x, z);
    return cell_walkable(cell.first, cell.second);
}

bool NavGrid::nearest_walkable(float x, float z, float& out_x, float& out_z) const {
    if (!valid()) return false;
    const auto [start_cx, start_cz] = cell_of(x, z);
    if (cell_walkable(start_cx, start_cz)) {
        out_x = x;
        out_z = z;
        return true;
    }

    // Expanding ring search around the start cell.
    const int max_radius = std::max(width_, height_);
    for (int radius = 1; radius <= max_radius; ++radius) {
        bool found = false;
        float best = std::numeric_limits<float>::max();
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // Only the ring itself.
                if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
                const int cx = start_cx + dx;
                const int cz = start_cz + dz;
                if (!cell_walkable(cx, cz)) continue;
                const auto centre = centre_of(cx, cz);
                const float ddx = centre.first - x;
                const float ddz = centre.second - z;
                const float distance = ddx * ddx + ddz * ddz;
                if (distance < best) {
                    best = distance;
                    out_x = centre.first;
                    out_z = centre.second;
                    found = true;
                }
            }
        }
        if (found) return true;
    }
    return false;
}

bool NavGrid::line_clear(float x0, float z0, float x1, float z1) const {
    if (!valid()) return false;
    const float dx = x1 - x0;
    const float dz = z1 - z0;
    const float distance = std::sqrt(dx * dx + dz * dz);
    // Sample finer than a cell so a diagonal cannot slip through a corner.
    const float step = cell_ * 0.35f;
    const int samples = std::max(1, static_cast<int>(std::ceil(distance / step)));
    for (int i = 0; i <= samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples);
        if (!point_walkable(x0 + dx * t, z0 + dz * t)) return false;
    }
    return true;
}

bool NavGrid::find_path(float start_x, float start_z, float goal_x, float goal_z,
                        std::vector<std::pair<float, float>>& out) const {
    out.clear();
    if (!valid()) return false;

    const auto [sx, sz] = cell_of(start_x, start_z);
    const auto [gx, gz] = cell_of(goal_x, goal_z);
    if (!cell_walkable(sx, sz) || !cell_walkable(gx, gz)) return false;
    if (sx == gx && sz == gz) return true;

    const std::size_t count = static_cast<std::size_t>(width_) * height_;
    const float infinity = std::numeric_limits<float>::max();
    std::vector<float> cost(count, infinity);
    std::vector<int> parent(count, -1);
    std::vector<std::uint8_t> closed(count, 0);

    const auto heuristic = [&](int ax, int az) {
        const float dx = static_cast<float>(std::abs(ax - gx));
        const float dz = static_cast<float>(std::abs(az - gz));
        return (dx + dz) + (std::sqrt(2.0f) - 2.0f) * std::min(dx, dz);
    };

    struct Node {
        float estimate;
        int cx, cz;
        bool operator>(const Node& other) const { return estimate > other.estimate; }
    };

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    cost[index_of(sx, sz)] = 0.0f;
    open.push({heuristic(sx, sz), sx, sz});

    static const int kSteps[8][2] = {{1, 0},  {-1, 0}, {0, 1},  {0, -1},
                                     {1, 1},  {1, -1}, {-1, 1}, {-1, -1}};
    bool found = false;
    while (!open.empty()) {
        const Node current = open.top();
        open.pop();
        const std::size_t current_index = index_of(current.cx, current.cz);
        if (closed[current_index]) continue;
        closed[current_index] = 1;
        if (current.cx == gx && current.cz == gz) {
            found = true;
            break;
        }

        for (const auto& step : kSteps) {
            const int nx = current.cx + step[0];
            const int nz = current.cz + step[1];
            if (!cell_walkable(nx, nz)) continue;
            // No corner cutting: a diagonal needs both orthogonal neighbours.
            if (step[0] != 0 && step[1] != 0) {
                if (!cell_walkable(current.cx + step[0], current.cz)) continue;
                if (!cell_walkable(current.cx, current.cz + step[1])) continue;
            }
            const std::size_t next_index = index_of(nx, nz);
            if (closed[next_index]) continue;
            const float step_cost = (step[0] != 0 && step[1] != 0) ? std::sqrt(2.0f) : 1.0f;
            const float candidate = cost[current_index] + step_cost;
            if (candidate < cost[next_index]) {
                cost[next_index] = candidate;
                parent[next_index] = static_cast<int>(current_index);
                open.push({candidate + heuristic(nx, nz), nx, nz});
            }
        }
    }
    if (!found) return false;

    // Walk back to the start, then reverse.
    std::vector<std::pair<int, int>> cells;
    int cursor = static_cast<int>(index_of(gx, gz));
    while (cursor != -1) {
        cells.push_back({cursor % width_, cursor / width_});
        cursor = parent[static_cast<std::size_t>(cursor)];
    }
    std::reverse(cells.begin(), cells.end());
    for (std::size_t i = 1; i < cells.size(); ++i) {
        out.push_back(centre_of(cells[i].first, cells[i].second));
    }
    // Finish on the exact goal rather than the last cell centre.
    out.push_back({goal_x, goal_z});

    // String pulling: replace the grid staircase with the furthest visible point
    // at each step, so movement is a straight line wherever the area allows.
    std::vector<std::pair<float, float>> points;
    points.push_back({start_x, start_z});
    for (const auto& point : out) points.push_back(point);

    std::vector<std::pair<float, float>> smoothed;
    std::size_t current = 0;
    while (current + 1 < points.size()) {
        std::size_t best = current + 1;
        for (std::size_t candidate = points.size() - 1; candidate > current + 1; --candidate) {
            if (line_clear(points[current].first, points[current].second,
                           points[candidate].first, points[candidate].second)) {
                best = candidate;
                break;
            }
        }
        smoothed.push_back(points[best]);
        current = best;
    }
    out = smoothed;
    return true;
}

}  // namespace lolview
