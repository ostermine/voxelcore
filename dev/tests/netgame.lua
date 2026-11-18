-- netgame loopback: connect, reliable echo, Bytearray payload, close.
-- The library exists only in builds with VOXELENGINE_NETGAME.
if not netgame then
    print("netgame is not built, skipping")
    return
end

local PORT = 27150
local COUNT = 50

local listener = netgame.listen(PORT)
local client = netgame.connect("localhost", PORT)
local server_conn
local connected = false
local echoed = 0
local last_bytes
local server_closed = false

local function pump()
    for _, event in ipairs(netgame.poll()) do
        if event.type == "connected" then
            if event.listener == listener then
                server_conn = event.conn
            elseif event.conn == client then
                connected = true
            end
        elseif event.type == "message" then
            if event.conn == server_conn then
                netgame.send(server_conn, event.data)
            elseif event.conn == client then
                echoed = echoed + 1
                last_bytes = event.data
            end
        elseif event.type == "disconnected" then
            assert(event.conn == server_conn, "unexpected disconnect")
            server_closed = true
        end
    end
    netgame.flush()
end

app.sleep_until(function ()
    pump()
    return connected and server_conn ~= nil
end, 2000, 10)

for i = 1, COUNT - 1 do
    assert(netgame.send(client, "ping " .. i, {nagle = false}))
end
assert(netgame.send(client, Bytearray({1, 2, 3, 250})))

app.sleep_until(function ()
    pump()
    return echoed == COUNT
end, 2000, 10)

assert(#last_bytes == 4)
assert(last_bytes[1] == 1 and last_bytes[4] == 250)

local stats = netgame.get_stats(client)
assert(stats and stats.ping >= 0)
debug.log(string.format("ping %s ms, out %.0f B/s", stats.ping, stats.out_bytes_per_sec))

netgame.close(client, "bye")
app.sleep_until(function ()
    pump()
    return server_closed
end, 2000, 10)
assert(not netgame.send(client, "late"))

netgame.close_listen(listener)
netgame.shutdown()
