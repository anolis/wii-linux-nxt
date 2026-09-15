# GX 1:1 scaler correction — 12 September 2026

The 1:1 scaler correction is enabled by default in the working build and has
passed native and full render regression testing. The installed provider was
not replaced. Arbitrary thin-primitive diagnostic faults remain unresolved;
this correction avoids that rendering pattern for identity blits.

## Problem and change

The native client updates four 320x240 rectangles in a 640x480 image. Although
source and destination rectangles have equal extents, the generic scaler
rendered them as thin column/row runs. Intermittent missing color bits appeared
in EFB before copying. The saved baseline command variants match an earlier
failing capture exactly; a short baseline pass is not evidence of reliability.

`scale_identity_quad`, now default true, uses one textured quad in each scaler
stage whenever source and destination rectangle extents match. It keeps source
cropping, active-rectangle scissoring, destination preservation and the existing
copy path. The predicate is independent of diagnostic tracing and specific
quadrant coordinates. Non-identity scaling retains its previous geometry.

The native horizontal/final stages change from 640/480 quads to 1/1. The tested
semantic negative-quarter UV phase preserves exact 1:1 sampling. No CPU pixel
correction or out-of-rectangle overdraw is used by this fix.

## Validation

| Check | Result |
|---|---|
| Native final-quad candidate, detailed diagnostics | 1000/1000 frames; 4000 rectangles; no stage or preservation errors |
| General identity path, detailed diagnostics | 16/16 frames |
| General identity path, tracing disabled | 1000/1000 frames; 307200000 client pixel checks; zero errors |
| Full render UAPI regression, identity enabled | PASS |
| Final default-enabled build, no enabling argument | Live parameter verified Y; 16/16 native frames; full render UAPI suite PASS |
| Build/harness checks | PowerPC W=1, strict client builds, audit regression tests and diff checks pass |

The detailed 1000-frame audit covered 12000 stage records, 12000 prior/preservation
records, 8000 command records and 4000 state records. Native comparisons use
RGB565 output precision; the untextured geometry diagnostics compare all 24 RGB
bits. Normal-operation testing uses the full client oracle and does not claim
unmeasured raw-EFB checks. All completed evidence manifests were verified.

## What the discriminating tests ruled out

- Late Z, depth ALWAYS/write controls and constant TEV color still failed.
- Tall quads clipped to thin rows passed short trials, then failed extended testing.
- CPU color-aperture writes passed 1000 iterations while thin primitive draws failed.
- Per-batch scans found errors in the batch just drawn, before final copying.
- Four-row overlapping writes passed 1000 for two-row strips, but failed for
  one-row strips. That overlap approach was rejected as a general correction.

Repeated first-error coordinates with the same missing bit are evidence to
investigate, not proof of a hardware defect. This work does not establish the
physical cause or guarantee arbitrary thin triangles/strips on this Wii.

## Build and evidence

Validated scaler module (retained independently of later diagnostic builds):
`/media/anolis/dev/wii-gcn-identity-default-module.ko`

SHA256:
`e4b9b7aeb5646107e12c6a9b6f8cd8cddf235cce914a1232cbe166a6615390c1`

Exact default-build archive:
`/media/anolis/dev/wii-gcn-identity-default-module.ko`

Evidence directories under `/media/anolis/dev/`:

- `wii-gcn-matrix-native-single-20260912-long-r2`: detailed 1000-frame pass.
- `wii-gcn-matrix-identity-quad-20260912-production1000`: normal 1000-frame pass.
- `wii-gcn-matrix-identity-quad-20260912-regression`: full render suite.
- `wii-gcn-matrix-identity-default-20260912-final`: default parameter and final suite.

The earlier `wii-gcn-matrix-native-single-20260912-long` stopped at storage
preflight before rendering. Old evidence was losslessly compressed and verified
on both host and Wii before reclaiming its redundant uncompressed copy.

See `wii-gcn-efb-matrix.md` for the reusable suite and `wii-6.18-gx-port.md` for
experimental history. The tested module was loaded temporarily and unloaded;
the CPU console was restored after every completed case.

## Follow-up: draw corruption over verified white

Two matched pairs, each capped at 16 iterations and stopped on its first error,
compared identical two-row draws over verified black versus verified white.
The reverse-order repetition also failed:

| Run | Background | Successful iterations before failure | First discrepancy |
|---|---|---:|---|
| r1 | Black | 0 | (600,429), red ff → f7 |
| r1 | White | 5 | (638,263), white → ffdfff (green ff → df) |
| r2 | White | 8 | (607,343), red ff → df |
| r2 | Black | 2 | (587,302), red ff → 7f |

Each failing draw followed a complete, zero-error background scan. In r1,
white drawing over white removed a green bit; in r2, red drawing removed a red
bit already present in white. Three repeated reads retained each wrong value,
and RGB565 copying reproduced it. Simple missed coverage cannot explain those
observations. Hardware versus pipeline-state causation remains unresolved.
The white background is a diagnostic control, not a workaround or full-coverage
correctness test: an omitted white draw would leave its expected pixels intact.

Evidence: `wii-gcn-matrix-white-background-20260912-r1` and `-r2` under
`/media/anolis/dev/`; both manifests verified. All 19 audit checks pass.
Diagnostic module: `/media/anolis/dev/wii-gcn-white-background-module.ko`, SHA256
`72cef8d3a51efd590c7ead2ab60fa74cdca332e5a0a1c45eb98be16a0dbf34aa`.
This white-background diagnostic build is retained in its named archive. The validated
scaler archive above retains its original checksum. The new diagnostic flag
is disabled by default; no installed provider was replaced.

## Follow-up: corruption with color writes disabled

Two further matched pairs tested the same thin geometry over verified white,
with and without color/alpha updates enabled. The masked case retained the
white oracle while still emitting alternating red/white vertex colors.

| Run | Writes | Successful iterations before failure | Raw / copy errors |
|---|---|---:|---:|
| r1 | Enabled | 2 | 1 / 1 |
| r1 | Disabled | 0 | 2 / 2 |
| r2 | Disabled | 0 | 3 / 2 |
| r2 | Enabled | 10 | 1 / 1 |

The first masked failure was (62,2), white ffffff → fffff7 (blue bit 3).
The repeat first failed at (331,146), white ffffff → fffeff (green bit 0).
All background pixels passed immediately before each draw. Three repeated
reads retained both first errors. The second first error is below RGB565
precision, while the full oracle found two RGB565-visible errors elsewhere
in that iteration. The red draw otherwise preserved white,
consistent with the mask taking effect. Disabling color updates is insufficient
to prevent corruption here; this does not establish a physical hardware cause
or prove which upstream stages execute with writes disabled.

Evidence: `wii-gcn-matrix-masked-write-20260912-r1` and `-r2`; both complete
manifests verified. PowerPC W=1 build, all 20 audit tests and diff checks pass.
Masked-write diagnostic archive: `/media/anolis/dev/wii-gcn-masked-write-module.ko`,
SHA256 `70b1d44a18a40ae14b00842f5c77cd48491a0c2c5fecf660fa2b3e1b238d3096`.
The validated scaler archive remains unchanged. Neither diagnostic flag is enabled by default.

## Follow-up: early depth rejection isolates a passing control

| Case | Short sweep | Extended sweep | Extended raw / copy errors |
|---|---:|---:|---:|
| State and fences, no primitives | 16/16 PASS | 1000/1000 PASS | 0 / 0 |
| Early depth NEVER, color writes enabled | 16/16 PASS | 1000/1000 PASS | 0 / 0 |
| Normal thin draws over verified white | Failed first iteration | Failed second iteration | 1 / 1 |

Each passing extended case checked 307,200,000 pixels per raw RGB24/copy oracle
and completed 30,000 submissions. Depth rejection took 163.21 seconds;
state-only took 160.98 seconds from kernel start to result. The normal control
then failed at (602,355), white ffffff → ffffef (blue bit 4); its verified white
background was correct, all three rereads agreed, and RGB565 copy was fffd.

This is evidence that early rejection avoids the tested corruption while
color-write masking alone does not. It narrows attention to work suppressed by
early rejection; it does not uniquely identify a physical pipeline stage or
hardware defect. Depth NEVER and state-only are preservation controls that
cannot render the intended content. Non-identity thin-draw correctness remains
unresolved; the separately validated 1:1 scaler correction remains intact.

Evidence under `/media/anolis/dev/`: `wii-gcn-matrix-reject-depth-20260912-r1`
and `wii-gcn-matrix-reject-depth-20260912-long`. Both manifests verified;
PowerPC W=1, all 22 audit tests and diff checks pass. Early-rejection diagnostic
archive `/media/anolis/dev/wii-gcn-reject-depth-module.ko` SHA256:
`7dfedfa5866d65c1ac28cd5e01e784404d1c7e06b6bbb6764efe893c528953a9`.
All new diagnostic flags are default-off; installed provider untouched.

## Follow-up: rejection timing changes the outcome

Two same-build comparisons reversed early/late ordering while preserving depth
NEVER, disabled depth writes, enabled color writes, thin geometry and the
verified-white oracle. Late rejection changes BP 0x43 from 0x40 to 0x00;
its extra five command bytes and `early=0` marker are audited.

| Run | Early rejection | Late rejection |
|---|---|---|
| r1, late first | 16/16 PASS | Failed first iteration: 3 raw / 2 copy errors |
| r2, early first | 16/16 PASS | Failed fifth iteration: 5 raw / 3 copy errors |

The r1 normal-draw control also failed on its first iteration. First late
errors were (75,334), ffffff → dfffff, and (530,54), ffffff → ffff7f.
Both followed a complete correct white-background scan; all repeated reads
agreed and RGB565 copies reproduced the visible discrepancies.

Together with the previous 1000-iteration early-rejection pass, these results
focus the investigation on work or behavior suppressed by early rejection
but retained with late rejection. They do not uniquely locate a physical stage:
changing rejection timing can alter internal activity and scheduling. Neither
NEVER control renders intended content, so this remains localization evidence,
not a general fix. The validated 1:1 scaler archive remains unchanged.

Both manifests verified: `wii-gcn-matrix-late-reject-20260912-r1` and `-r2`
under `/media/anolis/dev/`. W=1 build, all 23 audit tests and diff checks pass.
Late-rejection archive: `/media/anolis/dev/wii-gcn-late-reject-module.ko`, SHA256
`728becb7461dd026dedecb09949b66ea418ba3f9412d89781583bf5e2606af66`.
Diagnostic flags remain default-off; installed provider untouched.

## Follow-up: alpha rejection passes with late timing

| Case | Short sweep | Extended sweep |
|---|---|---|
| Alpha NEVER, late timing | 16/16 PASS | 1000/1000 PASS, zero raw/copy errors |
| Depth NEVER, late timing | Failed first iteration | Failed second iteration, 3 raw / 2 copy errors |
| Depth NEVER, early timing | 16/16 PASS | Previously validated 1000/1000 |

The alpha run checked 307,200,000 pixels per raw RGB24/copy oracle across
30,000 draw submissions, taking 162.11 seconds from kernel start to result.
The subsequent late-depth control failed at (547,2), ffffff → fffff7;
background validation passed, all three rereads agreed, and RGB565 copy was
fffe. Both mechanisms otherwise preserve the intended white background.

This distinguishes alpha rejection from late depth rejection in the tested
state. Together with the earlier early-depth pass and masked-write failures,
it focuses attention on activity affected differently by these discard paths.
It does not prove that all upstream stages execute identically, locate a
physical component, or provide a general rendering correction. Both passing
rejection controls deliberately discard the intended draws.

Evidence under `/media/anolis/dev/`: `wii-gcn-matrix-alpha-reject-20260912-r1`
and `wii-gcn-matrix-alpha-reject-20260912-long`; both manifests verified.
PowerPC W=1, all 24 audit tests and diff checks pass. Alpha-rejection archive:
`/media/anolis/dev/wii-gcn-alpha-reject-module.ko`, SHA256
`d0a8d65400343ec2cc6e2f04999b90b3c237e588bcb09a7e2115748a14767014`.
All diagnostic flags remain default-off. The validated 1:1 scaler archive and
installed provider remain unchanged.

## Follow-up: ordinary alpha threshold reproduces the distinction

Both threshold cases emit alpha 255, use reference 128 and late timing, and
retain identical geometry/submission sizes. LESS rejects; GEQUAL accepts.
The second comparison stays ALWAYS with AND. The different predicates use
separate expected-image oracles: preserved white versus rendered red/white.

| Case | Short sweep | Extended sweep |
|---|---|---|
| Alpha LESS 128 | 16/16 PASS | 1000/1000 PASS, zero raw/copy errors |
| Alpha GEQUAL 128 | Failed third iteration | Failed fourth iteration |
| Alpha NEVER | 16/16 PASS | Previously validated 1000/1000 |

