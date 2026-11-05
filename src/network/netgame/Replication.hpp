#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "NetGame.hpp"

class Level;

/// Player movement replication over netgame. The server reads the bound
/// players and sends every connection snapshots of the others, less often
/// for farther players; the client sends its player's movement and places
/// remote players smoothly from a buffer of snapshots (interpolation).
/// Remote players are out of physics (static bodies), their velocity and
/// the ground flag come from the snapshots for the animations.
/// Joining, names and other game messages stay with the game scripts.
///
/// Messages on the replication lane (little-endian):
///   move, client to server:  u8 1, u32 seq, f32 x, y, z, yaw, pitch, u8 flags
///   snapshot, server to client: u8 5, u32 server time (ms), u16 count, then
///     count times
///     u16 pid, i32 x, y, z (1/256 block), u16 yaw (1/65536 turn),
///     i16 pitch (1/300 degree), u8 flags
/// flags: 1 - crouching, 2 - on the ground.
namespace netgame::replication {
    struct Tier {
        /// players nearer than this (blocks) ...
        float distance;
        /// ... are sent every period-th tick
        int period;
    };

    struct Config {
        int lane = 1;
        std::vector<Tier> tiers {
            {50, 1}, {100, 2}, {std::numeric_limits<float>::max(), 4}
        };
        /// remote players are shown this many seconds in the past
        double interpolationDelay = 0.1;
        /// ahead of the last snapshot at most this many seconds, then stop
        double maxExtrapolation = 0.25;
        /// a remote player moved by late snapshots (a pause, lost packets)
        /// glides to the new place: the rest of the jump halves every this
        /// many seconds ...
        double smoothing = 0.08;
        /// ... if the jump is longer than this (blocks): shorter ones are
        /// not seen, and gliding them would only keep the player off its path
        float glideMin = 0.25f;
        /// ... and a jump longer than this (blocks) is a teleport
        float snapDistance = 8.0f;
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

/// Encoding and interpolation, open for tests
namespace netgame::replication::detail {
    constexpr uint8_t FLAG_CROUCH = 1;
    constexpr uint8_t FLAG_GROUNDED = 2;
    constexpr size_t RECORD_SIZE = 19;

    struct Record {
        uint16_t pid;
        glm::vec3 position;
        /// degrees
        float yaw;
        float pitch;
        uint8_t flags;
    };

    void write_record(std::string& out, const Record& record);

    /// false if the data ends before the record
    bool read_record(
        const std::vector<uint8_t>& data, size_t offset, Record& record
    );

    struct Sample {
        /// server time, seconds
        double time;
        glm::vec3 position;
        float yaw;
        float pitch;
        uint8_t flags;
    };

    struct Pose {
        glm::vec3 position;
        /// blocks per second
        glm::vec3 velocity;
        float yaw;
        float pitch;
        uint8_t flags;
    };

    /// The pose at the time: between two samples - interpolated, past the
    /// last one - extrapolated at most maxExtrapolation seconds, then still.
    /// samples are not empty and go in time order
    Pose sample_at(
        const std::deque<Sample>& samples, double time, double maxExtrapolation
    );

    /// What is left of a jump after dt seconds
    glm::vec3 fade(const glm::vec3& jump, double dt, double halfLife);
}
