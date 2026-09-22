#!/usr/bin/env python3
"""Generate the Lua side of the C ABI from sdk/include/tynima.h.

LuaJIT's FFI parses C declarations at run time and calls through them with
JIT-compiled code, so a script needs no bindings written by hand — it needs
the declarations. This turns the public header into exactly those: every
typedef, enum, struct and function-pointer table, as one ffi.cdef, plus the
header's constants as Lua values.

What is left out is what the FFI cannot parse or does not need: the inline
math (Lua has its own, with operators, in tynima.lua), the include guard and
the extern "C" wrapper, and the free functions a host calls, which a script
cannot reach anyway — a script talks to the engine through the same table a
C game module gets.

    python3 tools/gen_lua.py                 # write engine/script/lua/tynima_ffi.lua
    python3 tools/gen_lua.py --check         # exit 1 if that file is out of date
    python3 tools/gen_lua.py -o -            # to stdout

The result is checked in, so building a game needs no Python; a test
regenerates it and fails when it has drifted from the header.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "sdk" / "include" / "tynima.h"
OUTPUT = ROOT / "engine" / "script" / "lua" / "tynima_ffi.lua"

# Constants worth having in Lua, with the C expression they are defined as.
# Anything else in the header is a declaration, not a value.
CONSTANT_RE = re.compile(r"^#define\s+(TYNIMA_[A-Z0-9_]+)\s+(\S.*?)\s*(?:/\*.*)?$")
SKIP_CONSTANTS = {"TYNIMA_H", "TYNIMA_GAME_EXPORT", "TYNIMA_GAME_ENTRY", "TYNIMA_GAME_ENTRY_NAME"}


def strip_comments(text: str) -> str:
    """C comments out, string literals left alone (the header has none in code)."""
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def constants(text: str) -> list[tuple[str, str]]:
    found = []
    for line in text.splitlines():
        match = CONSTANT_RE.match(line.strip())
        if match is None:
            continue
        name, value = match.group(1), match.group(2).strip()
        if name in SKIP_CONSTANTS or "(" in name:
            continue
        # C integer suffixes and hex are Lua-compatible; drop the suffix.
        value = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uU][lL]?[lL]?\b", r"\1", value)
        found.append((name, value))
    return found


def declarations(text: str) -> str:
    """Every declaration the FFI needs, in the order the header gives them.

    The header is read as a sequence of top-level statements: a typedef
    (struct, enum or alias) ends at the semicolon after its closing brace, a
    function declaration at its own semicolon. Inline definitions and
    preprocessor lines are dropped, and so is everything a host calls but a
    script cannot.
    """
    text = strip_comments(text)
    values = {name: value for name, value in constants(text) if not value.startswith('"')}
    out: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        # Preprocessor lines, and the extern "C" wrapper, are not C declarations.
        if text[i] == "#":
            i = text.find("\n", i) + 1 or length
            continue
        if text.startswith("extern", i) or text.startswith("}", i):
            i = text.find("\n", i) + 1 or length
            continue
        if text[i].isspace():
            i += 1
            continue
        # A statement ends at its semicolon, minding braces — except a
        # function definition, which ends at its closing brace and has no
        # semicolon at all. Reading one as if it did would swallow whatever
        # declaration comes next.
        start = i
        definition = text.startswith("static inline", i)
        depth = 0
        opened = False
        while i < length:
            char = text[i]
            if char == "{":
                depth += 1
                opened = True
            elif char == "}":
                depth -= 1
                if definition and opened and depth == 0:
                    i += 1
                    break
            elif char == ";" and depth == 0:
                break
            elif char == "#" and depth > 0:
                # A preprocessor line inside a declaration (there are none
                # left in the header, but a stray one would be silent).
                raise SystemExit(f"gen_lua: a preprocessor line inside a declaration at {start}")
            i += 1
        statement = text[start:i].strip()
        i += 1
        if not statement:
            continue
        # Inline functions are definitions, not declarations: Lua has its own.
        if definition:
            continue
        # A free function is a host's; a script reaches the engine through
        # the table. Typedefs and the two structs a module exports stay.
        if not statement.startswith("typedef") and "(*" not in statement:
            continue
        # An array length written as a constant means nothing to the FFI:
        # give it the number the C compiler would have used.
        statement = re.sub(r"\bTYNIMA_[A-Z0-9_]+\b",
                           lambda m: values.get(m.group(0), m.group(0)), statement)
        out.append(" ".join(statement.split()) + ";")
    return "\n".join(out)


def generate() -> str:
    text = HEADER.read_text(encoding="utf-8")
    lines = [
        "-- Generated by tools/gen_lua.py from sdk/include/tynima.h. Do not edit:",
        "-- run the generator, or change the header. The engine's own test checks",
        "-- that this file still matches the header it was generated from.",
        "--",
        "-- What a script does with it: nothing directly. tynima.lua is the half",
        "-- written for people; this is the half written for the FFI.",
        "local ffi = require('ffi')",
        "",
        "ffi.cdef[[",
    ]
    for statement in declarations(text).splitlines():
        lines.append(statement)
    lines.append("]]")
    lines.append("")
    lines.append("-- The header's #defines. Its enum constants (every TYNIMA_KEY_*,")
    lines.append("-- TYNIMA_FIELD_*, TYNIMA_SHAPE_* and the rest) are the FFI's, so asking")
    lines.append("-- this table for one falls through to it: both kinds read the same way.")
    lines.append("local constants = {")
    for name, value in constants(text):
        lines.append(f"    {name} = {value},")
    lines.append("}")
    lines.append("")
    lines.append("return setmetatable(constants, {__index = function(_, name)")
    lines.append("    local ok, value = pcall(function() return ffi.C[name] end)")
    lines.append("    if not ok then")
    lines.append("        error(\"tynima.h declares nothing called '\" .. tostring(name) .. \"'\", 2)")
    lines.append("    end")
    lines.append("    return value")
    lines.append("end})")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", default=str(OUTPUT), help="- for stdout")
    parser.add_argument("--check", action="store_true", help="only report whether the file is current")
    args = parser.parse_args()

    generated = generate()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current == generated:
            print(f"{OUTPUT.relative_to(ROOT)} is up to date")
            return 0
        print(f"{OUTPUT.relative_to(ROOT)} is out of date: run tools/gen_lua.py", file=sys.stderr)
        return 1
    if args.output == "-":
        sys.stdout.write(generated)
        return 0
    path = Path(args.output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(generated, encoding="utf-8")
    print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
