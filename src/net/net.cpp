#include <lolview/net.hpp>

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lolview::net {
namespace {

// ---- little-endian writer / reader -------------------------------------

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xff));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    }
    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
        }
    }
    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
        }
    }
    void f32(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void str(const std::string& value) {
        u16(static_cast<std::uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> finish(MessageType type) {
        std::vector<std::uint8_t> packet;
        packet.reserve(12 + bytes_.size());
        packet.insert(packet.end(), kMagic, kMagic + 4);
        packet.push_back(static_cast<std::uint8_t>(type));
        packet.push_back(0);
        packet.push_back(0);
        packet.push_back(0);
        packet.push_back(0);  // sequence (reserved for now)
        packet.push_back(0);
        packet.push_back(0);
        packet.push_back(0);
        packet.insert(packet.end(), bytes_.begin(), bytes_.end());
        return packet;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return bytes_.size() - offset_; }

    std::uint8_t u8() {
        if (!need(1)) return 0;
        return bytes_[offset_++];
    }
    std::uint16_t u16() {
        if (!need(2)) return 0;
        const std::uint16_t value = static_cast<std::uint16_t>(
            bytes_[offset_] | (bytes_[offset_ + 1] << 8));
        offset_ += 2;
        return value;
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(bytes_[offset_ + i]) << (8 * i);
        }
        offset_ += 4;
        return value;
    }
    std::uint64_t u64() {
        if (!need(8)) return 0;
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + i]) << (8 * i);
        }
        offset_ += 8;
        return value;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string str() {
        const std::uint16_t length = u16();
        if (!need(length)) return {};
        std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return value;
    }
    void skip(std::size_t count) {
        if (need(count)) offset_ += count;
    }

private:
    bool need(std::size_t count) {
        if (offset_ + count > bytes_.size()) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

Reader payload_reader(std::span<const std::uint8_t> bytes, MessageType expected) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        return Reader({});
    }
    if (static_cast<MessageType>(bytes[4]) != expected) {
        return Reader({});
    }
    return Reader(bytes.subspan(12));
}

}  // namespace

std::vector<std::uint8_t> encode(const Hello& message) {
    Writer writer;
    writer.u32(message.version);
    writer.u64(message.content_hash);
    writer.str(message.name);
    writer.str(message.champion);
    return writer.finish(MessageType::Hello);
}

std::vector<std::uint8_t> encode(const Welcome& message) {
    Writer writer;
    writer.u16(message.player_id);
    writer.u16(message.unit_id);
    writer.u32(message.tick);
    writer.u16(static_cast<std::uint16_t>(message.models.size()));
    for (const auto& model : message.models) writer.str(model);
    return writer.finish(MessageType::Welcome);
}

std::vector<std::uint8_t> encode(const Intent& message) {
    Writer writer;
    writer.u16(message.player_id);
    writer.u8(static_cast<std::uint8_t>(message.kind));
    writer.u8(message.slot);
    writer.f32(message.x);
    writer.f32(message.y);
    writer.f32(message.z);
    writer.u16(message.target);
    return writer.finish(MessageType::Intent);
}

std::vector<std::uint8_t> encode(const Snapshot& message) {
    Writer writer;
    writer.u32(message.tick);
    writer.u16(static_cast<std::uint16_t>(message.units.size()));
    for (const auto& unit : message.units) {
        writer.u16(unit.id);
        writer.u8(unit.model);
        writer.f32(unit.x);
        writer.f32(unit.y);
        writer.f32(unit.z);
        writer.f32(unit.yaw);
        writer.f32(unit.hp);
        writer.f32(unit.max_hp);
        writer.u8(unit.team);
        writer.str(unit.anim);
    }
    return writer.finish(MessageType::Snapshot);
}

std::vector<std::uint8_t> encode(const Players& message) {
    Writer writer;
    writer.u16(static_cast<std::uint16_t>(message.list.size()));
    for (const auto& player : message.list) {
        writer.u16(player.player_id);
        writer.u16(player.unit_id);
        writer.str(player.champion);
    }
    return writer.finish(MessageType::Players);
}

std::optional<MessageType> peek_type(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 5 || std::memcmp(bytes.data(), kMagic, 4) != 0) return std::nullopt;
    const auto type = static_cast<MessageType>(bytes[4]);
    switch (type) {
        case MessageType::Hello:
        case MessageType::Welcome:
        case MessageType::Intent:
        case MessageType::Snapshot:
        case MessageType::Players:
            return type;
        default:
            return std::nullopt;
    }
}

std::optional<Hello> decode_hello(std::span<const std::uint8_t> bytes) {
    Reader reader = payload_reader(bytes, MessageType::Hello);
    if (reader.remaining() == 0) return std::nullopt;
    Hello hello;
    hello.version = reader.u32();
    hello.content_hash = reader.u64();
    hello.name = reader.str();
    hello.champion = reader.str();
    if (!reader.ok()) return std::nullopt;
    return hello;
}

