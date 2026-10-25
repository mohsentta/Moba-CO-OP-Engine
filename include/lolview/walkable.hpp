#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lolview {

// Walkable area authored in the in-game path editor (F3) and saved to a file.
// Points are in the same space as movement intents: spawn-relative.
struct WalkArea {
    std::vector<std::vector<std::pair<float, float>>> polygons;

    bool empty() const { return polygons.empty(); }

    // True when the point is walkable. An empty area means "no restriction".
    bool contains(float x, float z) const;

    // Loads "path <name> / point <x> <z> / end" blocks. Returns false when the
    // file cannot be opened; a malformed block is skipped rather than fatal.
    bool load(const std::string& path);

    // Parses the same format from memory (used by tests).
    void parse(const std::string& text);
};

}  // namespace lolview
