---
name: edvr-sync
description: RETIRED 2026-08-16. This repo is now the only EDVR code repo -- there is no private repo to sync with, no generated files and no manifest. Read this only if you see a reference to sync_common.py, a GENERATED banner, or "the private repo" and need to know it is stale.
---

# This repo is the whole thing

Until 2026-08-16, EDVR's shared code was generated here from a private repo by
`tools/sync_common.py`, and 31 files opened with a banner saying *do not edit
here*. **All of that is gone.** Those banners were stripped, the manifest is
frozen in the private repo's `archive/`, and this repo is canonical for every
line of code it contains.

Edit here. Build here (`build.bat` runs every test). Release from here.

## The one thing that still lives elsewhere

The **evidence ledger** — `docs/EVIDENCE.md` in
`C:\Users\seanm\projects\edvr` — plus the spec, testing and risk docs. That
repo is now documentation only, and it is where measurement findings get
written and committed. It keeps history and an off-machine backup, which a
gitignored folder here would not.

Read the ledger before making claims about the game's internals. It is the
memory this code assumes, and it records refuted theories along with why they
were refuted, which is often the more useful half.

## If you meet a stale reference

Comments mentioning FORKED files, SHARED files, or what `sync_common` could or
could not see are historical. The failures they describe were real — see the
retired skill in the docs repo for the two that cost the most — but the
mechanism is gone. Fix the comment when you touch the code around it; do not
resurrect the workflow.
