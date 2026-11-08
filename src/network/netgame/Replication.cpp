#include "Replication.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "debug/Logger.hpp"
#include "logic/scripting/scripting.hpp"
#include "objects/Entities.hpp"
#include "objects/Entity.hpp"
#include "objects/Player.hpp"
#include "objects/Players.hpp"
#include "physics/PhysicsSolver.hpp"
#include "world/Level.hpp"

using namespace netgame;
using namespace netgame::replication;
using namespace netgame::replication::detail;

static debug::Logger logger("netgame-replication");

namespace {
    constexpr uint8_t T_MOVE = 1;
    constexpr uint8_t T_INPUT = 2;
    constexpr uint8_t T_SNAPSHOT = 5;
    constexpr uint8_t T_ACK = 6;
    constexpr uint8_t FLAG_FLIGHT = 4;
    constexpr uint8_t FLAG_NOCLIP = 8;
    /// 7 + 60 * 19 bytes fits one UDP packet
    constexpr int MAX_RECORDS = 60;
    constexpr size_t MAX_SAMPLES = 32;
    /// server: commands waiting per player (a second at 256 FPS)
    constexpr size_t MAX_QUEUE = 256;
    /// client: positions after its commands kept for the acks
    constexpr size_t MAX_HISTORY = 1024;
    /// a command of a longer step is broken
    constexpr float MAX_COMMAND_DT = 0.25f;

    Config config;
    Stats stats;

    // server
    struct ServerPeer {
        int64_t pid;
        /// subject pid -> version last sent to this peer
        std::unordered_map<int64_t, uint32_t> sent;
    };
    struct Subject {
        uint32_t lastSeq = 0;
        /// grows with every applied movement
        uint32_t version = 0;
        /// from the client until the server moves players itself
        uint8_t flags = 0;

        // authoritative movement
        bool authority = false;
        std::deque<Command> queue;
        /// seconds of commands that may be repeated now
        double budget = 0.2;
        double budgetTime = -1.0;
        /// the last repeated command, 0 - none yet
        uint32_t lastCommand = 0;
        bool ackDue = false;
    };
    std::unordered_map<ConnectionId, ServerPeer> peers;
    std::unordered_map<int64_t, Subject> subjects;
    uint32_t serverTick = 0;
    /// snapshots carry real time, not ticks: a server falling behind its
    /// tick rate would make remote players jump
    double serverStart = -1.0;

    // client
    struct Remote {
        int64_t localPid;
        std::deque<Sample> samples;
        /// shown minus computed position: what is left of a jump
        glm::vec3 jump {};
        bool shown = false;
        /// the shown position before a snapshot is read
        bool anchored = false;
        glm::vec3 anchor {};
    };
    ConnectionId localConnection = 0;
    int64_t localPid = -1;
    uint32_t moveSeq = 0;
    std::unordered_map<uint32_t, Remote> remotes;
    bool clockSet = false;
    /// server time minus local time, seconds
    double serverOffset = 0.0;
    double lastFrame = -1.0;

    // client, authoritative movement
    struct Predicted {
        uint32_t seq;
        /// where the player was after the command
        glm::vec3 position;
    };
    bool localAuthority = false;
    uint32_t inputSeq = 0;
    uint32_t lastAck = 0;
    /// sent with every input message until acked
    std::deque<Command> unacked;
    std::deque<Predicted> history;
    /// the player is still to move by this (blocks)
    glm::vec3 correction {};

    // keys for core:player instead of the keyboard
    int64_t drivingPid = -1;
    int drivingKeys = -1;
    std::unordered_map<int64_t, int> presetInputs;

    double now_seconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    void put_u8(std::string& out, uint8_t value) {
        out.push_back(static_cast<char>(value));
    }

    void put_u16(std::string& out, uint16_t value) {
        put_u8(out, value & 0xFF);
        put_u8(out, value >> 8);
    }

