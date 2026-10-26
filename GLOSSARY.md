# EDVR

EDVR binds to code and data inside one specific build of Elite Dangerous:
Odyssey. These are the words for that binding and for carrying it across game
updates.

## Language

### Game builds

**Game build**:
One release of the game executable, named by the last field of its
`VersionInfo.txt` version (2026.10.13.334473 is build 334473).
_Avoid_: version, patch, game version (4.4.2.0 is the product version, which
several builds can share)

**Identity pair**:
The executable's PE timestamp and image size, which EDVR checks before
trusting any build-bound address.
_Avoid_: version check, build check, PE pair

**Archived executable**:
A copy of a game build's executable kept in the main checkout's `analysis\`,
so it can serve as the old build after Steam overwrites the installed one.
_Avoid_: backup (backups are the copies an install keeps of the files it
replaces)

### Binding

**Build-bound address**:
A game address EDVR hooks, reads or patches that is valid for one game build
only.
_Avoid_: offset, hardcoded address

**Struct offset**:
The position of a field inside a game object; it survives a game build unless
the object's layout changes.
_Avoid_: address, RVA

**Doc-only address**:
A game address cited in `docs\` that EDVR's source does not bind.
_Avoid_: unbound address

**Pin**:
The bytes or body hash EDVR expects at a build-bound address, checked before
it hooks.
_Avoid_: signature, fingerprint (the route profile's FNV fingerprints are one
kind of pin)

**Retarget**:
Moving every build-bound address, pin and the identity pair from one game
build to the next.
_Avoid_: remap, port, rebase

### Mapping one build onto the next

**Build map**:
The pairing of an old game build's code units and data with a new build's,
from which a retarget takes its new addresses.
_Avoid_: remap, diff (the diff is the baseline comparison, which cannot follow
moved code)

**Code unit**:
One piece of game code the build map pairs as a whole: a function, a function
fragment, or a stub between them.
_Avoid_: block, chunk

**IDENTICAL**:
A code unit whose bytes are the same in both builds, at a new address.
_Avoid_: exact, unchanged

**RELOC-ONLY**:
A code unit whose only differences are relocated displacements: the same code
pointing at moved targets.
_Avoid_: moved+changed, changed, patched

**CHANGED**:
A code unit whose body was edited between the builds, so placement inside it
is a lower-confidence estimate.
_Avoid_: moved, different

**AMBIGUOUS**:
A code unit with look-alike candidates in the new build, paired by order
rather than by content.
_Avoid_: uncertain, multiple matches

**NOT-FOUND**:
A code unit with no pairing in the new build: removed, rewritten, split or
merged.
_Avoid_: missing, gone

**XREF-EXACT**:
A data address whose new location is confirmed because code references both
the old and the new target.
_Avoid_: data match

**XREF-BRACKET**:
A data address placed by the shift that its nearest referenced neighbours
agree on, with its own bytes compared.
_Avoid_: estimated, interpolated

### Cameras

**Projection kind**:
The type of projection a game camera uses: 1 orthographic, 3 trigonometric,
4 and 5 a custom matrix. The headset eye cameras are kind 5, and the on-foot
world camera is kind 3.
_Avoid_: camera kind, projType, camera type
