#pragma once

#include <filesystem>
#include <vector>

namespace lolview {

// A read-only view of a League of Legends installation that the *user* pointed
// at. This class is the project's legal guardrail, expressed in the type system:
//
//   * there is no default constructor, so a program cannot start without an
//     explicit path;
//   * there is no bundled-asset fallback and never will be;
//   * the installation is only ever read, never written or patched.
//
// If this program ever needs assets, it must ask for them through this class.
class AssetSource {
public:
    // `path` may be the install root (the folder containing `Game`) or the
    // `Game` folder itself. Throws std::runtime_error when the path does not
    // look like a real installation.
    explicit AssetSource(std::filesystem::path path);

    const std::filesystem::path& game_dir() const noexcept { return game_dir_; }

    // Every `.wad.client` archive under the install's DATA tree, sorted.
    std::vector<std::filesystem::path> archives() const;

    // Always false. Present so callers (and auditors) can assert the invariant.
    static constexpr bool ships_assets() noexcept { return false; }

private:
    std::filesystem::path game_dir_;
};

// Resolves an install root or Game folder to the Game folder. Returns an empty
// path when `hint` is not an installation.
std::filesystem::path find_game_dir(const std::filesystem::path& hint);

}  // namespace lolview
