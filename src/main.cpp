#include <lolview/anm.hpp>
#include <lolview/asset_source.hpp>
#include <lolview/bin.hpp>
#include <lolview/pose.hpp>
#include <lolview/skl.hpp>
#include <lolview/skn.hpp>
#include <lolview/tex.hpp>
#include <lolview/vfx.hpp>
#include <lolview/vfx_def.hpp>
#include <lolview/wad.hpp>
#include <lolview/xxhash64.hpp>

#ifdef LOLVIEW_HAS_VIEWER
#include <lolview/renderer.hpp>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kBanner =
    "lolview " LOLVIEW_VERSION " - standalone model/effect viewer\n"
    "Not affiliated with Riot Games. Requires a legitimately installed copy of\n"
    "League of Legends. This program distributes no game assets: everything it\n"
    "reads comes from the installation you point it at, read-only.\n\n";

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "usage: lolview --game-dir <path> [command]\n"
        "\n"
        "  --game-dir <path>   your League of Legends install root or its Game folder\n"
        "                     (required; there is no bundled-asset fallback)\n"
        "  --verify            check the install, list discovered archives\n"
        "  --list              list chunk table entries from each archive\n"
        "  --archive <substr>  restrict --list to archives whose path contains this\n"
        "  --limit <n>         max entries printed per archive (default 20)\n"
        "  --hash <path>       print the chunk key for a virtual path, then exit\n"
        "  --lookup <path>     find that virtual path's chunk (requires --archive)\n"
        "  --extract <path>    decode that chunk and write it to --out (requires --archive)\n"
        "  --mesh <path>       parse a .skn and print its structure (requires --archive)\n"
        "  --render            draw the parsed mesh to a PNG (use with --mesh)\n"
        "  --texture <path>    apply a .tex colour map when rendering\n"
        "  --tex <path>        decode a .tex and save it to --out as PNG\n"
        "  --find <file>       test virtual paths (one per line) against --archive\n"
        "  --scan              decode every chunk and histogram its magic bytes\n"
        "  --scan-find <text>  report chunks whose decoded bytes contain <text>\n"
        "  --scan-prefix <s>   list chunks starting with ASCII prefix <s> (header dump)\n"
        "  --extract-hash <h>  decode the chunk with this 64-bit hash to --out\n"
        "  --vfx-info <h>      parse the VFX bin with this hash and list systems\n"
        "  --skeleton <path>   .skl to pose the mesh with\n"
        "  --anim <path>       .anm to play (needs --skeleton)\n"
        "  --time <seconds>    animation time to sample (default 0)\n"
        "  --effect <q|w|e|r>  spawn the sandbox spell effect at --time\n"
        "  --size <WxH>        render resolution (default 1024x1024)\n"
        "  --yaw <deg>         camera yaw around the model (default 35)\n"
        "  --pitch <deg>       camera pitch (default 12)\n"
        "  --out <file>        destination for --extract (default: the path's filename)\n"
        "  --help              show this text\n");
}

struct Options {
    fs::path game_dir;
    std::string archive_filter;
    std::string hash_path;
    std::string lookup_path;
    std::string extract_path;
    std::string mesh_path;
    std::string texture_path;
    std::string tex_path;
    std::string find_file;
    std::string scan_find;
    std::string scan_prefix;
    std::string extract_hash;
    std::string vfx_hash;
    bool scan = false;
    std::string skeleton_path;
    std::string anim_path;
    std::string effect_name;
    float time = 0.0f;
    fs::path out_file;
    bool render = false;
    int width = 1024;
    int height = 1024;
    float yaw = 35.0f;
    float pitch = 12.0f;
    std::size_t limit = 20;
    bool verify = false;
    bool list = false;
    bool help = false;
};

