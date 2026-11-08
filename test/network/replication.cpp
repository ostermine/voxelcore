#ifdef VOXELENGINE_NETGAME

#include <gtest/gtest.h>

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "network/netgame/Replication.hpp"

using namespace netgame::replication::detail;

static std::vector<uint8_t> bytes_of(const std::string& text) {
    return {text.begin(), text.end()};
}

/// Signed difference of two angles in degrees, -180..180
static float angle_diff(float a, float b) {
    return std::fmod(a - b + 540.0f, 360.0f) - 180.0f;
}

/// Velocity (8, 0, 16) blocks per second between them
static std::deque<Sample> two_samples() {
    return {
        Sample {1.0, {0, 0, 0}, 350.0f, 0.0f, 0},
        Sample {1.125, {1, 0, 2}, 10.0f, 20.0f, FLAG_GROUNDED},
    };
}

TEST(netgame_replication, record_round_trip) {
    std::string out;
    write_record(
        out,
        Record {
            513, {-12.3f, 70.5f, 1000.01f}, -90.0f, -45.5f,
            FLAG_CROUCH | FLAG_GROUNDED
        }
    );
    ASSERT_EQ(out.size(), RECORD_SIZE);

    Record record;
    ASSERT_TRUE(read_record(bytes_of(out), 0, record));
    EXPECT_EQ(record.pid, 513);
    EXPECT_NEAR(record.position.x, -12.3f, 1.0f / 512);
    EXPECT_NEAR(record.position.y, 70.5f, 1.0f / 512);
    EXPECT_NEAR(record.position.z, 1000.01f, 1.0f / 512);
    EXPECT_NEAR(record.yaw, 270.0f, 0.01f);
    EXPECT_NEAR(record.pitch, -45.5f, 1.0f / 300);
    EXPECT_EQ(record.flags, FLAG_CROUCH | FLAG_GROUNDED);
}

TEST(netgame_replication, record_yaw_wraps_pitch_clamps) {
    std::string out;
    write_record(out, Record {1, {}, 725.0f, 500.0f, 0});
    Record record;
    ASSERT_TRUE(read_record(bytes_of(out), 0, record));
    EXPECT_NEAR(record.yaw, 5.0f, 0.01f);
    EXPECT_NEAR(record.pitch, 32767 / 300.0f, 0.01f);
}

TEST(netgame_replication, record_at_offset_and_truncated) {
    std::string out = "abc";
    write_record(out, Record {7, {1, 2, 3}, 0.0f, 0.0f, 0});
    auto data = bytes_of(out);
    Record record;
    ASSERT_TRUE(read_record(data, 3, record));
    EXPECT_EQ(record.pid, 7);
    EXPECT_EQ(record.position, glm::vec3(1, 2, 3));

    data.pop_back();
    EXPECT_FALSE(read_record(data, 3, record));
}

TEST(netgame_replication, interpolates_between_samples) {
    auto pose = sample_at(two_samples(), 1.0625, 0.25);
    EXPECT_NEAR(pose.position.x, 0.5f, 1e-4f);
    EXPECT_NEAR(pose.position.z, 1.0f, 1e-4f);
    EXPECT_NEAR(pose.velocity.x, 8.0f, 1e-3f);
    EXPECT_NEAR(pose.velocity.z, 16.0f, 1e-3f);
    // the short way, across 0
    EXPECT_NEAR(angle_diff(pose.yaw, 0.0f), 0.0f, 1e-3f);
    EXPECT_NEAR(pose.pitch, 10.0f, 1e-3f);
    EXPECT_EQ(pose.flags, FLAG_GROUNDED);
}

TEST(netgame_replication, extrapolates_then_stops) {
    auto samples = two_samples();
    auto pose = sample_at(samples, 1.25, 0.25);
    EXPECT_NEAR(pose.position.x, 2.0f, 1e-4f);
    EXPECT_NEAR(pose.position.z, 4.0f, 1e-4f);
    EXPECT_NEAR(pose.velocity.x, 8.0f, 1e-3f);

    // no farther than 0.25 s past the last sample, and still
    pose = sample_at(samples, 2.125, 0.25);
    EXPECT_NEAR(pose.position.x, 3.0f, 1e-4f);
    EXPECT_NEAR(pose.position.z, 6.0f, 1e-4f);
    EXPECT_EQ(pose.velocity, glm::vec3(0.0f));
}

