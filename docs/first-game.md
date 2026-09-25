# Your first game in thirty minutes

By the end of this you will have a game: a paddle you slide left and right,
cubes that fall out of the sky, and a count of the ones you caught. It is
about seventy lines of Lua, and you will not compile anything or restart it
once — every save takes effect in the running window a quarter of a second
later.

You need the engine built once and the `tynima` command on your path:

```sh
cmake --preset macos-debug && cmake --build --preset macos-debug
export PATH="$PWD/build/macos-debug/tools/cli:$PATH"
```

(On Windows, `cmake --preset windows` and `cmake --build --preset
windows-debug`; the command lands beside the build either way.)

## A project

```sh
tynima new catcher
cd catcher
tynima run
```

A window opens with five crates falling onto a floor. That is the `basic`
template, and what it left you is four files:

```
catcher/
  tynima.toml     what to run, how big the window is, where the assets are
  src/game.lua    the game — all of it
  assets/         models, when you have some
  README.md
```

Leave `tynima run` going. Everything below is edited into `src/game.lua`
while it runs.

## What a game is

A game is a table with three functions in it, and the engine calls them:

```lua
local ty = require("tynima")

local game = {}

function game.load(reloaded) end   -- once, and again after every save
function game.update(dt) end       -- every frame; dt is its length in seconds
function game.unload(reloading) end -- before load() runs again

return game
```

`ty` is the engine. Everything it offers is in [the Lua
reference](lua-api.md), and underneath it is the same C table a game module
written in C receives ([the C reference](c-api.md)) — there is no layer in
between that can fall behind.

Replace `src/game.lua` with the smallest thing that runs:

```lua
local ty = require("tynima")
local game = {}
function game.load(reloaded)
    ty.log(reloaded and "reloaded" or "started")
end
function game.update(dt) end
return game
```

Save it. The window does not close; the log says `reloaded`, and you are
looking at an empty sky. Everything from here is adding to it.

## A floor, a light, and somewhere to look from

Nothing is drawn until something has a *model* and a *transform*. A model
can come from a file, but it can also come from a shape, which is how a
scene is built before there is any art:

```lua
function game.load(reloaded)
    local ground = ty.box(7, 0.25, 3)          -- half extents, in metres
    ty.entity {
        Name = { text = "ground" },
        Transform = { position = { 0, -0.25, 0 } },
        MeshRenderer = { model = ty.shape_model(ground, { 0.28, 0.30, 0.34 }, 0.9) },
    }

    ty.lighting { sun = { 0.4, 1.0, 0.3 }, intensity = 3.0, ambient = 0.20,
                  sky = { 0.07, 0.09, 0.13 } }
    ty.look_at({ 0, 5, 11 }, { 0, 2, 0 })      -- from here, at that
end
```

Save. A grey slab, lit from above and behind you.

`ty.entity` takes the components by name — `Name`, `Transform`,
`MeshRenderer` are three of the six the engine knows, and a game module
written in C++ can add its own, which a script can then read by name
without being told anything about them.

## A paddle you move yourself

Two kinds of thing move in a game: the ones the world moves, and the ones
you do. The paddle is yours, so it needs no physics at all — it is an
entity whose `Transform` you write every frame.

```lua
local state = {}
local SPEED = 7.0   -- metres a second
local REACH = 5.5   -- how far it may go either way

function game.load(reloaded)
    state = { x = 0 }
    -- ...the ground, as before...
    state.paddle = ty.entity {
        Name = { text = "paddle" },
        Transform = { position = { 0, 0.35, 0 } },
        MeshRenderer = { model = ty.shape_model(ty.box(1, 0.2, 1), { 0.95, 0.55, 0.2 }, 0.3) },
    }
end

function game.update(dt)
    local move = 0
    if ty.key_down("A") or ty.key_down("Left") then move = move - 1 end
    if ty.key_down("D") or ty.key_down("Right") then move = move + 1 end
    state.x = math.max(-REACH, math.min(REACH, state.x + move * SPEED * dt))
    ty.get(state.paddle, "Transform").position = ty.vec3(state.x, 0.35, 0)
end
```

