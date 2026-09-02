# The eye split: both eyes, every stage, one frame

An instrument, not a fix. It exists because a planetary body renders as a
featureless black disc in the right eye and correctly in the left, and eight
rounds of the draw census could not find a difference between the two eyes
in anything the game *asks for*.

---

## What the census could not answer

Field-reported 2026-08-29, reproduced by one commander on a Quest 3 through
Virtual Desktop, on stock Elite — the fault predates EDVR. The body is black
in the right eye at any position in view, on any body, in the scanner and
out of it. Its silhouette is correct and hides the stars behind it. The blue
mapped-surface markers and the interface render correctly in both eyes.

What the census established, over eight rounds:

- Both eyes receive the **same draws**, in the same order, with the same
  textures, and with each eye's own copies of the per-eye resources
  correctly swapped in.
- The scanner's own layer is built **once** into a shared 16:9 target and
  copied to both eyes, so it cannot differ — which is why the markers look
  right.
- Six draws of `72BDD292154158AD` paint the body's surface into the
  geometry buffer **for each eye**, counted per eye, every frame, in both
  views.
- Live skip probes confirmed the identification: removing that draw turns
  the body black **in the good eye**, reproducing the symptom exactly.
  Removing `ACE405F428C17EF6` removes the body altogether, silhouette and
  all.
- The per-eye CS `b1` asymmetry in the clustered-lighting compute — bound
  for one eye, empty for the other — was equalised in **both directions on
  all five shaders that carry it**, and changed nothing.

So the draw that paints the body runs six times for each eye, with the same
inputs, and one eye's pixels come out black. The divergence is in what the
draws *produce*, and no amount of reading bindings will reach it.

That is the same wall the FSS black-square hunt hit at round seventeen, and
it was broken the same way: stop reading what the game asks for and look at
the pixels.

## Why not fss_eye_dump

`fss_eye_dump` already answers this question, but only inside the Full
System Scanner. It recognises the ring quad and the body composite by
vertex-shader hash and counts "body frames" from them. A world body goes
through neither, so it never arms.

This is the same idea with the scanner knowledge removed and replaced with
no knowledge at all — which matters, because every guess this hunt made
about which draw was the body was wrong until a live probe settled it.

## What it does

`advanced.eye_split = N` arms it. On the Nth **scene** frame after arming
(frames that draw nothing into an eye target do not count, so a menu cannot
spend the arming), it copies every render target the scene drew into and
writes each one raw to `edvr_logs\dumps\`.

A deferred renderer hands over its stages for free that way. The measured
field frame carries **sixteen** eye-sized targets, not the six the headline
stages suggest — a geometry buffer, two depth-ish planes, an HDR image and a
long post-process chain, each twice.

At full resolution that is 322 MB, which is a file nobody wants to send and
322 MB of staging to hold besides. So each target is written at **every
fourth texel of every fourth row**: a 2324x2392 eye becomes 581x598, still
hundreds of pixels across a planet's disc, and the whole frame comes to
about 20 MB. Decimating rather than averaging is deliberate — it needs no
knowledge of the pixel format, so the C++ side stays dumb about the eleven
formats the frame contains and the offline tool's decoders keep working.

One visible hitch, one dump per arming.

Files are named `eyesplit_<w>x<h>f<fmt>_eye<N>.bin`. **Two files of one
shape are the two eyes of one stage**, and `eye0` is the eye rendered first.
Each carries an `EYESPLIT` manifest line in the gfx log with the stage
ordinal, the draw count into that target, the row pitch and the resource
pointer.

### Two decisions worth knowing

**The copies happen at the frame boundary, not as each pass ends.** The two
eyes' draws interleave — measured, q ordinals 109..151 against 119..181 for
one HDR pair — so capturing on render-target change would fire dozens of
times and copy a gigabyte. Every one of these targets keeps its final
contents until the next frame overwrites it, so one copy each at the end is
the same picture for a tenth of the cost. A target the game genuinely
recycles mid-frame would be caught in its later state; the manifest's
resource pointer is what would show that up.

**Texel size is a table, not a guess.** `fss_eye_dump` answers "eight bytes
for `R16G16B16A16`, four for everything else", which is true of the four
targets it was pointed at and false here. A measured frame already contains
a 2324x2392 target at format 60 — `R8_TYPELESS`, one byte a texel. Written
at four it would read three texels of the next row into every pixel and
produce a file that looks like noise with nothing to say it had gone wrong.
Unknown formats fall back to four **and say so in the log**.

## Reading the result

    python tools/diff_eye_split.py edvr_logs/dumps edvr_logs/edvr_gfx_*.log

The log argument is optional and gives the stages in the order the frame
drew them, which is what makes "the first stage that disagrees" meaningful.

**The eyes are registered before anything is compared.** The two eye
projections are off-centre by different amounts, so content at infinity does
not land on the same pixel in both — about 365 px apart at a 2517 px eye
width, which is 91 px in these 4x-decimated files. Compared tile for tile at
identical positions, a planet in one eye lands on empty sky in the other and
the tool calls that a difference. It did exactly that on 2026-09-01: it
reported a "7% blue overlay in the healthy eye" of a DSS scan and a fix was
built on the number. The tiles had landed on the Milky Way band. There was no
overlay.

So the tool first measures how far one eye's content sits from the other's
and moves eye1 by that vector, printing it in the header and again at the
end. The measurement comes from the R32 linear-depth target (format 39,
metres, sky written as 1e17): the finite-depth region of a far body is the
same shape in both eyes even when one eye's colour is black, so the offset
between the two centroids is the vector, checked by how well the two masks
then overlap. With no depth target it cross-correlates luminance profiles
instead, which is weaker because the bug being hunted takes brightness out
of one eye. `--no-register` restores the old pixel-for-pixel comparison and
is worth having only to reproduce an old report.

Registration also needs the two files to *be* two eyes, and where a shape
carries more than two targets that is a guess — the pairing goes by order of
first sighting, which cannot tell a post chain's several buffers from the
second eye's copy. Each stage is therefore measured both ways, and one that
agrees better unmoved is reported unmoved and labelled **NOT A STEREO
PAIR**.

The two eyes are *supposed* to differ — they see the world from 6 cm apart.
So the tool reports the **shape** of each difference rather than its
presence. Parallax moves content sideways and leaves the total brightness
alone; a body drawn for one eye and not the other removes brightness from a
region and puts none back. The `balance` column is that signed imbalance,
and a one-sided stage is the signature being hunted.

The answer the dump is for:

- **The geometry buffer already disagrees** — the surface pass produced
  nothing for that eye, and the fault is inside those six draws (their
  per-eye constants being the only channel left).
- **The geometry buffer agrees and the lit image disagrees** — the surface
  data is there and the lighting threw it away.

Those point at completely different fixes, which is the whole reason to
look.

## Cost when off

One bool read per eye draw, which is the same early-out every other
instrument in `vscreen.cpp` sits behind. Nothing is allocated, hooked or
copied until the key is set.
