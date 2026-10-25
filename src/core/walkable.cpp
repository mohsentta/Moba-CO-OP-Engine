#include <lolview/walkable.hpp>

#include <fstream>
#include <sstream>

namespace lolview {
namespace {

bool point_in_polygon(const std::vector<std::pair<float, float>>& polygon, float x, float z) {
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
    if (polygons.empty()) return true;
    for (const auto& polygon : polygons) {
        if (point_in_polygon(polygon, x, z)) return true;
    }
    return false;
}

void WalkArea::parse(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::pair<float, float>> current;
    std::string token;
    while (stream >> token) {
        if (token == "path") {
            if (current.size() >= 3) polygons.push_back(current);
            current.clear();
            std::string name;
            stream >> name;
        } else if (token == "point") {
            float x = 0.0f, z = 0.0f;
            if (!(stream >> x >> z)) break;
            current.push_back({x, z});
        } else if (token == "end") {
            if (current.size() >= 3) polygons.push_back(current);
            current.clear();
        }
    }
    if (current.size() >= 3) polygons.push_back(current);
}

bool WalkArea::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    parse(text.str());
    return true;
}

}  // namespace lolview