`ty.get` hands you the component itself, not a copy of it: writing
`position` writes the world's own memory, and the renderer draws from it on
the same frame.

Save, and slide it about with A and D. The key names are the ones in
[`tynima_key`](c-api.md#tynima_key) — they are places on the keyboard, not
letters printed on it, so `A` is where A is on a QWERTY board whatever your
layout says.

## Cubes the world drops for you

A falling cube is the other kind of thing. Give it a *body* and the physics
world moves it; tie the body to an entity with a `RigidBody` component and
the entity's transform follows it every frame, with nothing else to write.

```lua
local EVERY = 0.9   -- seconds between cubes

local function drop()
    local x = (math.random() - 0.5) * 2 * REACH
    local falling = ty.body { shape = ty.box(0.3), position = { x, 8, 0 }, mass = 1 }
    state.cubes[#state.cubes + 1] = {
        body = falling,
        entity = ty.entity {
            Name = { text = "cube" },
            Transform = { position = { x, 8, 0 } },
            MeshRenderer = { model = state.cube_model },
            RigidBody = { body = falling },
        },
    }
end
```

with `state.cubes = {}` and a model to draw them with in `load`, and a
clock in `update`:

```lua
    state.next_cube = state.next_cube - dt
    if state.next_cube <= 0 then
        state.next_cube = EVERY
        drop()
    end
```

Save. Cubes rain past the paddle and fall through the floor, because
nothing here has told them not to — a body's shape is what it collides
with, and the floor was never given one.

## Catching them

Ask each cube where its body has got to, and take it out of the world as it
reaches the paddle's height. Near the paddle is a catch; anywhere else is a
miss.

```lua
local CATCH_Y = 0.9

    for i = #state.cubes, 1, -1 do
        local cube = state.cubes[i]
        local where = ty.body_transform(cube.body)
        if where.y < CATCH_Y then
            if math.abs(where.x - state.x) < 0.9 then
                state.caught = state.caught + 1
            else
                state.missed = state.missed + 1
            end
            ty.destroy(cube.entity)
            ty.destroy_body(cube.body)
            table.remove(state.cubes, i)
            ty.log(string.format("%d caught, %d missed", state.caught, state.missed))
        end
    end
```

Backwards through the list, because it is being taken apart as it is read.
An entity and its body are two things and both go: the entity is what was
drawn, the body is what was falling.

That is the game. What is left is what makes it keep working.

## Clearing up after yourself

A reload does not start the engine again — the window, the camera and the
world are all still standing, and `load()` runs a second time into them. A
game that only ever builds will have two floors after one save, and six
after five.

So keep what you made, and take it down in `unload`:

```lua
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

function game.unload()
    for _, e in ipairs(made.entities) do ty.destroy(e) end
    for _, b in ipairs(made.bodies) do ty.destroy_body(b) end
    made = { entities = {}, bodies = {} }
end
```

and call `entity` and `body` instead of `ty.entity` and `ty.body`
everywhere. Destroying something twice is not an error, which is what makes
a cube that was already caught safe to have on the list.

## When you have art

Drop a `.gltf` or `.glb` into `assets/` and ask for it by name:

```lua
MeshRenderer = { model = ty.model("assets/crate.glb") }
```

The first time, the engine cooks it — mesh, materials, textures and their
mip levels, into one `.tymodel` blob in a `.cooked` directory beside the
source — and loads that. After that it loads the blob, which is a read and
an upload and no parsing at all. Save the file from your modelling tool
while the game runs and the model is cooked again and swapped in, like the
script.

`tynima cook` does the same thing ahead of time for everything under
`assets/`, which is what you want before you ship: `tynima build` runs the
cook and then builds a native module if your project has one.

## The whole program

This is `src/game.lua` with all of the above in it. The engine's own test
suite runs this listing every time it builds, so what is on this page is
what works.

```lua first-game
-- catcher — a game in one file. A and D slide the paddle, Escape quits.
local ty = require("tynima")

local game = {}
local state = {}

local SPEED = 7.0    -- how fast the paddle slides, metres a second
local REACH = 5.5    -- how far it may go either way
local EVERY = 0.9    -- seconds between cubes
local CATCH_Y = 0.9  -- a cube below this has been caught, or missed

-- Everything this script made, so that a reload can take it down again
-- before building it anew: load() runs a second time into the world the
-- first one built.
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

function game.load(reloaded)
    state = { x = 0, caught = 0, missed = 0, next_cube = 0.5, cubes = {} }

    entity {
        Name = { text = "ground" },
        Transform = { position = { 0, -0.25, 0 } },
        MeshRenderer = { model = ty.shape_model(ty.box(7, 0.25, 3), { 0.28, 0.30, 0.34 }, 0.9) },
    }
    state.paddle = entity {
        Name = { text = "paddle" },
        Transform = { position = { 0, 0.35, 0 } },
        MeshRenderer = { model = ty.shape_model(ty.box(1, 0.2, 1), { 0.95, 0.55, 0.20 }, 0.3) },
    }
    state.cube_model = ty.shape_model(ty.box(0.3), { 0.35, 0.70, 0.95 }, 0.4)

    ty.lighting { sun = { 0.4, 1.0, 0.3 }, intensity = 3.0, ambient = 0.20,
                  sky = { 0.07, 0.09, 0.13 } }
    ty.look_at({ 0, 5, 11 }, { 0, 2, 0 })
    ty.log(reloaded and "catcher reloaded" or "catcher: A and D move the paddle, Escape quits")
end

function game.unload()
    for _, e in ipairs(made.entities) do ty.destroy(e) end
    for _, b in ipairs(made.bodies) do ty.destroy_body(b) end
    made = { entities = {}, bodies = {} }
end

-- A cube somewhere above, for the world to drop.
local function drop()
    local x = (math.random() - 0.5) * 2 * REACH
    local falling = body { shape = ty.box(0.3), position = { x, 8, 0 }, mass = 1 }
    state.cubes[#state.cubes + 1] = {
        body = falling,
        entity = entity {
            Name = { text = "cube" },
            Transform = { position = { x, 8, 0 } },
            MeshRenderer = { model = state.cube_model },
            RigidBody = { body = falling },
        },
    }
end

function game.update(dt)
    if ty.key_pressed("Escape") then return ty.quit() end

    -- The paddle is yours: nothing in the world moves it.
    local move = 0
    if ty.key_down("A") or ty.key_down("Left") then move = move - 1 end
    if ty.key_down("D") or ty.key_down("Right") then move = move + 1 end
    state.x = math.max(-REACH, math.min(REACH, state.x + move * SPEED * dt))
    ty.get(state.paddle, "Transform").position = ty.vec3(state.x, 0.35, 0)

    -- A cube every EVERY seconds; the falling is the world's business.
    state.next_cube = state.next_cube - dt
    if state.next_cube <= 0 then
        state.next_cube = EVERY
        drop()
    end

    -- Backwards, because the list is taken apart as it is read.
    for i = #state.cubes, 1, -1 do
        local cube = state.cubes[i]
        local where = ty.body_transform(cube.body)
        if where.y < CATCH_Y then
            if math.abs(where.x - state.x) < 0.9 then
                state.caught = state.caught + 1
            else
                state.missed = state.missed + 1
            end
            ty.destroy(cube.entity)
            ty.destroy_body(cube.body)
            table.remove(state.cubes, i)
            ty.log(string.format("%d caught, %d missed", state.caught, state.missed))
        end
    end
end

return game
```

## Where to go next

- `tynima new mygame --template platformer` and `--template fps` are two
  bigger games written the same way: a 2D platformer with a proper jump,
  and a first-person scene whose gun is a ray and an impulse.
- [The Lua reference](lua-api.md) is everything `ty` can do; [the C
  reference](c-api.md) is what it rests on, and what you would write a game
  module against if you wanted C or C++ instead of Lua.
- `apps/sandbox/scripts/pile.lua` in the engine's own repository is a
  larger script: a fly camera, impulses, and a pile of boxes to knock over.
