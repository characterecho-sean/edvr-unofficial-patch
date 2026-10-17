#!/usr/bin/env python3
"""Post replies and a summary comment to a PR, but only after seeing everything
newer than the last thing you looked at.

Why this exists (issue #96): during PR #92 a whole review was missed, and once
the re-check ran in the same command as the post, found new activity, and
posted anyway. Hand-built `gh api ... --jq` calls also broke on PowerShell
quoting. This tool does the check and the post as one operation whose check
cannot be skipped.

Usage:
    python tools\\pr_post.py --repo OWNER/REPO --pr N --since 2026-10-16T00:00:00Z
        [--me LOGIN] [--reply COMMENT_ID=FILE ...] [--comment FILE]
        [--ack-new ID,ID,...] [--dry-run]

Flow:
    1. Fetch the PR's reviews, issue comments and inline review comments.
       List every item newer than --since whose author is not --me (default:
       the authenticated gh user). If there is any, print them and exit 1
       without posting. The only way past is --ack-new naming exactly those
       ids, so acknowledging requires writing down what you saw.
    2. Otherwise post each --reply as a reply to that inline comment, then
       post --comment as an issue comment. Bodies are read from files as UTF-8
       (a BOM is stripped; an empty body is refused) and sent as JSON on
       gh's stdin, never on a command line. Stop at the first failure and say
       what was and was not posted.
    3. Print the timestamp to use as the next --since.

--dry-run runs the guard (reads only) and prints the plan; it posts nothing.

Exit codes: 0 done (or dry-run plan printed); 1 guard tripped, nothing posted;
2 usage or gh read error, nothing posted; 3 a post failed, see the report.

    python tools\\pr_post.py --self-test
"""

import argparse
import datetime
import io
import json
import os
import subprocess
import sys
import tempfile


class ToolError(Exception):
    pass


# ---------------------------------------------------------------------------
# gh runner. A runner takes (args, stdin_text=None) and returns
# (returncode, stdout, stderr). The self-test substitutes a fake.
# ---------------------------------------------------------------------------

