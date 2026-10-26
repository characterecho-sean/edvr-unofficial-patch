#!/usr/bin/env python3
"""A drop-in prefix for a cl.exe command line that skips compiles whose inputs
have not changed, and never reuses a stale output.

    python tools\\compile_cache.py [options] cl.exe <exactly the cl.exe arguments>

Two modes, chosen by whether /c is on the line.

  /c (compile only): per-source caching.  Every source has a manifest beside
  its object (<obj>.ccm).  A source is a hit when the manifest's key matches
  and the object still has the size and mtime recorded when it was stored.
  ONE real cl.exe runs with only the missed sources, so /MP still parallelises
  them; when everything hits, nothing runs.

  compile+link (no /c): whole-invocation caching.  The manifest (<target>.ccm)
  keys every source, every input file named on the line (also after /link:
  .lib/.obj/.res/.def, wildcards expanded) and the flags.  A hit needs every
  recorded output (the /Fe target, a /PDB, a /LD import library) to still match
  its recorded size and mtime.  The objects in /Fo are intermediates nothing
  reads afterwards, so they are NOT checked; this keeps rigs that share one /Fo
  directory (and compile the same file under different defines) from evicting
  each other.

THE KEY.  sha256 over: the resolved cl.exe path plus size+mtime of cl.exe,
c1xx.dll, c2.dll and link.exe; the INCLUDE, LIB, LIBPATH, CL, _CL_, LINK and
_LINK_ environment variables (with /MP tokens removed from CL and _CL_); the
argument list with /MP tokens removed (and, in /c mode, without the source
files, so adding or removing a source does not invalidate the others); the
source's bytes; and the content hash of every header the source includes
EXCEPT headers under a directory named in INCLUDE (system and SDK headers are
covered by the INCLUDE string and the compiler's identity, which keeps hashing
cheap).  The header list comes from the compiler itself: the real cl call gets
/sourceDependencies <tmpdir>, which writes one JSON per source.  It works with
/MP on this toolchain (MSVC 14.44).  A later run recomputes the key from the
stored list; a header that no longer exists is a miss.

SAFETY.  A manifest is deleted BEFORE its output is rebuilt and written only
after cl exits 0, so a failed or interrupted compile cannot leave a hit.  A
manifest is also not written when any input is newer than the compile's start
(it may have changed under the compiler).  Manifests are per output, so
concurrent invocations share no writable state.  The exit code is cl's own,
including negative NTSTATUS crash codes.  A command line the tool cannot model
(response files, /Tp, /Tc, /P, /E, /Zi, /Yc, /Fa ...) is passed straight
through, uncached, with a note.

LIMITS, stated plainly.  A cache hit does not replay the compiler's warnings.
A new header that would now shadow an old one earlier on the include path is
not noticed until the includer changes.  __DATE__/__TIME__ in an included
header are frozen at the last real compile of that translation unit.

Options (before the compiler):
  --prune        in /c mode, delete *.obj (and their .ccm) in the /Fo directory
                 that no listed source produces: an object of a source REMOVED
                 from the list must not link back in.
  --keep NAME    with --prune, leave NAME alone (e.g. an ml64-made thunks.obj).
  --dry-run      print hit/miss decisions; write nothing, run no compiler.
  --off          pass straight through (also env EDVR_NO_COMPILE_CACHE=1).
                 With --prune this still deletes every *.obj in the /Fo dir
                 except the --keep names, which is what `del /q obj\\x\\*.obj`
                 did minus the assembler output the compiler cannot recreate.
  --self-test    fast tests with a fake compiler; no real compiler needed.
"""

import builtins
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SCHEMA = 1
SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".c++")
MP_TOKEN = re.compile(r"(?i)(?<!\S)[/-]MP\d*(?!\S)")
# Options whose value may be given as the NEXT argument, which this parser does
# not model: such a line is passed through uncached rather than misread.
SEPARATE_VALUE_OPTIONS = {"/i", "/d", "/fe", "/fo", "/fd", "/fp", "/fi", "/u", "/yu", "/yc",
                          "/sourcedependencies", "/ai", "/external:i", "/imsvc"}
# Options that produce files this tool does not track, or change what cl does.
UNCACHEABLE_PREFIXES = ("/tp", "/tc", "/zi", "/yc", "/yu", "/fa", "/fm", "/fr", "/fd", "/fp",
                        "/showincludes", "/sourcedependencies", "/scandependencies", "/ifcoutput",
                        "/experimental:module", "/headerunit")
UNCACHEABLE_EXACT = {"/p", "/e", "/ep"}
# Linker options that name an OUTPUT; they are not inputs to hash.
LINK_OUTPUT_OPTIONS = {"pdb", "out", "implib", "map", "ilk", "pgd", "pdbstripped", "manifestfile"}
LINK_UNCACHEABLE = {"implib", "map"}
ENV_KEYS = ("INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_")
IDENTITY_FILES = ("cl.exe", "c1xx.dll", "c2.dll", "link.exe")


def say(text):
    print(text)
    sys.stdout.flush()


# ---------------------------------------------------------------- parsing

class Parsed:
    def __init__(self):
        self.compiler = None
        self.args = []            # everything after the compiler, verbatim
        self.flags = []           # option arguments before /link, /MP tokens removed
        self.sources = []         # source arguments
        self.source_index = []    # their index in args
        self.inputs = []          # other non-option arguments before /link
        self.link_args = []       # everything after /link
        self.compile_only = False
        self.fo = None
        self.fe = None
        self.ld = False
        self.pdb = None
        self.out = None           # /OUT: after /link, which wins over /Fe
        self.uncacheable = None


def _strip_colon(value):
    return value[1:] if value.startswith(":") else value


def parse(compiler, args):
    p = Parsed()
    p.compiler = compiler
    p.args = list(args)
    in_link = False
    for index, a in enumerate(args):
        if in_link:
            p.link_args.append(a)
            low = a.lower()
            if low[:1] in "/-" and low[1:].split(":", 1)[0] in LINK_UNCACHEABLE:
                p.uncacheable = "linker option %s names an output" % a
            if low.startswith(("/pdb:", "-pdb:")):
                p.pdb = a[5:]
            if low.startswith(("/out:", "-out:")):
                p.out = a[5:].strip('"')
            continue
        if a.startswith("@"):
            p.uncacheable = "response file %s" % a
            continue
        low = a.lower()
        if low in ("/link", "-link"):
            in_link = True
            continue
        if a[:1] in "/-" and len(a) > 1:
            if MP_TOKEN.fullmatch(a):
                continue
            if low == "/c":
                p.compile_only = True
            elif low.startswith("/fo"):
                p.fo = _strip_colon(a[3:])
            elif low.startswith("/fe"):
                p.fe = _strip_colon(a[3:])
            elif low in ("/ld", "/ldd"):
                p.ld = True
            canon = "/" + low[1:]
            if canon in SEPARATE_VALUE_OPTIONS:
                p.uncacheable = "option %s takes its value as the next argument" % a
            elif canon in UNCACHEABLE_EXACT or canon.startswith(UNCACHEABLE_PREFIXES):
                p.uncacheable = "option %s is not modelled" % a
            p.flags.append(a)
        elif low.endswith(SOURCE_EXTENSIONS):
            p.sources.append(a)
            p.source_index.append(index)
        else:
            p.inputs.append(a)
    if not p.sources:
        p.uncacheable = p.uncacheable or "no source files on the line"
    if p.fo and not _is_dir_spec(p.fo) and len(p.sources) > 1:
        p.uncacheable = p.uncacheable or "/Fo names one file but there are several sources"
    return p


def _is_dir_spec(spec):
    return spec.endswith(("\\", "/")) or os.path.isdir(spec)


def object_path(p, source):
    stem = os.path.splitext(os.path.basename(source))[0]
    if not p.fo:
        return stem + ".obj"
    if _is_dir_spec(p.fo):
        return os.path.join(p.fo, stem + ".obj")
    return p.fo


