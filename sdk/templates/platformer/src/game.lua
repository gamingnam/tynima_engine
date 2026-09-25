-- {{name}} — a 2D platformer, in one file.
--
--     tynima run     play it; save this file while it runs and it reloads
--     tynima cook    cook the models under assets/ into cooked/
--
-- The engine is 3D. This game is 2D because everything it makes sits on the
-- z = 0 plane and the camera looks straight down -z at it: things at one
-- depth are all scaled alike, so the picture is flat. The scenery further
-- back is not, which is why it slides past more slowly as the camera pans —
-- parallax, for nothing.
--
-- A and D (or the arrows) run, Space jumps — let go early for a short hop.
-- Collect the coins, reach the flag. R starts the level again, Escape quits.
--
-- What `ty` offers is in engine/script/lua/tynima.lua; what it rests on is
-- sdk/include/tynima.h.
local ty = require("tynima")

local game = {}

-- How it plays. These are the numbers to change first: every one of them is
-- felt within a jump of saving the file.
local RUN_SPEED = 7.0     -- metres a second
local GROUND_ACCEL = 70.0 -- how fast that speed is reached, m/s²
local AIR_ACCEL = 24.0    -- less of a say in the air
local JUMP_SPEED = 8.4    -- m/s upwards: about 1.4 m of height
local RISE_PULL = 15.0    -- the game's own gravity, on top of the world's 9.81
local FALL_PULL = 28.0    -- heavier coming down: the arc a platformer wants
local CUT_JUMP = 0.45     -- what is left of a jump when Space is let go early
local COYOTE = 0.10       -- seconds you may still jump after walking off a ledge
local BUFFER = 0.12       -- seconds a jump pressed just before landing is kept
local HALF_W, HALF_H = 0.3, 0.5 -- the player, in half extents
local START = { 0, 1.4 }
local FALLEN = -12.0      -- below this you have fallen out of the level

-- The level: { x, y, half width, half height }, in metres. Nothing here is
-- special to the engine — they are boxes that are both drawn and collided
-- with, which is how a scene is built before there is any art.
local BLOCKS = {
    { 0, 0, 5, 0.5 },        -- the ground you start on
    { 8, 1.1, 1.5, 0.4 },
    { 12.5, 2.2, 1.5, 0.4 },
    { 17, 3.3, 1.5, 0.4 },
    { 22, 2.0, 3.0, 0.5 },   -- a wide ledge, with crates on it
    { 23.5, 3.0, 0.3, 0.5 }, -- and a step in the middle of it
    { 27.5, 3.1, 1.2, 0.4 },
    { 33, 4.2, 4.0, 0.5 },   -- the top, where the flag is
}
local COINS = { { 8, 2.3 }, { 12.5, 3.4 }, { 17, 4.5 }, { 21, 3.3 },
                { 24.5, 4.2 }, { 27.5, 4.3 }, { 33, 5.6 } }
local CRATES = { { 20, 3.0 }, { 21, 4.0 }, { 24.8, 3.0 } }
local FLAG = { 34.5, 5.4 }

-- What survives a reload. A `local` is made afresh every time the file is
-- read; a global is not — so what the player has earned goes here, and the
-- level itself is rebuilt from the lines above.
progress = progress or { coins = 0, falls = 0, best = nil }

-- Everything this script made, so that a reload can take it down again
-- before building it anew. Without this, every save would leave one more
-- copy of the level standing in the world.
local made = { entities = {}, bodies = {} }
local state = {}

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

-- One model per size and colour rather than one per block: the models a
-- script makes live as long as the engine does, so it is worth asking for
-- fewer of them.
local function model_for(shape, colour, roughness)
    local key = string.format("%.2f,%.2f,%.2f,%s", shape.half_extents.x, shape.half_extents.y,
                              shape.half_extents.z, table.concat(colour, ","))
    state.models[key] = state.models[key] or ty.shape_model(shape, colour, roughness)
    return state.models[key]
end

-- A block: the same box drawn and collided with, standing still.
local function block(x, y, half_w, half_h, colour)
    local shape = ty.box(half_w, half_h, 1.0)
    body { shape = shape, position = { x, y, 0 }, motion = "static", friction = 0.4 }
    entity {
        Name = { text = "block" },
        Transform = { position = { x, y, 0 } },
        MeshRenderer = { model = model_for(shape, colour, 0.9) },
    }
