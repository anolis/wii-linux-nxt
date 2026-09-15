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
