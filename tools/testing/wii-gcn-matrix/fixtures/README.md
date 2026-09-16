These gzip fixtures contain kernel logs from the Wii diagnostic driver.

- interior-pass.txt.gz: wii-gcn-efb-interior4, four clean reduced-coverage iterations.
- even-failure.txt.gz: wii-gcn-efb-interior-even4, red pass then white pixel failure.

- late-z-failure.txt.gz: late-z-one-row, with an extra first-batch PE-control write.

All original transfers were verified against device SHA-256 values. The tests
exercise acceptance of real passes/failures and rejection of incomplete or
inconsistent evidence. These are diagnostic logs, not expected rendering images.

Additional captures cover the native, focused primitive, offset, system,
reduction and bounded-scaling cases described in
`docs/wii-gcn-fault-investigation-2026-09-12.md`. Client captures accompany
tests that require progress or source-content schedules. Explicit oracle
injections and the provider-absent capture are negative controls, not
hardware fault observations. Test code labels any synthetic mutations.

The local `.gitignore` exception keeps compressed capture fixtures tracked
despite the kernel tree's general archive ignore rule. Run the suite from
the repository root with:

```
python3 -m unittest discover -s tools/testing/wii-gcn-matrix
```

`scheduler-loop-{trace,stats,client}.txt.gz`: 64-iteration whole-loop capture
from `wii-gcn-scheduler-loop-20260915-r1`, 5279 entries with zero loss. Includes
nested softirq `tcp_tsq_workfn` within worker execution and work boundaries in
between ioctl windows. Raw trace SHA256:
`b2085cec6b2734e13ad547058edab50139156afbabc7fdfd8e0084f4f50a4eb3`.

`system-baseline-profile-failure{,-client}.txt.gz`: first CPU comparison round,
`wii-gcn-matrix-cpu-pair-20260915-r1/system-baseline-profile`; 13 clean calls,
then a destination mismatch at (541,296) on attempt 14. Tests require the failed
attempt's profile record but exclude it from clean timing arrays.

`bounded-cache-profile{,-client}.txt.gz` preserves the first coordinate-cache
capture (`wii-gcn-matrix-coord-cache-20260915-r1`). The runner then checked only
the last repeated `--expect-param`; the current audit rejects this capture as
missing explicit cache-parameter verification. Do not use it as qualified data.
`bounded-cache-verified{,-client}.txt.gz` is the corrected round-four capture,
with both cache and identity parameters verified and all 32 calls passing.

`mixed-cache-{400,600}{,-client}.txt.gz`: verified 16-call screens from
`wii-gcn-matrix-mixed-cache-20260915-screen`. The tall odd mixed-axis geometry
forces horizontal 640+125-quad submissions, and final 400+108 or one 508-quad
submission. Tests reject incorrect final horizontal batch size and substitution
of the simpler offset geometry.

`bounded-quiet{,-client}.txt.gz`: verified 32-call quiet-mode capture from
`wii-gcn-matrix-bounded-quiet-20260915-r1`. It has complete client pixel/profile
records and explicit mode parameter checks, with no per-batch diagnostics.
Tests reject missing mode checks, unexpected bounded diagnostic records, and
attempts to treat this as a full batch-evidence capture.

`display-rgb565{,-client}.txt.gz`: 120-frame real KMS screen from
`wii-gcn-matrix-display-20260915-screen-r2`. Includes 118 presentation intervals,
full buffer pixel verification, CRTC restoration, and MEM1 recovery. Tests reject
missing restoration/accounting or inconsistent pacing counts.

`display-paced-rgb565{,-client}.txt.gz`: 120-frame boundary-only KMS capture
from `wii-gcn-matrix-display-paced-20260915-r1`. Only frames 0 and 119 are
pixel-verified. Tests reject missing/wrong boundaries, inflated pixel totals,
and substitution between full-oracle and boundary-only evidence.

`display-stages-rgb565{,-client}.txt.gz`: 120-frame boundary-only stage profile
from `wii-gcn-matrix-display-stages-20260915-r1`. Tests require a unique profile,
correct frame/verification/flip counts, positive totals, and render-total agreement.

`display-prepared-rgb565{,-client}.txt.gz`: 120-frame static-source reuse capture
from `wii-gcn-matrix-display-prepared-20260915-r1`. Tests reject absent/duplicate
source records, incorrect generation counts and dynamic/prepared substitution.
