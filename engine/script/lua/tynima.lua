-- tynima — the engine, as a script sees it.
--
-- The other file beside this one, tynima_ffi.lua, is generated from
-- sdk/include/tynima.h and holds the C declarations; this is what a game is
-- written against. Every call here goes straight through LuaJIT's FFI to
-- the same tynima_api table a game module written in C receives, so there
-- is no binding layer in between to fall behind the engine.
--
--     local ty = require("tynima")
--     local game = {}
--     function game.load()
--       local ground = ty.shape_model(ty.box(8, 0.5, 8), {0.4, 0.4, 0.4})
--       ty.entity{ Transform = {position = {0, -0.5, 0}}, MeshRenderer = {model = ground} }
--     end
--     function game.update(dt)
--       if ty.key_down("Space") then ... end
--     end
--     return game
--
-- Components are reached by name and read as plain fields —
-- `ty.get(e, "Transform").position.y = 3` — including components the
-- engine has never heard of: a game module's own, described through
-- reflection, get a struct built for them the first time a script asks.
local ffi = require("ffi")
local C = require("tynima_ffi")

local ty = {}
ty.C = C

-- Two floats, which reflection knows about and the C header has no name
-- for, and the names the reflected structs are given.
ffi.cdef [[
typedef struct tynima_vec2 { float x, y; } tynima_vec2;
]]

-- The engine this script talks to, set once by the VM. A VM made without
-- one — a console, a test — leaves this in place, so a call says what is
-- missing instead of reading through a null pointer.
local unbound = setmetatable({}, {
    __index = function(_, name)
        error("there is no engine behind this script, so " .. tostring(name) .. " cannot be called", 3)
    end,
})
local api, engine = unbound, nil

-- ---------------------------------------------------------------- vectors

local vec3_t = ffi.typeof("tynima_vec3")
local vec4_t = ffi.typeof("tynima_vec4")
local quat_t = ffi.typeof("tynima_quat")
local sqrt = math.sqrt

--- A vector: ty.vec3(1, 2, 3), ty.vec3{1, 2, 3} or ty.vec3(other).
function ty.vec3(x, y, z)
    if type(x) == "table" then
        return vec3_t(x[1] or x.x or 0, x[2] or x.y or 0, x[3] or x.z or 0)
    end
    if type(x) == "cdata" then
        return vec3_t(x.x, x.y, x.z)
    end
    return vec3_t(x or 0, y or 0, z or 0)
end

function ty.vec4(x, y, z, w)
    if type(x) == "table" then
        return vec4_t(x[1] or x.x or 0, x[2] or x.y or 0, x[3] or x.z or 0,
                      x[4] or x.w or (x[4] == nil and x.w == nil and 1 or 0))
    end
    return vec4_t(x or 0, y or 0, z or 0, w == nil and 1 or w)
end