def make_gh_runner():
    env = dict(os.environ, GH_PROMPT_DISABLED="1", NO_COLOR="1")

    def gh(args, stdin_text=None):
        try:
            p = subprocess.run(
                ["gh"] + list(args), env=env, shell=False, timeout=180,
                input=None if stdin_text is None else stdin_text.encode("utf-8"),
                stdin=subprocess.DEVNULL if stdin_text is None else None,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
        except FileNotFoundError:
            raise ToolError("gh was not found on PATH")
        except subprocess.TimeoutExpired:
            raise ToolError("gh timed out (%s)" % " ".join(args[:3]))
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    return gh


def parse_json_stream(text, what):
    """gh --paginate prints one JSON document per page, back to back."""
    items = []
    dec = json.JSONDecoder()
    i, n = 0, len(text)
    while True:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        try:
            doc, i = dec.raw_decode(text, i)
        except ValueError as e:
            raise ToolError("could not parse gh output for %s: %s" % (what, e))
        if isinstance(doc, list):
            items.extend(doc)
        else:
            items.append(doc)
    return items


def gh_get(gh, path, what):
    rc, out, err = gh(["api", "--paginate", path])
    if rc != 0:
        raise ToolError("gh api %s failed (exit %d): %s" % (path, rc, err.strip()[:300]))
    return parse_json_stream(out, what)


def parse_ts(text):
    if not isinstance(text, str) or not text:
        raise ValueError("empty timestamp")
    t = text.strip()
    if t.endswith(("Z", "z")):
        t = t[:-1] + "+00:00"
    d = datetime.datetime.fromisoformat(t)
    if d.tzinfo is None:
        d = d.replace(tzinfo=datetime.timezone.utc)
    return d.astimezone(datetime.timezone.utc)


def fmt_ts(d):
    return d.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def login_of(item):
    u = item.get("user") or {}
    return u.get("login") or "(unknown)"


def snippet(body, limit=200):
    s = " ".join((body or "").split())
    return s if len(s) <= limit else s[:limit - 3] + "..."


def read_activity(gh, repo, pr):
    """Every review, issue comment and inline comment, normalised."""
    base = "repos/%s" % repo
    out = []
    for item in gh_get(gh, "%s/pulls/%d/reviews?per_page=100" % (base, pr), "reviews"):
        ts = item.get("submitted_at") or item.get("created_at")
        if not ts:
            continue  # a PENDING review has no submit time and is not visible
        out.append(dict(kind="review", id=int(item["id"]), author=login_of(item), when=parse_ts(ts),
                        where="", body=item.get("body") or "", url=item.get("html_url", ""),
                        state=item.get("state", "")))
    for item in gh_get(gh, "%s/issues/%d/comments?per_page=100" % (base, pr), "issue comments"):
        out.append(dict(kind="comment", id=int(item["id"]), author=login_of(item),
                        when=parse_ts(item["created_at"]), where="", body=item.get("body") or "",
                        url=item.get("html_url", ""), state=""))
    for item in gh_get(gh, "%s/pulls/%d/comments?per_page=100" % (base, pr), "review comments"):
        line = item.get("line") or item.get("original_line")
        where = "%s:%s" % (item.get("path", "?"), line if line is not None else "?")
        out.append(dict(kind="inline", id=int(item["id"]), author=login_of(item),
                        when=parse_ts(item["created_at"]), where=where, body=item.get("body") or "",
                        url=item.get("html_url", ""), state=""))
    return out


def find_unseen(items, since, me):
    me_l = me.lower()
    new = [i for i in items if i["when"] > since and i["author"].lower() != me_l]
    new.sort(key=lambda i: (i["when"], i["id"]))
    return new


def describe(item):
    head = "  #%d  %s  %s  %s" % (item["id"], fmt_ts(item["when"]), item["author"], item["kind"])
    if item["state"]:
        head += " (%s)" % item["state"]
    if item["where"]:
        head += "  " + item["where"]
    return head + "\n      " + snippet(item["body"])


def read_body(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise ToolError("cannot read body file %s: %s" % (path, e))
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        raise ToolError("body file %s is not valid UTF-8: %s" % (path, e))
    if not text.strip():
        raise ToolError("body file %s is empty" % path)
    return text


def parse_replies(specs):
    out = []
    for spec in specs or []:
        cid, sep, path = spec.partition("=")
        if not sep or not cid.strip().isdigit() or not path:
            raise ToolError("--reply wants COMMENT_ID=FILE, got %r" % spec)
        out.append((int(cid), path))
    return out


def parse_ack(text):
    if text is None:
        return set()
    ids = set()
    for part in text.split(","):
        part = part.strip().lstrip("#")
        if not part:
            continue
        if not part.isdigit():
            raise ToolError("--ack-new wants comma-separated numeric ids, got %r" % part)
        ids.add(int(part))
    return ids


def post_json(gh, path, body):
    """POST {"body": body} via stdin. ASCII-only JSON, so no encoding hazard."""
    payload = json.dumps({"body": body})
    rc, out, err = gh(["api", "-X", "POST", path, "--input", "-"], payload)
    if rc != 0:
        raise ToolError("POST %s failed (exit %d): %s" % (path, rc, (err or out).strip()[:300]))
    try:
        return json.loads(out).get("html_url", "(no url returned)")
    except ValueError:
        return "(unparseable response)"


def run(args, gh, out, err):
    start = datetime.datetime.now(datetime.timezone.utc)
    try:
        since = parse_ts(args.since)
    except ValueError:
        print("pr_post: --since must be an ISO-8601 timestamp, got %r" % args.since, file=err)
        return 2
    try:
        repo = args.repo
        replies = parse_replies(args.reply)
        ack = parse_ack(args.ack_new)
        # Validate every body before touching the network or posting anything.
        reply_bodies = [(cid, path, read_body(path)) for cid, path in replies]
        comment_body = read_body(args.comment) if args.comment else None
        if not reply_bodies and comment_body is None:
            raise ToolError("nothing to post: give --reply and/or --comment")

        me = args.me
        if not me:
            rc, o, e = gh(["api", "user"])
            if rc != 0:
                raise ToolError("could not determine the authenticated user (exit %d): %s; pass --me"
                                % (rc, e.strip()[:200]))
            me = json.loads(o).get("login")
            if not me:
                raise ToolError("gh api user returned no login; pass --me")

        items = read_activity(gh, repo, args.pr)
    except ToolError as e:
        print("pr_post: %s" % e, file=err)
        return 2

    newest = max([i["when"] for i in items], default=since)
    new = find_unseen(items, since, me)
    print("PR %s#%d: %d item(s) read, %d newer than %s by someone other than %s"
          % (repo, args.pr, len(items), len(new), fmt_ts(since), me), file=out)

    if new:
        for i in new:
            print(describe(i), file=out)
        new_ids = {i["id"] for i in new}
        if ack != new_ids:
            print("\nGUARD: unseen activity. Nothing was posted.", file=out)
            print("Read the items above (full text: `gh pr view %d --repo %s --comments`), then rerun"
                  % (args.pr, repo), file=out)
            print("with --ack-new %s" % ",".join(str(i) for i in sorted(new_ids)), file=out)
            if ack:
                print("(the --ack-new you gave, %s, does not match exactly)"
                      % ",".join(str(i) for i in sorted(ack)), file=out)
            print("Next --since once you have read them: %s" % fmt_ts(max(i["when"] for i in new)), file=out)
            return 1
        print("--ack-new matches exactly; proceeding.", file=out)
    elif ack:
        print("note: --ack-new given but nothing new was found; ignoring it.", file=out)

    plan = [("reply to inline comment %d" % cid, "repos/%s/pulls/%d/comments/%d/replies" % (repo, args.pr, cid),
             path, body) for cid, path, body in reply_bodies]
    if comment_body is not None:
        plan.append(("issue comment", "repos/%s/issues/%d/comments" % (repo, args.pr), args.comment, comment_body))

    nxt = fmt_ts(max(newest, start))
    if args.dry_run:
        print("\nDRY RUN: would post %d item(s), posting nothing:" % len(plan), file=out)
        for what, path, src, body in plan:
            print("  %s  <- %s (%d chars)  POST %s" % (what, src, len(body), path), file=out)
            print("      " + snippet(body), file=out)
        print("Next --since: %s" % nxt, file=out)
        return 0

    done = []
    for n, (what, path, src, body) in enumerate(plan):
        try:
            url = post_json(gh, path, body)
        except ToolError as e:
            print("pr_post: %s" % e, file=err)
            print("Posted before the failure: %s" % (", ".join(done) or "nothing"), file=out)
            print("NOT posted: %s" % ", ".join(w for w, _, _, _ in plan[n:]), file=out)
            return 3
        done.append(what)
        print("posted %s: %s" % (what, url), file=out)
    print("Next --since: %s" % nxt, file=out)
    return 0


def build_parser():
    p = argparse.ArgumentParser(description="Post PR replies/comment only after the unseen-activity guard passes.")
    p.add_argument("--repo", help="OWNER/REPO")
    p.add_argument("--pr", type=int, help="pull request number")
    p.add_argument("--since", help="ISO-8601 UTC time of the last activity you read")
    p.add_argument("--me", help="your login (default: the authenticated gh user)")
    p.add_argument("--reply", action="append", metavar="COMMENT_ID=FILE",
                   help="reply to an inline review comment with the contents of FILE (repeatable)")
    p.add_argument("--comment", metavar="FILE", help="post FILE as an issue comment, after the replies")
    p.add_argument("--ack-new", metavar="ID,ID,...",
                   help="proceed past the guard; must list exactly the ids the guard printed")
    p.add_argument("--dry-run", action="store_true", help="run the guard and print the plan; post nothing")
    p.add_argument("--self-test", action="store_true", help="run the built-in tests and exit")
    return p


def main(argv=None, gh=None, stdout=None, stderr=None):
    out = stdout or sys.stdout
    err = stderr or sys.stderr
    args = build_parser().parse_args(argv)
    if args.self_test:
        return self_test()
    if not (args.repo and args.pr and args.since):
        print("pr_post: --repo, --pr and --since are required", file=err)
        return 2
    return run(args, gh or make_gh_runner(), out, err)


# ---------------------------------------------------------------------------
# Self-test: a fake gh, no network.
# ---------------------------------------------------------------------------

REPO = "o/r"
EM = "—"


class FakeGh:
    def __init__(self, reviews=(), comments=(), inline=(), me="sean", fail_post_at=None, pages=1):
        self.reviews, self.comments, self.inline = list(reviews), list(comments), list(inline)
        self.me, self.fail_post_at, self.pages = me, fail_post_at, pages
        self.calls = []      # (args, stdin_text)
        self.posts = []      # (path, decoded body)

    def _paged(self, items):
        n = max(1, self.pages)
        chunks = [items[i::n] for i in range(n)]
        return "\n".join(json.dumps(c) for c in chunks)

    def __call__(self, args, stdin_text=None):
        args = list(args)
        self.calls.append((args, stdin_text))
        if args[:2] == ["api", "user"]:
            return 0, json.dumps({"login": self.me}), ""
        if "-X" in args and args[args.index("-X") + 1] == "POST":
            path = args[args.index("-X") + 2]
            self.posts.append((path, json.loads(stdin_text)["body"]))
            if self.fail_post_at is not None and len(self.posts) - 1 == self.fail_post_at:
                return 1, "", "HTTP 422"
            return 0, json.dumps({"html_url": "https://example/%d" % len(self.posts)}), ""
        path = args[-1]
        if "/pulls/7/reviews" in path:
            return 0, self._paged(self.reviews), ""
        if "/issues/7/comments" in path:
            return 0, self._paged(self.comments), ""
        if "/pulls/7/comments" in path:
            return 0, self._paged(self.inline), ""
        return 1, "", "unexpected " + path


def _rv(id_, who, when, body="r"):
    return {"id": id_, "user": {"login": who}, "submitted_at": when, "body": body, "state": "COMMENTED"}


def _cm(id_, who, when, body="c"):
    return {"id": id_, "user": {"login": who}, "created_at": when, "body": body}


def _ic(id_, who, when, body="i", path="a.py", line=3):
    return {"id": id_, "user": {"login": who}, "created_at": when, "body": body, "path": path, "line": line}


def self_test():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    def go(fake, argv):
        o, e = io.StringIO(), io.StringIO()
        rc = main(argv, gh=fake, stdout=o, stderr=e)
        return rc, o.getvalue(), e.getvalue()

    with tempfile.TemporaryDirectory() as tmp:
        def wr(name, text, bom=False):
            p = os.path.join(tmp, name)
            with open(p, "wb") as f:
                f.write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))
            return p

        r1 = wr("r1.txt", "first %s reply é\n" % EM)
        r2 = wr("r2.txt", "second reply", bom=True)
        summ = wr("sum.txt", "summary %s done ✓" % EM)
        empty = wr("empty.txt", "  \n")
        base = ["--repo", REPO, "--pr", "7", "--since", "2026-10-16T00:00:00Z", "--me", "sean"]
        old, new = "2026-10-15T12:00:00Z", "2026-10-16T09:00:00Z"

        # Guard trips on a newer review by someone else; nothing posted.
        f = FakeGh(reviews=[_rv(11, "bob", new, "needs work")])
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 1, "guard: exit 1 on unseen review, got %r" % rc)
        check(not f.posts, "guard: nothing posted")
        check("bob" in o and "needs work" in o and "11" in o, "guard: item listed with author, body, id")
        check("--ack-new 11" in o, "guard: tells how to acknowledge")

        # Guard ignores --me and old items; passes and posts.
        f = FakeGh(reviews=[_rv(1, "bob", old), _rv(2, "sean", new)],
                   comments=[_cm(3, "SEAN", new)], inline=[_ic(4, "bob", old)])
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 0, "pass: exit 0, got %r (%s)" % (rc, e))
        check(len(f.posts) == 1, "pass: one post")

        # All three kinds are checked; inline shows path:line.
        f = FakeGh(comments=[_cm(21, "bob", new)], inline=[_ic(22, "bob", new, path="x/y.cpp", line=9)])
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 1 and "x/y.cpp:9" in o and "#21" in o and "#22" in o, "guard: issue + inline comments listed")

        # --ack-new: exact ids proceed; wrong, partial or extra still refuse.
        items = dict(reviews=[_rv(11, "bob", new)], inline=[_ic(12, "bob", new)])
        for ack, want in (("11,12", 0), ("12,11", 0), ("11", 1), ("11,99", 1), ("99", 1), ("11,12,99", 1)):
            f = FakeGh(**items)
            rc, o, e = go(f, base + ["--comment", summ, "--ack-new", ack])
            check(rc == want, "ack %s: want exit %d got %r" % (ack, want, rc))
            check(bool(f.posts) == (want == 0), "ack %s: posts iff accepted" % ack)

        # --dry-run: zero POSTs, still guards, prints the plan.
        f = FakeGh()
        before = sorted(os.listdir(tmp))
        rc, o, e = go(f, base + ["--reply", "5=" + r1, "--comment", summ, "--dry-run"])
        check(rc == 0 and not f.posts, "dry-run: exit 0, no POST")
        check(not any("POST" in c[0] for c in f.calls if "-X" in c[0]), "dry-run: no POST call at all")
        check("DRY RUN" in o and "comments/5/replies" in o and "issues/7/comments" in o, "dry-run: plan printed")
        check(sorted(os.listdir(tmp)) == before, "dry-run: wrote no files")
        f = FakeGh(reviews=[_rv(11, "bob", new)])
        rc, o, e = go(f, base + ["--comment", summ, "--dry-run"])
        check(rc == 1 and not f.posts, "dry-run: guard still trips")

        # Replies then comment, in order; bodies byte-exact from files; BOM stripped.
        f = FakeGh()
        rc, o, e = go(f, base + ["--comment", summ, "--reply", "5=" + r1, "--reply", "6=" + r2])
        check(rc == 0, "post: exit 0, got %r (%s)" % (rc, e))
        check([p for p, _ in f.posts] == ["repos/o/r/pulls/7/comments/5/replies",
                                          "repos/o/r/pulls/7/comments/6/replies",
                                          "repos/o/r/issues/7/comments"], "post: order replies then comment")
        check(f.posts and f.posts[0][1] == "first %s reply é\n" % EM, "post: em-dash round-trips")
        check(len(f.posts) > 1 and f.posts[1][1] == "second reply", "post: BOM stripped")
        check(len(f.posts) > 2 and f.posts[2][1] == "summary %s done ✓" % EM, "post: comment body exact")
        check(all(s is not None and s.isascii() for (a, s) in f.calls if "-X" in a), "post: stdin JSON is ASCII-safe")
        check(all(len(a) < 8 and not any("done" in x for x in a) for (a, s) in f.calls),
              "post: no body text on any command line")
        check("posted issue comment: https://example/3" in o and "Next --since:" in o, "post: URLs and next since printed")

        # Pagination followed.
        many = [_rv(100 + i, "bob", old) for i in range(5)] + [_rv(200, "bob", new)]
        f = FakeGh(reviews=many, pages=3)
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 1 and "#200" in o and "6 item(s) read" in o, "pagination: every page parsed")
        check(all("--paginate" in a for a, s in f.calls if a[:2] == ["api", "--paginate"]), "pagination: flag used")

        # POST failure stops further posts and reports.
        f = FakeGh(fail_post_at=1)
        rc, o, e = go(f, base + ["--reply", "5=" + r1, "--reply", "6=" + r2, "--comment", summ])
        check(rc == 3, "failure: exit 3, got %r" % rc)
        check(len(f.posts) == 2, "failure: stopped after the failing post, got %d" % len(f.posts))
        check("Posted before the failure: reply to inline comment 5" in o, "failure: reports what was posted")
        check("NOT posted: reply to inline comment 6, issue comment" in o, "failure: reports what was not")

        # Bad input is refused before any network or post.
        for label, extra in (("empty body", ["--comment", empty]),
                             ("missing file", ["--comment", os.path.join(tmp, "nope")]),
                             ("bad reply spec", ["--reply", "x", "--comment", summ]),
                             ("nothing to post", [])):
            f = FakeGh()
            rc, o, e = go(f, base + extra)
            check(rc == 2 and not f.posts and not f.calls, "%s: exit 2, no calls (rc=%r)" % (label, rc))
        f = FakeGh()
        rc, o, e = go(f, ["--repo", REPO, "--pr", "7", "--since", "yesterday", "--comment", summ])
        check(rc == 2 and not f.calls, "bad --since refused")

        # Default --me comes from `gh api user`.
        f = FakeGh(comments=[_cm(3, "sean", new)], me="sean")
        rc, o, e = go(f, ["--repo", REPO, "--pr", "7", "--since", "2026-10-16T00:00:00Z", "--comment", summ])
        check(rc == 0 and f.calls[0][0][:2] == ["api", "user"], "default --me from gh api user")

        # Next --since: newest item when the guard trips; never earlier than --since.
        f = FakeGh(reviews=[_rv(11, "bob", new)])
        rc, o, e = go(f, base + ["--comment", summ])
        check("Next --since once you have read them: 2026-10-16T09:00:00Z" in o, "next since on trip")

    if fails:
        for m in fails:
            print("FAIL: " + m)
        print("pr_post self-test: %d failure(s)" % len(fails))
        return 1
    print("pr_post self-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
