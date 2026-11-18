#ifdef VOXELENGINE_NETGAME

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "network/netgame/NetGame.hpp"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/// Poll until the predicate returns true or the timeout expires
static bool pump_until(
    const std::function<void(const netgame::Event&)>& handler,
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = 5000ms
) {
    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        for (const auto& event : netgame::poll()) {
            handler(event);
        }
        netgame::flush();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

struct Loopback {
    netgame::ListenId listener;
    netgame::ConnectionId client;
    netgame::ConnectionId server = 0;
    bool clientConnected = false;

    explicit Loopback(int port) {
        listener = netgame::listen(port);
        client = netgame::connect("127.0.0.1", port);
        bool ok = pump_until(
            [this](const auto& event) { onEvent(event); },
            [this]() { return clientConnected && server != 0; }
        );
        EXPECT_TRUE(ok) << "connection timeout";
    }

    void onEvent(const netgame::Event& event) {
        if (event.type != netgame::EventType::CONNECTED) {
            return;
        }
        if (event.listener == listener) {
            server = event.connection;
        } else if (event.connection == client) {
            clientConnected = true;
        }
    }

    ~Loopback() {
        netgame::shutdown();
    }
};

TEST(netgame, reliable_echo_in_order) {
    Loopback net(27140);
    ASSERT_NE(net.server, 0u);

    const int count = 1000;
    int echoed = 0;
    bool ordered = true;
    for (int i = 0; i < count; i++) {
        ASSERT_TRUE(netgame::send(net.client, std::to_string(i), {}));
    }
    bool ok = pump_until(
        [&](const netgame::Event& event) {
            ASSERT_NE(event.type, netgame::EventType::DISCONNECTED)
                << event.text;
            if (event.type != netgame::EventType::MESSAGE) {
                return;
            }
            std::string payload(event.data.begin(), event.data.end());
            if (event.connection == net.server) {
                netgame::send(net.server, payload, {});
            } else if (event.connection == net.client) {
                ordered = ordered && payload == std::to_string(echoed);
                echoed++;
            }
        },
        [&]() { return echoed == count; }
    );
    EXPECT_TRUE(ok) << "echoed " << echoed << " of " << count;
    EXPECT_TRUE(ordered);

    auto stats = netgame::get_stats(net.client);
    ASSERT_TRUE(stats.has_value());
    EXPECT_GE(stats->ping, 0);
}

TEST(netgame, lanes_and_large_message) {
    Loopback net(27141);
    ASSERT_NE(net.server, 0u);
    netgame::configure_lanes(net.client, {0, 1}, {});

    std::string large(200 * 1024, '\0');
    for (size_t i = 0; i < large.size(); i++) {
        large[i] = static_cast<char>(i * 31);
    }
    netgame::SendOptions bulk {};
    bulk.lane = 1;
    ASSERT_TRUE(netgame::send(net.client, large, bulk));
    ASSERT_FALSE(netgame::send(net.client, "x", {true, true, 5}))
        << "lane 5 is not configured";

    bool received = false;
    bool ok = pump_until(
        [&](const netgame::Event& event) {
            if (event.type == netgame::EventType::MESSAGE &&
                event.connection == net.server) {
                EXPECT_EQ(event.lane, 1);
                EXPECT_TRUE(std::equal(
                    large.begin(), large.end(), event.data.begin(),
                    event.data.end(),
                    [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; }
                ));
                received = true;
            }
        },
        [&]() { return received; }
    );
    EXPECT_TRUE(ok);
}

TEST(netgame, close_is_reported_to_peer) {
    Loopback net(27142);
    ASSERT_NE(net.server, 0u);

    netgame::close(net.client, "bye");
    bool closed = false;
    std::string message;
    bool ok = pump_until(
        [&](const netgame::Event& event) {
            if (event.type == netgame::EventType::DISCONNECTED &&
                event.connection == net.server) {
                closed = true;
                message = event.text;
            }
        },
        [&]() { return closed; }
    );
    EXPECT_TRUE(ok);
    EXPECT_EQ(message, "bye");
    EXPECT_FALSE(netgame::send(net.client, "late", {}));
}

TEST(netgame, invalid_address) {
    EXPECT_THROW(netgame::connect("not-an-ip", 1), std::runtime_error);
    netgame::shutdown();
}

#endif
