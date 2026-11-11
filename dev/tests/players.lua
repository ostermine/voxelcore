local util = require "core:tests_util"
util.create_demo_world()

local near = player.create("Xerxes")
local far = player.create("Segfault")
player.set_pos(near, 5, 100, 0)
player.set_pos(far, 10, 100, 0)

local found = player.get_all_in_radius({0, 100, 0}, 8)
assert(table.has(found, near), "player at distance 5 not found in radius 8")
assert(not table.has(found, far), "player at distance 10 found in radius 8")
