local tsf = entity.transform
local body = entity.rigidbody
local mob = entity:require_component("core:mob")

local cheat_speed_mul = 10.0

local function get_player_rotation(pid)
    local rx, ry, rz = player.get_rot(pid)
    local matrix = mat4.rotate({0, 1, 0}, rx)
    mat4.rotate(matrix, {1, 0, 0}, ry, matrix)
    mat4.rotate(matrix, {0, 0, 1}, rz, matrix)
    return matrix
end

-- the keys of the local player; nil while the input goes to a screen
local function read_keys()
    if not hud or hud.is_inventory_open() or menu.page ~= "" then
        return nil
    end
    return {
        forward = input.is_active('movement.forward'),
        back = input.is_active('movement.back'),
        left = input.is_active('movement.left'),
        right = input.is_active('movement.right'),
        jump = input.is_active('movement.jump'),
        sprint = input.is_active('movement.sprint'),
        crouch = input.is_active('movement.crouch'),
        cheat = input.is_active('movement.cheat'),
    }
end

local function apply_keys(keys, rot, delta)
    local front = mat4.mul(rot, {0, 0, -1})
    local right = mat4.mul(rot, {1, 0, 0})

    front[2] = 0.0
    vec3.normalize(front, front)

    body:set_crouching(keys.crouch)

    local vel = body:get_vel()
    local speed = keys.cheat and cheat_speed_mul or 1.0

    local dir = {0, 0, 0}

    if keys.forward then vec3.add(dir, front, dir) end
    if keys.back then vec3.sub(dir, front, dir) end
    if keys.right then vec3.add(dir, right, dir) end
    if keys.left then vec3.sub(dir, right, dir) end

    if vec3.length(dir) > 0.0 then
        mob.go({dir[1], dir[3]}, speed, keys.sprint, keys.crouch, vel, delta)
    end

    if mob.is_flight() then
        if keys.jump then
            mob.move_vertical(speed / 4.0)
        elseif keys.crouch then
            mob.move_vertical(-speed / 4.0)
        end
    elseif body:is_grounded() and keys.jump then
        mob.jump()
    end
end

function on_player_set(pid)
    mob.set_flight(player.is_flight(pid))
    body:set_body_type(player.is_noclip(pid) and "kinematic" or "dynamic")
end

function on_physics_update(delta)
    local pid = entity:get_player()
    if pid == -1 then
        return
    end

    mob.set_flight(player.is_flight(pid))
    -- netgame (fork build): a player moved by the commands of its client
    -- is out of the common physics step, the server steps it itself
    if not (netgame and netgame.is_driven(pid)) then
        body:set_body_type(player.is_noclip(pid) and "kinematic" or "dynamic")
    end

    local rot = get_player_rotation(pid)
    local front = mat4.mul(rot, {0, 0, -1})
    local pos = tsf:get_pos()

    -- keys not from the keyboard: a client's command on the server, a bot
    local keys = netgame and netgame.get_input(pid)
    if keys == nil and vc.is_client() and pid == hud.get_player() then
        keys = read_keys()
    end
    if netgame then
        -- the client keeps what it did for the server to repeat
        netgame.record_input(pid, keys, delta)
    end
    if keys then
        apply_keys(keys, rot, delta)
    end
    mob.look_at(vec3.add(pos, front))
end
