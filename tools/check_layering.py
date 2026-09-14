#!/usr/bin/env python3
"""Fail the build when a module includes a header from a layer it may not use.

Every module declares its *direct* dependencies in its CMakeLists.txt:

    tynima_add_module(NAME scene DEPENDS core render physics)
    tynima_add_app(NAME editor DEPENDS sdk)
    tynima_add_game_module(NAME sandbox_game DEPENDS sdk scene core)

That declaration is the single source of truth. This script reads it and
checks every C/C++ source under the module against four rules:

  1. A file in module M may `#include <tynima/D/...>` (or `tynima.h`, which
     belongs to sdk) only if D is M itself or one of M's declared dependencies.
     Transitive dependencies don't count: if you include it, declare it.
  2. Declared dependencies point down the layer order (LAYERS below).
     Apps under apps/ sit on top; nothing may depend on an app.
  3. `editor` may depend on `sdk` and nothing else — the editor is a client
     of the public API with no special privileges.
  4. Every source file must live inside a declared module.
  5. Third-party headers with a designated home stay there: SDL3, Tracy,
     cgltf, stb and Jolt may be included only by the modules listed in THIRD_PARTY below.

Usage:
    python3 tools/check_layering.py            # check, exit 1 on violations
    python3 tools/check_layering.py --graph    # print the declared graph (Mermaid)

Exit status: 0 clean, 1 violations found, 2 the declarations themselves are broken.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Bottom layer first. A module may only depend on modules earlier in this list.
LAYERS = ["core", "platform", "rhi", "render", "physics", "scene", "assets", "script", "sdk", "editor"]

# Directories that contain modules or apps (relative to the repo root).
SCAN_ROOTS = ["engine", "sdk", "editor", "apps"]

SOURCE_SUFFIXES = {".h", ".hpp", ".inl", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}

# Include path prefix (a directory with its slash, or a bare header name) ->
# the modules allowed to include it. Everything else goes through those.
THIRD_PARTY = {
    "SDL3/": {"platform", "rhi"},
    "tracy/": {"core"},
    "cgltf.h": {"assets"},
    "stb_": {"assets"},
    "Jolt/": {"physics"},
}

DECLARATION_RE = re.compile(r"tynima_add_(module|app|game_module)\s*\(\s*NAME\s+(\w+)(.*?)\)", re.S)
# <tynima/core/version.h>, "tynima/rhi/device.h", or the C ABI header <tynima.h>.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"](tynima\.h|tynima/([\w-]+)/[^>"]*)[>"]', re.M)
THIRD_PARTY_RE = re.compile(r'^\s*#\s*include\s*[<"](' + "|".join(map(re.escape, THIRD_PARTY)) + r')', re.M)


@dataclass
class Module:
    name: str
    kind: str  # "module" or "app"
    root: Path  # directory holding its CMakeLists.txt
    depends: list[str] = field(default_factory=list)

    @property
    def layer(self) -> int:
        return LAYERS.index(self.name) if self.name in LAYERS else len(LAYERS)


@dataclass
class Violation:
    path: Path
    line: int  # 0 when the problem is a declaration, not a source line
    message: str


def strip_cmake_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def parse_depends(body: str) -> list[str]:
    """Collect the tokens that follow DEPENDS, stopping at the next keyword."""
    deps: list[str] = []
    current = None
    for token in body.split():
        if token in ("DEPENDS", "LINK"):
            current = token
        elif current == "DEPENDS":
            deps.append(token)
    return deps


def discover_modules(root: Path) -> tuple[dict[str, Module], list[Violation]]:
    modules: dict[str, Module] = {}
    problems: list[Violation] = []
    for scan_root in SCAN_ROOTS:
        for cmake_file in sorted((root / scan_root).rglob("CMakeLists.txt")):
            text = strip_cmake_comments(cmake_file.read_text(encoding="utf-8"))
            for match in DECLARATION_RE.finditer(text):
                kind, name, body = match.group(1), match.group(2), match.group(3)
                if name in modules:
                    problems.append(Violation(cmake_file, 0, f"module '{name}' is declared twice"))
                    continue
                if kind == "game_module":
                    kind = "app" # loaded at run time, but for layering it sits on top like an app
                if kind == "module" and name not in LAYERS:
                    problems.append(
                        Violation(cmake_file, 0, f"module '{name}' is not in the layer order; add it to LAYERS in {Path(__file__).name}")
                    )
                    continue
                modules[name] = Module(name, kind, cmake_file.parent, parse_depends(body))
    return modules, problems


def check_declarations(modules: dict[str, Module]) -> list[Violation]:
    problems: list[Violation] = []
    for module in modules.values():
        cmake_file = module.root / "CMakeLists.txt"
        for dep in module.depends:
            target = modules.get(dep)
            if target is None:
                problems.append(Violation(cmake_file, 0, f"'{module.name}' depends on unknown module '{dep}'"))
            elif target.kind == "app":
                problems.append(Violation(cmake_file, 0, f"'{module.name}' depends on '{dep}', which is an app; nothing may depend on an app"))
            elif module.kind == "module" and target.layer >= module.layer:
                problems.append(
                    Violation(cmake_file, 0, f"'{module.name}' depends on '{dep}', but '{dep}' is not below it in the layer order ({' < '.join(LAYERS)})")
                )
            elif module.name == "editor" and dep != "sdk":
                problems.append(Violation(cmake_file, 0, f"'editor' may depend on 'sdk' only (it is a client of the public API), not '{dep}'"))
            elif dep == "editor":
                problems.append(Violation(cmake_file, 0, f"'{module.name}' depends on 'editor', which is an executable"))
    return problems


def module_for(path: Path, modules: dict[str, Module]) -> Module | None:
    best: Module | None = None
    for module in modules.values():
        if module.root in path.parents and (best is None or len(module.root.parts) > len(best.root.parts)):
            best = module
    return best


def check_sources(root: Path, modules: dict[str, Module]) -> list[Violation]:
    violations: list[Violation] = []
    for scan_root in SCAN_ROOTS:
        for path in sorted((root / scan_root).rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            module = module_for(path, modules)
            if module is None:
                violations.append(Violation(path, 0, "source file is not inside any declared module or app"))
                continue
            allowed = {module.name, *module.depends}
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in THIRD_PARTY_RE.finditer(text):
                prefix = match.group(1)
                if module.name not in THIRD_PARTY[prefix]:
                    line = text.count("\n", 0, match.start()) + 1
                    homes = ", ".join(sorted(THIRD_PARTY[prefix]))
                    violations.append(Violation(path, line, f"'{module.name}' includes {prefix.rstrip('/')} directly; only {homes} may — use that module's wrapper instead"))
            for match in INCLUDE_RE.finditer(text):
                included = "sdk" if match.group(1) == "tynima.h" else match.group(2)
                if included in allowed:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                if included not in modules:
                    violations.append(Violation(path, line, f"includes <{match.group(1)}>, but no module named '{included}' is declared"))
                else:
                    declared = ", ".join(module.depends) or "nothing"
                    violations.append(
                        Violation(path, line, f"'{module.name}' includes <{match.group(1)}>, but '{included}' is not a declared dependency of '{module.name}' (declared: {declared})")
                    )
    return violations


def print_graph(modules: dict[str, Module]) -> None:
    print("graph BT")
    for module in sorted(modules.values(), key=lambda m: (m.layer, m.name)):
        shape = f'{module.name}[["{module.name}"]]' if module.kind == "app" else f'{module.name}["{module.name}"]'
        print(f"    {shape}")
    for module in modules.values():
        for dep in module.depends:
            print(f"    {module.name} --> {dep}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent, help="repository root")
    parser.add_argument("--graph", action="store_true", help="print the declared dependency graph as Mermaid and exit")
    args = parser.parse_args()
    root = args.root.resolve()

    modules, problems = discover_modules(root)
    problems += check_declarations(modules)
    if problems:
        for p in problems:
            print(f"{p.path.relative_to(root)}: error: {p.message}")
        print(f"\n{len(problems)} declaration problem(s). Fix the CMakeLists.txt declarations first.")
        return 2

    if args.graph:
        print_graph(modules)
        return 0

    violations = check_sources(root, modules)
    for v in violations:
        where = f"{v.path.relative_to(root)}:{v.line}" if v.line else str(v.path.relative_to(root))
        print(f"{where}: error: {v.message}")
    if violations:
        print(f"\n{len(violations)} layering violation(s).")
        return 1

    print(f"layering ok: {len(modules)} modules, {sum(len(m.depends) for m in modules.values())} declared edges")
    return 0


if __name__ == "__main__":
    sys.exit(main())
