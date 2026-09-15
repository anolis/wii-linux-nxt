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
