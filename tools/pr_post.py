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
        [--ack-new ID@REV,ID@REV,...] [--dry-run]

Flow:
    1. Fetch the PR's reviews, issue comments and inline review comments.
       List every item newer than --since whose author is not --me (default:
       the authenticated gh user). If there is any, print them and exit 1
       without posting. The only way past is --ack-new naming exactly those
       items as ID@REV, where REV is the revision printed beside each item
       (a digest of its text and edit time). An edited comment keeps its id
       but gets a new REV, so an edit made after you read it trips the guard
       again. A bare id is refused: it cannot say which text you read.
    2. Otherwise post each --reply as a reply to that inline comment, then
       post --comment as an issue comment. Bodies are read from files as UTF-8
       (a BOM is stripped; an empty body is refused) and sent as JSON on
       gh's stdin, never on a command line. Stop at the first failure and say
       what was and was not posted.
    3. Print the timestamp to use as the next --since.

--dry-run runs the guard (reads only) and prints the plan; it posts nothing.

"Newer" means created or edited after --since (an edited comment counts again).
The printed next --since is the scan's start time minus CURSOR_SLACK_SECONDS,
never the newest item seen: the three activity lists are read one after the
other, so an item can appear in a list already read while a later list shows
something newer. A cursor at the newest item would skip it for good. The slack
covers GitHub's whole-second timestamps and clock skew; items inside it may be
listed again on the next run, which is harmless.

A POST's outcome is UNKNOWN unless the response proves it. gh is run with
--include, so a response GitHub actually returned begins with its status line
(`HTTP/2.0 422 Unprocessable Entity`); a 2xx is success and a 4xx (other than
408) is a proven rejection. Everything else is unknown: a timeout, EOF or a
reset (gh prints `Post "url": EOF` and exits 1 with no status line, even when
the server had read the whole body), a 5xx or 408 (a gateway can fail after
GitHub accepted the request), or output with no status line.

Exit codes: 0 done (or dry-run plan printed); 1 guard tripped, nothing posted;
2 usage or gh read error, nothing posted; 3 a post was rejected by a 4xx
response, see the report; 4 a post's outcome is UNKNOWN: GitHub may have
accepted it, so read the PR back before posting again (a retry can duplicate it).

    python tools\\pr_post.py --self-test
