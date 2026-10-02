#!/usr/bin/env python3
"""Build the contributor-credit list for a release, from sources.

Maintainer's rule (2026-10-02): every release's notes credit ALL contributors
by public GitHub handle, built from sources and not from memory. This tool
reads the commits, the pull requests, the issues and the docs diff of a
range, and lists WHO appears WHERE. It never writes a description of anyone's
contribution: a human writes the sentence.

    python tools\\release_credits.py --from v0.18.0 --thanks
    python tools\\release_credits.py --from v0.18.0 --to origin/main
    python tools\\release_credits.py --from v0.18.0 --format markdown --out credits.md
    python tools\\release_credits.py --from v0.18.0 --format json
    python tools\\release_credits.py --self-test

Sources (all read-only; git through `git log` / `git diff` / `git rev-list`,
GitHub through `gh api` / `gh pr` / `gh issue` / `gh repo view`):

  1. CODE      Authors and `Co-authored-by:` trailers of the non-merge commits
               in <from>..<to>. AI identities (email noreply@anthropic.com, or
               a name that is the word Claude) go in their own list and are
               never mapped or credited; GitHub bots are skipped. A human's
               login comes from, in order: a users.noreply.github.com email,
               the GitHub commit-author API for one of their commits, the
               author of the merged PR whose merge commit is their commit.
               Nobody resolved is listed by NAME under "unmapped authors"; the
               tool never guesses a handle.
  2. PRS       Merged PRs whose merge commit is in the range (author listed);
               open or closed-unmerged PRs by non-maintainers updated since
               the date of <from> go under "PRs not merged (credit needs a
               decision)".
  3. ISSUES    Three sources, each tagged on the row ("found by"):
                 commit  `#N` / `issue N` in a commit message in the range;
                 docs    `#N` / `issue N` in a line ADDED under docs\\ in
                         <from>..<to>;
                 window  any issue or PR created, closed, or commented on
                         (by a real comment timestamp, so a re-label does
                         not count) since the committer date of <from>.
               A number is kept only when it exists as an issue or a PR
               (draw-call indices do not; numbers above the repository's
               newest are dropped without a lookup). Each row lists the
               reporter, the commenters with comment counts (maintainer and
               bots excluded) and the commits that cite it, if any. A
               commenter is credited only for comments made since the window
               start; a reporter is credited for any issue that is listed. A
               PR found this way is listed with its author, and its author
               is credited by the PR sections, not as an issue reporter.
  4. DOCS      Lines added under docs\\ that mention `user N`, supporter,
               tester or the reporter: people known only by an anonymised
               label. A prompt for the maintainer, never a credit.

The maintainer is shown in the code summary but left out of the Thanks
skeleton unless --include-maintainer. No email address is ever printed.

--thanks prints only the `## Thanks` skeleton (one TODO line per resolved
person, merged-PR authors first, then other code authors, issue reporters,
issue commenters; `- TODO: AI assistance disclosure` only when AI co-authors
were found). What needs a decision (unmapped authors, docs mentions, PRs not
merged) goes to stderr so stdout stays pasteable. --format json always
carries the whole report, with a `thanks` array.

--out FILE writes the report as UTF-8 without BOM, LF endings. With --dry-run
it prints what it would write and creates no file and no directory.

Exit codes: 0 ok; 1 usage or tool failure (git or gh missing or failing, bad
rev); 2 when --thanks finds unmapped authors or docs mentions of reporters
with no handle (so a script can notice; the skeleton is still printed).
"""
import argparse
import contextlib
import datetime
import io
import json
import os
import re
import subprocess
import sys
import tempfile

SCHEMA = 1
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PR_LIMIT = 200
GH_PR_FIELDS = "number,title,author,state,mergedAt,mergeCommit,url,updatedAt"
GH_ISSUE_FIELDS = "number,title,author,state,createdAt,closedAt,comments,url"
GH_ISSUE_LIST_FIELDS = "number,title,author,state,updatedAt"
AI_EMAIL = "noreply@anthropic.com"
EXCERPT_MAX = 100
RESOLVE_TRIES = 3

# An email-looking string. Applied to every free-text field that reaches the
# output (names, titles, doc excerpts) so no address can print.
EMAIL_RE = re.compile(r"[A-Za-z0-9._%+\-\[\]]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)+")
NOREPLY_RE = re.compile(
    r"^(?:\d+\+)?([A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\[bot\])?)"
    r"@users\.noreply\.github\.com$",
    re.I,
)
LOGIN_RE = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\[bot\])?$")
COAUTHOR_RE = re.compile(
    r"^[ \t]*co-authored-by:[ \t]*(.*?)[ \t]*<([^<>\r\n]*)>[ \t]*$", re.I | re.M
)
ISSUE_HASH_RE = re.compile(r"#(\d+)\b")
ISSUE_WORD_RE = re.compile(r"\bissues?\s+(\d+)\b", re.I)
DOCS_ISSUE_RE = re.compile(r"(?i)\bissues? #?(\d+)\b|#(\d+)\b")
REPORTER_RE = re.compile(r"(?i)\buser ?\d+\b|\bsupporter\b|\btester\b|\bthe reporter\b")
NOT_FOUND_RE = re.compile(r"could not resolve|not found|\b404\b", re.I)
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
FIELD_SEP, RECORD_SEP = "\x1f", "\x1e"


class ToolError(Exception):
    """A tool failure: printed as one line, exit code 1."""


# ---------------------------------------------------------------------------
# The two runners. Everything else goes through them, so the self-test
# substitutes fakes. A runner takes an argument list and returns
# (returncode, stdout, stderr), decoded as UTF-8.
# ---------------------------------------------------------------------------

def _run(argv, cwd=None, env=None, timeout=180):
    try:
        p = subprocess.run(
            argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, shell=False,
        )
    except FileNotFoundError:
        raise ToolError("%s was not found on PATH" % argv[0])
    except subprocess.TimeoutExpired:
        raise ToolError("%s timed out after %d s (%s)" % (argv[0], timeout, " ".join(argv[1:4])))
    return (
        p.returncode,
        p.stdout.decode("utf-8", "replace"),
        p.stderr.decode("utf-8", "replace"),
    )


def make_git_runner(cwd):
    def git(args):
        return _run(["git", "-c", "core.quotepath=false"] + list(args), cwd=cwd)
    return git


def make_gh_runner(cwd):
    env = dict(os.environ, GH_PROMPT_DISABLED="1", NO_COLOR="1")

    def gh(args):
        return _run(["gh"] + list(args), cwd=cwd, env=env)
    return gh


def first_line(text, limit=200):
    for line in text.splitlines():
        if line.strip():
            return line.strip()[:limit]
    return ""


def git_out(git, args, what):
    rc, out, err = git(args)
    if rc != 0:
        raise ToolError("git %s failed (exit %d): %s" % (what, rc, first_line(err)))
    return out


def parse_json(text, what):
    try:
        return json.loads(text)
    except ValueError as e:
        raise ToolError("could not parse the %s output as JSON: %s" % (what, e))


def scrub(text):
    """Remove anything that looks like an email address."""
    return EMAIL_RE.sub("<email removed>", text or "")


def plural(n, word):
    return "%d %s%s" % (n, word, "" if n == 1 else "s")


def parse_iso(text):
    t = (text or "").strip()
    if not t:
        return None
    if t.endswith("Z"):
        t = t[:-1] + "+00:00"
    try:
        d = datetime.datetime.fromisoformat(t)
    except ValueError:
        return None
    if d.tzinfo is None:
        d = d.replace(tzinfo=datetime.timezone.utc)
    return d


