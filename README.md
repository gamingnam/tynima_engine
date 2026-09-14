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
| `engine/render/`  | `core rhi`                           | Frame graph, passes, materials, culling                     |
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
linear, an sRGB-encoded swapchain on the way out — and lets you fly around it: hold the right mouse button to look, W/A/S/D to move, Q/E
to descend and climb, Shift to run, Escape to quit. `1`/`2`/`3` switch between
unlit, Blinn-Phong and Cook-Torrance shading; `N`/`M`/`O`/`V`/`B` show the
mapped normals, metallic/roughness, occlusion, vertex normals and tangents;
`T` toggles tonemapping; the arrow keys move the light. `--model path.glb` loads
something else. `--headless --frames N` runs the same loop with no window and
no GPU (the model still loads), which is what the `sandbox_headless` CTest
does on CI.

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

To see it: run the sandbox and press Space — the game module launches the
pile through the API. Edit `apps/sandbox/game/src/game.cpp` (the launch
speed or spread), and rebuild only the module:

```sh
cmake --build --preset macos-debug --target tynima_sandbox_game
```

The sandbox notices the new file, unloads the old code, loads the new, and
the next Space uses the new numbers — same world, same pile, same camera, no
restart.

## Physics

`physics::PhysicsWorld` ([`engine/physics/include/tynima/physics/physics.h`](engine/physics/include/tynima/physics/physics.h))
is the one interface: bodies as generational handles, box / sphere / capsule
shapes, static / kinematic / dynamic motion, impulses, forces, ray casts and
`step(dt)`. `create_jolt_world()` puts [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
behind it — the reference implementation the Phase 3 solver is measured
against, on the same scenes through the same calls. Jolt steps on the
engine's job system, allocates through the engine's heap (visible in Tracy,
counted as external), and is built cross-platform deterministic. Its headers
never leave `engine/physics/`; the layering check enforces that.

Entities follow bodies through the `RigidBody` component and
`scene::update_bodies()`, run after the step and before `update_transforms()`.
The sandbox drops a pile of a hundred bottles on a floor; R drops it again.

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
