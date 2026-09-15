# EFB geometry matrix

Run the hardware diagnostics in one sweep. Each case stops at its first pixel
mismatch; the suite records the failure and continues to the next case. A
transport error, incomplete capture, unexpected driver error, or failed cleanup
stops the suite so later cases do not run against uncertain state.

The default is **4 iterations per case across 17 cases** (at most 68 iterations).
One iteration checks 640×480 pixels; batched cases submit 30 draws per iteration.
The ceiling is 1,000 iterations **per case**, not per suite. A full geometry extended
sweep can therefore attempt 17,000 iterations; selecting the additional pipeline
cases increases that total. Failures are first-error censored;
do not interpret the summary as an estimate of a stable failure percentage.

From the productization repository, using a module built from the current source:

```sh
tools/wii-gcn-efb-matrix.py --allow-dirty \
  --host 10.3.10.59 \
  --module /media/anolis/dev/wii-gcn-linear-v12-build/drivers/video/fbdev/gcn-gx.ko \
  --output /media/anolis/dev/wii-gcn-matrix-my-run
```

The output directory must be new. Omit `--allow-dirty` to require a clean tree.
The suite snapshots the module, driver source, harness scripts, Git status and
tracked diff. It uses the same SSH identity and legacy Wii connection options
as the render-cycle runner; `WII_SSH_KEY` and `WII_SSH_HOST` are supported.

The default `--cases geometry` selects the 17 geometry cases. `--cases pipeline`
selects the additional depth-path, TEV color-source, and matched scissor experiments; `--cases all` selects both.
The printed plan always states the selected count and total iteration ceiling.

Select or extend cases without editing scripts:

```sh
tools/wii-gcn-efb-matrix.py --list
tools/wii-gcn-efb-matrix.py --dry-run --cases interior,interior-even --iterations 1000
tools/wii-gcn-efb-matrix.py --allow-dirty \
  --module /media/anolis/dev/wii-gcn-linear-v12-build/drivers/video/fbdev/gcn-gx.ko \
  --cases interior,interior-even --iterations 1000 \
  --output /media/anolis/dev/wii-gcn-matrix-interior-comparison
```

Cases cover clear-only, one and two full-screen quads, split rows, batched split
rows, full-width one-row strips, 16-row bands with repeated/degenerate padding,
four-row and two-row strips, interleaved padding, alternate drawing order,
odd boundaries, paired edge strips, and odd/even interior-only coverage.
Interior cases intentionally leave 38,400 black pixels per frame. Those black
pixels are checked along with the 268,800 colored pixels; these cases are not
full-screen rendering workarounds.

The host and Wii show compact progress. The evidence directory contains:

- `summary.md`, `summary.csv`, `summary.json`: results, completed versus requested
  iterations, mismatch counts, first bad pixel, and device-measured duration.
- One directory per case: exact command, terminal output, compressed and plain
  kernel logs, before/after state and audited result.
- `module.ko`, source snapshots, Git provenance and `SHA256SUMS`.

The auditor checks full iteration/background sequences, expected colors and
coverage markers, draw counts/geometry/byte counts, repeat-color command-hash
consistency, first-error stopping and console cleanup. It rejects missing or
inconsistent logs. Hash consistency is checked within each case; the suite does
not treat a hash from a different geometry as its reference or prove geometry
correct solely from hashes.

The suite verifies the complete downloaded kernel capture against its device
SHA-256 before removing its own random-named temporary log and PID file. If
capture or audit fails, it retains remote evidence and records its path in the
case result. Existing logs are not deleted. Insufficient `/tmp` space stops the
suite; archive or compress older evidence before retrying. The current console
logging settings, boot ID and absence of `gcn_gx` are checked before/after each
case. The installed provider is not replaced.

Only one suite may hold `/tmp/wii-gcn-efb-matrix.lock`. Do not run another manual
hardware test concurrently. The runner timeout for each case defaults to 1,200 wall seconds
and can be set with `--timeout`. Log retrieval has a separate 180-second timeout.
A timeout/interruption is an ERROR, and remote
state must be inspected before retrying. After an abrupt host crash, inspect the
Wii and saved evidence before removing a stale lock. No automatic recovery
reboots or termination of unrelated tests are attempted.

Exit status: **0** all PASS, **1** one or more verified pixel failures after a
completed sweep, **2** suite error. A pixel FAIL is expected in some diagnostic
controls and is still a valid result, not a reason to stop the sweep.

Audit regression tests use two small captures from actual hardware runs:

```sh
python3 tools/testing/wii-gcn-matrix/test_audit.py
```

Matched scissor cases: `scissor-two-rows` and `scissor-tall-rows` write the same
full surface through two-row scissors. They differ only in visible quad height
(two rows versus the containing 16-row batch); command lengths, pad quads,
primitive headers and scissor writes match. Use both orders in short trials
before extending a promising arm. Compare `two-rows` to detect effects shared
by the extra scissor writes and primitive headers.

`cpu-write` bypasses primitives and writes every pixel through the CPU color
aperture from a checked black background, then uses the same raw/copy oracles.
`snapshot-two-rows` checks the complete EFB after every batch, including past
and future rows. Its JSON `first_batch_record` locates the first observed error;
`batch_observations_bad` counts bad observations across batches, which may
include the same pixel repeatedly. Intermediate errors fail the case even if
subsequent draws repair them. Full scans change timing; compare the baseline.

