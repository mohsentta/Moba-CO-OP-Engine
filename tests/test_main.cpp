// Dependency-free test runner for the core library.
//
// Deliberately no framework: the format code should build and be testable on a
// machine with nothing but a C++20 compiler. (Fixtures are synthetic; no game
// data is ever committed or required.)

#include <lolview/asset_source.hpp>
#include <lolview/bin.hpp>
#include <lolview/decompress.hpp>
#include <lolview/math.hpp>
#include <lolview/net.hpp>
#include <lolview/walkable.hpp>
#include <lolview/skn.hpp>
#include <lolview/wad.hpp>
#include <lolview/vfx.hpp>
#include <lolview/xxhash64.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>
#include <zstd.h>

namespace fs = std::filesystem;

namespace {

using namespace lolview;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Tests must not depend on the system TEMP being writable (it is not on some
// machines); scratch files live beside the build instead.
fs::path test_scratch_dir() {
    const fs::path dir = fs::current_path() / "lolview-test-scratch";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void put_u32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

void put_u64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[o + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// Synthetic two-chunk archive: 16-byte header, TOC of two 32-byte records,
// payloads starting at 80 and touching end-of-file at 100.
std::vector<std::uint8_t> make_archive() {
    std::vector<std::uint8_t> b(100, 0);
    b[0] = 'R';
    b[1] = 'W';
    b[16 + 0] = 0;
    put_u64(b, 16, 0x1111111111111111ULL);   // hash
    put_u32(b, 24, 80);                      // offset
    put_u32(b, 28, 10);                      // compressed size
    put_u32(b, 32, 10);                      // uncompressed size
    b[36] = 0;                               // stored

    put_u64(b, 48 + 0, 0x2222222222222222ULL);
    put_u32(b, 56, 90);
    put_u32(b, 60, 10);
    put_u32(b, 64, 10);
    b[68] = 0;

    for (std::size_t i = 80; i < 100; ++i) b[i] = static_cast<std::uint8_t>(i);
    return b;
}

void test_xxhash() {
    CHECK_EQ(lolview::xxh64(std::string_view("")), 0xEF46DB3751D8E999ULL);
    CHECK_EQ(lolview::xxh64(std::string_view("hello")), 0x26C7827D889F6DA3ULL);
    CHECK_EQ(lolview::xxh64(std::string_view("a")) != 0, true);
}

void test_path_hash() {
    CHECK_EQ(lolview::normalize_virtual_path("DATA/A.bin"), std::string("data/a.bin"));
    CHECK_EQ(lolview::path_hash("DATA\\A.bin"), lolview::path_hash("data/a.bin"));
    CHECK(lolview::path_hash("x.skn") != lolview::path_hash("y.skn"));
}

void test_wad_parse() {
    const fs::path dir = test_scratch_dir() / "wad";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "synthetic.wad.client";

    {
        const auto bytes = make_archive();
        std::FILE* f = std::fopen(file.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }

    const auto wad = lolview::Wad::open(file);
    CHECK_EQ(wad.entries().size(), std::size_t{2});
    CHECK_EQ(wad.toc_offset(), std::size_t{16});
    CHECK(wad.find(0x1111111111111111ULL).has_value());

    const auto idx = wad.find(0x2222222222222222ULL);
    CHECK(idx.has_value());
    if (idx) {
        const auto& e = wad.entries()[*idx];
        CHECK_EQ(e.offset, 90u);
        CHECK_EQ(e.uncompressed_size, 10u);
        const auto payload = wad.read_chunk(e);
        CHECK_EQ(payload.size(), std::size_t{10});
        CHECK_EQ(payload[0], static_cast<std::uint8_t>(90));
    }

    // A file that is not an RW archive must be rejected, not parsed as one.
    bool threw = false;
    try {
        const fs::path bad = dir / "not-a-wad.bin";
        std::FILE* f = std::fopen(bad.string().c_str(), "wb");
        if (f) { std::fputc('X', f); std::fclose(f); }
        (void)lolview::Wad::open(bad);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    fs::remove_all(dir, ec);
}

std::vector<std::uint8_t> zstd_compress(const std::string& input) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(input.size()));
    const std::size_t n = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), 5);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

std::vector<std::uint8_t> gzip_compress(const std::string& input) {
    z_stream zs{};
    if (deflateInit2(&zs, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};

    std::vector<std::uint8_t> out(compressBound(input.size()) + 64);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&zs, Z_FINISH);
    const std::size_t n = out.size() - zs.avail_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return {};
    out.resize(n);
    return out;
}

void test_decompress() {
    const std::string payload =
        "lolview decompressor roundtrip: the quick brown fox jumps over the lazy dog 0123456789 "
        "0123456789 0123456789 0123456789";

    // zstd: round trip, exact-length enforcement, and truncation rejection.
    {
        const auto packed = zstd_compress(payload);
        CHECK(!packed.empty());

        const auto back = lolview::zstd_decompress(packed, payload.size());
        CHECK_EQ(back.size(), payload.size());
        CHECK(std::equal(back.begin(), back.end(), payload.begin()));

        bool threw = false;
        try {
            (void)lolview::zstd_decompress(packed, payload.size() - 1);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);

        threw = false;
        try {
            (void)lolview::zstd_decompress(
                std::span<const std::uint8_t>(packed.data(), packed.size() - 4), payload.size());
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }

    // zstd-multi: several independent frames concatenated for one chunk.
    {
        const std::string first = payload.substr(0, 40);
        const std::string second = payload.substr(40);
        auto packed = zstd_compress(first);
        const auto packed2 = zstd_compress(second);
        packed.insert(packed.end(), packed2.begin(), packed2.end());

        const auto back = lolview::zstd_decompress(packed, payload.size());
        CHECK_EQ(back.size(), payload.size());
        CHECK(std::equal(back.begin(), back.end(), payload.begin()));
    }

    // gzip member.
    {
        const auto packed = gzip_compress(payload);
        CHECK(!packed.empty());

        const auto back = lolview::gzip_decompress(packed, payload.size());
        CHECK_EQ(back.size(), payload.size());
        CHECK(std::equal(back.begin(), back.end(), payload.begin()));

        bool threw = false;
        try {
            (void)lolview::gzip_decompress(packed, payload.size() + 8);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
    }
}

void put_f32(std::vector<std::uint8_t>& b, std::size_t o, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(b, o, bits);
}

// Synthetic version-4 SKN: one material, two triangles, four vertices.
std::vector<std::uint8_t> make_skn() {
    constexpr std::size_t kStride = 52;
    std::vector<std::uint8_t> b(12 + 80 + 60 + 12 + 4 * kStride, 0);
    std::size_t o = 0;

    put_u32(b, 0, 0x00112233);
    b[4] = 4; b[5] = 0;              // major 4, minor 0
    b[6] = 1; b[7] = 0;
    put_u32(b, 8, 1);                // material count
    o = 12;

    std::memcpy(b.data() + o, "Body", 4);
    o += 64;
    put_u32(b, o, 0); o += 4;        // start vertex
    put_u32(b, o, 4); o += 4;        // vertex count
    put_u32(b, o, 0); o += 4;        // start index
    put_u32(b, o, 6); o += 4;        // index count

    put_u32(b, o, 0); o += 4;        // unk0
    put_u32(b, o, 6); o += 4;        // total indices
    put_u32(b, o, 4); o += 4;        // total vertices
    put_u32(b, o, static_cast<std::uint32_t>(kStride)); o += 4;
    put_u32(b, o, 0); o += 4;        // unk1
    for (int i = 0; i < 10; ++i) { put_f32(b, o, static_cast<float>(i)); o += 4; }

    const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    for (std::uint16_t index : indices) {
        b[o] = static_cast<std::uint8_t>(index & 0xFF);
        b[o + 1] = static_cast<std::uint8_t>(index >> 8);
        o += 2;
    }

    for (int v = 0; v < 4; ++v) {
        const float fv = static_cast<float>(v);
        put_f32(b, o, fv); put_f32(b, o + 4, fv * 2.0f); put_f32(b, o + 8, fv * 3.0f); o += 12;
        b[o] = 0; b[o + 1] = 1; b[o + 2] = 2; b[o + 3] = 3; o += 4;
        put_f32(b, o, 1.0f); put_f32(b, o + 4, 0.0f); put_f32(b, o + 8, 0.0f);
        put_f32(b, o + 12, 0.0f); o += 16;
        put_f32(b, o, 0.0f); put_f32(b, o + 4, 0.0f); put_f32(b, o + 8, 1.0f); o += 12;
        put_f32(b, o, 0.5f); put_f32(b, o + 4, 0.5f); o += 8;
    }
    return b;
}

void test_skn() {
    const auto bytes = make_skn();
    const auto mesh = lolview::Skn::parse(bytes);

    CHECK_EQ(mesh.major_version(), std::uint16_t{4});
    CHECK_EQ(mesh.minor_version(), std::uint16_t{1});
    CHECK_EQ(mesh.vertex_stride(), 52u);
    CHECK_EQ(mesh.materials().size(), std::size_t{1});
    CHECK_EQ(mesh.materials()[0].name, std::string("Body"));
    CHECK_EQ(mesh.materials()[0].num_indices, 6u);
    CHECK_EQ(mesh.indices().size(), std::size_t{6});
    CHECK_EQ(mesh.vertices().size(), std::size_t{4});
    CHECK_EQ(mesh.triangle_count(), std::size_t{2});

    const auto bounds = mesh.compute_bounds();
    CHECK_EQ(bounds.min[0], 0.0f);
    CHECK_EQ(bounds.max[0], 3.0f);
    CHECK_EQ(mesh.vertices()[3].bone_indices[3], std::uint8_t{3});
    CHECK_EQ(mesh.vertices()[2].uv[0], 0.5f);
    CHECK_EQ(mesh.bbox()[9], 9.0f);

    // Wrong magic must be rejected, not silently accepted.
    auto bad_magic = bytes;
    bad_magic[0] = 0;
    bool threw = false;
    try {
        (void)lolview::Skn::parse(bad_magic);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // A truncated buffer must be rejected.
    threw = false;
    try {
        (void)lolview::Skn::parse(std::span<const std::uint8_t>(bytes.data(), 100));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void test_math_pose() {
    // A rigid transform and its inverse must compose to identity.
    const Vec3 translation{1.0f, 2.0f, -3.0f};
    const Vec3 scale{2.0f, 2.0f, 2.0f};
    const Quat rotation = normalize(Quat{0.0f, 0.7071068f, 0.0f, 0.7071068f});
    const Mat4 m = trs_matrix(translation, scale, rotation);
    const Mat4 composed = m * inverse_affine(m);

    for (int i = 0; i < 16; ++i) {
        const float expected = (i % 5 == 0) ? 1.0f : 0.0f;
        CHECK(std::fabs(composed.m[i] - expected) < 1e-4f);
    }

    // Trs must place the translation in the last column (column-vector layout).
    CHECK_EQ(m.m[12], 1.0f);
    CHECK_EQ(m.m[13], 2.0f);
    CHECK_EQ(m.m[14], -3.0f);

    // A 90-degree Y rotation maps +X onto -Z.
    const Mat4 rot_only = trs_matrix(Vec3{}, Vec3{1.0f, 1.0f, 1.0f},
                                     normalize(Quat{0.0f, 0.7071068f, 0.0f, 0.7071068f}));
    const float x = 1.0f;
    const float z = rot_only.m[2] * x;
    CHECK(std::fabs(z + 1.0f) < 1e-3f);
}

void test_vfx() {
    const Vec3 origin{0.0f, 100.0f, 0.0f};
    const Vec3 forward{0.0f, 0.0f, 1.0f};

    const auto q = lolview::simulate_effect(lolview::EffectKind::Spell1, 0.4f, origin, forward);
    CHECK(!q.empty());

    // Deterministic: identical inputs must produce identical particles.
    const auto q2 = lolview::simulate_effect(lolview::EffectKind::Spell1, 0.4f, origin, forward);
    CHECK_EQ(q.size(), q2.size());
    if (!q.empty() && !q2.empty()) {
        CHECK_EQ(q[0].position.z, q2[0].position.z);
        CHECK_EQ(q[0].size, q2[0].size);
    }

    // The projectile must travel along the forward axis.
    float max_z = -1e9f;
    for (const auto& p : q) max_z = std::max(max_z, p.position.z);
    CHECK(max_z > origin.z);

    // After its lifetime the effect is gone.
    const auto late = lolview::simulate_effect(lolview::EffectKind::Spell2, 30.0f, origin, forward);
    CHECK(late.empty());

    CHECK(lolview::effect_from_name("q") == lolview::EffectKind::Spell1);
    CHECK(lolview::effect_from_name("nope") == lolview::EffectKind::None);
}

void test_cube() {
    const auto cube = lolview::Skn::make_cube(10.0f);
    CHECK_EQ(cube.vertices().size(), std::size_t{24});
    CHECK_EQ(cube.indices().size(), std::size_t{36});
    CHECK_EQ(cube.triangle_count(), std::size_t{12});
    const auto bounds = cube.compute_bounds();
    CHECK_EQ(bounds.min[0], -10.0f);
    CHECK_EQ(bounds.max[2], 10.0f);
}

void test_bin_hash() {
    // Bin names are FNV-1a 32 over the *lowercased* name; this is the class
    // hash of a VFX system entry, verified against a real bin.
    CHECK_EQ(lolview::fnv1a("VfxSystemDefinitionData"), 0x45cd899Fu);
    CHECK_EQ(lolview::fnv1a("vfxsystemdefinitiondata"), 0x45cd899Fu);
    CHECK(lolview::fnv1a("particleName") == lolview::fnv1a("particlename"));
    CHECK(lolview::fnv1a("") == 0x811C9DC5u);
}

void test_asset_source_guardrail() {
    // A directory that is not an installation must be refused.
    const fs::path dir = test_scratch_dir() / "not-a-game";
    std::error_code ec;
    fs::create_directories(dir, ec);

    bool threw = false;
    try {
        (void)lolview::AssetSource(dir);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    CHECK_EQ(lolview::AssetSource::ships_assets(), false);
    fs::remove_all(dir, ec);
}

void test_walk_area() {
    using namespace lolview;

    // Square polygon, 100 units around the origin.
    WalkArea area;
    area.parse("path 1\npoint -100 -100\npoint 100 -100\npoint 100 100\npoint -100 100\nend\n");
    CHECK_EQ(area.polygons.size(), std::size_t{1});
    CHECK(area.contains(0.0f, 0.0f));
    CHECK(area.contains(90.0f, -90.0f));
    CHECK(!area.contains(150.0f, 0.0f));
    CHECK(!area.contains(0.0f, -150.0f));

    // An empty area imposes no restriction.
    WalkArea unrestricted;
    CHECK(unrestricted.contains(12345.0f, -9999.0f));

    // A second polygon extends the walkable set.
    area.parse("path 2\npoint 200 200\npoint 400 200\npoint 400 400\npoint 200 400\nend\n");
    CHECK_EQ(area.polygons.size(), std::size_t{2});
    CHECK(area.contains(300.0f, 300.0f));
    CHECK(!area.contains(150.0f, 150.0f));

    // Obstacles subtract from the walkable set.
    WalkArea blocked;
    blocked.parse(
        "path 1\npoint -100 -100\npoint 100 -100\npoint 100 100\npoint -100 100\nend\n"
        "obstacle 1\npoint -20 -20\npoint 20 -20\npoint 20 20\npoint -20 20\nend\n");
    CHECK_EQ(blocked.polygons.size(), std::size_t{1});
    CHECK_EQ(blocked.obstacles.size(), std::size_t{1});
    CHECK(blocked.contains(80.0f, 80.0f));
    CHECK(!blocked.contains(0.0f, 0.0f));  // inside the obstacle

    // The navigation grid routes around the obstacle, never through it.
    NavGrid grid;
    grid.build(blocked, 10.0f);
    CHECK(grid.valid());
    std::vector<std::pair<float, float>> route;
    CHECK(grid.find_path(-90.0f, 0.0f, 90.0f, 0.0f, route));
    CHECK(!route.empty());
    for (const auto& point : route) {
        CHECK(blocked.contains(point.first, point.second));
    }
    // Smoothed paths: a straight shot should collapse to a single waypoint, and
    // every segment must stay inside the area.
    std::vector<std::pair<float, float>> straight;
    CHECK(grid.find_path(-90.0f, -60.0f, 90.0f, -60.0f, straight));
    CHECK(straight.size() <= 3);
    float previous_x = -90.0f, previous_z = -60.0f;
    for (const auto& point : straight) {
        CHECK(grid.line_clear(previous_x, previous_z, point.first, point.second));
        previous_x = point.first;
        previous_z = point.second;
    }
    CHECK(std::fabs(previous_x - 90.0f) < 1.0f);
    CHECK(std::fabs(previous_z + 60.0f) < 1.0f);

    // A point outside the area can be pulled to the nearest walkable cell.
    float inside_x = 0.0f, inside_z = 0.0f;
    CHECK(grid.nearest_walkable(-70.0f, 0.0f, inside_x, inside_z));
    CHECK(blocked.contains(inside_x, inside_z));
    CHECK(grid.nearest_walkable(-300.0f, -300.0f, inside_x, inside_z));
    CHECK(blocked.contains(inside_x, inside_z));

    // A target outside the walkable area has no route.
    std::vector<std::pair<float, float>> unreachable;
    CHECK(!grid.find_path(-90.0f, 0.0f, 500.0f, 0.0f, unreachable));

    // Malformed blocks are skipped, not fatal.
    WalkArea broken;
    broken.parse("path 3\npoint 0 0\npoint 10 10\nend\n");
    CHECK_EQ(broken.polygons.size(), std::size_t{0});
}

void test_net() {
    using namespace lolview::net;

    Hello hello;
    hello.name = "jinx";
    hello.champion = "Jinx";
    hello.content_hash = 0x1122334455667788ull;
    const auto hello_bytes = encode(hello);
    // header + version + hash + name + champion
    CHECK_EQ(hello_bytes.size(), std::size_t{12} + 4 + 8 + (2 + 4) + (2 + 4));
    CHECK(peek_type(hello_bytes) == MessageType::Hello);
    const auto hello_back = decode_hello(hello_bytes);
    CHECK(hello_back.has_value());
    if (hello_back) {
        CHECK_EQ(hello_back->name, std::string("jinx"));
        CHECK_EQ(hello_back->content_hash, hello.content_hash);
        CHECK_EQ(hello_back->version, kProtocolVersion);
        CHECK_EQ(hello_back->champion, std::string("Jinx"));
    }

    Intent intent;
    intent.player_id = 3;
    intent.kind = IntentKind::Cast;
    intent.slot = 2;
    intent.x = 1.5f;
    intent.y = -2.25f;
    intent.z = 300.0f;
    intent.target = 7;
    const auto intent_back = decode_intent(encode(intent));
    CHECK(intent_back.has_value());
    if (intent_back) {
        CHECK_EQ(intent_back->player_id, std::uint16_t{3});
        CHECK(intent_back->kind == IntentKind::Cast);
        CHECK_EQ(intent_back->slot, std::uint8_t{2});
        CHECK(std::fabs(intent_back->x - 1.5f) < 1e-6f);
        CHECK(std::fabs(intent_back->z - 300.0f) < 1e-6f);
        CHECK_EQ(intent_back->target, std::uint16_t{7});
    }

    Welcome welcome;
    welcome.player_id = 1;
    welcome.tick = 42;
    welcome.models = {"minion", "jinx"};
    const auto welcome_back = decode_welcome(encode(welcome));
    CHECK(welcome_back.has_value());
    if (welcome_back) {
        CHECK_EQ(welcome_back->models.size(), std::size_t{2});
        CHECK_EQ(welcome_back->models[1], std::string("jinx"));
    }

    Snapshot snapshot;
    snapshot.tick = 9;
    UnitState unit;
    unit.id = 5;
    unit.model = 1;
    unit.x = 10.0f;
    unit.hp = 100.0f;
    unit.max_hp = 200.0f;
    unit.team = 1;
    unit.anim = "run";
    snapshot.units.push_back(unit);

    Players roster;
    PlayerInfo info;
    info.player_id = 1;
    info.unit_id = 4;
    info.champion = "Jinx";
    roster.list.push_back(info);
    const auto roster_back = decode_players(encode(roster));
    CHECK(roster_back.has_value());
    if (roster_back) {
        CHECK_EQ(roster_back->list.size(), std::size_t{1});
        CHECK_EQ(roster_back->list[0].unit_id, std::uint16_t{4});
        CHECK_EQ(roster_back->list[0].champion, std::string("Jinx"));
    }
    const auto snapshot_back = decode_snapshot(encode(snapshot));
    CHECK(snapshot_back.has_value());
    if (snapshot_back) {
        CHECK_EQ(snapshot_back->tick, std::uint32_t{9});
        CHECK_EQ(snapshot_back->units.size(), std::size_t{1});
        CHECK_EQ(snapshot_back->units[0].id, std::uint16_t{5});
        CHECK_EQ(snapshot_back->units[0].team, std::uint8_t{1});
        CHECK_EQ(snapshot_back->units[0].anim, std::string("run"));
    }

    // Truncated or mistyped datagrams must be rejected, not read past their end.
    auto truncated = encode(intent);
    truncated.resize(truncated.size() - 3);
    CHECK(!decode_intent(truncated).has_value());
    CHECK(!decode_hello(encode(intent)).has_value());

    // Loopback: payloads must survive a real UDP round trip.
    UdpSocket client;
    UdpSocket server;
    CHECK(client.bind(0));
    CHECK(server.bind(0));
    client.set_non_blocking(true);
    server.set_non_blocking(true);
    const std::uint16_t port = server.local_port();
    CHECK(port != 0);
    CHECK(client.send_to("127.0.0.1", port, hello_bytes));

    std::optional<UdpSocket::Datagram> received;
    for (int attempt = 0; attempt < 300 && !received; ++attempt) {
        received = server.recv();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(received.has_value());
    if (received) {
        CHECK_EQ(received->data.size(), hello_bytes.size());
        CHECK(decode_hello(received->data).has_value());
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    try {
        test_xxhash();
        test_path_hash();
        test_wad_parse();
        test_decompress();
        test_skn();
        test_math_pose();
        test_vfx();
        test_cube();
        test_bin_hash();
        test_asset_source_guardrail();
        test_walk_area();
        test_net();
    } catch (const std::exception& e) {
        std::printf("FAIL uncaught exception: %s\n", e.what());
        ++g_failures;
    }

    std::error_code ec;
    fs::remove_all(test_scratch_dir(), ec);

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
