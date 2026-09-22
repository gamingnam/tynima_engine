-- A game in Lua: this file is the whole of it. Run it with
--
--     tynima-sandbox --script apps/sandbox/scripts/pile.lua
--
-- and change it while it runs — a save is a reload, a quarter of a second
-- later, with the world it built still standing.
--
-- Everything here goes through the same tynima_api table a game module
-- written in C receives; see engine/script/lua/tynima.lua for what `ty`
-- offers, and sdk/include/tynima.h for what it is underneath.
local ty = require("tynima")

local game = {}
local state = {}

local BOXES = 24
local COLOURS = {
    { 0.85, 0.35, 0.28 }, { 0.30, 0.55, 0.80 }, { 0.45, 0.70, 0.35 },
    { 0.90, 0.75, 0.30 }, { 0.65, 0.40, 0.75 },
}

function game.load(reloaded)
    ty.log(reloaded and "the pile script reloaded" or "the pile script is starting")

    -- Models and bodies from the same shapes: nothing on disk is needed.
    local ground_shape = ty.box(12, 0.5, 12)
    local ground_model = ty.shape_model(ground_shape, { 0.42, 0.42, 0.40 }, 0.9)
    state.ground_body = ty.body { shape = ground_shape, position = { 0, -0.5, 0 }, motion = "static",
                                  friction = 0.7 }
    ty.entity { Name = { text = "ground" }, Transform = { position = { 0, -0.5, 0 } },
                MeshRenderer = { model = ground_model } }

    -- A pile of boxes, each an entity whose Transform follows its body.
    state.boxes = {}
    for i = 1, BOXES do
        local half = 0.25 + (i % 3) * 0.08
        local shape = ty.box(half, half, half)
        local colour = COLOURS[(i - 1) % #COLOURS + 1]
        local model = ty.shape_model(shape, colour, 0.5)
        local angle = i * 2.39996  -- the golden angle: a spiral, not a stack
        local radius = 0.35 * math.sqrt(i)
        local position = { math.cos(angle) * radius, 1.0 + i * 0.45, math.sin(angle) * radius }
        local body = ty.body { shape = shape, position = position, mass = 1.5 * half,
                               friction = 0.6, restitution = 0.05, user_data = i }
        state.boxes[i] = ty.entity {
            Name = { text = string.format("box %d", i) },
            Transform = { position = position },
            MeshRenderer = { model = model },
            RigidBody = { body = body },
        }
    end

    ty.lighting { sun = { 0.4, 1.0, 0.25 }, intensity = 3.2, ambient = 0.12, sky = { 0.09, 0.11, 0.15 } }
    ty.look_at({ 7, 5, 9 }, { 0, 1.5, 0 }, math.rad(55))
    state.time = 0
    state.shots = 0
end

function game.update(dt)
    state.time = state.time + dt

    -- The camera circles the pile, unless the mouse is dragging it around.
    if not ty.mouse_down("right") then
        local angle = state.time * 0.25
        ty.look_at({ math.cos(angle) * 11, 5, math.sin(angle) * 11 }, { 0, 1.5, 0 })
    end

    -- Space knocks the pile over: an impulse at each box, away from the middle.
    if ty.key_pressed("Space") then
        state.shots = state.shots + 1
        ty.each("Transform", "RigidBody", function(_, transform, rigid)
            local away = ty.vec3(transform.position.x, 0.4, transform.position.z):normalized()
            ty.impulse(rigid.body, away * 6)
        end)
        ty.log(string.format("shot %d: everything went flying", state.shots))
    end

    -- A light that follows the highest box, so the pile lights itself.
    local top, height = nil, -1000
    ty.each("Transform", "RigidBody", function(_, transform)
        if transform.position.y > height then
            top, height = ty.vec3(transform.position), transform.position.y
        end
    end)
    if top then
        ty.lights { { position = top + ty.vec3(0, 1.5, 0), radius = 8, color = { 3, 2.2, 1.4 } } }
    end

    if ty.key_pressed("Escape") then
        ty.quit()
    end
end

function game.unload(reloading)
    ty.log(reloading and "reloading" or "stopping")
end

return game