    void put_u32(std::string& out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            put_u8(out, (value >> (i * 8)) & 0xFF);
        }
    }

    void put_f32(std::string& out, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        put_u32(out, bits);
    }

    struct Reader {
        const std::vector<uint8_t>& data;
        size_t pos = 0;
        bool ok = true;

        uint8_t u8() {
            if (pos >= data.size()) {
                ok = false;
                return 0;
            }
            return data[pos++];
        }
        uint16_t u16() {
            uint16_t low = u8();
            return low | static_cast<uint16_t>(u8() << 8);
        }
        uint32_t u32() {
            uint32_t value = 0;
            for (int i = 0; i < 4; i++) {
                value |= static_cast<uint32_t>(u8()) << (i * 8);
            }
            return value;
        }
        float f32() {
            uint32_t bits = u32();
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    };

    Entity* entity_of(Level& level, const Player& player) {
        return level.entities->get(player.getEntity());
    }

    uint8_t flags_of(Level& level, const Player& player) {
        auto entity = entity_of(level, player);
        if (entity == nullptr) {
            return 0;
        }
        const auto& hitbox = entity->getRigidbody().hitbox;
        return (hitbox.crouching ? FLAG_CROUCH : 0) |
               (hitbox.grounded ? FLAG_GROUNDED : 0);
    }

    void set_rotation(Player& player, float yaw, float pitch) {
        auto rotation = player.getRotation();
        player.setRotation({yaw, pitch, rotation.z});
    }

    void place_moved(
        Level& level,
        Player& player,
        const glm::vec3& position,
        float yaw,
        float pitch,
        uint8_t flags
    ) {
        player.teleport(position);
        set_rotation(player, yaw, pitch);
        if (auto entity = entity_of(level, player)) {
            auto& hitbox = entity->getRigidbody().hitbox;
            hitbox.crouching = flags & FLAG_CROUCH;
            hitbox.velocity = {};
        }
    }

    void place_remote(Level& level, Player& player, const Pose& pose) {
        player.teleport(pose.position);
        set_rotation(player, pose.yaw, pose.pitch);
        if (auto entity = entity_of(level, player)) {
            auto& hitbox = entity->getRigidbody().hitbox;
            // out of the next physics step, so that the velocity and the
            // ground stay for the animations (core:player sets the body type
            // back after the step, this runs later in the frame)
            hitbox.type = BodyType::STATIC;
            hitbox.velocity = pose.velocity;
            hitbox.grounded = pose.flags & FLAG_GROUNDED;
            hitbox.crouching = pose.flags & FLAG_CROUCH;
        }
    }

    /// One physics step of the body alone, as Entities::updatePhysics does
    /// it for all of them
    void step_entity(Level& level, Entity& entity, Player& player, float dt) {
        auto& rigidbody = entity.getRigidbody();
        auto& hitbox = rigidbody.hitbox;
        auto& transform = entity.getTransform();
        if (!rigidbody.enabled) {
            return;
        }
        hitbox.type = player.isNoclip() ? BodyType::KINEMATIC : BodyType::DYNAMIC;
        hitbox.scale = transform.size;
        hitbox.selectable = rigidbody.selectable;
        hitbox.mass = hitbox.type == BodyType::DYNAMIC
                          ? rigidbody.mass
                          : std::numeric_limits<float>::infinity();
        hitbox.elasticity = rigidbody.elasticity;
        auto substeps = static_cast<unsigned>(
            std::max(std::min(static_cast<int>(dt * 1000), 200), 8)
        );
        level.physics->stepOne(hitbox, dt, substeps);
        if (!std::isfinite(hitbox.position.x) || !std::isfinite(hitbox.position.y) ||
            !std::isfinite(hitbox.position.z)) {
            hitbox.position = transform.pos;
        } else {
            transform.setPos(hitbox.position);
        }
        if (hitbox.grounded && !hitbox.prevGrounded) {
            scripting::on_entity_grounded(
                entity, glm::length(hitbox.prevVelocity - hitbox.velocity)
            );
        }
        if (!hitbox.grounded && hitbox.prevGrounded) {
            scripting::on_entity_fall(entity);
        }
    }

    /// Server: the command as the client made it - a physics step of its
    /// time, then core:player with its keys
    void run_command(Level& level, Player& player, Entity& entity, const Command& cmd) {
        auto rotation = player.getRotation();
        player.setRotation({cmd.yaw, cmd.pitch, rotation.z});
        step_entity(level, entity, player, cmd.dt);
        drivingPid = player.getId();
        drivingKeys = -1;
        if (cmd.hasKeys) {
            drivingKeys = config.allowCheat ? cmd.keys : (cmd.keys & ~KEY_CHEAT);
        }
        scripting::on_entity_physics_update(entity, cmd.dt);
        drivingPid = -1;
        drivingKeys = -1;
    }

    uint8_t ack_flags_of(Level& level, const Player& player) {
        return flags_of(level, player) | (player.isFlight() ? FLAG_FLIGHT : 0) |
               (player.isNoclip() ? FLAG_NOCLIP : 0);
    }

    /// Server: repeat the commands that came, out of the common physics step
    void drive(Level& level, ConnectionId conn, int64_t pid, Subject& subject) {
        auto player = level.players->get(pid);
        auto entity = player ? entity_of(level, *player) : nullptr;
        if (entity == nullptr) {
            return;
        }
        double now = now_seconds();
        if (subject.budgetTime >= 0) {
            subject.budget = std::min(
                config.inputBudget, subject.budget + (now - subject.budgetTime)
            );
        }
        subject.budgetTime = now;
        int applied = 0;
        while (!subject.queue.empty() &&
               subject.queue.front().dt <= subject.budget + 1e-6) {
            Command cmd = subject.queue.front();
            subject.queue.pop_front();
            subject.budget -= cmd.dt;
            run_command(level, *player, *entity, cmd);
            subject.lastCommand = cmd.seq;
            applied++;
        }
        auto& hitbox = entity->getRigidbody().hitbox;
        hitbox.type = BodyType::STATIC;
        if (applied == 0) {
            return;
        }
        subject.version++;
        stats.commandsApplied += applied;

        std::string message;
        put_u8(message, T_ACK);
        put_u32(message, subject.lastCommand);
        put_f32(message, hitbox.position.x);
        put_f32(message, hitbox.position.y);
        put_f32(message, hitbox.position.z);
        put_f32(message, hitbox.velocity.x);
        put_f32(message, hitbox.velocity.y);
        put_f32(message, hitbox.velocity.z);
        put_u8(message, ack_flags_of(level, *player));
        if (netgame::send(conn, message, {false, true, config.lane})) {
            stats.acks++;
        }
    }

    /// Server: commands from the client into the queue of its player
    void queue_input(Subject& subject, const std::vector<uint8_t>& data) {
        std::vector<Command> commands;
        if (!read_input(data, commands)) {
            return;
        }
        uint32_t last = subject.queue.empty() ? subject.lastCommand
                                              : subject.queue.back().seq;
        for (const auto& cmd : commands) {
            if (cmd.seq <= last) {
                // sent again until acked
                continue;
            }
            stats.commandsReceived++;
            if (subject.queue.size() >= MAX_QUEUE ||
                !(cmd.dt > 0.0f && cmd.dt <= MAX_COMMAND_DT) ||
                !std::isfinite(cmd.yaw) || !std::isfinite(cmd.pitch)) {
                stats.commandsDropped++;
                continue;
            }
            subject.queue.push_back(cmd);
            last = cmd.seq;
        }
    }

    /// Client: where the server has the player after a command
    void read_ack(Level& level, const std::vector<uint8_t>& data) {
        Reader reader {data};
        reader.u8();
        uint32_t seq = reader.u32();
        glm::vec3 position, velocity;
        position.x = reader.f32();
        position.y = reader.f32();
        position.z = reader.f32();
        velocity.x = reader.f32();
        velocity.y = reader.f32();
        velocity.z = reader.f32();
        uint8_t flags = reader.u8();
        if (!reader.ok || seq <= lastAck || seq > inputSeq) {
            return;
        }
        lastAck = seq;
        while (!unacked.empty() && unacked.front().seq <= seq) {
            unacked.pop_front();
        }
        while (!history.empty() && history.front().seq < seq) {
            history.pop_front();
        }
        auto player = level.players->get(localPid);
        if (player == nullptr) {
            return;
        }
        // the server decides
        player->setFlight(flags & FLAG_FLIGHT);
        player->setNoclip(flags & FLAG_NOCLIP);
        if (history.empty() || history.front().seq != seq) {
            return;
        }
        glm::vec3 error = position - history.front().position;
        history.pop_front();
        float length = glm::length(error);
        if (!(length > config.correctionMin)) {
            return;
        }
        // the later commands were made from the wrong place too
        for (auto& predicted : history) {
            predicted.position += error;
        }
        stats.corrections++;
        stats.correctionMax = std::max(stats.correctionMax, length);
        logger.info() << "correction " << length << " at command " << seq
                      << ": server " << position.x << " " << position.y << " "
                      << position.z << ", error " << error.x << " " << error.y
                      << " " << error.z << ", flags " << int(flags);
        auto entity = entity_of(level, *player);
        if (length > config.correctionSnap && entity) {
            auto& hitbox = entity->getRigidbody().hitbox;
            player->teleport(hitbox.position + correction + error);
            hitbox.velocity = velocity;
            correction = {};
            stats.snaps++;
        } else {
            correction += error;
        }
    }

    int period_for(float distance) {
        for (const auto& tier : config.tiers) {
            if (distance < tier.distance) {
                return std::max(1, tier.period);
            }
        }
        return config.tiers.empty() ? 1 : std::max(1, config.tiers.back().period);
    }

    float lerp_angle(float a, float b, float t) {
        float delta = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
        return a + delta * t;
    }

    /// A server catching up after a pause runs ticks a millisecond apart,
    /// and a movement between them would give a speed of hundreds of blocks
    /// per second: the velocity is taken over at least this many seconds
    constexpr double MIN_SPAN = 0.04;

    glm::vec3 velocity_to(const std::deque<Sample>& samples, size_t hi) {
        size_t lo = hi - 1;
        while (lo > 0 && samples[hi].time - samples[lo].time < MIN_SPAN) {
            lo--;
        }
        double span = std::max(samples[hi].time - samples[lo].time, MIN_SPAN);
        return (samples[hi].position - samples[lo].position) /
               static_cast<float>(span);
    }

    void apply_move(Level& level, int64_t pid, const std::vector<uint8_t>& data) {
        Reader reader {data};
        reader.u8();
        uint32_t seq = reader.u32();
        glm::vec3 position;
        position.x = reader.f32();
        position.y = reader.f32();
        position.z = reader.f32();
        float yaw = reader.f32();
        float pitch = reader.f32();
        uint8_t flags = reader.u8();
        if (!reader.ok || !std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            return;
        }
        auto& subject = subjects[pid];
        if (subject.version && seq <= subject.lastSeq) {
            return;
        }
        auto player = level.players->get(pid);
        if (player == nullptr) {
            return;
        }
        subject.lastSeq = seq;
        subject.version++;
        subject.flags = flags;
        place_moved(level, *player, position, yaw, pitch, flags);
        stats.movesApplied++;
    }

    double render_time(double now) {
        return now + serverOffset - config.interpolationDelay;
    }

    void read_snapshot(const std::vector<uint8_t>& data) {
        Reader reader {data};
        reader.u8();
        uint32_t timeMs = reader.u32();
        uint16_t count = reader.u16();
        if (!reader.ok) {
            return;
        }
        double now = now_seconds();
        // where the remote players are shown, to glide from there if the new
        // samples or the clock move them
        if (clockSet) {
            double before = render_time(now);
            for (auto& [_, remote] : remotes) {
                remote.anchored = remote.shown && !remote.samples.empty();
                if (remote.anchored) {
                    remote.anchor = sample_at(
                        remote.samples, before, config.maxExtrapolation
                    ).position;
                }
            }
        }
        double serverTime = timeMs / 1000.0;
        double offset = serverTime - now;
        if (!clockSet || std::abs(offset - serverOffset) > 0.5) {
            serverOffset = offset;
            clockSet = true;
        } else {
            serverOffset += (offset - serverOffset) * 0.05;
        }
        size_t pos = reader.pos;
        for (int i = 0; i < count; i++, pos += RECORD_SIZE) {
            Record record;
            if (!read_record(data, pos, record)) {
                break;
            }
            auto found = remotes.find(record.pid);
            if (found == remotes.end()) {
                continue;
            }
            auto& samples = found->second.samples;
            if (!samples.empty() && serverTime <= samples.back().time) {
                continue;
            }
            samples.push_back(Sample {
                serverTime, record.position, record.yaw, record.pitch, record.flags
            });
            if (samples.size() > MAX_SAMPLES) {
                samples.pop_front();
            }
        }
        double after = render_time(now);
        for (auto& [_, remote] : remotes) {
            if (!remote.anchored) {
                continue;
            }
            remote.anchored = false;
            glm::vec3 jump = remote.anchor - sample_at(
                remote.samples, after, config.maxExtrapolation
            ).position;
            if (glm::length(jump) < config.glideMin) {
                continue;
            }
            remote.jump += jump;
            if (glm::length(remote.jump) > config.snapDistance) {
                remote.jump = {};
            }
        }
        stats.snapshotsReceived++;
    }

    void server_tick(Level& level) {
        serverTick++;
        if (serverStart < 0) {
            serverStart = now_seconds();
        }
        auto timeMs = static_cast<uint32_t>((now_seconds() - serverStart) * 1000.0);
        struct State {
            int64_t pid;
            glm::vec3 position;
            uint32_t version;
            std::string record;
        };
        for (auto& [conn, peer] : peers) {
            auto subject = subjects.find(peer.pid);
            if (subject != subjects.end() && subject->second.authority) {
                drive(level, conn, peer.pid, subject->second);
            }
        }
        std::vector<State> states;
        for (const auto& [_, peer] : peers) {
            auto subject = subjects.find(peer.pid);
            auto player = level.players->get(peer.pid);
            if (player == nullptr || subject == subjects.end() ||
                subject->second.version == 0) {
                continue;
            }
            glm::vec3 position = player->getPosition();
            uint8_t flags = subject->second.flags;
            if (subject->second.authority) {
                // moved this tick, the player catches up on the next frame
                if (auto entity = entity_of(level, *player)) {
                    position = entity->getRigidbody().hitbox.position;
                }
                flags = flags_of(level, *player);
            }
            auto rotation = player->getRotation();
            State state {peer.pid, position, subject->second.version, {}};
            write_record(
                state.record,
                Record {
                    static_cast<uint16_t>(peer.pid),
                    position,
                    rotation.x,
                    rotation.y,
                    flags
                }
            );
            states.push_back(std::move(state));
        }
        for (auto& [conn, peer] : peers) {
            auto recipient = level.players->get(peer.pid);
            if (recipient == nullptr) {
                continue;
            }
            glm::vec3 origin = recipient->getPosition();
            std::string message;
            int count = 0;
            auto send = [&, conn = conn]() {
                if (count == 0) {
                    return;
                }
                message[5] = static_cast<char>(count & 0xFF);
                message[6] = static_cast<char>(count >> 8);
                if (netgame::send(conn, message, {false, true, config.lane})) {
                    stats.messages++;
                    stats.bytes += message.size();
                }
                message.clear();
                count = 0;
            };
            for (const auto& state : states) {
                if (state.pid == peer.pid) {
                    continue;
                }
                auto& sent = peer.sent[state.pid];
                if (sent == state.version) {
                    continue;
                }
                float distance = glm::length(glm::vec2(
                    state.position.x - origin.x, state.position.z - origin.z
                ));
                if ((serverTick + state.pid) % period_for(distance) != 0) {
                    continue;
                }
                if (count == 0) {
                    put_u8(message, T_SNAPSHOT);
                    put_u32(message, timeMs);
                    put_u16(message, 0);
                }
                message += state.record;
                count++;
                sent = state.version;
                stats.records++;
                if (count == MAX_RECORDS) {
                    send();
                }
            }
            send();
        }
    }

    void client_tick(Level& level) {
        auto player = level.players->get(localPid);
        if (player == nullptr) {
            return;
        }
        if (localAuthority) {
            // every command until acked: a lost message costs nothing
            if (!unacked.empty()) {
                std::string message;
                write_input(message, unacked);
                netgame::send(localConnection, message, {false, true, config.lane});
            }
            return;
        }
        const auto& position = player->getPosition();
        auto rotation = player->getRotation();
        std::string message;
        put_u8(message, T_MOVE);
        put_u32(message, ++moveSeq);
        put_f32(message, position.x);
        put_f32(message, position.y);
        put_f32(message, position.z);
        put_f32(message, rotation.x);
        put_f32(message, rotation.y);
        put_u8(message, flags_of(level, *player));
        netgame::send(localConnection, message, {false, true, config.lane});
    }
}

