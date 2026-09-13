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

On Windows: `cmake --preset windows`, then `cmake --build --preset windows-debug`
and `ctest --preset windows-debug`.

Options (`-D` at configure time): `TYNIMA_BUILD_TESTS`, `TYNIMA_BUILD_EDITOR`,
`TYNIMA_BUILD_APPS`, `TYNIMA_FETCH_SDL3`, `TYNIMA_WARNINGS_AS_ERRORS` (on in CI).

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