TEST(netgame_replication, samples_a_moment_apart) {
    // a server catching up after a pause: the last two ticks 2 ms apart
    std::deque<Sample> samples {
        Sample {1.0, {0, 0, 0}, 0.0f, 0.0f, 0},
        Sample {1.05, {0.25f, 0, 0}, 0.0f, 0.0f, 0},
        Sample {1.052, {0.5f, 0, 0}, 0.0f, 0.0f, 0},
    };
    // over 52 ms, not 0.25 blocks in 2 ms (125 blocks per second)
    auto pose = sample_at(samples, 1.152, 0.25);
    EXPECT_NEAR(pose.velocity.x, 0.5f / 0.052f, 0.01f);
    EXPECT_NEAR(pose.position.x, 0.5f + 0.5f / 0.052f * 0.1f, 1e-3f);

    pose = sample_at(samples, 1.051, 0.25);
    EXPECT_NEAR(pose.velocity.x, 0.5f / 0.052f, 0.01f);

    // too little history: no faster than the distance in MIN_SPAN
    std::deque<Sample> two {samples[1], samples[2]};
    pose = sample_at(two, 1.06, 0.25);
    EXPECT_NEAR(pose.velocity.x, 0.25f / 0.04f, 0.01f);
}

TEST(netgame_replication, before_first_and_single_sample) {
    auto samples = two_samples();
    auto pose = sample_at(samples, 0.5, 0.25);
    EXPECT_EQ(pose.position, samples.front().position);
    EXPECT_EQ(pose.velocity, glm::vec3(0.0f));

    std::deque<Sample> one {samples.back()};
    pose = sample_at(one, 5.0, 0.25);
    EXPECT_EQ(pose.position, one.front().position);
    EXPECT_EQ(pose.velocity, glm::vec3(0.0f));
}

TEST(netgame_replication, input_round_trip) {
    std::deque<Command> commands;
    for (uint32_t i = 0; i < 3; i++) {
        commands.push_back(Command {
            41 + i, 1.0f / 60 + i * 0.001f, static_cast<uint8_t>(1 | (16 << i)),
            i != 1, 12.34f * i, -5.5f
        });
    }
    std::string out;
    write_input(out, commands);
    ASSERT_EQ(out.size(), 6 + 3 * COMMAND_SIZE);

    std::vector<Command> read;
    ASSERT_TRUE(read_input(bytes_of(out), read));
    ASSERT_EQ(read.size(), 3u);
    for (size_t i = 0; i < 3; i++) {
        EXPECT_EQ(read[i].seq, commands[i].seq);
        // the server steps with exactly the client's time and rotation
        EXPECT_EQ(read[i].dt, commands[i].dt);
        EXPECT_EQ(read[i].keys, commands[i].keys);
        EXPECT_EQ(read[i].hasKeys, commands[i].hasKeys);
        EXPECT_EQ(read[i].yaw, commands[i].yaw);
        EXPECT_EQ(read[i].pitch, commands[i].pitch);
    }
}

TEST(netgame_replication, input_keeps_the_last_and_rejects_broken) {
    std::deque<Command> commands;
    for (uint32_t i = 1; i <= MAX_COMMANDS + 10; i++) {
        commands.push_back(Command {i, 0.05f, 0, true, 0.0f, 0.0f});
    }
    std::string out;
    write_input(out, commands);
    std::vector<Command> read;
    ASSERT_TRUE(read_input(bytes_of(out), read));
    ASSERT_EQ(read.size(), MAX_COMMANDS);
    EXPECT_EQ(read.front().seq, 11u);
    EXPECT_EQ(read.back().seq, MAX_COMMANDS + 10);

    auto data = bytes_of(out);
    data.pop_back();
    EXPECT_FALSE(read_input(data, read));
}

TEST(netgame_replication, jump_fades) {
    glm::vec3 jump {4.0f, 0.0f, -2.0f};
    EXPECT_NEAR(fade(jump, 0.08, 0.08).x, 2.0f, 1e-4f);
    EXPECT_NEAR(fade(jump, 0.32, 0.08).z, -0.125f, 1e-4f);
    EXPECT_EQ(fade(jump, 0.0, 0.08), jump);
    EXPECT_EQ(fade(jump, 0.1, 0.0), glm::vec3(0.0f));
}

#endif