void detail::write_record(std::string& out, const Record& record) {
    float yaw = std::fmod(record.yaw, 360.0f);
    if (yaw < 0) {
        yaw += 360.0f;
    }
    const auto& position = record.position;
    put_u16(out, record.pid);
    put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(position.x * 256))));
    put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(position.y * 256))));
    put_u32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(position.z * 256))));
    put_u16(out, static_cast<uint16_t>(std::lround(yaw / 360.0f * 65536.0f) & 0xFFFF));
    put_u16(out, static_cast<uint16_t>(static_cast<int16_t>(
        std::clamp<long>(std::lround(record.pitch * 300), -32767, 32767)
    )));
    put_u8(out, record.flags);
}

bool detail::read_record(
    const std::vector<uint8_t>& data, size_t offset, Record& record
) {
    Reader reader {data, offset};
    record.pid = reader.u16();
    record.position.x = static_cast<int32_t>(reader.u32()) / 256.0f;
    record.position.y = static_cast<int32_t>(reader.u32()) / 256.0f;
    record.position.z = static_cast<int32_t>(reader.u32()) / 256.0f;
    record.yaw = reader.u16() * 360.0f / 65536.0f;
    record.pitch = static_cast<int16_t>(reader.u16()) / 300.0f;
    record.flags = reader.u8();
    return reader.ok;
}