std::optional<Welcome> decode_welcome(std::span<const std::uint8_t> bytes) {
    Reader reader = payload_reader(bytes, MessageType::Welcome);
    if (reader.remaining() == 0) return std::nullopt;
    Welcome welcome;
    welcome.player_id = reader.u16();
    welcome.unit_id = reader.u16();
    welcome.tick = reader.u32();
    const std::uint16_t count = reader.u16();
    welcome.models.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) welcome.models.push_back(reader.str());
    if (!reader.ok()) return std::nullopt;
    return welcome;
}

std::optional<Intent> decode_intent(std::span<const std::uint8_t> bytes) {
    Reader reader = payload_reader(bytes, MessageType::Intent);
    if (reader.remaining() == 0) return std::nullopt;
    Intent intent;
    intent.player_id = reader.u16();
    intent.kind = static_cast<IntentKind>(reader.u8());
    intent.slot = reader.u8();
    intent.x = reader.f32();
    intent.y = reader.f32();
    intent.z = reader.f32();
    intent.target = reader.u16();
    if (!reader.ok()) return std::nullopt;
    return intent;
}

std::optional<Players> decode_players(std::span<const std::uint8_t> bytes) {
    Reader reader = payload_reader(bytes, MessageType::Players);
    if (reader.remaining() == 0) return std::nullopt;
    Players players;
    const std::uint16_t count = reader.u16();
    players.list.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        PlayerInfo info;
        info.player_id = reader.u16();
        info.unit_id = reader.u16();
        info.champion = reader.str();
        players.list.push_back(info);
    }
    if (!reader.ok()) return std::nullopt;
    return players;
}

std::optional<Snapshot> decode_snapshot(std::span<const std::uint8_t> bytes) {
    Reader reader = payload_reader(bytes, MessageType::Snapshot);
    if (reader.remaining() == 0) return std::nullopt;
    Snapshot snapshot;
    snapshot.tick = reader.u32();
    const std::uint16_t count = reader.u16();
    snapshot.units.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        UnitState unit;
        unit.id = reader.u16();
        unit.model = reader.u8();
        unit.x = reader.f32();
        unit.y = reader.f32();
        unit.z = reader.f32();
        unit.yaw = reader.f32();
        unit.hp = reader.f32();
        unit.max_hp = reader.f32();
        unit.team = reader.u8();
        unit.anim = reader.str();
        snapshot.units.push_back(unit);
    }
    if (!reader.ok()) return std::nullopt;
    return snapshot;
}

// ---- socket ------------------------------------------------------------

#ifdef _WIN32
namespace {
struct WinsockInit {
    WinsockInit() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
};
WinsockInit& winsock() {
    static WinsockInit init;
    return init;
}
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
}  // namespace
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

UdpSocket::UdpSocket() {
#ifdef _WIN32
    (void)winsock();
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    handle_ = socket == INVALID_SOCKET ? kInvalid : static_cast<std::intptr_t>(socket);
#else
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    handle_ = socket < 0 ? kInvalid : static_cast<std::intptr_t>(socket);
#endif
}

UdpSocket::~UdpSocket() {
    if (handle_ != kInvalid) {
#ifdef _WIN32
        closesocket(static_cast<NativeSocket>(handle_));
#else
        ::close(static_cast<NativeSocket>(handle_));
#endif
    }
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalid;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        if (handle_ != kInvalid) {
#ifdef _WIN32
            closesocket(static_cast<NativeSocket>(handle_));
#else
            ::close(static_cast<NativeSocket>(handle_));
#endif
        }
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

bool UdpSocket::bind(std::uint16_t port) {
    if (handle_ == kInvalid) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    return ::bind(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0;
}

void UdpSocket::set_non_blocking(bool non_blocking) {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    u_long mode = non_blocking ? 1u : 0u;
    ioctlsocket(static_cast<NativeSocket>(handle_), FIONBIO, &mode);
#else
    (void)non_blocking;
#endif
}

std::uint16_t UdpSocket::local_port() const {
    if (handle_ == kInvalid) return 0;
    sockaddr_in address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&address),
                    &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

bool UdpSocket::send_to(const std::string& host, std::uint16_t port,
                        std::span<const std::uint8_t> payload) {
    if (handle_ == kInvalid) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) return false;
    const int sent = static_cast<int>(
        ::sendto(static_cast<NativeSocket>(handle_),
                 reinterpret_cast<const char*>(payload.data()),
                 static_cast<int>(payload.size()), 0,
                 reinterpret_cast<sockaddr*>(&address), sizeof(address)));
    return sent == static_cast<int>(payload.size());
}

std::optional<UdpSocket::Datagram> UdpSocket::recv() {
    if (handle_ == kInvalid) return std::nullopt;
    std::uint8_t buffer[2048];
    sockaddr_in from{};
#ifdef _WIN32
    int from_length = sizeof(from);
#else
    socklen_t from_length = sizeof(from);
#endif
    const int received = static_cast<int>(
        ::recvfrom(static_cast<NativeSocket>(handle_), reinterpret_cast<char*>(buffer),
                   static_cast<int>(sizeof(buffer)), 0,
                   reinterpret_cast<sockaddr*>(&from), &from_length));
    if (received <= 0) return std::nullopt;

    Datagram datagram;
    char host[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
    datagram.from_host = host;
    datagram.from_port = ntohs(from.sin_port);
    datagram.data.assign(buffer, buffer + received);
    return datagram;
}

}  // namespace lolview::net