# ---------------------------------------------------------------------------
# Identities
# ---------------------------------------------------------------------------

def is_ai_identity(name, email):
    if (email or "").strip().lower() == AI_EMAIL:
        return True
    # The word Claude, not any name that merely begins with those letters.
    return re.match(r"(?i)claude\b", (name or "").strip()) is not None


def is_bot_identity(name, email):
    n = (name or "").lower()
    e = (email or "").strip().lower()
    return "[bot]" in n or e == "noreply@github.com" or "[bot]@users.noreply.github.com" in e


def classify_identity(name, email):
    """'ai', 'bot' or 'human'."""
    if is_ai_identity(name, email):
        return "ai"
    if is_bot_identity(name, email):
        return "bot"
    return "human"


def is_bot_login(login, flagged=False):
    low = (login or "").lower()
    return bool(
        flagged
        or low.endswith("[bot]")
        or low.startswith("app/")
        or low in ("ghost", "github-actions", "dependabot", "copilot")
    )


def noreply_login(email):
    m = NOREPLY_RE.match((email or "").strip())
    return m.group(1) if m else None


def valid_login(text):
    t = (text or "").strip()
    if not t or t.lower() == "null":
        return None
    return t if LOGIN_RE.match(t) else None


# ---------------------------------------------------------------------------
# Reading the range
# ---------------------------------------------------------------------------

def resolve_rev(git, rev):
    if not rev or rev.startswith("-"):
        raise ToolError("bad rev %r" % rev)
    rc, out, err = git(["rev-parse", "--verify", "--quiet", rev + "^{commit}"])
    sha = out.strip()
    if rc != 0 or not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ToolError("bad rev %r: not a commit in this repository" % rev)
    return sha


def read_commits(git, from_sha, to_sha):
    fmt = "%H%x1f%an%x1f%ae%x1f%B%x1e"
    raw = git_out(
        git, ["log", "--no-merges", "--format=" + fmt, "%s..%s" % (from_sha, to_sha), "--"],
        "log",
    )
    commits = []
    for rec in raw.split(RECORD_SEP):
        rec = rec.lstrip("\n")
        if not rec.strip():
            continue
        parts = rec.split(FIELD_SEP, 3)
        if len(parts) != 4:
            continue
        sha, name, email, message = parts
        co = [(n.strip(), e.strip()) for n, e in COAUTHOR_RE.findall(message)]
        commits.append({"sha": sha, "name": name.strip(), "email": email.strip(),
                        "message": message, "coauthors": co})
    return commits  # newest first, as git prints them


def read_range_shas(git, from_sha, to_sha):
    out = git_out(git, ["rev-list", "%s..%s" % (from_sha, to_sha), "--"], "rev-list")
    return set(out.split())


def issue_refs(message):
    nums = set()
    for rx in (ISSUE_HASH_RE, ISSUE_WORD_RE):
        for m in rx.finditer(message):
            n = int(m.group(1))
            if 0 < n < 10_000_000:
                nums.add(n)
    return nums


def iter_added_lines(diff_text):
    """Yield (file, line number, text) for every line a `git diff --unified=0`
    adds."""
    path, newline, in_hunk = None, 0, False
    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            path, in_hunk = None, False
            continue
        if not in_hunk:
            if line.startswith("+++ "):
                p = line[4:].split("\t")[0]
                path = None if p == "/dev/null" else (p[2:] if p.startswith("b/") else p)
                continue
            m = HUNK_RE.match(line)
            if m:
                newline, in_hunk = int(m.group(1)), True
            continue
        m = HUNK_RE.match(line)
        if m:
            newline = int(m.group(1))
        elif line.startswith("+"):
            if path:
                yield path, newline, line[1:]
            newline += 1
        elif line.startswith("-") or line.startswith("\\"):
            pass
        else:
            newline += 1


def scan_docs_diff(diff_text):
    """Added lines of a `git diff --unified=0` that mention a reporter by a
    label. Returns [{"file", "line", "text"}], first-seen order, no repeats."""
    hits, seen = [], set()
    for path, lineno, text in iter_added_lines(diff_text):
        hit = REPORTER_RE.search(text)
        if hit and (path, lineno) not in seen:
            seen.add((path, lineno))
            clean = scrub(text)
            hits.append({"file": path, "line": lineno,
                         "text": excerpt(clean, clean.find(hit.group(0)))})
    return hits


def docs_issue_numbers(diff_text):
    """Issue or PR numbers cited in the lines a docs diff adds."""
    nums = set()
    for _path, _lineno, text in iter_added_lines(diff_text):
        for m in DOCS_ISSUE_RE.finditer(text):
            n = int(m.group(1) or m.group(2))
            if 0 < n < 10_000_000:
                nums.add(n)
    return nums