Pose detail::sample_at(
    const std::deque<Sample>& samples, double time, double maxExtrapolation
) {
    auto pose_of = [](const Sample& sample) {
        return Pose {
            sample.position, {}, sample.yaw, sample.pitch, sample.flags
        };
    };
    if (time <= samples.front().time) {
        return pose_of(samples.front());
    }
    if (time >= samples.back().time) {
        Pose pose = pose_of(samples.back());
        if (samples.size() >= 2) {
            const auto& b = samples.back();
            glm::vec3 velocity = velocity_to(samples, samples.size() - 1);
            double ahead = time - b.time;
            if (ahead < maxExtrapolation) {
                pose.velocity = velocity;
            } else {
                ahead = maxExtrapolation;
            }
            pose.position += velocity * static_cast<float>(ahead);
        }
        return pose;
    }
    size_t i = 1;
    while (samples[i].time < time) {
        i++;
    }
    const auto& a = samples[i - 1];
    const auto& b = samples[i];
    double span = b.time - a.time;
    float k = static_cast<float>((time - a.time) / span);
    Pose pose = pose_of(b);
    pose.position = glm::mix(a.position, b.position, k);
    pose.velocity = velocity_to(samples, i);
    pose.yaw = lerp_angle(a.yaw, b.yaw, k);
    pose.pitch = a.pitch + (b.pitch - a.pitch) * k;
    return pose;
}

