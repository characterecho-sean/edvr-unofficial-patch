# Build 334925 retarget (2026-10-23): what is here and what was bound

Mirrors `.agents/build-334473/`. The write-up for people is `docs/build-334925.md`.

## PE old -> new

334473 -> 334925: stamp 0x6ACD513D -> 0x6ADA8048 (1791840573 -> 1792704584); SizeOfImage 0x640C000 unchanged; 7 sections keep their virtual addresses; `.text` VS 0x4DB4AEE -> 0x4DB498E, raw 0x4DB4C00 -> 0x4DB4A00; `.rdata` VS -0x20; `.pdata` VS -0x6C; `.reloc` VS -0x18. SHA-256 6891f70a...857e93 -> ed5a8f8cd10f9a80d3bda5badd7ba1d4fcfc15996c8c7e018852cb170e264b63. Steam `VersionInfo.txt` 2026.10.23.334925.

## Files

| File | What |
|---|---|
| `map_all.json` / `map_all.txt` | main's 222 build-bound addresses, 334473 -> 334925, `build_diff.py map` output in the 334473 `map_all.json` format (`rva` = 334473, `new_rva` = 334925) |
| `map_extra.json` | 232 more mapped addresses (rva >= 0x100000): the mem_tick pins and globals, CPU profiler tables, rig layouts, comment citations. Compact: rva, kind, new_rva, status. Includes coincidental numerics; `retarget.py` applies it only inside `EXTRA_FILES`, minus `EXTRA_DENY` |
| `retarget.py` | the rewrite of `src/` and `tools/` (RVAs, identity pair, SHA, build number); `--dry-run`, `--apply` |
| `pins.py` | the byte and FNV pin checker, capstone-based; `--mode pre|post`, `--write-baseline`, `--save-pre`, `--apply-new` |
| `pins_baseline.json` | every pin as the source held it for 334473, before the edit pass |
| `pins_report.txt` | the pre run (110 KEEP, 32 NEW, 1 SITE-OK) and the post run (142 KEEP, 1 SITE-OK) |
| `verify_data.py` | vtable slots, name literals, fixed data, the `.text` floor |

The caches that make a re-run take 15 s are not tracked: `analysis/build_map_334473_to_334925.cache.json` and `analysis/build_map_332841_to_334925.cache.json` (main checkout).

## Counts (main's 222 addresses)

code 180: 1 same RVA, 179 moved (54 IDENTICAL, 126 RELOC-ONLY), 0 changed, 0 unmapped. data 42: 19 same RVA, 23 moved (35 XREF-EXACT, 7 XREF-BRACKET), 0 changed, 0 unmapped.

## Per feature (all moved; none refuses)

Oculus selector bypass (11 FNVs, call chain, vtables); terrain cull cover (head 0x44414E0, site 0x4458CBD, call 0x4458CE7); vertex resync (FlushIA 0x522B50); light grid cache (builder 0x58B250); Explorer Cam and follow (8 prologues, head-joint slots, part table, name literals); kinematic set, engine-record velocity, cull gate, scheduler stack, object record writer; skin join (0x4C56BA0); admission store and optimized station (0x4314C60); pad-state probe; UI panel sizing (sites 0x457319A, 0x4573CA5); VR Supersampling gate, hold and window trampolines; transition-flash eye base; flat camera injector (0x592300), producer probe, stationary capture; mem_tick (18 pins).

## Not addresses, but moved with the build

`.text` span floor in `elite_oculus_profile.h` (0x4DB4C00 -> 0x4DB4A00); `base + 0x43513A5` list-merge re-check; `kPartCallReturnRva` 0x42B77B6; the installer's qualified SHA (`src/installer/probe.cpp`); `proxy.cpp` verified build "334925"; `build_diff.py` TARGET_SEED and `build_diff_targets.json` (recaptured); `edvr.ini` supported-build comment; the Explorer Cam rig's alignment claim (32, not 64) and the UI quality rig's site-end VA (0x1445731AB).

## Deliberately left at 334473

Historical statements keep their build: the Galaxy Map icon change "since game build 334473", measurements on 334473 (UI depth A/B, orbit-line shader provenance), decompile provenance comments, and `docs/build-334473.md`.
