#!/usr/bin/env python3
"""Make git worktrees that can build, and remove them without harming the main checkout.

    python tools\\worktree.py add <path> [-b <branch>] [<commit-ish>] [--dry-run]
    python tools\\worktree.py remove <path> [--dry-run]
    python tools\\worktree.py --self-test

`git worktree add` alone gives a tree with no third_party\\ngx, ffx-dx11, xess
or openxr\\loader: they are git-ignored, so build.bat cannot run there.  `add`
runs `git worktree add` and then links each of those folders to the main
checkout's copy with a directory junction (mklink /J).  A dependency the main
checkout does not have is skipped with a warning.  The target must not exist.

`remove` refuses unregistered, locked or dirty worktrees before touching links.
It restores detached junctions if removal fails. It deletes the junctions FIRST,
with rmdir, which removes the link and never the target's contents, and only
then runs `git worktree remove`. Order
matters: git deletes ignored files in a worktree, and doing that through a
junction would delete the main checkout's dependencies.  A dependency path in
the worktree that is a real directory, not a junction, is left for git.

--dry-run prints the plan and writes nothing: no worktree, no junction, no
directory.

Worktree builds, read from tools\\build_lock.py and tools\\compile_cache.py:
  * The build lock is machine-global (%TEMP%\\edvr_build.lock), so a worktree
    build and a main-checkout build wait for each other: use
    `build.bat --wait-lock`.  That is intended; they share the machine.
  * The compile cache is per output, as .ccm manifests beside each object under
    the worktree's own build\\ directory, so nothing is shared with the main
    checkout and nothing can be corrupted across trees.  The cost is that a
    worktree's first build is a full compile.
  * Keep the path SHORT (C:\\wt99, not a deep scratch folder).  The installer
    test rig builds fixtures under the tree and fails on MAX_PATH when the
    worktree path is ~190 characters; at C:\\wt99 the same build passes.
  * The dependency folders are shared through the junctions, read only by the
    build.  Do not run a fetch_*.py tool with a worktree's third_party as the
    destination: it would modify the main checkout's copy.
"""
import argparse
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
# A CLI dry run must not create an import cache for the local helper.
sys.dont_write_bytecode = True
sys.path.insert(0, str(SCRIPT_DIR))
from build_receipt import DEPENDENCY_ROOTS  # noqa: E402  (the one list of ignored dependency folders)


MAX_BUILD_PATH = 100  # 190 failed the installer test rig, 10 passed; warn well short of the limit


class WorktreeError(Exception):
    pass


def _git(repo, *args):
    r = subprocess.run(["git", "--no-optional-locks", "-C", str(repo), *args], capture_output=True, text=True)
    if r.returncode != 0:
        raise WorktreeError("git %s failed: %s" % (" ".join(args), (r.stderr or r.stdout).strip()))
    return r.stdout.strip()


def main_checkout(repo):
    """The main working tree, even when `repo` is itself a linked worktree."""
    common = _git(repo, "rev-parse", "--git-common-dir")
    common = Path(os.path.abspath(os.path.join(str(repo), common)))
    return common.parent


