#include "NetGame.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>

#include "debug/Logger.hpp"

static debug::Logger logger("netgame");

namespace {
    struct State {
        ISteamNetworkingSockets* sockets = nullptr;
        HSteamNetPollGroup pollGroup = k_HSteamNetPollGroup_Invalid;
        std::set<HSteamNetConnection> connections;
        std::set<HSteamListenSocket> listeners;
        /// Filled by the status callback during RunCallbacks
        std::vector<netgame::Event> events;
    };

    std::unique_ptr<State> state;
}

static void on_debug_output(
    ESteamNetworkingSocketsDebugOutputType type, const char* message
) {
    if (type <= k_ESteamNetworkingSocketsDebugOutputType_Error) {
        logger.error() << message;
    } else if (type <= k_ESteamNetworkingSocketsDebugOutputType_Warning) {
        logger.warning() << message;
    } else {
        logger.info() << message;
    }
}

static netgame::Event make_event(netgame::EventType type, uint32_t conn) {
    netgame::Event event {};
    event.type = type;
    event.connection = conn;
    return event;
}

static void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    if (state == nullptr) {
        return;
    }
    auto& sockets = *state->sockets;
    const auto& connInfo = info->m_info;
    HSteamNetConnection conn = info->m_hConn;

    switch (connInfo.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (connInfo.m_hListenSocket == k_HSteamListenSocket_Invalid) {
                break;
            }
            if (sockets.AcceptConnection(conn) != k_EResultOK) {
                sockets.CloseConnection(conn, 0, "accept failed", false);
                break;
            }
            sockets.SetConnectionPollGroup(conn, state->pollGroup);
            state->connections.insert(conn);
            break;
        case k_ESteamNetworkingConnectionState_Connected: {
            char address[SteamNetworkingIPAddr::k_cchMaxString];
            connInfo.m_addrRemote.ToString(address, sizeof(address), true);
            auto event = make_event(netgame::EventType::CONNECTED, conn);
            event.listener = connInfo.m_hListenSocket;
            event.text = address;
            state->events.push_back(std::move(event));
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            auto event = make_event(netgame::EventType::DISCONNECTED, conn);
            event.listener = connInfo.m_hListenSocket;
            event.reason = connInfo.m_eEndReason;
            event.text = connInfo.m_szEndDebug;
            state->events.push_back(std::move(event));
            state->connections.erase(conn);
            sockets.CloseConnection(conn, 0, nullptr, false);
            break;
        }
        default:
            break;
    }
}

static State& require_state() {
    if (state) {
        return *state;
    }
    SteamDatagramErrMsg error;
    if (!GameNetworkingSockets_Init(nullptr, error)) {
        throw std::runtime_error(
            "could not initialize netgame transport: " + std::string(error)
        );
    }
    auto utils = SteamNetworkingUtils();
    utils->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_Warning, on_debug_output
    );
    utils->SetGlobalCallback_SteamNetConnectionStatusChanged(on_status_changed);

    state = std::make_unique<State>();
    state->sockets = SteamNetworkingSockets();
    state->pollGroup = state->sockets->CreatePollGroup();
    // registered after the library init, so it runs before the library
    // statics are destroyed
    static bool exitHandlerSet = false;
    if (!exitHandlerSet) {
        std::atexit(netgame::shutdown);
        exitHandlerSet = true;
    }
    logger.info() << "transport initialized";
    return *state;
}

netgame::ListenId netgame::listen(int port) {
    auto& s = require_state();
    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = static_cast<uint16>(port);
    HSteamListenSocket socket =
        s.sockets->CreateListenSocketIP(address, 0, nullptr);
    if (socket == k_HSteamListenSocket_Invalid) {
        throw std::runtime_error(
            "could not listen on port " + std::to_string(port)
        );
    }
    s.listeners.insert(socket);
    logger.info() << "listening on port " << port;
    return socket;
}

void netgame::close_listen(ListenId id) {
    if (state && state->listeners.erase(id)) {
        state->sockets->CloseListenSocket(id);
    }
}

netgame::ConnectionId netgame::connect(const std::string& address, int port) {
    auto& s = require_state();
    SteamNetworkingIPAddr ipAddress;
    if (!ipAddress.ParseString(
            address == "localhost" ? "127.0.0.1" : address.c_str()
        )) {
        throw std::runtime_error("invalid IP address: " + address);
    }
    ipAddress.m_port = static_cast<uint16>(port);
    HSteamNetConnection conn =
        s.sockets->ConnectByIPAddress(ipAddress, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        throw std::runtime_error("could not connect to " + address);
    }
    s.sockets->SetConnectionPollGroup(conn, s.pollGroup);
    s.connections.insert(conn);
    return conn;
}

bool netgame::send(
    ConnectionId connection, std::string_view data, const SendOptions& options
) {
    if (state == nullptr) {
        return false;
    }
    int flags = options.reliable ? k_nSteamNetworkingSend_Reliable
                                 : k_nSteamNetworkingSend_Unreliable;
    if (!options.nagle) {
        flags |= k_nSteamNetworkingSend_NoNagle;
    }
    auto message = SteamNetworkingUtils()->AllocateMessage(data.size());
    std::memcpy(message->m_pData, data.data(), data.size());
    message->m_conn = connection;
    message->m_nFlags = flags;
    message->m_idxLane = static_cast<uint16>(options.lane);

    int64 result = 0;
    // true: the library releases the message even if sending fails
    state->sockets->SendMessages(1, &message, &result, true);
    return result >= 0;
}

