-- {{name}} — a first-person scene, in one file.
--
--     tynima run     play it; save this file while it runs and it reloads
--     tynima cook    cook the models under assets/ into cooked/
--
-- A room to walk around, and a gun that is a ray and an impulse. The player
-- is a capsule that never tips over: this script says how fast it wants to
-- go, the physics world decides where that leaves it, and the camera is put
-- where the capsule ended up. That is the whole of a first-person game.
--
-- Mouse looks, W/A/S/D walks, Shift runs, Space jumps, the left button
-- shoves whatever it hits, F is the torch, Tab gives the mouse back, R
-- stands the crates up again, Escape quits.
--
-- What `ty` offers is in engine/script/lua/tynima.lua; what it rests on is
-- sdk/include/tynima.h.
local ty = require("tynima")

local game = {}

local LOOK = 0.0022            -- radians per point of mouse movement
local WALK = 4.5               -- metres a second
local RUN = 8.0                -- with Shift held
local AIR_CONTROL = 7.0        -- how much of a say you have while falling
local JUMP_SPEED = 5.6         -- about 0.9 m of height
local EXTRA_PULL = 8.0         -- on top of the world's own 9.81
local RADIUS, HALF_H = 0.35, 0.5 -- the player: 1.7 m tall in all
local EYE = 0.6                -- above the middle of the capsule
local PITCH_LIMIT = math.rad(88)
local RANGE = 60.0             -- how far the gun reaches, metres
local PUNCH = 60.0             -- newton-seconds into whatever it hits
local START = { 0, 1.2, 9 }
local ROOM, WALL = 13.0, 2.0   -- half the floor, half the height of the walls

-- What is in the room. Pillars and steps are still; crates and balls are
-- not, and carry a mark the gun reads back out of what it hit.
local PILLARS = { { -6, 6 }, { 6, 6 }, { -6, -6 }, { 6, -6 } }
local STEPS = { { 0, 0.4, -7, 2.5, 0.4 }, { 0, 1.2, -10, 2.5, 0.4 } }
local CRATES = { { -2, 0.4, 0 }, { -1, 1.2, 0.3 }, { 0, 0.4, -0.4 }, { 1, 1.2, 0.2 },
                 { 2, 0.4, 0.1 }, { -4.5, 0.4, -3 }, { 4.5, 0.4, -3 }, { 0, 2.0, 0 } }
local BALLS = { { -8, 0.5, 2 }, { 8, 0.5, 2 }, { 0, 0.5, 7 } }
local LEVEL, CRATE, BALL = 0, 1, 2 -- what the gun may have hit

-- What survives a reload: a `local` is made afresh every time the file is
-- read, a global is not.
progress = progress or { shots = 0, hits = 0, torch = true }

-- Everything this script made, so that a reload can take it down again
-- before building it anew. Without this, every save would leave one more
-- room standing inside the last one.
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

-- One model per size and colour rather than one per thing: the models a
-- script makes live as long as the engine does.
local function model_for(shape, colour, roughness)
    local key = string.format("%d,%.2f,%.2f,%.2f,%.2f,%.2f,%s", tonumber(shape.type),
                              shape.half_extents.x, shape.half_extents.y, shape.half_extents.z,
                              shape.radius, shape.half_height, table.concat(colour, ","))
    state.models[key] = state.models[key] or ty.shape_model(shape, colour, roughness)
    return state.models[key]
end

-- A box that is drawn and collided with, standing still: floor, wall, step.
local function slab(position, half, colour, roughness)
    local shape = ty.box(half[1], half[2], half[3])
    body { shape = shape, position = position, motion = "static", friction = 0.6 }
    entity {
        Name = { text = "level" },
        Transform = { position = position },
        MeshRenderer = { model = model_for(shape, colour, roughness or 0.9) },
    }
end