ffi.metatype(vec3_t, {
    __add = function(a, b) return vec3_t(a.x + b.x, a.y + b.y, a.z + b.z) end,
    __sub = function(a, b) return vec3_t(a.x - b.x, a.y - b.y, a.z - b.z) end,
    __unm = function(a) return vec3_t(-a.x, -a.y, -a.z) end,
    __mul = function(a, b)
        if type(a) == "number" then a, b = b, a end
        if type(b) == "number" then return vec3_t(a.x * b, a.y * b, a.z * b) end
        return vec3_t(a.x * b.x, a.y * b.y, a.z * b.z)
    end,
    __eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end,
    __tostring = function(v) return string.format("(%.3f, %.3f, %.3f)", v.x, v.y, v.z) end,
    __index = {
        length = function(v) return sqrt(v.x * v.x + v.y * v.y + v.z * v.z) end,
        dot = function(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end,
        cross = function(a, b)
            return vec3_t(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)
        end,
        normalized = function(v)
            local l = sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
            return l > 0 and vec3_t(v.x / l, v.y / l, v.z / l) or vec3_t(0, 0, 0)
        end,
        unpack = function(v) return v.x, v.y, v.z end,
    },
})

--- A rotation of `angle` radians about an axis.
function ty.quat(axis, angle)
    if axis == nil then
        return quat_t(0, 0, 0, 1)
    end
    local a = ty.vec3(axis):normalized()
    local half = (angle or 0) * 0.5
    local s = math.sin(half)
    return quat_t(a.x * s, a.y * s, a.z * s, math.cos(half))
end

--- Yaw about y, then pitch about x, then roll about z — radians.
function ty.euler(yaw, pitch, roll)
    local y = ty.quat({ 0, 1, 0 }, yaw or 0)
    local x = ty.quat({ 1, 0, 0 }, pitch or 0)
    local z = ty.quat({ 0, 0, 1 }, roll or 0)
    local function mul(a, b)
        return quat_t(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
    end
    return mul(mul(y, x), z)
end

ffi.metatype(quat_t, {
    __index = {
        --- The vector, turned by this rotation.
        rotate = function(q, v)
            v = ty.vec3(v)
            local u = vec3_t(q.x, q.y, q.z)
            local t = u:cross(v) * 2
            return v + t * q.w + u:cross(t)
        end,
    },
    __tostring = function(q) return string.format("(%.3f, %.3f, %.3f, %.3f)", q.x, q.y, q.z, q.w) end,
})

-- --------------------------------------------------------------- the world

-- The components the C header declares: a script gets the real struct, so
-- `t.position.y = 1` is a store to the world's own memory.
local builtin = {
    Name = "tynima_name",
    Transform = "tynima_transform",
    LocalToWorld = "tynima_local_to_world",
    Parent = "tynima_parent",
    MeshRenderer = "tynima_mesh_renderer",
    RigidBody = "tynima_rigid_body",
}

-- A component's struct, both ways round: one to read the world's memory
-- through, one to make a value of. (LuaJIT's core has no way back from a
-- pointer type to what it points at, so both are kept.)
local function ctypes_of(name)
    return { pointer = ffi.typeof(name .. "*"), value = ffi.typeof(name) }
end

-- What reflection's field kinds are, as C types. A kind this table has no
-- entry for is read as its own bytes.
local kind_type = {
    [C.TYNIMA_FIELD_BOOL] = "bool",
    [C.TYNIMA_FIELD_INT8] = "int8_t",
    [C.TYNIMA_FIELD_UINT8] = "uint8_t",
    [C.TYNIMA_FIELD_INT16] = "int16_t",
    [C.TYNIMA_FIELD_UINT16] = "uint16_t",
    [C.TYNIMA_FIELD_INT32] = "int32_t",
    [C.TYNIMA_FIELD_UINT32] = "uint32_t",
    [C.TYNIMA_FIELD_INT64] = "int64_t",
    [C.TYNIMA_FIELD_UINT64] = "uint64_t",
    [C.TYNIMA_FIELD_FLOAT] = "float",
    [C.TYNIMA_FIELD_DOUBLE] = "double",
    [C.TYNIMA_FIELD_VEC2] = "tynima_vec2",
    [C.TYNIMA_FIELD_VEC3] = "tynima_vec3",
    [C.TYNIMA_FIELD_VEC4] = "tynima_vec4",
    [C.TYNIMA_FIELD_QUAT] = "tynima_quat",
    [C.TYNIMA_FIELD_MAT4] = "tynima_mat4",
    [C.TYNIMA_FIELD_ENTITY] = "tynima_entity",
    [C.TYNIMA_FIELD_HANDLE] = "tynima_body",
}

local component_ids = {}   -- name -> id
local component_types = {} -- id -> {pointer = ctype, value = ctype}
local reflected_count = 0
local field_buffer = ffi.new("tynima_field[1]")
local info_buffer = ffi.new("tynima_component_info[1]")

--- The struct a component of this id is, built from what reflection says:
--- every field at its own offset, with the gaps between them filled, so the
--- layout is the engine's exactly. This is the whole of "bindings generated
--- from reflection": a component the script was never told about becomes a
--- struct the moment it is asked for.
local function reflected_type(id)
    local count = api.component_field_count(engine, id)
    if count == 0 or not api.component_info(engine, id, info_buffer) then
        return nil
    end
    local size = info_buffer[0].size
    local parts, cursor, pad = {}, 0, 0
    for i = 0, count - 1 do
        if api.component_field(engine, id, i, field_buffer) then
            local field = field_buffer[0]
            -- A named enum read out of a struct is cdata, not a number, and
            -- cdata indexes no table: the kind has to be asked for as one.
            local kind = tonumber(field.kind)
            if field.offset > cursor then
                parts[#parts + 1] = string.format("uint8_t _pad%d[%d];", pad, field.offset - cursor)
                pad, cursor = pad + 1, field.offset
            end
            local name = ffi.string(field.name)
            if kind == C.TYNIMA_FIELD_STRING then
                parts[#parts + 1] = string.format("char %s[%d];", name, field.count)
            elseif kind_type[kind] == nil then
                parts[#parts + 1] = string.format("uint8_t %s[%d];", name, field.size)
            else
                parts[#parts + 1] = string.format("%s %s;", kind_type[kind], name)
            end
            cursor = cursor + field.size
        end
    end
    if size > cursor then
        parts[#parts + 1] = string.format("uint8_t _pad%d[%d];", pad, size - cursor)
    end
    reflected_count = reflected_count + 1
    local type_name = string.format("tynima_reflected_%d", reflected_count)
    local declaration = string.format("typedef struct __attribute__((packed)) { %s } %s;",
                                      table.concat(parts, " "), type_name)
    local ok, err = pcall(ffi.cdef, declaration)
    if not ok then
        error(string.format("cannot describe component %d: %s (%s)", id, err, declaration), 0)
    end
    if ffi.sizeof(type_name) ~= size then
        error(string.format("component %d is %d bytes, its fields describe %d", id, size,
                            ffi.sizeof(type_name)), 0)
    end
    return ctypes_of(type_name)
end

--- The id of a component, by name. nil when nothing is registered under it.
function ty.component(name)
    local id = component_ids[name]
    if id ~= nil then
        return id
    end
    id = api.find_component(engine, name)
    if id == C.TYNIMA_NO_COMPONENT then
        return nil
    end
    component_ids[name] = id
    component_types[id] = builtin[name] and ctypes_of(builtin[name]) or reflected_type(id)
    return id
end

--- Every component the world knows, by name.
function ty.components()
    local names = {}
    for id = 0, api.component_count(engine) - 1 do
        if api.component_info(engine, id, info_buffer) then
            names[#names + 1] = ffi.string(info_buffer[0].name)
        end
    end
    return names
end

--- The component on an entity, as a struct to read and write in place; nil
--- when the entity is dead or has no such component.
function ty.get(entity, name)
    local id = ty.component(name)
    if id == nil then
        return nil
    end
    local pointer = api.get_component(engine, entity, id)
    if pointer == nil then
        return nil
    end
    local ctype = component_types[id]
    return ctype and ffi.cast(ctype.pointer, pointer)[0] or pointer
end

--- Fills a struct from a table: numbers and strings straight in, nested
--- tables into nested structs, {x, y, z} into a vector.
local function fill(target, values)
    for key, value in pairs(values) do
        if type(value) == "table" then
            local field = target[key]
            if type(field) == "cdata" and value[1] ~= nil then
                local n = #value
                if n >= 1 then field.x = value[1] end
                if n >= 2 then field.y = value[2] end
                if n >= 3 and pcall(function() return field.z end) then field.z = value[3] end
                if n >= 4 and pcall(function() return field.w end) then field.w = value[4] end
            else
                fill(field, value)
            end
        else
            target[key] = value
        end
    end
end
ty.fill = fill

--- An entity with the components named: ty.entity{ Transform = {...} }.
--- Order does not matter; a component the world does not know is an error,
--- so a typo is caught where it is written.
---
--- Anything with a Transform also gets a LocalToWorld, which is where the
--- engine keeps the matrix it works out from that Transform and whatever
--- parent it has — and what the renderer draws from. A script that wants
--- to set one itself may name it; naming it is what turns this off.
function ty.entity(components)
    components = components or {}
    if components.Transform ~= nil and components.LocalToWorld == nil then
        local copy = {}
        for name, fields in pairs(components) do
            copy[name] = fields
        end
        copy.LocalToWorld = {}
        components = copy
    end
    local ids, values, keep, count = {}, {}, {}, 0
    for name, fields in pairs(components) do
        local id = ty.component(name)
        if id == nil then
            error("no component called '" .. tostring(name) .. "' is registered", 2)
        end
        if not api.component_info(engine, id, info_buffer) then
            error("component '" .. tostring(name) .. "' has no description", 2)
        end
        local ctype = component_types[id]
        local value
        if ctype ~= nil then
            value = ffi.new(ctype.value)
            local defaults = api.component_defaults(engine, id)
            if defaults ~= nil then
                ffi.copy(value, defaults, info_buffer[0].size)
            end
            if type(fields) == "table" then
                fill(value, fields)
            end
        else
            value = ffi.new("uint8_t[?]", info_buffer[0].size)
        end
        count = count + 1
        ids[count] = id
        values[count] = ffi.cast("void*", value)
        keep[count] = value -- the array holds pointers; this holds the memory
    end
    local id_array = ffi.new("tynima_component_id[?]", math.max(count, 1), ids)
    local value_array = ffi.new("const void*[?]", math.max(count, 1), values)
    local entity = api.create_entity(engine, id_array, value_array, count)
    if entity.generation == 0 then
        error("the world is full: no entity was made", 2)
    end
    return entity
end

function ty.destroy(entity) return api.destroy_entity(engine, entity) end
function ty.alive(entity) return api.entity_alive(engine, entity) end
function ty.entity_count() return api.entity_count(engine) end

--- Adds or removes a component on an entity that is already there.
function ty.add(entity, name, fields)
    local id = ty.component(name)
    if id == nil or not api.component_info(engine, id, info_buffer) then
        return false
    end
    local ctype = component_types[id]
    local value = ctype and ffi.new(ctype.value) or ffi.new("uint8_t[?]", info_buffer[0].size)
    local defaults = api.component_defaults(engine, id)
    if defaults ~= nil and ctype then
        ffi.copy(value, defaults, info_buffer[0].size)
    end
    if type(fields) == "table" and ctype then
        fill(value, fields)
    end
    return api.add_component(engine, entity, id, ffi.cast("void*", value))
end

function ty.remove(entity, name)
    local id = ty.component(name)
    return id ~= nil and api.remove_component(engine, entity, id)
end

--- Every entity with all of the named components: ty.each("Transform",
--- "RigidBody", function(entity, transform, body) ... end). The callback
--- gets the entity and each component, in the order they were named.
function ty.each(...)
    local names = { ... }
    local fn = table.remove(names)
    if type(fn) ~= "function" then
        error("ty.each takes component names and then a function", 2)
    end
    local ids, types = {}, {}
    for i, name in ipairs(names) do
        local id = ty.component(name)
        if id == nil then
            return 0 -- nothing is registered under that name, so nothing has it
        end
        ids[i] = id
        types[i] = component_types[id]
    end
    local id_array = ffi.new("tynima_component_id[?]", math.max(#ids, 1), ids)
    local seen = 0
    local columns = {}
    local callback = ffi.cast("tynima_chunk_fn", function(_, entities, count, raw)
        for row = 0, count - 1 do
            for i = 1, #ids do
                local column = raw[i - 1]
                if types[i] ~= nil then
                    columns[i] = ffi.cast(types[i].pointer, column)[row]
                else
                    columns[i] = column
                end
            end
            seen = seen + 1
            fn(entities[row], unpack(columns, 1, #ids))
        end
    end)
    local ok, err = pcall(api.each_chunk, engine, id_array, #ids, callback, nil)
    callback:free()
    if not ok then
        error(err, 0)
    end
    return seen
end

-- --------------------------------------------------------------- the input

local key_cache = {}
local function key_of(name)
    local key = key_cache[name]
    if key == nil then
        key = C["TYNIMA_KEY_" .. name]
        key_cache[name] = key
    end
    return key
end

--- ty.key_down("W"), ty.key_pressed("Space"): the names are the ones in
--- tynima.h, which are where the key is on the board, not what is on it.
function ty.key_down(name) return api.key_down(engine, key_of(name)) end
function ty.key_pressed(name) return api.key_pressed(engine, key_of(name)) end
function ty.key_released(name) return api.key_released(engine, key_of(name)) end

local buttons = { left = C.TYNIMA_MOUSE_LEFT, middle = C.TYNIMA_MOUSE_MIDDLE, right = C.TYNIMA_MOUSE_RIGHT }
function ty.mouse_down(button) return api.mouse_down(engine, buttons[button or "left"]) end
function ty.mouse_pressed(button) return api.mouse_pressed(engine, buttons[button or "left"]) end
function ty.mouse_released(button) return api.mouse_released(engine, buttons[button or "left"]) end

local float2 = ffi.new("float[2]")
function ty.mouse_position()
    api.mouse_position(engine, float2, float2 + 1)
    return float2[0], float2[1]
end
function ty.mouse_delta()
    api.mouse_delta(engine, float2, float2 + 1)
    return float2[0], float2[1]
end
function ty.mouse_wheel()
    api.mouse_wheel(engine, float2, float2 + 1)
    return float2[0], float2[1]
end
function ty.relative_mouse(enabled) api.set_relative_mouse(engine, enabled and true or false) end

local uint2 = ffi.new("uint32_t[2]")
function ty.window_size()
    api.window_size(engine, uint2, uint2 + 1)
    return uint2[0], uint2[1]
end

-- ------------------------------------------------------- the view and light

local camera_buffer = ffi.new("tynima_camera[1]")

--- The camera, as a struct to read and change in place. Set its fields and
--- the change is the engine's: ty.camera().position = ty.vec3(0, 2, 5).
function ty.camera()
    api.get_camera(engine, camera_buffer)
    return setmetatable({}, {
        __index = function(_, key) return camera_buffer[0][key] end,
        __newindex = function(_, key, value)
            if key == "position" then
                camera_buffer[0].position = ty.vec3(value)
            elseif key == "rotation" then
                camera_buffer[0].rotation = value
            else
                camera_buffer[0][key] = value
            end
            api.set_camera(engine, camera_buffer)
        end,
    })
end

--- Points the camera at something, from where it is.
function ty.look_at(from, target, fov_y)
    from, target = ty.vec3(from), ty.vec3(target)
    local forward = (target - from):normalized()
    local yaw = math.atan2(-forward.x, -forward.z)
    local pitch = math.asin(math.max(-1, math.min(1, forward.y)))
    camera_buffer[0].position = from
    camera_buffer[0].rotation = ty.euler(yaw, pitch, 0)
    if fov_y then camera_buffer[0].fov_y = fov_y end
    api.set_camera(engine, camera_buffer)
end

local lighting_buffer = ffi.new("tynima_lighting[1]")

--- The sun and the sky: ty.lighting{ sun = {0, 1, 0.3}, intensity = 4 }.
--- With nothing given, what they are now, as a table.
function ty.lighting(values)
    api.get_lighting(engine, lighting_buffer)
    if values == nil then
        local l = lighting_buffer[0]
        return {
            sun = ty.vec3(l.sun_direction), intensity = l.sun_intensity,
            color = ty.vec3(l.sun_color), ambient = l.ambient, sky = ty.vec3(l.sky),
        }
    end
    if values.sun then lighting_buffer[0].sun_direction = ty.vec3(values.sun):normalized() end
    if values.intensity then lighting_buffer[0].sun_intensity = values.intensity end
    if values.color then lighting_buffer[0].sun_color = ty.vec3(values.color) end
    if values.ambient then lighting_buffer[0].ambient = values.ambient end
    if values.sky then lighting_buffer[0].sky = ty.vec3(values.sky) end
    api.set_lighting(engine, lighting_buffer)
end

--- This frame's point lights: a list of { position = , radius = , color = }.
function ty.lights(list)
    list = list or {}
    local count = #list
    if count == 0 then
        api.set_point_lights(engine, nil, 0)
        return
    end
    local array = ffi.new("tynima_point_light[?]", count)
    for i, light in ipairs(list) do
        array[i - 1].position = ty.vec3(light.position or light[1] or { 0, 0, 0 })
        array[i - 1].radius = light.radius or light[2] or 1
        array[i - 1].color = ty.vec3(light.color or light[3] or { 1, 1, 1 })
    end
    api.set_point_lights(engine, array, count)
end

-- ------------------------------------------------------ models and bodies

--- A shape: ty.box(1, 2, 1), ty.sphere(0.5), ty.capsule(0.3, 0.6).
function ty.box(x, y, z)
    local shape = ffi.new("tynima_shape")
    shape.type = C.TYNIMA_SHAPE_BOX
    shape.half_extents = ty.vec3(x, y or x, z or x)
    shape.radius, shape.half_height = 0.5, 0.5
    return shape
end

function ty.sphere(radius)
    local shape = ty.box(0.5)
    shape.type = C.TYNIMA_SHAPE_SPHERE
    shape.radius = radius or 0.5
    return shape
end

function ty.capsule(radius, half_height)
    local shape = ty.sphere(radius)
    shape.type = C.TYNIMA_SHAPE_CAPSULE
    shape.half_height = half_height or 0.5
    return shape
end

--- A model of one shape and one plain colour, to build a scene with before
--- there is any art. Returns the index a MeshRenderer draws.
function ty.shape_model(shape, color, roughness)
    local index = api.shape_model(engine, shape, ty.vec4(color or { 0.8, 0.8, 0.8, 1 }), roughness or 0.6)
    if index == C.TYNIMA_NO_MODEL then
        error("that shape could not be made into a model", 2)
    end
    return index
end

--- A model from a file: a cooked .tymodel, or a .gltf/.glb cooked on the way in.
function ty.model(path)
    local index = api.load_model(engine, path)
    if index == C.TYNIMA_NO_MODEL then
        error("cannot load the model '" .. tostring(path) .. "'", 2)
    end
    return index
end

local motions = { static = C.TYNIMA_MOTION_STATIC, kinematic = C.TYNIMA_MOTION_KINEMATIC,
                  dynamic = C.TYNIMA_MOTION_DYNAMIC }

--- A physics body: ty.body{ shape = ty.box(1), position = {0, 3, 0}, mass = 2 }.
--- `motion` is "static", "kinematic" or "dynamic" (the default).
function ty.body(values)
    values = values or {}
    local desc = ffi.new("tynima_body_desc")
    desc.shape = values.shape or ty.box(0.5)
    desc.position = ty.vec3(values.position or { 0, 0, 0 })
    desc.rotation = values.rotation or ty.quat()
    desc.motion = motions[values.motion or "dynamic"] or C.TYNIMA_MOTION_DYNAMIC
    desc.mass = values.mass or 0
    desc.friction = values.friction or 0.5
    desc.restitution = values.restitution or 0
    desc.linear_velocity = ty.vec3(values.velocity or { 0, 0, 0 })
    desc.angular_velocity = ty.vec3(values.spin or { 0, 0, 0 })
    desc.start_active = values.start_active ~= false
    desc.lock_rotation = values.lock_rotation == true
    desc.user_data = values.user_data or 0
    local body = api.create_body(engine, desc)
    if body.generation == 0 then
        error("no body was made: the physics world is full, or there is none", 2)
    end
    return body
end

function ty.destroy_body(body) return api.destroy_body(engine, body) end

local vec3_buffer = ffi.new("tynima_vec3[2]")
local quat_buffer = ffi.new("tynima_quat[1]")

--- Where a body is: position, rotation. nil for a body that is gone.
function ty.body_transform(body)
    if not api.body_transform(engine, body, vec3_buffer, quat_buffer) then
        return nil
    end
    return ty.vec3(vec3_buffer[0]), quat_t(quat_buffer[0])
end

--- How fast a body is going: linear, angular. nil for a body that is gone.
function ty.body_velocity(body)
    if not api.body_velocity(engine, body, vec3_buffer, vec3_buffer + 1) then
        return nil
    end
    return ty.vec3(vec3_buffer[0]), ty.vec3(vec3_buffer[1])
end

function ty.set_body_transform(body, position, rotation)
    api.body_set_transform(engine, body, ty.vec3(position), rotation or ty.quat())
end

function ty.set_body_velocity(body, linear, angular)
    api.body_set_velocity(engine, body, ty.vec3(linear or { 0, 0, 0 }), ty.vec3(angular or { 0, 0, 0 }))
end

function ty.impulse(body, impulse, point)
    if point == nil then
        api.body_add_impulse(engine, body, ty.vec3(impulse))
    else
        api.body_add_impulse_at(engine, body, ty.vec3(impulse), ty.vec3(point))
    end
end

local hit_buffer = ffi.new("tynima_ray_hit[1]")

--- The nearest body along a ray. Returns a table of what it met, or nil.
function ty.cast_ray(origin, direction, max_distance)
    if not api.cast_ray(engine, ty.vec3(origin), ty.vec3(direction):normalized(), max_distance or 1000,
                        hit_buffer) then
        return nil
    end
    local hit = hit_buffer[0]
    return {
        body = hit.body, position = ty.vec3(hit.position), normal = ty.vec3(hit.normal),
        distance = hit.distance, user_data = hit.user_data,
    }
end

-- ---------------------------------------------------------------- the rest

local levels = { trace = C.TYNIMA_LOG_TRACE, debug = C.TYNIMA_LOG_DEBUG, info = C.TYNIMA_LOG_INFO,
                 warn = C.TYNIMA_LOG_WARN, error = C.TYNIMA_LOG_ERROR }

--- A line in the engine's log, in the "game" category.
function ty.log(message, level)
    api.log(engine, levels[level or "info"] or C.TYNIMA_LOG_INFO, tostring(message))
end

function ty.time() return api.time_seconds(engine) end
function ty.quit() api.quit(engine) end
function ty.api_version() return api.version end

--- The engine behind this script, for a game that wants the raw table.
function ty.raw() return api, engine end

--- Called once by the VM before any script runs: the two pointers the
--- engine handed it become the api and the context every call above uses.
function ty.bind(api_pointer, engine_pointer)
    -- A reload starts over: ids and reflected structs are the old world's.
    component_ids, component_types = {}, {}
    local pointer = api_pointer ~= nil and ffi.cast("const tynima_api*", api_pointer) or nil
    if pointer == nil then
        api, engine = unbound, nil
        return ty
    end
    if pointer.version < 9 then
        error(string.format("this engine speaks API version %d; scripts need 9", tonumber(pointer.version)), 0)
    end
    api = pointer
    engine = ffi.cast("tynima_engine*", engine_pointer)
    return ty
end

--- Whether there is an engine behind this script at all.
function ty.bound() return api ~= unbound end

return ty