LESS checked 307,200,000 pixels per raw RGB24/copy oracle over 30,000 draw
submissions in 161.87 seconds. Short GEQUAL first failed at (55,14), expected
ff0000 → ff0008: an unexpected blue bit, not a missing bit. Extended GEQUAL
first failed at (633,472), ffffff → 7fffff. All backgrounds passed, all triple
rereads agreed, and RGB565 copies reproduced both visible errors.

Thus the passing discard result is not confined to the NEVER opcode. This
still does not prove identical internal work for all predicates or fix thin
rendering. A useful next discriminator is mixed accepted/rejected strips in
the same draw batches with a fixed comparator and per-strip alpha: separately
check written pixels and preserved pixels rather than only all-discard images.
The following section records the subsequently implemented mixed test.

Evidence under `/media/anolis/dev/`: `wii-gcn-matrix-alpha-threshold-20260912-r1`
and `wii-gcn-matrix-alpha-threshold-20260912-long`; both manifests verified.
W=1 build, all 25 audit tests and diff checks pass. Threshold-test archive:
`/media/anolis/dev/wii-gcn-alpha-threshold-module.ko`, SHA256
`58107cd1ecca13ebc2215c0fd4abf38d062a1d0e98dddc5aada052b372d36dd0`.
Diagnostic flags remain default-off; installed provider and validated scaler
archive remain unchanged.

## Follow-up: mixed accepted/rejected strips localize observed errors

One fixed GEQUAL128 comparison accepts alpha255 strips and rejects alpha0
strips in the same batches. Every second two-row strip is rejected. Roles
reverse every two iterations; a second case starts with the opposite phase.
Each iteration checks 153600 pixels in each partition, with independent
raw/copy error totals. Red iterations require accepted strips to change from
white to red, validating both written and preserved parts of the image.

| Run / starting phase | Successful iterations before failure | Accepted raw/copy errors | Rejected raw/copy errors |
|---|---:|---:|---:|
| r1 normal | 7 | 1 / 0 | 0 / 0 |
| r1 reverse | 4 | 1 / 0 | 0 / 0 |
| r2 reverse | 1 | 1 / 1 | 0 / 0 |
| r2 normal | 0 | 1 / 1 | 0 / 0 |

First pixels respectively: (547,154) ffffff → feffff; (46,107) ff0000 → fe0000;
(575,94) ffffff → f7ffff; (541,241) ff0000 → ef0000. Triple rereads retain every
error. RGB565 hides the two red-LSB errors in r1 and reproduces the visible
errors in r2. All checked rejected pixels remain exact. The r1 all-accept
control failed on iteration2; all-reject passed16.

These four censored runs place observed faults in accepted strips even with
neighboring rejected strips in the same batches. They support a relationship
to accepted-fragment processing, without proving a physical component, a
universal absence of collateral corruption, or an acceptable failure rate.
Mixed acceptance does not fix rendering.

Both evidence manifests verified: `wii-gcn-matrix-alpha-mixed-20260912-r1` and
`-r2` under `/media/anolis/dev/`. W=1 build, all 26 audit tests and diff checks
pass. Mixed-alpha archive `/media/anolis/dev/wii-gcn-alpha-mixed-module.ko`,
SHA256 `c8ca6f801d3304d75c484bffb56d4e51191b4845b10e338f638f76da6c2145af`.
The general color-rect wrapper still emits alpha255; only the diagnostic mixed
mode varies it. All new flags remain default-off; installed provider and the
validated 1:1 scaler archive remain unchanged.

## Follow-up: forcing final white output still corrupts pixels

COPY and SET logic operations use identical accepted thin geometry, alpha
comparison, late timing and command lengths. COPY checks fragment red/white;
SET checks white independent of incoming/destination color. Both permit color
and alpha updates. A contrasting black-background check validates active SET
output rather than preservation of an already-white surface alone.

| Case | Successful iterations before failure | Raw / copy errors |
|---|---:|---:|
| r1 COPY, white background | 1 | 2 / 2 |
| r1 SET, white background | 15 | 1 / 0 |
| r2 SET, white background | 3 | 1 / 1 |
| r2 COPY, white background | 1 | 1 / 0 |
| SET, verified black background | 0 | 4 / 2 |
| COPY, verified black background | 1 | 4 / 1 |

SET over black produced correct white at 307196 of 307200 pixels on its first
red-vertex draw, demonstrating active output transformation. The first error
was (530,54), ffffff → ffff7f, retained across triple rereads and RGB565 copy
ffef. SET over white first failed at (547,154), ffffff → feffff, then at
(575,14), ffffff → fffff7. Complete background scans were correct beforehand.

Forcing final color to white does not prevent the corruption. This strengthens
the case for investigating the final color-operation/output path or its state
handling, but it does not uniquely identify a physical component or establish
a repair. SET cannot render arbitrary content; all diagnostic flags remain off
by default. The validated 1:1 scaler correction is unchanged.

Evidence under `/media/anolis/dev/`: `wii-gcn-matrix-logic-output-20260912-r1`,
`wii-gcn-matrix-logic-output-20260912-r2`, and `wii-gcn-matrix-logic-black-20260912-r1`.
All manifests verified. W=1 build, all 28 audit tests and diff checks pass.
White-background module archive: `wii-gcn-logic-output-module.ko`, SHA256
`255ef708e33325b488e9554363b6ab3a0d6b2ecfa30ff68d1c024941b714258a`.
Black-background logic archive `/media/anolis/dev/wii-gcn-logic-black-module.ko`, SHA256
`a1e4a0cb06133f0266047cb139bdfaae8704221b645e22fdc109d0d865886cc9`.
Installed provider remains unchanged.

## Follow-up: inversion fails in both output directions

Both arms use the same INVERT destination operation and accepted thin geometry.
Only the verified starting color and complementary expected result differ.
All 30 first-iteration draw command hashes match across arms in each pair.

| Run | Transition | Successful iterations | Raw / copy errors | First discrepancy |
|---|---|---:|---:|---|
| r1 | White → black | 0 | 6 / 5 | (529,100), 000000 → 800000 |
| r1 | Black → white | 0 | 13 / 12 | (34,18), ffffff → 7fffff |
| r2 | Black → white | 0 | 7 / 7 | (35,34), ffffff → ffefff |
| r2 | White → black | 0 | 2 / 2 | (546,170), 000000 → 100000 |

All background scans passed. Every first error persisted across three rereads,
and visible discrepancies were reproduced by RGB565 copying. Each pixel must
change, preventing absent draws from passing. Both opposite transitions fail,
so the issue is not confined to white output or to cleared expected bits.
These results do not distinguish faulty destination reads from output writes
or state handling; INVERT exercises the destination read as well as writing.
They are diagnostic evidence, not a rendering correction or failure-rate study.

Evidence: `wii-gcn-matrix-logic-invert-20260912-r1` and `-r2` under
`/media/anolis/dev/`; both manifests verified. W=1 build, all 29 audit tests
and diff checks pass. Inversion archive:
`/media/anolis/dev/wii-gcn-logic-invert-module.ko`, SHA256
`efabc937557396ab2d3c32c85bba5bb343cd5f3e958c7be1387d75f4d759c40a`.
All diagnostic flags remain default-off; installed provider and validated
1:1 scaler archive unchanged.

## Follow-up: forced-zero output also fails

CLEAR and INVERT-from-white both must actively change a verified white image
to black. Geometry, alpha acceptance, late timing and command lengths match.
CLEAR's expected result is independent of source and destination color values;
INVERT's result depends on the destination.

| Run | Operation | Successful iterations before failure | Raw / copy errors | First discrepancy |
|---|---|---:|---:|---|
| r1 | CLEAR | 2 | 1 / 1 | (226,474), 000000 → 000010 |
| r1 | INVERT | 0 | 2 / 2 | (303,178), 000000 → 000020 |
| r2 | INVERT | 0 | 4 / 3 | (474,110), 000000 → 010000 |
| r2 | CLEAR | 1 | 1 / 1 | (210,158), 000000 → 000008 |

Every background scan passed and each first wrong value persisted across three
rereads. CLEAR's unexpected blue bits were visible in RGB565 copies 0002 and
0001. Thus corruption is not confined to output operations with source or
destination color dependence: forced-zero and previously tested forced-white
outputs both fail. This does not establish whether hidden reads occur or
uniquely identify faulty hardware versus state/output handling. General thin
rendering remains unresolved.

Evidence: `wii-gcn-matrix-logic-clear-20260912-r1` and `-r2` under
`/media/anolis/dev/`; both manifests verified. W=1 build, all 30 audit tests
and diff checks pass. Forced-zero archive:
`/media/anolis/dev/wii-gcn-logic-clear-module.ko`, SHA256
`c015f2ad3453ae7a02008dcef3d7b463a15b58aaa498419c3f83b57581d1106d`.
All diagnostic flags remain default-off; installed provider and validated
1:1 scaler archive unchanged.

## Follow-up: changing storage to RGBA6 does not remove the fault

RGBA6/Z24 retains the non-multisampled mode and depth precision of RGB8/Z24.
RGB565 was deferred because libogc pairs it with multisampling. Exact black and
white outputs avoid expected-color quantization ambiguity, but RGBA6 is still
lower storage precision: its expanded raw values are labeled accordingly.
The diagnostic copy path explicitly retains the chosen format in its sync
command; the normal wrapper keeps its original RGB8 control.

| Run / case | Result | Raw / copy errors |
|---|---|---:|
| r1 RGBA6 forced white from black | Failed second iteration | 2 / 2 |
| r1 RGB8 forced white from black | Failed second iteration | 4 / 4 |
| r1 RGBA6 forced black from white | Failed fifth iteration | 1 / 0 |
| r1 RGB8 forced black from white | Failed fourth iteration | 1 / 1 |
| r2 RGBA6 forced black from white | 16/16 PASS | 0 / 0 |
| r2 RGBA6 forced white from black | Failed first iteration | 3 / 3 |
| r2 normal full render UAPI regression | PASS | Client checks |

RGBA6 first discrepancies: (34,66) ffffff → ffff7d; (171,82) 000000 → 000004;
repeat white output (32,144) ffffff → ffefff. Backgrounds passed, repeated reads
retained the values, and the visible errors appeared in RGB565 copies. The
short clear pass does not negate its earlier failure. Reduced precision is
not a demonstrated fix or proof of a particular physical fault.

