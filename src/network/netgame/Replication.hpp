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
///
/// Authoritative movement (both sides bound with authority): instead of
/// positions the client sends its input, a command per physics step of its
/// player, and the server repeats them on its copy of the player - the same
/// core:player with the keys, its own physics step with the same time:
///   input, client to server: u8 2, u32 seq of the first, u8 count, then
///     count times f32 dt (s), u8 keys, u8 1 - keys read (0 - the input
///     went to a screen), f32 yaw, f32 pitch
///   ack, server to the owner every tick with new commands: u8 6,
///     u32 seq of the last repeated command, f32 x, y, z, vx, vy, vz,
///     u8 flags (+ 4 - flight, 8 - noclip; the server decides them)
/// The client keeps where its player was after each command and moves it
/// by the difference from the server (prediction and reconciliation).
/// keys: 1 forward, 2 back, 4 left, 8 right, 16 jump, 32 sprint,
/// 64 crouch, 128 cheat.
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

        /// server: commands are repeated no faster than real time (a speed
        /// hack waits); this much time may be saved up for late commands
        double inputBudget = 0.5;
        /// server: the cheat key (10 times faster) is ignored unless allowed
        bool allowCheat = false;
        /// client: a difference from the server shorter than this (blocks)
        /// is left as it is ...
        float correctionMin = 0.01f;
        /// ... a longer one fades (halves every this many seconds) ...
        double correctionSmoothing = 0.05;
        /// ... and a difference longer than this (blocks) is fixed at once
        float correctionSnap = 2.0f;
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

        /// authoritative movement, server: commands received, repeated,
        /// dropped (a full queue, repeated or old), acks sent
        int64_t commandsReceived = 0;
        int64_t commandsApplied = 0;
        int64_t commandsDropped = 0;
        int64_t acks = 0;
        /// client: commands made, differences from the server fixed (of
        /// them at once), the longest difference (blocks)
        int64_t commandsRecorded = 0;
        int64_t corrections = 0;
        int64_t snaps = 0;
        float correctionMax = 0.0f;
    };

    void configure(const Config& config);
    const Config& get_config();

    /// Server: movement from the connection moves the player, and the
    /// connection gets snapshots of the other bound players. With authority
    /// the connection sends commands, and the server moves the player
    void bind_player(ConnectionId connection, int64_t pid, bool authority = false);

    /// Client: send the movement of the local player to the server, with
    /// authority - its commands (record_input), fixing the player by acks
    void bind_local(ConnectionId connection, int64_t pid, bool authority = false);

    /// Server: the player is moved by its client's commands (out of the
    /// common physics step)
    bool is_driven(int64_t pid);

    /// Keys core:player applies to the player instead of the keyboard: the
    /// command being repeated (server) or set by set_input (bots, tests).
    /// -1 - none
    int get_input(int64_t pid);
    /// keys < 0 - none
    void set_input(int64_t pid, int keys);

    /// Client: core:player applied keys (< 0 - none, the input went to a
    /// screen) to the player for a step of delta seconds
    void record_input(Level& level, int64_t pid, int keys, float delta);

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

    constexpr uint8_t KEY_CHEAT = 128;
    constexpr size_t COMMAND_SIZE = 14;
    /// commands per input message: 6 + 64 * 14 bytes fits one UDP packet
    constexpr size_t MAX_COMMANDS = 64;

    struct Command {
        uint32_t seq;
        /// seconds of the physics step
        float dt;
        uint8_t keys;
        /// false - the input went to a screen, no keys applied
        bool hasKeys;
        float yaw;
        float pitch;
    };

    /// An input message of commands with consecutive seqs (at most
    /// MAX_COMMANDS, the last ones)
    void write_input(std::string& out, const std::deque<Command>& commands);

    /// false if the message is broken
    bool read_input(const std::vector<uint8_t>& data, std::vector<Command>& commands);
}
