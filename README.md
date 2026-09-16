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
| `editor/`         | `sdk`                                | ImGui tools — a client of the SDK, no special privileges    |
| `sdk/`            | everything below                     | `tynima.h` C ABI; later the CLI, templates, codegen         |
| `engine/script/`  | `core scene assets`                  | Lua / C# bindings generated from reflection                 |
| `engine/assets/`  | `core platform render scene`         | Runtime loading of cooked blobs, GPU upload, hot reload     |
| `engine/scene/`   | `core render physics`                | Archetype ECS, transforms, the systems that drive the rest  |
| `engine/physics/` | `core`                               | Rigid bodies behind `PhysicsWorld`: Jolt now, ours next     |
| `engine/render/`  | `core platform rhi`                  | Frame graph, passes, materials, culling                     |
| `engine/rhi/`     | `core platform`                      | Render hardware interface — SDL3 GPU, then native Metal     |
| `engine/platform/`| `core`                               | Window, input, filesystem, time, threads (wraps SDL3)       |
| `engine/core/`    | —                                    | Allocators, containers, math, jobs, logging, reflection     |
| `apps/sandbox/`   | any engine module                    | Engine developer's playground — a test bed, not a template  |
| `apps/sandbox/game/` | `tynima.h` only (headers of sdk/scene/physics/core) | The sandbox's hot-reloadable game module    |

Transitive dependencies don't count: if a file includes `tynima/rhi/...`, its
module must declare `rhi`. Nothing may depend on an app, and `editor` may depend
on `sdk` alone.

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
```

The sandbox loads a glTF model (a CC0 Khronos sample, downloaded into
`build/<preset>/assets/` at configure time) with its base color texture,
draws it with a reverse-Z depth buffer through SDL3 GPU with its Metal shaders
compiled at runtime — sRGB textures sampled through sRGB formats, lighting in
linear into a 16-bit float target, tonemapped onto an sRGB-encoded swapchain
in a second pass — and lets you fly around it: hold the right mouse button to look, W/A/S/D to move, Q/E
to descend and climb, Shift to run, Escape to quit. `F` puts the camera
behind the character instead: then W/A/S/D walk it, Space jumps, Shift
runs, and it can shove bottles about, push the gate open and swing the
hanging chain. `1`/`2`/`3` switch between
unlit, Blinn-Phong and Cook-Torrance shading; `N`/`M`/`O`/`V`/`B` show the
mapped normals, metallic/roughness, occlusion, vertex normals and tangents;
`T` toggles tonemapping; the arrow keys move the light. `--model path.glb` loads
something else. `--headless --frames N` runs the same loop with no window and
no GPU (the model still loads) with a scripted player at the keyboard, which
is what the `sandbox_headless` CTest does on CI. `--record run.tyrec` writes
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
speed or spread), and rebuild only the module:

```sh
cmake --build --preset macos-debug --target tynima_sandbox_game
```

The sandbox notices the new file, unloads the old code, loads the new, and
the next L uses the new numbers — same world, same pile, same camera, no
restart.

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

The sandbox draws through it: a `scene` pass into an `Rgba16Float` transient
with a depth transient beside it, and a `tonemap` pass that samples the
result onto the swapchain (ACES, and the sRGB encode where the swapchain
does not do it). The graph works out that depth is never stored and HDR is,
and the sandbox logs the plan once it has settled (here at 2560×1440):

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

## Logging and asserts

Every message is an event — level, category, source location, thread, time —
handed to the installed sinks. The console sink prints one line per event to
stderr, coloured on a terminal; the profiler sink puts it on Tracy's timeline:

```
    0.140 INFO  model    1 materials, 4 images decoded in 0.14 s
    0.140 WARN  gltf     image 2: unsupported format [thread 3]
    1.204 FATAL assert   engine_allocations == 0: a frame allocated ... (apps/sandbox/src/main.cpp:817)
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
`core`, cgltf and stb in `assets`, Jolt in `physics`. Everything else reaches
them through that module's own API.

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
stb (MIT / public domain), Jolt Physics (MIT).
