// lolview_server: authoritative game host.
//
// Owns the world (units, HP, tick) and runs the game mode as a Lua script. The
// transport is the UDP protocol from lolview/net.hpp: this executable is the
// shell that keeps the script away from sockets and the player away from state.

#include <lolview/net.hpp>
#include <lolview/walkable.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>
#include <string>
#include <vector>

namespace {

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr float kSnapshotSeconds = 1.0f / 20.0f;

// Model names a unit may use. Indices travel on the wire; the client resolves
// them through the same table sent in WELCOME.
const std::vector<std::string> kModels = {"champion", "pet", "minion", "cube"};

struct Unit {
    std::uint16_t id = 0;
    std::string model;
    float x = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float hp = 100.0f;
    float max_hp = 100.0f;
    int team = 0;
    bool dead = false;
};

struct World {
    std::vector<Unit> units;
    std::uint16_t next_id = 1;
    int tick = 0;
    float elapsed = 0.0f;
    int on_tick_ref = LUA_NOREF;
    int on_intent_ref = LUA_NOREF;
    std::map<std::uint16_t, std::uint16_t> player_units;  // player id -> unit id
    // Walkable area from the editor (scripts/paths.txt). Empty = unrestricted.
    lolview::WalkArea area;

    Unit* find(std::uint16_t id) {
        for (auto& unit : units) {
            if (unit.id == id) return &unit;
        }
        return nullptr;
    }
};

struct Client {
    std::string host;
    std::uint16_t port = 0;
    std::uint16_t player_id = 0;
    std::uint16_t unit_id = 0;
};

World& world_from(lua_State* state) {
    return *static_cast<World*>(lua_touserdata(state, lua_upvalueindex(1)));
}

int world_spawn(lua_State* state) {
    World& world = world_from(state);
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "model");
    Unit unit;
    unit.model = lua_isstring(state, -1) ? lua_tostring(state, -1) : "minion";
    lua_pop(state, 1);

    const auto number_field = [&](const char* name, float fallback) {
        lua_getfield(state, 1, name);
        const float value = lua_isnumber(state, -1) ? static_cast<float>(lua_tonumber(state, -1))
                                                   : fallback;
        lua_pop(state, 1);
        return value;
    };
    unit.x = number_field("x", 0.0f);
    unit.z = number_field("z", 0.0f);
    unit.hp = number_field("hp", 100.0f);
    unit.max_hp = number_field("max_hp", unit.hp);
    unit.team = static_cast<int>(number_field("team", 0.0f));
    unit.id = world.next_id++;
    world.units.push_back(unit);
    lua_pushinteger(state, unit.id);
    return 1;
}

int world_set_hp(lua_State* state) {
    World& world = world_from(state);
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    const float hp = static_cast<float>(luaL_checknumber(state, 2));
    if (Unit* unit = world.find(id)) unit->hp = std::max(0.0f, hp);
    return 0;
}

int world_move(lua_State* state) {
    World& world = world_from(state);
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    const float z = static_cast<float>(luaL_checknumber(state, 3));
    if (Unit* unit = world.find(id)) {
        unit->x = x;
        unit->z = z;
    }
    return 0;
}