def link_outputs(p):
    """The files a compile+link line is expected to produce and that a hit must find intact."""
    stem = os.path.splitext(os.path.basename(p.sources[0]))[0]
    ext = ".dll" if p.ld else ".exe"
    fe = p.fe
    if not fe:
        target = stem + ext
    elif _is_dir_spec(fe):
        target = os.path.join(fe, stem + ext)
    elif not os.path.splitext(fe)[1]:
        target = fe + ext
    else:
        target = fe
    if p.out:
        target = p.out
    outputs = [target]
    if p.pdb:
        outputs.append(p.pdb.strip('"'))
    return outputs


# ---------------------------------------------------------------- hashing

def strip_mp(text):
    return " ".join(MP_TOKEN.sub(" ", text or "").split())


def file_hash(path, memo):
    key = os.path.normcase(os.path.abspath(path))
    if key in memo:
        return memo[key]
    try:
        with open(path, "rb") as handle:
            value = hashlib.sha256(handle.read()).hexdigest()
    except OSError:
        value = None
    memo[key] = value
    return value


def stat_pair(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


def identity_part(compiler):
    resolved = shutil.which(compiler) or compiler
    folder = os.path.dirname(os.path.abspath(resolved))
    parts = [os.path.normcase(os.path.abspath(resolved)), stat_pair(resolved)]
    for name in IDENTITY_FILES:
        parts.append([name, stat_pair(os.path.join(folder, name))])
    return digest(parts)


def env_part(environ):
    values = {}
    for name in ENV_KEYS:
        value = environ.get(name, "")
        if name in ("CL", "_CL_"):
            value = strip_mp(value)
        values[name] = value
    return digest(values)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode("utf-8")).hexdigest()


def system_dirs(environ):
    out = []
    for entry in environ.get("INCLUDE", "").split(";"):
        entry = entry.strip().strip('"')
        if entry:
            out.append(os.path.normcase(os.path.abspath(entry)).rstrip("\\/") + os.sep)
    return out


def is_system(path, sysdirs):
    norm = os.path.normcase(os.path.abspath(path))
    return any(norm.startswith(d) for d in sysdirs)


def rel(path):
    try:
        r = os.path.relpath(path)
    except ValueError:
        return path
    return r if not r.startswith("..") else path


# ---------------------------------------------------------------- manifests

