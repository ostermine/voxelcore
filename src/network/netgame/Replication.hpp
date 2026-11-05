#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "NetGame.hpp"

class Level;

/// Player movement replication over netgame. The server reads the bound
/// players and sends every connection snapshots of the others, less often
/// for farther players; the client sends its player's movement and places
/// remote players smoothly from a buffer of snapshots (interpolation).
/// Joining, names and other game messages stay with the game scripts.
///
/// Messages on the replication lane (little-endian):
///   move, client to server:  u8 1, u32 seq, f32 x, y, z, yaw, pitch, u8 flags
///   snapshot, server to client: u8 5, u32 tick, u16 count, then count times
///     u16 pid, i32 x, y, z (1/256 block), u16 yaw (1/65536 turn),
///     i16 pitch (1/300 degree), u8 flags
/// flags: 1 - crouching.
namespace netgame::replication {
    struct Tier {
        /// players nearer than this (blocks) ...
        float distance;
        /// ... are sent every period-th tick
        int period;
    };

    struct Config {
        int lane = 1;
        int tickRate = 20;
        std::vector<Tier> tiers {
            {50, 1}, {100, 2}, {std::numeric_limits<float>::max(), 4}
        };
        /// remote players are shown this many seconds in the past
        double interpolationDelay = 0.1;
        /// ahead of the last snapshot at most this many seconds, then stop
        double maxExtrapolation = 0.25;
    };

    struct Stats {
        /// tick() calls, their total and longest duration in microseconds
        int64_t ticks = 0;
        int64_t tickUsecTotal = 0;
        int64_t tickUsecMax = 0;
        int64_t movesApplied = 0;
        int64_t records = 0;
        int64_t messages = 0;
        int64_t bytes = 0;
        int64_t snapshotsReceived = 0;
    };

    void configure(const Config& config);
    const Config& get_config();

    /// Server: movement from the connection moves the player, and the
    /// connection gets snapshots of the other bound players
    void bind_player(ConnectionId connection, int64_t pid);

    /// Client: send the movement of the local player to the server
    void bind_local(ConnectionId connection, int64_t pid);

    /// Client: snapshots of the server player move the local remote player
    void add_remote(uint32_t serverPid, int64_t localPid);
    void remove_remote(uint32_t serverPid);

    void unbind(ConnectionId connection);
    void reset();

    /// Handle a replication message; false for any other message
    bool consume(const Event& event, Level& level);

    /// Server: send snapshots. Client: send the local player's movement.
    void tick(Level& level);

    /// Client: place remote players for the current moment (every frame)
    void frame(Level& level);

    /// Counters since the previous call
    Stats take_stats();
}