glm::vec3 detail::fade(const glm::vec3& jump, double dt, double halfLife) {
    if (halfLife <= 0) {
        return {};
    }
    return jump * static_cast<float>(std::exp2(-dt / halfLife));
}

void detail::write_input(std::string& out, const std::deque<Command>& commands) {
    size_t count = std::min(commands.size(), MAX_COMMANDS);
    size_t first = commands.size() - count;
    put_u8(out, T_INPUT);
    put_u32(out, count ? commands[first].seq : 0);
    put_u8(out, static_cast<uint8_t>(count));
    for (size_t i = first; i < commands.size(); i++) {
        const auto& cmd = commands[i];
        put_f32(out, cmd.dt);
        put_u8(out, cmd.keys);
        put_u8(out, cmd.hasKeys ? 1 : 0);
        put_f32(out, cmd.yaw);
        put_f32(out, cmd.pitch);
    }
}

bool detail::read_input(
    const std::vector<uint8_t>& data, std::vector<Command>& commands
) {
    Reader reader {data};
    reader.u8();
    uint32_t seq = reader.u32();
    uint8_t count = reader.u8();
    if (!reader.ok || data.size() != 6 + count * COMMAND_SIZE) {
        return false;
    }
    commands.clear();
    for (int i = 0; i < count; i++) {
        Command cmd;
        cmd.seq = seq + i;
        cmd.dt = reader.f32();
        cmd.keys = reader.u8();
        cmd.hasKeys = reader.u8() != 0;
        cmd.yaw = reader.f32();
        cmd.pitch = reader.f32();
        commands.push_back(cmd);
    }
    return reader.ok;
}

