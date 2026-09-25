-- {{name}} — a Tynima game, in one file.
--
--     tynima run          play it
--     tynima cook         cook the models under assets/ into cooked/
--
-- Leave it running and save this file: a quarter of a second later the game
-- is built again from what you wrote, in the window that never closed. What
-- `ty` offers is in engine/script/lua/tynima.lua; what it rests on is
-- sdk/include/tynima.h.
local ty = require("tynima")

local game = {}
local state = {}

-- Everything this script made, so that a reload can take it down again
-- before building it anew. Without this, every save would leave one more
-- floor and five more crates standing in the world.
local made = { entities = {}, bodies = {} }

local function entity(components)
    local made_entity = ty.entity(components)
    made.entities[#made.entities + 1] = made_entity
    return made_entity
end

local function body(values)
    local made_body = ty.body(values)
    made.bodies[#made.bodies + 1] = made_body
    return made_body
end

-- A crate at a point: a body that falls, and an entity that draws where it is.
local function spawn(x, y, z)
    return entity {
        Name = { text = "crate" },
        Transform = { position = { x, y, z } },
        MeshRenderer = { model = state.model },
        RigidBody = { body = body { shape = ty.box(0.5), position = { x, y, z }, mass = 1 } },
    }
end

function game.load(reloaded)
    ty.log(reloaded and "{{name}} reloaded" or "{{name}} is starting")

    -- A floor: one shape, used both as the thing that is drawn and as the
    -- thing that is collided with.
    local ground = ty.box(10, 0.5, 10)
    entity {
        Name = { text = "ground" },
        Transform = { position = { 0, -0.5, 0 } },
        MeshRenderer = { model = ty.shape_model(ground, { 0.40, 0.42, 0.45 }, 0.9) },
    }
    body { shape = ground, position = { 0, -0.5, 0 }, motion = "static" }

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

function game.unload()
    -- Entities first: a crate's body is nothing to the world once the thing
    -- that drew it is gone.
    for _, e in ipairs(made.entities) do ty.destroy(e) end
    for _, b in ipairs(made.bodies) do ty.destroy_body(b) end
    made = { entities = {}, bodies = {} }
end

function game.update(dt)
    if ty.key_pressed("Space") then
        state.crates[#state.crates + 1] = spawn((math.random() - 0.5) * 2, 6, (math.random() - 0.5) * 2)
        ty.log(string.format("%d crates", #state.crates))
    end

    if ty.key_pressed("R") then
        game.unload()
        game.load(false)
    end

    if ty.key_pressed("Escape") then
        ty.quit()
    end
end

return game