Overlap controls use a 528-row viewport in both arms, while checking/copying the
visible480 rows. `extended-two-rows` / `extended-one-row` retain thin strips;
`overlap-two-rows` / `overlap-one-row` extend them to four rows and overwrite
intersections in increasing row order. `striped-` variants alternate red/white
between strips and flip phase each iteration. The stripe marker declares one
or two rows per stripe; the auditor rejects a missing or mismatched marker.
These diagnostics write below the visible test image. Native integration must
separately preserve content outside the active destination rectangle.

Native cases (`native-single`, `native-baseline`) interpret `--iterations` as
frames, with four quadrant operations per frame. They use `--native-client`
(defaulting to the archived offscreen client) and archive the exact binary.
The single case enables the diagnostic one-quad path; baseline uses thin runs.
Both retain crop/horizontal processing, preservation and state fences. Native
records are audited separately: three stages and three preservation snapshots
per rectangle, two command records, one state record, expected origins, final
quad/byte counts, stable command hashes, and client completion. A client pixel
failure stops at the frame boundary; an intermediate stage error also makes
the post-run audit FAIL even if the client completed. Native long runs should
use `--timeout 3600`. Each case still restores module and console state.

Precision: geometry/CPU EFB diagnostics compare all24 RGB bits. The existing
native stage checker compares EFB values after RGB565 conversion, matching its
destination format; native passes do not establish absence of discarded low-bit
EFB differences. Native `raw_errors` therefore means errors visible at RGB565
precision. The full geometry diagnostics remain necessary for that distinction.

### White-background discrimination

`white-background-two-rows` retains the two-row draw commands and final
red/white oracle, but first clears to white and verifies every background pixel.
It is diagnostic only: unlike the black-background baseline, a white draw can
pass without changing pixels. Missing red bits after a verified white background
would demonstrate corruption of already-correct bits and reject simple missed
coverage for that observation. Pair it with `two-rows`; do not promote a pass
as a rendering correction. The audit requires the white-background mode marker.

### Color-write-mask discrimination

`masked-two-rows` pairs with `white-background-two-rows`. It issues the same
alternating red/white thin geometry over fully verified white, but appends
BP 0x41 value 0x3100 after normal draw state to disable color and alpha writes.
The final raw RGB24 and RGB565 oracles require white on every iteration; draw
command hashes still alternate by vertex color. The audit requires both mode
markers and the extra five state bytes. This is a preservation control, not a
rendering fix, and a pass cannot prove the upstream pipeline was fully active.
Bits 3 and 4 were cross-checked against Dolphin's BlendMode definition:
https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPMemory.h

### Early depth rejection and state-only controls

`z-never-two-rows` enables depth comparison NEVER with depth writes disabled,
retaining the existing early-Z setting and color updates. Its full final oracle
requires the verified white background to survive alternating red/white draws.
`state-only` issues the same setup and 30 completion submissions with no
primitive headers or vertices. It also requires preserved white. The audit
checks each mode marker, all-white expectations, geometry counts and authored
lengths: depth rejection adds five first-batch bytes; state-only emits 385
first-batch bytes and five bytes in each later batch. Pair both with
`white-background-two-rows`. These controls do not render intended content.

`late-z-never-two-rows` uses the same depth NEVER comparison and preserved-white
oracle, adding the existing late-Z switch (BP 0x43000000). Pair it with
`z-never-two-rows` and normal `white-background-two-rows`. The audit requires
`early=0` and 1166 first-batch bytes, versus `early=1` and 1161 bytes for early
rejection; both retain 776 later-batch bytes. Extra/contradictory depth mode
markers are rejected. State-only still disallows late-Z to keep that control
unchanged. This tests rejection timing, not a usable rendering correction.

`alpha-never-two-rows` selects late timing and both alpha comparisons NEVER
with AND (BP 0xf3000000), keeping depth testing disabled and color writes
permitted. It requires preserved white while alternating red/white vertices
are emitted. Pair with late and early depth NEVER cases. Alpha and late depth
rejection each emit 1166 first-batch bytes and 776 later bytes; their rejection
register differs. These are different discard mechanisms, not identical state.
The audit requires the alpha mode marker, white oracle and extra state bytes.

`alpha-less-two-rows` and `alpha-gequal-two-rows` compare emitted alpha 255
against reference 128 with LESS (reject) or GEQUAL (accept), respectively.
The second comparison remains ALWAYS with AND; late timing and geometry match.
BP words are 0xf3390080 and 0xf33e0080. LESS checks preserved white; GEQUAL
checks rendered red/white. Both first-batch lengths are 1166 bytes. The audit
requires the specific threshold marker and corresponding full output oracle.
This checks a data-dependent comparator rather than relying solely on NEVER.

### Mixed alpha in the same batches

`alpha-mixed-two-rows` and `alpha-mixed-reverse-two-rows` keep GEQUAL128 and
late timing fixed. Every second two-row strip emits alpha0 and is rejected;
the others emit alpha255 and are accepted. Reverse flips the starting role;
roles flip every two iterations, so both red and white drawing occur at each
role before a swap. There are 153600 pixels per partition per iteration.
Rejected rows must preserve verified white; accepted rows must match red/white.
The audit requires the mixed mode and 0-255 alpha markers, complete per-iteration
partition counts summing to the full oracle, unchanged command lengths and
command hash stability across the four-iteration pattern period.