def is_junction(path):
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return bool(getattr(st, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def junction_target(path):
    return Path(os.path.realpath(path))


def make_junction(link, target):
    import _winapi
    _winapi.CreateJunction(str(target), str(link))


def plan_links(main, worktree, roots):
    """[(link, target)] for roots the main checkout has; [warning] for those it lacks."""
    links, warnings = [], []
    for rel in roots:
        src = Path(main) / rel
        if src.is_dir():
            links.append((Path(worktree) / rel, src))
        else:
            warnings.append("skipping %s: the main checkout has no %s" % (rel, src))
    return links, warnings


def add(repo, path, branch=None, commitish=None, dry_run=False, roots=DEPENDENCY_ROOTS, out=print):
    path = Path(os.path.abspath(path))
    if path.exists():
        raise WorktreeError("refusing: %s already exists" % path)
    main = main_checkout(repo)
    cmd = ["worktree", "add"]
    if branch:
        cmd += ["-b", branch]
    cmd.append(str(path))
    if commitish:
        cmd.append(commitish)
    links, warnings = plan_links(main, path, roots)
    out("git -C %s %s" % (main, " ".join(cmd)))
    for link, target in links:
        out("junction %s -> %s" % (link, target))
    for w in warnings:
        out("warning: " + w)
    if len(str(path)) > MAX_BUILD_PATH:
        out("warning: %d-character path; the installer test rig fails on MAX_PATH in a deep worktree, use a short one"
            % len(str(path)))
    if dry_run:
        out("dry run: nothing written")
        return
    _git(main, *cmd)
    for link, target in links:
        link.parent.mkdir(parents=True, exist_ok=True)
        make_junction(link, target)


def remove(repo, path, dry_run=False, roots=DEPENDENCY_ROOTS, out=print):
    path = Path(os.path.abspath(path))
    if not path.is_dir():
        raise WorktreeError("no such worktree: %s" % path)
    main = main_checkout(repo)
    if os.path.normcase(str(path.resolve())) == os.path.normcase(str(main.resolve())):
        raise WorktreeError("refusing: %s is the main checkout" % path)
    # Porcelain -z preserves spaces, newlines and quoting in registered paths.
    registered = None
    for record in _git(main, "worktree", "list", "--porcelain", "-z").split("\0\0"):
        fields = record.split("\0")
        if fields[0].startswith("worktree "):
            candidate = Path(fields[0][len("worktree "):])
            if os.path.normcase(str(candidate.resolve())) == os.path.normcase(str(path.resolve())):
                registered = fields
                break
    if registered is None:
        raise WorktreeError("refusing: %s is not a registered worktree" % path)
    if any(f == "locked" or f.startswith("locked ") for f in registered):
        raise WorktreeError("refusing: %s is a locked worktree" % path)
    # --no-optional-locks in _git prevents status refreshing the index, including
    # on a refused dry run. Ignored dependency links are not user modifications.
    if _git(path, "status", "--porcelain", "-z", "--untracked-files=all"):
        raise WorktreeError("refusing: %s contains modified or untracked files" % path)
    junctions = [(path / rel, junction_target(path / rel))
                 for rel in roots if is_junction(path / rel)]
    for link, target in junctions:
        out("rmdir junction %s (target left alone)" % link)
    out("git -C %s worktree remove %s" % (main, path))
    if dry_run:
        out("dry run: nothing written")
        return
    detached = []
    try:
        for link, target in junctions:
            os.rmdir(link)  # removes the link only, never the target's contents
            detached.append((link, target))
        _git(main, "worktree", "remove", str(path))
    except (OSError, WorktreeError) as error:
        # Git may refuse after preflight (a new edit/lock), or fail while deleting.
        # Never delete a competing path or walk a dependency target to roll back.
        failures = []
        for link, target in detached:
            try:
                if os.path.lexists(link):
                    if not is_junction(link) or junction_target(link) != target:
                        raise OSError("path now exists; refusing to overwrite it")
                elif not path.is_dir():
                    raise OSError("worktree directory disappeared; cannot restore link")
                else:
                    link.parent.mkdir(parents=True, exist_ok=True)
                    make_junction(link, target)
            except OSError as restore_error:
                failures.append("%s -> %s: %s" % (link, target, restore_error))
        detail = "; junction restoration failed: " + "; ".join(failures) if failures else ""
        raise WorktreeError("%s%s" % (error, detail)) from error


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

def _listing(root):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        for n in dirnames + filenames:
            out.append(os.path.relpath(os.path.join(dirpath, n), root))
    return sorted(out)


def _snapshot(root):
    """File bytes/mtimes and links, without descending into junction targets."""
    result = [(".", "directory", Path(root).stat().st_mtime_ns)]
    for base, dirs, files in os.walk(root):
        for name in dirs[:]:
            entry = Path(base) / name
            if is_junction(entry):
                result.append((str(entry.relative_to(root)), "junction", str(junction_target(entry)), entry.lstat().st_mtime_ns))
                dirs.remove(name)
            else:
                result.append((str(entry.relative_to(root)), "directory", entry.stat().st_mtime_ns))
        for name in files:
            entry = Path(base) / name
            result.append((str(entry.relative_to(root)), entry.read_bytes(), entry.stat().st_mtime_ns))
    return sorted(result)


def _self_test():
    failures = []

    def expect(cond, msg):
        if not cond:
            failures.append(msg)

    def raises(fn):
        try:
            fn()
        except WorktreeError:
            return True
        return False

    quiet = lambda *_: None
    fake_roots = ("third_party/ngx", "third_party/openxr/loader", "third_party/absent")
    with tempfile.TemporaryDirectory(prefix="edvr-worktree-test-") as scratch:
        scratch = Path(scratch)
        # TemporaryDirectory recursively cleans this fixture on exit. Validate its
        # absolute resolved boundary before creating anything that it will remove.
        if (not scratch.is_absolute() or scratch.resolve().parent != Path(tempfile.gettempdir()).resolve()
                or not scratch.name.startswith("edvr-worktree-test-")):
            raise RuntimeError("unsafe self-test cleanup boundary: %s" % scratch)
        repo = scratch / "main"
        repo.mkdir()
        g = lambda *a: subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True)
        g("init", "-q", "-b", "main")
        g("config", "user.email", "t@example.invalid")
        g("config", "user.name", "t")
        g("config", "commit.gpgsign", "false")
        (repo / ".gitignore").write_text("third_party/ngx/\nthird_party/openxr/loader/\nthird_party/absent/\n")
        (repo / "a.txt").write_text("a")
        g("add", ".")
        g("commit", "-q", "-m", "init")
        (repo / "third_party" / "ngx" / "lib").mkdir(parents=True)
        (repo / "third_party" / "ngx" / "lib" / "sdk.lib").write_text("sdk")
        (repo / "third_party" / "openxr" / "loader").mkdir(parents=True)
        (repo / "third_party" / "openxr" / "loader" / "l.dll").write_text("loader")
        deps_before = _listing(repo / "third_party")

        wt = scratch / "wt"

        # --dry-run writes nothing at all.
        before = _snapshot(scratch)
        add(repo, wt, branch="b1", dry_run=True, roots=fake_roots, out=quiet)
        expect(_snapshot(scratch) == before, "add --dry-run changed the filesystem")
        expect(not wt.exists(), "add --dry-run created the worktree")
        expect("b1" not in g("branch", "--list", "b1").stdout, "add --dry-run created the branch")

        # add makes the worktree and junctions that point at the source.
        add(repo, wt, branch="b1", roots=fake_roots, out=quiet)
        expect((wt / "a.txt").is_file(), "add did not create the worktree checkout")
        for rel in ("third_party/ngx", "third_party/openxr/loader"):
            expect(is_junction(wt / rel), "%s is not a junction" % rel)
            expect(junction_target(wt / rel) == Path(os.path.realpath(repo / rel)),
                   "%s does not point at the main checkout's folder" % rel)
        expect((wt / "third_party/ngx/lib/sdk.lib").read_text() == "sdk", "junction contents not visible (ngx)")
        expect((wt / "third_party/openxr/loader/l.dll").read_text() == "loader", "junction contents not visible (loader)")
        expect(not (wt / "third_party" / "absent").exists(), "a dependency the main checkout lacks was linked")

        # add refuses an existing target.
        expect(raises(lambda: add(repo, wt, roots=fake_roots, out=quiet)), "add did not refuse an existing target")
        expect(raises(lambda: add(repo, scratch, roots=fake_roots, out=quiet)), "add did not refuse an existing directory")

        # run from inside the worktree, the main checkout is still found.
        expect(main_checkout(wt) == Path(os.path.realpath(repo)) or main_checkout(wt) == repo,
               "main_checkout from a worktree is not the main checkout")

        # remove --dry-run writes nothing.
        before = _snapshot(scratch)
        remove(repo, wt, dry_run=True, roots=fake_roots, out=quiet)
        expect(_snapshot(scratch) == before, "remove --dry-run changed the filesystem")

        # Dirty/locked/unregistered refusals must preserve every link and byte,
        # including index contents/mtime in failed dry runs.
        def refused_unchanged(label, target=wt):
            for dry_run in (True, False):
                before = _snapshot(scratch)
                expect(raises(lambda: remove(repo, target, dry_run=dry_run,
                                            roots=fake_roots, out=quiet)),
                       label + " was not refused")
                expect(_snapshot(scratch) == before, label + " changed the filesystem")

        (wt / "a.txt").write_text("dirty user edit")
        refused_unchanged("dirty tracked worktree")
        expect((wt / "a.txt").read_text() == "dirty user edit", "dirty user edit was lost")
        (wt / "a.txt").write_text("a")
        (wt / "untracked.txt").write_text("user file")
        refused_unchanged("untracked worktree")
        (wt / "untracked.txt").unlink()
        g("worktree", "lock", str(wt), "--reason", "self-test lock")
        refused_unchanged("locked worktree")
        g("worktree", "unlock", str(wt))
        unrelated = scratch / "not-registered"
        unrelated.mkdir()
        make_junction(unrelated / "dependency", repo / "third_party/ngx")
        refused_unchanged("unregistered directory", unrelated)
        os.rmdir(unrelated / "dependency")
        unrelated.rmdir()

        # Exercise production rollback after preflight with actual Git refusing
        # a new user edit, a late lock, and an injected command failure.
        global _git
        real_git = _git
        for scenario in ("edit race", "lock race", "injected failure", "replacement race"):
            intercepted = []
            def failing_git(where, *args):
                if args[:2] == ("worktree", "remove"):
                    intercepted.append(True)
                    expect(all(not os.path.lexists(wt / rel) for rel in fake_roots),
                           "Git remove ran before links were detached")
                    if scenario == "edit race":
                        (wt / "a.txt").write_text("late user edit")
                    elif scenario == "lock race":
                        real_git(repo, "worktree", "lock", str(wt))
                    else:
                        if scenario == "replacement race":
                            (wt / fake_roots[0]).mkdir()
                            (wt / fake_roots[0] / "user.txt").write_text("new user file")
                        raise WorktreeError("injected Git removal failure")
                return real_git(where, *args)
            _git = failing_git
            try:
                try:
                    remove(repo, wt, roots=fake_roots, out=quiet)
                    expect(False, scenario + " did not fail")
                except WorktreeError as error:
                    if scenario == "replacement race":
                        expect("refusing to overwrite" in str(error),
                               "replacement conflict was not reported")
            finally:
                _git = real_git
            expect(bool(intercepted), scenario + " never reached Git remove")
            if scenario == "replacement race":
                replacement = wt / fake_roots[0]
                expect((replacement / "user.txt").read_text() == "new user file",
                       "rollback overwrote a replacement user file")
                expect(not is_junction(replacement), "rollback replaced a user directory")
                (replacement / "user.txt").unlink()
                replacement.rmdir()
                make_junction(replacement, repo / fake_roots[0])
            for rel in fake_roots[:2]:
                expect(is_junction(wt / rel), scenario + " did not restore " + rel)
                expect(junction_target(wt / rel) == (repo / rel).resolve(),
                       scenario + " restored wrong target")
            expect((wt / "third_party/ngx/lib/sdk.lib").read_text() == "sdk",
                   scenario + " damaged dependency target")
            if scenario == "edit race":
                expect((wt / "a.txt").read_text() == "late user edit", "rollback lost late edit")
                (wt / "a.txt").write_text("a")
            elif scenario == "lock race":
                g("worktree", "unlock", str(wt))

        # An unlink failure after the first detach also restores earlier links.
        real_rmdir = os.rmdir
        unlinks = []
        def failing_rmdir(link, *args, **kwargs):
            unlinks.append(link)
            if Path(link) == wt / fake_roots[1]:
                raise OSError("injected second junction unlink failure")
            return real_rmdir(link, *args, **kwargs)
        os.rmdir = failing_rmdir
        try:
            expect(raises(lambda: remove(repo, wt, roots=fake_roots, out=quiet)),
                   "unlink failure was not reported")
        finally:
            os.rmdir = real_rmdir
        expect(len(unlinks) == 2, "unlink failure did not exercise partial detach")
        expect(all(is_junction(wt / rel) for rel in fake_roots[:2]),
               "partial unlink failure lost dependency links")

        # remove deletes junctions and worktree, never the source.
        remove(repo, wt, roots=fake_roots, out=quiet)
        expect(not wt.exists(), "remove left the worktree directory")
        expect(_listing(repo / "third_party") == deps_before, "remove changed the source dependency folders")
        expect((repo / "third_party/ngx/lib/sdk.lib").read_text() == "sdk", "remove damaged a source dependency file")
        expect((repo / "third_party/openxr/loader/l.dll").read_text() == "loader", "remove damaged the loader file")
        expect(str(wt).lower() not in g("worktree", "list").stdout.lower().replace("/", "\\"),
               "git still lists the removed worktree")

        # remove refuses the main checkout and a missing path.
        expect(raises(lambda: remove(repo, repo, roots=fake_roots, out=quiet)), "remove did not refuse the main checkout")
        expect(raises(lambda: remove(repo, scratch / "nope", roots=fake_roots, out=quiet)), "remove did not refuse a missing path")
        expect((repo / "third_party/ngx/lib/sdk.lib").is_file(), "refused remove still damaged the source")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("worktree.py self-test ok")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    a = sub.add_parser("add", help="git worktree add, then junction the dependency folders")
    a.add_argument("path")
    a.add_argument("-b", dest="branch", help="create this new branch")
    a.add_argument("commitish", nargs="?")
    a.add_argument("--dry-run", action="store_true")
    r = sub.add_parser("remove", help="remove the junctions, then git worktree remove")
    r.add_argument("path")
    r.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()
    if not args.cmd:
        ap.error("a command (add, remove) or --self-test is required")
    try:
        if args.cmd == "add":
            add(SCRIPT_DIR.parent, args.path, args.branch, args.commitish, args.dry_run)
        else:
            remove(SCRIPT_DIR.parent, args.path, args.dry_run)
    except WorktreeError as e:
        print("worktree.py: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