end

-- Scenery: drawn, never collided with, and further back — so it drifts as
-- the camera moves. Depth is the whole of the effect.
local function backdrop()
    for i = 1, 14 do
        local x = -8 + i * 3.6
        local height = 2.5 + ((i * 7) % 5) * 1.1
        local depth = (i % 2 == 0) and -9 or -17
        local shade = (i % 2 == 0) and { 0.15, 0.18, 0.28 } or { 0.10, 0.12, 0.20 }
        local shape = ty.box(1.6, height, 1.0)
        entity {
            Name = { text = "backdrop" },
            Transform = { position = { x, height - 3.0, depth } },
            MeshRenderer = { model = model_for(shape, shade, 1.0) },
        }
    end
end

function game.load(reloaded)
    state = { time = 0, models = {}, coins = {}, ground = false,
              since_ground = 0, since_jump = 99, won = false }

    backdrop()
    for _, b in ipairs(BLOCKS) do
        block(b[1], b[2], b[3], b[4], { 0.36, 0.40, 0.46 })
    end

    -- The player: a box that never tips over, whose velocity this script
    -- sets every frame. The physics world does the rest — it is what stops
    -- the player walking through the level.
    state.body = body { shape = ty.box(HALF_W, HALF_H, HALF_W), position = { START[1], START[2], 0 },
                        mass = 5, friction = 0, restitution = 0, lock_rotation = true }
    state.player = entity {
        Name = { text = "player" },
        Transform = { position = { START[1], START[2], 0 } },
        MeshRenderer = { model = model_for(ty.box(HALF_W, HALF_H, HALF_W), { 0.90, 0.45, 0.25 }, 0.4) },
        RigidBody = { body = state.body },
    }

    local coin_shape = ty.box(0.18, 0.28, 0.18)
    for i, c in ipairs(COINS) do
        state.coins[i] = {
            x = c[1], y = c[2],
            entity = entity {
                Name = { text = "coin" },
                Transform = { position = { c[1], c[2], 0 } },
                MeshRenderer = { model = model_for(coin_shape, { 0.95, 0.78, 0.25 }, 0.25) },
            },
        }
    end

    -- Crates are the other half: real physics, pushed about by the player
    -- and by each other. Nothing in this script moves them.
    local crate_shape = ty.box(0.35, 0.35, 0.35)
    for _, c in ipairs(CRATES) do
        entity {
            Name = { text = "crate" },
            Transform = { position = { c[1], c[2], 0 } },
            MeshRenderer = { model = model_for(crate_shape, { 0.55, 0.38, 0.24 }, 0.7) },
            RigidBody = { body = body { shape = crate_shape, position = { c[1], c[2], 0 }, mass = 4,
                                        friction = 0.5, lock_rotation = true } },
        }
    end

    local flag_shape = ty.box(0.12, 0.7, 0.12)
    entity {
        Name = { text = "flag" },
        Transform = { position = { FLAG[1], FLAG[2], 0 } },
        MeshRenderer = { model = model_for(flag_shape, { 0.30, 0.85, 0.45 }, 0.3) },
    }

    ty.lighting { sun = { 0.35, 0.85, 0.45 }, intensity = 3.0, ambient = 0.30,
                  sky = { 0.09, 0.12, 0.18 } }
    state.camera_x, state.camera_y = START[1], START[2] + 1.0
    ty.set_camera { position = { state.camera_x, state.camera_y, 11 }, rotation = ty.euler(0, 0, 0),
                    fov_y = math.rad(42) }
    ty.log(reloaded and "{{name}} reloaded" or "{{name}}: A/D run, Space jumps, R restarts, Escape quits")
end

function game.unload()
    -- Entities first: a crate's body is nothing to the world once the thing
    -- that drew it is gone. Destroying what is already destroyed is not an
    -- error, which is what makes a collected coin safe to list here.
    for _, e in ipairs(made.entities) do ty.destroy(e) end
    for _, b in ipairs(made.bodies) do ty.destroy_body(b) end
    made = { entities = {}, bodies = {} }
end