void netgame::configure_lanes(
    ConnectionId connection,
    const std::vector<int>& priorities,
    const std::vector<uint16_t>& weights
) {
    if (state == nullptr) {
        return;
    }
    if (!weights.empty() && weights.size() != priorities.size()) {
        throw std::runtime_error("lane priorities and weights size mismatch");
    }
    auto result = state->sockets->ConfigureConnectionLanes(
        connection,
        static_cast<int>(priorities.size()),
        priorities.data(),
        weights.empty() ? nullptr : weights.data()
    );
    if (result != k_EResultOK) {
        throw std::runtime_error(
            "could not configure lanes (EResult " + std::to_string(result) + ")"
        );
    }
}

void netgame::flush() {
    if (state == nullptr) {
        return;
    }
    for (auto conn : state->connections) {
        state->sockets->FlushMessagesOnConnection(conn);
    }
}

void netgame::close(ConnectionId connection, const std::string& reason) {
    if (state && state->connections.erase(connection)) {
        state->sockets->CloseConnection(
            connection,
            k_ESteamNetConnectionEnd_App_Generic,
            reason.c_str(),
            true
        );
    }
}

std::vector<netgame::Event> netgame::poll() {
    if (state == nullptr) {
        return {};
    }
    state->sockets->RunCallbacks();
    std::vector<Event> events;
    events.swap(state->events);

    std::vector<Event> received;
    constexpr int BATCH = 64;
    SteamNetworkingMessage_t* messages[BATCH];
    int count;
    while ((count = state->sockets->ReceiveMessagesOnPollGroup(
                state->pollGroup, messages, BATCH
            )) > 0) {
        for (int i = 0; i < count; i++) {
            auto message = messages[i];
            auto event = make_event(EventType::MESSAGE, message->m_conn);
            event.lane = message->m_idxLane;
            auto bytes = static_cast<const uint8_t*>(message->m_pData);
            event.data.assign(bytes, bytes + message->m_cbSize);
            received.push_back(std::move(event));
            message->Release();
        }
    }
    // A connection may get connected between RunCallbacks and the receive:
    // its first message would come before "connected", and a game drops
    // messages of unknown connections (lost hellos of a crowd joining at
    // once). Its callback is queued by now: "connected" goes first, other
    // events wait for the next poll, after the messages they followed
    state->sockets->RunCallbacks();
    std::vector<Event> later;
    for (auto& event : state->events) {
        if (event.type == EventType::CONNECTED) {
            events.push_back(std::move(event));
        } else {
            later.push_back(std::move(event));
        }
    }
    state->events = std::move(later);
    for (auto& event : received) {
        events.push_back(std::move(event));
    }
    return events;
}

std::optional<netgame::ConnectionStats> netgame::get_stats(
    ConnectionId connection
) {
    if (state == nullptr) {
        return std::nullopt;
    }
    SteamNetConnectionRealTimeStatus_t status;
    if (state->sockets->GetConnectionRealTimeStatus(
            connection, &status, 0, nullptr
        ) != k_EResultOK) {
        return std::nullopt;
    }
    return ConnectionStats {
        status.m_nPing,
        status.m_flConnectionQualityLocal,
        status.m_flConnectionQualityRemote,
        status.m_flOutBytesPerSec,
        status.m_flInBytesPerSec,
        status.m_nSendRateBytesPerSecond,
        status.m_cbPendingReliable,
        status.m_cbPendingUnreliable,
        status.m_usecQueueTime,
    };
}

void netgame::set_fake_network(int lagMs, float lossPercent) {
    require_state();
    auto utils = SteamNetworkingUtils();
    utils->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_FakePacketLag_Send, lagMs
    );
    utils->SetGlobalConfigValueFloat(
        k_ESteamNetworkingConfig_FakePacketLoss_Send, lossPercent
    );
}

void netgame::set_send_rate(int minRate, int maxRate) {
    auto& s = require_state();
    auto utils = SteamNetworkingUtils();
    utils->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_SendRateMin, minRate
    );
    utils->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_SendRateMax, maxRate
    );
    for (auto conn : s.connections) {
        utils->SetConnectionConfigValueInt32(
            conn, k_ESteamNetworkingConfig_SendRateMin, minRate
        );
        utils->SetConnectionConfigValueInt32(
            conn, k_ESteamNetworkingConfig_SendRateMax, maxRate
        );
    }
}

void netgame::shutdown() {
    if (state == nullptr) {
        return;
    }
    for (auto conn : state->connections) {
        state->sockets->CloseConnection(conn, 0, "shutdown", false);
    }
    for (auto socket : state->listeners) {
        state->sockets->CloseListenSocket(socket);
    }
    state->sockets->DestroyPollGroup(state->pollGroup);
    state.reset();
    GameNetworkingSockets_Kill();
}