### Final logic-operation controls

`logic-copy-two-rows` and `logic-set-two-rows` retain the all-accept GEQUAL128
fixture and late timing, adding a BP 0x41 logic-operation override. COPY uses
0x4100311a and checks red/white; SET uses 0x4100f11a and checks forced white.
Color/alpha updates stay enabled, blending/dither disabled. SET ignores source
and destination color values for its expected output. Both first-batch lengths
are 1171 bytes, with 776 later bytes. The audit requires exact operation markers
and corresponding output expectations. The third control is ordinary
`alpha-gequal-two-rows`. SET is diagnostic and cannot reproduce general content.

The `logic-set-black-two-rows` and `logic-copy-black-two-rows` controls override
the initial background to verified black. SET must actively produce white;
COPY must produce red/white. The audit requires background marker00000000 for
these cases, keeping all final-oracle and command checks unchanged. This
prevents preservation of an already-white surface from being the only evidence
that SET behaves as intended.

### Opposite-color inversion

`logic-invert-white-two-rows` and `logic-invert-black-two-rows` use the same
INVERT destination operation (BP0x4100a11a), accepted thin geometry and late
timing. Verified white must become black; verified black must become white.
Both retain first1171/later776 bytes and alternating red/white vertex commands,
which the logic result does not depend on. Every pixel must change from its
background. Audit requires INVERT marker, correct background and complementary
full result oracle. Compare draw hashes across arms to check matched commands.

`logic-clear-two-rows` uses CLEAR logic (BP0x4100011a) to force zero output,
starting from verified white. Pair with `logic-invert-white-two-rows`: both
must actively produce black with identical geometry, alpha acceptance, timing
and command lengths, but CLEAR has no source/destination color dependence.
The audit requires CLEAR mode and full zero output. This is a color-operation
control, not the existing EFB copy-clear mechanism or a general rendering fix.

### RGBA6 storage comparison

`rgba6-logic-set-black-two-rows` and `rgba6-logic-clear-two-rows` select RGBA6/Z24
with no multisampling, restricted to exact forced-white/black colors. Initial
clear, draw and copy synchronization retain the selected format; normal callers
still use RGB8 copy control. First-batch size adds five bytes (1176), later776
unchanged. The audit requires the format marker and labels results
`rgba6-expanded-rgb888`, not full RGB8 storage precision. Pair with corresponding
RGB8 cases. RGB565 was not selected because libogc couples it to multisampling;
RGBA6 retains depth precision/sample mode. Reference:
https://github.com/devkitPro/libogc/blob/master/libogc/gx.c (GX_SetPixelFmt).

### Focused recurrent-pixel probe and corpus

`focus-530-54` emits two 8x2 SET quads covering x528..535/y52..55, from verified
black. The full oracle checks32 white and307168 preserved black pixels each
iteration. Exactly one submission/two quads/499 bytes is audited, with explicit
scope marker; pair with `logic-set-black-two-rows`. Reduced workload can test
context dependence at a recurrent site, but is not a full rendering fix.

`python3 tools/wii-gcn-fault-corpus.py --output NEW_DIRECTORY` collects geometry
first-pixel results from matrix directories. It verifies each used result/kernel
against its suite manifest and recorded kernel hash, rejects duplicate kernel
captures, and emits JSON, Markdown and a checksum manifest with its own source.
The corpus is first-error-censored and deliberately selected: no unbiased rates,
complete spatial distribution or physical diagnosis can be inferred from it.

### Wide recurrent-site probe

`focus-wide-530-54` adds `efb_primitive_focus_wide=1` to the existing
`focus-530-54` case. It expands the left edge from 528 to 0 while retaining
right edge 536, rows 52..55, two quads, one submission and 499 command bytes.
The oracle requires 2144 white pixels and 305056 preserved black pixels;
both raw RGB24 and RGB565 scans still cover all 307200 pixels. The host audit
requires the exact geometry marker, so small and wide captures cannot be
substituted. All diagnostic flags remain default-off.

The intermediate cases `focus-width-{16,32,64,128,256,512}` use the bounded
`efb_primitive_focus_width` integer parameter (8..536, default 8). The fixed
right edge remains x536 and the left edge is 536 minus width. The existing
wide flag remains compatible and selects width 536; combining it with a
nondefault integer width is rejected. Every case explicitly sets the integer
parameter, and the audit derives exact white/black coverage from its case
specification. The tiny and wide geometry, submission count and command size
are unchanged.

`focus-split-128` adds `efb_primitive_focus_split=1`, requiring focus mode,
width 128, or the wide override for the 536-pixel variant. Each y52..53/y54..55 strip is divided at x472
into x408..471 and x472..535 quads, emitted in row-major order. Coverage
remains 512 white and 306688 black pixels. There are four quads in one
submission, 595 command bytes (versus two quads/499 bytes unsplit). The audit
requires the split marker as well as the unchanged full-image coverage and
updated quad/byte counts. The parameter is diagnostic and defaults off.

`focus-split-536` applies the split flag to the wide focused case: row-major
pieces x0..63 through x448..511, followed by x512..535, for each two-row
strip. It requires 18 quads, 1267 bytes and the original 2144-white-pixel
coverage. The split marker reports first boundary 64 and maximum span 64.