def excerpt(text, centre=0):
    s = " ".join(text.split())
    if len(s) <= EXCERPT_MAX:
        return s
    centre = max(0, min(centre, len(s)))
    start = max(0, min(centre - EXCERPT_MAX // 3, len(s) - (EXCERPT_MAX - 3)))
    chunk = s[start:start + EXCERPT_MAX - 3]
    return chunk + "..."


# ---------------------------------------------------------------------------
# GitHub reads
# ---------------------------------------------------------------------------

def read_pr_list(gh, repo):
    rc, out, err = gh(["pr", "list", "--repo", repo, "--state", "all", "--limit", str(PR_LIMIT),
                       "--json", GH_PR_FIELDS])
    if rc != 0:
        raise ToolError("gh pr list failed (exit %d): %s" % (rc, first_line(err)))
    data = parse_json(out, "gh pr list")
    if not isinstance(data, list):
        raise ToolError("gh pr list did not return a list")
    return data


def pr_author(pr):
    a = pr.get("author") or {}
    login = valid_login(a.get("login"))
    return login, is_bot_login(login, bool(a.get("is_bot")))


def read_issue(gh, repo, number):
    rc, out, err = gh(["issue", "view", str(number), "--repo", repo, "--json", GH_ISSUE_FIELDS])
    if rc != 0:
        if NOT_FOUND_RE.search(err):
            return None
        raise ToolError("gh issue view %d failed (exit %d): %s" % (number, rc, first_line(err)))
    data = parse_json(out, "gh issue view %d" % number)
    return data if isinstance(data, dict) else None


def read_max_number(gh, repo, warnings):
    """The newest issue-or-PR number in the repository (they share one
    sequence), or None. A cited number above it cannot exist."""
    rc, out, err = gh(["api", "repos/%s/issues?state=all&per_page=1" % repo, "--jq", ".[0].number"])
    text = out.strip()
    if rc == 0 and text.isdigit():
        return int(text)
    warnings.append("could not read the newest issue number (%s); every cited number is looked up"
                    % (first_line(err, 80) or "no output"))
    return None


def read_window_issues(gh, repo, from_date, warnings):
    """Numbers of issues GitHub says were updated since the window start. A
    superset: the real activity is judged from the comment timestamps."""
    if from_date is None:
        return []
    day = from_date.astimezone(datetime.timezone.utc).date() - datetime.timedelta(days=1)
    rc, out, err = gh(["issue", "list", "--repo", repo, "--state", "all", "--limit", str(PR_LIMIT),
                       "--search", "updated:>=%s" % day.isoformat(), "--json", GH_ISSUE_LIST_FIELDS])
    if rc != 0:
        raise ToolError("gh issue list failed (exit %d): %s" % (rc, first_line(err)))
    data = parse_json(out, "gh issue list")
    if not isinstance(data, list):
        raise ToolError("gh issue list did not return a list")
    if len(data) >= PR_LIMIT:
        warnings.append("gh issue list returned %d issues, its limit; older activity may be missing"
                        % PR_LIMIT)
    return [d["number"] for d in data if isinstance(d, dict) and isinstance(d.get("number"), int)]


def stamp_since(stamp, start):
    """True when a GitHub timestamp is present and at or after the window
    start (an issue's creation or closing)."""
    when = parse_iso(stamp)
    return start is not None and when is not None and when >= start


def comment_in_window(stamp, start):
    """True when a comment was made since the window start. A comment whose
    time is missing cannot be shown to predate it, so it counts."""
    when = parse_iso(stamp)
    return start is None or when is None or when >= start


# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------

class Person:
    def __init__(self, login):
        self.login = login
        self.prs = set()        # PR numbers: authored (merged) or commented on
        self.issues = set()     # issue numbers: reported or commented on
        self.commits = 0
        self.merged_prs = set()
        self.reported = set()
        self.comments = 0

    def group(self):
        if self.merged_prs:
            return 0
        if self.commits:
            return 1
        if self.reported:
            return 2
        return 3

    def rank(self):
        g = self.group()
        key = self.login.lower()
        if g == 0:
            return (min(self.merged_prs), key)
        if g == 1:
            return (-self.commits, key)
        if g == 2:
            return (min(self.reported), key)
        return (-self.comments, key)

    def sources(self):
        out = ["PR #%d" % n for n in sorted(self.prs)]
        out += ["issue #%d" % n for n in sorted(self.issues)]
        if self.commits:
            out.append(plural(self.commits, "commit"))
        return out


GROUP_NAMES = ("merged_pr_author", "code_author", "issue_reporter", "issue_commenter")


def build_report(git, gh, repo, from_rev, to_rev, maintainer, include_maintainer):
    warnings = []
    from_sha = resolve_rev(git, from_rev)
    to_sha = resolve_rev(git, to_rev)
    from_date_iso = git_out(git, ["log", "-1", "--format=%cI", from_sha, "--"], "log -1").strip()
    from_date = parse_iso(from_date_iso)
    maint = (maintainer or "").lower()

    commits = read_commits(git, from_sha, to_sha)
    range_shas = read_range_shas(git, from_sha, to_sha)

    # -- identities -------------------------------------------------------
    idents = {}      # email key -> {name, email, authored[shas], shas:set}
    ai = {}          # casefolded name -> {name, shas:set}
    bots = {}        # casefolded name -> {name, shas:set}

    def note(name, email, sha, authored):
        kind = classify_identity(name, email)
        if kind in ("ai", "bot"):
            bucket = ai if kind == "ai" else bots
            slot = bucket.setdefault(name.casefold(), {"name": name, "shas": set()})
            slot["shas"].add(sha)
            return
        key = email.lower() if email else "name:" + name.lower()
        slot = idents.setdefault(key, {"name": name, "email": email, "authored": [], "shas": set()})
        slot["shas"].add(sha)
        if authored and sha not in slot["authored"]:
            slot["authored"].append(sha)

    for c in commits:
        note(c["name"], c["email"], c["sha"], True)
        for n, e in c["coauthors"]:
            note(n, e, c["sha"], False)

    # -- PRs --------------------------------------------------------------
    prs = read_pr_list(gh, repo)
    if len(prs) >= PR_LIMIT:
        warnings.append("gh pr list returned %d PRs, its limit; older PRs may be missing" % PR_LIMIT)
    merged_by_sha = {}
    prs_merged, prs_not_merged = [], []
    for pr in prs:
        number = pr.get("number")
        if not isinstance(number, int):
            continue
        login, bot = pr_author(pr)
        state = str(pr.get("state") or "").upper()
        oid = (pr.get("mergeCommit") or {}).get("oid") or ""
        if oid and state == "MERGED":
            merged_by_sha[oid] = login
        if state == "MERGED" and oid in range_shas:
            prs_merged.append({"number": number, "title": scrub(pr.get("title") or ""),
                               "author": login, "bot": bot, "merge_commit": oid[:8]})
        elif state in ("OPEN", "CLOSED") and not pr.get("mergedAt"):
            updated = parse_iso(pr.get("updatedAt"))
            fresh = updated is None or from_date is None or updated >= from_date
            if fresh and not bot and not (login and login.lower() == maint):
                prs_not_merged.append({"number": number, "title": scrub(pr.get("title") or ""),
                                       "author": login, "state": state})
    prs_merged.sort(key=lambda p: p["number"])
    prs_not_merged.sort(key=lambda p: p["number"])

    # -- resolving humans to logins ---------------------------------------
    def resolve(ident):
        login = noreply_login(ident["email"])
        if login:
            return login
        for sha in ident["authored"][:RESOLVE_TRIES]:
            rc, out, err = gh(["api", "repos/%s/commits/%s" % (repo, sha), "--jq", ".author.login"])
            if rc == 0:
                login = valid_login(out)
                if login:
                    return login
            else:
                w = "gh api commits/%s failed: %s" % (sha[:8], first_line(err, 120))
                if w not in warnings:
                    warnings.append(w)
        for sha in ident["authored"]:
            login = valid_login(merged_by_sha.get(sha))
            if login:
                return login
        return None

    by_login = {}    # casefolded login -> {login, shas:set}
    unmapped = {}    # casefolded name -> {name, shas:set}
    for key in sorted(idents):
        ident = idents[key]
        login = resolve(ident)
        if login and is_bot_login(login):
            bots.setdefault(ident["name"].casefold(), {"name": ident["name"], "shas": set()})["shas"] |= ident["shas"]
        elif login:
            slot = by_login.setdefault(login.lower(), {"login": login, "shas": set()})
            slot["shas"] |= ident["shas"]
        else:
            slot = unmapped.setdefault(ident["name"].casefold(), {"name": ident["name"], "shas": set()})
            slot["shas"] |= ident["shas"]

    # -- issues -----------------------------------------------------------
    refs = {}  # number -> short hashes, oldest first
    for c in reversed(commits):
        for n in sorted(issue_refs(c["message"])):
            lst = refs.setdefault(n, [])
            if c["sha"][:8] not in lst:
                lst.append(c["sha"][:8])
    diff = git_out(git, ["diff", "--no-color", "--no-ext-diff", "--unified=0",
                         "%s..%s" % (from_sha, to_sha), "--", "docs"], "diff")
    docs_mentions = scan_docs_diff(diff)
    docs_refs = docs_issue_numbers(diff)

    # The activity window: what GitHub says was updated since <from>, issues
    # and PRs alike. The real activity is judged below from timestamps.
    window_nums = set(read_window_issues(gh, repo, from_date, warnings))
    for pr in prs:
        if isinstance(pr.get("number"), int) and stamp_since(pr.get("updatedAt"), from_date):
            window_nums.add(pr["number"])
    newest = read_max_number(gh, repo, warnings)

    issues = []
    for n in sorted(set(refs) | docs_refs | window_nums):
        if newest is not None and n > newest:
            continue  # above the repository's newest number: a draw-call index, say
        data = read_issue(gh, repo, n)
        if data is None:
            continue  # not an issue or a PR
        is_pr = "/pull/" in str(data.get("url") or "")
        author = data.get("author") or {}
        reporter = valid_login(author.get("login"))
        active = stamp_since(data.get("createdAt"), from_date) or stamp_since(data.get("closedAt"), from_date)
        counts = {}
        for cm in data.get("comments") or []:
            a = cm.get("author") or {}
            login = valid_login(a.get("login"))
            if not login or is_bot_login(login, bool(a.get("is_bot"))):
                continue
            recent = comment_in_window(cm.get("createdAt"), from_date)
            if recent:
                active = True  # any human comment since the window start, the maintainer's included
            if login.lower() == maint:
                continue
            slot = counts.setdefault(login.lower(), [login, 0, 0])
            slot[1] += 1
            slot[2] += 1 if recent else 0
        found_by = []
        if n in refs:
            found_by.append("commit")
        if n in docs_refs:
            found_by.append("docs")
        if active:
            found_by.append("window")
        if not found_by:
            continue  # updated in the window, but only re-labelled or edited
        commenters = sorted(({"login": v[0], "comments": v[1], "in_window": v[2]} for v in counts.values()),
                            key=lambda d: (-d["in_window"], -d["comments"], d["login"].lower()))
        issues.append({
            "number": n,
            "kind": "pr" if is_pr else "issue",
            "title": scrub(str(data.get("title") or "")),
            "state": str(data.get("state") or "").upper(),
            "reporter": reporter,
            "reporter_bot": is_bot_login(reporter, bool(author.get("is_bot"))) if reporter else False,
            "commenters": commenters,
            "commits": refs.get(n, []),
            "found_by": found_by,
        })

    # -- the Thanks skeleton ------------------------------------------------
    people = {}

    def person(login):
        return people.setdefault(login.lower(), Person(login))

    for p in prs_merged:
        if p["author"] and not p["bot"]:
            who = person(p["author"])
            who.prs.add(p["number"])
            who.merged_prs.add(p["number"])
    for slot in by_login.values():
        who = person(slot["login"])
        who.commits = len(slot["shas"])
    for it in issues:
        if it["kind"] == "issue" and it["reporter"] and not it["reporter_bot"]:
            who = person(it["reporter"])
            who.issues.add(it["number"])
            who.reported.add(it["number"])
        for cm in it["commenters"]:
            if not cm["in_window"]:
                continue  # only comments from before the window: credited in an earlier release
            who = person(cm["login"])
            (who.prs if it["kind"] == "pr" else who.issues).add(it["number"])
            who.comments += cm["in_window"]
    thanks = []
    for who in sorted(people.values(), key=lambda w: (w.group(), w.rank())):
        if who.login.lower() == maint and not include_maintainer:
            continue
        thanks.append({"login": who.login, "group": GROUP_NAMES[who.group()],
                       "sources": who.sources(), "commits": who.commits})

    code = sorted(
        ({"login": s["login"], "commits": len(s["shas"]), "maintainer": s["login"].lower() == maint}
         for s in by_login.values()),
        key=lambda d: (-d["commits"], d["login"].lower()))

    def named(bucket):
        return sorted(({"name": scrub(v["name"]), "commits": len(v["shas"])} for v in bucket.values()),
                      key=lambda d: (-d["commits"], d["name"].lower()))

    return {
        "schema": SCHEMA,
        "repo": repo,
        "from": from_rev,
        "to": to_rev,
        "from_commit": from_sha[:8],
        "to_commit": to_sha[:8],
        "from_date": from_date_iso,
        "maintainer": maintainer,
        "commit_count": len(commits),
        "code": code,
        "ai_coauthors": named(ai),
        "bots_skipped": named(bots),
        "unmapped_authors": named(unmapped),
        "prs_merged": [{k: v for k, v in p.items() if k != "bot"} for p in prs_merged],
        "prs_not_merged": prs_not_merged,
        "issues": [{k: v for k, v in it.items() if k != "reporter_bot"} for it in issues],
        "docs_reporter_mentions": docs_mentions,
        "thanks": thanks,
        "ai_disclosure_needed": bool(ai),
        "warnings": warnings,
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def at(login):
    return "@" + login if login else "(no handle)"


def comment_note(c):
    """`3`, or `3, 1 in window` / `3, none in window` when some predate it."""
    if c["in_window"] == c["comments"]:
        return str(c["comments"])
    return "%d, %s in window" % (c["comments"], c["in_window"] or "none")


def sections(report):
    """[(title, [(head, [sub lines])])] shared by the text and markdown forms."""
    code = []
    for c in report["code"]:
        code.append(("%s  %s%s" % (at(c["login"]), plural(c["commits"], "commit"),
                                   " (maintainer)" if c["maintainer"] else ""), []))
    if report["bots_skipped"]:
        code.append(("bots skipped: " + ", ".join(
            "%s (%s)" % (b["name"], plural(b["commits"], "commit")) for b in report["bots_skipped"]), []))
    ai = [("%s  %s" % (a["name"], plural(a["commits"], "commit")), []) for a in report["ai_coauthors"]]
    unmapped = [("%s  %s" % (u["name"], plural(u["commits"], "commit")), [])
                for u in report["unmapped_authors"]]
    merged = [("#%d  %s  by %s  (merge commit %s)" % (p["number"], p["title"], at(p["author"]),
                                                       p["merge_commit"]), [])
              for p in report["prs_merged"]]
    notmerged = [("#%d  [%s]  %s  by %s" % (p["number"], p["state"], p["title"], at(p["author"])), [])
                 for p in report["prs_not_merged"]]
    issues = []
    for it in report["issues"]:
        head = "#%d  [%s]%s  %s" % (it["number"], it["state"], " (PR)" if it["kind"] == "pr" else "",
                                    it["title"])
        subs = ["%s: %s" % ("PR author" if it["kind"] == "pr" else "reporter", at(it["reporter"]))]
        found = ", ".join(it["found_by"])
        if "commit" not in it["found_by"]:
            found += " (in window, no commit cites it)" if "window" in it["found_by"] else " (no commit cites it)"
        subs.append("found by: " + found)
        if it["commenters"]:
            subs.append("commenters: " + ", ".join(
                "%s (%s)" % (at(c["login"]), comment_note(c)) for c in it["commenters"]))
        subs.append("commits: " + (", ".join(it["commits"]) if it["commits"] else "none"))
        issues.append((head, subs))
    docs = [("%s:%d  %s" % (d["file"], d["line"], d["text"]), []) for d in report["docs_reporter_mentions"]]
    return [
        ("Code authors (%s, merges excluded)" % plural(report["commit_count"], "commit"), code),
        ("AI co-authors, not credited as people", ai),
        ("unmapped authors (needs a handle)", unmapped),
        ("PRs merged in range", merged),
        ("PRs not merged (credit needs a decision)", notmerged),
        ("Issues (cited by commits or docs, or active in the window)", issues),
        ("docs mention reporters with no handle (needs a name or consent)", docs),
    ]


def render_text(report):
    lines = ["release credits: %s..%s  (%s)" % (report["from"], report["to"], report["repo"]),
             "maintainer: %s   from %s (%s)   to %s" % (at(report["maintainer"]), report["from_commit"],
                                                      report["from_date"][:10], report["to_commit"]),
             ""]
    for title, items in sections(report):
        lines.append(title)
        if not items:
            lines.append("  (none)")
        for head, subs in items:
            lines.append("  " + head)
            lines.extend("      " + s for s in subs)
        lines.append("")
    for w in report["warnings"]:
        lines.append("warning: " + w)
    return "\n".join(lines).rstrip("\n") + "\n"


def render_markdown(report):
    lines = ["## Release credits: %s..%s" % (report["from"], report["to"]), "",
             "Repository %s. Maintainer %s. From %s (%s) to %s." % (
                 report["repo"], at(report["maintainer"]), report["from_commit"],
                 report["from_date"][:10], report["to_commit"]), ""]
    for title, items in sections(report):
        lines += ["### " + title, ""]
        if not items:
            lines.append("- (none)")
        for head, subs in items:
            lines.append("- " + head)
            lines.extend("  - " + s for s in subs)
        lines.append("")
    for w in report["warnings"]:
        lines.append("> warning: " + w)
    return "\n".join(lines).rstrip("\n") + "\n"


def render_json(report):
    return json.dumps(report, indent=2, ensure_ascii=False) + "\n"


def render_thanks(report):
    lines = ["## Thanks"]
    for t in report["thanks"]:
        lines.append("- %s: TODO describe (sources: %s)" % (at(t["login"]), ", ".join(t["sources"])))
    if report["ai_disclosure_needed"]:
        lines.append("- TODO: AI assistance disclosure (maintainer decides)")
    return "\n".join(lines) + "\n"


def needs_decision(report):
    return bool(report["unmapped_authors"] or report["docs_reporter_mentions"])


def decision_notes(report):
    """The lines --thanks sends to stderr."""
    notes = []
    if report["unmapped_authors"]:
        notes.append("unmapped authors (needs a handle): " + "; ".join(
            "%s (%s)" % (u["name"], plural(u["commits"], "commit")) for u in report["unmapped_authors"]))
    docs = report["docs_reporter_mentions"]
    if docs:
        notes.append("docs mention reporters with no handle (needs a name or consent): %s"
                     % plural(len(docs), "line"))
        notes.extend("  %s:%d  %s" % (d["file"], d["line"], d["text"]) for d in docs[:10])
        if len(docs) > 10:
            notes.append("  ... %d more; run without --thanks for the full list" % (len(docs) - 10))
    if report["prs_not_merged"]:
        notes.append("PRs not merged (credit needs a decision): " + "; ".join(
            "#%d %s [%s]" % (p["number"], at(p["author"]), p["state"]) for p in report["prs_not_merged"]))
    notes.extend("warning: " + w for w in report["warnings"])
    return notes


def emit(text, out, dry_run, stdout):
    text = text.replace("\r\n", "\n")
    if not out:
        stdout.write(text)
        return
    data = text.encode("utf-8")
    if dry_run:
        stdout.write("[dry-run] would write %s (%d bytes, UTF-8, no BOM, LF); nothing created\n"
                     % (out, len(data)))
        stdout.write(text)
        return
    parent = os.path.dirname(os.path.abspath(out))
    os.makedirs(parent, exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    stdout.write("wrote %s (%d bytes)\n" % (out, len(data)))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

class Parser(argparse.ArgumentParser):
    def error(self, message):
        # argparse exits 2 by default; here 2 means "needs a decision".
        self.print_usage(sys.stderr)
        sys.stderr.write("release_credits: error: %s\n" % message)
        raise SystemExit(1)


def build_parser():
    p = Parser(
        prog="release_credits.py",
        description="Build the contributor-credit list for a release, from sources.",
        epilog="Exit codes: 0 ok; 1 usage or tool failure (git or gh missing or failing, bad rev); "
               "2 when --thanks finds unmapped authors or docs mentions of reporters with no "
               "handle (the skeleton is still printed). No email address is ever printed.",
    )
    p.add_argument("--from", dest="from_rev",
                   help="previous release tag or rev (exclusive); its committer date starts the "
                        "issue and PR activity window")
    p.add_argument("--to", dest="to_rev", default="origin/main", help="end rev (default origin/main)")
    p.add_argument("--repo", help="owner/name (default: gh repo view)")
    p.add_argument("--maintainer", help="maintainer login (default: the repo owner)")
    p.add_argument("--include-maintainer", action="store_true",
                   help="keep the maintainer in the Thanks skeleton")
    p.add_argument("--format", choices=("text", "markdown", "json"), default="text")
    p.add_argument("--thanks", action="store_true",
                   help="print only the ## Thanks skeleton (json: the full report as usual)")
    p.add_argument("--out", help="write the report to FILE (UTF-8, no BOM, LF)")
    p.add_argument("--dry-run", action="store_true",
                   help="with --out: print what would be written; create no file and no directory")
    p.add_argument("--self-test", action="store_true", help="run the built-in tests and exit")
    return p


def main(argv=None, git=None, gh=None, stdout=None, stderr=None):
    stdout = stdout or sys.stdout
    stderr = stderr or sys.stderr
    args = build_parser().parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.from_rev:
        build_parser().error("--from is required")
    git = git or make_git_runner(ROOT)
    gh = gh or make_gh_runner(ROOT)
    try:
        repo = args.repo
        if not repo:
            rc, out, err = gh(["repo", "view", "--json", "nameWithOwner"])
            if rc != 0:
                raise ToolError("gh repo view failed (exit %d): %s" % (rc, first_line(err)))
            repo = str(parse_json(out, "gh repo view").get("nameWithOwner") or "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
            raise ToolError("bad repo %r, expected owner/name" % repo)
        maintainer = args.maintainer or repo.split("/")[0]
        report = build_report(git, gh, repo, args.from_rev, args.to_rev, maintainer,
                              args.include_maintainer)
        if args.format == "json":
            text = render_json(report)
        elif args.thanks:
            text = render_thanks(report)
        elif args.format == "markdown":
            text = render_markdown(report)
        else:
            text = render_text(report)
        text = scrub(text)  # belt and braces: no address leaves, whatever the source
        emit(text, args.out, args.dry_run, stdout)
    except ToolError as e:
        stderr.write("release_credits: error: %s\n" % e)
        return 1
    if args.thanks:
        decisions = decision_notes(report)
        if decisions:
            stderr.write("release_credits: needs a decision before publishing:\n")
            stderr.write("\n".join("  " + n for n in decisions) + "\n")
        if needs_decision(report):
            return 2
    elif report["warnings"] and args.format == "json":
        stderr.write("\n".join("release_credits: warning: " + w for w in report["warnings"]) + "\n")
    return 0


# ---------------------------------------------------------------------------
# Self-test: a temporary git repo and a fake gh. No network, no real gh.
# ---------------------------------------------------------------------------

def _make_repo(tmp):
    """Build the fixture repo. Returns {label: sha}."""
    env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
               GIT_TERMINAL_PROMPT="0")

    def run(args, extra=None):
        e = dict(env, **(extra or {}))
        p = subprocess.run(["git"] + args, cwd=tmp, env=e, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=False)
        if p.returncode != 0:
            raise AssertionError("fixture git %s failed: %s" % (args, p.stderr.decode("utf-8", "replace")))
        return p.stdout.decode("utf-8", "replace").strip()

    shas = {}
    seq = [0]

    def commit(label, name, email, message, files=None):
        seq[0] += 1
        for rel, body in (files or {}).items():
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(body.encode("utf-8"))
        run(["add", "-A"])
        mf = os.path.join(tmp, ".git", "msg%d" % seq[0])
        with open(mf, "wb") as f:
            f.write(message.encode("utf-8"))
        run(["commit", "-q", "--allow-empty", "-F", mf],
            {"GIT_AUTHOR_NAME": name, "GIT_AUTHOR_EMAIL": email,
             "GIT_COMMITTER_NAME": "Committer Z", "GIT_COMMITTER_EMAIL": "committer@example.org"})
        shas[label] = run(["rev-parse", "HEAD"])

    run(["init", "-q"])
    commit("base", "Maint Dev", "maint@example.org", "base\n")
    run(["tag", "v0.1.0"])
    commit("c1", "Alice A", "111+alice-gh@users.noreply.github.com",
           "Add feature\n\nFixes #10\n\nCo-authored-by: Eve E <222+eve-gh@users.noreply.github.com>\n")
    commit("c2", "Bob B", "bob@example.org", "Tweak the thing\n")
    commit("c3", "Carol C", "carol@example.org", "Squash merge (#7)\n")
    commit("c4", "Dave D", "dave@example.org",
           "Docs and notes, see #181\n\nCo-authored-by: Claude <noreply@anthropic.com>\n",
           {"docs/notes.md": "# Notes\n\nuser 5 sent logs\nwrote to dave@example.org about a tester\nplain line\n"
                             "see issue 14 and #15 for details\n"})
    commit("c5", "Claude", "claude-bot@example.org", "AI authored commit\n")
    commit("c6", "Maint Dev", "maint@example.org", "Fix issue 10 and #11 and #16\n")
    commit("c7", "dependabot[bot]", "49699333+dependabot[bot]@users.noreply.github.com", "Bump a dep\n")
    commit("c8", "Frank F", "333+frank-gh@users.noreply.github.com", "Frank's change\n")
    return shas


def _make_fake_gh(shas, calls):
    api_logins = {shas["c2"]: "bob-gh", shas["c6"]: "maint-gh", shas["c3"]: "null",
                  shas["c4"]: "null"}
    far = "2099-01-01T00:00:00Z"
    prs = [
        {"number": 7, "title": "Carol's squash", "author": {"login": "carol-gh", "is_bot": False},
         "state": "MERGED", "mergedAt": far, "mergeCommit": {"oid": shas["c3"]}, "url": "u7",
         "updatedAt": far},
        {"number": 8, "title": "Frank's open work", "author": {"login": "frank-gh", "is_bot": False},
         "state": "OPEN", "mergedAt": None, "mergeCommit": None, "url": "u8", "updatedAt": far},
        {"number": 9, "title": "Old merged PR", "author": {"login": "grace-gh", "is_bot": False},
         "state": "MERGED", "mergedAt": far, "mergeCommit": {"oid": shas["base"]}, "url": "u9",
         "updatedAt": far},
        {"number": 20, "title": "Stale closed PR", "author": {"login": "oldtimer", "is_bot": False},
         "state": "CLOSED", "mergedAt": None, "mergeCommit": None, "url": "u20",
         "updatedAt": "2000-01-01T00:00:00Z"},
        {"number": 21, "title": "Maintainer draft", "author": {"login": "maint-gh", "is_bot": False},
         "state": "OPEN", "mergedAt": None, "mergeCommit": None, "url": "u21", "updatedAt": far},
    ]

    old = "2000-01-01T00:00:00Z"

    def comment(login, bot=False, when=far):
        return {"author": {"login": login, "is_bot": bot}, "createdAt": when}

    issues = {
        # cited by commits; one commenter whose only comment predates the window
        10: {"number": 10, "title": "Crash on launch", "state": "OPEN", "createdAt": far,
             "closedAt": None, "author": {"login": "ivan-gh"}, "url": "https://x/issues/10",
             "comments": [comment("ivan-gh"), comment("ivan-gh"), comment("maint-gh"),
                          comment("maint-gh"), comment("maint-gh"), comment("judy-gh"),
                          comment("judy-gh"), comment("github-actions[bot]", True),
                          comment("old-gh", when=old)]},
        # cited by a commit, nothing happened to it since the window start
        11: {"number": 11, "title": "Second report", "state": "CLOSED", "createdAt": old,
             "closedAt": old, "author": {"login": "judy-gh"}, "url": "https://x/issues/11",
             "comments": []},
        # in the window only: no commit and no doc cites it
        12: {"number": 12, "title": "Window only", "state": "OPEN", "createdAt": far,
             "closedAt": None, "author": {"login": "kate-gh"}, "url": "https://x/issues/12",
             "comments": [comment("liam-gh"), comment("mia-gh", when=old)]},
        # re-labelled in the window, but no real activity
        13: {"number": 13, "title": "Only re-labelled", "state": "OPEN", "createdAt": old,
             "closedAt": None, "author": {"login": "nina-gh"}, "url": "https://x/issues/13",
             "comments": [comment("nina-gh", when=old)]},
        # cited by a docs line only; the reporter wrote it before the window, a comment is in it
        14: {"number": 14, "title": "Docs only", "state": "OPEN", "createdAt": old,
             "closedAt": None, "author": {"login": "oscar-gh"}, "url": "https://x/issues/14",
             "comments": [comment("pia-gh")]},
        7: {"number": 7, "title": "Carol's squash", "state": "MERGED", "createdAt": far,
            "closedAt": far, "author": {"login": "carol-gh"}, "url": "https://x/pull/7",
            "comments": [comment("heidi-gh")]},
    }
    issue_list = [{"number": n, "title": issues[n]["title"], "author": issues[n]["author"],
                   "state": issues[n]["state"], "updatedAt": far} for n in (10, 11, 12, 13, 14)]
    newest = 30  # numbers above this cannot exist, so #181 is never looked up

    def gh(args):
        calls.append(list(args))
        if args[:2] == ["repo", "view"]:
            return 0, json.dumps({"nameWithOwner": "maint-gh/proj"}), ""
        if args[0] == "api" and args[1].endswith("/issues?state=all&per_page=1"):
            return 0, "%d\n" % newest, ""
        if args[0] == "api":
            sha = args[1].rsplit("/", 1)[1]
            return 0, api_logins.get(sha, "null") + "\n", ""
        if args[:2] == ["issue", "list"]:
            search = args[args.index("--search") + 1]
            if not re.fullmatch(r"updated:>=\d{4}-\d\d-\d\d", search):
                return 1, "", "bad search %r" % search
            return 0, json.dumps(issue_list), ""
        if args[:2] == ["pr", "list"]:
            return 0, json.dumps(prs), ""
        if args[:2] == ["issue", "view"]:
            n = int(args[2])
            if n in issues:
                return 0, json.dumps(issues[n]), ""
            return 1, "", "GraphQL: Could not resolve to an issue or pull request with the number of %d." % n
        return 1, "", "unexpected gh call %r" % (args,)
    return gh


def self_test():
    def check(cond, what):
        if not cond:
            print("release_credits: self-test FAILED: " + what)
            sys.exit(1)

    # -- pure helpers ---------------------------------------------------
    check(noreply_login("123+login@users.noreply.github.com") == "login", "noreply with id")
    check(noreply_login("login@users.noreply.github.com") == "login", "noreply without id")
    check(noreply_login("login@example.org") is None, "ordinary email is not noreply")
    check(classify_identity("Claude", "x@example.org") == "ai", "name Claude is AI")
    check(classify_identity("Claude Opus 5.5", "noreply@anthropic.com") == "ai", "anthropic email is AI")
    check(classify_identity("Claudette Smith", "c@example.org") == "human", "Claudette is a person")
    check(classify_identity("x[bot]", "a@example.org") == "bot", "[bot] name")
    check(classify_identity("GitHub", "noreply@github.com") == "bot", "noreply@github.com")
    check(issue_refs("Fix Issue #45: x, see #12 and issue 63; page#12abc #1f") == {45, 12, 63},
          "issue ref extraction: %r" % issue_refs("Fix Issue #45: x, see #12 and issue 63; #1f"))
    check(scrub("mail a.b+c@host.example.org now") == "mail <email removed> now", "scrub")
    check(len(excerpt("x" * 500, 300)) <= EXCERPT_MAX, "excerpt length")
    check(valid_login("null") is None and valid_login("") is None and valid_login("a-b") == "a-b",
          "login validation")
    hits = scan_docs_diff(
        "diff --git a/docs/a.md b/docs/a.md\nindex 1..2 100644\n--- a/docs/a.md\n+++ b/docs/a.md\n"
        "@@ -1,2 +10,3 @@\n+plain\n+the Supporter said so\n-removed tester\n+User 7 again\n"
        "diff --git a/docs/b.md b/docs/b.md\n--- /dev/null\n+++ b/docs/b.md\n@@ -0,0 +1 @@\n+a tester\n")
    check([(h["file"], h["line"]) for h in hits] == [("docs/a.md", 11), ("docs/a.md", 12),
                                                    ("docs/b.md", 1)], "docs diff line numbers: %r" % hits)
    check(docs_issue_numbers(
        "diff --git a/docs/a.md b/docs/a.md\n--- a/docs/a.md\n+++ b/docs/a.md\n@@ -1 +1,3 @@\n"
        "+see Issue 14, PR #15 and issues #16; page#17abc issue65 #1f\n-old issue 99\n+draw #181\n"
    ) == {14, 15, 16, 181}, "docs issue number extraction")
    start = parse_iso("2026-10-01T21:27:14+00:00")
    check(stamp_since("2026-10-01T21:27:14Z", start) and not stamp_since("2026-10-01T21:27:13Z", start),
          "a stamp at the window start counts, one second before does not")
    check(not stamp_since(None, start) and not stamp_since("2099-01-01T00:00:00Z", None),
          "no stamp or no window is never activity")
    check(comment_in_window(None, start) and not comment_in_window("2026-10-01T10:29:41Z", start),
          "a comment before the window does not count; one with no time cannot be shown to predate it")

    with tempfile.TemporaryDirectory() as tmp:
        repo_dir = os.path.join(tmp, "repo")
        os.makedirs(repo_dir)
        shas = _make_repo(repo_dir)
        git = make_git_runner(repo_dir)

        def run(argv, calls=None, gh=None):
            calls = calls if calls is not None else []
            out, err = io.StringIO(), io.StringIO()
            rc = main(argv, git=git, gh=gh or _make_fake_gh(shas, calls), stdout=out, stderr=err)
            return rc, out.getvalue(), err.getvalue()

        # -- the whole range, as data (json) -----------------------------
        calls = []
        rc, js, err = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "json"], calls)
        check(rc == 0, "json run exit 0, got %d: %s" % (rc, err))
        rep = json.loads(js)
        check(json.loads(json.dumps(rep)) == rep and rep["schema"] == SCHEMA, "json round trip")
        check(render_json(rep) == js, "json is stable")
        check(rep["repo"] == "maint-gh/proj" and rep["maintainer"] == "maint-gh",
              "repo and maintainer default from gh repo view")
        code = {c["login"]: c for c in rep["code"]}

        def ai_ok(r):
            return ([a["name"] for a in r["ai_coauthors"]] == ["Claude"]
                    and r["ai_coauthors"][0]["commits"] == 2
                    and all("claude" not in t["login"].lower() for t in r["thanks"])
                    and all("claude" not in c["login"].lower() for c in r["code"]))

        def bot_ok(r):
            return not any("dependabot" in t["login"].lower() for t in r["thanks"])

        def api_calls_for(sha):
            return [c for c in calls if c[0] == "api" and c[1].endswith("/" + sha)]

        def window_ok(r):
            rows = {i["number"]: i for i in r["issues"]}
            return (not any(t["login"] in ("old-gh", "mia-gh", "nina-gh") for t in r["thanks"])
                    and 13 not in rows and rows[12]["found_by"] == ["window"])

        check("alice-gh" in code, "(a) a noreply email resolves to its login")
        check(not api_calls_for(shas["c1"]), "(a) the noreply email needed no API call")
        check("bob-gh" in code, "(b) an author resolved through the commit-author API")
        check("carol-gh" in code, "a null commit author resolved through the merged PR")
        check("eve-gh" in code, "a Co-authored-by noreply email resolves")
        check([u["name"] for u in rep["unmapped_authors"]] == ["Dave D"]
              and rep["unmapped_authors"][0]["commits"] == 1, "(c) unresolved author is unmapped by name")
        check(not any("dave" in t["login"].lower() for t in rep["thanks"]), "(c) never in Thanks")
        check(ai_ok(rep), "(d) AI co-author and AI author go to the AI list and are never credited")
        check(bot_ok(rep), "a bot is skipped")
        check([b["name"] for b in rep["bots_skipped"]] == ["dependabot[bot]"], "bot listed as skipped")
        check(not api_calls_for(shas["c5"]), "AI identity never mapped")
        check(len(api_calls_for(shas["c2"])) == 1, "one API lookup per unresolved author")
        check([p["number"] for p in rep["prs_merged"]] == [7] and rep["prs_merged"][0]["author"] == "carol-gh",
              "(e)(f) merged PR in range listed, merge commit outside the range not")
        check([(p["number"], p["state"]) for p in rep["prs_not_merged"]] == [(8, "OPEN")],
              "(e) open non-maintainer PR listed; stale, maintainer and merged ones not")
        rows = {i["number"]: i for i in rep["issues"]}
        check(sorted(rows) == [7, 10, 11, 12, 14],
              "(g) nonexistent #15 #16 and #181 dropped silently, re-labelled #13 not counted: %r" % sorted(rows))
        viewed = [c[2] for c in calls if c[:2] == ["issue", "view"]]
        check("15" in viewed and "16" in viewed, "nonexistent numbers below the newest are looked up and dropped")
        check("181" not in viewed, "a number above the repository's newest is dropped without a lookup")
        check(len(viewed) == len(set(viewed)), "each number is looked up once")
        i10 = rows[10]
        check(i10["reporter"] == "ivan-gh" and i10["state"] == "OPEN", "(g) reporter and state kept")
        check(i10["commenters"] == [{"login": "ivan-gh", "comments": 2, "in_window": 2},
                                    {"login": "judy-gh", "comments": 2, "in_window": 2},
                                    {"login": "old-gh", "comments": 1, "in_window": 0}],
              "(g) commenters with counts, maintainer and bots excluded, pre-window flagged: %r"
              % i10["commenters"])
        check(i10["commits"] == [shas["c1"][:8], shas["c6"][:8]], "issue lists the commits citing it")
        check(i10["found_by"] == ["commit", "window"] and rows[11]["found_by"] == ["commit"]
              and rows[7]["found_by"] == ["commit", "window"], "commit-cited rows are tagged commit")
        check(rows[11]["commits"] == [shas["c6"][:8]], "a commit-cited issue with no activity lists its commit")
        check(rows[12]["found_by"] == ["window"] and rows[12]["commits"] == [] and rows[12]["reporter"] == "kate-gh"
              and rows[12]["commenters"] == [{"login": "liam-gh", "comments": 1, "in_window": 1},
                                             {"login": "mia-gh", "comments": 1, "in_window": 0}],
              "an issue only in the activity window is listed with its reporter and commenters: %r" % rows[12])
        check(rows[14]["found_by"] == ["docs", "window"] and rows[14]["reporter"] == "oscar-gh",
              "a docs-only citation is listed and tagged: %r" % rows[14])
        check(window_ok(rep), "re-labelled issue and pre-window commenters are not credited")
        check([(d["file"], d["line"]) for d in rep["docs_reporter_mentions"]] == [("docs/notes.md", 3), ("docs/notes.md", 4)],
              "(h) docs lines mentioning reporters listed: %r" % rep["docs_reporter_mentions"])
        check(all("@" not in d["text"] for d in rep["docs_reporter_mentions"]), "docs excerpt scrubbed")

        # -- Thanks --------------------------------------------------------
        rc, th, err = run(["--from", "v0.1.0", "--to", "HEAD", "--thanks"])
        check(rc == 2, "(m) exit 2 when --thanks finds unmapped authors, got %d" % rc)
        lines = th.splitlines()
        check(lines[0] == "## Thanks", "skeleton starts with the heading")
        who = [l.split(":")[0][3:] for l in lines[1:] if l.startswith("- @")]
        check(who == ["carol-gh", "alice-gh", "bob-gh", "eve-gh", "frank-gh", "ivan-gh", "judy-gh",
                      "kate-gh", "oscar-gh", "heidi-gh", "liam-gh", "pia-gh"],
              "(k) order: merged-PR authors, code authors, reporters, commenters: %r" % who)
        check(not any(x in who for x in ("old-gh", "mia-gh", "nina-gh")),
              "a commenter whose comments predate the window and a re-labelled issue's people are not credited")
        for name, expect in (("kate-gh", "issue #12"), ("liam-gh", "issue #12"), ("oscar-gh", "issue #14"),
                             ("pia-gh", "issue #14")):
            line = [l for l in lines if l.startswith("- @" + name + ":")][0]
            check(expect in line, "window and docs people reach the skeleton: " + line)
        check(len(who) == len(set(who)) and "maint-gh" not in who, "(k) each person once, maintainer excluded")
        check(all("TODO" in l for l in lines[1:]), "(k) every skeleton line carries TODO")
        check(lines[-1] == "- TODO: AI assistance disclosure (maintainer decides)", "(k) AI disclosure line")
        carol = [l for l in lines if l.startswith("- @carol-gh")][0]
        check(carol == "- @carol-gh: TODO describe (sources: PR #7, 1 commit)", "carol line: " + carol)
        judy = [l for l in lines if l.startswith("- @judy-gh")][0]
        check(judy == "- @judy-gh: TODO describe (sources: issue #10, issue #11)", "(k) merged sources: " + judy)
        heidi = [l for l in lines if l.startswith("- @heidi-gh")][0]
        check("PR #7" in heidi, "a PR commenter is credited as a commenter: " + heidi)
        check("Dave D" in err and "docs mention reporters" in err, "decision notes go to stderr")
        check("Dave" not in th and "Claude" not in th, "(c)(d) no unmapped or AI name in the skeleton")
        rc, th_m, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--thanks", "--include-maintainer"])
        check("- @maint-gh:" in th_m, "--include-maintainer keeps the maintainer")

        # -- a range with nothing to decide and no AI ---------------------
        rc, th2, err2 = run(["--from", shas["c5"], "--to", "HEAD", "--thanks"])
        check(rc == 0 and "unmapped" not in err2 and "docs mention" not in err2,
              "(m) exit 0 when nothing needs a decision: %d %r" % (rc, err2))
        check("AI assistance" not in th2, "(k) no AI line when no AI was found")
        check([l.split(":")[0] for l in th2.splitlines()[1:]] == [
            "- @frank-gh", "- @ivan-gh", "- @judy-gh", "- @kate-gh", "- @oscar-gh", "- @heidi-gh",
            "- @liam-gh", "- @pia-gh"], "later range: %r" % th2)

        # -- never an email ------------------------------------------------
        for fmt in ("text", "markdown", "json"):
            rc, body, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", fmt])
            check(rc == 0, "(i) %s run exit 0" % fmt)
            check(not leaks_email(body), "(i) %s output carries no email address" % fmt)
        rc, body, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "markdown"])
        for needle in ("Unmapped", "unmapped authors (needs a handle)", "AI co-authors, not credited as people",
                       "PRs not merged (credit needs a decision)",
                       "docs mention reporters with no handle (needs a name or consent)",
                       "found by: window (in window, no commit cites it)", "found by: docs, window (in window, no commit cites it)",
                       "found by: commit, window", "@old-gh (1, none in window)", "commits: none"):
            check(needle.lower() in body.lower(), "present in the report: " + needle)

        # -- --out ----------------------------------------------------------
        out_file = os.path.join(tmp, "newdir", "sub", "credits.md")
        rc, dry, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "markdown", "--out", out_file,
                          "--dry-run"])
        check(rc == 0 and "[dry-run]" in dry and "## Release credits" in dry, "(l) dry run prints the report")
        check(not os.path.exists(out_file) and not os.path.exists(os.path.dirname(out_file))
              and not os.path.exists(os.path.join(tmp, "newdir")),
              "(l) --dry-run creates no file and no directory")
        rc, _, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "markdown", "--out", out_file])
        check(rc == 0 and os.path.isfile(out_file), "(l) --out writes the file")
        with open(out_file, "rb") as f:
            raw = f.read()
        check(not raw.startswith(b"\xef\xbb\xbf") and b"\r" not in raw, "(l) no BOM, LF endings")
        check(raw.decode("utf-8").startswith("## Release credits"), "(l) file holds the report")

        # -- failures --------------------------------------------------------
        rc, _, err = run(["--from", "no-such-tag", "--to", "HEAD"])
        check(rc == 1 and "bad rev" in err, "bad rev exits 1")
        try:
            resolve_rev(git, "--output=x")
            check(False, "an option-looking rev is refused")
        except ToolError:
            pass
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                rc, _, _ = run(["--to", "HEAD"])
        except SystemExit as e:
            rc = e.code
        check(rc == 1, "missing --from exits 1, got %r" % (rc,))
        fake = _make_fake_gh(shas, [])

        def broken(args):
            return (1, "", "HTTP 401: auth required") if args[:2] == ["pr", "list"] else fake(args)

        rc, _, err = run(["--from", "v0.1.0", "--to", "HEAD"], gh=broken)
        check(rc == 1 and "gh pr list failed" in err, "a failing gh exits 1")

        # -- mutation checks: the assertions above have teeth ---------------
        # (A bot is filtered twice: by identity, and again by the login it
        # resolves to. The mutation breaks both layers.)
        saved = {k: globals()[k] for k in ("is_ai_identity", "scrub", "is_bot_identity", "is_bot_login",
                                           "comment_in_window")}
        try:
            globals()["comment_in_window"] = lambda stamp, start: True
            _, js_m, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "json"])
            check(not window_ok(json.loads(js_m)),
                  "mutation: a window that admits every comment must fail the window assertion")
            globals()["comment_in_window"] = saved["comment_in_window"]
            globals()["is_ai_identity"] = lambda name, email: False
            _, js_m, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "json"])
            check(not ai_ok(json.loads(js_m)), "mutation: a broken AI classifier must fail the AI assertion")
            globals()["is_ai_identity"] = saved["is_ai_identity"]
            globals()["is_bot_identity"] = lambda name, email: False
            globals()["is_bot_login"] = lambda login, flagged=False: False
            _, js_m, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "json"])
            check(not bot_ok(json.loads(js_m)), "mutation: a broken bot filter must fail the bot assertion")
            globals()["is_bot_identity"] = saved["is_bot_identity"]
            globals()["is_bot_login"] = saved["is_bot_login"]
            globals()["scrub"] = lambda text: text or ""
            _, body_m, _ = run(["--from", "v0.1.0", "--to", "HEAD"])
            check(leaks_email(body_m), "mutation: a disabled scrub must trip the email-leak check")
        finally:
            globals().update(saved)

        # the mutations were undone
        rc, js_again, _ = run(["--from", "v0.1.0", "--to", "HEAD", "--format", "json"])
        check(js_again == js, "mutations restored")

    print("release_credits: self-test passed")
    return 0


def leaks_email(text):
    """True when the text carries an email address or one of the fixture's."""
    if EMAIL_RE.search(text):
        return True
    return any(s in text for s in ("example.org", "noreply@", "users.noreply"))


if __name__ == "__main__":
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