-- A thing that falls, is pushed, and can be shot: the mark is what the gun
-- reads back out of a hit. Props are kept in a list of their own as well,
-- so that R can put them back without taking the room down with them.
local function prop(shape, position, colour, mass, mark)
    local prop_body = body { shape = shape, position = position, mass = mass, friction = 0.5,
                             restitution = mark == BALL and 0.4 or 0.05, user_data = mark }
    local prop_entity = entity {
        Name = { text = mark == BALL and "ball" or "crate" },
        Transform = { position = position },
        MeshRenderer = { model = model_for(shape, colour, 0.6) },
        RigidBody = { body = prop_body },
    }
    state.props[#state.props + 1] = { entity = prop_entity, body = prop_body }
end

local function build_props()
    local crate = ty.box(0.4, 0.4, 0.4)
    for _, c in ipairs(CRATES) do
        prop(crate, { c[1], c[2], c[3] }, { 0.60, 0.42, 0.26 }, 12, CRATE)
    end
    local ball = ty.sphere(0.45)
    for _, b in ipairs(BALLS) do
        prop(ball, { b[1], b[2], b[3] }, { 0.75, 0.25, 0.30 }, 6, BALL)
    end
end

function game.load(reloaded)
    state = { models = {}, props = {}, time = 0, ground = false, spark_until = 0,
              spark = ty.vec3(0, 0, 0) }

    slab({ 0, -0.5, 0 }, { ROOM, 0.5, ROOM }, { 0.30, 0.31, 0.34 })
    slab({ 0, WALL, -ROOM }, { ROOM, WALL, 0.5 }, { 0.38, 0.36, 0.40 })
    slab({ 0, WALL, ROOM }, { ROOM, WALL, 0.5 }, { 0.38, 0.36, 0.40 })
    slab({ -ROOM, WALL, 0 }, { 0.5, WALL, ROOM }, { 0.34, 0.33, 0.38 })
    slab({ ROOM, WALL, 0 }, { 0.5, WALL, ROOM }, { 0.34, 0.33, 0.38 })
    for _, p in ipairs(PILLARS) do
        slab({ p[1], 1.6, p[2] }, { 0.5, 1.6, 0.5 }, { 0.46, 0.44, 0.42 }, 0.5)
    end
    for _, s in ipairs(STEPS) do
        slab({ s[1], s[2], s[3] }, { s[4], s[5], s[4] }, { 0.42, 0.40, 0.36 })
    end
    build_props()

    -- The player: a capsule that never tips over, moved by the velocity
    -- this script gives it. Its entity is what draws it — which you never
    -- see, since the camera sits inside it.
    state.body = body { shape = ty.capsule(RADIUS, HALF_H), position = START, mass = 80,
                        friction = 0, restitution = 0, lock_rotation = true }
    state.player = entity {
        Name = { text = "player" },
        Transform = { position = START },
        MeshRenderer = { model = model_for(ty.capsule(RADIUS, HALF_H), { 0.8, 0.8, 0.8 }, 0.5),
                         visible = false },
        RigidBody = { body = state.body },
    }

    state.yaw, state.pitch = 0, 0
    state.looking = true
    ty.relative_mouse(true)
    ty.lighting { sun = { 0.3, 0.9, 0.35 }, intensity = 1.6, ambient = 0.10,
                  sky = { 0.05, 0.06, 0.09 } }
    ty.log(reloaded and "{{name}} reloaded"
           or "{{name}}: mouse looks, WASD walks, click shoves, F torch, Tab frees the mouse")
end

function game.unload()
    for _, e in ipairs(made.entities) do ty.destroy(e) end
    for _, b in ipairs(made.bodies) do ty.destroy_body(b) end
    made = { entities = {}, bodies = {} }
    ty.relative_mouse(false)
end

-- Standing on something: a ray straight down from the middle of the
-- capsule, a little longer than half of it. A ray never meets the body it
-- starts inside, so this cannot find the player; the normal is what keeps a
-- wall from counting as a floor.
local function on_ground(position)
    local hit = ty.cast_ray(position, { 0, -1, 0 }, HALF_H + RADIUS + 0.15)
    return hit ~= nil and hit.normal.y > 0.6
end

-- The gun: a ray from the eye, and an impulse where it lands. Everything
-- about what was hit comes back out of the hit — including the mark the
-- body was made with, which is how a crate is told from a wall.
local function shoot(eye, forward)
    progress.shots = progress.shots + 1
    local hit = ty.cast_ray(eye, forward, RANGE)
    if hit == nil then
        return ty.log("a miss")
    end
    state.spark, state.spark_until = hit.position, state.time + 0.12
    local mark = tonumber(hit.user_data)
    if mark == LEVEL then
        return ty.log(string.format("the room, %.1f m away", hit.distance))
    end
    progress.hits = progress.hits + 1
    ty.impulse(hit.body, forward * PUNCH, hit.position)
    ty.log(string.format("%s at %.1f m — %d of %d shots have hit something loose",
                         mark == BALL and "a ball" or "a crate", hit.distance,
                         progress.hits, progress.shots))
end

function game.update(dt)
    dt = math.min(dt, 1 / 30) -- one slow frame must not teleport anyone
    state.time = state.time + dt

    if ty.key_pressed("Escape") then return ty.quit() end
    if ty.key_pressed("Tab") then
        state.looking = not state.looking
        ty.relative_mouse(state.looking)
    end
    if ty.key_pressed("F") then progress.torch = not progress.torch end
    if ty.key_pressed("R") then
        for _, p in ipairs(state.props) do
            ty.destroy(p.entity)
            ty.destroy_body(p.body)
        end
        state.props = {}
        build_props()
        ty.log("the crates are up again")
    end

    -- Looking: the mouse turns the head, and nothing else. Yaw and pitch
    -- are the game's own state, not read back from the camera.
    if state.looking then
        local dx, dy = ty.mouse_delta()
        state.yaw = state.yaw - dx * LOOK
        state.pitch = math.max(-PITCH_LIMIT, math.min(PITCH_LIMIT, state.pitch - dy * LOOK))
    end
    local aim = ty.euler(state.yaw, state.pitch, 0)
    local level = ty.euler(state.yaw, 0, 0)
    local forward = level:rotate(ty.vec3(0, 0, -1))
    local right = level:rotate(ty.vec3(1, 0, 0))

    -- Walking: where you want to go, at the speed you asked for. On the
    -- ground that is simply the velocity; in the air you only lean into it.
    local position = ty.body_transform(state.body)
    local velocity = ty.body_velocity(state.body)
    state.ground = on_ground(position)
    local wish = ty.vec3(0, 0, 0)
    if ty.key_down("W") then wish = wish + forward end
    if ty.key_down("S") then wish = wish - forward end
    if ty.key_down("D") then wish = wish + right end
    if ty.key_down("A") then wish = wish - right end
    local speed = (ty.key_down("LeftShift") or ty.key_down("RightShift")) and RUN or WALK
    wish = wish:length() > 0 and wish:normalized() * speed or wish
    local blend = state.ground and 1 or math.min(AIR_CONTROL * dt, 1)
    local vx = velocity.x + (wish.x - velocity.x) * blend
    local vz = velocity.z + (wish.z - velocity.z) * blend
    local vy = velocity.y - EXTRA_PULL * dt
    if state.ground and ty.key_pressed("Space") then vy = JUMP_SPEED end
    ty.set_body_velocity(state.body, { vx, vy, vz })

    -- The camera goes where the capsule ended up, at eye height.
    local eye = ty.vec3(position.x, position.y + EYE, position.z)
    ty.set_camera { position = eye, rotation = aim }

    if ty.mouse_pressed("left") then
        shoot(eye, aim:rotate(ty.vec3(0, 0, -1)))
    end

    -- Two lamps, the torch, and whatever the last shot lit up.
    local lights = {
        { position = { -6, 3.2, 0 }, radius = 16, color = { 0.9, 0.7, 0.45 } },
        { position = { 6, 3.2, 0 }, radius = 16, color = { 0.4, 0.55, 0.9 } },
    }
    if progress.torch then
        lights[#lights + 1] = { position = eye + forward * 0.6, radius = 13,
                                color = { 1.0, 0.95, 0.85 } }
    end
    if state.time < state.spark_until then
        lights[#lights + 1] = { position = state.spark, radius = 4, color = { 2.0, 1.4, 0.6 } }
    end
    ty.lights(lights)
end

return game
