-- player.delete removes the player's inventory too
app.config_packs({"base"})
app.new_world("demo", "2019", "core:default")

local pid = player.create("Temporary")
local invid = player.get_inventory(pid)
assert(inventory.size(invid) > 0)

player.delete(pid)
app.tick()
assert(not pcall(inventory.size, invid), "the inventory of a deleted player is still there")

app.close_world(false)
app.delete_world("demo")
