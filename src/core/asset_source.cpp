#include <lolview/asset_source.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace lolview {
namespace {

bool looks_like_game_dir(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(p / "League of Legends.exe", ec)) return true;
    if (std::filesystem::is_directory(p / "DATA" / "FINAL", ec)) return true;
    return false;
}

}  // namespace

std::filesystem::path find_game_dir(const std::filesystem::path& hint) {
    std::error_code ec;
    if (!std::filesystem::is_directory(hint, ec)) return {};

    if (looks_like_game_dir(hint)) return hint;

    const std::filesystem::path nested = hint / "Game";
    if (looks_like_game_dir(nested)) return nested;

    const std::filesystem::path league_nested = hint / "League of Legends" / "Game";
    if (looks_like_game_dir(league_nested)) return league_nested;

    return {};
}

AssetSource::AssetSource(std::filesystem::path path) {
    if (path.empty()) {
        throw std::runtime_error(
            "no game directory given; this tool reads assets from a League of Legends "
            "installation that you own and distributes none of its own");
    }
    game_dir_ = find_game_dir(path);
    if (game_dir_.empty()) {
        throw std::runtime_error(
            path.string() +
            ": not a League of Legends install (expected 'League of Legends.exe' or "
            "'DATA/FINAL'). Point --game-dir at your own installation.");
    }
}

std::vector<std::filesystem::path> AssetSource::archives() const {
    std::vector<std::filesystem::path> found;
    std::error_code ec;

    const std::filesystem::path data_root = game_dir_ / "DATA";
    if (!std::filesystem::is_directory(data_root, ec)) return found;

    for (std::filesystem::recursive_directory_iterator it(
             data_root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;

        const std::string name = it->path().filename().string();
        constexpr std::string_view suffix = ".wad.client";
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found.push_back(it->path());
        }
    }

    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace lolview
