#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Summarize verified, distinct matrix first-pixel captures; never estimate a rate."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/media/anolis/dev'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    rows, skipped, seen = [], [], set()
    pattern = re.compile(r'efb-clear-first iteration=(\d+) x=(\d+) y=(\d+) expected=([0-9a-f]+) raw=([0-9a-f]+)')
    for path in sorted(args.root.glob('wii-gcn-matrix-*/*/result.json')):
        result = json.loads(path.read_text())
        match = pattern.search(result.get('first_record', ''))
        if not match:
            continue
        suite = path.parent.parent
        try:
            # Multiple files can have the same hash: preserve paths as keys instead.
            manifest = {name.strip().lstrip('*'): sha for sha, name in
                        (line.split(None, 1) for line in (suite / 'SHA256SUMS').read_text().splitlines())}
            raw_result = path.read_bytes()
            kernel_path = path.parent / 'kernel.txt'
            raw_kernel = kernel_path.read_bytes()
            for item, data in ((path, raw_result), (kernel_path, raw_kernel)):
                if manifest[str(item.relative_to(suite))] != digest(data):
                    raise ValueError('manifest mismatch: ' + str(item))
            kernel_hash = digest(raw_kernel)
            if result['kernel_sha256'] != kernel_hash:
                raise ValueError('result kernel hash differs')
            if result['first_record'] not in raw_kernel.decode():
                raise ValueError('first record absent from verified kernel capture')
            if kernel_hash in seen:
                skipped.append(dict(path=str(path), reason='duplicate kernel capture'))
                continue
            seen.add(kernel_hash)
        except (OSError, ValueError, KeyError) as error:
            skipped.append(dict(path=str(path), reason=str(error)))
            continue
        iteration, x, y = map(int, match.groups()[:3])
        expected, actual = (int(v, 16) & 0xffffff for v in match.groups()[3:])
        if not (0 <= x < 640 and 0 <= y < 480 and expected != actual):
            raise ValueError('invalid fault coordinates/value: ' + str(path))
        rows.append(dict(path=str(path), suite=suite.name, case=result['case'],
                         iteration=iteration, x=x, y=y, expected=f'{expected:06x}',
                         actual=f'{actual:06x}', xor=f'{expected ^ actual:06x}',
                         precision=result.get('efb_precision', 'unspecified'),
                         kernel_sha256=kernel_hash))
    locations = defaultdict(list)
    for row in rows:
        locations[row['x'], row['y']].append(row)
    repeated = [(xy, records) for xy, records in locations.items() if len(records) > 1]
    repeated.sort(key=lambda item: (-len(item[1]), item[0]))
    exact_bits = sum(len({r['xor'] for r in records}) == 1 for _, records in repeated)
    lines = ['# Verified first-pixel fault corpus', '',
             f'{len(rows)} distinct verified captures; {len(locations)} first-pixel locations.',
             f'{len(repeated)} locations recur; {exact_bits} have the same observed XOR mask on every recurrence.', '',
             'These are first-error-censored, deliberately selected experiments. They do not estimate a failure rate,',
             'a complete spatial distribution, independent pixel probabilities, or a physical hardware diagnosis.',
             'Only result/kernel files used here were reverified against their suite manifests; complete suite verification',
             'is recorded separately by each experiment. Native traces and background failures are outside this corpus.',
             'Older captures may omit precision metadata; differing storage formats must not be treated as equal bit layouts.', '',
             '| Pixel | Captures | XOR masks | Cases |', '|---|---:|---|---|']
    for (x, y), records in repeated:
        lines.append(f"| ({x},{y}) | {len(records)} | {', '.join(sorted({r['xor'] for r in records}))} | {', '.join(sorted({r['case'] for r in records}))} |")
    lines += ['', f"Single-bit differences: {sum(int(r['xor'], 16).bit_count() == 1 for r in rows)} / {len(rows)} first errors.",
              f'Excluded captures: {len(skipped)} (reasons in skipped.json).', '']
    (args.output / 'report.md').write_text('\n'.join(lines))
    (args.output / 'captures.json').write_text(json.dumps(rows, indent=2) + '\n')
    (args.output / 'skipped.json').write_text(json.dumps(skipped, indent=2) + '\n')
    (args.output / 'source.py').write_bytes(Path(__file__).read_bytes())
    (args.output / 'SHA256SUMS').write_text(''.join(f'{digest(p.read_bytes())}  {p.name}\n'
                                                for p in sorted(args.output.iterdir()) if p.is_file()))
    print(f'{len(rows)} verified distinct captures; {len(repeated)} recurring locations, {exact_bits} stable XOR masks; {len(skipped)} exclusions.')
    print(args.output / 'report.md')


if __name__ == '__main__':
    main()