void replication::configure(const Config& newConfig) {
    config = newConfig;
}

const Config& replication::get_config() {
    return config;
}

void replication::bind_player(ConnectionId connection, int64_t pid, bool authority) {
    peers[connection] = ServerPeer {pid, {}};
    Subject subject;
    subject.authority = authority;
    subjects[pid] = std::move(subject);
}

void replication::bind_local(ConnectionId connection, int64_t pid, bool authority) {
    localConnection = connection;
    localPid = pid;
    moveSeq = 0;
    clockSet = false;
    lastFrame = -1.0;
    localAuthority = authority;
    inputSeq = 0;
    lastAck = 0;
    unacked.clear();
    history.clear();
    correction = {};
}

bool replication::is_driven(int64_t pid) {
    auto found = subjects.find(pid);
    return found != subjects.end() && found->second.authority;
}

int replication::get_input(int64_t pid) {
    if (pid == drivingPid) {
        return drivingKeys;
    }
    auto found = presetInputs.find(pid);
    return found == presetInputs.end() ? -1 : found->second;
}

void replication::set_input(int64_t pid, int keys) {
    if (keys < 0) {
        presetInputs.erase(pid);
    } else {
        presetInputs[pid] = keys & 0xFF;
    }
}

void replication::record_input(Level& level, int64_t pid, int keys, float delta) {
    if (!localConnection || !localAuthority || pid != localPid) {
        return;
    }
    auto player = level.players->get(pid);
    auto entity = player ? entity_of(level, *player) : nullptr;
    if (entity == nullptr) {
        return;
    }
    auto& hitbox = entity->getRigidbody().hitbox;
    // a part of the difference from the server
    if (correction != glm::vec3(0.0f)) {
        glm::vec3 part = correction - fade(correction, delta, config.correctionSmoothing);
        correction -= part;
        player->teleport(hitbox.position + part);
    }
    auto rotation = player->getRotation();
    Command cmd {
        ++inputSeq,
        delta,
        static_cast<uint8_t>(keys < 0 ? 0 : keys),
        keys >= 0,
        rotation.x,
        rotation.y
    };
    unacked.push_back(cmd);
    if (unacked.size() > MAX_COMMANDS) {
        unacked.pop_front();
    }
    history.push_back({cmd.seq, hitbox.position + correction});
    if (history.size() > MAX_HISTORY) {
        history.pop_front();
    }
    stats.commandsRecorded++;
}