`full-split-64` applies `efb_primitive_full_split=1` to the full-screen SET
from black control. Each of 30 batches retains its eight degenerate padding
quads and eight two-row strips, subdividing each strip into ten adjacent
64-pixel quads. This means 88 quads per batch, 4627 first-batch bytes and
4232 subsequent-batch bytes, well below the 64 KiB FIFO. The full 307200
pixels remain actively written and checked by both raw RGB24 and RGB565
oracles. The audit requires the full-screen split marker and per-batch
quad/byte counts. This is a default-off diagnostic.

`full-split-color-64` retains the original ordinary red/white-per-iteration
two-row state. `full-split-striped-64` alternates red/white two-row strips
and flips their phase each iteration; its unsplit control is
`two-rows-striped-control`. These use 4612 first-batch bytes and 4232 later
bytes; the SET variant includes 15 additional initial state bytes. All use
88 quads per batch and unchanged full-image coverage.

### Bounded textured native-span comparison

`native-span-64` and `native-span-160` disable the validated 1:1 quad shortcut
only in the temporary test module. Both retain the native horizontal stage
and final-state fence and split the final 320x240 rectangle into three
80-row submissions. The native geometry predicate restricts this diagnostic
to the four known 320x240 quadrants; this is not yet nonidentity scaling.

The `scale_native_span` parameter accepts 0 (off), 64 or 160 and requires
native tracing, state fencing, and disabled single/identity-quad modes.
Texture coordinates retain the existing negative-quarter S phase and
constant per-row T sampling. Each 64-pixel batch has 400 quads/32008 bytes;
the matched 160-pixel control has 160 quads/12808 bytes. Both fit the 64 KiB
FIFO, checked before emission. The original baseline emits 480 quads in one
38408-byte final stream. Subdivision would require 96008 bytes if emitted
unbatched, so three batches bound memory at the cost of two extra waits.

The host audit requires exactly three ordered span markers per sequence,
exact row ranges, quad counts and byte budgets, in addition to all native
source/intermediate/final/preservation oracles. Each frame has four sequences.
Native EFB comparison remains at RGB565 output precision.

### Nonidentity offset enlargement spans

`offset-span-64` and `offset-span-128` use `scale_offset_trace=1`,
`scale_offset_split=1` and bounded `scale_offset_span=64/128`. They target
the existing tiled RGB565 255x79 source at (0,43), enlarged to 256x79 at
(0,97) in a 256x256 destination. The native identity shortcut stays enabled
and does not apply to these unequal widths. All other scaler stages remain
unchanged. The final stage emits four or two quads per source row, using
the original constant-T and phased-S coordinates, in one submission.

The render client accepts `--offset-enlarge-repeat N` (1..1000), retaining
its existing source pattern, full destination oracle, source-integrity check
and MEM1 recovery check. It stops on first failure. The historical
`--offset-enlarge-only` selector retains its prior 2000-iteration behavior.
The matrix uses the explicit bounded selector and its `OFFSET: N/M` progress.

Auditing requires ordered crop/horizontal/final records for each iteration
(20145/20224/65536 pixels), exact span and vertex counts, and exact final
bytes from the logged pre-vertex FIFO position plus header, vertices and BP.
It verifies client progress and first-error stopping. Final EFB comparisons
use RGB565 output precision, including preserved destination pixels outside
the scaled region. Both cases use existing buffers and one final submission.

### Full-screen 2x system enlargement

`system-span-64` and `system-span-320` set `scale_system_trace=1`,
`scale_system_split=1` and `scale_system_span=64/320`. This is restricted
to the existing linear RGB565 320x240-to-640x480 system-memory predicate.
The unchanged horizontal nearest-run stage creates the intermediate; the
final stage emits 64x2 or 320x2 quads with the original phased-S/constant-T
coordinates. Six batches each cover 40 source rows / 80 destination rows.
Only the first batch carries setup state; all later batches retain it.
Pre-emission guards reserve FIFO trailer space. No new texture buffer is
allocated.

`--system-enlarge-repeat N` (1..1000) repeats the existing full-system linear
layout test, stopping on failure. Each iteration verifies every output
pixel and MEM1 accounting, and cleans its objects. The host audit checks
three ordered stages (76800/153600/307200 pixels), six contiguous batches,
quad counts, exact command sizes and client completion. EFB output
comparisons use RGB565 precision. Largest final batches: 32974 bytes (64)
and 7374 bytes (320). Total final bytes per operation: 193014 and 39414.
Both cases use six final submissions.

### One-submission 2x reduction

`reduce-span-64` and `reduce-span-160` enable the bounded diagnostic
`scale_reduce_span=64/160` with `scale_trace=1`, `scale_efb_full=1` and
`scale_split_reduce=1`. The exact existing 640x240-to-320x120 predicate
limits the changed final stage. The horizontal stage remains unchanged.
Final sampling retains source row2*y+1 and the phased S coordinates; the
client independently checks source column2*x+1 and row2*y+1.

`--reduce-repeat N` selects1..1000 iterations of the existing nonuniform
reduction test, stopping on failure. Each iteration checks all38400 output
pixels. The raw EFB oracle compares at RGB565 precision against the source
and copied output. The audit reports raw source mismatch counts and EFB/copy
differences separately; it does not invent a copied-error pixel count from
the client, which stops at its first mismatch.

