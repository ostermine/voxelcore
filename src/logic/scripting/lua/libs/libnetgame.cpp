#include "api_lua.hpp"

#include <algorithm>

#include "devtools/Project.hpp"
#include "engine/Engine.hpp"
#include "logic/scripting/scripting.hpp"
#include "network/netgame/NetGame.hpp"
#include "network/netgame/Replication.hpp"

using namespace scripting;

static void require_permission() {
    if (!engine->getProject().permissions.has(Permissions::NETWORK)) {
        throw std::runtime_error(
            "network subsystem is not available in the project"
        );
    }
}

static int l_listen(lua::State* L) {
    require_permission();
    return lua::pushinteger(L, netgame::listen(lua::tointeger(L, 1)));
}

static int l_close_listen(lua::State* L) {
    netgame::close_listen(lua::tointeger(L, 1));
    return 0;
}

static int l_connect(lua::State* L) {
    require_permission();
    auto address = lua::require_string(L, 1);
    int port = lua::tointeger(L, 2);
    return lua::pushinteger(L, netgame::connect(address, port));
}

static int l_send(lua::State* L) {
    auto connection = static_cast<netgame::ConnectionId>(lua::tointeger(L, 1));
    // the helper expects the value on the stack top
    lua::pushvalue(L, 2);
    std::string data(lua::bytearray_as_string(L, -1));
    lua::pop(L);

    netgame::SendOptions options {};
    if (lua::istable(L, 3)) {
        if (lua::getfield(L, "reliable", 3)) {
            options.reliable = lua::toboolean(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "nagle", 3)) {
            options.nagle = lua::toboolean(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "lane", 3)) {
            options.lane = lua::tointeger(L, -1);
            lua::pop(L);
        }
    }
    return lua::pushboolean(L, netgame::send(connection, data, options));
}

static std::vector<int> read_ints(lua::State* L, int idx) {
    std::vector<int> values;
    if (!lua::istable(L, idx)) {
        return values;
    }
    int len = lua::objlen(L, idx);
    for (int i = 1; i <= len; i++) {
        lua::rawgeti(L, i, idx);
        values.push_back(lua::tointeger(L, -1));
        lua::pop(L);
    }
    return values;
}

static int l_configure_lanes(lua::State* L) {
    auto connection = static_cast<netgame::ConnectionId>(lua::tointeger(L, 1));
    auto priorities = read_ints(L, 2);
    std::vector<uint16_t> weights;
    for (int weight : read_ints(L, 3)) {
        weights.push_back(static_cast<uint16_t>(weight));
    }
    netgame::configure_lanes(connection, priorities, weights);
    return 0;
}

static int l_flush(lua::State* L) {
    netgame::flush();
    return 0;
}

static int l_close(lua::State* L) {
    std::string reason;
    if (lua::isstring(L, 2)) {
        reason = lua::tostring(L, 2);
    }
    netgame::close(lua::tointeger(L, 1), reason);
    return 0;
}

static int l_poll(lua::State* L) {
    auto events = netgame::poll();
    // replication messages are handled here and do not reach scripts
    if (level) {
        events.erase(
            std::remove_if(
                events.begin(),
                events.end(),
                [](const netgame::Event& event) {
                    if (event.type == netgame::EventType::DISCONNECTED) {
                        netgame::replication::unbind(event.connection);
                    }
                    return netgame::replication::consume(event, *level);
                }
            ),
            events.end()
        );
    }
    lua::createtable(L, events.size(), 0);
    for (size_t i = 0; i < events.size(); i++) {
        const auto& event = events[i];
        lua::createtable(L, 0, 5);
        lua::pushinteger(L, event.connection);
        lua::setfield(L, "conn");
        switch (event.type) {
            case netgame::EventType::CONNECTED:
                lua::pushstring(L, "connected");
                lua::setfield(L, "type");
                lua::pushinteger(L, event.listener);
                lua::setfield(L, "listener");
                lua::pushstring(L, event.text);
                lua::setfield(L, "address");
                break;
            case netgame::EventType::DISCONNECTED:
                lua::pushstring(L, "disconnected");
                lua::setfield(L, "type");
                lua::pushinteger(L, event.listener);
                lua::setfield(L, "listener");
                lua::pushinteger(L, event.reason);
                lua::setfield(L, "reason");
                lua::pushstring(L, event.text);
                lua::setfield(L, "message");
                break;
            case netgame::EventType::MESSAGE:
                lua::pushstring(L, "message");
                lua::setfield(L, "type");
                lua::pushinteger(L, event.lane);
                lua::setfield(L, "lane");
                lua::create_bytearray(L, event.data.data(), event.data.size());
                lua::setfield(L, "data");
                break;
        }
        lua::rawseti(L, i + 1);
    }
    return 1;
}

static int l_get_stats(lua::State* L) {
    auto stats = netgame::get_stats(lua::tointeger(L, 1));
    if (!stats) {
        return 0;
    }
    lua::createtable(L, 0, 9);
    lua::pushinteger(L, stats->ping);
    lua::setfield(L, "ping");
    lua::pushnumber(L, stats->qualityLocal);
    lua::setfield(L, "quality_local");
    lua::pushnumber(L, stats->qualityRemote);
    lua::setfield(L, "quality_remote");
    lua::pushnumber(L, stats->outBytesPerSec);
    lua::setfield(L, "out_bytes_per_sec");
    lua::pushnumber(L, stats->inBytesPerSec);
    lua::setfield(L, "in_bytes_per_sec");
    lua::pushinteger(L, stats->sendRate);
    lua::setfield(L, "send_rate");
    lua::pushinteger(L, stats->pendingReliable);
    lua::setfield(L, "pending_reliable");
    lua::pushinteger(L, stats->pendingUnreliable);
    lua::setfield(L, "pending_unreliable");
    lua::pushinteger(L, stats->queueTimeUsec);
    lua::setfield(L, "queue_time_us");
    return 1;
}

