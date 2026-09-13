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
| `engine/physics/` | `core`                               | Broadphase, narrowphase, solver, joints                     |
| `engine/render/`  | `core rhi`                           | Frame graph, passes, materials, culling                     |
| `engine/rhi/`     | `core platform`                      | Render hardware interface — SDL3 GPU, then native Metal     |
| `engine/platform/`| `core`                               | Window, input, filesystem, time, threads (wraps SDL3)       |
| `engine/core/`    | —                                    | Allocators, containers, math, jobs, logging, reflection     |
| `apps/sandbox/`   | any engine module                    | Engine developer's playground — a test bed, not a template  |

Transitive dependencies don't count: if a file includes `tynima/rhi/...`, its
module must declare `rhi`. Nothing may depend on an app, and `editor` may depend
on `sdk` alone.

## Build

Requirements: CMake 3.28+, a C++20 compiler, Ninja on macOS, Visual Studio 2022+
on Windows. The first configure fetches SDL3 and doctest from GitHub.

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
unlit, Blinn-Phong and Cook-Torrance shading; `N`/`M`/`O` show normals,
metallic/roughness and occlusion; `T` toggles tonemapping; the arrow keys move
the light. `--model path.glb` loads
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

## Adding a module

1. Create `engine/<name>/` with `include/tynima/<name>/`, `src/` and optionally `tests/`.
2. Add a `CMakeLists.txt` containing `tynima_add_module(NAME <name> DEPENDS ...)`.
3. Insert `<name>` at its layer in `LAYERS` in `tools/check_layering.py` and add
   the `add_subdirectory` line to the root `CMakeLists.txt`, in layer order.

A module with no sources yet is an interface library, so it can be declared and
depended on before it has code. Tests under `tests/` become
`tynima_<name>_tests` and register with CTest automatically.

## License

Not chosen yet — see "Decide early" in the roadmap.
