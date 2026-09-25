-- {{name}} — a Tynima game, in one file.
--
--     tynima run          play it
--     tynima cook         cook the models under assets/ into cooked/
--
-- Leave it running and save this file: the game reloads in a quarter of a
-- second, keeping the world it built. What `ty` offers is in
-- engine/script/lua/tynima.lua; what it rests on is sdk/include/tynima.h.
local ty = require("tynima")

local game = {}
local state = {}

function game.load(reloaded)
    ty.log(reloaded and "{{name}} reloaded" or "{{name}} is starting")

    -- A floor: one shape, used both as the thing that is drawn and as the
    -- thing that is collided with.
    local ground = ty.box(10, 0.5, 10)
    ty.entity {
        Name = { text = "ground" },
        Transform = { position = { 0, -0.5, 0 } },
        MeshRenderer = { model = ty.shape_model(ground, { 0.40, 0.42, 0.45 }, 0.9) },
    }
    state.ground = ty.body { shape = ground, position = { 0, -0.5, 0 }, motion = "static" }

    -- Something to look at, and to knock over.
    state.model = ty.shape_model(ty.box(0.5), { 0.85, 0.45, 0.25 }, 0.4)
    state.crates = {}
    for i = 1, 5 do
        state.crates[i] = spawn(0, 0.5 + (i - 1) * 1.2, 0)
    end

    ty.lighting { sun = { 0.4, 1.0, 0.3 }, intensity = 3.0, ambient = 0.12, sky = { 0.10, 0.12, 0.16 } }
    ty.look_at({ 6, 4, 7 }, { 0, 1, 0 })
    ty.log("Space drops another crate, R starts over, Escape quits")
end

-- A crate at a point: a body that falls, and an entity that draws where it is.
function spawn(x, y, z)
    local body = ty.body { shape = ty.box(0.5), position = { x, y, z }, mass = 1 }
    return ty.entity {
        Name = { text = "crate" },
        Transform = { position = { x, y, z } },
        MeshRenderer = { model = state.model },
        RigidBody = { body = body },
    }
end

function game.update(dt)
    if ty.key_pressed("Space") then
        state.crates[#state.crates + 1] = spawn((math.random() - 0.5) * 2, 6, (math.random() - 0.5) * 2)
        ty.log(string.format("%d crates", #state.crates))
    end

    if ty.key_pressed("R") then
        for _, entity in ipairs(state.crates) do
            local rigid = ty.get(entity, "RigidBody")
            if rigid then ty.destroy_body(rigid.body) end
            ty.destroy(entity)
        end
        state.crates = {}
        for i = 1, 5 do
            state.crates[i] = spawn(0, 0.5 + (i - 1) * 1.2, 0)
        end
    end

    if ty.key_pressed("Escape") then
        ty.quit()
    end
end

return game
