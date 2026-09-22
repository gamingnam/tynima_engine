# Tynima Engine

A 2D/3D game engine built from scratch, designed for tile-based, unified-memory
GPUs first (Apple Silicon) and discrete PC GPUs second, with a C-ABI SDK so games
and the editor are built on the same public surface.

Personal project. The plan, phase by phase, lives in the
[roadmap](https://claude.ai/code/artifact/2250a38b-e371-4824-a78b-5e0df0672165).

## Layout

Dependencies point down only. Each module's `CMakeLists.txt` declares what it
may depend on, and `tools/check_layering.py` fails CI when a `#include` reaches
past that declaration.

| Module            | Depends on                           | Responsibility                                              |
| ----------------- | ------------------------------------ | ----------------------------------------------------------- |
| `editor/`         | `sdk` (and Dear ImGui's headers)     | The editor — a client of the SDK, no special privileges     |
| `sdk/`            | everything below                     | `tynima.h` C ABI and the runtime behind its host half       |
| `engine/script/`  | `core platform scene assets`         | LuaJIT: the VM a game's scripts run in                      |
| `engine/cooker/`  | `core platform render assets`        | Offline: glTF import, mesh ordering, mip chains, the cook   |
| `engine/assets/`  | `core platform rhi render`           | Runtime loading of cooked blobs, GPU upload, file watching  |
| `engine/scene/`   | `core render physics`                | Archetype ECS, transforms, the systems that drive the rest  |
| `engine/physics/` | `core`                               | Rigid bodies behind `PhysicsWorld`: Jolt now, ours next     |
| `engine/ui/`      | `core platform rhi`                  | Dear ImGui, fed by `platform` and drawn through `rhi`       |
| `engine/render/`  | `core platform rhi`                  | Frame graph, the scene renderer, materials, the post stack  |
| `engine/rhi/`     | `core platform`                      | Render hardware interface — SDL3 GPU and native Metal       |
| `engine/platform/`| `core`                               | Window, input, filesystem, time, threads (wraps SDL3)       |
| `engine/core/`    | —                                    | Allocators, containers, math, jobs, logging, reflection     |
| `apps/sandbox/`   | any engine module                    | Engine developer's playground — a test bed, not a template  |
| `tools/cook/`     | `core platform assets cooker`        | `tynima-cook`: the asset cooker's command line              |
| `tools/golden/`   | `core platform render scene cooker sdk` | `tynima-golden`: the renderer's golden image tests       |
| `apps/sandbox/game/` | `tynima.h` only (headers of sdk/scene/physics/core) | The sandbox's hot-reloadable game module    |

Transitive dependencies don't count: if a file includes `tynima/rhi/...`, its
module must declare `rhi`. Nothing may depend on an app, and `editor` may depend
on `sdk` alone. Third-party headers have one home each: SDL3 in `platform`
and `rhi`, Jolt in `physics`, cgltf, stb and meshoptimizer in `cooker`, Dear
ImGui in `ui` and the editor.

## Build

Requirements: CMake 3.28+, a C++20 compiler, Ninja on macOS, Visual Studio 2022+
on Windows. The first configure fetches SDL3, doctest, Jolt and the rest from
GitHub as pinned tarballs.

```sh
brew install cmake ninja            # macOS, once
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
./build/macos-debug/apps/sandbox/tynima-sandbox
./build/macos-debug/editor/tynima-editor --model build/macos-debug/assets/WaterBottle.glb
./build/macos-debug/tools/cook/tynima-cook build/macos-debug/assets/ -o cooked/
./build/macos-debug/tools/golden/tynima-golden          # the renderer's golden image tests
./build/macos-debug/apps/sandbox/tynima-sandbox --script apps/sandbox/scripts/pile.lua
```

The sandbox loads a glTF model (a CC0 Khronos sample, downloaded into
`build/<preset>/assets/` at configure time, cooked beside itself on the first
run — see [Cooked assets](#cooked-assets-and-hot-reload)) with its base color texture,
draws it with a reverse-Z depth buffer through SDL3 GPU with its Metal shaders
compiled at runtime — sRGB textures sampled through sRGB formats, lighting in
linear into a 16-bit float target, bloomed, anti-aliased and tonemapped onto
an sRGB-encoded swapchain by the post stack — and lets you fly around it: hold the right mouse button to look, W/A/S/D to move, Q/E
to descend and climb, Shift to run, Escape to quit. `F` puts the camera
behind the character instead: then W/A/S/D walk it, Space jumps, Shift
runs, and it can shove bottles about, push the gate open and swing the
hanging chain. `1`/`2`/`3` switch between
unlit, Blinn-Phong and Cook-Torrance shading; `N`/`M`/`O`/`V`/`B` show the
mapped normals, metallic/roughness, occlusion, vertex normals and tangents;
`C` tints the picture by shadow cascade and `X` turns shadows off; `K`
shows how many point lights each cluster holds and `P` turns the hundred of
them off; `G` toggles bloom, `T` cycles the tonemapper (off, ACES, AgX) and
`H` the anti-aliasing (off, FXAA, TAA); the arrow keys move the sun. `--model path.glb` loads
something else. `--headless --frames N` runs the same loop with no window and
no GPU (the model still loads) with a scripted player at the keyboard, which
is what the `sandbox_headless` CTest does on CI. `--rhi sdl` draws through SDL GPU
instead of the native Metal backend; `--shading fused|split|forward` picks
how the scene is lit (see The tile, below). `--record run.tyrec` writes
every frame's input and frame time, and the physics world's hash at the end,
to a text log; `--replay run.tyrec` plays it back in place of the clock and
the keyboard and exits 3 if the run does not end on the same hash — see
[Determinism](#determinism) below.

GPU notes:

- SDL's GPU backends hang off the video driver, so there is no GPU device in
  headless mode. The `rhi` tests use the real driver where a display exists
  (they open a 320×240 window for a moment) and degrade to "failure is
  explained" where there is none.
- Windows builds and runs the sandbox, but draws nothing until the shader
  pipeline can produce DXIL/SPIR-V (SDL_shadercross) — the engine only carries
  MSL source today, and the sandbox says so on stdout.

On Windows: `cmake --preset windows`, then `cmake --build --preset windows-debug`
and `ctest --preset windows-debug`.

Options (`-D` at configure time): `TYNIMA_BUILD_TESTS`, `TYNIMA_BUILD_EDITOR`,
`TYNIMA_BUILD_APPS`, `TYNIMA_FETCH_SDL3`, `TYNIMA_WARNINGS_AS_ERRORS` (on in CI).

## The editor

`tynima-editor` is the first tool, and the rule it lives by is the one the
roadmap set for it: it is a client of the SDK with no special privileges.
It includes [`tynima.h`](sdk/include/tynima.h) and Dear ImGui's headers and
nothing else — `tools/check_layering.py` fails the build otherwise — so
anything the editor can do, a game's runtime can do through the same header.
When the editor needed something the header lacked, the header grew: that is
how the SDK gets complete.

Four panels, docked (the layout is kept in `tynima-editor.ini`; View > Reset
layout restores it):

- **Hierarchy** — every entity, under its `Parent`, by its `Name` or as
  "entity N". Click to select.
- **Inspector** — the selected entity's components, as the world lists them,
  every field of every one drawn from the description the engine keeps (see
  [Reflection](#reflection)): the editor knows no component's layout, not
  even `Transform`'s. Each kind of field gets its widget — a quaternion is
  edited as yaw, pitch and roll, an entity offers to select what it points
  at, a string has its capacity, read-only fields are shown greyed and
  hidden ones not at all. Editing the transform of an entity with a
  `RigidBody` teleports the body along, since the body drives the transform
  every frame. Every edit goes on an undo stack: Cmd+Z and Cmd+Shift+Z, or
  the Edit menu, which counts what is there. An undo is the component's
  bytes put back, so a component whose entity has since gone is skipped.
  (Making and deleting entities, adding and removing components are not
  undoable yet.)
- **Viewport** — the scene, drawn by the engine into a texture the panel
  shows at its own size (`tynima_ui_scene_texture`); the window behind is
  cleared. Hold the right mouse button over it to look, W/A/S/D and Q/E to
  fly, Shift to hurry, the wheel to change the pace. Click to select what
  is under the mouse (its bounds are outlined), then move, rotate or scale
  it with the gizmo — W, E and R switch, Shift snaps to 10 cm, 15° or a
  tenth; a click on nothing deselects.
- **Console** — the engine's log, the last 2048 events, filtered by level and
  text; a second combo sets what the engine logs at all.
- **Stats** — frames per second, CPU and GPU time, entities and bodies, the
  frame graph's passes and what never left the tile, and the frame's heap
  allocations from engine code (the rule says zero, and the editor's panels
  are not the engine's).

The Render menu picks the shading path (forward, fused deferred, split
deferred), model, debug view, shadows, point lights, bloom, tonemapper and
anti-aliasing — the same choices the sandbox's keys make.
`--game path.so` loads a game module, hot-reloaded as in the sandbox;
`--headless --frames N` draws the panels without a display, which is what the
`editor_headless` CTest does.

### Reflection

A component lists its fields beside its definition, and that list is what
the inspector draws, what a scene file will write and what a script binding
will read — instead of the C++ type, which none of them can see:

```cpp
struct Transform {
    math::Vec3 position;
    math::Quat rotation;
    math::Vec3 scale;
};
TY_REFLECT(Transform, TY_FIELD(position), TY_FIELD(rotation), TY_FIELD(scale));
```

Macros, not codegen ([`core/reflect.h`](engine/core/include/tynima/core/reflect.h)):
no build step, nothing generated to check in, and the compiler still checks
every name — a field that does not exist fails to compile, and its kind,
size and offset come from `decltype` and `offsetof`, never from a second
copy. The cost is that the list is written by hand; it is one line. A
field's kind comes from a `FieldTraits` specialization for its type (the
numbers, the vectors, quaternions and matrices, handles, entities, `char`
arrays as strings) and anything else is opaque bytes; `TY_FIELD_FLAGS`
marks a field read-only or hidden.

The World keeps the fields beside the layout when a component is registered
(`register_component` copies both, names included — a game module's strings
do not survive its reload, the World does) and the C API hands them out:
`component_field_count`, `component_field`. A module describes its own
components the other way, `describe_component`, and for one written in C++
[`sdk/reflect.h`](sdk/include/tynima/sdk/reflect.h) does it from the same
`TY_REFLECT` list. The sandbox's game module does exactly that: its launch
speed and spread are no longer constants but a `Launcher` component on a
"launcher" entity, which the module finds again after a reload and which
`tynima-editor --game build/macos-debug/apps/sandbox/game/sandbox_game.so`
shows and edits like any other — an editor that has never heard of it.

### Picking and gizmos

A click in the viewport asks the engine what is under it: `tynima_pick`
casts a ray against every drawable's model bounds — each box in its own
model space, the ray taken there through the inverse of the entity's world
matrix, which keeps the distance along the ray the same on both sides —
and answers with the nearest entity and how far it is. Bounds, not
triangles: right for a click, not for a bullet. The ray itself comes from
`tynima_camera_ray`, and `tynima_camera_project` goes the other way; both
are inline in the header, the same projection the engine draws with, so a
host needs no matrix library to draw over the picture.

The gizmos are the editor's own, drawn with ImGui's draw list over the
viewport: three arrows to move along the world's axes, three circles to
rotate about them, three squares to scale along the entity's own axes and
one in the middle for all three at once. A handle is picked by its distance
on screen from the mouse, and a drag is solved in the world: for an arrow,
the point on the axis nearest the mouse's ray; for a circle, where the ray
meets the axis's plane and the angle it makes there; a move or a turn is
then taken into the entity's parent's space, so a child of a turned parent
still goes where the mouse points. The whole drag is one edit on the undo
stack, and a body follows its transform as it moves.

### Scene files

A scene is a text file, in TOML — the git-native pillar of the plan, in
its first form:

```toml
# Tynima scene
version = 1

[[entities]]
[entities.Name]
text = "floor"
[entities.Transform]
position = [0.0, -0.25, 0.0]
rotation = [0.0, 0.0, 0.0, 1.0]
scale = [1.0, 1.0, 1.0]
[entities.LocalToWorld]
[entities.MeshRenderer]
model = 1
visible = true
```

One entity after another, a table per component, a line per field, in an
order that never changes for the same world, so version control diffs and
merges it line by line. It is written from the fields reflection describes
([`scene/scene_file.h`](engine/scene/include/tynima/scene/scene_file.h)),
so no component needs code of its own — a game module's component saves
like the engine's, and one nobody described is saved as its name alone and
loaded with its defaults. Numbers are written with the fewest digits that
read back to the same value (`std::to_chars`), so a value saved and loaded
is the same bits, and a file saved again is the same text: the scene tests
hold every kind of field to that round trip. Read-only and hidden fields
are derived or runtime state and are not saved; nor are handles — a
physics body means nothing in another run (recreating bodies from a scene
is for the physics description that a later phase adds). Entities are
numbered by their position in the file and an entity-valued field
(`Parent`) names one by that number; the numbers are given again on the
next save.

The reader takes the subset of TOML the writer produces and refuses the
rest with the line number, and it reads and checks the whole file before
it touches the world, so a bad file changes nothing. Every component in a
file must be registered before the file is loaded — the module that
defines one goes in first — and the error says so by name.

In the editor: File > New, Open, Save and Save as (Cmd+O, Cmd+S,
Cmd+Shift+S; a path typed into a small dialog, since there is no native
one yet), `--scene file.toml` to open one at start; entities are made and
deleted from the hierarchy, components added from their defaults and
removed from the inspector. `tynima-sandbox --save-scene pile.toml` writes
the pile as it ended, which the editor opens with
`--game …/sandbox_game.so` (for its `Launcher` component) — 108 entities,
every bottle where it fell. Through the header:
`tynima_save_scene`, `tynima_load_scene`, `tynima_clear_scene`.

### The host half of tynima.h

Up to Phase 4 the C API was what a game module receives: a table for the
world, the keys, time and logging. Version 4 adds what a *host* needs — the
program that runs the engine, be it a game's runtime or the editor:
`tynima_engine_create()` brings up the platform, the window, the GPU, the
world, the physics, the game module and the UI; `tynima_engine_begin_frame()`
pumps events and settles the clock and the input; `tynima_engine_end_frame()`
runs the module, steps the simulation at 60 Hz, renders and presents. Between
the two, the host draws its Dear ImGui panels (the engine owns the context)
and reads or changes the world through the same table a module gets, which
grew component queries (`find_component`, `component_info`,
`entity_components`), the mouse, and body poses. Models load with
`tynima_load_model`; the camera and the render settings have getters and
setters; the log ring and the statistics feed the console and stats panels.
The built-in components are declared as C structs (`tynima_transform`, ...)
whose layouts the engine checks against its own when it is built, and a few
inline vector and quaternion helpers keep a C host from needing a math library.

### The ABI, and the audit that keeps it

Phase 6 starts by settling what that header promises. Three things came out
of reading it as a stranger would.

It was not self-contained: it included `<tynima/platform/keys.def>` for the
key list, so a game outside this tree could not compile the one file it is
supposed to need. The list is inlined now, and `sdk/src/api.cpp` asserts it
against the engine's own, key by key, so the copy cannot drift.

It had no way to say no to a module built against an older version: the
loader demanded an exact match, which would break every existing game on
each release even though the table only ever grows at the end. A module now
loads when its version is between `TYNIMA_API_VERSION_MIN` and the engine's
own, and is refused with which of the two to rebuild when it is not.

And nothing enforced the promises. [`sdk/src/abi.cpp`](sdk/src/abi.cpp) is a
file of nothing but `static_assert`: the size of the table, the offset of the
first entry of every version's block, the size and every field offset of
every struct that crosses the boundary, and that `tynima_game` — which the
engine reads out of a module built against an older header — never grows.
Inserting a function into the middle of the table fails the build with the
number that changed, instead of shipping and reading the wrong entry.
`sdk/tests/c_abi.c` compiles the header as C11 with warnings as errors,
twice in one translation unit, and uses it the way a game in C would; a test
checks its answers against the C++ side's.

The other half of the audit is the editor. It is a client of `tynima.h` and
nothing else, and `tools/check_layering.py` now enforces exactly that
(`PUBLIC_API_ONLY`) rather than merely that it depends on `sdk` — so a panel
that wants something the header lacks is a hole in the header. Reading the
editor and the sandbox side by side turned up what a game could not do at
all: move the camera, light the scene, make a model without an asset file,
create a physics body, cast a ray, size the window, or quit. All of that was
host-only C++. Version 9 of the table adds it, as one block at the end, and
a test builds a whole scene — ground, a falling ball, lights, a ray that
finds it — through the table alone, which is what a game module gets.

Behind the C functions is `sdk::Runtime` ([`sdk/include/tynima/sdk/runtime.h`](sdk/include/tynima/sdk/runtime.h)),
the same object the sandbox drives directly: everything `apps/sandbox`'s
`main()` did by hand through Phase 4 — the window, the device, the frame
graph, the scene renderer and post stack, the game module, the fixed-step
physics, the frame arena and the heap rule, replay and record — now lives
there, and the sandbox is a client of it that keeps its own scene, its keys
and its reports. The golden replays under `apps/sandbox/replays/` still end on
the same hashes, so the move changed nothing about the simulation.

### Cooked assets and hot reload

A shipping build never parses glTF or decodes a PNG. The cooker
([`engine/cooker`](engine/cooker)) does that once, offline, and writes a
*cooked model* (`.tymodel`, [`engine/assets/include/tynima/assets/model_blob.h`](engine/assets/include/tynima/assets/model_blob.h)):
the vertex buffer in the one format the pipelines read, 32-bit indices, the
submeshes and materials, and every texture with its whole mip chain, RGBA8
as the device samples it. Loading a blob is reading the file and a copy per
buffer — the water bottle, 8.9 MB of glTF that took over a second to import,
loads in 30 ms from its 90 MB blob, and in a Debug build at that. The bytes
between: the mesh goes through meshoptimizer (identical vertices welded,
each submesh's triangles ordered for the post-transform cache and then
against overdraw, the vertices ordered for fetch), and the mip levels are
box-filtered in linear light through a table, so an sRGB texture's distant
levels are not darker than its near ones and the bytes are the same on every
platform. The reader checks every count, offset and index against the file
before it copies anything, so a truncated or edited blob is a message, not a
GPU reading past a buffer, and refuses a blob of another version.

`tynima-cook` ([`tools/cook`](tools/cook)) cooks files or whole directories
(`-o` for where; by default a `.cooked/` directory beside each source, which
`.gitignore` knows), leaves alone a blob newer than its source, and with
`--watch` keeps going. The runtime does the same on its own: `tynima_load_model`
takes a `.tymodel` as it is, or a `.gltf`/`.glb` that it cooks first — unless
its blob is already newer — so the editor's `--model WaterBottle.glb` works as
before, cooking on the first run and loading the blob on every run after.
Either way the file is watched (an `assets::FileWatch`: a stat per file every
quarter second, reported once the write time has held still for another — an
exporter writing a file in pieces is not read in pieces), and when it changes
the model is loaded again into the same index: every entity drawing it shows
the new one, and `tynima_stats.model_reloads` counts it. Export from Blender
over the file, or run `tynima-cook` into the blob the editor has open, and it
changes in the viewport without a restart — the other half of the game
module's reload. What the format leaves room for next is a block-compressed
texture format (BC7): the same chain at a quarter of the bytes, which needs
an encoder in the cooker and the format in both backends.

### Golden images

The renderer's regression tests are pictures. `tynima-golden`
([`tools/golden`](tools/golden)) builds each of its scenes through the
runtime — the shapes through the forward, fused and split paths, a data view,
the clustered point lights, TAA and FXAA, and the water bottle near to far
when the sample is there — draws it offscreen for a few frames (a
`RuntimeDesc::offscreen` runtime: a GPU but no window, the picture read back
from the viewport texture) and compares the 480×270 result with the golden
kept under [`tools/golden/goldens/`](tools/golden/goldens). The comparison is
perceptual ([`engine/cooker/include/tynima/cooker/image_diff.h`](engine/cooker/include/tynima/cooker/image_diff.h)):
each pixel of both images goes to OKLab, where distance is close to how
different two colours look; a pixel differs when that distance is over 0.02,
about the least a person notices, unless a pixel within one of it in the other
image is close enough — an edge a GPU rasterised half a pixel over is not a
regression, a region of wrong colour is. A scene passes when fewer than half
a percent of its pixels differ and the mean distance stays under 0.004, so
another GPU's rounding passes and a broken pass fails. Every render lands in
`build/<preset>/golden/`, and a failure leaves a strip beside it: golden,
render and a red heatmap of where they part. `ctest` runs it where a GPU is
(and skips it where none is, as in a sandbox or on the Windows runner until
shaders exist for it); CI keeps the renders as an artifact. After a deliberate
change to the picture, `tynima-golden --update` writes new goldens — look at
them before committing, since they are the definition of right.

### The UI

`engine/ui` hosts Dear ImGui (the docking branch, pinned by tarball like the
rest). ImGui's own platform and renderer backends are not used: the layer
feeds ImGui from `platform`'s input snapshot and events (keys, mouse, wheel,
typed text — which needed `TextInput` events, text-input mode, OS cursors and
the clipboard added to `platform`), and draws its lists through `rhi`, which
grew per-target blend modes, a scissor rectangle and a packed-colour vertex
format for it. It is a 1.92-style backend: ImGui asks for textures (its font
atlas, growing as glyphs are used) and the layer creates, updates and frees
them through the device. The UI is one more pass in the frame graph, over the
picture — or over a cleared window when the picture went to the editor's
viewport texture, which ImGui then shows like any image.

## Games in Lua

A script is a game module written in Lua, and it reaches the engine the
same way a C one does: through the `tynima_api` table. LuaJIT's FFI calls C
from declarations rather than from bindings, so there are no bindings to
write — [`tools/gen_lua.py`](tools/gen_lua.py) turns
[`sdk/include/tynima.h`](sdk/include/tynima.h) into
[`engine/script/lua/tynima_ffi.lua`](engine/script/lua/tynima_ffi.lua), every
typedef, enum and the table itself, and a test regenerates it and fails when
it has drifted from the header. The calls are JIT-compiled; there is no
marshalling layer in between, and the sizes LuaJIT computes are the ones
[`sdk/src/abi.cpp`](sdk/src/abi.cpp) pins for the C compiler.

[`engine/script/lua/tynima.lua`](engine/script/lua/tynima.lua) is the half
written for people: vectors with operators, entities by component name,
shapes that make both a body and the model that draws it, the camera, the
light, rays. A whole game:

```lua
local ty = require("tynima")
local game = {}

function game.load()
  local shape = ty.box(0.5)
  local model = ty.shape_model(shape, {0.8, 0.3, 0.2})
  local body = ty.body{ shape = shape, position = {0, 4, 0}, mass = 2 }
  game.crate = ty.entity{ Transform = {position = {0, 4, 0}},
                          MeshRenderer = {model = model}, RigidBody = {body = body} }
  ty.look_at({6, 4, 8}, {0, 1, 0})
end

function game.update(dt)
  if ty.key_pressed("Space") then ty.impulse(ty.get(game.crate, "RigidBody").body, ty.vec3(0, 8, 0)) end
end

return game
```

`tynima-sandbox --script apps/sandbox/scripts/pile.lua` runs one
([that script](apps/sandbox/scripts/pile.lua) drops two dozen boxes and
knocks them over with Space). Save the file and it reloads within a quarter
second, with the world it built still standing — the same file watch the
cooked models use, and no compiler in the loop at all.

The part that is "generated from reflection" is components. The six the
engine declares are real C structs, so `ty.get(e, "Transform").position.y = 3`
is a store into the world's own memory. Anything else — a component a game
module defined in C++ and described through `TY_REFLECT` — has no
declaration for the FFI to read, so the script builds one: it asks the
engine for the component's fields, writes a packed C struct with each field
at its own offset and the gaps between them filled, `ffi.cdef`s it, and
checks that what it built is the size the engine says. A script can then
read and write a component that did not exist when the script was written.

LuaJIT is fetched and built by its own Makefile
([`cmake/Dependencies.cmake`](cmake/Dependencies.cmake)), which is every
platform here but MSVC — `TYNIMA_LUA` is off there until Phase 6's Windows
task, and without it the engine builds and `script::Vm::create()` says there
is no VM.

## Game modules and hot reload

Game code is a shared library the engine loads at run time
(`sdk::GameModule`) and talks to only through the C API in
[`sdk/include/tynima.h`](sdk/include/tynima.h): a table of function pointers
for components (by name), entities, chunk iteration, keys, time and logging.
The module links nothing from the engine, so the engine has exactly one copy
of its state, and all game state lives in the World — which is what lets the
engine swap the library for a new build while everything keeps running.

To see it: run the sandbox and press L — the game module launches the
pile through the API. Edit `apps/sandbox/game/src/game.cpp` (the launch
speed or spread: the defaults of its `Launcher` component), and rebuild
only the module:

```sh
cmake --build --preset macos-debug --target tynima_sandbox_game
```

The sandbox notices the new file, unloads the old code, loads the new, and
the next L uses the new numbers — same world, same pile, same camera, no
restart. The runtime does the noticing (`sdk::Runtime` polls the module's
file every frame), so the editor's `--game` gets the same reload.

## Physics

`physics::PhysicsWorld` ([`engine/physics/include/tynima/physics/physics.h`](engine/physics/include/tynima/physics/physics.h))
is the one interface: bodies as generational handles, box / sphere / capsule
shapes, static / kinematic / dynamic motion, impulses, forces, ray casts,
distance and hinge joints, and `step(dt)`. `create_jolt_world()` puts [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
behind it — the reference implementation the Phase 3 solver is measured
against, on the same scenes through the same calls. Jolt steps on the
engine's job system, allocates through the engine's heap (visible in Tracy,
counted as external), and is built cross-platform deterministic. Its headers
never leave `engine/physics/`; the layering check enforces that.

Entities follow bodies through the `RigidBody` component and
`scene::update_bodies()`, run after the step and before `update_transforms()`.
The simulation runs at a fixed 60 Hz whatever the frame rate:
`physics::FixedStepper` ([`fixed_step.h`](engine/physics/include/tynima/physics/fixed_step.h))
turns frame time into whole steps (at most four a frame, so a hitch drops
time rather than spiralling) and says how far the frame is between the last
two states; `scene::record_previous_poses()` before each step and
`update_bodies(world, physics, alpha)` after them draw everything blended
between those states. The sandbox drops a pile of a hundred bottles on a
floor — through the engine's own solver by default, through Jolt with
`--physics jolt` — and R drops it again; beside it hang a gate on a limited
hinge and a chain of bottles on distance joints, and a character stands
ready to walk into them.

The pieces of our own solver arrive behind their own interfaces, each tested
against a brute-force reference and timed on the pile's real trajectories
sampled from Jolt (`ctest` prints the numbers). So far:

- **Broadphase** ([`broadphase.h`](engine/physics/include/tynima/physics/broadphase.h)):
  proxies (a box and a value), `find_pairs`, box and ray queries.
  `create_brute_force_broadphase` is the reference; `create_sweep_and_prune`
  keeps interval endpoints sorted along whichever axis the scene spreads on;
  `create_aabb_tree` is a Box2D-style dynamic tree with fat boxes and
  rotations, the spatial index the rest of the engine will query. All three
  return identical pair sets.
- **Narrowphase** ([`collision.h`](engine/physics/include/tynima/physics/collision.h)):
  `gjk()` for the distance and closest points between two convex cores,
  `epa()` for the way out when they overlap, and `collide()` for the whole
  answer as a manifold of up to four points — the convex-radius trick makes
  spheres and capsules exact and keeps EPA for boxes, and clipping the two
  supporting faces gives a resting box all four corners in one frame.
  `ManifoldCache` keeps manifolds across frames, matching points by where
  they sit on each body so the solver's impulses carry over. Every contact
  Jolt reports on a pile of spheres and capsules is found by `collide()`
  with the same normal and depth to a twentieth of a millimetre; boxes agree
  to within the rounding Jolt puts on their edges.
- **Solver** ([`tynima_world.cpp`](engine/physics/src/tynima_world.cpp)):
  `create_tynima_world()` is the engine's own `PhysicsWorld` — the tree, the
  manifolds, and a sequential-impulse solver in Box2D's shape: warm-started
  velocity iterations with friction and restitution, then non-linear
  Gauss-Seidel position iterations against fresh anchors, so penetration is
  fixed without pumping energy in. Every scene in the world tests runs
  through both worlds with the same expectations, and the sandbox's pile is
  dropped through both (`--physics jolt` for the reference); nothing in a
  step allocates.
- **Islands and sleep**: each step, union-find over the touching pairs and
  the joints groups the bodies into islands — what touches or is joined,
  directly or through others. An island is solved on its own (on its own
  job when the world has a job system; the narrowphase runs over the pairs
  in parallel too), sleeps as one once every body in it has been still for
  half a second, and wakes as one when an awake body touches it, when
  something kinematic pushes on it however slowly, or when it loses what it
  rested on or hung from. The same scene with and without the job system
  comes out bit for bit the same, on both backends.
- **Joints** (`JointDesc` in [`physics.h`](engine/physics/include/tynima/physics/physics.h)):
  a distance joint keeps two anchors a set length apart (a rod, a chain
  link), a hinge keeps them together and lets the bodies turn about one
  shared axis, optionally between limits; either can be to the world.
  In our solver a distance joint is one bilateral row, and a hinge is six
  rows — three for the point, two for the axis, one for the limit — solved
  as a single 6×6 block per iteration. Row by row they undo each other (an
  angular impulse about the centre of mass moves the anchor) and converge
  slowly enough that a door rebounds off its stop at 5 % of its speed, which
  is what Jolt does; as a block, the door stops dead at the limit. A
  pendulum's period, a five-link chain's link lengths, a door's swing after
  a push and both of its stops come out the same in both worlds.
- **Character controller** ([`character.h`](engine/physics/include/tynima/physics/character.h)):
  `CharacterController` is a capsule that never turns, driven by
  `move(walk_velocity, jump, dt)` before each step: it stands on any contact
  within its slope limit, walks along the ground plane at the asked-for
  speed up and down slopes, holds still on them (its weight is cancelled
  while it stands, so nothing creeps), jumps at its jump speed, keeps air
  control, is stopped by walls and shoves light bodies aside. Walking,
  climbing, jumping and pushing measure the same on both worlds.

### Determinism

The simulation is meant to come out bit for bit the same on every run,
every thread count and every platform the engine builds for, which is what
makes replays exact and a recorded run a regression test. Three things make
it so:

- **The arithmetic.** IEEE adds, multiplies, divides and square roots round
  the same everywhere as long as the compiler neither reorders them nor
  fuses a multiply and an add into one instruction, so every target we own
  builds with contraction off (`-ffp-contract=off`, `/fp:precise`) and never
  with fast-math ([`cmake/Warnings.cmake`](cmake/Warnings.cmake)).
- **The transcendentals.** `sin`, `cos`, `atan2` and friends come from the
  platform's libm, which rounds differently on macOS and Windows, so
  simulation code uses the engine's own `math::sine`, `cosine`, `tangent`,
  `arctan2`, `arcsin`, `arccos` ([`math/trig.h`](engine/core/include/tynima/core/math/trig.h)):
  Cephes-style polynomials after a Cody-Waite range reduction, within a few
  ulps of libm and the same bits on every platform. `Quat::from_axis_angle`
  and the transform helpers use them; libm is left to rendering.
- **The order of work.** Islands are solved in parallel but each island is
  solved serially, and the narrowphase writes each pair's manifold to its
  own slot, so the result does not depend on which thread got there first.

`PhysicsWorld::state_hash()` folds every body's pose, velocities and sleep
state into 64 bits, and the tests check it: the same history hashes the
same, one nudge of a tenth of a millinewton-second shows, and the threaded
and the serial run of one scene hash the same on every step. On top of that
sits the input log ([`platform/input_log.h`](engine/platform/include/tynima/platform/input_log.h)):
`tynima-sandbox --record` takes down every frame's input and frame time and
the hash at the end, `--replay` plays the log back and compares, and
[`apps/sandbox/replays/`](apps/sandbox/replays/) holds one such run on each
backend — five seconds of the pile dropping, the character walking and
jumping, the game module launching the pile — that the `sandbox_replay_*`
CTests replay on every platform CI builds. Any change to the simulation, a
new shape in the scene or one rounding in the solver, fails them; when the
change is meant, look at the new run, then record the logs again with the
commands in [`apps/sandbox/CMakeLists.txt`](apps/sandbox/CMakeLists.txt).

## Frame graph

`render::FrameGraph` ([`frame_graph.h`](engine/render/include/tynima/render/frame_graph.h))
is how a frame is drawn: passes declare what they read and write, and the
graph decides the rest — the shape Frostbite described at GDC 2017, built
every frame from scratch (declaring is a few dozen stores; nothing in it
allocates after construction). A pass's setup callback names its
attachments and the textures it samples through versioned handles (every
write makes a new version, so the graph knows exactly which pass produced
what another consumes); its execute callback draws. `compile()` then:

- **orders** the passes: after whatever wrote what they read, after whatever
  read what they overwrite, otherwise as added — and refuses a cycle;
- **culls** what nobody consumes: a pass whose writes are never read, never
  loaded by a later write and not an imported texture (the swapchain, a
  shadow atlas kept across frames) does not run, nor does whatever only
  fed it;
- **allocates transients**: a texture the graph creates lives from its first
  use to its last, so two whose lives do not overlap share one physical
  texture, and the physical textures persist across frames and are dropped
  when unused for a couple of seconds;
- **decides every load and store**: a write nothing reads afterwards is
  stored `DontCare`, and a transient that is never sampled and never stored
  is flagged as one that could live entirely in tile memory — which is the
  point of the project. SDL GPU has no memoryless attachments; the native
  Metal backend later in Phase 4 will act on the flag.

Barriers: on SDL GPU the pass order is the synchronisation (the backend
tracks hazards within a command buffer), and the graph's edges are what a
backend with explicit barriers would emit them from.

The scene renderer (`render::SceneRenderer`,
[`scene_renderer.h`](engine/render/include/tynima/render/scene_renderer.h) —
a list of models at world matrices, a camera, a sun and some point lights,
which `scene::collect_draws` gathers from the World) declares its passes
into it, and the post stack its own: at the simplest a `scene` pass into an
`Rgba16Float` transient with a depth transient beside it, and a `tonemap`
pass that samples the result onto the swapchain (ACES, and the sRGB encode
where the swapchain does not do it). The graph works out that depth is
never stored and HDR is, and the runtime logs the plan once it has settled
(here at 2560×1440):

```
graph: 2 passes (0 culled); 2 transients in 2 textures, 44.2 MB of 44.2 MB asked; 2 attachments stored, 1 discarded; 1 could live in tile memory
graph:   scene: writes hdr (clear, store), depth (clear, discard)
graph:   tonemap: reads hdr; writes swapchain (dontcare, store)
```

The RHI grew what the graph needs: passes on any texture with load and
store actions (`Frame::begin_pass`), pipelines built for explicit target
formats, and the swapchain image as an ordinary texture handle for the
frame (`Frame::swapchain_texture()`). The render tests compile graphs with
no device at all — culling, ordering, aliasing, stores, and every way a
graph can be malformed — and the RHI tests draw offscreen and sample it
back where a GPU exists.

### Two backends

`rhi::Device` ([`device.h`](engine/rhi/include/tynima/rhi/device.h)) is
an interface with two implementations behind `Device::create()`:
[`sdl_device.cpp`](engine/rhi/src/sdl_device.cpp), SDL3 GPU over Metal,
Vulkan or D3D12, and [`metal_device.mm`](engine/rhi/src/metal_device.mm),
the engine's own path onto Apple GPUs with nothing between the calls and
the Metal API — Objective-C++ under ARC, the pools holding Metal objects
as retained pointers, a per-frame autorelease pool, three frames in flight
on a semaphore, streamed writes through per-frame shared memory and a blit,
uniforms pushed as inline bytes. Both bind shaders the same way (uniforms
first at `[[buffer(n)]]`, storage buffers after, the vertex buffer at
`[[buffer(14)]]`), so every shader the engine has runs on either unchanged,
and every GPU test in `rhi` and `render` runs on both — the same calls, the
same expectations, the way the physics tests hold the engine's solver to
Jolt. On a Mac the sandbox draws through the native backend; `--rhi sdl`
asks for SDL GPU instead.

### The tile

The thesis, in code. `--shading fused` lights the scene the deferred way in
a single render pass: every mesh writes its material into a G-buffer —
albedo and metallic (RGBA8), the shading normal and roughness (RGBA16F),
the view depth and occlusion (RG32F), with emissive straight into the HDR
target — and then a full-screen draw in the *same* pass reads those three
attachments back through MSL's `[[color(n)]]` inputs, straight from the
tile, and writes the lit result into HDR. The frame graph sees three
transients that are written and never read afterwards, so it stores none of
them, and on the native backend it creates them memoryless
(`TextureDesc::memoryless`, `MTLStorageModeMemoryless`): twenty bytes a
pixel that never exist in DRAM at all. `--shading split` is the same
G-buffer stored by one pass and sampled by the next — what a deferred
renderer must do on a GPU without tile memory — and `--shading forward` is
the path the sandbox had before. `Y` cycles the three at run time; the
picture is the same.

What to measure. The sandbox logs at frame 60 how much of the graph's
memory is in DRAM and how much is memoryless, and every five seconds the
GPU time per frame (`Device::gpu_stats()`, from the command buffer's
timestamps on Metal). Fused against split is the G-buffer's round trip —
about 20 bytes a pixel written and read back, 150 MB a frame at 2560×1440 —
and it shows in the GPU time. For the bandwidth itself, Xcode's GPU
profiler (a Metal capture of one frame) lists the memory traffic of each
pass: the fused pass's G-buffer attachments show no store and no load,
which is the whole point.

The RHI grew what the fusion needs: `TextureDesc::memoryless`, a
per-attachment write mask in `GraphicsPipelineDesc` (the lighting draw
declares which attachments it only reads), `Rg32Float`,
`Device::gpu_stats()`, and `Device::download_texture()` so a test can read
a rendered image back — which one does, on both backends: an attachment
written by one draw and read by the next through the tile, memoryless
where the backend allows, comes out with the expected pixels.

Buffers go through the graph as imported resources with the same versioned
handles: a compute pass (`add_compute_pass`) writes them, any pass reads
them, and the versions order the passes. Compute came with it in the RHI —
`ComputePipelineDesc`, `Frame::begin_compute_pass`, `Storage` buffers,
`Frame::write_buffer` to stream a frame's data in without a wait, and
`Device::download_buffer` so a test can read a kernel's result back.

### Shadows

The sun casts cascaded shadow maps ([`shadows.h`](engine/render/include/tynima/render/shadows.h)):
`fit_cascades()` cuts the view frustum into four slices out to 60 m —
between uniform and logarithmic, the "practical split scheme" — and fits an
orthographic light-space box around each slice's bounding sphere. The
sphere, not the slice, so the box keeps its size as the camera turns; its
position snapped to the shadow map's texel grid, so edges do not shimmer as
the camera moves; reverse-Z like the camera, so one depth convention serves
the whole frame. Every cascade is a 2048² depth-only pass in the frame
graph, and the scene pass reads them: the first cascade whose map holds the
pixel is sampled with 3×3 taps of the hardware's own 2×2 comparison (a
comparison sampler, `SamplerDesc::compare`), with the lookup point pushed
two texels off the surface along the normal and one texel towards the
light against acne. The tests check that every corner of every slice lands
inside its cascade at a depth the map can hold, that nearer the light means
deeper into 1, and that a step smaller than a texel moves the map's contents
by a whole texel or not at all.

### Lights

A hundred coloured point lights circle the pile, and each pixel shades only
the ones that reach it ([`clusters.h`](engine/render/include/tynima/render/clusters.h)).
The view frustum is a grid of 16 × 9 screen tiles by 24 depth slices spaced
by equal ratios out to 80 m; every frame a compute pass (`cluster lights`
in the graph, one thread per cell) tests each light's sphere against each
cell's box and writes the cell's list, and the scene shader finds its
pixel's cell from the window position and the view depth, then runs the
same BRDF as the sun over that list with an inverse-square falloff windowed
to nothing at the light's radius. `ClusterGrid` on the CPU is the same
arithmetic the shaders use — slice from depth, cell box from tile and slice
— and `assign_lights()` is the kernel's reference: the tests hold every
light inside the grid to be listed by its own cell and every listed light to
actually reach its cell, and where a GPU exists they run the kernel and
compare its table with the CPU's cell for cell.

### Post

`render::PostStack` ([`post.h`](engine/render/include/tynima/render/post.h))
takes the lit HDR image to the screen, every step a pass in the graph and
every step optional: **bloom** — the bright parts (a soft-kneed threshold)
downsampled through Jimenez's 13-tap filter to a fifth of a fifth of the
size and brought back up through a 3×3 tent, each level adding its own —
then **TAA** — the scene drawn a Halton-sequence fraction of a pixel off
each frame and blended nine-to-one with the last frame fetched from where
each pixel's surface was then (reprojected through the depth buffer),
clamped to the neighbourhood's colour range in YCoCg so a moved object
leaves no trail, weighted by inverse luminance so a bright speck does not
flicker in; the resolved image is written straight into the history texture
the next frame reads — then **tonemapping**, ACES (Narkowicz's fit) or AgX
(Sobotka's, in Wrensch's compact form), leaving linear light for the sRGB
encode — then **FXAA**, Lottes's filter in its compact form, when TAA is
not on. The stack declares its passes with or without a device, which is
how the tests check the chain's shape: five downsamples and four upsamples
at 720p, the tonemap reading the top of the chain, FXAA reading an LDR
intermediate, the depth buffer staying on-tile until TAA asks for it.

## Logging and asserts

Every message is an event — level, category, source location, thread, time —
handed to the installed sinks. The console sink prints one line per event to
stderr, coloured on a terminal; the profiler sink puts it on Tracy's timeline:

```
    0.140 INFO  model    1 materials, 4 images decoded in 0.14 s
    0.140 WARN  gltf     image 2: unsupported format [thread 3]
    1.204 FATAL assert   engine_allocations == 0: a frame allocated ... (sdk/src/runtime.cpp:342)
```

```cpp
#include <tynima/core/log.h>
TY_LOG_INFO("gpu", "%s, depth %s", backend, format);   // printf formats, checked at compile time
```

Levels are trace, debug, info, warn, error and fatal; the minimum is debug
in Debug builds and info otherwise. `TYNIMA_LOG=trace ./tynima-sandbox`
changes it for a run, `core::set_log_level` from code. A message is formatted
on the stack, so logging inside a frame is fine; `core::add_log_sink` adds a
destination (a file, a test's capture). Game modules log through the C API's
`log(engine, level, message)`, under the "game" category.

`TY_ASSERT(condition, "message")` is checked in Debug builds and compiled
away in Release. A failure is logged at fatal and then traps in the debugger
on the failing line when one is attached — step past it to carry on — or
aborts when none is. `core::set_assert_handler` takes over for tests.

## Profiling

Every configuration compiles in Tracy instrumentation (`TYNIMA_PROFILE`, on
by default) in on-demand mode, so it records nothing until a profiler
connects. The GUI must be the same version as the client we build against —
0.13.1, which is what Homebrew ships:

```sh
brew install tracy   # then run `tracy` and connect to 127.0.0.1 while the sandbox runs
```

Instrument with the macros in `engine/core/include/tynima/core/profile.h`;
`core` is the only module that includes Tracy directly.

## Math conventions

`tynima::math` (`engine/core/include/tynima/core/math/`) fixes these once, for
every module above it:

- World and view space are right-handed, y up; a camera looks down −z.
- Matrices are column-major and act on column vectors: `M * v`, and
  `T * R * S` scales first, then rotates, then translates.
  `Mat4::from_rows` writes a matrix the way it is printed.
- Clip space follows Metal, D3D12 and SDL GPU: y up, depth 0 at near and 1 at
  far. `perspective_infinite_reverse_z` puts 1 at near and 0 at infinity —
  the renderer's default once it has a depth buffer (GREATER test, clear to 0).
- Quaternions are `{x, y, z, w}`, glTF's order; `a * b` applies `b` first,
  like matrices.
- Angles are radians. `normalize` of zero is zero and `inverse` of a singular
  matrix is the identity — a degenerate input never produces NaN.

## Layering check

```sh
python3 tools/check_layering.py          # exit 1 on violations, 2 on broken declarations
python3 tools/check_layering.py --graph  # the declared graph, as Mermaid
```

Third-party headers have one home each: SDL3 in `platform` and `rhi`, Tracy in
`core`, cgltf, stb and meshoptimizer in `cooker`, Jolt in `physics`, Dear ImGui
in `ui` and the editor. Everything else reaches them through that module's own
API.

## Adding a module

1. Create `engine/<name>/` with `include/tynima/<name>/`, `src/` and optionally `tests/`.
2. Add a `CMakeLists.txt` containing `tynima_add_module(NAME <name> DEPENDS ...)`.
3. Insert `<name>` at its layer in `LAYERS` in `tools/check_layering.py` and add
   the `add_subdirectory` line to the root `CMakeLists.txt`, in layer order.

A module with no sources yet is an interface library, so it can be declared and
depended on before it has code. Tests under `tests/` become
`tynima_<name>_tests` and register with CTest automatically.

## License

MIT — see [LICENSE](LICENSE). Sample content downloaded at configure time
(Khronos glTF-Sample-Assets) is CC0. Third-party code fetched by the build
keeps its own license: SDL3 (zlib), doctest (MIT), Tracy (BSD-3), cgltf (MIT),
stb (MIT / public domain), meshoptimizer (MIT), Jolt Physics (MIT), Dear ImGui
(MIT), LuaJIT (MIT).
