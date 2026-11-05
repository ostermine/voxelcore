#include "Replication.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "objects/Entities.hpp"
#include "objects/Entity.hpp"
#include "objects/Player.hpp"
#include "objects/Players.hpp"
#include "world/Level.hpp"

using namespace netgame;
using namespace netgame::replication;
using namespace netgame::replication::detail;

namespace {
    constexpr uint8_t T_MOVE = 1;
    constexpr uint8_t T_SNAPSHOT = 5;
    /// 7 + 60 * 19 bytes fits one UDP packet
    constexpr int MAX_RECORDS = 60;
    constexpr size_t MAX_SAMPLES = 32;

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
        std::vector<State> states;
        for (const auto& [_, peer] : peers) {
            auto subject = subjects.find(peer.pid);
            auto player = level.players->get(peer.pid);
            if (player == nullptr || subject == subjects.end() ||
                subject->second.version == 0) {
                continue;
            }
            const auto& position = player->getPosition();
            auto rotation = player->getRotation();
            State state {peer.pid, position, subject->second.version, {}};
            write_record(
                state.record,
                Record {
                    static_cast<uint16_t>(peer.pid),
                    position,
                    rotation.x,
                    rotation.y,
                    subject->second.flags
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

void replication::configure(const Config& newConfig) {
    config = newConfig;
}

const Config& replication::get_config() {
    return config;
}

void replication::bind_player(ConnectionId connection, int64_t pid) {
    peers[connection] = ServerPeer {pid, {}};
    subjects[pid] = Subject {};
}

void replication::bind_local(ConnectionId connection, int64_t pid) {
    localConnection = connection;
    localPid = pid;
    moveSeq = 0;
    clockSet = false;
    lastFrame = -1.0;
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
    stats = {};
}

bool replication::consume(const Event& event, Level& level) {
    if (event.type != EventType::MESSAGE || event.lane != config.lane ||
        event.data.empty()) {
        return false;
    }
    uint8_t type = event.data[0];
    if (type == T_MOVE) {
        auto found = peers.find(event.connection);
        if (found == peers.end()) {
            return false;
        }
        apply_move(level, found->second.pid, event.data);
        return true;
    }
    if (type == T_SNAPSHOT && localConnection &&
        event.connection == localConnection) {
        read_snapshot(event.data);
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