void replication::add_remote(uint32_t serverPid, int64_t pid) {
    remotes[serverPid] = Remote {pid, {}};
}

void replication::remove_remote(uint32_t serverPid) {
    remotes.erase(serverPid);
}

void replication::unbind(ConnectionId connection) {
    auto found = peers.find(connection);
    if (found != peers.end()) {
        int64_t pid = found->second.pid;
        peers.erase(found);
        subjects.erase(pid);
        // the pid may be given to the next player
        for (auto& [_, peer] : peers) {
            peer.sent.erase(pid);
        }
    }
    if (connection == localConnection) {
        localConnection = 0;
        localPid = -1;
        remotes.clear();
        clockSet = false;
        localAuthority = false;
        unacked.clear();
        history.clear();
        correction = {};
    }
}

void replication::reset() {
    serverStart = -1.0;
    peers.clear();
    subjects.clear();
    remotes.clear();
    localConnection = 0;
    localPid = -1;
    clockSet = false;
    lastFrame = -1.0;
    localAuthority = false;
    unacked.clear();
    history.clear();
    correction = {};
    presetInputs.clear();
    stats = {};
}

bool replication::consume(const Event& event, Level& level) {
    if (event.type != EventType::MESSAGE || event.lane != config.lane ||
        event.data.empty()) {
        return false;
    }
    uint8_t type = event.data[0];
    if (type == T_MOVE || type == T_INPUT) {
        auto found = peers.find(event.connection);
        if (found == peers.end()) {
            return false;
        }
        int64_t pid = found->second.pid;
        auto& subject = subjects[pid];
        // an authoritative player does not tell where it is, a trusted one
        // has nothing to repeat
        if (type == T_INPUT && subject.authority) {
            queue_input(subject, event.data);
        } else if (type == T_MOVE && !subject.authority) {
            apply_move(level, pid, event.data);
        }
        return true;
    }
    bool fromServer = localConnection && event.connection == localConnection;
    if (type == T_SNAPSHOT && fromServer) {
        read_snapshot(event.data);
        return true;
    }
    if (type == T_ACK && fromServer) {
        if (localAuthority) {
            read_ack(level, event.data);
        }
        return true;
    }
    return false;
}