-- Standing on something: a ray straight down from the middle of the player,
-- a little longer than they are tall. A ray never meets the body it starts
-- inside, so this cannot find the player themselves; the normal is what keeps
-- a wall from counting as a floor.
local function on_ground(position)
    local hit = ty.cast_ray({ position.x, position.y, 0 }, { 0, -1, 0 }, HALF_H + 0.14)
    return hit ~= nil and hit.normal.y > 0.5
end

local function restart(kept_coins)
    progress.coins = kept_coins or 0
    game.unload()
    game.load(false)
end

function game.update(dt)
    dt = math.min(dt, 1 / 30) -- one slow frame must not teleport anyone
    state.time = state.time + dt

    if ty.key_pressed("Escape") then return ty.quit() end
    if ty.key_pressed("R") then return restart(0) end

    local position = ty.body_transform(state.body)
    local velocity = ty.body_velocity(state.body)
    state.ground = on_ground(position)

    -- Sideways: towards the speed asked for, at the rate the ground allows.
    local wish = 0
    if ty.key_down("A") or ty.key_down("Left") then wish = wish - 1 end
    if ty.key_down("D") or ty.key_down("Right") then wish = wish + 1 end
    local step = (state.ground and GROUND_ACCEL or AIR_ACCEL) * dt
    local target = wish * RUN_SPEED
    local vx = velocity.x + math.max(-step, math.min(step, target - velocity.x))

    -- Upwards: the world's gravity plus the game's, which is most of how a
    -- jump feels. Coyote time forgives a jump pressed just too late, and the
    -- buffer one pressed just too early.
    local vy = velocity.y - (velocity.y > 0 and RISE_PULL or FALL_PULL) * dt
    state.since_ground = state.ground and 0 or state.since_ground + dt
    state.since_jump = ty.key_pressed("Space") and 0 or state.since_jump + dt
    if state.since_jump < BUFFER and state.since_ground < COYOTE then
        vy = JUMP_SPEED
        state.since_jump, state.since_ground = BUFFER, COYOTE -- both spent
    end
    if ty.key_released("Space") and vy > 0 then
        vy = vy * CUT_JUMP
    end
    ty.set_body_velocity(state.body, { vx, vy, 0 })

    -- A game on a plane stays on it: nothing here pushes the player off z,
    -- but a shove from a crate could.
    if math.abs(position.z) > 0.001 then
        ty.set_body_transform(state.body, { position.x, position.y, 0 })
    end

    for i = #state.coins, 1, -1 do
        local coin = state.coins[i]
        ty.get(coin.entity, "Transform").rotation = ty.euler(state.time * 2.5 + i, 0, 0)
        if math.abs(coin.x - position.x) < 0.55 and math.abs(coin.y - position.y) < 0.75 then
            ty.destroy(coin.entity)
            table.remove(state.coins, i)
            progress.coins = progress.coins + 1
            ty.log(string.format("%d coins, %d left", progress.coins, #state.coins))
        end
    end

    if not state.won and math.abs(FLAG[1] - position.x) < 0.8 and
       math.abs(FLAG[2] - position.y) < 1.3 then
        state.won = true
        progress.best = math.min(progress.best or state.time, state.time)
        ty.log(string.format("the flag, in %.1f s (best %.1f s) with %d coins", state.time,
                             progress.best, progress.coins))
        return restart(progress.coins)
    end

    if position.y < FALLEN then
        progress.falls = progress.falls + 1
        ty.set_body_transform(state.body, { START[1], START[2], 0 })
        ty.set_body_velocity(state.body, { 0, 0, 0 })
        ty.log(string.format("fallen %d times", progress.falls))
    end

    -- The camera catches up rather than sticking to the player, and never
    -- drops below the ground it started on.
    local lag = 1 - math.exp(-7 * dt)
    state.camera_x = state.camera_x + (position.x + wish * 1.2 - state.camera_x) * lag
    state.camera_y = state.camera_y + (math.max(position.y, 1.2) + 0.9 - state.camera_y) * lag
    ty.set_camera { position = { state.camera_x, state.camera_y, 11 } }
    ty.lights {
        { position = { position.x, position.y + 0.4, 1.5 }, radius = 6, color = { 0.6, 0.35, 0.15 } },
        { position = { FLAG[1], FLAG[2], 1.5 }, radius = 7, color = { 0.2, 0.9, 0.4 } },
    }
end

return game