The narrow case emits600 quads /48974 bytes; the wider240 quads /20174
bytes, each one final submission. FIFO bounds are checked before emission.
No additional texture buffers or waits. The host requires ordered complete
image checks, exact quad/byte budgets and complete client progress.

### Repeatable reduction content sweep

`reduce-content-64` and `reduce-content-160` use the unchanged reduction
module and `--reduce-content-repeat N` client selector. Geometry, nearest
sampling, buffers and submission count are unchanged. Iteration i selects
pattern i%8: black, white, red, green, blue, checkerboard, walking one bit,
or deterministic mixed values. Checker phase and walking-bit position vary
with i/8; mixed content uses a fixed 32-bit integer hash of source position
and seed `(i+1)*0x9e3779b9` modulo2^32. Each pattern appears125 times in
a complete1000-iteration run.

The client generates source content and independently evaluates it at the
existing expected sampled coordinates. The kernel still compares against
the actual source buffer at those coordinates. Pattern/seed markers are
audited in exact iteration order, and mismatched/missing schedules are
rejected. Existing fixed-pattern and uniform selectors retain their behavior.

### Focused vertical placement

`efb_primitive_focus_top` selects top row0..476 (default52), requiring focus
mode when nondefault. It moves both focused two-row rectangles together;
width, right edge, height and coverage counts remain unchanged. The marker,
geometry emission, batch row records and full-image oracle all use that
parameter. Existing focus-split cases also retain their relative rows.

`focus-wide-top-53`, `focus-wide-top-60` and `focus-wide-top-180` compare
one-row, eight-row and128-row downward shifts from the original wide focus
case. Every case writes2144 white pixels from verified black, checks the
remaining305056 pixels stay black, and submits two quads /499 bytes.
The host rejects captures with a mismatched top or batch row interval.

### Compensated viewport control

`focus-viewport-0` and `focus-viewport-32` enable the matched
`efb_primitive_focus_viewport` rewrite. The latter moves viewport origin
to y32 and subtracts32 from authored vertex y, retaining physical coverage
x0..535/y52..55. The control rewrites viewport origin0 and keeps vertices
y52..55. Both retain the physical scissor, viewport size640x480, two quads
and one submission. The extra six-register XF write costs29 bytes, making
both command streams528 bytes. The unchanged original remains499 bytes.

The mode requires the unsplit wide focus at top52. Its shift accepts0 or32.
The audit verifies compensated vertex/physical positions, full-image oracle
and exact command budget. This is a diagnostic, not default rendering.

### Matched command-count padding control

`focus-padded-536` adds `efb_primitive_focus_pad=1` to the original wide
probe. It emits16 zero-area quads at(0,52), then the original two536x2
quads. The mode requires unsplit wide focus at top52 with no viewport
rewrite. The padding marker is audited separately from the subdivision
marker. Both this control and `focus-split-536` have18 quads,72 vertices,
1267 bytes and one submission, with identical2144-white/305056-black
coverage. The original unpadded probe remains two quads/499 bytes.

This matches authored command volume and primitive count, not internal
raster work: degenerate padding produces no fragments while all subdivided
rectangles have area. It does not isolate every consequence of subdivision.

### Focused subdivision order

`focus-split-reverse` and `focus-split-columns` use the same 18 narrow quads
as `focus-split-536`, changing only their emission order. The parameter
`efb_primitive_focus_order` accepts 0 (row-major, the default), 1 (reverse
the complete row-major list), or 2 (draw both rows of each column before
advancing horizontally). A nonzero value requires focused subdivision.

All three orders retain 1267 command bytes, one submission, 2144 written
pixels and 305056 preserved black pixels. The audit requires the matching
order marker in addition to the geometry, command budget and full-image
checks. The original row-major command hashes were checked against the
previous padding comparison and matched for all 64 screening iterations.
This tests sensitivity to traversal among identical quads; it does not
isolate primitive width from all other consequences of subdivision.

### Focused subdivision width

`focus-span-96`, `focus-span-128` and `focus-span-256` retain the wide
focused coverage and row-major order, using maximum piece widths of 96,
128 and 256 pixels. `efb_primitive_focus_span` accepts those values or its
default 64; a nondefault value requires focused subdivision. The final
piece is clipped to the original right edge at 536.

| Piece width | Quads | Command bytes | Submissions |
| --- | --- | --- | --- |
| 64 (`focus-split-536`) | 18 | 1267 | 1 |
| 96 | 12 | 979 | 1 |
| 128 | 10 | 883 | 1 |
| 256 | 6 | 691 | 1 |
| Unsplit (`focus-wide-530-54`) | 2 | 499 | 1 |

Every case checks the same 2144 white and 305056 black pixels. The host
requires the selected span, first split boundary, quad count and byte
count. Width also changes piece boundaries and primitive count, so this
comparison cannot attribute a result solely to width.

`focus-right-64`, `focus-right-96`, `focus-right-128` and `focus-right-256`
add `efb_primitive_focus_right=1`, placing the remainder at the left edge.
Each retains the corresponding left-anchored case's coverage, piece count,
row-major order and command budget. A right-anchor marker reports the
first piece's width; the split marker reports its right boundary. The
rightmost piece is now a full selected span wide. Nondefault anchoring
requires focused subdivision. Both markers are audited against the case.