void replication::tick(Level& level) {
    auto started = std::chrono::steady_clock::now();
    if (!peers.empty()) {
        server_tick(level);
    }
    if (localConnection) {
        client_tick(level);
    }
    int64_t usec = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - started
    )
                       .count();
    stats.ticks++;
    stats.tickUsecTotal += usec;
    stats.tickUsecMax = std::max(stats.tickUsecMax, usec);
}

void replication::frame(Level& level) {
    if (!clockSet) {
        return;
    }
    double now = now_seconds();
    double dt = lastFrame < 0 ? 0.0 : std::max(0.0, now - lastFrame);
    lastFrame = now;
    double time = render_time(now);
    for (auto& [_, remote] : remotes) {
        auto& samples = remote.samples;
        if (samples.empty()) {
            continue;
        }
        // keep a second of history, at least two samples
        while (samples.size() > 2 && samples[1].time < time - 1.0) {
            samples.pop_front();
        }
        auto player = level.players->get(remote.localPid);
        if (player == nullptr) {
            continue;
        }
        Pose pose = sample_at(samples, time, config.maxExtrapolation);
        remote.jump = fade(remote.jump, dt, config.smoothing);
        pose.position += remote.jump;
        place_remote(level, *player, pose);
        remote.shown = true;
    }
}

Stats replication::take_stats() {
    Stats result = stats;
    stats = {};
    return result;
}
