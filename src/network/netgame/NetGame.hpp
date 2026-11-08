#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Game transport: message-oriented reliable/unreliable connections with
/// lanes, encryption and simulated lag/loss. Backed by GameNetworkingSockets,
/// built only with VOXELENGINE_NETGAME. Not thread-safe: main thread only.
namespace netgame {
    using ConnectionId = uint32_t;
    using ListenId = uint32_t;

    enum class EventType {
        CONNECTED,
        DISCONNECTED,
        MESSAGE,
    };

    struct Event {
        EventType type;
        ConnectionId connection;
        /// Listen socket that accepted the connection, 0 for outgoing ones
        ListenId listener = 0;
        int lane = 0;
        /// Connection end reason code (DISCONNECTED only)
        int reason = 0;
        /// Remote address for CONNECTED, debug message for DISCONNECTED
        std::string text;
        std::vector<uint8_t> data;
    };

    struct SendOptions {
        bool reliable = true;
        /// false sends immediately instead of waiting for more messages
        bool nagle = true;
        int lane = 0;
    };

    struct ConnectionStats {
        int ping;
        float qualityLocal;
        float qualityRemote;
        float outBytesPerSec;
        float inBytesPerSec;
        int sendRate;
        int pendingReliable;
        int pendingUnreliable;
        int64_t queueTimeUsec;
    };

    /// Start accepting connections on all interfaces.
    /// Incoming connections are accepted automatically.
    ListenId listen(int port);
    void close_listen(ListenId id);

    /// Connect to the IP address (IPv4/IPv6 literal or "localhost").
    /// Completion is reported by the CONNECTED event.
    ConnectionId connect(const std::string& address, int port);

    /// @return false if the message was not queued (connection is closed,
    /// invalid lane, etc.)
    bool send(
        ConnectionId connection,
        std::string_view data,
        const SendOptions& options
    );

    /// Lanes are numbered from 0, lower priority value goes first,
    /// weights share bandwidth between lanes of the same priority.
    void configure_lanes(
        ConnectionId connection,
        const std::vector<int>& priorities,
        const std::vector<uint16_t>& weights
    );

    /// Send queued (Nagle-delayed) messages now. Flushes all connections.
    void flush();

    /// Close gracefully: queued reliable messages are still delivered.
    void close(ConnectionId connection, const std::string& reason);

    /// Run transport callbacks and collect connection events and messages
    /// received since the previous call.
    std::vector<Event> poll();

    std::optional<ConnectionStats> get_stats(ConnectionId connection);

    /// Simulate lag (milliseconds) and packet loss (percent) for sent packets
    void set_fake_network(int lagMs, float lossPercent);

    /// Bytes per second; applied to existing and new connections
    void set_send_rate(int minRate, int maxRate);

    /// Close everything and release the transport. Called on exit.
    void shutdown();
}