`focus-right-128-reverse` and `focus-right-128-columns` retain the
right-anchored 128-pixel layout (10 quads, 883 bytes, one submission),
selecting reverse and column-major emission respectively. The existing
anchor, span, coverage, command-budget and order audits all apply. These
cases use the existing module parameters; no driver rebuild is required.

`focus-right-64-reverse` combines the same anchor and reverse order with
64-pixel pieces, retaining 18 quads and 1267 bytes. It checks narrow
subdivision under both changes together against the reverse-128 control.

`focus-right-96-reverse` selects the intermediate 96-pixel span with the
same right anchor and reverse order: 12 quads, 979 bytes, one submission.
Its full-image oracle and geometry/order audits match the other focused
cases, with span and command budget checked against its own specification.

### Two-batch textured native diagnostic

`native-two-batches-64` and `native-two-batches-160` set
`scale_native_batch_rows=120` instead of the default 80. The final textured
stage retains its 240 one-row strips, sampling phase, row-major order and
piece widths, using two batches rather than three. Span 64 uses 600 quads
and 48008 bytes per batch (96016 total); span 160 uses 240 quads and 19208
bytes per batch (38416 total). The existing FIFO guard retains 256 bytes
of headroom. No extra texture allocation is introduced.

The parameter accepts only 80 or 120, and a nondefault value requires the
native-span diagnostic. Audits require exactly two ordered batches with
row intervals 0..120 and 120..240, the selected quad/byte counts, all
crop/horizontal/final checks and prior-content preservation checks.
These cases disable the validated identity shortcut to exercise thin
textured primitives; they do not change production defaults. One client
frame still contains four quadrant sequences.

`native-two-reverse-64` and `native-two-reverse-160` additionally set
`scale_native_reverse=1`. They reverse the complete quad list within each
120-row batch, retaining batch order, coordinates, phase, coverage and
command budgets. The parameter requires the native-span diagnostic and
defaults off. Each reversed batch emits a sequence/batch/order marker;
the audit rejects missing, reordered or mismatched traversal markers.
Normal-order captures must not contain reverse markers.

For traced native cases, the end-of-frame client can miss an intermediate
image error that later quadrant draws overwrite. The archived stage audit
still marks such a case FAIL even if the client exits successfully. These
errors are detected after capture, so native runs do not necessarily stop
at the first stage mismatch. `completed` reports full clean frames before
the first audited error; `checked` and `client_frames_completed` report
how far the run actually went. `first_error_sequence`, `first_error_frame`
(both one-based) and `client_reported_pass` make this distinction explicit.
This reporting correction does not alter earlier archived results.

### Stop after a bad native quadrant

`native-stop-64` and `native-stop-160` use the reversed two-batch layouts
with `scale_native_stop_on_error=1`. Native stage and preservation
comparisons latch a local error flag. After completing all checks and
copying the current quadrant's output, the driver logs `native-stop` and
returns `EILSEQ` before the next quadrant ioctl. This preserves complete
three-stage evidence for the failing sequence while preventing later
quadrants from hiding the error. Production behavior defaults unchanged.

The audit accepts an incomplete four-quadrant frame only with the exact
final-sequence stop marker, a client failure and a real oracle mismatch in
that sequence. It still requires every stage, preservation check, command
record and batch for all captured sequences. Missing evidence is ERROR,
not a verified stop. Checked frames include a partial last frame; clean
and client-completed frame counts exclude it.

`native-stop-control-1` and `native-stop-control-3` deliberately toggle one
bit in the expected value at final-stage pixel (0,0), using
`scale_native_test_mismatch=1` or `3`. They never modify framebuffer data
and require stop mode. The kernel and audit explicitly identify this
injection; `injected_oracle_error=true` distinguishes it from hardware
faults. Expected control results are FAIL with exactly one raw/one copied
oracle mismatch, no other faults, and a stop at the chosen sequence.
These controls must not be included in hardware-failure statistics.

### Native source-content cycle

`native-content-64` and `native-content-160` retain the reversed two-batch
stop-enabled geometry and add the client's `--content-cycle` selector.
It requires offscreen `xrgb8888-native-tiled` mode. Frame modulo eight
selects black, white, red, green, blue, one-pixel checkerboard, walking
RGB565 bit, then deterministic hashed RGB565 noise. RGB565 channels are
expanded by bit replication into the XRGB8888 source. Checker phase and
walking-bit position advance each eight-frame cycle; noise uses the seed
`(frame + 1) * 0x9e3779b9` modulo 2^32 and a fixed integer hash.

Every attempted frame logs its zero-based frame, pattern index and seed.
The native audit requires the exact schedule through the partially checked
last frame, if any. Existing source-based kernel oracles and full-image
client checks remain active. No driver or geometry change is involved.
Use the archived content-capable client through `--native-client`; older
clients do not support this selector. Other client patterns are unchanged.

### General bounded final-stage regression

`bounded-final-regression` runs the full render UAPI regression with
`scale_bounded_final=1`. The opt-in path keeps the existing nearest-source
row mapping and sampling phase, splitting each resulting destination row
run horizontally into pieces at most 64 pixels wide, including a clipped
tail. It works with arbitrary supported widths, offsets and scaling ratios
instead of the fixed diagnostic dimensions. The validated 1:1 shortcut
retains precedence.