"""

import argparse
import datetime
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile


class ToolError(Exception):
    pass


class RunnerTimeout(ToolError):
    """gh did not answer in time. For a read that is just a failure; for a POST
    the request may still have been accepted, so the outcome is unknown."""


class PostOutcomeUnknown(ToolError):
    """The response does not prove whether GitHub accepted the POST."""


class PostRejected(ToolError):
    """GitHub returned a 4xx status: the request was refused, nothing was created."""


# GitHub timestamps have whole-second precision and its clock is not ours.
CURSOR_SLACK_SECONDS = 30


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
            raise RunnerTimeout("gh timed out (%s)" % " ".join(args[:3]))
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


SNIPPET_LIMIT = 200


def snippet(body, limit=SNIPPET_LIMIT):
    s = " ".join((body or "").split())
    return s if len(s) <= limit else s[:limit - 3] + "..."


def comment_time(item):
    """(when, edited): the later of created_at and updated_at. A comment created
    before --since and edited after it is new text the reader has not seen."""
    created = parse_ts(item["created_at"])
    updated = parse_ts(item["updated_at"]) if item.get("updated_at") else created
    return (updated, True) if updated > created else (created, False)


def revision(kind, item, stamp):
    """Short digest of what a reader sees of an item: its text, its state and
    the time it last changed. An edit changes it; the id never does."""
    raw = json.dumps([kind, item.get("id"), stamp or "", item.get("state") or "", item.get("body") or ""],
                     ensure_ascii=True)
    return hashlib.sha256(raw.encode("ascii")).hexdigest()[:8]


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
                        state=item.get("state", ""), edited=False, rev=revision("review", item, ts),
                        api="repos/%s/pulls/%d/reviews/%d" % (repo, pr, int(item["id"]))))
    for item in gh_get(gh, "%s/issues/%d/comments?per_page=100" % (base, pr), "issue comments"):
        when, edited = comment_time(item)
        out.append(dict(kind="comment", id=int(item["id"]), author=login_of(item),
                        when=when, where="", body=item.get("body") or "",
                        url=item.get("html_url", ""), state="", edited=edited,
                        rev=revision("comment", item, item.get("updated_at") or item["created_at"]),
                        api="repos/%s/issues/comments/%d" % (repo, int(item["id"]))))
    for item in gh_get(gh, "%s/pulls/%d/comments?per_page=100" % (base, pr), "review comments"):
        line = item.get("line") or item.get("original_line")
        where = "%s:%s" % (item.get("path", "?"), line if line is not None else "?")
        when, edited = comment_time(item)
        out.append(dict(kind="inline", id=int(item["id"]), author=login_of(item),
                        when=when, where=where, body=item.get("body") or "",
                        url=item.get("html_url", ""), state="", edited=edited,
                        rev=revision("inline", item, item.get("updated_at") or item["created_at"]),
                        api="repos/%s/pulls/comments/%d" % (repo, int(item["id"]))))
    return out


def find_unseen(items, since, me):
    me_l = me.lower()
    new = [i for i in items if i["when"] > since and i["author"].lower() != me_l]
    new.sort(key=lambda i: (i["when"], i["id"]))
    return new


def describe(item):
    head = "  #%d@%s  %s  %s  %s" % (item["id"], item["rev"], fmt_ts(item["when"]), item["author"], item["kind"])
    if item["state"]:
        head += " (%s)" % item["state"]
    if item["edited"]:
        head += " (edited)"
    if item["where"]:
        head += "  " + item["where"]
    full = item["url"] or "gh api " + item["api"]
    return head + "\n      " + snippet(item["body"]) + "\n      full text: " + full


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
    """{id: revision} from ID@REV,ID@REV. A bare id is a usage error: it cannot
    name which version of the text was read."""
    if text is None:
        return {}
    acked = {}
    for part in text.split(","):
        part = part.strip().lstrip("#")
        if not part:
            continue
        cid, sep, rev = part.partition("@")
        if not cid.isdigit() or not sep or not rev.strip():
            raise ToolError("--ack-new wants ID@REV (the revision the guard printed beside the id), got %r" % part)
        acked[int(cid)] = rev.strip().lower()
    return acked


STATUS_LINE = re.compile(r"HTTP/\d(?:\.\d)?[ \t]+(\d{3})\b")


def split_response(out):
    """(status, body) from `gh api --include` stdout, or (None, out) when GitHub
    returned no response. gh prints resp.Proto + resp.Status first, then the
    headers, a blank line, then the body (cli/cli pkg/cmd/api/api.go,
    processResponse); an error before any response prints nothing to stdout."""
    m = STATUS_LINE.match(out or "")
    if not m:
        return None, out or ""
    rest = out[m.end():]
    k = rest.find("\r\n\r\n")
    return int(m.group(1)), (rest[k + 4:] if k >= 0 else "")


def post_json(gh, path, body):
    """POST {"body": body} via stdin. ASCII-only JSON, so no encoding hazard.
    The outcome is unknown unless the response proves it: 2xx accepted, 4xx
    (not 408) rejected, anything else unknown."""
    payload = json.dumps({"body": body})
    try:
        rc, out, err = gh(["api", "--include", "-X", "POST", path, "--input", "-"], payload)
    except RunnerTimeout as e:
        raise PostOutcomeUnknown(str(e))
    status, text = split_response(out)
    detail = (err or text).strip()[:300]
    if status is None:
        raise PostOutcomeUnknown("POST %s: no HTTP response from gh (exit %d): %s" % (path, rc, detail))
    if 200 <= status < 300:
        try:
            return json.loads(text).get("html_url", "(no url returned)")
        except (ValueError, AttributeError):
            return "(unparseable response)"
    if 400 <= status < 500 and status != 408:
        raise PostRejected("POST %s rejected (HTTP %d): %s" % (path, status, detail))
    raise PostOutcomeUnknown("POST %s: HTTP %d does not show whether it was accepted: %s" % (path, status, detail))


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc)


def run(args, gh, out, err, clock=utc_now):
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

        # Taken before the first request: nothing created after this can be
        # in a list already read, so it bounds what a scan may have missed.
        scan_start = clock()
        items = read_activity(gh, repo, args.pr)
    except ToolError as e:
        print("pr_post: %s" % e, file=err)
        return 2

    # Never behind --since (that would re-list what the reader already saw).
    resume = max(since, scan_start - datetime.timedelta(seconds=CURSOR_SLACK_SECONDS))
    new = find_unseen(items, since, me)
    print("PR %s#%d: %d item(s) read, %d newer than %s by someone other than %s"
          % (repo, args.pr, len(items), len(new), fmt_ts(since), me), file=out)

    if new:
        for i in new:
            print(describe(i), file=out)
        want = {i["id"]: i["rev"] for i in new}
        if ack != want:
            print("\nGUARD: unseen activity. Nothing was posted.", file=out)
            print("Read the items above in full (bodies are cut to %d characters; open each full-text"
                  % SNIPPET_LIMIT, file=out)
            print("link or run its gh api command; `gh pr view --comments` omits inline comments), then rerun",
                  file=out)
            print("with --ack-new %s" % ",".join("%d@%s" % (k, want[k]) for k in sorted(want)), file=out)
            print("(the @REV changes when an item is edited, so an edit after you read it trips this again)",
                  file=out)
            if ack:
                print("(the --ack-new you gave, %s, does not match exactly)"
                      % ",".join("%d@%s" % (k, ack[k]) for k in sorted(ack)), file=out)
            print("Next --since once you have read them: %s" % fmt_ts(resume), file=out)
            return 1
        print("--ack-new matches exactly; proceeding.", file=out)
    elif ack:
        print("note: --ack-new given but nothing new was found; ignoring it.", file=out)

    plan = [("reply to inline comment %d" % cid, "repos/%s/pulls/%d/comments/%d/replies" % (repo, args.pr, cid),
             path, body) for cid, path, body in reply_bodies]
    if comment_body is not None:
        plan.append(("issue comment", "repos/%s/issues/%d/comments" % (repo, args.pr), args.comment, comment_body))

    nxt = fmt_ts(resume)
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
        except PostOutcomeUnknown as e:
            print("pr_post: %s" % e, file=err)
            print("Posted before this one: %s" % (", ".join(done) or "nothing"), file=out)
            print("OUTCOME UNKNOWN: %s. GitHub may have accepted it (a timeout, dropped connection or"
                  " 5xx does not show it did not)." % what, file=out)
            print("Read the PR back before posting again (a retry can duplicate it):", file=out)
            readback = path.split("/comments/")[0] + "/comments" if path.endswith("/replies") else path
            print("  gh api --paginate %s" % readback, file=out)
            rest = [w for w, _, _, _ in plan[n + 1:]]
            print("Not attempted: %s" % (", ".join(rest) or "nothing"), file=out)
            return 4
        except PostRejected as e:
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
    p.add_argument("--ack-new", metavar="ID@REV,ID@REV,...",
                   help="proceed past the guard; must list exactly the ID@REV pairs the guard printed "
                        "(REV is a digest of the item's text and edit time; an edit changes it)")
    p.add_argument("--dry-run", action="store_true", help="run the guard and print the plan; post nothing")
    p.add_argument("--self-test", action="store_true", help="run the built-in tests and exit")
    return p


def main(argv=None, gh=None, stdout=None, stderr=None, clock=utc_now):
    out = stdout or sys.stdout
    err = stderr or sys.stderr
    args = build_parser().parse_args(argv)
    if args.self_test:
        return self_test()
    if not (args.repo and args.pr and args.since):
        print("pr_post: --repo, --pr and --since are required", file=err)
        return 2
    return run(args, gh or make_gh_runner(), out, err, clock)


# ---------------------------------------------------------------------------
# Self-test: a fake gh, no network.
# ---------------------------------------------------------------------------

REPO = "o/r"
NOW = datetime.datetime(2026, 10, 16, 10, 0, 0, tzinfo=datetime.timezone.utc)  # the fake scan start
EM = "—"


def http_response(status, body):
    """What `gh api --include` writes to stdout for a response GitHub returned:
    resp.Proto + resp.Status, headers, a blank line, the body (captured from real
    gh 2.101.0 against api.github.com: `HTTP/2.0 404 Not Found`, headers, body)."""
    return "HTTP/2.0 %s\nContent-Type: application/json; charset=utf-8\r\nServer: github.com\r\n\r\n%s" % (status, body)


# Captured from real gh 2.101.0: a 4xx prints the status line and body on stdout and
# `gh: <message> (HTTP <code>)` on stderr, exit 1.
HTTP_422 = (1, http_response("422 Unprocessable Entity", '{"message":"Validation Failed","status":"422"}'),
            "gh: Validation Failed (HTTP 422)\n")
HTTP_502 = (1, http_response("502 Bad Gateway", "<html>Bad Gateway</html>"), "gh: HTTP 502\n")
# Captured from real gh 2.101.0 pointed at a local server that read the TLS hello and closed:
# exit 1, nothing on stdout, no status line.
EOF_DROP = (1, "", 'Post "https://localhost:18555/api/v3/repos/o/r/issues/7/comments": EOF\n')
REFUSED = (1, "", 'Post "https://localhost:1/api/v3/repos/o/r/issues/7/comments": dial tcp [::1]:1: connectex: '
                  'No connection could be made because the target machine actively refused it.\n')


class FakeGh:
    def __init__(self, reviews=(), comments=(), inline=(), me="sean", fail_post_at=None, pages=1,
                 timeout_post_at=None, on_get=None, post_fault=None):
        self.reviews, self.comments, self.inline = list(reviews), list(comments), list(inline)
        self.me, self.fail_post_at, self.pages = me, fail_post_at, pages
        self.timeout_post_at = timeout_post_at  # accept the POST, then raise the runner timeout
        self.on_get = on_get                    # called with the path before each list is served
        self.post_fault = post_fault or {}      # {post index: (rc, stdout, stderr)}: what real gh printed
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
            if self.timeout_post_at is not None and len(self.posts) - 1 == self.timeout_post_at:
                raise RunnerTimeout("gh timed out (api -X POST)")
            if self.fail_post_at is not None and len(self.posts) - 1 == self.fail_post_at:
                return HTTP_422
            if len(self.posts) - 1 in self.post_fault:
                return self.post_fault[len(self.posts) - 1]
            ok = json.dumps({"html_url": "https://example/%d" % len(self.posts)})
            # Like real gh: the status line is printed only when asked for with --include.
            return 0, (http_response("201 Created", ok) if "--include" in args else ok), ""
        path = args[-1]
        if self.on_get:
            self.on_get(path)
        if "/pulls/7/reviews" in path:
            return 0, self._paged(self.reviews), ""
        if "/issues/7/comments" in path:
            return 0, self._paged(self.comments), ""
        if "/pulls/7/comments" in path:
            return 0, self._paged(self.inline), ""
        return 1, "", "unexpected " + path


def _rv(id_, who, when, body="r"):
    return {"id": id_, "user": {"login": who}, "submitted_at": when, "body": body, "state": "COMMENTED"}


def _cm(id_, who, when, body="c", updated=None, url="https://example/c%d"):
    d = {"id": id_, "user": {"login": who}, "created_at": when, "updated_at": updated or when, "body": body,
         "html_url": url % id_ if "%d" in url else url}
    return d


def _ic(id_, who, when, body="i", path="a.py", line=3, updated=None, url="https://example/i%d"):
    d = {"id": id_, "user": {"login": who}, "created_at": when, "updated_at": updated or when, "body": body,
         "path": path, "line": line, "html_url": url % id_ if "%d" in url else url}
    return d


def guard_revs(text):
    """{id: rev} from the `#ID@REV` markers in the guard listing."""
    return {int(i): r for i, r in re.findall(r"^  #(\d+)@([0-9a-f]+) ", text, re.M)}


def self_test():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    def go(fake, argv):
        o, e = io.StringIO(), io.StringIO()
        rc = main(argv, gh=fake, stdout=o, stderr=e, clock=lambda: NOW)
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
        check("--ack-new 11@" in o, "guard: tells how to acknowledge")

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
        revs = guard_revs(go(FakeGh(**items), base + ["--comment", summ])[1])
        check(set(revs) == {11, 12}, "ack: guard prints id@rev for each item, got %r" % revs)
        a11, a12 = "11@%s" % revs.get(11), "12@%s" % revs.get(12)
        for ack, want in ((a11 + "," + a12, 0), (a12 + "," + a11, 0), (a11, 1), (a11 + ",99@abcd1234", 1),
                          ("99@abcd1234", 1), (a11 + "," + a12 + ",99@abcd1234", 1),
                          ("11@00000000," + a12, 1), (a11 + ",12@00000000", 1)):
            f = FakeGh(**items)
            rc, o, e = go(f, base + ["--comment", summ, "--ack-new", ack])
            check(rc == want, "ack %s: want exit %d got %r" % (ack, want, rc))
            check(bool(f.posts) == (want == 0), "ack %s: posts iff accepted" % ack)
        # A bare id (the old form) or a missing revision is refused as usage: it cannot name what was read.
        for ack in ("11,12", "11@", "@abcd1234", "x@abcd1234"):
            f = FakeGh(**items)
            rc, o, e = go(f, base + ["--comment", summ, "--ack-new", ack])
            check(rc == 2 and not f.posts and "ID@REV" in e, "ack %r: refused as usage (rc=%r)" % (ack, rc))

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
        check("Next --since once you have read them: 2026-10-16T09:59:30Z" in o,
              "next since on trip is the scan start less the slack, not the newest item")

        # Finding 1: the lists are read one after another. A review lands after
        # the reviews list was served, then a comment lands before the comments
        # list is served. The cursor must stay behind the review nobody saw.
        t_scan = "2026-10-16T10:00:01Z"
        for label, extra in (("guard trip", []), ("successful post", ["--ack-new", "31@" + revision("comment", _cm(31, "bob", "2026-10-16T10:00:02Z", "seen by this scan"), "2026-10-16T10:00:02Z")]), ("dry run", ["--dry-run"])):
            late_review = _rv(30, "bob", t_scan, "arrived after the reviews list was read")
            f = FakeGh()
            seen = {"reviews": False}

            def on_get(path, f=f, late_review=late_review, seen=seen):
                if "/pulls/7/reviews" in path:
                    seen["reviews"] = True
                elif "/issues/7/comments" in path and seen["reviews"] and not f.comments:
                    f.reviews.append(late_review)  # missed by this scan
                    f.comments.append(_cm(31, "bob", "2026-10-16T10:00:02Z", "seen by this scan"))
            f.on_get = on_get
            rc, o, e = go(f, base + ["--comment", summ] + extra)
            m = [ln for ln in o.splitlines() if "Next --since" in ln]
            check(len(m) == 1, "interleave %s: one cursor line, got %r" % (label, m))
            cursor = m[0].rsplit(": ", 1)[1].strip() if m else ""
            check(cursor and cursor < t_scan,
                  "interleave %s: cursor %r must stay behind the unlisted review at %s" % (label, cursor, t_scan))
            check("#30" not in o, "interleave %s: the late review was not in this scan" % label)
            f2 = FakeGh(reviews=[late_review], comments=[_cm(31, "bob", "2026-10-16T10:00:02Z")])
            rc2, o2, e2 = go(f2, ["--repo", REPO, "--pr", "7", "--since", cursor, "--me", "sean", "--comment", summ])
            check(rc2 == 1 and "#30" in o2, "interleave %s: rerun from the cursor lists the review" % label)
        # --since later than the scan start (a future cursor) is never moved backwards.
        f = FakeGh()
        rc, o, e = go(f, ["--repo", REPO, "--pr", "7", "--since", "2026-10-16T11:00:00Z", "--me", "sean", "--comment", summ])
        check("Next --since: 2026-10-16T11:00:00Z" in o, "cursor never earlier than --since")

        # Finding 2: a comment created before --since and edited after it is unseen,
        # for both issue and inline comments; an edit before --since is not.
        f = FakeGh(comments=[_cm(41, "bob", old, "edited text", updated=new)],
                   inline=[_ic(42, "bob", old, "edited inline", updated=new),
                           _ic(43, "bob", old, "old edit", updated="2026-10-15T13:00:00Z")])
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 1 and "#41" in o and "#42" in o and "#43" not in o, "edited comments: only post-since edits listed")
        check("(edited)" in o, "edited comments: marked as edited")
        f = FakeGh(comments=[_cm(41, "bob", old, updated=new)], inline=[_ic(42, "bob", old, updated=new)])
        revs = guard_revs(go(f, base + ["--comment", summ])[1])
        f = FakeGh(comments=[_cm(41, "bob", old, updated=new)], inline=[_ic(42, "bob", old, updated=new)])
        rc, o, e = go(f, base + ["--comment", summ, "--ack-new", "41@%s,42@%s" % (revs.get(41), revs.get(42))])
        check(rc == 0 and len(f.posts) == 1, "edited comments: acknowledged by id@rev")
        f = FakeGh(comments=[_cm(44, "sean", old, updated=new)])
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 0, "edited comments: my own edit is not unseen")

        # Finding 3: a timed-out POST may have been accepted. Distinct report and
        # exit code, never "NOT posted" for it, no further posts, read-back named.
        f = FakeGh(timeout_post_at=1)
        rc, o, e = go(f, base + ["--reply", "5=" + r1, "--reply", "6=" + r2, "--comment", summ])
        check(rc == 4, "timeout: exit 4, got %r" % rc)
        check(len(f.posts) == 2, "timeout: nothing posted after the timed-out request, got %d" % len(f.posts))
        check("OUTCOME UNKNOWN: reply to inline comment 6" in o and "may have accepted" in o, "timeout: outcome reported unknown")
        check("Posted before this one: reply to inline comment 5" in o, "timeout: earlier posts reported")
        check("Not attempted: issue comment" in o, "timeout: untried posts reported as not attempted")
        check("NOT posted" not in o, "timeout: never claims the timed-out post was not posted")
        check("Read the PR back before posting again" in o and "gh api --paginate repos/o/r/pulls/7/comments" in o,
              "timeout: read-back required and named")
        f = FakeGh(timeout_post_at=0)
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 4 and "gh api --paginate repos/o/r/issues/7/comments" in o, "timeout: issue comment read-back path")
        # A timeout while reading is an ordinary read error: nothing was posted.
        def boom(path):
            raise RunnerTimeout("gh timed out (api --paginate)")
        f = FakeGh(on_get=boom)
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 2 and not f.posts, "read timeout: exit 2, nothing posted")

        # Review 2, finding 2: an edit keeps the id. Sean's two runs: list #41, stop; then #41's
        # body and updated_at change; rerunning with the first run's --ack-new must trip again.
        t1, t2 = "2026-10-16T08:00:00Z", "2026-10-16T09:30:00Z"
        f = FakeGh(comments=[_cm(41, "bob", old, "first text", updated=t1)])
        rc, o, e = go(f, base + ["--comment", summ])
        rev1 = guard_revs(o).get(41)
        check(rc == 1 and rev1, "revision: first run lists #41 with a revision")
        f = FakeGh(comments=[_cm(41, "bob", old, "first text", updated=t1)])
        rc, o, e = go(f, base + ["--comment", summ, "--ack-new", "41@%s" % rev1])
        check(rc == 0 and len(f.posts) == 1, "revision: unedited item with its revision passes")
        f = FakeGh(comments=[_cm(41, "bob", old, "REVISED text", updated=t2)])
        rc, o, e = go(f, base + ["--comment", summ, "--ack-new", "41@%s" % rev1])
        check(rc == 1 and not f.posts, "revision: edited item (new body, new updated_at) trips again, rc=%r" % rc)
        check("REVISED text" in o and guard_revs(o).get(41) not in (None, rev1),
              "revision: new text shown with a new revision to acknowledge")
        f = FakeGh(comments=[_cm(41, "bob", old, "silent edit", updated=t1)])
        check(guard_revs(go(f, base + ["--comment", summ])[1]).get(41) != rev1, "revision: body change alone changes it")
        f = FakeGh(comments=[_cm(41, "bob", old, "first text", updated=t2)])
        check(guard_revs(go(f, base + ["--comment", summ])[1]).get(41) != rev1, "revision: updated_at change alone changes it")
        for label, mk in (("review", lambda b, t: dict(reviews=[_rv(61, "bob", t, b)])),
                          ("inline", lambda b, t: dict(inline=[_ic(61, "bob", t, b, updated=t)]))):
            rv1 = guard_revs(go(FakeGh(**mk("one", new)), base + ["--comment", summ])[1]).get(61)
            f = FakeGh(**mk("two", new))
            rc, o, e = go(f, base + ["--comment", summ, "--ack-new", "61@%s" % rv1])
            check(rc == 1 and not f.posts, "revision %s: edited body trips again" % label)

        # Review 2, finding 1: a POST's outcome is unknown unless the response proves a rejection.
        for label, fault in (("dropped response (EOF)", EOF_DROP), ("connection refused", REFUSED),
                             ("502", HTTP_502), ("504", (1, http_response("504 Gateway Timeout", ""), "gh: HTTP 504\n")),
                             ("408", (1, http_response("408 Request Timeout", ""), "gh: HTTP 408\n")),
                             ("exit 0 with no status line", (0, "", "")),
                             ("garbage output", (1, "<<<not http>>>", "boom"))):
            f = FakeGh(post_fault={1: fault})
            rc, o, e = go(f, base + ["--reply", "5=" + r1, "--reply", "6=" + r2, "--comment", summ])
            check(rc == 4, "%s: exit 4 (unknown), got %r" % (label, rc))
            check("NOT posted" not in o, "%s: must not claim NOT posted" % label)
            check("OUTCOME UNKNOWN: reply to inline comment 6" in o and "Not attempted: issue comment" in o,
                  "%s: unknown outcome and untried posts reported" % label)
            check("gh api --paginate repos/o/r/pulls/7/comments" in o, "%s: read-back named" % label)
            check(len(f.posts) == 2, "%s: stopped after it, got %d posts" % (label, len(f.posts)))
        for label, fault in (("422", HTTP_422), ("404", (1, http_response("404 Not Found", '{"message":"Not Found"}'),
                                                         "gh: Not Found (HTTP 404)\n")),
                             ("403", (1, http_response("403 Forbidden", '{"message":"x"}'), "gh: x (HTTP 403)\n"))):
            f = FakeGh(post_fault={1: fault})
            rc, o, e = go(f, base + ["--reply", "5=" + r1, "--reply", "6=" + r2, "--comment", summ])
            check(rc == 3, "%s: proven rejection exit 3, got %r" % (label, rc))
            check("NOT posted: reply to inline comment 6, issue comment" in o and "OUTCOME UNKNOWN" not in o,
                  "%s: reported as not posted" % label)
        f = FakeGh(post_fault={0: (0, http_response("201 Created", "not json"), "")})
        rc, o, e = go(f, base + ["--comment", summ])
        check(rc == 0 and "unparseable response" in o, "2xx with a bad body is posted, rc=%r" % rc)

        # Finding 4: every listed item carries a full-text route that works for inline comments.
        f = FakeGh(inline=[_ic(51, "bob", new, "x" * 500, url="https://github.com/o/r/pull/7#discussion_r51")],
                   comments=[_cm(52, "bob", new, url="")])
        rc, o, e = go(f, base + ["--comment", summ])
        check("full text: https://github.com/o/r/pull/7#discussion_r51" in o, "full text: inline html_url printed")
        check("full text: gh api repos/o/r/issues/comments/52" in o, "full text: api command when there is no url")
        check("x" * 300 not in o, "full text: body still cut in the list")
        check("gh pr view" in o and "omits inline comments" in o, "full text: warns --comments omits inline")

    if fails:
        for m in fails:
            print("FAIL: " + m)
        print("pr_post self-test: %d failure(s)" % len(fails))
        return 1
    print("pr_post self-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