bool next_value(int argc, char** argv, int& i, std::string& out) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        std::string value;
        if (arg == "--help" || arg == "-h") {
            o.help = true;
        } else if (arg == "--verify") {
            o.verify = true;
        } else if (arg == "--list") {
            o.list = true;
        } else if (arg == "--game-dir") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.game_dir = value;
        } else if (arg == "--archive") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.archive_filter = value;
        } else if (arg == "--hash") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.hash_path = value;
        } else if (arg == "--lookup") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.lookup_path = value;
        } else if (arg == "--extract") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.extract_path = value;
        } else if (arg == "--mesh") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.mesh_path = value;
        } else if (arg == "--render") {
            o.render = true;
        } else if (arg == "--texture") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.texture_path = value;
        } else if (arg == "--tex") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.tex_path = value;
        } else if (arg == "--find") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.find_file = value;
        } else if (arg == "--scan") {
            o.scan = true;
        } else if (arg == "--scan-find") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.scan_find = value;
        } else if (arg == "--scan-prefix") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.scan_prefix = value;
        } else if (arg == "--extract-hash") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.extract_hash = value;
        } else if (arg == "--vfx-info") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.vfx_hash = value;
        } else if (arg == "--skeleton") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.skeleton_path = value;
        } else if (arg == "--anim") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.anim_path = value;
        } else if (arg == "--time") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.time = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--effect") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.effect_name = value;
        } else if (arg == "--size") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            int w = 0;
            int h = 0;
            if (std::sscanf(value.c_str(), "%dx%d", &w, &h) != 2 || w < 16 || h < 16) {
                std::fprintf(stderr, "error: --size expects WxH, got '%s'\n", value.c_str());
                return std::nullopt;
            }
            o.width = w;
            o.height = h;
        } else if (arg == "--yaw") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.yaw = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--pitch") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.pitch = static_cast<float>(std::atof(value.c_str()));
        } else if (arg == "--out") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            o.out_file = value;
        } else if (arg == "--limit") {
            if (!next_value(argc, argv, i, value)) return std::nullopt;
            try {
                o.limit = static_cast<std::size_t>(std::stoull(value));
            } catch (const std::exception&) {
                std::fprintf(stderr, "error: --limit expects a number, got '%s'\n", value.c_str());
                return std::nullopt;
            }
        } else {
            std::fprintf(stderr, "error: unknown argument '%.*s'\n",
                         static_cast<int>(arg.size()), arg.data());
            return std::nullopt;
        }
    }
    return o;
}