Both manifests verified: `wii-gcn-matrix-rgba6-20260912-r1` and `-r2` under
`/media/anolis/dev/`. W=1 build, all 31 audit tests and diff checks pass, as does
the full normal rendering regression covering the shared copy-helper change.
RGBA6 archive `/media/anolis/dev/wii-gcn-rgba6-module.ko`, SHA256
`ae9fb17229e60e627a8f1371c03545362ca6e0d944b973a4971640c9e7553f13`.
All new diagnostic flags remain default-off. Installed provider and validated
1:1 scaler archive unchanged. Reference for pixel-format/sample coupling:
[libogc GX_SetPixelFmt](https://github.com/devkitPro/libogc/blob/master/libogc/gx.c).

## Follow-up: recurring pixel/bit patterns and a focused probe

The reproducible corpus now includes 103 distinct verified first-pixel captures
at 69 locations. Nineteen locations recur; 18 retain exactly the same XOR mask
across all appearances. The exception spans RGB8 and RGBA6 bit layouts. No
captures were excluded for failed verification or duplicate kernel logs.
The corpus verifies each used result/kernel against its manifest and recorded
kernel hash. These deliberately selected, first-error-censored captures do not
support unbiased rates, complete spatial distributions or a physical diagnosis.

Repeated (530,54)/blue-bit7 failures motivated two 8x2 SET quads covering
x528..535/y52..55. Each iteration verifies black background, writes 32 white
pixels and checks all 307168 surrounding pixels remain black. The full image
is scanned by both raw RGB24 and RGB565 oracles. The probe uses one submission
and 499 bytes, compared with 30 submissions/full-screen work in its control.

| Case | Short | Extended |
|---|---|---|
| Focused recurrent-site probe | 16/16 PASS | 1000/1000 PASS, zero raw/copy errors |
| Full-screen SET control | Failed first iteration | Failed first iteration |

The extended probe completed in 159.24 seconds, checked 307200000 pixels per
oracle and actively rewrote the recurrent site 1000 times. The full workload
then failed at (605,296), ffffff → 7fffff, with two raw/two copy errors. This
shows the known site can survive repeated small GPU writes while broader work
still corrupts pixels. It supports context-dependent behavior over a simple
permanently stuck-bit explanation, without proving the hardware healthy or
identifying the responsible context. This is not a general rendering fix.

The next useful variable is primitive width with the same two rows/batch count:
widen the focused probe while retaining its physical target and checked image.
That follow-up is implemented below.

Evidence: `wii-gcn-matrix-focus-20260912-r1`, `wii-gcn-matrix-focus-20260912-long`,
and `wii-gcn-fault-corpus-20260912-final` under `/media/anolis/dev/`. Manifests
verified; W=1 build, all 32 audit tests and diff checks pass. Corpus source is
archived with its outputs for reproducibility. Small-probe build/archive:
`/media/anolis/dev/wii-gcn-focus-module.ko`, SHA256
`ed03e28e52a36c4546b1482781f8f87533b658b4a6b6d9fdfe411f92bc8047c9`.
Diagnostic flags remain default-off; installed provider and validated scaler
archive unchanged. Cleanup confirmed module unloaded and matrix lock released.

## Wide two-primitive reproducer

The new `focus-wide-530-54` case changes only the focused rectangles' left
edge from 528 to 0. The right edge stays 536, preserving the recurrent pixel's
position relative to that edge. Rows 52..55, two quads, one submission,
499 command bytes, full viewport, logic SET and verified black background
remain the same. Coverage grows from 32 to 2144 white pixels; all remaining
305056 pixels must remain black.

The first comparison passed 16/16 for both focused widths, while the full
SET control failed its first iteration (7 raw, 4 copy errors). The extended
wide run then failed iteration 1, after one clean iteration: exactly (530,54),
ffffff → ffff7f, with one raw and one copy error. All three repeat peeks agreed;
the pre-draw background remained clean. The full control then failed its first
iteration (1 raw, 1 copy error). Both suite manifests verified.

This reproduces the recurrent bit fault with only two primitives in one
submission. Full-screen draw volume and 30 submissions are therefore not
necessary. Widening also changes covered pixels and raster work, so the
comparison does not isolate numerical width from all consequences of width.
The original tiny probe's 1000-pass result does not establish immunity.

Evidence: `/media/anolis/dev/wii-gcn-matrix-focus-wide-20260912-r1` and
`/media/anolis/dev/wii-gcn-matrix-focus-wide-20260912-long`. Current diagnostic
archive: `/media/anolis/dev/wii-gcn-focus-wide-module.ko`, SHA256
`2da323dbb81596f0583c3043f5693c166707427a4d83dbc716ea7872182242bb`.
W=1 build and 33 audit tests pass, including real-capture coverage mismatch
checks. The underlying thin-primitive fault remains unresolved.

The reversed-order repeat (`wii-gcn-matrix-focus-wide-20260912-r2`) passed
1000/1000 tiny-probe iterations (159.21 seconds, 307200000 pixels per oracle),
then failed the wide probe's first iteration at the identical (530,54),
ffffff → ffff7f. Again one raw/one copy error, clean background and three
agreeing peeks. Its manifest verified. This reproducer should now replace
full-screen draws for initial discriminating experiments; left-edge/width
steps can test whether specific span ranges trigger the recurrent fault,
without assuming a monotonic threshold from intermittent failures.

Final checks: all 34 audit tests pass and `git diff --check` is clean. Remote
module unloaded, matrix lock released, boot UUID and printk unchanged, and
installed provider SHA256 remains
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
The validated 1:1 scaler archive remains untouched. No general thin-primitive
fix or physical hardware diagnosis is established.

## Intermediate focused-width sweep

Added the bounded `efb_primitive_focus_width` parameter (8..536, default 8).
The existing wide flag remains compatible; conflicting nondefault width plus
wide flag is rejected. All widths end at x536 and cover y52..55 with two
quads, one submission and 499 command bytes. Exact coverage is width times
four white pixels, with all other pixels checked black, by both full-image
oracles. Thus this changes span and left edge together, preserving the right
edge and recurrent target. It does not isolate span from covered addresses
or total raster work.

The 64-iteration screen used order 536,256,64,16,512,128,32,8 to avoid a
simple association between width and test time. Results:

| Width | Result | Clean iterations before stop | First pixel |
|---|---|---:|---|
| 8 | PASS | 64/64 |  |
| 16 | PASS | 64/64 |  |
| 32 | PASS | 64/64 |  |
| 64 | PASS | 64/64 |  |
| 128 | PASS | 64/64 |  |
| 256 | FAIL | 48/64 | 512,52 |
| 512 | FAIL | 2/64 | 302,54 |
| 536 | FAIL | 0/64 | 530,54 |

Screen evidence: `/media/anolis/dev/wii-gcn-matrix-focus-width-20260912-r1`,
manifest verified. Each failure had one raw and one copy mismatch, with clean
background. These first-error-censored runs do not establish a failure rate
or a monotonic safe-width threshold.

The longer follow-up already invalidated a 128-pixel safe-boundary inference:
width 128 failed after 182 clean iterations at (512,52), one raw/one copy
error. This is the same first coordinate as the width-256 screening failure.

Current diagnostic archive: `/media/anolis/dev/wii-gcn-focus-width-module.ko`,
SHA256 `a131b0e6749a734b66f4f284cff9584822b7ba055620ddd6be08254da83edb1a`.
W=1 build and 35 audit tests pass, including real intermediate-width captures
and rejection of mismatched geometry/coverage.

Completed longer follow-up (`wii-gcn-matrix-focus-width-20260912-long`):
width 64 passed 1000/1000 iterations, zero raw/copy errors, 155.32 seconds
and 307200000 checked pixels per oracle. The following width-256 control
failed after 62 clean iterations, again at (512,52), with one raw/one copy
error. The long suite manifest verified. Width 64 is a passing observation
for this two-rectangle fixture, not a generally validated workaround.

A useful next discriminator is to split the failing 128-pixel span into
adjacent 64-pixel quads while preserving its total coverage and submission.
That can compare primitive span against total written pixels/addresses; it
will change primitive count and command length, which must be audited.
That experiment is implemented below.

Final cleanup confirmed the same boot UUID and printk, module unloaded and
matrix lock released. Installed provider hash remains unchanged. No driver
deployment or general thin-primitive fix has been made.

## Equal-coverage horizontal subdivision

Added default-off `efb_primitive_focus_split`. For width 128 it divides each
x408..535 strip at x472, retaining y52..55 and the complete 512-white-pixel/
306688-black-pixel oracle. There are now four quads/595 bytes versus two
quads/499 bytes, still one submission. The pieces are emitted row-major.
This controls total coverage and submission count, while primitive count,
command length and raster traversal change along with primitive span.

`wii-gcn-matrix-focus-split-20260912-r1`: split 128 passed 1000/1000
iterations (307200000 pixels per oracle); the unsplit 128 control then failed
after 80 clean iterations at (512,52), with one raw/one copy error. Suite
manifest verified. This is evidence for testing subdivision further, not a
general thin-primitive fix. The archived module for this exact experiment is
`/media/anolis/dev/wii-gcn-focus-split-module.ko`, SHA256
`9a1ebd2e6590931d2ee171cebb356e36b99572c7bb6ca9747f07f7e967170e34`.

The subsequent build generalizes the same flag to the wide 536-pixel focus
case: each strip has eight 64-pixel quads and one 24-pixel tail, 18 quads
total, 1267 bytes, one submission. Coverage remains 2144 white and 305056
black pixels, including both recurrent fault locations. Width-128 geometry
and its marker remain unchanged. Current diagnostic archive:
`/media/anolis/dev/wii-gcn-focus-split-wide-module.ko`, SHA256
`efc64f3c3c573bd824e0d5500b537aa35dfdeb8d2f2fafb1804faa82c43a5d73`.
W=1 build and 36 audit tests pass, including real split-pass capture checks
for marker, authored bytes, total coverage and unsplit-case rejection.

`wii-gcn-matrix-focus-split-wide-20260912-r1`: the wide split passed
1000/1000 iterations in 160.37 seconds (307200000 pixels per oracle). Its
unsplit wide control then failed after three clean iterations at (530,54),
ffffff → ffff7f, one raw/one copy error, clean background and three agreeing
repeat peeks. Manifest verified. Both tested equal-coverage subdivision
comparisons therefore passed while their unsplit controls reproduced faults.

The next necessary generalization is full-screen thin-strip subdivision;
the following experiment implements it. These two focused passes do not
validate a general rendering fix. Final checks: 37 audit tests pass, W=1
build and diff checks clean, module unloaded, lock released, boot/printk and
installed provider hash unchanged. The validated 1:1 scaler archive remains
untouched.

## Full-screen two-row subdivision

Added default-off `efb_primitive_full_split`. The full-screen SET-from-black
case retains all 30 submissions and eight degenerate padding quads per
batch. Each of the eight two-row strips per batch becomes ten adjacent
64x2 quads. Thus each batch has 88 quads instead of 16, 4627 first-batch
bytes/4232 subsequent bytes, with the same complete 307200-pixel written
image and background/readback oracles. The 64 KiB FIFO accommodates these
commands. Primitive count and traversal change, so this is not an isolated
measurement of width alone.

`wii-gcn-matrix-full-split-20260912-r1`: full-screen subdivision passed
1000/1000 iterations, zero raw/copy errors, 164.99 seconds, 30000 submissions
and 307200000 pixels per oracle. The unsplit SET control then failed on its
first iteration with two raw/one copy error. Manifest verified. Exact archive:
`/media/anolis/dev/wii-gcn-full-split-module.ko`, SHA256
`63cefdd9299cc82d4b906c2c79f6f9eab0968537e1e9d8b6aeeac4940796de17`.

The subsequent build also admits ordinary two-row rendering and alternating
red/white strip colors, preserving their original oracle. Cases are
`full-split-color-64`, `full-split-striped-64`, with unsplit `two-rows` and
`two-rows-striped-control`. This checks the forced-SET result against direct
vertex-color rendering and a nonuniform image. Current diagnostic archive:
`/media/anolis/dev/wii-gcn-full-split-color-module.ko`, SHA256
`a10586a716d0097e3fac08294406a9d894fc8d908cf437156891b235f2b59077`.
W=1 build and 38 audit tests pass, including the full-screen real capture's
30000 batch records and rejection of reduced coverage/incorrect commands.

`wii-gcn-matrix-full-split-color-20260912-r1`: ordinary-color subdivision
passed 1000/1000 iterations, zero raw/copy errors, 163.15 seconds, 30000
submissions and 307200000 pixels per oracle. Its unsplit `two-rows` control
failed immediately at (591,302), one raw/one copy error. The subsequent
striped case was rejected at probe time with -EINVAL because the historical
stripe-colors guard required an extended viewport. The host correctly marked
ERROR (missing test start) and stopped the suite before the striped control.
The suite manifest verified; the ERROR is not a rendering pass or failure.

The guard now admits standard-viewport ordinary two-row strip colors,
excluding odd/interleaved/alternate/focused geometry. The corrected striped
comparison runs separately under `wii-gcn-matrix-full-split-striped-20260912-r1`.
No draw state or geometry changed in this correction. Current archive:
`/media/anolis/dev/wii-gcn-full-split-striped-module.ko`, SHA256
`7a9181e8620f51f038cbf5f4db71a839ef1eebd25c6f868a58383aaf3befab8f`.

Corrected striped results: 1000/1000 PASS, zero raw/copy errors, 164.63
seconds, 30000 submissions and 307200000 pixels per oracle. Its unsplit
striped control failed immediately at (605,296), three raw/two copy errors.
The corrected suite manifest verified. Full-screen subdivision therefore
passed all three tested images (SET white, ordinary red/white, alternating
two-row strips), totaling 3000 clean iterations and 921600000 pixels per
oracle across those split cases. Each corresponding unsplit control failed
on its first iteration. These selected runs do not establish a general
failure probability or prove all rendering fixed.

Next work should carry the 64-pixel subdivision into a diagnostic textured
scaler path and validate nonidentity scaling with the exact source/UV oracle;
one-row and other geometry also remain outside these two-row results. The
normal driver has not been changed to enable subdivision. No installed
provider replacement occurred. Final cleanup: module unloaded, matrix lock
released, same boot UUID/printk and installed provider hash. W=1 build, all
39 audit tests and diff checks pass.

## 2026-09-13: bounded textured native-span experiment

To respect the Wii's 64 KiB FIFO, the textured final stage is subdivided
into three 80-row batches. Width 64 needs 1200 textured quads/96024 total
command bytes, 32008 bytes per batch. Matched width 160 retains the original
480 quads but uses the same three submissions (38424 total bytes). The
one-batch baseline uses 38408 bytes. This comparison controls submission
count while varying span and vertex count; the baseline also tests whether
batching alone matters. No extra texture or EFB allocation is introduced.

This is restricted to the existing 1:1 native trace predicate, with the
validated identity shortcut disabled only for these diagnostic cases. The
existing S/T phase calculations are retained. It does not yet establish
correctness for nonidentity scaling or one-row direct-color rendering.

`wii-gcn-matrix-native-span-20260913-r1`: both spans passed 16/16 frames,
64 sequences each, with complete stage/prior/command audits and no errors.
Manifest verified. Trace spans were 26.37 seconds (64) and 22.50 seconds
(160); these include diagnostic readback and are not production benchmarks.
The longer comparison reverses order (160,64,original baseline), capped at
256 frames each and first mismatch.

Current diagnostic archive: `/media/anolis/dev/wii-gcn-native-span-module.ko`,
SHA256 `c9242a56a45153ce9bcfdfebd2b966261c48eb868cd36d9e9475bdc9d4e2bf08`.
W=1 build and 40 audit tests pass, including real native-span captures and
rejection of incorrect batch bounds, spans and command budgets.

Extended matched comparison: width 160 failed after 217 clean frames
(872 sequences captured), first bad sequence 869, origin 0,0, final stage
at (60,128): expected f800, actual b800, raw ARGB 00bf0000. There was one
final raw/output-precision mismatch and one copied mismatch; all prior
preservation and crop/horizontal stage checks were clean. Width 64 then
passed all 256 frames / 1024 sequences with zero raw/copy/prior errors.
Both cases use three final-stage batches, so batching alone did not eliminate
the fault in the matched wider case. This comparison still changes primitive
count/vertex traffic together with span.

Resource totals are now exposed by the native audit as final draw submission
count, total authored final command bytes and maximum final batch bytes.
This accounting was added while the extended suite was already running;
its original archived result schema is preserved, with any re-audit recorded
separately.

The original one-batch baseline also passed 256/256 frames in this sweep.
This must be retained alongside the matched width-160 failure: it prevents
a claim that the original baseline invariably fails or that extra batching
has a monotonic benefit. The matched three-batch comparison remains
64 PASS256 versus 160 FAIL after217. Suite manifest verified.

A separate re-audit with resource accounting is archived at
`/media/anolis/dev/wii-gcn-native-span-costs-20260913`, with the audit source,
input hashes, results and SHA256SUMS; original suite captures are unmodified.
For 1024 sequences, width64 uses 3072 final submissions and 98328576
authored final bytes versus baseline1024 submissions and39329792 bytes.
Maximum final batch sizes are32008 and38408 bytes respectively. The matched
width160 has three12808-byte batches per sequence. No additional texture
allocation is introduced; extra commands and waits are real costs.

Final checks: 40 audit tests and diff checks pass, W=1 module build clean.
Remote module unloaded, lock released, boot/printk unchanged and installed
provider hash unchanged. Next meaningful validation is nonidentity textured
scaling with bounded streams; this native1:1 experiment is not a general fix
and does not replace the already validated identity-quad shortcut.

## 2026-09-13: nonidentity offset enlargement

The next comparison uses the existing tiled RGB565 255x79-to-256x79 offset
enlargement oracle. It samples a nonuniform source at y43 into destination
y97 and verifies all 65536 destination pixels, plus source integrity and
MEM1 recovery. Subdivision affects only the final stage; horizontal nearest
sampling is unchanged. Four 64-pixel or two 128-pixel quads per row give
316 or 158 quads in one submission, safely below the 64 KiB FIFO. This
avoids the extra-wait confound of the larger native experiment. No extra
texture buffers are allocated.

The client now supports an explicit 1..1000 repeat selector; it reuses its
existing objects across iterations instead of nesting another loop around
the old 2000-iteration loop. The old selector retains its old behavior.

`wii-gcn-matrix-offset-span-20260913-r1`: both widths passed16/16 iterations
with complete crop/horizontal/final and client audits. Manifest verified.
Current diagnostic module archive `/media/anolis/dev/wii-gcn-offset-span-module.ko`,
SHA256 `86b71ee530dc9516d998a6d2f50cb18f9d7591fabcc12b4e3aaa5e729ffc683b`.
Client `/media/anolis/dev/wii-gcn-offset-span-clients/wii-gcn-render-test`,
SHA256 `c0a15013946fca8677ddad1178ea6e7c6499d045fc6a59acb8ef5337ca702c3b`.
W=1 module build, cross-client build,41 audit tests and diff checks pass.

`wii-gcn-matrix-offset-span-20260913-long` reversed the order (128,64):
both passed 1000/1000 iterations with zero crop/horizontal/final mismatches.
Manifest verified. Each final pass checked 65536000 destination pixels
over the run, including preservation outside y97..175; client checks also
verified source integrity and MEM1 recovery. This validates compatibility
with this nonidentity sampling case, not a reliability improvement because
the wider control also passed.

Measured authored final stream sizes were 26933 bytes for width64 and
14293 for width128 (includes identical state setup), one submission each.
The narrower case therefore costs an additional12640 bytes per operation
without additional texture memory or waits here. A broad always-on change
is not justified by this passing compatibility case. Larger enlargement
ratios and reductions remain to be validated, with explicit FIFO batching
where required.

The full render UAPI regression passed with subdivision disabled, using
the same module and updated client (`wii-gcn-matrix-offset-span-20260913-regression`).
Its manifest verified. Final cleanup confirmed module unloaded, matrix lock
released, boot UUID and printk unchanged, and installed provider SHA256
unchanged. All41 audit tests and diff checks pass; no deployment was made.

## 2026-09-13: full-screen 2x system enlargement

The bounded textured test now covers linear RGB565 320x240-to-640x480
scaling. Existing source/intermediate/final oracles and horizontal rendering
are retained. The final stage uses six batches, each mapping 40 source rows
to 80 destination rows using 64x2 or 320x2 quads. Both variants have identical
wait/batch count. The narrow variant uses 400 quads per batch, wider80;
only the first batch carries setup state. All buffers are reused from the
existing scaler. Command-size guards check FIFO bounds before emission.

A new bounded client selector repeats the established full-system linear
layout test and its all-pixel/MEM1 checks. `wii-gcn-matrix-system-span-20260913-r1`
passed16/16 for both variants; manifest verified. Long order is320 then64,
up to1000 iterations each. Current diagnostic archive:
`/media/anolis/dev/wii-gcn-system-span-module.ko`, SHA256
`fed1305862511da9578efb4b5c70ec8fabe6250fed0be75d1666868334f8e1ee`.
Client `/media/anolis/dev/wii-gcn-system-span-clients/wii-gcn-render-test`,
SHA256 `6d80a4bda9f89b64d1a74900cb3929035c3404479206be4e214f88397448886a`.
W=1/cross-client builds,42 audit tests and diff checks pass.

Resource accounting: final maximum batch32974 versus7374 bytes; total
final bytes193014 versus39414 per enlargement (64 versus320 spans).
Both have six final submissions, five more than the original unbatched
final path. These are material costs, not production performance timings.

Extended results (`wii-gcn-matrix-system-span-20260913-long`): both widths
passed 1000/1000 iterations, with zero crop, horizontal and final output
mismatches. Each case checked 307200000 final pixels, 6000 final batches
and full client pixel/MEM1 recovery checks. Manifest verified. This is
compatibility evidence for full-screen2x enlargement, not a reliability
improvement: the wider matched control also passed. The source pattern is
fixed per iteration, so this does not cover arbitrary content or ratios.

The next discriminating extension is reduction, whose final sampling
produces one-row runs rather than the two-row runs in this2x enlargement.
Do not infer a safe general64-pixel width or enable subdivision everywhere
from these passing enlargement tests.

The default full render regression passed with the same module/client and
subdivision disabled (`wii-gcn-matrix-system-span-20260913-regression`);
manifest verified. Final cleanup confirmed module unloaded, matrix lock
released, unchanged boot/printk and unchanged installed provider hash.
W=1/client builds,42 audit tests and diff checks pass. No deployment made.

## 2026-09-13: one-submission textured reduction

Added diagnostic final spans64/160 to the existing exact640x240-to-320x120
reduction trace. This exercises one-row final runs without introducing
additional waits:600 narrow quads or240 wider quads fit in a single FIFO
stream (48974/20174 bytes including state). The horizontal stage, nonuniform
source pattern and odd-coordinate nearest sampling remain unchanged.

The bounded client selector `--reduce-repeat N` runs the existing exact
all38400-pixel reduction check. Raw EFB comparisons are at RGB565 precision,
with source errors and EFB/copy differences counted separately. An unknown
copy error count is left null; the client still checks all output pixels.

`wii-gcn-matrix-reduce-span-20260913-r1`: both variants passed16/16;
manifest verified. Current archive `/media/anolis/dev/wii-gcn-reduce-span-module.ko`,
SHA256 `07046b7e7517a70b4189201dd1f924c885c23694746f509adfd7b3495eade704`.
Client `/media/anolis/dev/wii-gcn-reduce-span-clients/wii-gcn-render-test`,
SHA256 `d94fc9fb17489bd3c40d589f83f14d77796d8daca19c4dd76f48aaf005f9b80f`.
W=1/client builds,43 audit tests and diff checks pass.

`wii-gcn-matrix-reduce-span-20260913-long` reversed the order (160,64).
Both passed 1000/1000 iterations, zero raw source mismatches and zero
EFB/copy differences. Each run checked 38400000 final pixels; the client
also passed its complete copied-output oracle. Manifest verified.

Together, the offset enlargement, full-screen 2x enlargement and 2x reduction
are passing compatibility checks at both tested widths. They do not show a
reliability benefit for subdivision in those specific patterned fixtures.
The demonstrated benefits remain scoped to earlier direct-color thin-strip
comparisons and the matched native trace comparison. Do not replace the
validated identity shortcut or enable costly subdivision globally on this
evidence. A useful next step is varying source content in a fixed geometry
with paired span controls, rather than repeating these already-passing
fixed-pattern scaling cases.

Default full render regression passed using the same module and updated
client with subdivision disabled (`wii-gcn-matrix-reduce-span-20260913-regression`);
manifest verified. Final cleanup confirmed module unloaded, matrix lock
released, unchanged boot/printk and installed provider hash. W=1/client
builds, 43 audit tests and diff checks pass. No deployment made.

## 2026-09-13: source-content variation at fixed reduction geometry

No driver change: reuse reduction module SHA256
`07046b7e7517a70b4189201dd1f924c885c23694746f509adfd7b3495eade704`.
The new client cycles black, white, RGB primaries, checkerboard, walking-bit
and deterministic mixed source data, retaining the exact640x240-to-320x120
geometry and one submission. The phase/seed changes across cycles, and
both widths receive the same ordered schedule. The first16-iteration suite
(`wii-gcn-matrix-reduce-content-20260913-r1`) passed both widths and its
manifest verified. The longer reverse-order comparison has a1000-iteration
ceiling per case,125 uses of each pattern on complete runs.

Client archive `/media/anolis/dev/wii-gcn-reduce-content-clients/wii-gcn-render-test`,
SHA256 `c87d28a4228ecf8f516981ca0b2ab9cf1522f8606d8e2f494cff658de11f0824`.
The cross-client build and44 audit tests pass, including real captures
checking exact pattern/seed schedules. An initial client compilation error
in the refactored default call site was corrected before hardware testing.
The existing default regression still selects its original ramp source.

`wii-gcn-matrix-reduce-content-20260913-long`: both widths passed 1000/1000
iterations, all125 cycles, with zero raw EFB source mismatches and zero
EFB/copy differences. The client passed every copied-output check. Each
case checked38400000 output pixels; the verified pattern schedule contains
125 instances of each family. Manifest verified. This shows that these
content families do not by themselves trigger the fault in this fixed
reduction geometry; it does not show that content is irrelevant elsewhere.

Further repetitions of this passing reduction are now low value. Return
to a previously failing geometry and vary placement/extent or compare the
same content schedule there, keeping the successful reduction as a control.
The reduction ends at y119 and occupies x0..319, unlike the larger native
and full-screen workloads; its passes cannot establish correctness over
the rest of EFB. No general driver change is justified by this sweep.

Full standard render regression passed with the updated client and unchanged
module (`wii-gcn-matrix-reduce-content-20260913-regression`); manifest verified.
Final cleanup confirmed module unloaded, lock released, same boot/printk
and installed provider hash. Client build,44 audit tests and diff checks
pass. No installed-provider replacement or default driver change was made.

## 2026-09-13: vertical placement of the compact failing probe

Return to the536x4 focused SET-from-black probe. A bounded top-row
parameter moves its two536x2 quads without changing width, color, viewport,
coverage, two-quad count, one submission or499 bytes. The original top52
is compared with53,60,180. All307200 pixels remain checked by raw RGB24
and RGB565 oracles, including all surrounding black pixels.

`wii-gcn-matrix-focus-placement-20260913-r1` (64-iteration ceiling): original
top52 failed after one clean iteration at (530,54), one raw/one copy error.
Top53 failed after14 clean iterations at (302,54), one raw/one copy error.
Top180 and top60 both passed64/64. Manifest verified. Thus a one-row shift
does not remove the fault, and the observed first errors stayed on physical
row54 while its relative position within a rectangle changed. This does
not prove a faulty physical row or that all other rows are immune.

Longer order is60,180,52 with a1000-iteration ceiling per case. Archive
`/media/anolis/dev/wii-gcn-focus-placement-module.ko`, SHA256
`f2de5c69bbbe050ff190c9169d6a311fbe11f9ae90995d545f9a39eb72ffe535`.
W=1 build and45 audit tests pass, including verified real shifted captures
and rejection of mismatched top-row/coverage metadata.

Extended placement results (`wii-gcn-matrix-focus-placement-20260913-long`):

| Top row | Result | Clean iterations | First mismatch |
|---|---|---:|---|
| 60 | FAIL | 717 | (475,63), ffffff → fffbff |
| 180 | PASS | 1000 | none |
| 52, repeated control | FAIL | 9 | (530,54), ffffff → ffff7f |

Both failures had one raw/one copy error, with three agreeing repeat peeks.
The top-180 run checked 307200000 pixels per oracle in 162.11 seconds.
Manifest verified. Moving away from row54 therefore does not eliminate
the fault; the top-60 short pass was overturned by longer testing. The
odd-shift top53 failure on row54 also means even primitive boundaries are
not necessary for that observed fault. These selected, first-error-censored
runs do not establish comparative failure probabilities, a permanently bad
physical row, or an immune region.

A useful follow-up is to hold geometry and known failing coordinates fixed
while shifting scissor/viewport coordinates together, or otherwise vary the
rendering path without changing physical coverage; any such test must
account explicitly for changed transform/state. This is a proposed next
experiment, not yet implemented. Repeatedly extending passing locations
alone would be less discriminating.

Final checks: W=1 module build,45 audit tests and diff checks pass. Module
unloaded, matrix lock released, same boot/printk and installed provider
hash. No default scaler behavior or installed provider was changed.

## 2026-09-13: compensated viewport at the failing physical location

Move viewport origin from0 to32 while moving vertex top from52 to20,
keeping physical coverage x0..535/y52..55. A matched control rewrites the
original viewport at0. Both retain size640x480, physical scissor, color,
two quads and one528-byte submission. Full RGB24/RGB565 oracles validate
2144 white pixels and305056 preserved black pixels. This changes XF state
and vertex coordinates, but not intended screen-space coverage.

First64-ceiling comparison (`wii-gcn-matrix-focus-viewport-20260913-r1`):
compensated case failed after37 clean iterations, rewrite control after10,
original499-byte control after3. All first errors were at(530,54). Reversed
repeat (`wii-gcn-matrix-focus-viewport-20260913-r2`): rewrite control failed
after13, compensated after27, again at(530,54), ffffff → ffff7f, with
agreement among repeated peeks. Both manifests verified. These results
reject this compensated viewport as a workaround; they do not establish
the precise internal stage or a physical hardware defect.

Archive `/media/anolis/dev/wii-gcn-focus-viewport-module.ko`, SHA256
`7c00a4151197da55f2c097623f050648a844fb5866bde29a90ec31c55527ad48`.
W=1 build,46 real-capture audit tests and diff checks pass. Final cleanup
confirmed module unloaded, lock released, same boot/printk and installed
provider hash. No default behavior or installed driver change.

A next useful control for the successful subdivision is matching its vertex
and command count with degenerate padding plus the original wide quads.
That can test whether extra command traffic/primitive count alone explains
the subdivision result while preserving physical coverage and submissions.
It has not yet been implemented or tested.

## 2026-09-13: command-count matched padding versus subdivision

Added16 zero-area prefix quads to the two original536x2 quads. This matches
the successful wide subdivision's18 quads/72 vertices/1267 authored bytes
and one submission, while retaining the same2144 written pixels and full
black-surround oracle. Default behavior is unchanged.

`wii-gcn-matrix-focus-padding-20260913-r1` (64-iteration ceiling): padding
control failed after17 clean iterations at(530,54), two raw/one copy
errors. Subdivision passed64/64. Original unpadded control failed after12
clean iterations at(530,54), one raw/one copy error. Manifest verified.
This rejects extra authored command volume/primitive count alone as a
sufficient workaround in that comparison. It does not match internal
fragment generation or isolate width from traversal and nondegenerate
primitive count.

Long comparison reverses order: subdivision then padded control, ceiling
1000 per case. Current archive `/media/anolis/dev/wii-gcn-focus-padding-module.ko`,
SHA256 `9b75d3f4dcc4e4975152308c576a42a4996fecd4e13071fb3025d1b7a2d167a8`.
W=1 build,47 audit tests and diff checks pass, including real-capture
checks of equal command budgets and distinct padding/subdivision markers.

Extended reversed-order result (`wii-gcn-matrix-focus-padding-20260913-long`):
subdivision passed 1000/1000 in 159.86 seconds, zero raw/copy errors and
307200000 pixels per oracle. Padded wide control then failed after 14 clean
iterations at (530,54), ffffff → ffff7f, with two raw/one copy errors and
three agreeing peeks at the first mismatch. Manifest verified.

The two matched runs strengthen the evidence that simply adding vertex
traffic or padding is insufficient. The successful subdivision changes
how nondegenerate primitives cover the pixels. It remains inappropriate
to identify one internal hardware mechanism or claim a universal fix.
No new texture/EFB buffer allocation, waits or production defaults were
introduced by this diagnostic control. Final cleanup confirmed module
unloaded, matrix lock released, same boot/printk and installed provider
hash. W=1 build,47 audit tests and diff checks pass.

## 2026-09-13: subdivision survives reversed and column-major order

Added diagnostic `efb_primitive_focus_order`: 0 retains row-major emission,
1 reverses the entire list, and 2 emits both rows of each column before
advancing horizontally. The same 18 quads cover x0..535/y52..55 in every
order: 2144 white pixels, 305056 preserved black pixels, 1267 command bytes
and one submission. No additional buffers or waits are introduced.

Screening suite `wii-gcn-matrix-focus-order-20260913-r1` passed 64 iterations
for reverse, column-major and row-major orders. The padded-wide control
failed after 5 clean iterations at (530,54), with two raw/one copy errors.
All 64 row-major command hashes exactly matched the previous padding
screen's `focus-split-536` capture, confirming preservation of that stream.

Long suite `wii-gcn-matrix-focus-order-20260913-long`:

| Case | Result | Raw/copy errors | Time |
| --- | --- | --- | --- |
| Column-major subdivision | PASS 1000/1000 | 0/0 | 159.39 s |
| Reverse subdivision | PASS 1000/1000 | 0/0 | 160.32 s |
| Padded-wide control | FAIL after 14 clean iterations | 2/1 | 2.35 s |

Each passing case checked 307200000 pixels per oracle. The control's first
error was again (530,54), white `ffffff` becoming `ffff7f`, with three
agreeing peeks and copied RGB565 `ffef` instead of `ffff`. The runner's
nonzero suite exit reflects this expected diagnostic control failure, not
an infrastructure error. Both evidence manifests verified.

Together with the previous row-major 1000-pass runs, these observations
show that the successful subdivision is not limited to one tested draw
order. They strengthen subdivision as a scoped workaround, but do not
isolate primitive width from every consequence of subdivision or establish
a universal fix. First-error-censored controls do not measure failure rate.

Archive `/media/anolis/dev/wii-gcn-focus-order-module.ko`, SHA256
`995942a336b2e65523ccb1ede2010706d964b86ebe9b237f26b461125c2942bb`.
W=1 build, 48 audit tests and diff checks pass. Cleanup confirmed module
unloaded, matrix lock released, unchanged boot/printk and installed provider
hash. Default rendering and the installed driver are unchanged.

## 2026-09-13: wider subdivision pieces at fixed coverage

Added diagnostic `efb_primitive_focus_span` with default 64 and alternatives
96, 128 and 256. The wide focus case retains its physical coverage,
row-major emission and one submission, clipping each row's last piece to
x536. This compares 12, 10 and 6 quads (979, 883 and 691 bytes) against
the existing 18-quads/1267-byte subdivision and two-quads/499-byte unsplit
control. No extra memory allocation or waits; production behavior is
unchanged. Piece boundaries and primitive count change along with width.

Screen `wii-gcn-matrix-focus-span-20260913-r1`: widths 256, 128, 96 and 64
each passed 64 iterations. The unsplit wide control failed after 40 clean
iterations, with one raw/one copy error. Manifest verified. The default
64-pixel command hashes match the prior order screen exactly. Added
real-capture audit checks for selected span/boundary, full coverage and
command budgets; all 49 audit tests pass, as does the W=1 module build.

Archive `/media/anolis/dev/wii-gcn-focus-span-module.ko`, SHA256
`97312e59f949b1b03145489a1578afa069d9824a7d5aef3d230f944a7aed1ac0`.
The reversed-width long suite `wii-gcn-matrix-focus-span-20260913-long`
tests 96, 128, 256 and the unsplit control with a ceiling of 1000 each.

Interpretation constraint: left-anchored subdivision leaves a final piece
of 56 pixels for span 96, and 24 pixels for spans 64, 128 and 256. Thus
the recurrent pixel (530,54) lies in a short piece in every subdivided
case. Even a 256-span pass would not establish that this pixel tolerates
a 256-pixel primitive. Moving the remainder to the left while retaining
coverage, piece count and command budget is the next discriminating
control; this width sweep alone cannot separate those effects.

The long width sweep passed 1000/1000 for all three variants: span 96 in
157.82 seconds, span 128 in 159.02 seconds, and span 256 in 153.74 seconds.
Each checked 307200000 pixels per oracle with zero errors. The unsplit
control then failed after 12 clean iterations at (530,54), with one raw
and one copied-pixel error. The complete manifest verified. The scoped
six-quad result uses 691 rather than 1267 authored bytes, but the short
tail constraint above prevents recommending a universal 256-pixel limit.

## 2026-09-13: move the subdivision remainder to the opposite end

Added diagnostic `efb_primitive_focus_right` to anchor the grid at x536.
The short remainder is emitted first at the left edge, followed by full
pieces in the same row-major order. Coverage, piece count, width multiset,
command bytes and submissions match each corresponding left-anchored
case. Markers and audits explicitly identify the anchor, remainder and
first boundary. Production defaults are unchanged.

Screen `wii-gcn-matrix-focus-right-20260913-r1`: right-anchored spans 256,
128, 96 and 64 each passed 64 iterations, followed by a left-anchored 256
control which also passed 64. Its command hashes exactly match the prior
width screen. Manifest verified. The right-anchored 256 case now includes
full pieces x280..535, matching the earlier standalone width-256 probe
which failed at (512,52). Unlike that standalone probe, other visible
pieces precede these draws. This is an additional contextual difference,
not proof of a particular internal mechanism.

Archive `/media/anolis/dev/wii-gcn-focus-right-module.ko`, SHA256
`b6cf9a1914bf97d225682adb128c2b5cbbc7c422c300676b49c98da2b8d0dd4f`.
W=1 build and 50 real-capture audit tests pass. The extended suite
`wii-gcn-matrix-focus-right-20260913-long` tests right-anchored 256 then
the original wide control with a ceiling of 1000 each.

The extended right-256 case failed after 9 clean iterations at (530,54),
white `ffffff` becoming `ffff7f`, one raw/one copy error with three agreeing
peeks. The original wide control failed after 27 clean iterations at the
same pixel. Manifest verified. Thus the earlier 64-pass screen missed an
intermittent failure, and the left-256 1000-pass result does not survive
moving the remainder. This rejects a general 256-pixel subdivision limit.
The comparison holds coverage, width multiset, primitive count and command
volume fixed, while changing boundaries and their relationship to pixels.
It does not identify the internal hardware mechanism.

Follow-up `wii-gcn-matrix-focus-right-20260913-narrow` tests right-64 then
right-256, ceiling 1000 each, to check the narrower subdivision against
this boundary-sensitive failing control. Added the verified failure to
the audit fixtures; all 51 tests pass.

Follow-up result: right-64 passed 1000/1000 in 161.68 seconds, zero raw/copy
errors across 307200000 pixels per oracle. Right-256 then failed after 230
clean iterations in 37.19 seconds, again at (530,54), `ffffff` to `ffff7f`,
one raw/one copy error and three agreeing peeks. Its command hashes match
the first failing run. The manifest verified. The two failures (after 9
and 230 clean iterations) demonstrate recurrence, not a measured stable
failure probability. Right-96 and right-128 have only 64-iteration screen
results; do not treat them as extended passes.

Final cleanup confirmed module unloaded, matrix lock released, unchanged
boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and
installed module SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
All five suite manifests in this width/anchor series verified. W=1 builds,
51 audit tests and diff checks pass. No installed-provider or default
rendering change. The underlying thin-primitive fault remains unresolved;
the evidence supports 64-pixel subdivision in these tested geometries,
not a universal rendering guarantee.

## 2026-09-13: extended intermediate widths with the shifted boundary

Reused the exact right-anchor module above without code changes. Suite
`wii-gcn-matrix-focus-right-20260913-intermediate` runs right-128, right-96
and right-256 with a ceiling of 1000 iterations each, stopping each case
on its first mismatch.

| Case | Result | Raw/copy errors | Time |
| --- | --- | --- | --- |
| Right-128 | PASS 1000/1000 | 0/0 | 161.09 s |
| Right-96 | PASS 1000/1000 | 0/0 | 160.02 s |
| Right-256 | FAIL after 580 clean iterations | 1/1 | 93.31 s |

Each passing case checked 307200000 pixels per oracle. Right-256 failed
again at (530,54), white `ffffff` becoming `ffff7f`; three peeks agreed and
the copied RGB565 pixel was `ffef` instead of `ffff`. Its command hashes
match both earlier extended failures. The three first-failure positions
(9, 230 and 580 clean iterations) demonstrate recurrence and variable
time to failure, not an unbiased failure-rate estimate.

This extends the earlier right-96/right-128 screen results. At this fixed
coverage, span 128 now passed 1000 with both grid anchors, using 10 quads
and 883 bytes versus 18 quads and 1267 bytes for span 64, with one submission
in either case. That is a command-cost reduction for this diagnostic,
not a measured production speedup or a generally safe width threshold:
the standalone 128-pixel probe previously failed after 182 clean iterations
at (512,52), and surrounding visible draws differ between those layouts.
No production subdivision policy or installed driver was changed.

The complete evidence manifest verified. Cleanup confirmed module unloaded,
matrix lock released, same boot UUID/printk and unchanged installed-provider
hash recorded above. The module remains
`b6cf9a1914bf97d225682adb128c2b5cbbc7c422c300676b49c98da2b8d0dd4f`.
Existing W=1 build and 51 audit tests apply unchanged; this turn only adds
hardware evidence and documentation. Diff checks pass.

## 2026-09-13: right-anchored 128-pixel subdivision is order-sensitive

Added matrix cases combining existing right anchoring and span 128 with
reverse and column-major order. No module changes: the same archived
`b6cf9a1914bf97d225682adb128c2b5cbbc7c422c300676b49c98da2b8d0dd4f`
module emits the identical ten quads in different orders. Both retain
883 command bytes, one submission and the full 2144-white/305056-black
coverage oracle.

In `wii-gcn-matrix-focus-right-order-20260913-long`, reverse failed after
21 clean iterations at (530,54), `ffffff` to `ffff7f`, with one raw/one
copy error and three agreeing peeks. Column-major passed 1000/1000 in
160.49 seconds, checking 307200000 pixels per oracle without errors. The
prior row-major 1000-pass result therefore does not survive reversal.
This rejects an order-independent 128-pixel workaround for this layout;
it does not establish an internal hardware mechanism. The reverse failure
is preserved in an audit fixture; all 52 audit tests pass.

The suite's right-256 control failed after 326 clean iterations at (530,54),
again `ffffff` to `ffff7f`, one raw/one copy error and three agreeing peeks.
The complete manifest verified.

Follow-up `wii-gcn-matrix-focus-right-order-20260913-narrow` combines right
anchoring and reverse order at span 64, then repeats reverse-128:

| Case | Result | Raw/copy errors | Time |
| --- | --- | --- | --- |
| Right-64 reverse | PASS 1000/1000 | 0/0 | 159.80 s |
| Right-128 reverse | FAIL after 83 clean iterations | 1/1 | 13.57 s |

The passing case checked 307200000 pixels per oracle. Reverse-128 failed
at (530,54) with the same `ffffff` to `ffff7f` error and agreeing peeks;
its command hashes match the initial 21-clean-iteration failure. This
reproduces the failure with a different preceding test and supports the
narrower subdivision under the combined boundary/order changes. It does
not establish a general safe-width threshold or the internal cause.

Both manifests verified. Final cleanup confirmed module unloaded, lock
released, unchanged boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`,
printk `7 4 1 7`, and installed-provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
The archived driver is unchanged; only matrix cases, capture fixtures,
audit tests and documentation changed. All 54 audit tests and diff checks
pass. No production default or installed driver change.

## 2026-09-14: reverse order also breaks the 96-pixel layout

Added matrix case `focus-right-96-reverse`, combining existing module
parameters without a driver change. It uses 12 quads and 979 authored
bytes in one submission over the same 2144 white and 305056 black pixels.
The exact archived module remains
`b6cf9a1914bf97d225682adb128c2b5cbbc7c422c300676b49c98da2b8d0dd4f`.

Suite `wii-gcn-matrix-focus-96-order-20260914-long`: reverse-96 failed after
373 clean iterations in 60.05 seconds at (530,54), white `ffffff` becoming
`ffff7f`, one raw/one copy error and three agreeing peeks. Reverse-128 then
failed after 28 clean iterations at the same pixel. Manifest verified.
The earlier 96-pixel row-major passes therefore do not extend to reversal.
This rejects the remaining tested intermediate width as an order-independent
workaround in this layout; it does not establish a universal 64-pixel limit.

Follow-up `wii-gcn-matrix-focus-96-order-20260914-repeat` tests reverse-64
then reverse-96, ceiling 1000 each, to check recurrence after the narrower
comparison. Added the initial 96-pixel failure to the audit fixtures;
all 55 audit tests pass. No production rendering change.

Repeat result: reverse-64 passed 1000/1000 in 160.64 seconds with zero
raw/copy errors over 307200000 pixels per oracle. Reverse-96 then failed
after 118 clean iterations in 18.97 seconds, again at (530,54), white
`ffffff` to `ffff7f`, one raw/one copy error and three agreeing peeks. Its
command hashes match the initial 373-clean-iteration failure. Both suite
manifests verified. These first-error-censored runs establish recurrence,
not a stable failure-rate estimate.

This closes the intermediate-width/reversal comparison: 96 and 128 have
repeated reverse-order failures despite row-major 1000-pass runs; 256 has
repeated shifted-boundary failures; 64 has retained its passing results
under the tested orders and anchors, including repeated combined right-
anchor/reverse runs. Retain 64 as the conservative diagnostic choice.
Do not spend further runs optimizing between 64 and 96 before checking
that any proposed production policy handles actual textured workloads,
sampling phase, FIFO limits and command cost. These direct-color focused
results alone do not justify enabling a general production subdivision
policy. The underlying thin-primitive fault remains unresolved.

Final cleanup verified module unloaded, matrix lock released, unchanged
boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and
installed module hash
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Driver binary and production defaults unchanged. All 55 audit tests and
diff checks pass; only matrix cases, fixtures, tests and docs changed.

## 2026-09-14: reduce textured native subdivision from three batches to two

Returned to the existing textured native diagnostic rather than further
direct-color width optimization. Added `scale_native_batch_rows`, default
80, alternative 120 requiring native-span mode. The existing 240 rows now
permit two 120-row batches while retaining the same piece widths, row-major
emission, sampling phase and total quad count. The validated production
identity shortcut remains unchanged and is disabled only in these tests.

| Span | Quads per batch | Bytes per batch | Final bytes per sequence | Final submissions |
| --- | --- | --- | --- | --- |
| 64, 120-row batches | 600 | 48008 | 96016 | 2 |
| 160, 120-row batches | 240 | 19208 | 38416 | 2 |
| 64, original 80-row batches | 400 | 32008 | 96024 | 3 |
| 160, original 80-row batches | 160 | 12808 | 38424 | 3 |

The 64 KiB FIFO guard retains 256 bytes of headroom. No additional texture
allocation is introduced. This saves one final submission/completion wait
and eight authored bytes per sequence versus the three-batch variant,
not the extra vertex traffic inherent in 64-pixel subdivision. Traced
timings include readback and are not production performance measurements.

`wii-gcn-matrix-native-two-batches-20260914-r1`: both spans passed 16 frames
(64 quadrant sequences) with zero raw/copy/prior errors. Full stage,
preservation, exact batch bounds and resource audits passed; manifest
verified. The extended suite `wii-gcn-matrix-native-two-batches-20260914-long`
reverses case order (160,64), up to 256 frames / 1024 sequences per case,
stopping each case on the first bad frame.

Archive `/media/anolis/dev/wii-gcn-native-two-batches-module.ko`, SHA256
`a27161c852165421df9d86520c7c02e1aee054862a677003c6b0e6c2272ac299`.
W=1 build and 56 audit tests pass, including real captures rejecting wrong
batch rows, missing batches, wrong byte budgets and three-batch case
misclassification. Driver production defaults are unchanged.

Extended result: both two-batch spans passed 256/256 frames, 1024 quadrant
sequences each, with zero crop/horizontal/final, raw/copy and prior-content
errors. The full manifest verified. Resource totals:

| Span | Final submissions | Final command bytes | Largest final batch |
| --- | --- | --- | --- |
| 160 | 2048 | 39337984 | 19208 |
| 64 | 2048 | 98320384 | 48008 |

Compared with the archived three-batch 64-pixel 1024-sequence pass, the
new 64-pixel arrangement saves 1024 final submissions/completion waits and
8192 command bytes, with the same 1200 quads per sequence and no additional
texture buffers. It still incurs approximately 2.5 times the original
thin-run final vertex traffic. This validates the tested batching/cost
change, not a production speedup or a universal workaround.

The wider two-batch case also passed. Preserve that result alongside its
earlier three-batch failure after 217 clean frames and the original
one-batch baseline's pass: these observations do not show a monotonic
relationship between batch count and reliability or prove the underlying
fault resolved. No nonidentity sampling policy is enabled by this native
1:1 diagnostic result.

Cleanup verified module unloaded, matrix lock released, same boot UUID
`444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and installed
provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
W=1 build, 56 audit tests and diff checks pass. The 80-row default and
production identity shortcut remain unchanged; the installed driver was
not replaced.

## 2026-09-14: reverse textured quads within the two native batches

Added diagnostic `scale_native_reverse`, default off and requiring
native-span mode. It reverses each batch's complete quad list by reversing
row and column traversal, while retaining batch order, coordinates,
sampling phase, pixel coverage, quad counts, waits and command budgets.
The nested loops avoid adding a division for every emitted quad. A marker
identifies every sequence/batch as reversed within the batch; host audits
require the complete ordered marker set and reject it in normal cases.
Production identity behavior and default batch size remain unchanged.

`wii-gcn-matrix-native-reverse-20260914-r1`: reversed spans 64 and 160 each
passed 16 frames / 64 quadrant sequences. Normal-order two-batch span 64
also passed 16 frames. All 64 horizontal and final sequence command hashes
in that normal-order capture exactly match the prior two-batch module's
screen, confirming preservation of the authored streams. Manifest verified.

The extended suite `wii-gcn-matrix-native-reverse-20260914-long` reverses
case order (160,64) with a ceiling of 256 frames / 1024 sequences each.
The 64-pixel final batches remain 48008 bytes each, and 160-pixel batches
19208 bytes each, two submissions per sequence and no extra textures.

Archive `/media/anolis/dev/wii-gcn-native-reverse-module.ko`, SHA256
`17fd2c858ef94856d00cf9657bf30ef5fa0eb20447f1ceed344710b534d432cf`.
W=1 build and 57 real-capture audit tests pass, including missing, wrong-
scope and wrong-order marker rejection. This is diagnostic stress of an
already-tested textured path, not a new production rendering policy.

Extended result: reverse-64 passed 256 frames / 1024 sequences with zero
stage, raw/copy and preservation errors. Reverse-160 failed the stage
audit at sequence 967 (one-based frame 242), after 241 fully clean frames.
The full client nevertheless completed all 256 frames and reported PASS.
At origin (0,240), final-stage pixel (366,286) was `5a58` instead of `5a5a`,
raw ARGB `005a49c6`, with one raw/output-precision mismatch and one copied
mismatch. Raw EFB and copied output agreed. Crop/horizontal and prior
checks were clean. This pixel lies outside the quadrant being updated;
subsequent quadrant rendering can hide such an intermediate-image error
from an end-of-frame client. The stage audit correctly marked FAIL.

This exposes a reporting/stop-policy limitation, not a missed PASS/FAIL
classification: the old native `completed` field counted client completion
and showed FAIL 256/256. Native stage checks are audited after capture, so
these runs do not necessarily stop at the first intermediate error. The
earlier generic first-mismatch-stop description does not apply to this
case. The audit now reports clean frames before the first bad sequence as
`completed`, with `checked`, `client_frames_completed`,
`client_reported_pass`, `first_error_sequence` and `first_error_frame`
separately. The new real-capture test requires FAIL despite client success,
241 clean frames, 256 checked frames, sequence 967 and frame 242.

Original evidence is immutable and its manifest verified. A separate
corrected re-audit is archived at
`/media/anolis/dev/wii-gcn-native-reverse-reaudit-20260914`, including audit
source, input paths/hashes, results, explanation and verified SHA256SUMS.
Both the short-screen and extended-suite manifests verified. Reverse-64
retains two final submissions and 96016 authored bytes per sequence, with
48008-byte batches and no extra texture allocation. The 160-pixel failure
does not establish a universal width threshold or the underlying cause.

Final cleanup verified module unloaded, lock released, unchanged boot UUID
`444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and installed
provider hash
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
W=1 build, 58 audit tests and diff checks pass. Production defaults and
the installed driver remain unchanged.

## 2026-09-14: stop traced native tests after the first bad quadrant

Added diagnostic `scale_native_stop_on_error`, default off. Native stage
and prior-content comparisons now return a mismatch flag while preserving
their existing logs. A per-call flag accumulates errors without skipping
later comparisons. After completing the current quadrant's three stages,
preservation checks and output copy, the driver logs `native-stop` and
returns `EILSEQ`, preventing the next quadrant ioctl from running. This
stops at a complete sequence boundary rather than discarding evidence
mid-stage. Production behavior is unchanged when the option is off.

The host accepts a partial four-quadrant frame only when the last captured
sequence has the exact stop marker, a verified mismatch and client failure.
It still requires all stages, commands, state and batch records through
that sequence, and rejects continued sequences after the first error.
Checked frames include the partial last frame; fully clean/client-completed
frames exclude it. Historical full-frame and post-run-only audits remain
supported.

Controls use `scale_native_test_mismatch=1` or `3`, requiring stop mode.
They alter one bit of a local expected value at final-stage pixel (0,0),
without changing framebuffer data. Explicit kernel injection markers and
`injected_oracle_error=true` keep these separate from real hardware faults.
The audit requires exactly the deliberate mismatch and no additional
errors; control FAIL results are expected and must not enter fault-rate
statistics.

`wii-gcn-matrix-native-stop-20260914-controls`: controls stopped exactly at
sequences 1 and 3, each with one raw/one copied oracle mismatch, zero fully
completed frames and one partially checked frame. The clean stop-enabled
64-pixel case then passed 16 frames / 64 sequences. All 64 horizontal and
final command records match the previous reverse-order module's screen.
Manifest verified. The separate live suite
`wii-gcn-matrix-native-stop-20260914-live` runs reverse-160 with stop enabled
and no injection, up to 256 frames.

Archive `/media/anolis/dev/wii-gcn-native-stop-module.ko`, SHA256
`607df130d483618caba539183569aa2041b45babd709126f6c81ccb334c91434`.
W=1 build and 60 audit tests pass, including complete partial-frame controls,
missing/misplaced stop markers, missing stages, missing injection labels,
and rejection of a stop with no actual mismatch. No installed-driver change.

Live validation reproduced a real error at sequence 31, frame 8, origin
(0,240), final-stage pixel (111,335): expected `07ff`, actual/copy/EFB
`07df`, raw ARGB `0000fbff`. There was one raw/output-precision mismatch
and one copied mismatch, with no crop/horizontal or prior-content errors.
The exact stop marker followed sequence 31; sequence 32 never ran. The
client returned failure on the ioctl rather than reporting a final PASS.
The audit reports 7 fully clean/client-completed frames, 8 checked frames,
31 captured sequences, `stopped_on_error=true`, and
`injected_oracle_error=false`. The final command hashes match the previous
reverse-160 module. This validates early stopping on both deliberately
wrong expected values and a naturally occurring hardware-path mismatch;
it does not fix the rendering fault itself.

The live suite manifest verified. The real partial-frame capture is also
in the audit fixtures; all 61 tests pass, including rejecting an incorrect
stop boundary or treating this partial frame as an old non-stop case.
W=1 build and diff checks pass. Final cleanup confirmed module unloaded,
lock released, unchanged boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`,
printk `7 4 1 7`, and installed-provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Production defaults and installed driver are unchanged. Use the explicit
stop-enabled cases for subsequent diagnostic comparisons when first-bad-
quadrant termination is desired; legacy cases retain their old behavior.

## 2026-09-14: full stop-enabled textured narrow/wide comparison

Reused the exact stop-module archive without code changes or injections.
`wii-gcn-matrix-native-stop-20260914-comparison` runs reverse-64 then
reverse-160 with two 120-row batches and a ceiling of 256 frames each.

Span 64 passed 256/256 frames, 1024 quadrant sequences, with zero stage,
raw/copy, copy-difference and prior-content errors. Span 160 failed at
sequence 86 (frame 22), after 21 clean frames, and stopped before sequence
87. The final-stage pixel (495,234), origin (320,0), was `0760` instead of
`07e0`, raw ARGB `0000ef00`; raw EFB and copied output agreed. There was
one raw/output-precision and one copied mismatch, with all crop/horizontal
and prior checks clean. The audit reports 22 checked frames including the
partial last frame, `stopped_on_error=true`, and
`injected_oracle_error=false`.

The final command hashes match the earlier reverse-160 runs. This repeats
the textured narrow/wide distinction under the verified first-bad-quadrant
stop policy, using an unchanged module. It does not establish a universal
64-pixel safety guarantee, a stable failure probability, or the internal
cause. The 64-pixel path retains two final submissions and 96016 authored
bytes per sequence, with 48008-byte batches and no added texture buffers.

Manifest verified. Cleanup confirmed module unloaded, lock released,
unchanged boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk
`7 4 1 7`, and installed provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Existing W=1 build and 61 passing audit tests apply unchanged; only evidence
and documentation were added. Diff checks pass. Production defaults and
installed driver unchanged.

## 2026-09-14: vary source content in the stop-enabled native geometry

Kept the exact stop-enabled driver and reversed two-batch geometry, changing
only the offscreen client's optional source content. `--content-cycle`
requires native-tiled offscreen mode and cycles frame modulo eight through
black, white, red, green, blue, one-pixel checkerboard, walking RGB565 bit,
and deterministic hashed RGB565 noise. Channels are expanded into XRGB8888
by bit replication. Checker phase and walking-bit position advance each
cycle; noise uses seed `(frame + 1) * 0x9e3779b9` modulo 2^32 and the fixed
integer mixer documented in the client. The full-image client and actual-
source kernel stage/preservation oracles remain active.

Every attempted frame logs frame/pattern/seed. The host requires the exact
schedule through any partially checked last frame, rejecting missing or
wrong pattern/seed records. Original client patterns retain their behavior.
No driver parameters, geometry, sampling phase, FIFO budget or texture
allocation change beyond selecting the existing stop-enabled cases.

`wii-gcn-matrix-native-content-20260914-r1`: both widths passed 16 frames /
64 sequences (two complete content cycles), with zero errors. The rebuilt
client's original pattern also passed 16 stop-enabled 64-pixel frames.
Manifest verified. The extended suite
`wii-gcn-matrix-native-content-20260914-long` runs widths 160 then 64, each
capped at 256 frames / 1024 sequences / 32 content cycles with first-bad-
quadrant stopping and no injected errors.

Driver archive remains `/media/anolis/dev/wii-gcn-native-stop-module.ko`,
SHA256 `607df130d483618caba539183569aa2041b45babd709126f6c81ccb334c91434`.
Client `/media/anolis/dev/wii-gcn-native-content-clients/wii-gcn-kms-flip-test`,
SHA256 `9e98abfdf2f943c373de6f25a607f4bbab3b35eba38b42681a0e88e51134cb3b`.
Strict client build and 62 audit tests pass. No production rendering change.

Extended results: content-64 passed all 256 frames / 1024 sequences,
32 complete cycles, with zero raw/copy, stage and preservation errors.
Content-160 stopped at sequence 32, frame 8 (zero-based content frame 7,
pattern 7: seeded noise), after 7 clean frames. At origin (320,240),
final-stage pixel (94,303) was `6c82` instead of `7c82`, raw ARGB `006b9210`.
The pixel lies in a previously drawn quadrant. Raw EFB and copied output
agreed; crop/horizontal and prior checks were clean. An independent host
calculation of the noise value at (94,303), seed `f1bbcdc8`, gives `7c82`,
matching the expected value in the captured oracle.

The stop was genuine (`injected_oracle_error=false`), with exactly one
raw/output-precision and one copied mismatch. The 160-pixel short screen
had passed two cycles, so this does not prove noise always fails or that
other patterns are immune. The passing 64-pixel run broadens the source
content evidence at fixed geometry; it does not establish a universal
rendering guarantee or the internal fault mechanism.

Both suite manifests verified. Added the real stopped content capture to
the audit tests, requiring the exact schedule through frame 7 and rejecting
missing or post-stop content markers. All 63 audit tests pass. Additional
pattern logs exposed a progress-only issue: the 2048-byte log tail could
lose the last 30-frame checkpoint and reset the bar. The display now keeps
its previous maximum and uses pre-frame content markers conservatively
(frame N means N earlier frames completed), plus final PASS counts. This
does not alter captured evidence or authoritative audit results.

Final cleanup confirmed module unloaded, matrix lock released, unchanged
boot UUID `444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and
installed-provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Strict client build and diff checks pass. Driver binary and production
defaults unchanged; changes are limited to client content, matrix/audits,
fixtures, progress display and documentation.

## 2026-09-14: opt-in general bounded final-stage scaling

Added `scale_bounded_final`, default off, to make the tested width/batching
approach usable beyond fixed diagnostic predicates. The helper retains
the existing nearest-source-row grouping and semantic UV phase, splits
each destination run into pieces at most 64 pixels wide, clips the final
piece, and honors arbitrary supported destination offsets. It handles
both enlargement and reduction. The existing identity shortcut retains
precedence; specialized trace modes cannot be combined with this option.

The helper finishes preservation/state before active draws, then emits at
most 600 quads per batch. Each complete batch has at most 48008 authored
bytes, with a compile-time guard preserving 256 bytes of FIFO headroom.
Intermediate batches complete before continuing; the caller submits the
last batch before copying output. No texture/EFB allocation is added.
This adds a state-completion wait per call and potentially substantial
vertex traffic; it is not enabled as a production policy or presented as
a measured speedup.

`wii-gcn-matrix-bounded-final-20260914-r1`: the full render UAPI regression
passed with the option enabled, then passed with the default path. The
new path was exercised by 27 scaling calls, including odd widths, nonzero
offsets, one-row sources, enlargement, reduction and 640-pixel outputs.
The audit independently verified nearest-source row counts and every batch:
37 final submissions, 27 state submissions, 984936 authored final command
bytes, maximum final batch 48008 bytes. The result JSON retains all call
geometries. Client pixel checks passed; raw EFB errors were not sampled
and remain null rather than being reported as zero.

This is a compatibility screen across real scaling paths, not a long-run
reliability qualification. Before enabling the option by default, run
repeated varied nonidentity workloads and assess command/wait costs. The
underlying thin-primitive fault is not identified or proven fixed.

Archive `/media/anolis/dev/wii-gcn-bounded-final-module.ko`, SHA256
`c4f19e9dbf033e25fe6254c57093b29044a82461766d99a8ff533f54f2e00267`.
W=1 build and 64 audit tests pass, including the real regression capture
and rejection of wrong nearest-run counts, missing batches, oversized
commands or an unexercised helper. Manifest and diff checks verified.
Cleanup confirmed module unloaded, lock released, unchanged boot UUID
`444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and installed
provider hash
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
Production defaults and installed driver unchanged.


## 2026-09-14: repeated general scaling exposes a full-screen failure

The three 16-iteration screens in
`wii-gcn-matrix-bounded-workloads-20260914-r1` passed. The reversed-order
1000-iteration sweep in `wii-gcn-matrix-bounded-workloads-20260914-long-r2`
used the same archived general-path module (`c4f19e9d...`) and render
client (`c87d28a4...`):

| Workload | Result | Clean / attempted | Client pixels checked in clean iterations |
|---|---|---:|---:|
| Eight-pattern 640x240 to 320x120 reduction | PASS | 1000 / 1000 | 38400000 |
| Linear 320x240 to 640x480 enlargement | FAIL | 51 / 52 | 15667200 |
| Offset 255x79 to 256x79 enlargement | PASS | 1000 / 1000 | 65536000 |

Enlargement iteration 52 reported pixel (317,352), actual RGB565 `0xdc9a`
versus expected `0xdc9e` (XOR `0x0004`). All four 600-quad / 48008-byte
final batches were captured for each of the 52 calls. The client stopped
at the first failed call, MEM1 returned to its original free count, and
the runner restored the CPU console. The other cases completed normally.
Reduction covered 125 cycles of the eight patterns.

These are full destination client checks, without intermediate raw EFB
sampling. The failing stage cannot be localized from this capture. The
64-pixel general subdivision is therefore insufficient to qualify the
path as reliable. The earlier single regression pass remains valid as a
compatibility screen but did not predict this intermittent result.

The original suite remains immutable, including its empty first-pixel
summary field. The audit now extracts the client's failure record and
coordinates for new captures. Real failure fixtures require exactly 51
clean and 52 attempted calls and preserve the observed pixel/value pair.
Short-capture tests reject wrong progress, geometry and content schedules;
a separately marked synthetic outcome tests failure accounting. Both
original suite manifests verify. The initially sandbox-blocked launch
created `wii-gcn-matrix-bounded-workloads-20260914-long` without hardware
results; `long-r2` is the actual completed sweep.

To isolate batching from geometry, added an experimental
`scale_bounded_batch_quads=400|600` parameter, default 600. The 400 setting
requires `scale_bounded_final=1`; all production defaults remain unchanged.
`bounded-system-400` uses six 400-quad batches; `bounded-system` retains four
600-quad batches. Both use the same rebuilt module, source pattern, client
checks, 64-pixel pieces and state fence. The 400 setting adds two completion
boundaries and 16 authored final bytes per full-screen call (192048 versus
192032 bytes); it does not reduce total vertex count or add texture memory.
This comparison changes batch boundaries and their associated waits, so
it cannot distinguish those effects from each other.

Rebuilt module: `/media/anolis/dev/wii-gcn-bounded-batch-module.ko`, SHA256
`d10658d90eeac013a9aa8acdc38e8082530a481a075f1206d6ab823e759ae8fe`.
W=1 build is clean. The earlier module archives are preserved.


### Batch comparison: both limits fail

`wii-gcn-matrix-bounded-batch-20260914-r1`, same rebuilt module/client:

| Final batch limit | Clean / attempted | First bad pixel | Actual / expected RGB565 |
|---|---:|---|---|
| 400 quads (six batches) | 144 / 145 | (285,368) | `0xe68a` / `0xe68e` |
| 600 quads (four batches) | 132 / 133 | (541,296) | `0xba06` / `0xba0e` |

Both cases stopped on the first failed client iteration. The 400-quad
capture contains all 870 expected final batches and has a maximum batch
of 32008 bytes. The 600-quad capture contains all 532 expected batches and
has a maximum batch of 48008 bytes. No raw stage oracle was active. These
observations reject reducing this final batch limit to 400 as a sufficient
fix; they do not establish a failure probability or rank the two limits.

The next discriminating experiment should localize the faulty stage using
source-based crop/horizontal/final oracles while retaining the general
helper. The specialized system trace previously passed, but it also changes
submission/readback timing. A traced pass alone would therefore not explain
these untraced failures. Do not spend another broad sweep tuning final batch
size before finding where the corruption first appears.

The default full render UAPI regression passed with the new module in
`wii-gcn-matrix-bounded-batch-default-20260914-r1`. All 69 audit tests pass,
including the real 400-quad failure and rejection of wrong/missing batches.
W=1 build and diff checks pass; both batch/default suite manifests verify.
Final hardware check confirms the same boot UUID and printk values, module
unloaded, matrix lock released, and unchanged installed-provider SHA256
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
No driver installation or production default change was made.


## 2026-09-14: source-based trace localizes a fault to horizontal rendering

`bounded-system-trace` combines the general 64-pixel final helper with
`scale_system_trace=1`, retaining the original unsplit horizontal geometry.
System tracing is now permitted with bounded mode, but the specialized
system-span geometry override remains rejected. The system oracle returns
whether any stage mismatched; bounded mode accumulates all three results,
finishes the current call's final copy, then returns `-EILSEQ` with an
explicit `bounded-stop` marker. Other system trace modes retain their prior
behavior. The CPU crop is checked against source memory; horizontal and
final stages compare raw EFB and copied texture at RGB565 precision.

`wii-gcn-matrix-bounded-trace-20260914-r1` failed on attempt 657, after 656
clean iterations:

| Stage | Source mismatches in copied texture | Raw EFB mismatches | EFB versus copy differences |
|---|---:|---:|---:|
| CPU crop, 76800 pixels | 0 | Not sampled | Not sampled |
| Horizontal, 153600 pixels | 1 | 1 | 0 |
| Final, 307200 pixels | 2 | 2 | 0 |

The first horizontal error is (62,166), RGB565 actual `0xcf9d`, expected
`0xcf9f`, raw ARGB `0x00cef3ef`. The final first error is (62,332), with the
same values. This is consistent with the vertical enlargement duplicating
one corrupted intermediate pixel into two output rows. The counts sum to
three across stages; they are not three independent fault events.

All 657 CPU crops were clean. The horizontal command record is unsplit,
320 quads, 26574 bytes, hash `89bff809` on the failing call. Each final call
retains four 600-quad batches. The capture ends with `bounded-stop seq=657`
and the client reports the ioctl error; it never submits attempt 658.
No copy divergence was observed: corruption already exists in raw EFB
after horizontal drawing. This localizes this observed event, without
claiming every previous untraced failure had the same origin.

The specialized passing system tests also enabled `scale_system_split=1`,
which splits each 2x240 horizontal strip into two 2x120 pieces. The failing
general workload did not. Added `bounded-system-horizontal-split` to test
that one geometry change without diagnostic EFB readbacks, retaining the
same general final helper and 600-quad limit. The runner verifies the loaded
`scale_system_split=Y` parameter. This doubles horizontal vertices (320 to
640 quads) while keeping one horizontal submission; no extra texture
allocation is required. Source mapping and final geometry are unchanged.

Module archive `/media/anolis/dev/wii-gcn-bounded-trace-module.ko`, SHA256
`0ff249ebbee66b6138d1d62cddd88be5ce805150c306c0389bcf68c964c8727a`.
W=1 build is clean. The traced archive manifest verifies. The real failure
fixture verifies stop accounting and rejects missing/wrong stages, changed
horizontal commands and missing first-fault/stop records. Earlier module
and test archives remain immutable.


### Untraced horizontal split passes on both sides of a failing control

`wii-gcn-matrix-bounded-horizontal-20260914-r1`:

- Horizontal split enabled: 1000/1000 PASS, 307200000 destination pixels checked.
- Original horizontal geometry: FAIL on attempt 90 after 89 clean iterations,
  at (541,296), actual `0xba06`, expected `0xba0e`.

The control repeats the same coordinate/value pair as the preceding
600-quad batch comparison. `wii-gcn-matrix-bounded-horizontal-20260914-r2`
then repeats the split case after that failure: another 1000/1000 PASS.
Thus the split has 2000 clean iterations / 614400000 destination pixel
checks, with an observed failing unsplit control between the two runs.
All three use the same module/client and general final batches, without
raw EFB readback. These pixel counts are coverage, not independent trials
or a statistical failure-rate estimate.

Together with the traced horizontal failure, this supports horizontal
subdivision as a focused workaround for this enlargement workload. It does
not prove that the general final helper is reliable for all workloads or
that a 120-pixel horizontal extent is universally safe. The next practical
step is a general bounded horizontal helper that preserves nearest-source
column grouping and UV phase, followed by odd-size/offset/ratio compatibility
and repeated untraced content tests. Keep the validated identity shortcut
in precedence, account for vertex/FIFO costs, and retain an unsplit control.
No production default should be inferred from these focused results.


The default render UAPI regression in `bounded-horizontal-20260914-r2`
passed. All 71 audit tests pass, including the real traced failure and a
real 1000-iteration split capture; missing loaded-parameter verification is
rejected. All three new suite manifests and diff checks verify. Final
cleanup confirms module unloaded, lock released, unchanged boot UUID
`444193a6-aee4-4ae3-a619-4f6dd90fccf1`, printk `7 4 1 7`, and installed
module SHA256 `a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
The installed driver and production defaults remain unchanged.


## 2026-09-14: opt-in general horizontal subdivision passes compatibility

Added `scale_bounded_horizontal`, default off and requiring bounded final
mode. It preserves nearest-source column groups, source S phase +2 and
vertical T phase -2. Each column run is split into pieces at most 120 pixels
high, clipping the tail. Up to 640 quads fit each horizontal batch; the first
batch includes existing state, later batches preserve it, and the caller
submits the final batch. Both compile-time and runtime checks retain the
256-byte FIFO reserve. No new texture allocations or initial state-only
submission are introduced. Identity copies retain their shortcut.

For the previously validated 320x240 to 640x480 workload, this emits the
same 640 horizontal quads in one submission (52174 authored bytes), followed
by the existing general final helper's four 600-quad submissions. The helper
also handles arbitrary supported sizes and offsets through the existing
crop stage. Specialized tracing/split overrides remain incompatible with
this new option. The default focused reduction split is superseded while
the new horizontal option is active.

The initial `wii-gcn-matrix-bounded-both-20260914-screen` was an infrastructure
ERROR: a validation conflict with the default reduction flag prevented
provider registration. The client printed its provider-absent PASS, but the
bounded audit rejected the unexercised helper. Corrected the option
precedence and strengthened all render audits to require an active provider.
The actual failed configuration is retained as a negative fixture; it is not
a hardware rendering fault. Its module archive is not the validated build.

The corrected `wii-gcn-matrix-bounded-both-20260914-screen-r2` passed the full
existing regression plus 16 iterations each of system enlargement, varied
reduction and offset enlargement. That regression exercised 27 horizontal
calls / submissions, 443258 horizontal authored bytes, maximum 52174 bytes.
It did not require multiple horizontal batches.

Added a full-client regression case: source (1,1), size 255x255, destination
(0,61), size 256x127. Its 765 horizontal quads split into 640 and 125, crossing
a batch boundary inside a column and exercising the 15-pixel tail. The
expanded regression passed in `wii-gcn-matrix-bounded-both-20260914-long`:
28 horizontal calls, 29 submissions, 505440 horizontal authored bytes, maximum
52174 bytes; 38 final submissions. All destination and outside pixels are
checked by the independent nearest-source client oracle.

Validated module `/media/anolis/dev/wii-gcn-bounded-both-r2-module.ko`, SHA256
`50bfa40e6127c21c8774f4bd337cfdefe2413495b28cdaf48d0324b376524f32`.
Expanded render client `/media/anolis/dev/wii-gcn-bounded-both-clients/wii-gcn-render-test`,
SHA256 `59efdac1c22160219b885e624d2f816cfd81792513ea981be5e85adc2dd5ca01`.
W=1 driver build and strict client build pass. Earlier archives are preserved.


### General two-stage long sweep passes

`wii-gcn-matrix-bounded-both-20260914-long` completed all cases:

| Workload | Result | Clean iterations | Destination pixels checked |
|---|---|---:|---:|
| Linear 320x240 to 640x480 | PASS | 1000 | 307200000 |
| Eight-pattern 640x240 to 320x120 | PASS | 1000 | 38400000 |
| Offset 255x79 to 256x79 | PASS | 1000 | 65536000 |

Total: 3000 clean workload iterations, 411136000 destination pixel checks.
Reduction covers 125 cycles of eight source patterns. These are full client
checks without raw EFB sampling; raw/copy error fields remain null. Both the
expanded bounded regression and the default regression passed. This is
positive validation of the opt-in helpers for these workloads, not proof of
universal reliability or a measured performance improvement. General
full-screen enlargement still needs broader source-content coverage and
performance measurement before any default-policy decision.

All 75 audit tests pass, including real multi-batch compatibility, exact
workload geometry and rejection of a provider-absent PASS. Corrected-screen
and long-run manifests verify. W=1 and strict client builds, shell syntax
and diff checks pass. Cleanup confirms the same boot UUID and printk,
module unloaded, matrix lock released, and unchanged installed provider
`a2e7df8e7f62d85b1c4d107b9142addb7bbf2ab0cf8812aa295dde3c8eea5d09`.
The compressed audit fixtures are explicitly tracked for reproducibility
from a clean checkout. General horizontal/final options remain off by default.