Before active rendering it completes the preservation/state stream. Each
subsequent final batch contains at most 600 quads / 48008 authored bytes;
a compile-time check retains the 256-byte FIFO reserve. Intermediate
batches finish before the next begins, and the caller completes the last
batch before copying output. No new texture buffers are allocated.

The option defaults off and rejects combination with the specialized
scale/native/offset tracing modes and the system-span geometry override.
System tracing alone is supported for stage localization. `bounded-begin` and
`bounded-batch` records identify every call and batch. The audit independently
computes nearest-row run counts, requires exact piece/batch counts and byte
budgets, and requires the full client regression result. These tests use
client pixel checks, not raw EFB readback. The path is experimental: extra
vertex traffic and a state-completion wait per call are real costs, and a
single regression pass is not an intermittent-fault qualification.

`bounded-offset`, `bounded-system`, and `bounded-reduce-content` repeat
nonidentity workloads with the same general helper, up to 1000 iterations
each. They require a render client supporting `--offset-enlarge-repeat`,
`--system-enlarge-repeat`, and `--reduce-content-repeat`, respectively.
Each iteration checks the entire destination and stops on a client failure.
Reduction cycles eight source patterns, including checkerboard, walking
bits and deterministic noise. The audit requires every progress record,
the exact geometry and batch sequence for every attempted call, and the
exact reduction pattern/seed schedule. A failing last attempt counts as
checked but not completed. Raw EFB and copy error counts remain null.

Example using the archived experimental module and compatible client:

```sh
tools/wii-gcn-efb-matrix.py --allow-dirty \
  --module /media/anolis/dev/wii-gcn-bounded-final-module.ko \
  --render-client /media/anolis/dev/wii-gcn-native-content-clients/wii-gcn-render-test \
  --cases bounded-reduce-content,bounded-system,bounded-offset \
  --iterations 1000 --output /media/anolis/dev/wii-gcn-bounded-workloads-new-run
```

The 16-iteration real-capture fixtures come from
`wii-gcn-matrix-bounded-workloads-20260914-r1`. Audit tests also mutate those
captures to reject incomplete progress, wrong geometry, wrong content
schedules, and inconsistent failure accounting. Those mutations are unit
test inputs, not observed hardware failures.

`bounded-system-400` selects `scale_bounded_batch_quads=400`, producing
six 32008-byte final batches instead of four 48008-byte batches. The
parameter permits only 400 or 600, defaults to 600, and requires bounded
mode when changed from its default. The audit selects the exact expected
batch limit from the case. Both limits have now failed repeated full-screen
enlargement checks; neither is a qualified workaround. New workload results
retain the first client failure record and pixel coordinates, when present.

`bounded-system-trace` retains the general path's unsplit horizontal pass
and 600-quad final batches while enabling `scale_system_trace=1`. It checks
the CPU-created crop against the source, then checks raw EFB and copied
RGB565 pixels after horizontal and final rendering. A stage mismatch stops
after the current call's three checks and final copy, with an explicit
`bounded-stop` marker and `-EILSEQ`. The audit requires ordered stage counts,
horizontal geometry, all final batches, and the exact first-fault stop.
Raw errors are measured at RGB565 precision; the CPU crop has no raw EFB
sample. Allocating/readback of an EFB snapshot changes diagnostic timing,
so a traced pass cannot qualify the untraced path.

`bounded-system-horizontal-split` uses `scale_system_split=1` without stage
readbacks. It splits each horizontal 2x240 strip into two 2x120 rectangles,
retains the general final helper's four 600-quad batches, and runs the same
full-destination client checks. The runner and audit require verification
of the loaded split parameter. This tests a focused geometry change; it
does not implement a general horizontal subdivision policy.

### General horizontal and final subdivision

`bounded-both-regression` and `bounded-both-{system,reduce-content,offset}`
enable `scale_bounded_final=1 scale_bounded_horizontal=1`. The horizontal
helper retains nearest-source column groups and sampling phase, splits
their height into at most 120-pixel pieces, and clips the last piece. It
emits up to 640 quads per batch, includes existing state in the first
submission, and finishes intermediate batches before continuing. This
keeps the validated 320x240 to 640x480 horizontal geometry in one submission.
The 1:1 shortcut retains precedence. No additional texture buffer is added.
The option defaults off, requires bounded final mode and rejects conflicting
specialized trace/split overrides; it supersedes the default focused
reduction split while enabled.

`bounded-horizontal-begin` and `bounded-horizontal-batch` records allow the
audit to independently verify column-run counts, clipped-piece counts,
cross-stage dimensions, batch sizes and FIFO budgets. Every attempted
horizontal call must match a final call. Repeated workloads also require
their exact source geometry. These cases use client pixel checks, without
raw EFB readback. The expanded regression includes 255x255 to 256x127 with
offsets: its 765 horizontal quads require two batches, crossing a batch
boundary inside a column and exercising a clipped 15-pixel tail.

All render regression audits require an active provider. The client's
intentional provider-absent test can print PASS, which is insufficient for
these hardware suites. The failed first general-helper configuration is
retained as a fixture to ensure this situation is rejected.

### Enlargement content and ioctl timing

`bounded-both-system-content` uses `--system-content-repeat N` to cycle
black, white, red, green, blue, one-pixel source checkerboard, walking RGB565
bits, and deterministic noise. Checker phase and walking-bit position advance
each cycle. The existing reduction generator is evaluated at `(2*x,2*y)`;
this produces one-pixel patterns in the 320x240 source. Every attempted call
logs its iteration, pattern index and seed; the audit requires the exact
schedule, both helpers' geometry and every destination pixel check.