std::string human_bytes(std::uint64_t bytes) {
    char buf[64];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::snprintf(buf, sizeof(buf), "%.1f MiB", mb);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::fputs(kBanner, stdout);

    const auto options = parse_args(argc, argv);
    if (!options) {
        print_usage(stderr);
        return 2;
    }
    if (options->help) {
        print_usage(stdout);
        return 0;
    }

    // Diagnostic that needs no installation at all.
    if (!options->hash_path.empty()) {
        const std::uint64_t h = lolview::path_hash(options->hash_path);
        std::printf("path:  %s\nhash:  %016" PRIx64 "  (xxh64 seed 0 of lowercase path)\n",
                    lolview::normalize_virtual_path(options->hash_path).c_str(), h);
        return 0;
    }

    if (options->game_dir.empty()) {
        std::fprintf(stderr, "error: --game-dir is required\n\n");
        print_usage(stderr);
        return 2;
    }

    try {
        const lolview::AssetSource source(options->game_dir);
        std::printf("install: %s\n", source.game_dir().string().c_str());

        const auto archives = source.archives();
        std::printf("archives: %zu\n", archives.size());
        if (archives.empty()) {
            std::fprintf(stderr,
                         "warning: no *.wad.client archives found under %s\\DATA\n",
                         source.game_dir().string().c_str());
            return 1;
        }

        // Resolve a virtual path to decoded bytes in the first matching archive.
        const auto resolve_chunk =
            [&](const std::string& virtual_path) -> std::optional<std::vector<std::uint8_t>> {
            const std::uint64_t key = lolview::path_hash(virtual_path);
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(key);
                if (!idx) continue;
                return wad.read_chunk(wad.entries()[*idx]);
            }
            return std::nullopt;
        };

        if (!options->lookup_path.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --lookup needs --archive to say where to look\n");
                return 2;
            }
            const std::uint64_t key = lolview::path_hash(options->lookup_path);
            std::printf("lookup: %s\nkey:    %016" PRIx64 "\n",
                        lolview::normalize_virtual_path(options->lookup_path).c_str(), key);

            bool found = false;
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(key);
                if (!idx) continue;
                const auto& e = wad.entries()[*idx];
                std::printf("HIT  %s  %6u -> %6u B  %-10s @0x%08x\n",
                            wad.path().filename().string().c_str(), e.compressed_size,
                            e.uncompressed_size,
                            std::string(lolview::compression_name(e.compression)).c_str(),
                            e.offset);
                found = true;
            }
            if (!found) {
                std::printf("no chunk for that path in the selected archive(s)\n");
            }
            return found ? 0 : 1;
        }

        if (!options->extract_path.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --extract needs --archive to say where to look\n");
                return 2;
            }
            const std::uint64_t key = lolview::path_hash(options->extract_path);
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(key);
                if (!idx) continue;

                const auto& e = wad.entries()[*idx];
                const auto bytes = wad.read_chunk(e);

                fs::path destination = options->out_file;
                if (destination.empty()) {
                    destination = fs::path(options->extract_path).filename();
                }
                std::ofstream file(destination, std::ios::binary | std::ios::trunc);
                if (!file) {
                    std::fprintf(stderr, "error: cannot write '%s'\n",
                                 destination.string().c_str());
                    return 1;
                }
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                file.close();
                if (!file) {
                    std::fprintf(stderr, "error: write failed for '%s'\n",
                                 destination.string().c_str());
                    return 1;
                }

                std::printf("source:    %s\nentry:     %6u -> %6u B  %-10s\nwrote:     %zu bytes -> %s\n",
                            wad.path().filename().string().c_str(), e.compressed_size,
                            e.uncompressed_size,
                            std::string(lolview::compression_name(e.compression)).c_str(),
                            bytes.size(), destination.string().c_str());
                return 0;
            }
            std::printf("no chunk for that path in the selected archive(s)\n");
            return 1;
        }

        if (!options->find_file.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --find needs --archive to say where to look\n");
                return 2;
            }
            std::ifstream list(options->find_file);
            if (!list) {
                std::fprintf(stderr, "error: cannot read '%s'\n", options->find_file.c_str());
                return 1;
            }

            std::vector<std::string> candidates;
            std::string line;
            while (std::getline(list, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) candidates.push_back(line);
            }

            std::size_t checked = 0;
            std::size_t hits = 0;
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                for (const auto& candidate : candidates) {
                    ++checked;
                    const std::uint64_t key = lolview::path_hash(candidate);
                    const auto idx = wad.find(key);
                    if (!idx) continue;
                    const auto& e = wad.entries()[*idx];
                    std::printf("HIT  %s  %6u -> %6u B  %s\n", candidate.c_str(),
                                e.compressed_size, e.uncompressed_size,
                                std::string(lolview::compression_name(e.compression)).c_str());
                    ++hits;
                }
            }
            std::printf("\n%zu candidates, %zu hits\n", checked, hits);
            return hits > 0 ? 0 : 1;
        }

        if (options->scan || !options->scan_find.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --scan needs --archive to say where to look\n");
                return 2;
            }

            std::map<std::string, std::pair<std::size_t, std::uint64_t>> histogram;
            std::size_t decoded = 0;
            std::size_t hits = 0;
            std::size_t failed = 0;

            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                for (const auto& entry : wad.entries()) {
                    std::vector<std::uint8_t> bytes;
                    try {
                        bytes = wad.read_chunk(entry);
                    } catch (const std::exception&) {
                        ++failed;
                        continue;
                    }
                    ++decoded;

                    if (!options->scan_find.empty()) {
                        const std::string& needle = options->scan_find;
                        if (bytes.size() >= needle.size()) {
                            for (std::size_t i = 0; i + needle.size() <= bytes.size(); ++i) {
                                if (std::memcmp(bytes.data() + i, needle.data(), needle.size()) ==
                                    0) {
                                    std::printf("HIT %016" PRIx64 "  %zu bytes  at +0x%zx\n",
                                                entry.hash, bytes.size(), i);
                                    ++hits;
                                    break;
                                }
                            }
                        }
                        continue;
                    }

                    std::string key;
                    if (bytes.size() >= 4) {
                        const bool printable =
                            bytes[0] >= 32 && bytes[0] <= 126 && bytes[1] >= 32 && bytes[1] <= 126 &&
                            bytes[2] >= 32 && bytes[2] <= 126 && bytes[3] >= 32 && bytes[3] <= 126;
                        if (printable) {
                            key.assign(reinterpret_cast<const char*>(bytes.data()), 4);
                        } else {
                            const std::uint32_t word = static_cast<std::uint32_t>(bytes[0]) |
                                                       (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                                       (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                                       (static_cast<std::uint32_t>(bytes[3]) << 24);
                            char buffer[16];
                            std::snprintf(buffer, sizeof(buffer), "%08" PRIx32, word);
                            key = buffer;
                        }
                    } else {
                        key = "<short>";
                    }
                    auto& stats = histogram[key];
                    stats.first += 1;
                    stats.second += bytes.size();
                }
            }

            if (!options->scan_find.empty()) {
                std::printf("\n%zu decoded, %zu hits, %zu undecodable\n", decoded, hits, failed);
                return hits > 0 ? 0 : 1;
            }

            std::vector<std::pair<std::string, std::pair<std::size_t, std::uint64_t>>> sorted(
                histogram.begin(), histogram.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second.first > b.second.first; });

            std::printf("%-10s %8s %14s\n", "magic", "chunks", "total bytes");
            for (const auto& [magic, stats] : sorted) {
                std::printf("%-10s %8zu %14" PRIu64 "\n", magic.c_str(), stats.first,
                            stats.second);
            }
            std::printf("\n%zu decoded, %zu undecodable, %zu distinct magics\n", decoded, failed,
                        histogram.size());
            return 0;
        }

        if (!options->scan_prefix.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --scan-prefix needs --archive\n");
                return 2;
            }
            const std::string& prefix = options->scan_prefix;
            std::size_t found = 0;
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                for (const auto& entry : wad.entries()) {
                    std::vector<std::uint8_t> bytes;
                    try {
                        bytes = wad.read_chunk(entry);
                    } catch (const std::exception&) {
                        continue;
                    }
                    if (bytes.size() < prefix.size() ||
                        std::memcmp(bytes.data(), prefix.data(), prefix.size()) != 0) {
                        continue;
                    }
                    char ascii[32] = {};
                    for (std::size_t i = 0; i < 15 && i < bytes.size(); ++i) {
                        ascii[i] = (bytes[i] >= 32 && bytes[i] <= 126)
                                       ? static_cast<char>(bytes[i])
                                       : '.';
                    }
                    std::printf("%016" PRIx64 "  %8zu  ", entry.hash, bytes.size());
                    for (std::size_t i = 0; i < 8 && i < bytes.size(); ++i) {
                        std::printf("%02x", bytes[i]);
                    }
                    std::printf("  %s\n", ascii);
                    ++found;
                }
            }
            std::printf("\n%zu chunks with prefix '%s'\n", found, prefix.c_str());
            return 0;
        }

        if (!options->vfx_hash.empty()) {
            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --vfx-info needs --archive\n");
                return 2;
            }
            std::uint64_t wanted = 0;
            try {
                wanted = std::stoull(options->vfx_hash, nullptr, 16);
            } catch (const std::exception&) {
                std::fprintf(stderr, "error: bad hash '%s'\n", options->vfx_hash.c_str());
                return 2;
            }
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(wanted);
                if (!idx) continue;
                const auto bytes = wad.read_chunk(wad.entries()[*idx]);
                const auto document = lolview::BinDocument::parse(bytes);
                const auto systems = lolview::extract_vfx_systems(document);
                std::printf("bin %016" PRIx64 ": %zu entries, %zu VFX systems\n", wanted,
                            document.entries().size(), systems.size());
                for (const auto& system : systems) {
                    std::printf("\n  %s  (%zu emitters)\n", system.name.c_str(),
                                system.emitters.size());
                    for (const auto& emitter : system.emitters) {
                        std::printf(
                            "    %-24s rate %6.2f  life %5.2f  blend %d  size %6.1f  %s\n",
                            emitter.name.c_str(), emitter.rate, emitter.lifetime,
                            emitter.blend_mode,
                            std::max(emitter.birth_scale.x, emitter.birth_scale.y),
                            emitter.texture.empty() ? "(no texture)" : emitter.texture.c_str());
                    }
                }
                return 0;
            }
            std::printf("no chunk with hash %016" PRIx64 "\n", wanted);
            return 1;
        }

        if (!options->extract_hash.empty()) {
            if (options->archive_filter.empty() || options->out_file.empty()) {
                std::fprintf(stderr, "error: --extract-hash needs --archive and --out\n");
                return 2;
            }
            std::uint64_t wanted = 0;
            try {
                wanted = std::stoull(options->extract_hash, nullptr, 16);
            } catch (const std::exception&) {
                std::fprintf(stderr, "error: bad hash '%s'\n", options->extract_hash.c_str());
                return 2;
            }
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(wanted);
                if (!idx) continue;
                const auto& entry = wad.entries()[*idx];
                const auto bytes = wad.read_chunk(entry);
                std::ofstream file(options->out_file, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                file.close();
                std::printf("wrote %zu bytes to %s\n", bytes.size(),
                            options->out_file.string().c_str());
                return 0;
            }
            std::printf("no chunk with hash %016" PRIx64 "\n", wanted);
            return 1;
        }

        if (!options->tex_path.empty()) {
            if (options->archive_filter.empty() || options->out_file.empty()) {
                std::fprintf(stderr, "error: --tex needs --archive and --out\n");
                return 2;
            }
#ifdef LOLVIEW_HAS_VIEWER
            const std::uint64_t key = lolview::path_hash(options->tex_path);
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(key);
                if (!idx) continue;
                const auto image = lolview::decode_tex(wad.read_chunk(wad.entries()[*idx]));
                lolview::save_png(options->out_file, image.width, image.height,
                                  image.rgba.data());
                std::printf("tex:        %s (%dx%d %s)\n", options->tex_path.c_str(),
                            image.width, image.height, image.format.c_str());
                std::printf("wrote:      %s\n", options->out_file.string().c_str());
                return 0;
            }
            std::printf("no chunk for that path in the selected archive(s)\n");
            return 1;