int world_unit(lua_State* state) {
    World& world = world_from(state);
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    const Unit* unit = world.find(id);
    if (unit == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    lua_newtable(state);
    lua_pushnumber(state, unit->x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, unit->z);
    lua_setfield(state, -2, "z");
    lua_pushnumber(state, unit->hp);
    lua_setfield(state, -2, "hp");
    lua_pushnumber(state, unit->max_hp);
    lua_setfield(state, -2, "max_hp");
    lua_pushinteger(state, unit->team);
    lua_setfield(state, -2, "team");
    return 1;
}

int world_damage(lua_State* state) {
    World& world = world_from(state);
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    const float amount = static_cast<float>(luaL_checknumber(state, 2));
    if (Unit* unit = world.find(id)) {
        unit->hp = std::max(0.0f, unit->hp - amount);
        if (unit->hp <= 0.0f) unit->dead = true;
        lua_pushnumber(state, unit->hp);
        return 1;
    }
    lua_pushnumber(state, 0.0);
    return 1;
}

int world_remove(lua_State* state) {
    World& world = world_from(state);
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    world.units.erase(std::remove_if(world.units.begin(), world.units.end(),
                                     [&](const Unit& unit) { return unit.id == id; }),
                      world.units.end());
    return 0;
}

int world_time(lua_State* state) {
    World& world = world_from(state);
    lua_pushnumber(state, world.elapsed);
    return 1;
}

int world_on_intent(lua_State* state) {
    World& world = world_from(state);
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_pushvalue(state, 1);
    if (world.on_intent_ref != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, world.on_intent_ref);
    world.on_intent_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return 0;
}

void push_unit(lua_State* state, const Unit& unit) {
    lua_newtable(state);
    lua_pushinteger(state, unit.id);
    lua_setfield(state, -2, "id");
    lua_pushnumber(state, unit.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, unit.z);
    lua_setfield(state, -2, "z");
    lua_pushnumber(state, unit.hp);
    lua_setfield(state, -2, "hp");
    lua_pushnumber(state, unit.max_hp);
    lua_setfield(state, -2, "max_hp");
    lua_pushinteger(state, unit.team);
    lua_setfield(state, -2, "team");
}

int world_player_unit(lua_State* state) {
    World& world = world_from(state);
    const auto player = static_cast<std::uint16_t>(luaL_checkinteger(state, 1));
    const auto it = world.player_units.find(player);
    if (it == world.player_units.end()) {
        lua_pushnil(state);
        return 1;
    }
    const Unit* unit = world.find(it->second);
    if (unit == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_unit(state, *unit);
    return 1;
}

int world_units(lua_State* state) {
    World& world = world_from(state);
    lua_newtable(state);
    int index = 1;
    for (const auto& unit : world.units) {
        if (unit.dead) continue;
        lua_newtable(state);
        lua_pushinteger(state, unit.id);
        lua_setfield(state, -2, "id");
        lua_pushnumber(state, unit.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, unit.z);
        lua_setfield(state, -2, "z");
        lua_pushnumber(state, unit.hp);
        lua_setfield(state, -2, "hp");
        lua_pushnumber(state, unit.max_hp);
        lua_setfield(state, -2, "max_hp");
        lua_pushinteger(state, unit.team);
        lua_setfield(state, -2, "team");
        lua_rawseti(state, -2, index++);
    }
    return 1;
}

int world_count(lua_State* state) {
    World& world = world_from(state);
    lua_pushinteger(state, static_cast<lua_Integer>(world.units.size()));
    return 1;
}

int world_log(lua_State* state) {
    std::printf("[lua] %s\n", luaL_checkstring(state, 1));
    return 0;
}

int world_on_tick(lua_State* state) {
    World& world = world_from(state);
    luaL_checktype(state, 1, LUA_TFUNCTION);
    lua_pushvalue(state, 1);
    if (world.on_tick_ref != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, world.on_tick_ref);
    world.on_tick_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    return 0;
}

void register_world(lua_State* state, World& world) {
    static const luaL_Reg functions[] = {
        {"spawn", world_spawn}, {"set_hp", world_set_hp},       {"move", world_move},
        {"unit", world_unit},   {"count", world_count}, {"units", world_units}, {"player_unit", world_player_unit},         {"log", world_log},
        {"on_tick", world_on_tick}, {"damage", world_damage},   {"remove", world_remove},
        {"time", world_time},       {"on_intent", world_on_intent}, {nullptr, nullptr},
    };
    lua_newtable(state);
    for (const luaL_Reg* function = functions; function->name != nullptr; ++function) {
        lua_pushlightuserdata(state, &world);
        lua_pushcclosure(state, function->func, 1);
        lua_setfield(state, -2, function->name);
    }
    lua_setglobal(state, "world");
}

void dispatch_intent(lua_State* state, World& world, std::uint16_t player_id,
                     lolview::net::IntentKind kind, std::uint8_t slot, std::uint16_t target,
                     float x, float z) {
    if (world.on_intent_ref == LUA_NOREF) return;
    const char* kind_name = kind == lolview::net::IntentKind::Attack
                                ? "attack"
                                : (kind == lolview::net::IntentKind::Cast ? "cast" : "move");
    lua_rawgeti(state, LUA_REGISTRYINDEX, world.on_intent_ref);
    lua_pushinteger(state, player_id);
    lua_pushstring(state, kind_name);
    lua_pushinteger(state, slot);
    lua_pushinteger(state, target);
    lua_pushnumber(state, x);
    lua_pushnumber(state, z);
    if (lua_pcall(state, 6, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "[lua] on_intent error: %s\n", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

void run_tick(lua_State* state, World& world, float dt) {
    if (world.on_tick_ref == LUA_NOREF) return;
    lua_rawgeti(state, LUA_REGISTRYINDEX, world.on_tick_ref);
    lua_pushnumber(state, dt);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
        std::fprintf(stderr, "[lua] on_tick error: %s\n", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

std::uint8_t model_index(const std::string& model) {
    for (std::size_t i = 0; i < kModels.size(); ++i) {
        if (kModels[i] == model) return static_cast<std::uint8_t>(i);
    }
    return static_cast<std::uint8_t>(kModels.size() - 1);
}

void print_world(const World& world) {
    std::printf("[tick %4d] %zu units:", world.tick, world.units.size());
    for (const auto& unit : world.units) {
        std::printf(" #%u %s(%.0f,%.0f) hp %.0f/%.0f", unit.id, unit.model.c_str(), unit.x,
                    unit.z, unit.hp, unit.max_hp);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string script = "server/main.lua";
    float duration = 5.0f;
    std::uint16_t port = 27015;
    std::string paths_file = "scripts/paths.txt";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--script" && i + 1 < argc) {
            script = argv[++i];
        } else if (argument == "--duration" && i + 1 < argc) {
            duration = static_cast<float>(std::atof(argv[++i]));
        } else if (argument == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (argument == "--paths" && i + 1 < argc) {
            paths_file = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: lolview_server [--script <file>] [--duration <s>] [--port <n>]\n");
            return 2;
        }
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    World world;
    lua_State* state = luaL_newstate();
    luaL_openlibs(state);
    register_world(state, world);
    if (luaL_dofile(state, script.c_str()) != LUA_OK) {
        std::fprintf(stderr, "error: %s: %s\n", script.c_str(), lua_tostring(state, -1));
        lua_close(state);
        return 1;
    }

    if (!paths_file.empty() && world.area.load(paths_file)) {
        std::printf("server: %zu walkable polygon(s) from %s\n", world.area.polygons.size(),
                    paths_file.c_str());
    }

    lolview::net::UdpSocket socket;
    if (!socket.bind(port)) {
        std::fprintf(stderr, "error: cannot bind udp/%u\n", port);
        lua_close(state);
        return 1;
    }
    socket.set_non_blocking(true);
    std::printf("server: %s loaded (%zu units), listening on udp/%u\n", script.c_str(),
                world.units.size(), socket.local_port());

    std::vector<Client> clients;
    std::uint16_t next_player = 1;
    float since_snapshot = 0.0f;
    float since_report = 0.0f;

    const auto tick_start = std::chrono::steady_clock::now();
    long long ticks_done = 0;
    while (duration <= 0.0f || world.elapsed < duration) {
        // ---- inbound: join requests and intents ----
        while (const auto datagram = socket.recv()) {
            std::printf("server: datagram %zu bytes from %s:%u\n", datagram->data.size(),
                        datagram->from_host.c_str(), datagram->from_port);
            if (const auto hello = lolview::net::decode_hello(datagram->data)) {
                Client client;
                client.host = datagram->from_host;
                client.port = datagram->from_port;
                client.player_id = next_player++;

                Unit unit;
                unit.id = world.next_id++;
                unit.model = "champion";
                // Spread players along x so they can see each other.
                unit.x = 260.0f * (static_cast<float>(client.player_id) - 1.5f) * 2.0f;
                unit.z = 0.0f;
                unit.hp = 620.0f;
                unit.max_hp = 620.0f;
                unit.team = 0;
                world.units.push_back(unit);
                client.unit_id = unit.id;
                world.player_units[client.player_id] = unit.id;
                clients.push_back(client);

                lolview::net::Welcome welcome;
                welcome.player_id = client.player_id;
                welcome.unit_id = client.unit_id;
                welcome.tick = static_cast<std::uint32_t>(world.tick);
                welcome.models = kModels;
                socket.send_to(client.host, client.port, lolview::net::encode(welcome));
                std::printf("server: player %u joined from %s:%u (unit #%u, name '%s')\n",
                            client.player_id, client.host.c_str(), client.port, unit.id,
                            hello->name.c_str());
            } else if (const auto intent = lolview::net::decode_intent(datagram->data)) {
                for (const auto& client : clients) {
                    if (client.player_id != intent->player_id) continue;
                    if (Unit* unit = world.find(client.unit_id)) {
                        if (intent->kind == lolview::net::IntentKind::Move) {
                            if (!world.area.contains(intent->x, intent->z)) {
                                std::printf("server: player %u move rejected: outside the walkable area\n",
                                            intent->player_id);
                                break;
                            }
                            unit->x = intent->x;
                            unit->z = intent->z;
                            std::printf("server: player %u move -> (%.0f, %.0f)\n",
                                        intent->player_id, intent->x, intent->z);
                        } else {
                            // Attacks and casts are resolved by the game mode.
                            dispatch_intent(state, world, intent->player_id, intent->kind,
                                            intent->slot, intent->target, intent->x, intent->z);
                        }
                    }
                    break;
                }
            }
        }

        // ---- simulate ----
        run_tick(state, world, kTickSeconds);
        ++world.tick;
        world.elapsed += kTickSeconds;

        // ---- outbound: snapshots ----
        since_snapshot += kTickSeconds;
        if (since_snapshot >= kSnapshotSeconds && !clients.empty()) {
            since_snapshot = 0.0f;
            lolview::net::Snapshot snapshot;
            snapshot.tick = static_cast<std::uint32_t>(world.tick);
            snapshot.units.reserve(world.units.size());
            for (const auto& unit : world.units) {
                if (unit.dead) continue;
                lolview::net::UnitState state_on_wire;
                state_on_wire.id = unit.id;
                state_on_wire.model = model_index(unit.model);
                state_on_wire.x = unit.x;
                state_on_wire.y = 0.0f;
                state_on_wire.z = unit.z;
                state_on_wire.yaw = unit.yaw;
                state_on_wire.hp = unit.hp;
                state_on_wire.max_hp = unit.max_hp;
                state_on_wire.team = static_cast<std::uint8_t>(unit.team);
                snapshot.units.push_back(state_on_wire);
            }
            const auto packet = lolview::net::encode(snapshot);
            for (const auto& client : clients) socket.send_to(client.host, client.port, packet);
        }

        since_report += kTickSeconds;
        if (since_report >= 0.5f) {
            since_report = 0.0f;
            print_world(world);
        }

        // Pace the simulation to real time (30 Hz) instead of spinning.
        ++ticks_done;
        const auto deadline =
            tick_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(ticks_done * kTickSeconds));
        std::this_thread::sleep_until(deadline);
    }

    print_world(world);
    lua_close(state);
    return 0;
}