The client also records monotonic-clock duration around each system scaling
ioctl, including CPU staging and driver command/completion work but excluding
object allocation, source generation and client verification. The audit keeps
raw timing samples and mean, median, nearest-rank p95 and maximum for clean
iterations only. The failed attempt, if any, is excluded from these summaries.
Kernel diagnostic logging remains active; these measurements are not display
frame rates or a production benchmark.

`system-baseline-timed` uses the driver's default path; `bounded-both-system-timed`
uses both helpers with the original ramp source. They require the new timing
client and exact per-attempt timing records. A short order-reversed comparison
can assess cost without an unnecessary long run of a known unreliable control.
Never use timings from censored failures to claim a stable speed/reliability tradeoff.

`bounded-both-system-profile` selects `--system-profile-repeat N`, retaining
the eight-pattern enlargement sequence while recording thread CPU time
(`CLOCK_THREAD_CPUTIME_ID`) and process voluntary/involuntary context-switch
deltas (`getrusage`; the client is single-threaded). The elapsed interval
encloses the profile reads as well as the ioctl. The audit requires every
CPU record and a consistent reported clock resolution, and keeps clean-call
CPU samples, switch counts and signed elapsed-minus-CPU differences.

Elapsed-minus-CPU is an approximation of off-CPU time plus measurement
overhead, not a GPU wait timer. Accounting granularity may also affect it;
negative differences are preserved rather than clamped or called waits.
Context switches can indicate blocking/preemption but do not identify the
specific wait or competing task. This profile changes measurement overhead
and does not add kernel instrumentation or alter rendering commands.

### Isolated scheduler captures

`bounded-both-system-sched` selects `--system-sched-repeat N`. It requires
a separately prepared tracefs instance at
`/sys/kernel/tracing/instances/wii-gcn-sched`, initially stopped, with a
bounded buffer (the validated captures used 1024 KiB), `mono` clock and
`sched/sched_switch` enabled. Optional workqueue execute-start/end events
identify functions when their records fall inside a captured window.
Use a short run such as 64 iterations. Refuse an existing instance rather
than reusing another tracing session. If tracefs was not mounted, remember
to restore that state after removing this instance.

The client enables tracing and emits `GCN begin iteration=N` before each
profiled call, then emits the matching end marker and disables tracing
before client verification. Exit cleanup also attempts to disable tracing.
After the case, verify tracing is off, download `trace`, CPU0 `stats` and
the device's trace SHA256, and remove only the instance created for the test.
The regular matrix result validates pixels/CPU records; it does not validate
the separately collected scheduler trace.

`tools/wii-gcn-scheduler-audit.py --trace TRACE --stats REMOTE_STATE
--client CLIENT_LOG --output NEW_RESULT` separately checks the device hash,
zero loss/overrun counters, entry counts, single-CPU records, ordered complete
windows and task-switch continuity. REMOTE_STATE contains the CPU0 stats
followed by `sha256sum` of the instance's trace file. The output attributes
scheduled intervals within each window to PIDs and retains workqueue records.
Scheduled time is not pure CPU execution: interrupt time may be included.
Functions that began outside the window remain unknown; do not label them
from a worker's name or another window's function. Archive this supplementary
evidence with its client log and analyzer source independently of the matrix.

### Deferred output comparison

`bounded-both-system-profile-deferred` uses the same client/module/profile
as the live case, but omits the live `dmesg -W` collector and redirects client
output to a unique remote file until the client exits. The runner then emits
the saved client output; the matrix separately verifies its device checksum
and exact streamed suffix before archival. The runner's `--defer-output`
accepts only the matrix's random `/tmp/gcn-matrix-<32 hex digits>.client` path
and refuses an existing file.

Kernel diagnostics remain in the ring and are collected after console
restoration, bracketed by unique begin/end markers. Missing, duplicate or
misordered markers and incomplete helper records are errors. The kernel's
16 KiB ring limits this case to eight iterations; no log clearing, kernel
reboot or buffer expansion is used. Remote files are deleted only after
checksum verification and successful audit; failures preserve evidence.
The progress bar advances when deferred client output becomes available.
Use short matched pairs in both orders to compare capture overhead, not
these small batches to qualify intermittent-fault reliability.

### Whole-loop scheduler capture

`bounded-both-system-sched-loop` uses `--system-sched-loop-repeat N` and the
same isolated trace instance setup as `bounded-both-system-sched`. The matrix
limits this case to 64 iterations. Allocate a 1024 KiB instance buffer and enable
`sched_switch`, `workqueue_execute_start`, and `workqueue_execute_end` before
running it. Tracing spans allocation, source generation, ioctls, and verification;
`GCN begin/end iteration=N` still delimit each ioctl observation window.
`GCN loop begin/end` distinguish continuous capture from the older gated mode.

The scheduler audit tracks work identities across the gaps and reports
`other_work_scheduled` within each ioctl window. Missing starts remain `unknown`;
gated captures discard function state after each window. These intervals are
scheduled time and may include interrupt execution, not exact function CPU time.
The audit rejects lost entries, incomplete loop markers, and mismatched work ends.
Archive and checksum the stopped trace before removing the owned instance and
restoring the original tracefs mount state.
