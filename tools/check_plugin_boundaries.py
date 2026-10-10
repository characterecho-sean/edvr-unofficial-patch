#!/usr/bin/env python3
"""Check first-party plugin include boundaries.

The private implementation of one plugin must not depend on another plugin's
private files, even through an intervening core header. Local quoted includes
must resolve inside the repository. Public core headers remain valid shared
dependencies.

Usage: python tools/check_plugin_boundaries.py [--self-test] [--quiet]
"""

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PLUGIN_DIR = Path("src") / "plugins"
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".inl"}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*([<\"])([^>\"]+)[>\"]")
INCLUDE_DIRECTIVE_RE = re.compile(r"^\s*#\s*include\b")


def inside(path, root):
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def strip_comments(text):
    """Replace C/C++ comments with spaces while preserving quoted strings."""
    out = []
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == '"':
                state = "string"
                out.append(ch)
            elif ch == "'":
                state = "char"
                out.append(ch)
            elif ch == "/" and nxt == "/":
                state = "line"
                out.extend("  ")
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block"
                out.extend("  ")
                i += 1
            else:
                out.append(ch)
        elif state in ("string", "char"):
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                i += 1
                out.append(text[i])
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif state == "line":
            if ch == "\n":
                out.append(ch)
                state = "code"
            else:
                out.append(" ")
        else:  # block comment
            if ch == "*" and nxt == "/":
                out.extend("  ")
                i += 1
                state = "code"
            else:
                out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def includes(path):
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [], ["cannot read %s: %s" % (path, exc)]
    # A continued preprocessor line is still one include directive.
    text = re.sub(r"\\\r?\n", " ", strip_comments(text))
    found = []
    for line_no, line in enumerate(text.splitlines(), 1):
        match = INCLUDE_RE.match(line)
        if match:
            found.append((line_no, match.group(1) == '"', match.group(2).strip()))
        elif INCLUDE_DIRECTIVE_RE.match(line):
            found.append((line_no, None, None))
    return found, []


def resolve_include(name, source, root, include_dirs):
    normalized = name.replace("\\", "/")
    candidate_path = Path(normalized)
    if candidate_path.is_absolute() or re.match(r"^[A-Za-z]:", normalized):
        return None, "absolute include escapes repository: %s" % name
    candidates = [source.parent / candidate_path, root / candidate_path]
    candidates.extend(directory / candidate_path for directory in include_dirs)
    seen = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        key = os.path.normcase(str(resolved))
        if key in seen:
            continue
        seen.add(key)
        if not inside(resolved, root):
            return None, "include path escapes repository: %s" % name
        if resolved.is_file():
            return resolved, None
    return None, None


def plugin_roots(root):
    base = root / PLUGIN_DIR
    if not base.is_dir():
        return {}
    result = {}
    for child in sorted(base.iterdir()):
        if child.is_dir() and not child.name.startswith("_"):
            result[child.name] = child.resolve()
    return result


def plugin_for(path, roots):
    matches = [(name, base) for name, base in roots.items() if inside(path, base)]
    return max(matches, key=lambda item: len(str(item[1])))[0] if matches else None


def check(root=ROOT, required_plugins=(), required_sources=(), include_dirs=()):
    root = root.resolve()
    dirs = [(root / Path(p)).resolve() if not Path(p).is_absolute() else Path(p).resolve()
            for p in include_dirs]
    errors = []
    roots = plugin_roots(root)
    for name in required_plugins:
        if name not in roots:
            errors.append("required plugin source root is missing: %s" % (PLUGIN_DIR / name))
    for source_name in required_sources:
        source = (root / Path(source_name)).resolve()
        if not inside(source, root) or not source.is_file():
            errors.append("required migrated source is missing: %s" % source_name)
    if not roots:
        errors.append("no first-party plugin source roots found under %s" % PLUGIN_DIR)
        return errors

    files_by_plugin = {}
    for name, base in roots.items():
        files = sorted(p for p in base.rglob("*") if p.is_file() and p.suffix.lower() in SOURCE_SUFFIXES)
        if not files:
            errors.append("plugin root has no source or header files: %s" % (PLUGIN_DIR / name))
        files_by_plugin[name] = files

    for plugin, files in files_by_plugin.items():
        for origin in files:
            queue = [(origin, (origin,))]
            visited = set()
            while queue:
                current, chain = queue.pop(0)
                current_key = os.path.normcase(str(current.resolve()))
                if current_key in visited:
                    continue
                visited.add(current_key)
                found, read_errors = includes(current)
                errors.extend(read_errors)
                for line_no, quoted, name in found:
                    location = "%s:%d" % (current.relative_to(root).as_posix(), line_no)
                    if name is None:
                        errors.append("%s: macro-expanded include cannot be checked" % location)
                        continue
                    resolved, resolution_error = resolve_include(name, current, root, dirs)
                    if resolution_error:
                        errors.append("%s: %s" % (location, resolution_error))
                        continue
                    if resolved is None:
                        # System/SDK angle includes are outside this contract.
                        # Repository-shaped angle includes must still resolve.
                        repo_shaped = name.replace("\\", "/").startswith(("src/", "tools/", "third_party/", "plugins/"))
                        if quoted or repo_shaped:
                            errors.append("%s: missing local include %s" % (location, name))
                        continue
                    foreign = plugin_for(resolved, roots)
                    if foreign is not None and foreign != plugin:
                        trail = " -> ".join(p.relative_to(root).as_posix() for p in chain + (resolved,))
                        errors.append("%s: plugin %s reaches private files of plugin %s (%s)"
                                      % (location, plugin, foreign, trail))
                    if resolved.suffix.lower() in SOURCE_SUFFIXES:
                        queue.append((resolved, chain + (resolved,)))
    return errors


def self_test():
    def write(root, rel, text):
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def expect(label, errors, needle):
        if not any(needle in error for error in errors):
            raise AssertionError("%s: expected %r in %r" % (label, needle, errors))

    with tempfile.TemporaryDirectory(prefix="edvr_plugin_boundary_") as temp:
        root = Path(temp)
        write(root, "src/plugins/cockpit_visuals/night_vision.cpp",
              '#include "night_vision.h"\n'
              '#include "..\\\\..\\\\d3d11\\\\plugin_registry.h"\n')
        write(root, "src/plugins/cockpit_visuals/night_vision.h", "#pragma once\n")
        write(root, "src/d3d11/plugin_registry.h", "#pragma once\n")
        write(root, "src/plugins/temporal_aa/private.h", "#pragma once\n")
        write(root, "src/common/bridge.h", '#include "../plugins/temporal_aa/private.h"\n')
        errors = check(root, required_plugins=("cockpit_visuals",),
                       required_sources=("src/plugins/cockpit_visuals/night_vision.cpp",))
        if errors:
            raise AssertionError("valid same-plugin and core includes rejected: %r" % errors)

        write(root, "src/plugins/cockpit_visuals/indirect.h", '#include "../../common/bridge.h"\n')
        errors = check(root)
        expect("transitive foreign-private include", errors, "reaches private files")

        write(root, "src/plugins/cockpit_visuals/path.h",
              '#include "..\\\\..\\\\..\\\\..\\\\outside.h"\n')
        errors = check(root)
        expect("path escape", errors, "escapes repository")

        write(root, "src/plugins/cockpit_visuals/missing.h", '#include "missing_local.h"\n')
        errors = check(root)
        expect("missing quoted include", errors, "missing local include")

        write(root, "src/plugins/cockpit_visuals/macro.h", "#include PRIVATE_PLUGIN_HEADER\n")
        errors = check(root)
        expect("macro include", errors, "macro-expanded include cannot be checked")

        write(root, "src/plugins/cockpit_visuals/comments.h",
              '// #include "../../plugins/temporal_aa/private.h"\n'
              '/* #include "absent.h" */\n#include "night_vision.h" // ok\n')
        errors = check(root)
        if any("comments.h" in error for error in errors):
            raise AssertionError("commented directives were treated as includes: %r" % errors)

        errors = check(root, required_plugins=("intro",),
                       required_sources=("src/plugins/cockpit_visuals/not_here.cpp",))
        expect("missing declared plugin", errors, "required plugin source root is missing")
        expect("missing migrated source", errors, "required migrated source is missing")

    print("[plugin-boundaries] self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--require-plugin", action="append", default=[])
    parser.add_argument("--require-source", action="append", default=[])
    parser.add_argument("--include-dir", action="append", default=[])
    args = parser.parse_args(argv)
    if args.self_test:
        self_test()
        return 0
    errors = check(required_plugins=args.require_plugin, required_sources=args.require_source,
                   include_dirs=args.include_dir)
    if errors:
        for error in errors:
            print("[plugin-boundaries] ERROR: " + error, file=sys.stderr)
        return 1
    if not args.quiet:
        count = sum(1 for name in plugin_roots(ROOT) for path in (ROOT / PLUGIN_DIR / name).rglob("*")
                    if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
        print("[plugin-boundaries] checked %d plugin source/header files" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