static int l_set_fake_network(lua::State* L) {
    netgame::set_fake_network(lua::tointeger(L, 1), lua::tonumber(L, 2));
    return 0;
}

static int l_set_send_rate(lua::State* L) {
    netgame::set_send_rate(lua::tointeger(L, 1), lua::tointeger(L, 2));
    return 0;
}

static int l_shutdown(lua::State* L) {
    netgame::replication::reset();
    netgame::shutdown();
    return 0;
}

static Level& require_level() {
    if (level == nullptr) {
        throw std::runtime_error("level is not initialized");
    }
    return *level;
}

static int l_replicate_player(lua::State* L) {
    netgame::replication::bind_player(lua::tointeger(L, 1), lua::tointeger(L, 2));
    return 0;
}

static int l_replicate_local(lua::State* L) {
    netgame::replication::bind_local(lua::tointeger(L, 1), lua::tointeger(L, 2));
    return 0;
}

static int l_unreplicate(lua::State* L) {
    netgame::replication::unbind(lua::tointeger(L, 1));
    return 0;
}

static int l_add_remote(lua::State* L) {
    netgame::replication::add_remote(lua::tointeger(L, 1), lua::tointeger(L, 2));
    return 0;
}

static int l_remove_remote(lua::State* L) {
    netgame::replication::remove_remote(lua::tointeger(L, 1));
    return 0;
}

static int l_replication_tick(lua::State* L) {
    netgame::replication::tick(require_level());
    return 0;
}

static int l_replication_frame(lua::State* L) {
    netgame::replication::frame(require_level());
    return 0;
}

static int l_replication_stats(lua::State* L) {
    auto stats = netgame::replication::take_stats();
    lua::createtable(L, 0, 8);
    lua::pushinteger(L, stats.ticks);
    lua::setfield(L, "ticks");
    lua::pushnumber(L, stats.ticks ? stats.tickUsecTotal / 1000.0 / stats.ticks : 0.0);
    lua::setfield(L, "tick_avg_ms");
    lua::pushnumber(L, stats.tickUsecMax / 1000.0);
    lua::setfield(L, "tick_max_ms");
    lua::pushinteger(L, stats.movesApplied);
    lua::setfield(L, "moves");
    lua::pushinteger(L, stats.records);
    lua::setfield(L, "records");
    lua::pushinteger(L, stats.messages);
    lua::setfield(L, "messages");
    lua::pushinteger(L, stats.bytes);
    lua::setfield(L, "bytes");
    lua::pushinteger(L, stats.snapshotsReceived);
    lua::setfield(L, "snapshots");
    return 1;
}

static int l_configure_replication(lua::State* L) {
    auto config = netgame::replication::get_config();
    if (lua::istable(L, 1)) {
        if (lua::getfield(L, "lane", 1)) {
            config.lane = lua::tointeger(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "delay", 1)) {
            config.interpolationDelay = lua::tonumber(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "max_extrapolation", 1)) {
            config.maxExtrapolation = lua::tonumber(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "smoothing", 1)) {
            config.smoothing = lua::tonumber(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "snap_distance", 1)) {
            config.snapDistance = lua::tonumber(L, -1);
            lua::pop(L);
        }
        if (lua::getfield(L, "tiers", 1)) {
            config.tiers.clear();
            int count = lua::objlen(L, -1);
            for (int i = 1; i <= count; i++) {
                lua::rawgeti(L, i);
                lua::rawgeti(L, 1);
                float distance = lua::tonumber(L, -1);
                lua::pop(L);
                lua::rawgeti(L, 2);
                int period = lua::tointeger(L, -1);
                lua::pop(L);
                lua::pop(L);
                config.tiers.push_back({distance, period});
            }
            lua::pop(L);
        }
    }
    netgame::replication::configure(config);
    return 0;
}

const luaL_Reg netgamelib[] = {
    {"listen", lua::wrap<l_listen>},
    {"close_listen", lua::wrap<l_close_listen>},
    {"connect", lua::wrap<l_connect>},
    {"send", lua::wrap<l_send>},
    {"configure_lanes", lua::wrap<l_configure_lanes>},
    {"flush", lua::wrap<l_flush>},
    {"close", lua::wrap<l_close>},
    {"poll", lua::wrap<l_poll>},
    {"get_stats", lua::wrap<l_get_stats>},
    {"set_fake_network", lua::wrap<l_set_fake_network>},
    {"set_send_rate", lua::wrap<l_set_send_rate>},
    {"shutdown", lua::wrap<l_shutdown>},
    {"replicate_player", lua::wrap<l_replicate_player>},
    {"replicate_local", lua::wrap<l_replicate_local>},
    {"unreplicate", lua::wrap<l_unreplicate>},
    {"add_remote", lua::wrap<l_add_remote>},
    {"remove_remote", lua::wrap<l_remove_remote>},
    {"replication_tick", lua::wrap<l_replication_tick>},
    {"replication_frame", lua::wrap<l_replication_frame>},
    {"replication_stats", lua::wrap<l_replication_stats>},
    {"configure_replication", lua::wrap<l_configure_replication>},
    {nullptr, nullptr}
};