def load_manifest(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError:
        return None, "no manifest"
    except (OSError, ValueError):
        return None, "manifest unreadable"
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        return None, "manifest from another version"
    return data, None


def write_manifest(path, data):
    temp = "%s.tmp%d" % (path, os.getpid())
    with open(temp, "w", encoding="utf-8") as handle:
        json.dump(data, handle, sort_keys=True, indent=0)
    os.replace(temp, path)


def remove_quiet(path):
    try:
        os.remove(path)
    except OSError:
        pass


def check_hashes(recorded, label, memo, current=None):
    """Compare recorded {path: hash} to the files now; return a reason or None."""
    for path, old in recorded.items():
        new = file_hash(path, memo)
        if new is None:
            return "%s %s no longer exists" % (label, rel(path))
        if new != old:
            return "%s %s changed" % (label, rel(path))
    return None


def output_hashes(paths):
    """{path: sha256} of the outputs now on disk.  Recorded beside their size and mtime so that
    unpack_store can tell a restored file is byte for byte the one cl wrote."""
    memo = {}
    return {path: value for path in paths for value in [file_hash(path, memo)] if value is not None}


def check_outputs(recorded):
    for path, pair in recorded.items():
        now = stat_pair(path)
        if now is None:
            return "output %s missing" % rel(path)
        if now != pair:
            return "output %s replaced since it was stored" % rel(path)
    return None


def compare_parts(data, expect):
    """Name the first part that differs; None if all equal."""
    names = (("cl", "compiler changed"), ("env", "INCLUDE/LIB/CL environment changed"),
             ("flags", "flags changed"), ("src", "source changed"))
    for name, message in names:
        if data.get("parts", {}).get(name) != expect[name]:
            return message
    return None


# ---------------------------------------------------------------- the work

class Context:
    def __init__(self, environ, runner, dry_run, out):
        self.environ = environ
        self.runner = runner
        self.dry_run = dry_run
        self.out = out
        self.memo = {}
        self.sysdirs = system_dirs(environ)
        self.started = time.time()


def read_dependency_files(directory):
    """{normcased source path: [include paths]} from the JSON files /sourceDependencies wrote."""
    found = {}
    try:
        names = os.listdir(directory)
    except OSError:
        return found
    for name in names:
        if not name.lower().endswith(".json"):
            continue
        try:
            with open(os.path.join(directory, name), "r", encoding="utf-8") as handle:
                data = json.load(handle)
            body = data["Data"]
            found[os.path.normcase(os.path.abspath(body["Source"]))] = list(body.get("Includes", []))
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return found


def real_compile(ctx, p, args, tmpdir):
    args = list(args)
    # Before /link: everything after it belongs to the linker.
    at = next((i for i, a in enumerate(args) if a.lower() in ("/link", "-link")), len(args))
    command = [p.compiler] + args[:at] + ["/sourceDependencies", tmpdir] + args[at:]
    return ctx.runner(command, ctx.environ)


def signed(code):
    code = int(code)
    return code - (1 << 32) if code >= (1 << 31) else code


def fs_now_ns(tmpdir):
    """The file system's own clock: the mtime of a file made right now.  Inputs are compared
    against it, not against time.time_ns(), because file times come from a coarser clock."""
    path = os.path.join(tmpdir, "stamp")
    with open(path, "w") as handle:
        handle.write("")
    return os.stat(path).st_mtime_ns


def finished_deps(ctx, includes):
    """({path: hash}, newest_ns) for the non-system includes, hashed now."""
    deps = {}
    newest = 0
    for inc in includes:
        if is_system(inc, ctx.sysdirs):
            continue
        digest_value = file_hash(inc, ctx.memo)
        if digest_value is None:
            return None, 0
        deps[inc] = digest_value
        pair = stat_pair(inc)
        if pair:
            newest = max(newest, pair[1])
    return deps, newest


def run_compile_only(ctx, p, prune, keep):
    started = time.time()
    objs = {s: object_path(p, s) for s in p.sources}
    listed = {os.path.normcase(os.path.basename(o)) for o in objs.values()}
    name = os.path.basename(os.path.normpath(p.fo)) if p.fo and _is_dir_spec(p.fo) else "compile"
    if prune:
        prune_directory(ctx, p, listed, keep, everything=False)
    cl_part = identity_part(p.compiler)
    env_p = env_part(ctx.environ)
    flags_p = digest(["c", p.flags, p.inputs])
    decisions = {}
    for source in p.sources:
        src_h = file_hash(source, ctx.memo)
        expect = {"cl": cl_part, "env": env_p, "flags": flags_p, "src": digest([source, src_h])}
        if src_h is None:
            decisions[source] = (False, "source missing", expect)
            continue
        data, why = load_manifest(objs[source] + ".ccm")
        if data is None:
            decisions[source] = (False, why, expect)
            continue
        why = compare_parts(data, expect)
        if why is None:
            why = check_hashes(data.get("deps", {}), "header", ctx.memo)
        if why is None:
            why = check_outputs(data.get("outputs", {}))
        decisions[source] = (why is None, why, expect)
    missed = [s for s in p.sources if not decisions[s][0]]
    hits = len(p.sources) - len(missed)
    if ctx.dry_run:
        for s in p.sources:
            ok, why, _ = decisions[s]
            ctx.out("[edvr] compile_cache: dry-run: %s: %s" % (rel(s), "hit" if ok else "MISS (%s)" % why))
        return 0
    if not missed:
        ctx.out("[edvr] compile_cache: %s: 0 compiled, %d reused (%.1f s)" % (name, hits, time.time() - started))
        return 0
    for s in missed:
        remove_quiet(objs[s] + ".ccm")
        remove_quiet(objs[s])
    drop = {p.source_index[i] for i, s in enumerate(p.sources) if decisions[s][0]}
    args = [a for i, a in enumerate(p.args) if i not in drop]
    tmpdir = tempfile.mkdtemp(prefix="edvr-cc-")
    t_start_ns = fs_now_ns(tmpdir)
    try:
        rc = real_compile(ctx, p, args, tmpdir)
        rc = signed(rc)
        stored = 0
        if rc == 0:
            deps_by_source = read_dependency_files(tmpdir)
            for s in missed:
                includes = deps_by_source.get(os.path.normcase(os.path.abspath(s)))
                out_pair = stat_pair(objs[s])
                if includes is None or out_pair is None:
                    continue
                deps, newest = finished_deps(ctx, includes)
                src_pair = stat_pair(s)
                if deps is None or newest >= t_start_ns or (src_pair and src_pair[1] >= t_start_ns):
                    continue
                write_manifest(objs[s] + ".ccm", {
                    "schema": SCHEMA, "kind": "obj", "source": s, "parts": decisions[s][2],
                    "key": digest([decisions[s][2], deps]), "deps": deps,
                    "outputs": {objs[s]: out_pair}, "hashes": output_hashes([objs[s]])})
                stored += 1
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    ctx.out("[edvr] compile_cache: %s: %d compiled, %d reused (%.1f s)%s" % (
        name, len(missed), hits, time.time() - started, "" if rc == 0 else "  [cl exit %d]" % rc))
    for s in missed[:3]:
        ctx.out("[edvr] compile_cache:   miss %s: %s" % (rel(s), decisions[s][1]))
    if len(missed) > 3:
        ctx.out("[edvr] compile_cache:   ... and %d more" % (len(missed) - 3))
    return rc


def prune_directory(ctx, p, listed, keep, everything):
    if not (p.fo and _is_dir_spec(p.fo)) or not os.path.isdir(p.fo):
        return
    keep_set = {os.path.normcase(k) for k in keep}
    removed = []
    for entry in sorted(os.listdir(p.fo)):
        low = os.path.normcase(entry)
        base = low[:-4] if low.endswith(".ccm") else low
        if not base.endswith(".obj"):
            continue
        if base in keep_set or (not everything and base in listed):
            continue
        removed.append(entry)
    for entry in removed:
        if ctx.dry_run:
            ctx.out("[edvr] compile_cache: dry-run: would prune %s" % rel(os.path.join(p.fo, entry)))
        else:
            remove_quiet(os.path.join(p.fo, entry))
    if removed and not ctx.dry_run:
        ctx.out("[edvr] compile_cache: pruned %d stale object file(s) from %s" % (len(removed), rel(p.fo)))


def link_input_files(p):
    """Existing files named on the line (inputs and linker arguments), wildcards expanded, sorted."""
    candidates = []
    for token in p.inputs:
        candidates.append(token)
    for token in p.link_args:
        if token[:1] in "/-" and len(token) > 1:
            head, sep, value = token[1:].partition(":")
            if head.lower() in LINK_OUTPUT_OPTIONS or not sep:
                continue
            candidates.append(value.strip('"'))
        else:
            candidates.append(token)
    files = set()
    for token in candidates:
        if not token:
            continue
        if any(c in token for c in "*?"):
            files.update(glob.glob(token))
        elif os.path.isfile(token):
            files.add(token)
    return sorted(files)


def run_link(ctx, p):
    started = time.time()
    outputs = link_outputs(p)
    target = outputs[0]
    name = os.path.basename(target)
    manifest = target + ".ccm"
    cl_part = identity_part(p.compiler)
    env_p = env_part(ctx.environ)
    flags_p = digest(["l", p.flags, p.inputs, p.link_args])
    src_hashes = {s: file_hash(s, ctx.memo) for s in p.sources}
    inputs = {}
    for f in link_input_files(p):
        inputs[os.path.normcase(os.path.abspath(f))] = file_hash(f, ctx.memo)
    expect = {"cl": cl_part, "env": env_p, "flags": flags_p, "src": digest(src_hashes)}
    why = None
    if any(h is None for h in src_hashes.values()):
        why = "source missing"
    else:
        data, why = load_manifest(manifest)
        if data is not None:
            why = compare_parts(data, expect)
            if why is None and data.get("inputs", {}) != inputs:
                old = data.get("inputs", {})
                changed = [k for k in set(old) | set(inputs) if old.get(k) != inputs.get(k)]
                why = "input %s changed" % rel(sorted(changed)[0])
            if why is None:
                why = check_hashes(data.get("deps", {}), "header", ctx.memo)
            if why is None:
                why = check_outputs(data.get("outputs", {}))
    if ctx.dry_run:
        ctx.out("[edvr] compile_cache: dry-run: %s: %s" % (name, "hit" if why is None else "MISS (%s)" % why))
        return 0
    if why is None:
        ctx.out("[edvr] compile_cache: %s: reused (%.1f s)" % (name, time.time() - started))
        return 0
    remove_quiet(manifest)
    tmpdir = tempfile.mkdtemp(prefix="edvr-cc-")
    t_start_ns = fs_now_ns(tmpdir)
    try:
        rc = signed(real_compile(ctx, p, p.args, tmpdir))
        if rc == 0:
            by_source = read_dependency_files(tmpdir)
            deps = {}
            newest = 0
            complete = True
            for s in p.sources:
                includes = by_source.get(os.path.normcase(os.path.abspath(s)))
                if includes is None:
                    complete = False
                    break
                got, n = finished_deps(ctx, includes)
                if got is None:
                    complete = False
                    break
                deps.update(got)
                newest = max(newest, n)
            extra = list(outputs)
            if p.ld:
                stem = os.path.splitext(target)[0]
                extra += [stem + ".lib", stem + ".exp"]
            recorded = {}
            for path in extra:
                pair = stat_pair(path)
                if pair is not None:
                    recorded[path] = pair
            if target not in recorded:
                complete = False
            stamps = [stat_pair(s) for s in p.sources] + [stat_pair(f) for f in link_input_files(p)]
            if any(pair and pair[1] >= t_start_ns for pair in stamps):
                complete = False
            if complete and newest < t_start_ns:
                write_manifest(manifest, {
                    "schema": SCHEMA, "kind": "link", "parts": expect,
                    "key": digest([expect, deps, inputs]), "deps": deps, "inputs": inputs,
                    "outputs": recorded, "hashes": output_hashes(recorded)})
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    ctx.out("[edvr] compile_cache: %s: built (%.1f s)%s" % (name, time.time() - started,
                                                            "" if rc == 0 else "  [cl exit %d]" % rc))
    ctx.out("[edvr] compile_cache:   miss: %s" % why)
    return rc


def execute(argv, environ, runner, out=say, dry_run=False, off=False, prune=False, keep=()):
    """argv = [compiler, cl arguments...].  Returns the exit code."""
    if not argv:
        raise SystemExit("compile_cache: no compiler given")
    p = parse(argv[0], argv[1:])
    ctx = Context(environ, runner, dry_run, out)
    if off or environ.get("EDVR_NO_COMPILE_CACHE") == "1" or p.uncacheable:
        if p.uncacheable and not off:
            out("[edvr] compile_cache: passthrough, uncached: %s" % p.uncacheable)
        if prune and p.compile_only:
            prune_directory(ctx, p, set(), keep, everything=True)
        if dry_run:
            out("[edvr] compile_cache: dry-run: passthrough, nothing run")
            return 0
        return signed(runner([p.compiler] + p.args, environ))
    if p.compile_only:
        return run_compile_only(ctx, p, prune, keep)
    return run_link(ctx, p)


# ---------------------------------------------------------------- carrying the cache between machines
#
# A manifest hits only while its outputs still have the size and mtime it recorded, and a file
# restored from an archive has neither guarantee (tar keeps a time, but to a coarser unit than the
# file system's).  pack_store and unpack_store move the cache through a directory an archive
# service (actions/cache) can carry, and re-establish the mtime from the manifest only for a file
# whose bytes are the ones cl wrote: the manifest records a sha256 of every output.  Nothing else
# about a hit changes: the key (sources, headers, flags, compiler identity, INCLUDE) is still
# compared on every use, so a restored entry for an input that has changed is simply a miss.
#
# store/m/<path>.ccm   a manifest, at its place under the root
# store/f/<path>       an output a manifest names, at its place under the root
# store/x/<path>       a file a caller asked for with --extra, carried as it is
STORE_MANIFESTS, STORE_FILES, STORE_EXTRA = "m", "f", "x"


def under(root, path):
    """`path` relative to `root`, or None when it is not inside it."""
    try:
        r = os.path.relpath(os.path.abspath(path), os.path.abspath(root))
    except ValueError:
        return None
    if r == "." or r == ".." or r.startswith(".." + os.sep) or os.path.isabs(r):
        return None
    return r


def manifests_under(folder, skip=None):
    """Every *.ccm under `folder`, sorted; `skip` is a directory not to descend into."""
    found = []
    skip = os.path.normcase(os.path.abspath(skip)) if skip else None
    for here, dirs, files in os.walk(folder):
        if skip:
            dirs[:] = [d for d in dirs if os.path.normcase(os.path.abspath(os.path.join(here, d))) != skip]
        found.extend(os.path.join(here, n) for n in files if n.endswith(".ccm"))
    return sorted(found)


def place_file(src, dst, link=False):
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    remove_quiet(dst)
    if link:
        try:
            os.link(src, dst)
            return
        except OSError:
            pass
        shutil.copy2(src, dst)
        return
    try:
        os.replace(src, dst)
    except OSError:
        shutil.copy2(src, dst)
        remove_quiet(src)


def tidy_reasons(reasons):
    return ", ".join("%d %s" % (n, why) for why, n in sorted(reasons.items(), key=lambda kv: (-kv[1], kv[0])))


def pack_store(root, store, extras=(), dry_run=False, out=say):
    """Copy every manifest under `root` whose outputs are still exactly what it recorded, and
    those outputs, into `store` (emptied first).  Files are hard-linked when the volume allows.
    Returns {"packed": n, "files": n, "skipped": {reason: n}}."""
    root = os.path.abspath(root)
    packed = files = size = 0
    skipped = {}
    if not dry_run:
        shutil.rmtree(store, ignore_errors=True)
        os.makedirs(store, exist_ok=True)
    for path in manifests_under(root, skip=store):
        data, why = load_manifest(path)
        reason = None
        names = []
        if data is None:
            reason = why
        elif not isinstance(data.get("outputs"), dict) or not isinstance(data.get("hashes"), dict):
            reason = "recorded before outputs were hashed"
        else:
            for name, pair in data["outputs"].items():
                if under(root, name) is None:
                    reason = "an output outside the root"
                elif stat_pair(name) != pair:
                    reason = "an output no longer as recorded"
                elif output_hashes([name]).get(name) != data["hashes"].get(name):
                    reason = "an output whose bytes changed"
                if reason:
                    break
                names.append(name)
        if reason:
            skipped[reason] = skipped.get(reason, 0) + 1
            continue
        packed += 1
        files += len(names)
        size += sum(data["outputs"][n][0] for n in names)
        if dry_run:
            continue
        for name in names:
            place_file(name, os.path.join(store, STORE_FILES, under(root, name)), link=True)
        place_file(path, os.path.join(store, STORE_MANIFESTS, under(root, path)), link=True)
    carried = 0
    for extra in extras:
        if os.path.isfile(os.path.join(root, extra)):
            carried += 1
            if not dry_run:
                place_file(os.path.join(root, extra), os.path.join(store, STORE_EXTRA, extra), link=True)
    out("[edvr] compile_cache: %s%d entries (%d files, %.1f MB) from %s into %s%s%s" % (
        "dry run, would pack " if dry_run else "packed ", packed, files, size / 1e6, rel_or_abs(root), rel_or_abs(store),
        (", %d extra file(s)" % carried) if extras else "",
        ("; skipped " + tidy_reasons(skipped)) if skipped else ""))
    return {"packed": packed, "files": files, "skipped": skipped, "extra": carried}


def rel_or_abs(path):
    return rel(os.path.abspath(path))


def unpack_store(root, store, extras=(), dry_run=False, out=say):
    """Put the entries of `store` (made by pack_store) back under `root`.  An entry is installed
    only if every output it names is in the store with the recorded size and sha256; its mtime is
    set to the recorded one and its manifest is written last.  An entry that does not verify, or
    that names a path outside `root`, is left out.  Files are moved out of the store.
    Returns {"restored": n, "files": n, "rejected": {reason: n}}."""
    root = os.path.abspath(root)
    restored = files = size = 0
    rejected = {}
    manifest_dir = os.path.join(store, STORE_MANIFESTS)
    for path in manifests_under(manifest_dir) if os.path.isdir(manifest_dir) else []:
        data, why = load_manifest(path)
        reason = None
        plan = []
        if data is None:
            reason = why
        elif not isinstance(data.get("outputs"), dict) or not isinstance(data.get("hashes"), dict) or not data["outputs"]:
            reason = "recorded before outputs were hashed"
        else:
            for name, pair in data["outputs"].items():
                inside = under(root, name)
                held = os.path.join(store, STORE_FILES, inside) if inside else None
                if inside is None:
                    reason = "an output outside the root"
                elif not isinstance(pair, list) or len(pair) != 2:
                    reason = "an output with no size and time recorded"
                elif not os.path.isfile(held):
                    reason = "an output missing from the store"
                elif os.path.getsize(held) != pair[0]:
                    reason = "an output of the wrong size"
                elif output_hashes([held]).get(held) != data["hashes"].get(name):
                    reason = "an output whose bytes differ from what cl wrote"
                if reason:
                    break
                plan.append((name, held, pair))
        if reason:
            rejected[reason] = rejected.get(reason, 0) + 1
            continue
        restored += 1
        files += len(plan)
        size += sum(pair[0] for _, _, pair in plan)
        if dry_run:
            continue
        for name, held, pair in plan:
            place_file(held, name)
            os.utime(name, ns=(pair[1], pair[1]))
        if all(stat_pair(name) == pair for name, _, pair in plan):
            target = os.path.join(root, os.path.relpath(path, manifest_dir))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            write_manifest(target, data)
        else:
            for name, _, _ in plan:
                remove_quiet(name)
            restored -= 1
            files -= len(plan)
            size -= sum(pair[0] for _, _, pair in plan)
            rejected["a restored output that would not take its recorded time"] = \
                rejected.get("a restored output that would not take its recorded time", 0) + 1
    carried = 0
    for extra in extras:
        held, dest = os.path.join(store, STORE_EXTRA, extra), os.path.join(root, extra)
        if os.path.isfile(held) and not os.path.exists(dest):
            carried += 1
            if not dry_run:
                place_file(held, dest)
    out("[edvr] compile_cache: %s%d entries (%d files, %.1f MB) from %s under %s%s%s" % (
        "dry run, would restore " if dry_run else "restored ", restored, files, size / 1e6, rel_or_abs(store), rel_or_abs(root),
        (", %d extra file(s)" % carried) if extras else "",
        ("; left out " + tidy_reasons(rejected)) if rejected else ""))
    return {"restored": restored, "files": files, "rejected": rejected, "extra": carried}


# ---------------------------------------------------------------- reading a build log back

SUMMARY_GROUP = re.compile(r"^\[edvr\] compile_cache: (\S+): (\d+) compiled, (\d+) reused \(([\d.]+) s\)")
SUMMARY_LINK = re.compile(r"^\[edvr\] compile_cache: (\S+): (built|reused) \(([\d.]+) s\)")
SUMMARY_MISS = re.compile(r"^\[edvr\] compile_cache:\s+miss(?: \S+)?: (.*)$")


def summarize_log(text):
    """Totals from the compile_cache lines of a build's stdout: how many compiles ran and how
    many were reused, the seconds the compiles took, and why the first misses of each group
    missed (the tool names only the first three of a group).  Returns a dict."""
    groups = [0, 0, 0.0]      # sources compiled, reused, seconds
    links = {"built": [0, 0.0], "reused": [0, 0.0]}
    reasons = {}
    seen = False
    for line in text.splitlines():
        m = SUMMARY_GROUP.match(line)
        if m:
            seen = True
            groups[0] += int(m.group(2))
            groups[1] += int(m.group(3))
            groups[2] += float(m.group(4))
            continue
        m = SUMMARY_LINK.match(line)
        if m:
            seen = True
            links[m.group(2)][0] += 1
            links[m.group(2)][1] += float(m.group(3))
            continue
        m = SUMMARY_MISS.match(line)
        if m:
            reason = re.sub(r"\S+\.(?:cpp|cc|cxx|c|h|hpp|inc|lib|obj|exe|dll|def|res)\b", "<file>", m.group(1))
            reasons[reason] = reasons.get(reason, 0) + 1
    return {"seen": seen, "sources_compiled": groups[0], "sources_reused": groups[1], "group_seconds": groups[2],
            "links_built": links["built"][0], "links_built_seconds": links["built"][1],
            "links_reused": links["reused"][0], "links_reused_seconds": links["reused"][1], "reasons": reasons}


def summary_text(totals):
    if not totals["seen"]:
        return "compile_cache: no compile_cache lines in the log (the cache was off, or this is not a build log)"
    lines = ["compile_cache: per-source compiles: %d compiled, %d reused (%.0f s inside those invocations)" % (
                 totals["sources_compiled"], totals["sources_reused"], totals["group_seconds"]),
             "compile_cache: compile+link invocations: %d built (%.0f s), %d reused (%.0f s)" % (
                 totals["links_built"], totals["links_built_seconds"], totals["links_reused"], totals["links_reused_seconds"])]
    if totals["reasons"]:
        lines.append("compile_cache: why the misses missed (first three of each invocation): " + tidy_reasons(totals["reasons"]))
    return "\n".join(lines)


def real_runner(command, environ):
    return subprocess.call(command, env=environ)


def store_main(argv):
    """--pack STORE / --unpack STORE [--root DIR] [--extra FILE]... [--dry-run], and --summary LOG
    [--markdown FILE]: the commands around a cache archive, not a compiler wrapper."""
    import argparse
    ap = argparse.ArgumentParser(prog="compile_cache.py", add_help=True)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--pack", metavar="STORE", help="copy the valid cache entries under --root into STORE (emptied first)")
    mode.add_argument("--unpack", metavar="STORE", help="restore the entries in STORE under --root, verified")
    mode.add_argument("--summary", metavar="LOG", help="print the hit/miss totals of a build's stdout log")
    ap.add_argument("--root", default="build", help="the build directory (default: build)")
    ap.add_argument("--extra", action="append", default=[], metavar="FILE",
                    help="a file under --root to carry as it is (it must validate itself); repeatable")
    ap.add_argument("--markdown", metavar="FILE", help="with --summary, append the totals to FILE (a job summary)")
    ap.add_argument("--dry-run", action="store_true", help="say what would happen; write nothing")
    args = ap.parse_args(argv)
    if args.summary:
        with open(args.summary, "r", encoding="utf-8", errors="replace") as handle:
            text = summary_text(summarize_log(handle.read()))
        say(text)
        if args.markdown and not args.dry_run:
            with open(args.markdown, "a", encoding="utf-8") as handle:
                handle.write("### Compile cache\n\n```\n%s\n```\n" % text)
        return 0
    if args.pack:
        pack_store(args.root, args.pack, args.extra, args.dry_run)
    else:
        unpack_store(args.root, args.unpack, args.extra, args.dry_run)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("--pack", "--unpack", "--summary"):
        return store_main(argv)
    opts = {"dry_run": False, "off": False, "prune": False, "keep": []}
    while argv and argv[0].startswith("--"):
        flag = argv.pop(0)
        if flag == "--self-test":
            return self_test()
        if flag == "--dry-run":
            opts["dry_run"] = True
        elif flag == "--off":
            opts["off"] = True
        elif flag == "--prune":
            opts["prune"] = True
        elif flag == "--keep" and argv:
            opts["keep"].append(argv.pop(0))
        elif flag == "--":
            break
        else:
            print("compile_cache: unknown option %s" % flag, file=sys.stderr)
            return 64
    if not argv:
        print("usage: compile_cache.py [--dry-run|--off|--prune|--keep NAME] cl.exe <cl arguments>\n"
              "       compile_cache.py --pack|--unpack STORE [--root DIR] [--extra FILE]... [--dry-run]\n"
              "       compile_cache.py --summary LOG [--markdown FILE]", file=sys.stderr)
        return 64
    return execute(argv, dict(os.environ), real_runner, **opts)


# ---------------------------------------------------------------- self-test

class FakeCl:
    """Stands in for cl.exe: writes objects, an exe, and /sourceDependencies JSON in the
    format the real compiler writes (Version 1.2, lowercased absolute paths)."""

    def __init__(self):
        self.calls = []
        self.compiled = []
        self.exit_code = None

    def __call__(self, command, environ):
        self.calls.append(list(command))
        args = command[1:]
        sources, include_dirs, fo, fe, deps_dir, compile_only = [], [], None, None, None, False
        i = 0
        while i < len(args):
            a = args[i]
            if a.lower() == "/sourcedependencies":
                deps_dir = args[i + 1]
                i += 1
            elif a == "/c":
                compile_only = True
            elif a.startswith("/Fo"):
                fo = a[3:]
            elif a.startswith("/Fe"):
                fe = a[3:]
            elif a.startswith("/I"):
                include_dirs.append(a[2:])
            elif a.lower() == "/link":
                break
            elif not a.startswith("/") and a.lower().endswith(".cpp"):
                sources.append(a)
            i += 1
        system = [d for d in environ.get("INCLUDE", "").split(";") if d]
        fail = False
        for s in sources:
            text = open(s, encoding="utf-8").read()
            self.compiled.append(os.path.basename(s))
            if "FAIL" in text:
                fail = True
                continue
            seen = []
            self._scan(s, text, include_dirs, system, seen)
            stem = os.path.splitext(os.path.basename(s))[0]
            obj = os.path.join(fo, stem + ".obj") if fo else stem + ".obj"
            with open(obj, "w", encoding="utf-8") as handle:
                handle.write("OBJ %s %s %s" % (text, " ".join(args), time.time_ns()))
            if deps_dir:
                body = {"Version": "1.2", "Data": {
                    "Source": os.path.abspath(s).lower(), "ProvidedModule": "",
                    "Includes": [os.path.abspath(x).lower() for x in seen]}}
                with open(os.path.join(deps_dir, os.path.basename(s) + ".json"), "w", encoding="utf-8") as handle:
                    json.dump(body, handle)
        if self.exit_code is not None:
            return self.exit_code
        if fail:
            return 2
        if not compile_only:
            target = fe if fe else os.path.splitext(os.path.basename(sources[0]))[0] + ".exe"
            with open(target, "w", encoding="utf-8") as handle:
                handle.write("EXE %s %s" % (" ".join(args), time.time_ns()))
        return 0

    def _scan(self, path, text, include_dirs, system, seen):
        for match in re.finditer(r'#include\s*([<"])([^>"]+)[>"]', text):
            kind, name = match.groups()
            dirs = ([os.path.dirname(path) or "."] if kind == '"' else []) + include_dirs + system
            for d in dirs:
                candidate = os.path.join(d, name)
                if os.path.isfile(candidate):
                    if candidate not in seen:
                        seen.append(candidate)
                        self._scan(candidate, open(candidate, encoding="utf-8").read(), include_dirs, system, seen)
                    break


class Forbidden(AssertionError):
    pass


class forbid_writes:
    """While active, any file write, removal, mkdir, temp dir or subprocess raises."""

    def __enter__(self):
        self.saved = (builtins.open, os.replace, os.remove, os.unlink, os.makedirs, os.mkdir,
                      tempfile.mkdtemp, shutil.rmtree, subprocess.call)
        real_open = builtins.open

        def guarded_open(file, mode="r", *a, **k):
            if any(c in str(mode) for c in "wax+"):
                raise Forbidden("write to %s" % (file,))
            return real_open(file, mode, *a, **k)

        def refuse(*a, **k):
            raise Forbidden("filesystem change %r" % (a,))
        builtins.open = guarded_open
        os.replace = os.remove = os.unlink = os.makedirs = os.mkdir = refuse
        tempfile.mkdtemp = shutil.rmtree = subprocess.call = refuse
        return self

    def __exit__(self, *exc):
        (builtins.open, os.replace, os.remove, os.unlink, os.makedirs, os.mkdir,
         tempfile.mkdtemp, shutil.rmtree, subprocess.call) = self.saved
        return False


def _write(path, text):
    folder = os.path.dirname(path)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    past = time.time_ns() - 10_000_000_000   # inputs older than any compile that follows
    os.utime(path, ns=(past, past))


def self_test():
    here = os.getcwd()
    for body in (_self_test_body, _store_self_test_body):
        root = tempfile.mkdtemp(prefix="edvr-compile-cache-")
        os.chdir(root)
        try:
            body()
        finally:
            os.chdir(here)
            shutil.rmtree(root, ignore_errors=True)
    print("compile_cache: self-test passed")
    return 0


def _self_test_body():
    os.makedirs("src/sub")
    os.makedirs("sys")
    os.makedirs("obj")
    _write("fake_cl.exe", "x")
    _write("sys/s.h", "// system\n")
    _write("src/h.h", "#define H 1\n")
    _write("src/sub/d.h", "#define D 1\n")
    _write("src/a.cpp", '#include "h.h"\n#include <s.h>\nint a;\n')
    _write("src/b.cpp", '#include "h.h"\nint b;\n')
    _write("src/c.cpp", '#include "sub/d.h"\nint c;\n')
    sources = ["src/a.cpp", "src/b.cpp", "src/c.cpp"]
    fake = FakeCl()
    lines = []
    environ = {"INCLUDE": os.path.abspath("sys"), "LIB": "L", "CL": "/MP"}

    def run(extra=(), srcs=sources, env=None, **opts):
        del lines[:]
        fake.compiled = []
        before = len(fake.calls)
        args = ["fake_cl.exe", "/nologo", "/c", "/EHsc", "/Fo" + "obj" + os.sep] + list(extra) + list(srcs)
        rc = execute(args, dict(env or environ), fake, out=lines.append, **opts)
        return rc, sorted(fake.compiled), len(fake.calls) - before

    def expect(condition, message):
        if not condition:
            raise AssertionError("compile_cache self-test: " + message + "\n" + "\n".join(lines))

    rc, compiled, calls = run()
    expect(rc == 0 and compiled == ["a.cpp", "b.cpp", "c.cpp"], "first run compiles everything")
    expect(all(os.path.exists("obj/%s.obj.ccm" % n) for n in "abc"), "manifests written")
    with open("obj/a.obj.ccm", encoding="utf-8") as handle:
        stored = json.load(handle)
    expect(any(k.endswith("h.h") for k in stored["deps"]) and not any(k.endswith("s.h") for k in stored["deps"]),
           "dependency list holds the project header and not the INCLUDE-dir header")
    rc, compiled, calls = run()
    expect(compiled == [] and calls == 0, "unchanged run reuses everything and runs no compiler")
    expect("0 compiled, 3 reused" in lines[-1], "summary line reports reuse")

    _write("src/a.cpp", '#include "h.h"\n#include <s.h>\nint a2;\n')
    rc, compiled, _ = run()
    expect(compiled == ["a.cpp"], "source edit recompiles only that source")
    expect(any("source changed" in l for l in lines), "source edit names its reason")
    run()

    _write("src/h.h", "#define H 2\n")
    rc, compiled, _ = run()
    expect(compiled == ["a.cpp", "b.cpp"], "header edit recompiles its includers only")
    expect(any("header" in l and "h.h" in l for l in lines), "header edit names the header")
    run()

    rc, compiled, _ = run(["/DFOO=1"])
    expect(compiled == ["a.cpp", "b.cpp", "c.cpp"] and any("flags changed" in l for l in lines),
           "define change recompiles all, reason flags")
    run(["/DFOO=1"])
    os.makedirs("sys2")
    inc2 = dict(environ, INCLUDE=os.path.abspath("sys") + ";" + os.path.abspath("sys2"))
    rc, compiled, _ = run(["/DFOO=1"], env=inc2)
    expect(compiled == ["a.cpp", "b.cpp", "c.cpp"], "INCLUDE change recompiles all")
    run(["/DFOO=1"], env=inc2)

    # a header under an INCLUDE directory is covered by the INCLUDE string, not hashed
    _write("sys/s.h", "// system, edited\n")
    rc, compiled, _ = run(["/DFOO=1"], env=inc2)
    expect(compiled == [], "an INCLUDE-dir header edit is by design not hashed")

    # cl identity
    _write("fake_cl.exe", "xyz")
    rc, compiled, _ = run(["/DFOO=1"], env=inc2)
    expect(compiled == ["a.cpp", "b.cpp", "c.cpp"] and any("compiler changed" in l for l in lines),
           "compiler identity change recompiles all")
    run(["/DFOO=1"], env=inc2)

    # /MP tokens do not matter, in args or in CL
    run(["/MP4"], env=inc2)    # drops /DFOO: a real flag change, rebuilds and settles
    rc, compiled, calls = run(["/MP"], env=dict(inc2, CL="/MP16"))
    expect(compiled == [] and calls == 0, "/MP token and CL=/MPn changes do not miss")
    rc, compiled, calls = run([], env=dict(inc2, CL=""))
    expect(compiled == [], "dropping CL=/MP does not miss")

    # outputs deleted or replaced
    os.remove("obj/b.obj")
    rc, compiled, _ = run([], env=inc2)
    expect(compiled == ["b.cpp"] and any("missing" in l for l in lines), "deleted object recompiles it")
    before = os.stat("obj/c.obj")
    with open("obj/c.obj", "w", encoding="utf-8") as handle:
        handle.write("X" * before.st_size)
    os.utime("obj/c.obj", ns=(before.st_mtime_ns + 5_000_000_000, before.st_mtime_ns + 5_000_000_000))
    rc, compiled, _ = run([], env=inc2)
    expect(compiled == ["c.cpp"] and any("replaced" in l for l in lines),
           "replaced object (same size, new mtime) recompiles it")
    rc, compiled, _ = run([], env=inc2)
    expect(compiled == [], "settled after rebuild")

    # a header that no longer exists is a miss
    os.rename("src/h.h", "src/h_moved.h")
    rc, compiled, _ = run([], env=inc2)
    expect(compiled == ["a.cpp", "b.cpp"] and any("no longer exists" in l for l in lines),
           "a vanished header is a miss for its includers")
    os.rename("src/h_moved.h", "src/h.h")
    run([], env=inc2)

    # failure leaves no manifest and a later run does not hit
    _write("src/c.cpp", "FAIL\n")
    rc, compiled, _ = run([], env=inc2)
    expect(rc != 0 and "c.cpp" in compiled, "failing compile returns nonzero")
    expect(not os.path.exists("obj/c.obj.ccm") and not os.path.exists("obj/c.obj"), "failed compile leaves no manifest or object")
    rc, compiled, _ = run([], env=inc2)
    expect("c.cpp" in compiled and rc != 0, "failed source is retried, never a hit")
    fake.exit_code = -1073741819
    _write("src/c.cpp", "int c;\n")
    rc, compiled, _ = run([], env=inc2)
    expect(rc == -1073741819, "NTSTATUS crash code is preserved")
    expect(not os.path.exists("obj/c.obj.ccm"), "a crashed compile leaves no manifest")
    fake.exit_code = None
    unsigned_fake = FakeCl()
    unsigned_fake.exit_code = 3221225477
    rc = execute(["fake_cl.exe", "/c", "/Foobj" + os.sep, "src/c.cpp"], dict(inc2), unsigned_fake, out=lines.append)
    expect(rc == -1073741819, "an unsigned NTSTATUS from the runner becomes the signed code")
    run([], env=inc2)

    # prune
    _write("obj/stale.obj", "old")
    _write("obj/stale.obj.ccm", "{}")
    _write("obj/thunks.obj", "ml64")
    rc, compiled, _ = run([], env=inc2, prune=True, keep=["thunks.obj"])
    expect(not os.path.exists("obj/stale.obj") and not os.path.exists("obj/stale.obj.ccm"), "prune removes an unlisted object and manifest")
    expect(os.path.exists("obj/thunks.obj"), "--keep protects a named object")
    expect(os.path.exists("obj/a.obj") and compiled == [], "prune leaves listed objects (still hits)")
    rc, compiled, _ = run([], srcs=sources[:2], env=inc2, prune=True, keep=["thunks.obj"])
    expect(not os.path.exists("obj/c.obj") and not os.path.exists("obj/c.obj.ccm") and compiled == [],
           "a source removed from the list loses its object")
    rc, compiled, _ = run([], env=inc2, prune=True, keep=["thunks.obj"])
    expect(compiled == ["c.cpp"], "re-adding the source compiles it again")

    # dry run writes and runs nothing
    _write("src/b.cpp", "int b3;\n")
    snapshot = sorted(os.listdir("obj"))
    stamps = {n: os.stat("obj/" + n).st_mtime_ns for n in snapshot}
    with forbid_writes():
        rc, compiled, calls = run([], env=inc2, dry_run=True, prune=True)
    expect(rc == 0 and compiled == [] and calls == 0, "dry-run runs no compiler")
    expect(any("MISS" in l and "b.cpp" in l for l in lines) and any("hit" in l for l in lines), "dry-run prints hit and miss")
    expect(sorted(os.listdir("obj")) == snapshot and all(os.stat("obj/" + n).st_mtime_ns == stamps[n] for n in snapshot),
           "dry-run left the directory untouched")
    run([], env=inc2)

    # --off and the env escape hatch pass straight through
    for kwargs, env in (({"off": True}, inc2), ({}, dict(inc2, EDVR_NO_COMPILE_CACHE="1"))):
        rc, compiled, calls = run([], env=env, **kwargs)
        expect(compiled == ["a.cpp", "b.cpp", "c.cpp"] and calls == 1, "off compiles everything in one call")
        expect("/sourceDependencies" not in " ".join(fake.calls[-1]), "off passes the arguments through unchanged")
    _write("obj/stale2.obj", "old")
    run([], env=inc2, off=True, prune=True)
    expect(not os.path.exists("obj/stale2.obj"), "off with prune still empties stale objects like del *.obj did")
    run([], env=inc2)

    # --keep holds in every bypass path.  build.bat assembles thunks.obj just before the
    # compile; the bypass prune used to delete it (review 2026-10-15 finding 1), and the
    # compiler cannot recreate it, so the link failed with unresolved thunk exports.
    for label, extra, kwargs, env in (
            ("--off", [], {"off": True}, inc2),
            ("EDVR_NO_COMPILE_CACHE", [], {}, dict(inc2, EDVR_NO_COMPILE_CACHE="1")),
            ("an uncacheable line", ["/Zi"], {}, inc2)):
        _write("obj/thunks.obj", "ml64")
        _write("obj/stale3.obj", "old")
        _write("obj/stale3.obj.ccm", "{}")
        rc, compiled, calls = run(extra, env=env, prune=True, keep=["thunks.obj"], **kwargs)
        expect(rc == 0 and calls == 1, "%s with prune and keep still compiles" % label)
        expect(os.path.exists("obj/thunks.obj"), "%s with prune honours --keep" % label)
        expect(not os.path.exists("obj/stale3.obj") and not os.path.exists("obj/stale3.obj.ccm"),
               "%s with prune still removes an unkept object" % label)
        run([], env=inc2)
    # dry-run in bypass mode deletes nothing, kept or not
    _write("obj/stale3.obj", "old")
    with forbid_writes():
        run([], env=inc2, off=True, dry_run=True, prune=True, keep=["thunks.obj"])
    expect(os.path.exists("obj/thunks.obj") and os.path.exists("obj/stale3.obj"), "bypass dry-run prunes nothing")
    os.remove("obj/stale3.obj")
    os.remove("obj/thunks.obj")

    # uncacheable lines pass through
    rc, compiled, calls = run(["/Zi"], env=inc2)
    expect(calls == 1 and any("uncached" in l for l in lines), "an unmodelled option passes through with a note")
    _write("r.rsp", "/nologo")
    rc, compiled, calls = run(["@r.rsp"], env=inc2)
    expect(calls == 1 and any("response file" in l for l in lines), "a response file passes through")

    # compile+link mode
    _write("rig/lib.lib", "lib v1")
    _write("rig/x.def", "EXPORTS")
    os.makedirs("out")
    link = ["fake_cl.exe", "/nologo", "/O2", "/Foobj" + os.sep, "/Fe" + os.path.join("out", "rig.exe"), "src/a.cpp", "src/b.cpp",
            "/link", "/INCREMENTAL:NO", "rig/lib.lib", "/DEF:rig/x.def", "kernel32.lib"]

    def run_link_case(**opts):
        del lines[:]
        before = len(fake.calls)
        rc = execute(list(link), dict(inc2), fake, out=lines.append, **opts)
        return rc, len(fake.calls) - before

    rc, calls = run_link_case()
    expect(rc == 0 and calls == 1 and os.path.exists("out/rig.exe.ccm"), "link mode builds and stores a manifest")
    first_call = fake.calls[-1]
    expect(first_call.index("/sourceDependencies") < first_call.index("/link"),
           "/sourceDependencies goes before /link, not to the linker")
    rc, calls = run_link_case()
    expect(calls == 0 and "reused" in lines[-1], "link mode hits when nothing changed")
    _write("rig/lib.lib", "lib v2!")
    rc, calls = run_link_case()
    expect(calls == 1 and any("lib.lib" in l for l in lines), "changed .lib input misses")
    rc, calls = run_link_case()
    expect(calls == 0, "settled after the .lib rebuild")
    _write("rig/x.def", "EXPORTS foo")
    rc, calls = run_link_case()
    expect(calls == 1 and any("x.def" in l for l in lines), "changed /DEF: file misses")
    run_link_case()
    _write("src/h.h", "#define H 9\n")
    rc, calls = run_link_case()
    expect(calls == 1 and any("h.h" in l for l in lines), "changed header misses")
    run_link_case()
    os.remove("out/rig.exe")
    rc, calls = run_link_case()
    expect(calls == 1 and any("missing" in l for l in lines), "deleted exe misses")
    before = os.stat("out/rig.exe")
    with open("out/rig.exe", "w", encoding="utf-8") as handle:
        handle.write("Y" * before.st_size)
    os.utime("out/rig.exe", ns=(before.st_mtime_ns + 5_000_000_000,) * 2)
    rc, calls = run_link_case()
    expect(calls == 1 and any("replaced" in l for l in lines), "replaced exe misses")
    os.remove("obj/a.obj")
    rc, calls = run_link_case()
    expect(calls == 0, "intermediate objects are not part of a link-mode hit")
    with forbid_writes():
        rc, calls = run_link_case(dry_run=True)
    expect(calls == 0 and any("hit" in l for l in lines), "link-mode dry-run is read-only")
    rc, calls = run_link_case(off=True)
    expect(calls == 1 and "/sourceDependencies" not in " ".join(fake.calls[-1]), "link-mode --off passes through")
    q = parse("cl.exe", ["/Fe" + "a.exe", "x.cpp", "/link", '/OUT:b.exe'])
    expect(q.uncacheable is None and link_outputs(q)[0] == "b.exe", "/OUT: after /link names the target")
    expect(parse("cl.exe", ["x.cpp", "/link", "/IMPLIB:x.lib"]).uncacheable, "/IMPLIB: is not modelled")
    fake.exit_code = 2
    _write("rig/lib.lib", "lib v3")
    rc, calls = run_link_case()
    expect(rc == 2 and not os.path.exists("out/rig.exe.ccm"), "failed link leaves no manifest")
    fake.exit_code = None
    rc, calls = run_link_case()
    expect(calls == 1 and rc == 0, "retry after failure rebuilds")


def _store_self_test_body():
    """pack_store / unpack_store / summarize_log: a cache carried to an empty build directory
    still hits, and anything that is not exactly what cl wrote does not come back."""
    os.makedirs("sys")
    os.makedirs("src")
    os.makedirs("build/obj")
    os.makedirs("build/out")
    os.makedirs("elsewhere")
    _write("fake_cl.exe", "x")
    _write("src/h.h", "#define H 1\n")
    _write("src/a.cpp", '#include "h.h"\nint a;\n')
    _write("src/b.cpp", '#include "h.h"\nint b;\n')
    environ = {"INCLUDE": os.path.abspath("sys"), "LIB": "L", "CL": "/MP"}
    fake = FakeCl()
    lines = []

    def expect(condition, message):
        if not condition:
            raise AssertionError("compile_cache store self-test: " + message + "\n" + "\n".join(lines))

    def compile_objects(fo=os.path.join("build", "obj") + os.sep):
        del lines[:]
        fake.compiled = []
        rc = execute(["fake_cl.exe", "/nologo", "/c", "/Fo" + fo, "src/a.cpp", "src/b.cpp"], dict(environ), fake,
                     out=lines.append)
        return rc, sorted(fake.compiled)

    def link_rig():
        del lines[:]
        before = len(fake.calls)
        rc = execute(["fake_cl.exe", "/nologo", "/Fe" + os.path.join("build", "out", "rig.exe"), "src/a.cpp", "/link", "kernel32.lib"],
                     dict(environ), fake, out=lines.append)
        return rc, len(fake.calls) - before

    def quiet(function, *args, **kwargs):
        del lines[:]
        return function(*args, out=lines.append, **kwargs)

    rc, compiled = compile_objects()
    expect(rc == 0 and compiled == ["a.cpp", "b.cpp"], "objects compile")
    rc, calls = link_rig()
    expect(rc == 0 and calls == 1, "the rig links")
    for name in ("build/obj/a.obj.ccm", "build/out/rig.exe.ccm"):
        with open(name, encoding="utf-8") as handle:
            recorded = json.load(handle)
        expect(set(recorded["hashes"]) == set(recorded["outputs"]) and all(len(h) == 64 for h in recorded["hashes"].values()),
               "%s records a sha256 of each output" % name)

    # An output outside the root and an entry written before hashes were recorded are not packed.
    rc, compiled = compile_objects("elsewhere" + os.sep)
    expect(rc == 0, "objects outside the root compile")
    legacy = "build/obj/b.obj.ccm"
    with open(legacy, encoding="utf-8") as handle:
        legacy_data = json.load(handle)
    del legacy_data["hashes"]
    with open(legacy, "w", encoding="utf-8") as handle:
        json.dump(legacy_data, handle)
    _write("build/gen/shader.h", "// key: 1\n")
    _write("build/times.json", "{}")
    with forbid_writes():
        result = quiet(pack_store, "build", "store", extras=("gen/shader.h",), dry_run=True)
    expect(result["packed"] == 2 and not os.path.exists("store"), "a dry run counts and writes nothing: %r" % result)
    result = quiet(pack_store, "build", "store", extras=("gen/shader.h", "gen/absent.h"))
    expect(result["packed"] == 2 and result["files"] == 2 and result["extra"] == 1,
           "pack takes the object and the exe, and one extra file: %r" % result)
    expect(list(result["skipped"]) == ["recorded before outputs were hashed"],
           "an entry with no hashes is skipped, by name: %r" % result["skipped"])
    expect(os.path.isfile("store/m/obj/a.obj.ccm") and os.path.isfile("store/f/obj/a.obj")
           and os.path.isfile("store/f/out/rig.exe") and os.path.isfile("store/x/gen/shader.h"),
           "the store has manifests, outputs and extras at their places")
    expect(not os.path.exists("store/m/obj/b.obj.ccm"), "the unhashed entry is not in the store")

    # A fresh machine: empty build directory, store whose files carry times a tar would round.
    shutil.rmtree("build")
    os.makedirs("build")
    for folder, _, names in os.walk("store/f"):
        for name in names:
            stamp = os.stat(os.path.join(folder, name)).st_mtime_ns + 123456789
            os.utime(os.path.join(folder, name), ns=(stamp, stamp))
    with forbid_writes():
        result = quiet(unpack_store, "build", "store", extras=("gen/shader.h",), dry_run=True)
    expect(result["restored"] == 2 and not os.path.exists("build/obj"), "a dry-run unpack writes nothing: %r" % result)
    result = quiet(unpack_store, "build", "store", extras=("gen/shader.h",))
    expect(result["restored"] == 2 and result["files"] == 2 and not result["rejected"] and result["extra"] == 1,
           "unpack restores both entries: %r" % result)
    expect(os.path.isfile("build/gen/shader.h") and not os.path.exists("store/f/obj/a.obj"),
           "extras come back, and the store's files were moved out of it")
    rc, compiled = compile_objects()
    expect(rc == 0 and compiled == ["b.cpp"], "restored object is a hit (its time put back), the unhashed one compiles: %s" % compiled)
    rc, calls = link_rig()
    expect(rc == 0 and calls == 0 and "reused" in lines[-1], "restored exe is a hit")

    # A restored entry for an input that has changed is a miss: the key is still compared.
    _write("src/h.h", "#define H 2\n")
    rc, compiled = compile_objects()
    expect(compiled == ["a.cpp", "b.cpp"], "a changed header misses restored objects: %s" % compiled)
    _write("src/h.h", "#define H 1\n")
    rc, compiled = compile_objects()
    rc, calls = link_rig()

    # Anything that is not what cl wrote stays out.
    quiet(pack_store, "build", "store")
    for variant in ("flipped", "short", "missing"):
        shutil.rmtree("store")
        quiet(pack_store, "build", "store")
        victim = "store/f/obj/a.obj"
        if variant == "flipped":
            with open(victim, "rb") as handle:
                body = handle.read()
            os.remove(victim)
            with open(victim, "wb") as handle:
                handle.write(bytes([body[0] ^ 1]) + body[1:])
        elif variant == "short":
            with open(victim, "rb") as handle:
                body = handle.read()
            os.remove(victim)
            with open(victim, "wb") as handle:
                handle.write(body[:-1])
        else:
            os.remove(victim)
        shutil.rmtree("build")
        os.makedirs("build")
        result = quiet(unpack_store, "build", "store")
        expect(result["restored"] == 2 and sum(result["rejected"].values()) == 1
               and not os.path.exists("build/obj/a.obj") and not os.path.exists("build/obj/a.obj.ccm"),
               "a %s output is rejected whole, with no manifest left behind: %r" % (variant, result))
        expect(os.path.isfile("build/out/rig.exe.ccm"), "the intact entry still comes back (%s)" % variant)
        rc, compiled = compile_objects()
        expect(compiled == ["a.cpp"], "only the rejected entry is recompiled (%s): %s" % (variant, compiled))
        rc, calls = link_rig()
        expect(calls == 0, "and the intact one is reused (%s)" % variant)

    # pack leaves out an output that changed after its manifest was written.
    with open("build/obj/a.obj", "a", encoding="utf-8") as handle:
        handle.write("tampered")
    result = quiet(pack_store, "build", "store")
    expect("an output no longer as recorded" in result["skipped"] and result["packed"] == 2,
           "a replaced output is not packed: %r" % result)

    # An existing, valid file is left in place; an extra never overwrites.
    _write("build/gen/shader.h", "// key: newer\n")
    quiet(unpack_store, "build", "store", extras=("gen/shader.h",))
    with open("build/gen/shader.h", encoding="utf-8") as handle:
        expect(handle.read() == "// key: newer\n", "an existing extra file is not overwritten")
    expect(quiet(unpack_store, "build", "nowhere")["restored"] == 0, "no store is no entries, not an error")

    # The log reader.
    log = "\n".join([
        "[edvr] compile_cache: d3d11: 141 compiled, 0 reused (123.6 s)",
        "[edvr] compile_cache:   miss src\\common\\log.cpp: no manifest",
        "[edvr] compile_cache:   miss src\\common\\config.cpp: header build\\gen\\edvr_version.h changed",
        "[edvr] compile_cache: a_test.exe: built (12.5 s)",
        "[edvr] compile_cache:   miss: input rig\\x.lib changed",
        "[edvr] compile_cache: b_test.exe: reused (0.4 s)",
        "[edvr] compile_cache: c: 3 compiled, 40 reused (2.0 s)",
        "unrelated line"])
    totals = summarize_log(log)
    expect(totals["sources_compiled"] == 144 and totals["sources_reused"] == 40 and totals["links_built"] == 1
           and totals["links_reused"] == 1 and abs(totals["group_seconds"] - 125.6) < 1e-9, "summary adds up: %r" % totals)
    expect(totals["reasons"] == {"no manifest": 1, "header <file> changed": 1, "input <file> changed": 1},
           "summary groups the miss reasons without file names: %r" % totals["reasons"])
    expect("144 compiled, 40 reused" in summary_text(totals) and "no compile_cache lines" in summary_text(summarize_log("x")),
           "summary text")
    _write("build.log", log)
    expect(store_main(["--summary", "build.log", "--markdown", "summary.md"]) == 0
           and "144 compiled" in open("summary.md", encoding="utf-8").read(), "--summary writes a job summary")


if __name__ == "__main__":
    sys.exit(main())