#else
            std::fprintf(stderr, "error: --tex needs a build with the viewer enabled\n");
            return 2;
#endif
        }

        if (!options->mesh_path.empty()) {            if (options->archive_filter.empty()) {
                std::fprintf(stderr, "error: --mesh needs --archive to say where to look\n");
                return 2;
            }
            const std::uint64_t key = lolview::path_hash(options->mesh_path);
            for (const auto& archive_path : archives) {
                if (archive_path.string().find(options->archive_filter) == std::string::npos) {
                    continue;
                }
                const auto wad = lolview::Wad::open(archive_path);
                const auto idx = wad.find(key);
                if (!idx) continue;

                const auto& e = wad.entries()[*idx];
                const auto bytes = wad.read_chunk(e);
                const auto mesh = lolview::Skn::parse(bytes);
                const auto bounds = mesh.compute_bounds();

                lolview::Skl skeleton;
                bool has_skeleton = false;
                if (!options->skeleton_path.empty()) {
                    const auto skeleton_bytes = resolve_chunk(options->skeleton_path);
                    if (!skeleton_bytes) {
                        std::fprintf(stderr, "warning: skeleton not found: %s\n",
                                     options->skeleton_path.c_str());
                    } else {
                        skeleton = lolview::Skl::parse(*skeleton_bytes);
                        has_skeleton = true;
                    }
                }

                lolview::Anm animation;
                bool has_animation = false;
                if (!options->anim_path.empty()) {
                    const auto anim_bytes = resolve_chunk(options->anim_path);
                    if (!anim_bytes) {
                        std::fprintf(stderr, "warning: animation not found: %s\n",
                                     options->anim_path.c_str());
                    } else {
                        animation = lolview::Anm::parse(*anim_bytes);
                        has_animation = true;
                    }
                }

                std::printf("mesh:       %s\n", options->mesh_path.c_str());
                std::printf("source:     %s (%u -> %u B)\n",
                            wad.path().filename().string().c_str(), e.compressed_size,
                            e.uncompressed_size);
                std::printf("version:    %u.%u   vertex stride: %u\n", mesh.major_version(),
                            mesh.minor_version(), mesh.vertex_stride());
                std::printf("materials:  %zu\n", mesh.materials().size());
                for (const auto& material : mesh.materials()) {
                    std::printf("  %-24s verts %u+%u  indices %u+%u\n", material.name.c_str(),
                                material.start_vertex, material.num_vertices,
                                material.start_index, material.num_indices);
                }
                std::printf("geometry:   %zu vertices, %zu indices, %zu triangles\n",
                            mesh.vertices().size(), mesh.indices().size(),
                            mesh.triangle_count());
                std::printf("bounds:     min (%.3f, %.3f, %.3f)  max (%.3f, %.3f, %.3f)\n",
                            bounds.min[0], bounds.min[1], bounds.min[2], bounds.max[0],
                            bounds.max[1], bounds.max[2]);

                if (has_skeleton) {
                    std::printf("skeleton:   %zu joints\n", skeleton.joints().size());
                }
                if (has_animation) {
                    std::printf("animation:  v%u, %zu tracks, %zu frames, %.2f s @ %.1f fps\n",
                                animation.version(), animation.track_count(),
                                animation.frame_count(), animation.duration(), animation.fps());
                }

                std::vector<lolview::Mat4> bones;
                if (has_skeleton && has_animation) {
                    bones = lolview::compute_skin_matrices(skeleton, &animation, options->time);
                    std::printf("pose:       t=%.3f s, %zu skin matrices\n", options->time,
                                bones.size());
                }

                if (options->render) {
#ifdef LOLVIEW_HAS_VIEWER
                    lolview::RenderOptions render_options;
                    render_options.width = options->width;
                    render_options.height = options->height;
                    render_options.yaw_degrees = options->yaw;
                    render_options.pitch_degrees = options->pitch;

                    const fs::path png = options->out_file.empty() ? fs::path("mesh.png")
                                                                   : options->out_file;

                    lolview::TexImage texture;
                    bool has_texture = false;
                    if (!options->texture_path.empty()) {
                        const std::uint64_t texture_key =
                            lolview::path_hash(options->texture_path);
                        for (const auto& archive_path : archives) {
                            if (archive_path.string().find(options->archive_filter) ==
                                std::string::npos) {
                                continue;
                            }
                            const auto wad = lolview::Wad::open(archive_path);
                            const auto index = wad.find(texture_key);
                            if (!index) continue;
                            texture = lolview::decode_tex(wad.read_chunk(wad.entries()[*index]));
                            has_texture = true;
                            std::printf("texture:    %s (%dx%d %s)\n",
                                        options->texture_path.c_str(), texture.width,
                                        texture.height, texture.format.c_str());
                            break;
                        }
                        if (!has_texture) {
                            std::fprintf(stderr, "warning: texture not found: %s\n",
                                         options->texture_path.c_str());
                        }
                    }

                    std::vector<lolview::Particle> particles;
                    if (!options->effect_name.empty()) {
                        const lolview::Vec3 origin{
                            (bounds.min[0] + bounds.max[0]) * 0.5f,
                            bounds.min[1] + 0.6f * (bounds.max[1] - bounds.min[1]),
                            bounds.max[2] + 15.0f};
                        particles = lolview::simulate_effect(
                            lolview::effect_from_name(options->effect_name), options->time, origin,
                            lolview::Vec3{0.0f, 0.0f, 1.0f});
                        std::printf("effect:     %s, %zu particles at t=%.2f s\n",
                                    options->effect_name.c_str(), particles.size(),
                                    options->time);
                    }

                    lolview::Renderer renderer;
                    renderer.render_skn(mesh, has_texture ? &texture : nullptr,
                                        bones.empty() ? nullptr : &bones,
                                        particles.empty() ? nullptr : &particles, render_options,
                                        png);                    std::printf("rendered:   %s  (%dx%d, GL %s)\n", png.string().c_str(),
                                render_options.width, render_options.height,
                                renderer.gl_version().c_str());
#else
                    std::fprintf(stderr,
                                 "error: --render needs a build with the viewer enabled "
                                 "(LOLVIEW_BUILD_VIEWER=ON)\n");
                    return 2;
#endif
                }
                return 0;
            }
            std::printf("no chunk for that path in the selected archive(s)\n");
            return 1;
        }

        if (!options->list) {
            std::size_t shown = 0;            for (const auto& a : archives) {
                if (shown++ >= 12) {
                    std::printf("  ... and %zu more\n", archives.size() - 12);
                    break;
                }
                std::error_code ec;
                const auto size = fs::file_size(a, ec);
                std::printf("  %-48s %s\n", fs::relative(a, source.game_dir()).string().c_str(),
                            ec ? "?" : human_bytes(size).c_str());
            }
        }

        if (!options->list) return 0;

        std::size_t total_entries = 0;
        for (const auto& archive_path : archives) {
            if (!options->archive_filter.empty() &&
                archive_path.string().find(options->archive_filter) == std::string::npos) {
                continue;
            }
            const auto wad = lolview::Wad::open(archive_path);
            std::printf("\n== %s ==\n", wad.path().filename().string().c_str());
            std::printf("   tables: %zu chunks, TOC at 0x%zx, %s\n",
                        wad.entries().size(), wad.toc_offset(),
                        human_bytes(wad.file_size()).c_str());

            const std::size_t shown = std::min(options->limit, wad.entries().size());
            for (std::size_t i = 0; i < shown; ++i) {
                const auto& e = wad.entries()[i];
                std::printf("   %016" PRIx64 "  %6u -> %6u B  %-10s @0x%08x\n",
                            e.hash, e.compressed_size, e.uncompressed_size,
                            std::string(lolview::compression_name(e.compression)).c_str(),
                            e.offset);
            }
            if (shown < wad.entries().size()) {
                std::printf("   ... %zu more\n", wad.entries().size() - shown);
            }
            total_entries += wad.entries().size();
        }
        std::printf("\ntotal chunks: %zu\n", total_entries);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
