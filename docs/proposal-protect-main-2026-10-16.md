# Proposal: protect `main` (2026-10-16)

## Status

- **State**: proposal from the PR #92 retro, for Sean to decide. Nothing is
  enabled.
- **Depends on**: #93. This PR extends the existing `release.yml` build to
  run on pull requests and pushes to `main`. It has to be green before it
  can be a required check.
- **If accepted**: enable the rule below, then update the AGENTS.md Git
  section, which still says "Solo project: branch, commit, merge to main,
  push. No pull requests."

## Why

The project now has two people and several agents committing.

- **Nothing runs on `main`.** `main` has no workflow at all. The temporary
  manual Windows build (c3c5b046, removed in 519ec0bf) never passed in its
  four runs. Its last failure, a gate that breaks on a Windows CRLF checkout
  (#94), was already on `main` and stayed there. PR #92 merged with no CI
  run.
- **Direct pushes race open PRs.** PR #92 needed two catch-up merges of
  `main`, and one had a conflict in `menu.cpp`. Both were against work
  pushed straight to `main` while the PR was under review.
- **A broken gate reached `main` unnoticed.** After the second merge,
  `sharp_weapons_test` failed on a Windows checkout with
  `core.autocrlf=true` (#94). Nothing caught it before it landed.

## The rule

GitHub branch protection, or a ruleset, on `main`:

1. Changes land through a pull request. No direct pushes.
2. The `Native Windows build` check (`.github/workflows/release.yml`, job
   `build`) must pass. Enable this once #93 is green.
3. No force-pushes, and the branch can't be deleted.
4. No required approvals to start with. Sean can merge his own PRs
   immediately; the rule enforces the gate, not a second reviewer.
5. Admins may bypass, for emergencies only.

## What it costs

- **Sean's workflow:** push a branch and run `gh pr create --fill`, then
  `gh pr merge --auto --merge`. The merge happens when CI passes. That's
  one extra command per change, and the wait is CI's run time.
- **Agents:** the AGENTS.md Git section changes from "merge to main, push"
  to "push the branch, open a PR, merge when green". The "task is not done
  until it is pushed" check becomes "until the PR is merged".
- **CI time:** every change waits for a full Windows build. If that's too
  slow, gate on the Python source gates first and keep the full build as a
  second, non-blocking job.

## Enabling it

Once #93 is green, in the repository's Settings → Rules → Rulesets: target
`main`, then turn on "Require a pull request before merging" (0 approvals),
"Require status checks to pass" (the Windows build job), "Block force
pushes" and "Restrict deletions".

Or the classic protection API:

```
gh api -X PUT repos/characterecho-sean/edvr-unofficial-patch/branches/main/protection \
  -F required_status_checks.strict=false \
  -f "required_status_checks.contexts[]=full-build" \
  -F enforce_admins=false \
  -F required_pull_request_reviews.required_approving_review_count=0 \
  -F restrictions=null -F allow_force_pushes=false -F allow_deletions=false
```

## Alternatives

- **CI required, PRs optional:** not possible. Required checks apply only to
  PR merges, so direct pushes would still skip them.
- **Keep pushing to `main` and just watch CI:** better than today, but a
  check nobody is required to pass gets ignored. The manual workflow's four
  red runs were never followed up on `main`.
