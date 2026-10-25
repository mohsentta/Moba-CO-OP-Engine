#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lolview::net {

// Wire protocol for the co-op session. All integers are little-endian.
//
//   HELLO    client -> server : name, protocol version, content hash
//   WELCOME  server -> client : player id, tick, model table
//   INTENT   client -> server : player, kind (move/attack/cast), slot, target
//   SNAPSHOT server -> client : tick + one record per unit
constexpr std::uint32_t kProtocolVersion = 2;
constexpr const char* kMagic = "LVN1";

enum class MessageType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Intent = 3,
    Snapshot = 4,
};

enum class IntentKind : std::uint8_t {
    Move = 0,
    Attack = 1,
    Cast = 2,
};

inline constexpr std::uint16_t kNoTarget = 0xffff;

struct Hello {
    std::string name;
    std::uint32_t version = kProtocolVersion;
    std::uint64_t content_hash = 0;
};

struct Welcome {
    std::uint16_t player_id = 0;
    std::uint16_t unit_id = 0;  // this player's own unit, so the client can skip it
    std::uint32_t tick = 0;
    std::vector<std::string> models;
};

struct Intent {
    std::uint16_t player_id = 0;
    IntentKind kind = IntentKind::Move;
    std::uint8_t slot = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint16_t target = kNoTarget;
};

struct UnitState {
    std::uint16_t id = 0;
    std::uint8_t model = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float hp = 0.0f;
    float max_hp = 0.0f;
    std::uint8_t team = 0;
};

struct Snapshot {
    std::uint32_t tick = 0;
    std::vector<UnitState> units;
};

std::vector<std::uint8_t> encode(const Hello& message);
std::vector<std::uint8_t> encode(const Welcome& message);
std::vector<std::uint8_t> encode(const Intent& message);
std::vector<std::uint8_t> encode(const Snapshot& message);

std::optional<MessageType> peek_type(std::span<const std::uint8_t> bytes);

std::optional<Hello> decode_hello(std::span<const std::uint8_t> bytes);
std::optional<Welcome> decode_welcome(std::span<const std::uint8_t> bytes);
std::optional<Intent> decode_intent(std::span<const std::uint8_t> bytes);
std::optional<Snapshot> decode_snapshot(std::span<const std::uint8_t> bytes);

// Minimal non-blocking UDP socket (IPv4).
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool bind(std::uint16_t port);
    void set_non_blocking(bool non_blocking);
    std::uint16_t local_port() const;

    bool send_to(const std::string& host, std::uint16_t port,
                 std::span<const std::uint8_t> payload);

    struct Datagram {
        std::string from_host;
        std::uint16_t from_port = 0;
        std::vector<std::uint8_t> data;
    };
    std::optional<Datagram> recv();

    bool valid() const { return handle_ != kInvalid; }

private:
    static constexpr std::intptr_t kInvalid = -1;
    std::intptr_t handle_ = kInvalid;
};

}  // namespace lolview::net
