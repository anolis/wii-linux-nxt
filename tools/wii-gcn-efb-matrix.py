#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run bounded EFB geometry controls, retaining and auditing every case's logs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
import uuid
import csv

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ('test single_quad two_quads batch full_width bands repeat_bands pad_bands '
         'two_rows interleave alternate odd_rows edge_pair interior_only interior_even four_rows late_z z_test z_write constant scissor_rows tall_rows cpu_write batch_snapshot extended_viewport overlap_rows stripe_colors white_background no_color_write z_never no_draw alpha_never alpha_threshold alpha_threshold_pass alpha_mixed alpha_mixed_reverse logic logic_set logic_black logic_invert logic_clear rgba6 focus focus_wide focus_split full_split focus_viewport focus_pad focus_right').split()
BASE = ['test', 'batch', 'full_width', 'bands', 'repeat_bands', 'pad_bands']

def case(name, flags, checker, quads=16, batches=30):
    return dict(name=name, flags=flags, checker=checker, quads=quads, batches=batches)

CASES = [
    case('clear', [], '', 0, 0),
    case('single-quad', ['test', 'single_quad'], 'single', 1, 1),
    case('two-quads', ['test', 'two_quads'], 'two', 2, 1),
    case('split-rows', ['test'], 'split', 960, 1),
    case('batched-split', ['test', 'batch'], 'batch', 32),
    case('one-row', ['test', 'batch', 'full_width'], 'full'),
    case('bands', BASE[:4], 'bands', 1),
    case('repeat-bands', BASE[:5], 'repeat'),
    case('padded-bands', BASE, 'padded'),
    case('four-rows', BASE + ['four_rows'], 'four-rows'),
    case('two-rows', BASE + ['two_rows'], 'two-rows'),
    case('interleaved', BASE + ['two_rows', 'interleave'], 'interleaved'),
    case('alternate', BASE + ['two_rows', 'alternate'], 'alternate'),
    case('odd-rows', BASE + ['two_rows', 'odd_rows'], 'odd-rows'),
    case('edge-pair', BASE + ['two_rows', 'odd_rows', 'edge_pair'], 'edge-pair'),
    case('interior', BASE + ['two_rows', 'odd_rows', 'interior_only'], 'interior'),
    case('interior-even', BASE + ['two_rows', 'odd_rows', 'interior_only', 'interior_even'], 'interior-even'),
]
# Pipeline controls keep the corresponding geometry and oracle unchanged.
for original in ('one-row', 'two-rows', 'interior-even'):
    reference = next(c for c in CASES if c['name'] == original)
    variant = dict(reference, name='late-z-' + original,
                   flags=reference['flags'] + ['late_z'], experimental=True)
    CASES.append(variant)

for original in ('one-row', 'two-rows', 'interior-even'):
    reference = next(c for c in CASES if c['name'] == original)
    for prefix, flags in [('z-test-', ['z_test']), ('z-write-', ['z_test', 'z_write']),
                          ('constant-', ['constant'])]:
        CASES.append(dict(reference, name=prefix + original,
                          flags=reference['flags'] + flags, experimental=True))

for name, flags in [('scissor-two-rows', ['scissor_rows']),
                    ('scissor-tall-rows', ['scissor_rows', 'tall_rows'])]:
    CASES.append(dict(case(name, BASE + ['two_rows'] + flags, 'two-rows'),
                      experimental=True))

for name, flag, quads in [('z-never-two-rows', 'z_never', 16), ('state-only', 'no_draw', 0)]:
    CASES.append(dict(case(name, BASE + ['two_rows', 'white_background', flag],
                           'two-rows', quads), experimental=True))

for name, extra in [('logic-clear-two-rows', ['logic_clear']), ('logic-copy-two-rows', []), ('logic-set-two-rows', ['logic_set']),
                    ('logic-copy-black-two-rows', ['logic_black']),
                    ('logic-set-black-two-rows', ['logic_set', 'logic_black']),
                    ('logic-invert-white-two-rows', ['logic_invert']),
                    ('logic-invert-black-two-rows', ['logic_invert', 'logic_black'])]:
    CASES.append(dict(case(name, BASE + ['two_rows', 'white_background', 'alpha_threshold',
                                        'alpha_threshold_pass', 'late_z', 'logic'] + extra,
                           'two-rows'), experimental=True))

reference = next(c for c in CASES if c['name'] == 'two-rows')
CASES.append(dict(reference, name='full-split-color-64', flags=reference['flags'] + ['full_split'], quads=88))
CASES.append(dict(reference, name='full-split-striped-64', flags=reference['flags'] + ['full_split', 'stripe_colors'], quads=88))
CASES.append(dict(reference, name='two-rows-striped-control', flags=reference['flags'] + ['stripe_colors']))

reference = next(c for c in CASES if c['name'] == 'logic-set-black-two-rows')
CASES.append(dict(reference, name='full-split-64', flags=reference['flags'] + ['full_split'], quads=88))
CASES.append(dict(reference, name='focus-530-54', flags=reference['flags'] + ['focus'], quads=2, batches=1))
CASES.append(dict(reference, name='focus-wide-530-54', flags=reference['flags'] + ['focus', 'focus_wide'], quads=2, batches=1))
for shift in (0,32):
    CASES.append(dict(reference, name=f'focus-viewport-{shift}', flags=reference['flags'] + ['focus','focus_wide','focus_viewport'],
                      quads=2,batches=1,viewport_shift=shift))
for top in (53, 60, 180):
    CASES.append(dict(reference, name=f'focus-wide-top-{top}', flags=reference['flags'] + ['focus', 'focus_wide'],
                      quads=2, batches=1, focus_top=top))
for width in (16, 32, 64, 128, 256, 512):
    CASES.append(dict(reference, name=f'focus-width-{width}', flags=reference['flags'] + ['focus'],
                      quads=2, batches=1, focus_width=width))

reference = next(c for c in CASES if c['name'] == 'focus-width-128')
CASES.append(dict(reference, name='focus-split-128', flags=reference['flags'] + ['focus_split'], quads=4))
reference = next(c for c in CASES if c['name'] == 'focus-wide-530-54')
CASES.append(dict(reference, name='focus-split-536', flags=reference['flags'] + ['focus_split'], quads=18))
for span in (96, 128, 256):
    CASES.append(dict(reference, name=f'focus-span-{span}', flags=reference['flags'] + ['focus_split'],
                      quads=2 * ((536 + span - 1) // span), focus_span=span))
for span in (64, 96, 128, 256):
    CASES.append(dict(reference, name=f'focus-right-{span}', flags=reference['flags'] + ['focus_split', 'focus_right'],
                      quads=2 * ((536 + span - 1) // span), focus_span=span))
for order,name in ((1,'reverse'),(2,'columns')):
    CASES.append(dict(reference,name=f'focus-split-{name}',flags=reference['flags']+['focus_split'],quads=18,focus_order=order))
for order, name in ((1, 'reverse'), (2, 'columns')):
    CASES.append(dict(reference, name=f'focus-right-128-{name}',
                      flags=reference['flags'] + ['focus_split', 'focus_right'],
                      quads=10, focus_span=128, focus_order=order))
CASES.append(dict(reference, name='focus-right-96-reverse', flags=reference['flags'] + ['focus_split', 'focus_right'],
                  quads=12, focus_span=96, focus_order=1))
CASES.append(dict(reference, name='focus-right-64-reverse', flags=reference['flags'] + ['focus_split', 'focus_right'],
                  quads=18, focus_span=64, focus_order=1))
CASES.append(dict(reference, name='focus-padded-536', flags=reference['flags'] + ['focus_pad'], quads=18))

for original in ('logic-clear-two-rows', 'logic-set-black-two-rows'):
    reference = next(c for c in CASES if c['name'] == original)
    CASES.append(dict(reference, name='rgba6-' + original, flags=reference['flags'] + ['rgba6']))

for name, extra in [('alpha-mixed-two-rows', []), ('alpha-mixed-reverse-two-rows', ['alpha_mixed_reverse'])]:
    CASES.append(dict(case(name, BASE + ['two_rows', 'white_background', 'alpha_threshold',
                                        'alpha_threshold_pass', 'late_z', 'alpha_mixed'] + extra,
                           'two-rows'), experimental=True))

for name, extra in [('alpha-less-two-rows', []), ('alpha-gequal-two-rows', ['alpha_threshold_pass'])]:
    CASES.append(dict(case(name, BASE + ['two_rows', 'white_background', 'alpha_threshold', 'late_z'] + extra,
                           'two-rows'), experimental=True))

CASES.append(dict(case('alpha-never-two-rows', BASE + ['two_rows', 'white_background', 'alpha_never', 'late_z'],
                       'two-rows'), experimental=True))

CASES.append(dict(case('late-z-never-two-rows', BASE + ['two_rows', 'white_background', 'z_never', 'late_z'],
                       'two-rows'), experimental=True))

CASES.append(dict(case('masked-two-rows', BASE + ['two_rows', 'white_background', 'no_color_write'],
                       'two-rows'), experimental=True))

CASES.append(dict(case('white-background-two-rows', BASE + ['two_rows', 'white_background'],
                       'two-rows'), experimental=True))

CASES.append(dict(case('cpu-write', ['test', 'cpu_write'], 'cpu-write', 0, 0), experimental=True))

CASES.append(dict(case('snapshot-two-rows', BASE + ['two_rows', 'batch_snapshot'], 'two-rows'), experimental=True))

for name, flags in [('extended-two-rows', ['extended_viewport']),
                    ('overlap-two-rows', ['extended_viewport', 'overlap_rows'])]:
    CASES.append(dict(case(name, BASE + ['two_rows'] + flags, 'two-rows'), experimental=True))

for name, flags in [('striped-two-rows', []), ('striped-overlap-two-rows', ['overlap_rows'])]:
    CASES.append(dict(case(name, BASE + ['two_rows', 'extended_viewport', 'stripe_colors'] + flags,
                          'two-rows'), experimental=True))

for name, flags in [('extended-one-row', []), ('overlap-one-row', ['overlap_rows']),
                    ('striped-one-row', ['stripe_colors']),
                    ('striped-overlap-one-row', ['stripe_colors', 'overlap_rows'])]:
    CASES.append(dict(case(name, ['test', 'batch', 'full_width', 'extended_viewport'] + flags,
                          'full'), experimental=True))

for name, single in [('native-baseline', False), ('native-single', True)]:
    CASES.append(dict(case(name, [], '', 0, 0), native=True, single=single, experimental=True))

for span in (64, 160):
    CASES.append(dict(case(f'native-span-{span}', [], '', 0, 0), native=True, single=False, span=span, experimental=True))

for span in (64, 160):
    CASES.append(dict(case(f'native-two-batches-{span}', [], '', 0, 0), native=True, single=False,
                      span=span, batch_rows=120, experimental=True))

for span in (64, 160):
    CASES.append(dict(case(f'native-two-reverse-{span}', [], '', 0, 0), native=True, single=False,
                      span=span, batch_rows=120, reverse=True, experimental=True))

for span in (64, 160):
    CASES.append(dict(case(f'native-stop-{span}', [], '', 0, 0), native=True, single=False,
                      span=span, batch_rows=120, reverse=True, stop_on_error=True, experimental=True))
for span in (64, 160):
    CASES.append(dict(case(f'native-content-{span}', [], '', 0, 0), native=True, single=False,
                      span=span, batch_rows=120, reverse=True, stop_on_error=True, content=True, experimental=True))
for sequence in (1, 3):
    CASES.append(dict(case(f'native-stop-control-{sequence}', [], '', 0, 0), native=True, single=False,
                      span=64, batch_rows=120, reverse=True, stop_on_error=True,
                      inject_sequence=sequence, experimental=True))

for name, traced in [('native-identity', True), ('native-identity-production', False)]:
    CASES.append(dict(case(name, [], '', 0, 0), native=True, identity=True,
                      traced=traced, single=True, experimental=True))

CASES.append(dict(case('native-default-production', [], '', 0, 0), native=True, identity=True,
                  traced=False, single=True, use_default=True, experimental=True))

for format_name in ('rgb565', 'xrgb8888', 'xrgb8888-native-tiled'):
    CASES.append(dict(case(f'display-quiet-{format_name}', [], '', 0, 0),
                      presentation=True, display_format=format_name, experimental=True))

for format_name in ('rgb565', 'xrgb8888', 'xrgb8888-native-tiled'):
    CASES.append(dict(case(f'display-paced-{format_name}', [], '', 0, 0),
                      presentation=True, boundary_checks=True, display_format=format_name, experimental=True))

CASES.append(dict(case('render-regression', [], '', 0, 0), regression=True, experimental=True))
CASES.append(dict(case('bounded-final-regression', [], '', 0, 0), regression=True, bounded=True, experimental=True))
for workload in ('offset', 'system', 'reduce-content'):
    CASES.append(dict(case(f'bounded-{workload}', [], '', 0, 0), bounded=True,
                      bounded_workload=workload, experimental=True))
CASES.append(dict(case('bounded-system-400', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_batch_quads=400, experimental=True))
CASES.append(dict(case('bounded-system-trace', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_trace=True, experimental=True))
CASES.append(dict(case('bounded-system-horizontal-split', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal_split=True, experimental=True))

CASES.append(dict(case('bounded-both-regression', [], '', 0, 0), regression=True,
                  bounded=True, bounded_horizontal=True, experimental=True))
for workload in ('offset', 'system', 'reduce-content'):
    CASES.append(dict(case(f'bounded-both-{workload}', [], '', 0, 0), bounded=True,
                      bounded_horizontal=True, bounded_workload=workload, experimental=True))

for workload in ('offset', 'reduce-content'):
    CASES.append(dict(case(f'bounded-both-{workload}-cached', [], '', 0, 0), bounded=True,
                      bounded_horizontal=True, bounded_workload=workload,
                      coord_cache=True, experimental=True))

for limit in (600, 400):
    CASES.append(dict(case(f'bounded-both-mixed-cached-{limit}', [], '', 0, 0), bounded=True,
                      bounded_horizontal=True, bounded_workload='offset', mixed_offset=True,
                      bounded_batch_quads=limit, coord_cache=True, experimental=True))

for name, baseline, content in [('system-baseline-timed', True, False),
                                ('bounded-both-system-timed', False, False),
                                ('bounded-both-system-content', False, True)]:
    CASES.append(dict(case(name, [], '', 0, 0), bounded=True, bounded_workload='system',
                      bounded_horizontal=not baseline, system_baseline=baseline,
                      system_content=content, system_timed=True, experimental=True))

CASES.append(dict(case('system-baseline-profile', [], '', 0, 0), bounded=True,
                  bounded_workload='system', system_baseline=True, system_content=True,
                  system_timed=True, system_profile=True, experimental=True))

CASES.append(dict(case('bounded-both-system-profile', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, experimental=True))

CASES.append(dict(case('bounded-both-system-profile-cached', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, coord_cache=True, experimental=True))
CASES.append(dict(case('bounded-both-regression-cached', [], '', 0, 0), bounded=True,
                  regression=True, bounded_horizontal=True, coord_cache=True, experimental=True))

CASES.append(dict(case('bounded-both-system-profile-quiet', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, coord_cache=True, bounded_quiet=True, experimental=True))
CASES.append(dict(case('bounded-both-regression-quiet', [], '', 0, 0), bounded=True,
                  regression=True, bounded_horizontal=True, coord_cache=True, bounded_quiet=True, experimental=True))

CASES.append(dict(case('bounded-both-system-sched', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, system_sched=True, experimental=True))

CASES.append(dict(case('bounded-both-system-sched-loop', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, system_sched=True,
                  system_sched_loop=True, experimental=True))

CASES.append(dict(case('bounded-both-system-profile-deferred', [], '', 0, 0), bounded=True,
                  bounded_workload='system', bounded_horizontal=True, system_content=True,
                  system_timed=True, system_profile=True, deferred=True, experimental=True))

for span in (64, 128):
    CASES.append(dict(case(f'offset-span-{span}', [], '', 0, 0), offset=True, span=span, experimental=True))

for span in (64, 320):
    CASES.append(dict(case(f'system-span-{span}', [], '', 0, 0), system=True, span=span, experimental=True))

for span in (64, 160):
    CASES.append(dict(case(f'reduce-span-{span}', [], '', 0, 0), reduce=True, span=span, experimental=True))
    CASES.append(dict(case(f'reduce-content-{span}', [], '', 0, 0), reduce=True, span=span, content=True, experimental=True))

def audit_presentation(log, client, spec, requested, rc):
    require('CPU console restored' in log and 'gcn-gx: ready fifo=' in log,
            'display provider/cleanup evidence missing')
    require(not re.search(r'\btimeout\b|\bstall\b|\bOops:|\bBUG:|Call Trace:',log,re.I), 'display kernel fault')
    for name,value in (('scale_bounded_final','Y'),('scale_bounded_horizontal','Y'),
                       ('scale_bounded_coord_cache','Y'),('scale_bounded_log','N')):
        require(log.count(f'parameter {name} verified {value}') == 1, 'display parameter not verified')
    require(len(re.findall(r'gcn-kms-flip-test: MEM1 recovered bytes=\d+',client)) == 1,
            'display MEM1 recovery missing')
    failure = re.search(r'frame (\d+) mismatch at \((\d+),(\d+)\): got=([0-9a-f]+) expected=([0-9a-f]+)',client)
    if rc:
        require(rc == 1 and failure, 'display infrastructure/client failure')
        completed = int(failure[1])
        require(completed < requested and (not completed or
                client.count('restored previous console framebuffer') == 1), 'failed display cleanup/count mismatch')
        if spec.get('boundary_checks'):
            require(completed in (0, requested-1), 'pixel failure outside checked boundaries')
        return dict(case=spec['name'],status='FAIL',requested=requested,completed=completed,
                    checked=(1 if completed == 0 else 2) if spec.get('boundary_checks') else completed+1,first_pixel=f'{failure[2]},{failure[3]}',first_record=failure[0],
                    raw_errors=None,copy_errors=None)
    require(not failure, 'display success despite pixel mismatch')
    require(client.count('restored previous console framebuffer') == 1, 'CRTC restoration missing')
    verified_frames = 2 if spec.get('boundary_checks') else requested
    if spec.get('boundary_checks'):
        require(re.findall(r'gcn-kms-flip-test: verified frame=(\d+)',client) == ['0',str(requested-1)]
                and client.count('verification mode=boundary frames=2') == 1, 'wrong boundary verification')
    else:
        require('verification mode=boundary' not in client, 'boundary mode in fully checked capture')
    rows = re.findall(r'gcn-kms-flip-test: PASS format=(\S+) frames=(\d+) last-vblank=(\d+) pixels=(\d+) render-us-avg=(\d+) render-us-max=(\d+)',client)
    require(len(rows)==1 and rows[0][0]==spec['display_format'] and int(rows[0][1])==requested
            and int(rows[0][3])==verified_frames*307200 and 0<int(rows[0][4])<=int(rows[0][5]), 'wrong display completion')
    pace = re.findall(r'pacing intervals=(\d+) total-ns=(\d+) min-ns=(\d+) max-ns=(\d+) gaps=([0-9,]+)',client)
    require(len(pace)==1, 'missing presentation pacing')
    n,total,minimum,maximum=map(int,pace[0][:4]);gaps=list(map(int,pace[0][4].split(',')))
    require(n==requested-2 and n>0 and len(gaps)==9 and sum(gaps)==n
            and 0<minimum<=maximum and n*minimum<=total<=n*maximum,'invalid presentation pacing')
    return dict(case=spec['name'],status='PASS',requested=requested,completed=requested,checked=verified_frames,
                client_pixels_checked=verified_frames*307200,pixel_verified_frames=verified_frames,raw_errors=None,copy_errors=None,
                evidence_mode='boundary-verified-buffers-and-KMS-events' if spec.get('boundary_checks') else 'verified-buffers-and-KMS-events',physical_screen_verified=False,
                render_us_avg=int(rows[0][4]),render_us_max=int(rows[0][5]),
                presentation_intervals=n,presentation_total_ns=total,presentation_min_ns=minimum,
                presentation_max_ns=maximum,vblank_gap_histogram=gaps)

def audit_regression(log, client_log, requested, rc):
    require('CPU console restored' in log, 'regression cleanup missing')
    require('registered scanout accelerator gcn-gx' in log and 'gcn-gx: ready fifo=' in log
            and 'provider absent' not in client_log, 'render provider not exercised')
    require(not re.search(r'\btimeout\b|\bstall\b|\bOops:|\bBUG:|Call Trace:', log, re.I), 'regression kernel fault')
    passed = client_log.count('PASS: GCN render UAPI')
    failures = re.findall(r'(\d+) render UAPI test\(s\) failed', client_log)
    require((rc == 0 and passed == 1 and not failures) or
            (rc == 1 and passed == 0 and len(failures) == 1), 'incomplete/unexplained render regression result')
    return dict(case='render-regression', status='PASS' if rc == 0 else 'FAIL', requested=1,
                completed=int(rc == 0), checked=1, raw_errors=None, copy_errors=None,
                first_pixel='', first_record='', seconds=None, failures=int(failures[0]) if failures else 0,
                efb_precision='render UAPI client checks')

def audit_bounded_regression(log, client_log, spec, requested, rc):
    result = audit_regression(log, client_log, requested, rc)
    if spec.get('coord_cache'):
        require(log.count('parameter scale_bounded_coord_cache verified Y') == 1,
                'bounded coordinate cache parameter not verified')
    result['case'] = spec['name']
    if spec.get('bounded_quiet'):
        for name, value in (('scale_bounded_final','Y'), ('scale_bounded_horizontal','Y'),
                            ('scale_bounded_log','N'), ('scale_bounded_batch_quads','600')):
            require(log.count(f'parameter {name} verified {value}') == 1, 'quiet bounded parameter not verified')
        require(not re.search(r'gcn-gx: bounded-(?:begin|batch|horizontal-begin|horizontal-batch)', log),
                'unexpected bounded diagnostics in quiet capture')
        result['evidence_mode'] = 'client-pixels-and-verified-parameters'
        return result
    starts = re.findall(r'bounded-begin seq=(\d+) x=(\d+) y=(\d+) width=(\d+) src_height=(\d+) dst_height=(\d+) runs=(\d+) quads=(\d+)', log)
    batches = re.findall(r'bounded-batch seq=(\d+) batch=(\d+) quads=(\d+) bytes=(\d+) last=([01])', log)
    require(bool(starts), 'generic bounded path not exercised')
    expected = []
    for index, record in enumerate(starts, 1):
        seq, x, y, width, sh, dh, runs, quads = map(int, record)
        require(seq == index and 0 < width <= 640 and 0 < sh <= 528 and 0 < dh <= 528 and
                x + width <= 640 and y + dh <= 528, 'wrong bounded geometry')
        expected_runs = len({min(((2 * row + 1) * sh) // (2 * dh), sh - 1) for row in range(dh)})
        require(runs == expected_runs and quads == runs * ((width + 63) // 64), 'wrong bounded nearest-run coverage')
        limit = spec.get('bounded_batch_quads', 600)
        for batch, first in enumerate(range(0, quads, limit)):
            count = min(limit, quads - first)
            expected.append((seq, batch, count, 8 + count * 80, int(first + count == quads)))
    require([tuple(map(int, row)) for row in batches] == expected, 'incomplete/wrong bounded batches')
    result.update(bounded_calls=len(starts), final_draw_submissions=len(batches),
                  state_submissions=len(starts), final_command_bytes=sum(int(row[3]) for row in batches),
                  max_final_command_bytes=max(int(row[3]) for row in batches),
                  bounded_geometries=[list(map(int, row[1:])) for row in starts])
    if spec.get('bounded_horizontal'):
        starts_h = re.findall(r'bounded-horizontal-begin seq=(\d+) src_width=(\d+) dst_width=(\d+) height=(\d+) runs=(\d+) quads=(\d+)', log)
        batches_h = re.findall(r'bounded-horizontal-batch seq=(\d+) batch=(\d+) quads=(\d+) before=(\d+) bytes=(\d+) last=([01])', log)
        require(len(starts_h) == len(starts), 'missing bounded horizontal calls')
        expected_h = []
        for i, row in enumerate(starts_h, 1):
            seq, sw, dw, height, runs, quads = map(int, row)
            require(seq == i and 0 < sw <= 640 and 0 < dw <= 640 and 0 < height <= 528,
                    'wrong bounded horizontal geometry')
            require((dw, height) == (int(starts[i-1][3]), int(starts[i-1][4])),
                    'horizontal/final geometry disagreement')
            count = len({min(((2*x+1)*sw)//(2*dw), sw-1) for x in range(dw)})
            require(runs == count and quads == count*((height+119)//120), 'wrong horizontal run coverage')
            for batch, first in enumerate(range(0, quads, 640)):
                n = min(640, quads-first)
                expected_h.append((seq, batch, n, int(first+n == quads)))
        require(len(batches_h) == len(expected_h), 'missing horizontal batches')
        for row, expected in zip(batches_h, expected_h):
            seq, batch, n, before, size, last = map(int, row)
            require((seq, batch, n, last) == expected and (before > 0 if batch == 0 else before == 0)
                    and size == before + 8 + 80*n and size <= 65536-256, 'wrong horizontal FIFO budget')
        result.update(horizontal_geometries=[list(map(int,r[1:])) for r in starts_h],
                      horizontal_draw_submissions=len(batches_h),
                      horizontal_command_bytes=sum(int(r[4]) for r in batches_h),
                      max_horizontal_command_bytes=max(int(r[4]) for r in batches_h))
    return result

def audit_bounded_workload(log, client_log, spec, requested, rc):
    result = (audit_regression(log, client_log, requested, rc) if spec.get('system_baseline')
              else audit_bounded_regression(log, client_log, spec, requested, rc))
    result['case'] = spec['name']
    if spec.get('deferred'):
        begins = re.findall(r'gcn-matrix-deferred begin (/tmp/gcn-matrix-[a-f0-9]{32})', log)
        ends = re.findall(r'gcn-matrix-deferred end (/tmp/gcn-matrix-[a-f0-9]{32})', log)
        require(len(begins) == 1 and ends == begins, 'missing/wrong deferred capture markers')
        require(log.index('gcn-matrix-deferred begin') < log.index('gcn-gx: ready fifo=') <
                log.index('CPU console restored') < log.index('gcn-matrix-deferred end'),
                'deferred markers do not enclose the case')
        result['capture_mode'] = 'deferred'
    if spec.get('bounded_horizontal_split'):
        require(log.count('parameter scale_system_split verified Y') == 1,
                'bounded horizontal split parameter not verified')
    workload = spec['bounded_workload']
    prefix, geometry, pixels = {
        'offset': ('OFFSET', [0, 97, 256, 79, 79, 79, 316], 65536),
        'system': ('SYSTEM', [0, 0, 640, 240, 480, 240, 2400], 307200),
        'reduce-content': ('REDUCE', [0, 0, 320, 240, 120, 120, 600], 38400),
    }[workload]
    if spec.get('mixed_offset'):
        geometry = [0, 61, 256, 255, 127, 127, 508]
    if spec.get('bounded_horizontal') and not spec.get('bounded_quiet'):
        geometry_h = {'offset': [255,256,79,255,255], 'system': [320,640,240,320,640],
                      'reduce-content': [640,320,240,320,640]}[workload]
        if spec.get('mixed_offset'):
            geometry_h = [255,256,255,255,765]
        require(result['horizontal_geometries'] == [geometry_h] * result['bounded_calls'],
                'wrong horizontal workload geometry')
    progress = re.findall(rf'{prefix}: (\d+)/(\d+) iterations', client_log)
    completed = len(progress)
    timings = re.findall(r'SYSTEM TIMING: iteration=(\d+) ns=(\d+) status=(-?\d+)', client_log)
    checked = len(timings) if spec.get('system_baseline') or spec.get('bounded_quiet') else result['bounded_calls']
    require(progress == [(str(i + 1), str(requested)) for i in range(completed)], 'wrong bounded workload progress')
    if not spec.get('system_baseline') and not spec.get('bounded_quiet'):
        require(result['bounded_geometries'] == [geometry] * checked, 'wrong bounded workload geometry')
    require((rc == 0 and completed == checked == requested) or
            (rc == 1 and completed < requested and checked == completed + 1), 'incomplete bounded workload')
    content = re.findall(r'REDUCE CONTENT: iteration=(\d+) pattern=(\d+) seed=(\d+)', client_log)
    expected_content = [(str(i), str(i % 8), str(((i + 1) * 0x9e3779b9) & 0xffffffff))
                        for i in range(checked)] if workload == 'reduce-content' else []
    require(content == expected_content, 'wrong bounded workload content schedule')
    result.update(requested=requested, completed=completed, checked=checked,
                  client_pixels_checked=completed * pixels)
    system_content = re.findall(r'SYSTEM CONTENT: iteration=(\d+) pattern=(\d+) seed=(\d+)', client_log)
    require(system_content == ([(str(i), str(i % 8), str(((i+1)*0x9e3779b9)&0xffffffff)) for i in range(checked)]
                               if spec.get('system_content') else []), 'wrong system content schedule')
    if timings or spec.get('system_timed'):
        require(len(timings) == checked and all(row[0] == str(i) and int(row[1]) > 0
                and (row[2] == '0' or (i >= completed and rc == 1))
                and row[2] in ('0', '-1') for i, row in enumerate(timings)), 'wrong system timing records')
        samples = [int(row[1]) for row in timings[:completed]]
        ordered = sorted(samples)
        result.update(ioctl_samples_ns=samples, ioctl_timed_clean_calls=len(samples),
                      ioctl_mean_ns=sum(samples)//len(samples) if samples else None,
                      ioctl_median_ns=(ordered[(len(ordered)-1)//2]+ordered[len(ordered)//2])//2 if samples else None,
                      ioctl_p95_ns=ordered[(95*len(ordered)+99)//100-1] if samples else None,
                      ioctl_max_ns=max(samples) if samples else None)
    cpu = re.findall(r'SYSTEM CPU: iteration=(\d+) ns=(\d+) voluntary=(\d+) involuntary=(\d+) resolution_ns=(\d+)', client_log)
    if spec.get('system_profile'):
        require(len(cpu) == checked and all(r[0] == str(i) and int(r[4]) > 0 for i,r in enumerate(cpu)),
                'incomplete/wrong system CPU records')
        require(len({r[4] for r in cpu}) == 1, 'changing CPU clock resolution')
        result.update(cpu_samples_ns=[int(r[1]) for r in cpu[:completed]],
                      voluntary_switches=[int(r[2]) for r in cpu[:completed]],
                      involuntary_switches=[int(r[3]) for r in cpu[:completed]],
                      cpu_clock_resolution_ns=int(cpu[0][4]),
                      elapsed_minus_cpu_ns=[int(t[1])-int(c[1]) for t,c in zip(timings[:completed],cpu[:completed])])
    else:
        require(not cpu, 'unexpected system CPU profile')
    if rc:
        result['first_record'] = next((line for line in client_log.splitlines() if line.startswith('FAIL:')), '')
        pixel = re.search(r'mismatch at \((\d+),(\d+)\)', result['first_record'])
        result['first_pixel'] = ','.join(pixel.groups()) if pixel else ''
    if spec.get('bounded_trace'):
        rows = re.findall(r'system-scale seq=(\d+) stage=(\w+) pixels=(\d+) mismatches=(\d+) efb_mismatches=(\d+) copy_mismatches=(\d+)', log)
        require(len(rows) == checked * 3, 'incomplete bounded stage capture')
        for i, row in enumerate(rows):
            stage, pixels = (('crop', 76800), ('horizontal', 153600), ('final', 307200))[i % 3]
            require(row[:3] == (str(i // 3 + 1), stage, str(pixels)), 'wrong bounded stage order/coverage')
        commands = re.findall(r'system-horizontal seq=(\d+) split=(\d+) quads=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
        require(len(commands) == checked, 'missing bounded horizontal commands')
        for i, row in enumerate(commands):
            require(row[:4] == (str(i + 1), '0', '320', '26574'),
                    'wrong bounded horizontal geometry')
        bad = [row for row in rows if any(map(int, row[3:]))]
        firsts = re.findall(r'system-scale-first seq=(\d+) stage=(\w+) x=(\d+) y=(\d+) actual=([0-9a-f]{4}) expected=([0-9a-f]{4}) argb=([0-9a-f]{8}) efb=([0-9a-f]{4})', log)
        require([(r[0], r[1]) for r in firsts] == [(r[0], r[1]) for r in bad],
                'missing/wrong bounded first-fault records')
        stops = re.findall(r'bounded-stop seq=(\d+) reason=oracle-mismatch errno=84', log)
        require((not bad and not stops) or
                (rc == 1 and stops == [str(checked)] and all(row[0] == str(checked) for row in bad)),
                'wrong bounded oracle stop')
        result.update(raw_errors=sum(int(row[4]) for row in rows),
                      copy_errors=sum(int(row[3]) for row in rows),
                      copy_differences=sum(int(row[5]) for row in rows),
                      stage_failures=bad, efb_precision='rgb565; crop CPU copy only',
                      stopped_on_error=bool(stops))
        if bad:
            result['first_record'] = next(line for line in log.splitlines() if 'system-scale-first' in line)
            pixel = re.search(r'x=(\d+) y=(\d+)', result['first_record'])
            result['first_pixel'] = ','.join(pixel.groups()) if pixel else ''
    return result

def audit_reduce(log, client_log, spec, requested, rc):
    audit_regression(log, client_log, requested, rc)
    rows = re.findall(r'scale-efb-full seq=(\d+) pixels=(\d+) copy_mismatches=(\d+) source_mismatches=(\d+)', log)
    spans = re.findall(r'reduce-span seq=(\d+) span=(\d+) quads=(\d+) fifo_before=(\d+) bytes=(\d+)', log)
    checked = len(rows)
    require(0 < checked <= requested and len(spans) == checked, 'incomplete reduction capture')
    for i,(row,command) in enumerate(zip(rows,spans)):
        require(row[:2] == (str(i+1),'38400'), 'wrong reduction coverage/order')
        seq,span,quads,before,size = map(int,command)
        require((seq,span,quads) == (i+1,spec['span'],120*(320//spec['span'])), 'wrong reduction geometry')
        require(before > 0 and size == before+8+80*quads and size < 65536, 'wrong reduction FIFO budget')
    patterns = re.findall(r'REDUCE CONTENT: iteration=(\d+) pattern=(\d+) seed=(\d+)', client_log)
    require(patterns == ([(str(i), str(i % 8), str(((i + 1) * 0x9e3779b9) & 0xffffffff)) for i in range(checked)]
                         if spec.get('content') else []), 'wrong reduction content schedule')
    progress = re.findall(r'REDUCE: (\d+)/(\d+) iterations', client_log)
    completed = len(progress)
    require(progress == [(str(i+1),str(requested)) for i in range(completed)], 'wrong reduction progress')
    bad = [r for r in rows if any(map(int,r[2:]))]
    require((rc == 0 and not bad and completed == checked == requested) or
            (rc == 1 and bad and completed == checked-1 and not any(any(map(int,r[2:])) for r in rows[:-1])), 'unexplained reduction result')
    return dict(case=spec['name'],status='FAIL' if bad else 'PASS',requested=requested,completed=completed,checked=checked,
                raw_errors=sum(int(r[3]) for r in rows),copy_errors=None,
                copy_differences=sum(int(r[2]) for r in rows),efb_precision='rgb565',first_pixel='',seconds=None,
                first_record=next((l for l in log.splitlines() if 'scale-efb-first' in l),''),
                final_draw_submissions=checked,final_command_bytes=sum(int(r[4]) for r in spans),
                max_final_command_bytes=max(int(r[4]) for r in spans))

def audit_system(log, client_log, spec, requested, rc):
    audit_regression(log, client_log, requested, rc)
    rows = re.findall(r'system-scale seq=(\d+) stage=(\w+) pixels=(\d+) mismatches=(\d+) efb_mismatches=(\d+) copy_mismatches=(\d+)', log)
    require(rows and len(rows) % 3 == 0, 'incomplete system stages')
    checked = len(rows) // 3
    require(0 < checked <= requested, 'system count outside request')
    spans = re.findall(r'system-span seq=(\d+) batch=(\d+) first=(\d+) end=(\d+) span=(\d+) quads=(\d+) fifo_before=(\d+) bytes=(\d+)', log)
    require(len(spans) == checked * 6, 'incomplete system span submissions')
    for i in range(checked):
        for j, (stage,pixels) in enumerate((('crop',76800), ('horizontal',153600), ('final',307200))):
            require(rows[i*3+j][:3] == (str(i+1),stage,str(pixels)), 'wrong system stage/coverage')
    for i, record in enumerate(spans):
        seq,batch,first,end,span,quads,before,size = map(int,record)
        require((seq,batch,first,end,span,quads) == (i//6+1,i%6,(i%6)*80,(i%6+1)*80,spec['span'],40*(640//spec['span'])), 'wrong system span geometry')
        require((before > 0 if batch == 0 else before == 0) and size == before+8+80*quads and size < 65536, 'wrong system FIFO budget')
    progress = re.findall(r'SYSTEM: (\d+)/(\d+) iterations', client_log)
    completed = len(progress)
    require(progress == [(str(i+1),str(requested)) for i in range(completed)], 'wrong system progress')
    bad = [r for r in rows if any(map(int,r[3:]))]
    require((rc == 0 and not bad and completed == checked == requested) or
            (rc == 1 and bad and completed == checked-1 and not any(any(map(int,r[3:])) for r in rows[:-3])), 'unexplained system result')
    return dict(case=spec['name'],status='FAIL' if bad else 'PASS',requested=requested,completed=completed,checked=checked,
                raw_errors=sum(int(r[4]) for r in rows),copy_errors=sum(int(r[3]) for r in rows),
                copy_differences=sum(int(r[5]) for r in rows),stage_failures=bad,first_pixel='',seconds=None,
                first_record=next((l for l in log.splitlines() if 'system-scale-first' in l),''),efb_precision='rgb565',
                final_draw_submissions=len(spans),final_command_bytes=sum(int(r[7]) for r in spans),
                max_final_command_bytes=max(int(r[7]) for r in spans))

def audit_offset(log, client_log, spec, requested, rc):
    audit_regression(log, client_log, requested, rc)
    rows = re.findall(r'offset-scale seq=(\d+) stage=(\w+) pixels=(\d+) mismatches=(\d+) efb_mismatches=(\d+) copy_mismatches=(\d+)', log)
    require(rows and len(rows) % 3 == 0, 'incomplete offset stages')
    checked = len(rows) // 3
    require(0 < checked <= requested, 'offset count outside request')
    spans = re.findall(r'offset-span seq=(\d+) span=(\d+) quads=(\d+) vertex_bytes=(\d+) fifo_before=(\d+)', log)
    commands = re.findall(r'offset-final seq=(\d+) split=(\d+) quads=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
    require(len(spans) == len(commands) == checked, 'missing offset command records')
    quads = 79 * (256 // spec['span'])
    for i in range(checked):
        for j, (stage, pixels) in enumerate((('crop',20145), ('horizontal',20224), ('final',65536))):
            require(rows[i*3+j][:3] == (str(i+1), stage, str(pixels)), 'wrong offset stage/coverage')
        seq, span, q, vertices, before = map(int, spans[i])
        require((seq,span,q,vertices) == (i+1,spec['span'],quads,80*quads), 'wrong offset span geometry')
        require(tuple(map(int, commands[i][:4])) == (i+1,1,quads,before+8+vertices), 'wrong offset final command budget')
        require(before+8+vertices < 65536, 'oversized offset stream')
    progress = re.findall(r'OFFSET: (\d+)/(\d+) iterations', client_log)
    completed = len(progress)
    require(progress == [(str(i+1),str(requested)) for i in range(completed)], 'wrong offset client progress')
    bad = [r for r in rows if any(map(int,r[3:]))]
    require((rc == 0 and not bad and completed == checked == requested) or
            (rc == 1 and bad and completed == checked-1 and not any(any(map(int,r[3:])) for r in rows[:-3])),
            'unexplained offset failure or incomplete pass')
    return dict(case=spec['name'], status='FAIL' if bad else 'PASS', requested=requested,
                completed=completed, checked=checked, sequences=checked,
                raw_errors=sum(int(r[4]) for r in rows), copy_errors=sum(int(r[3]) for r in rows),
                copy_differences=sum(int(r[5]) for r in rows), stage_failures=bad,
                first_record=next((l for l in log.splitlines() if 'offset-first' in l), ''), first_pixel='',
                seconds=None, efb_precision='rgb565', final_draw_submissions=checked,
                final_command_bytes=sum(int(r[3]) for r in commands),
                max_final_command_bytes=max(int(r[3]) for r in commands))

def audit_native_plain(log, client_log, spec, requested, rc):
    if spec.get('use_default'):
        require('parameter scale_identity_quad verified Y' in log, 'default parameter not verified')
    require('CPU console restored' in log, 'native cleanup missing')
    require(not re.search(r'\btimeout\b|\bstall\b|\bOops:|\bBUG:|Call Trace:', log, re.I), 'native kernel fault')
    require('native-scale seq=' not in log, 'unexpected native tracing in production control')
    content = re.findall(r'native-content frame=(\d+) pattern=(\d+) seed=([0-9a-f]{8})', client_log)
    expected_content = [(str(frame), str(frame % 8), f'{((frame + 1) * 0x9e3779b9) & 0xffffffff:08x}')
                        for frame in range((sequences + 3) // 4)] if spec.get('content') else []
    require(content == expected_content, 'wrong or incomplete native content schedule')
    passed = re.findall(r'PASS offscreen format=xrgb8888-native-tiled frames=(\d+) pixels=(\d+)', client_log)
    failures = re.findall(r'frame (\d+) mismatch at \((\d+),(\d+)\): got=([0-9a-f]+) expected=([0-9a-f]+)', client_log)
    if rc == 0:
        require(passed == [(str(requested), str(requested * 307200))] and not failures, 'incomplete production client pass')
        completed = requested
    else:
        require(rc == 1 and len(failures) == 1 and not passed, 'unexplained production client failure')
        completed = int(failures[0][0])
        require(completed < requested, 'production failure outside frame range')
    return dict(case=spec['name'], status='PASS' if rc == 0 else 'FAIL', requested=requested,
                completed=completed, checked=completed + int(rc != 0), raw_errors=None,
                copy_errors=0 if rc == 0 else 1, first_pixel=','.join(failures[0][1:3]) if failures else '',
                first_record=str(failures[0]) if failures else '', seconds=None,
                efb_precision='not sampled; full client RGB565 oracle', traced=False)

def audit_native(log, client_log, spec, requested, rc):
    if not spec.get("traced", True):
        return audit_native_plain(log, client_log, spec, requested, rc)
    scales = re.findall(r'native-scale seq=(\d+) origin=(\d+,\d+) stage=(\w+) pixels=(\d+) mismatches=(\d+) efb_mismatches=(\d+) copy_mismatches=(\d+)', log)
    priors = re.findall(r'native-prior seq=(\d+) snapshot=(\d+) pixels=(\d+) texture_mismatches=(\d+) efb_mismatches=(\d+)', log)
    commands = re.findall(r'native-(horizontal|final) seq=(\d+) split=(\d+) quads=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
    states = re.findall(r'native-state seq=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
    require(bool(scales), 'missing native records')
    sequences = max(int(r[0]) for r in scales)
    stop_markers = re.findall(r'native-stop seq=(\d+) reason=([a-z-]+) errno=(\d+)', log)
    stopped = bool(stop_markers)
    require(not stopped or spec.get('stop_on_error'), 'unexpected native stop')
    require(0 < sequences <= requested * 4 and (sequences % 4 == 0 or stopped),
            'incomplete/out-of-range native frames')
    require(not stopped or stop_markers == [(str(sequences), 'oracle-mismatch', '84')],
            'wrong native stop boundary/reason')
    injections = re.findall(r'native-test-mismatch seq=(\d+) stage=final x=0 y=0 expected_xor=0001', log)
    injected = spec.get('inject_sequence', 0)
    require(injections == ([str(injected)] if injected else []), 'wrong native injected oracle control')

    require(len(scales) == sequences * 3 and len(priors) == sequences * 3 and
            len(commands) == sequences * 2 and len(states) == sequences, 'incomplete native capture')
    batch_rows = spec.get('batch_rows', 80)
    batches = 240 // batch_rows
    spans = re.findall(r'native-span seq=(\d+) batch=(\d+) first=(\d+) end=(\d+) span=(\d+) quads=(\d+) bytes=(\d+)', log)
    require(len(spans) == (sequences * batches if spec.get('span') else 0), 'incomplete native span batches')
    for index, record in enumerate(spans):
        seq, batch = index // batches + 1, index % batches
        span = spec['span']
        quads = batch_rows * (320 // span)
        require(tuple(map(int, record)) == (seq, batch, batch * batch_rows, (batch + 1) * batch_rows, span, quads, 8 + 80 * quads),
                'wrong native span geometry/command budget')
    orders = re.findall(r'native-span-order seq=(\d+) batch=(\d+) order=([a-z-]+) scope=([a-z-]+)', log)
    expected_orders = [(str(seq), str(batch), 'reverse', 'within-batch')
                       for seq in range(1, sequences + 1) for batch in range(batches)] if spec.get('reverse') else []
    require(orders == expected_orders, 'wrong native span traversal order')
    origins = ['0,0', '320,0', '0,240', '320,240']
    hashes = {}
    for n in range(sequences):
        origin = origins[n % 4]
        for j, stage in enumerate(('crop', 'horizontal', 'final')):
            r = scales[n * 3 + j]
            require(r[:4] == (str(n + 1), origin, stage, str(307200 if j == 2 else 76800)), 'wrong native stage/order')
            r = priors[n * 3 + j]
            require(r[:3] == (str(n + 1), str(j), '307200'), 'wrong native prior/order')
        for j, stage in enumerate(('horizontal', 'final')):
            r = commands[n * 2 + j]
            quads = (1 if spec.get('identity') else 640) if j == 0 else (batch_rows * (320 // spec['span']) if spec.get('span') else 1 if spec['single'] else 480)
            require(r[:4] == (stage, str(n + 1), '1', str(quads)), 'wrong native command geometry')
            require(int(r[4]) < 65536, 'oversized native command')
            if j == 1:
                require(int(r[4]) == 8 + 80 * quads, 'wrong native final command length')
            key = (stage, origin)
            require(hashes.setdefault(key, r[4:]) == r[4:], 'native command hash changed')
        require(states[n][0] == str(n + 1), 'native state sequence missing')
    require('CPU console restored' in log, 'native cleanup missing')
    require(not re.search(r'\btimeout\b|\bstall\b|\bOops:|\bBUG:|Call Trace:', log, re.I), 'native kernel fault')
    failures = [r for r in scales if any(map(int, r[4:]))]
    prior_failures = [r for r in priors if any(map(int, r[3:]))]
    content = re.findall(r'native-content frame=(\d+) pattern=(\d+) seed=([0-9a-f]{8})', client_log)
    expected_content = [(str(frame), str(frame % 8), f'{((frame + 1) * 0x9e3779b9) & 0xffffffff:08x}')
                        for frame in range((sequences + 3) // 4)] if spec.get('content') else []
    require(content == expected_content, 'wrong or incomplete native content schedule')
    passed = re.findall(r'PASS offscreen format=xrgb8888-native-tiled frames=(\d+) pixels=(\d+)', client_log)
    if rc == 0:
        require(passed == [(str(requested), str(requested * 307200))] and sequences == requested * 4, 'incomplete native client pass')
    else:
        require(rc == 1 and not passed and bool(failures or prior_failures), 'unexplained native client failure')
    bad = bool(failures or prior_failures)
    first = next((l for l in log.splitlines() if 'native-first' in l), '')
    first_bad_sequence = min((int(r[0]) for r in failures + prior_failures), default=None)
    clean_frames = (first_bad_sequence - 1) // 4 if first_bad_sequence is not None else sequences // 4
    if spec.get('stop_on_error'):
        require(stopped == bad, 'native stop missing or unexplained')
        if stopped:
            require(rc == 1 and first_bad_sequence == sequences, 'native continued after first bad sequence')
    if injected:
        require(stopped and first_bad_sequence == injected and len(failures) == 1 and
                failures[0][2] == 'final' and failures[0][4:] == ('1', '1', '0') and not prior_failures,
                'injected control has missing or additional faults')
    return dict(case=spec['name'], status='FAIL' if bad else 'PASS', requested=requested,
                completed=clean_frames, checked=(sequences + 3) // 4,
                stopped_on_error=stopped, injected_oracle_error=bool(injected),
                client_frames_completed=(sequences - 1) // 4 if rc != 0 else sequences // 4, client_reported_pass=bool(passed),
                first_error_sequence=first_bad_sequence,
                first_error_frame=(first_bad_sequence - 1) // 4 + 1 if first_bad_sequence is not None else None,
                raw_errors=sum(int(r[5]) for r in scales), copy_errors=sum(int(r[4]) for r in scales),
                copy_differences=sum(int(r[6]) for r in scales), background_errors=sum(int(r[3])+int(r[4]) for r in priors),
                first_pixel='', first_record=first, seconds=None, sequences=sequences, efb_precision='rgb565',
                stage_failures=failures, prior_failures=prior_failures,
                final_draw_submissions=sequences * (batches if spec.get('span') else 1),
                final_command_bytes=(sum(int(r[6]) for r in spans) if spans else
                                     sum(int(r[4]) for r in commands if r[0] == 'final')),
                max_final_command_bytes=(max(int(r[6]) for r in spans) if spans else
                                         max(int(r[4]) for r in commands if r[0] == 'final')),
                command_variants={str(k): v for k, v in hashes.items()})

def sha(data):
    return hashlib.sha256(data).hexdigest()

def require(condition, message):
    if not condition:
        raise ValueError(message)

def audit(log, spec, requested, runner_status):
    """Reject incomplete logs or inconsistent results; pixel failures are valid results."""
    starts = re.findall(r'efb-clear-start iterations=(\d+) width=640 height=480 primitive=(\d+)', log)
    require(starts == [(str(requested), str(int(bool(spec['flags']))))], 'missing/duplicate/wrong test start')
    results = re.findall(r'efb-clear-result ret=(-?\d+) completed=(\d+) requested=(\d+)', log)
    require(len(results) == 1, 'missing or duplicate result')
    ret, completed, count = map(int, results[0])
    require(count == requested, 'iteration count differs from request')
    rows = re.findall(r'efb-clear iteration=(\d+) expected=([0-9a-f]+) pixels=(\d+) efb_mismatches=(\d+) copy_mismatches=(\d+) copy_differences=(\d+)', log)
    require(len(rows) <= requested, 'iteration ceiling exceeded')
    bad_rows = []
    totals = [0, 0, 0]
    for index, row in enumerate(rows):
        require(int(row[0]) == index and int(row[2]) == 307200, 'missing/out-of-order pixel checks')
        expected = ('00ffffff' if index % 2 or
                    any(f in spec['flags'] for f in ('no_color_write', 'z_never', 'no_draw', 'alpha_never', 'logic_set')) or
                    ('alpha_threshold' in spec['flags'] and 'alpha_threshold_pass' not in spec['flags'])
                    else '00ff0000')
        if 'logic_invert' in spec['flags']:
            expected = '00ffffff' if 'logic_black' in spec['flags'] else '00000000'
        if 'logic_clear' in spec['flags']:
            expected = '00000000'
        require(row[1] == expected, 'wrong color sequence')
        counts = list(map(int, row[3:]))
        totals = [x + y for x, y in zip(totals, counts)]
        if any(counts):
            bad_rows.append(index)
    observations = re.findall(r'efb-batch-snapshot iteration=(\d+) batch=(\d+) end=(\d+) pixels=307200 mismatches=(\d+)', log)
    if 'batch_snapshot' in spec['flags']:
        require(len(observations) == len(rows) * 30, 'incomplete batch snapshots')
        for n, observation in enumerate(observations):
            i, b, end, bad = map(int, observation)
            require((i, b, end) == (n // 30, n % 30, (n % 30 + 1) * 16), 'wrong snapshot geometry/order')
            if bad and i not in bad_rows:
                bad_rows.append(i)
        bad_rows.sort()
    else:
        require(not observations, 'unexpected batch snapshots')
    for flag, marker in [('z_never', f"efb-reject-depth compare=NEVER early={int('late_z' not in spec['flags'])} expected=00ffffff"),
                         ('alpha_never', 'efb-reject-alpha compare=NEVER late=1 expected=00ffffff'),
                         ('no_draw', 'efb-no-draw batches=30 expected=00ffffff')]:
        require(log.count(marker) == int(flag in spec['flags']), 'wrong rejection oracle mode')
    require(log.count('efb-reject-depth ') == int('z_never' in spec['flags']), 'unexpected depth-rejection marker')
    thresholds = re.findall(r'efb-alpha-threshold compare=(LESS|GEQUAL) ref=128 vertex_alpha=(255|0-255) late=1', log)
    require(thresholds == ([('GEQUAL' if 'alpha_threshold_pass' in spec['flags'] else 'LESS',
                            '0-255' if 'alpha_mixed' in spec['flags'] else '255')]
                           if 'alpha_threshold' in spec['flags'] else []), 'wrong alpha threshold mode')
    mixed = 'alpha_mixed' in spec['flags']
    mixed_markers = re.findall(r'efb-alpha-mixed rows=2 reverse=([01]) phase_period=4 accepted_pixels=153600 rejected_pixels=153600', log)
    require(mixed_markers == ([str(int('alpha_mixed_reverse' in spec['flags']))] if mixed else []),
            'wrong mixed-alpha coverage mode')
    partitions = re.findall(r'efb-alpha-partition iteration=(\d+) accepted_raw=(\d+) rejected_raw=(\d+) accepted_copy=(\d+) rejected_copy=(\d+)', log)
    require(len(partitions) == (len(rows) if mixed else 0), 'incomplete mixed-alpha partition checks')
    partition_totals = [0, 0, 0, 0]
    for n, partition in enumerate(partitions):
        i, ar, rr, ac, rc = map(int, partition)
        require(i == n and ar + rr == int(rows[n][3]) and ac + rc == int(rows[n][4]),
                'mixed-alpha partition totals disagree')
        require(all(v <= 153600 for v in (ar, rr, ac, rc)), 'mixed-alpha partition count out of range')
        partition_totals = [a + b for a, b in zip(partition_totals, (ar, rr, ac, rc))]
    focus_markers = re.findall(r'efb-focus left=(\d+) top=(\d+) width=(\d+) height=4 colored_pixels=(\d+) black_pixels=(\d+)', log)
    focus_width = 536 if 'focus_wide' in spec['flags'] else spec.get('focus_width', 8)
    focus_top = spec.get('focus_top', 52)
    focus_geometry = tuple(map(str, (536 - focus_width, focus_top, focus_width, focus_width * 4, 307200 - focus_width * 4)))
    require(focus_markers == ([focus_geometry] if 'focus' in spec['flags'] else []), 'wrong focused coverage oracle')
    viewports = re.findall(r'efb-focus-viewport shift_y=(\d+) vertex_top=(\d+) physical_top=52 width=640 height=480 scissor_top=0 scissor_height=480', log)
    shift = spec.get('viewport_shift',0)
    require(viewports == ([(str(shift),str(52-shift))] if 'focus_viewport' in spec['flags'] else []), 'wrong compensated viewport')
    pads = re.findall(r'efb-focus-pad pads=(\d+) real=(\d+) order=([a-z]+)', log)
    require(pads == ([('16','2','prefix')] if 'focus_pad' in spec['flags'] else []), 'wrong focused padding')
    focus_span = spec.get('focus_span', 64)
    focus_right = 'focus_right' in spec['flags']
    remainder = (focus_width - 1) % focus_span + 1
    anchors = re.findall(r'efb-focus-anchor side=([a-z]+) remainder=(\d+)', log)
    require(anchors == ([('right', str(remainder))] if focus_right else []), 'wrong focused anchor')
    boundary = 536 - focus_width + (remainder if focus_right else focus_span)
    splits = re.findall(r'efb-focus-split boundary=(\d+) span=(\d+) quads=(\d+) order=([a-z-]+)', log)
    require(splits == ([(str(boundary), str(focus_span), str(spec['quads']), ('row-major','reverse','column-major')[spec.get('focus_order',0)])] if 'focus_split' in spec['flags'] else []),
            'wrong focused split geometry')
    full_splits = re.findall(r'efb-full-split span=(\d+) strips_per_batch=(\d+) pieces_per_strip=(\d+) pads=(\d+) quads=(\d+) order=([a-z-]+)', log)
    require(full_splits == ([('64', '8', '10', '8', '88', 'row-major')] if 'full_split' in spec['flags'] else []),
            'wrong full-screen split geometry')
    storage = re.findall(r'efb-storage format=RGBA6_Z24 multisample=0 oracle=black-white', log)
    require(len(storage) == int('rgba6' in spec['flags']), 'wrong EFB storage format')
    logic_modes = re.findall(r'efb-logic operation=(SET|COPY|INVERT|CLEAR) color_update=1 alpha_update=1', log)
    require(logic_modes == (['CLEAR' if 'logic_clear' in spec['flags'] else
                             'INVERT' if 'logic_invert' in spec['flags'] else
                             'SET' if 'logic_set' in spec['flags'] else 'COPY']
                            if 'logic' in spec['flags'] else []), 'wrong logic output mode')
    write_masks = re.findall(r'efb-write-mask color=0 alpha=0 expected=00ffffff', log)
    require(len(write_masks) == int('no_color_write' in spec['flags']), 'wrong color-write oracle mode')
    background_modes = re.findall(r'efb-background-mode expected=([0-9a-f]+)', log)
    require(background_modes == (['00000000' if 'logic_black' in spec['flags'] else '00ffffff']
                                 if 'white_background' in spec['flags'] else []),
            'wrong background oracle mode')
    backgrounds = re.findall(r'efb-background iteration=(\d+) pixels=307200 mismatches=(\d+)', log)
    bad_backgrounds = [int(i) for i, n in backgrounds if int(n)]
    if spec['flags']:
        require([int(i) for i, _ in backgrounds] == list(range(len(backgrounds))), 'background checks not sequential')
        require(len(backgrounds) == len(rows) + int(bool(bad_backgrounds)), 'incomplete background checks')
    else:
        require(not backgrounds, 'unexpected primitive background checks')
    faults = [line for line in log.splitlines() if re.search(r'\btimeout\b|\bstall\b|\bOops:|\bBUG:|Call Trace:', line, re.I)]
    require(not faults, 'kernel fault or timeout: ' + '; '.join(faults[:2]))
    require('CPU console restored' in log, 'console cleanup notice absent')
    if ret == 0:
        require(runner_status == 0 and completed == requested == len(rows), 'inconsistent success')
        require(not bad_rows and not bad_backgrounds, 'success despite pixel errors')
        status = 'PASS'
    else:
        require(ret == -5 and runner_status != 0, f'non-pixel test error: ret={ret}, runner={runner_status}')
        require((bad_rows == [len(rows) - 1] and not bad_backgrounds and completed == len(rows) - 1)
                or (not bad_rows and bad_backgrounds == [len(rows)] and completed == len(rows)),
                'failure did not stop at first bad iteration')
        status = 'FAIL'
    stripe_markers = re.findall(r'efb-clear-stripes rows=(\d+) colors=red-white', log)
    require(stripe_markers == ([str(2 if 'two_rows' in spec['flags'] else 1)]
                              if 'stripe_colors' in spec['flags'] else []),
            'wrong stripe-color oracle mode')
    if 'interior_only' in spec['flags']:
        require('colored_pixels=268800 black_edge_pixels=38400 batch_rows=16' in log, 'interior oracle marker absent')
        if 'interior_even' in spec['flags']:
            require('batch_rows=16 interior_even=1' in log, 'even alignment marker absent')
        else:
            require('batch_rows=16 interior_even=1' not in log, 'unexpected even alignment')
    else:
        require('efb-clear-pattern' not in log, 'unexpected reduced-coverage oracle')
    batches = re.findall(r'efb-primitive-batch iteration=(\d+) batch=(\d+) first=(\d+) end=(\d+) quads=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
    primitives = re.findall(r'efb-primitive iteration=(\d+) quads=(\d+) bytes=(\d+) hash=([0-9a-f]+)', log)
    hashes = {}
    records = []
    state_bytes = (388 + 5 * sum(flag in spec['flags'] for flag in ('late_z', 'z_test', 'no_color_write', 'z_never', 'alpha_never', 'alpha_threshold', 'logic', 'rgba6'))
                   + 25 * ('constant' in spec['flags']) + 29 * ('focus_viewport' in spec['flags']))
    if 'batch' in spec['flags']:
        batch_count = spec['batches']
        require(len(batches) == len(rows) * batch_count and not primitives, 'incomplete batch records')
        for index, row in enumerate(batches):
            i, batch, first, end, quads, size = map(int, row[:6])
            want_first = spec.get('focus_top', 52) if 'focus' in spec['flags'] else index % batch_count * 16
            want_end = want_first + 4 if 'focus' in spec['flags'] else want_first + 16
            require((i, batch, first, end, quads) == (index // batch_count, index % batch_count,
                                                     want_first, want_end, spec['quads']), 'wrong batch geometry/count')
            require(size == (state_bytes if batch == 0 else 8) + 48 * quads
                    + 114 * ('scissor_rows' in spec['flags'])
                    - 3 * ('no_draw' in spec['flags']), 'wrong authored command length')
            records.append((i, batch, row[-1]))
    elif spec['batches'] == 1:
        require(len(primitives) == len(rows) and not batches, 'incomplete primitive records')
        for index, row in enumerate(primitives):
            i, quads, size = map(int, row[:3])
            require((i, quads, size) == (index, spec['quads'], state_bytes + 48 * spec['quads']), 'wrong primitive geometry/count')
            records.append((i, 0, row[-1]))
    else:
        require(not primitives and not batches, 'unexpected draw commands in clear test')
    for i, batch, digest in records:
        key = (i % (4 if mixed else 2), batch)
        require(hashes.setdefault(key, digest) == digest, 'command hash changed within the same color/geometry')
    cpu_writes = re.findall(r'efb-cpu-write iteration=(\d+) pixels=307200 expected=([0-9a-f]+)', log)
    if 'cpu_write' in spec['flags']:
        require(cpu_writes == [(str(i), '00ffffff' if i % 2 else '00ff0000')
                               for i in range(len(rows))], 'missing/wrong CPU fill records')
    else:
        require(not cpu_writes, 'unexpected CPU fill')
    first = next((line for line in log.splitlines() if 'efb-clear-first ' in line or 'efb-background-first ' in line), '')
    first_pixel = ''
    match = re.search(r'x=(\d+) y=(\d+)', first)
    if match:
        first_pixel = ','.join(match.groups())
    start = re.search(r'\[\s*([0-9.]+)\] gcn-gx: efb-clear-start', log)
    end = re.search(r'\[\s*([0-9.]+)\] gcn-gx: efb-clear-result', log)
    duration = round(float(end[1]) - float(start[1]), 6) if start and end else None
    return dict(case=spec['name'], status=status, requested=count, completed=completed,
                alpha_partition_totals=partition_totals if mixed else None,
                checked=len(rows), raw_errors=totals[0], copy_errors=totals[1],
                copy_differences=totals[2], batch_observations_bad=sum(int(o[3]) for o in observations), background_errors=sum(int(n) for _, n in backgrounds),
                first_pixel=first_pixel, first_record=first, seconds=duration,
                efb_precision='rgba6-expanded-rgb888' if 'rgba6' in spec['flags'] else 'rgb888',
                first_batch_record=next((l for l in log.splitlines() if 'efb-batch-first ' in l), ''),
                draw_submissions=len(records), pixels_per_oracle=len(rows) * 307200,
                command_hashes={f'{parity}:{batch}': value for (parity, batch), value in hashes.items()})

class Remote:
    def __init__(self, host, directory):
        self.host = host if '@' in host else 'root@' + host
        self.directory = directory
        self.command = ['ssh', '-o', 'ConnectTimeout=8', '-o', 'BatchMode=yes',
                        '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3',
                        '-o', 'IdentitiesOnly=yes', '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa',
                        '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
                        '-o', 'ForwardX11=no', '-o', 'LogLevel=ERROR',
                        '-i', os.environ.get('WII_SSH_KEY', str(Path.home() / '.ssh/id_rsa')), self.host]
    def run(self, command, timeout=120):
        result = subprocess.run(self.command + [command], capture_output=True, timeout=timeout)
        with (self.directory / 'ssh.log').open('ab') as stream:
            stream.write(result.stderr)
        require(result.returncode == 0, 'SSH operation failed: ' + result.stderr.decode(errors='replace')[-500:])
        return result.stdout
    def state(self):
        data = self.run("cat /proc/sys/kernel/printk; cat /proc/sys/kernel/random/boot_id; "
                        "if test -d /sys/module/gcn_gx; then echo loaded; else echo unloaded; fi; "
                        "df -Pk /tmp | tail -n 1") .decode().splitlines()
        require(len(data) == 4, 'unexpected remote state output')
        return dict(printk=data[0], boot=data[1], module=data[2], free_kib=int(data[3].split()[3]))

def write_summary(directory, results, metadata):
    (directory / 'summary.json').write_text(json.dumps(dict(metadata=metadata, results=results), indent=2) + '\n')
    fields = ['case', 'status', 'completed', 'requested', 'checked', 'raw_errors', 'copy_errors',
              'copy_differences', 'background_errors', 'first_pixel', 'seconds', 'error']
    with (directory / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(results)
    lines = ['| Case | Result | Completed | Raw errors | Copy errors | First pixel | Seconds |',
             '|---|---|---:|---:|---:|---|---:|']
    for r in results:
        lines.append(f"| {r['case']} | {r['status']} | {r.get('completed', '?')}/{r['requested']} | {r.get('raw_errors', '?')} | {r.get('copy_errors', '?')} | {r.get('first_pixel', '')} | {r.get('seconds', '')} |")
    lines += ['', 'FAIL means a verified test failure. ERROR means infrastructure, capture, or audit failure.',
              'Client-visible mismatches stop a case; traced native intermediate errors may be found only in the post-run audit. These runs do not estimate a stable failure rate.',
              'Interior cases intentionally leave 38,400 black pixels per frame; they are not full-screen fixes.']
    if any(r.get('sequences') for r in results):
        lines += ['Native EFB comparisons use RGB565 output precision; geometry diagnostics check all 24 RGB bits.',
                  'For native failures, Completed counts full clean frames before the first audited error; Checked and client completion are recorded separately in result.json.']
    (directory / 'summary.md').write_text('\n'.join(lines) + '\n')

def run_client(command, log_path, label, requested, timeout, native=False):
    with log_path.open('wb') as output:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        started = time.monotonic()
        done = 0
        try:
            while process.poll() is None:
                require(time.monotonic() - started < timeout, 'case timeout; inspect remote state before retrying')
                with log_path.open('rb') as source:
                    source.seek(max(0, log_path.stat().st_size - 2048))
                    data = source.read()
                    matches = re.findall(rb'offscreen frame=(\d+)' if native else rb'(\d+)/\d+ iterations', data)
                    display_frames = re.findall(rb'gcn-kms-flip-test: frame=(\d+)',data)
                    if display_frames:
                        done = min(requested, int(display_frames[-1])+1)
                observed = int(matches[-1]) + int(native) if matches else 0
                if native:
                    # A content marker precedes its frame: only earlier frames are complete.
                    content_frames = re.findall(rb'native-content frame=(\d+)', data)
                    passes = re.findall(rb'PASS offscreen format=\S+ frames=(\d+)', data)
                    observed = max(observed, int(content_frames[-1]) if content_frames else 0,
                                   int(passes[-1]) if passes else 0)
                done = min(requested, max(done, observed))
                filled = done * 24 // requested
                print(f'\r{label} [{"#" * filled:<24}] {done}/{requested}', end='', flush=True)
                time.sleep(1)
            return process.returncode
        except BaseException:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            raise
        finally:
            print()

def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog='Exit codes: 0 all PASS, 1 verified pixel FAIL(s), 2 suite ERROR. Pixel failures do not stop later cases.')
    parser.add_argument('--host', default=os.environ.get('WII_SSH_HOST', '10.3.10.59'))
    parser.add_argument('--module', type=Path)
    parser.add_argument('--render-client', type=Path, default=Path('/media/anolis/dev/wii-gcn-native-final-clients/wii-gcn-render-test'))
    parser.add_argument('--native-client', type=Path, default=Path('/media/anolis/dev/wii-gcn-offscreen-clients/wii-gcn-kms-flip-test'))
    parser.add_argument('--iterations', type=int, default=4, help='per-case ceiling, 1..1000 (default: 4)')
    parser.add_argument('--cases', default='geometry', help='comma-separated names, geometry (default), pipeline, or all')
    parser.add_argument('--output', type=Path, help='new evidence directory; must not already exist')
    parser.add_argument('--allow-dirty', action='store_true')
    parser.add_argument('--timeout', type=int, default=1200, help='maximum wall seconds per case (default: 1200)')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--dry-run', action='store_true', help='print plan without contacting the Wii')
    args = parser.parse_args()
    if args.list:
        for spec in CASES:
            print(f"{spec['name']:<18} {spec['batches']:2} draw submissions/iteration; {spec['quads']} quads/submission")
        return 0
    require(1 <= args.iterations <= 1000, 'iterations must be 1..1000 per case')
    require(args.timeout > 0, 'timeout must be positive')
    names = ([c['name'] for c in CASES if args.cases == 'all' or
              bool(c.get('experimental')) == (args.cases == 'pipeline')]
             if args.cases in ('all', 'geometry', 'pipeline') else args.cases.split(','))
    require(len(set(names)) == len(names) and names, 'empty or duplicate case selection')
    lookup = {c['name']: c for c in CASES}
    require(all(n in lookup for n in names), 'unknown case; use --list')
    selected = [lookup[n] for n in names]
    require(args.iterations <= 64 or not any(c.get('system_sched_loop') for c in selected),
            'whole-loop scheduler capture is limited to 64 iterations')
    require(args.iterations <= 8 or not any(c.get('deferred') for c in selected),
            'deferred capture is limited to eight iterations for the 16 KiB kernel log')
    require(args.iterations >= 3 or not any(c.get('presentation') for c in selected),
            'display pacing needs at least three frames')
    total = sum(1 if c.get('regression') else args.iterations for c in selected)
    print(f'{len(selected)} cases, up to {total} total iterations/suite runs; frame/geometry ceiling {args.iterations}.')
    if args.dry_run:
        for spec in selected:
            print(spec['name'] + ': ' + ' '.join(spec['flags']))
        return 0
    require(args.module is not None and args.module.is_file(), '--module must name a built module with the current diagnostic parameters')
    status = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT).decode()
    require(args.allow_dirty or not status, 'dirty tree: use --allow-dirty to archive and test it')
    directory = (args.output or Path('/media/anolis/dev') / ('wii-gcn-matrix-' + time.strftime('%Y%m%d-%H%M%S'))).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    module = directory / 'module.ko'
    module.write_bytes(args.module.read_bytes())
    if any(c.get('native') for c in selected):
        (directory / 'native-client').write_bytes(args.native_client.read_bytes())
    if any(c.get('presentation') or c.get('bounded') or c.get('regression') or c.get('offset') or c.get('system') or c.get('reduce') for c in selected):
        (directory / 'render-client').write_bytes(args.render_client.read_bytes())
    snapshots = directory / 'sources'
    snapshots.mkdir()
    for path in ['tools/wii-gcn-efb-matrix.py', 'tools/wii-gcn-render-cycle.sh',
                 'tools/wii-gcn-efb-progress.sh', 'tools/wii-gcn-efb-primitive-result.sh',
                 'tools/wii-gcn-efb-clear-result.sh', 'drivers/video/fbdev/gcn-gx.c', 'tools/wii-gcn-kms-flip-test.c', 'tools/wii-gcn-render-test.c']:
        (snapshots / Path(path).name).write_bytes((ROOT / path).read_bytes())
    (directory / 'git-status.txt').write_text(status)
    (directory / 'git-diff.patch').write_bytes(subprocess.check_output(['git', 'diff', 'HEAD'], cwd=ROOT))
    metadata = dict(host=args.host, module_sha256=sha(module.read_bytes()), iterations=args.iterations,
                    cases=names, git_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT).decode().strip(),
                    started=time.strftime('%Y-%m-%dT%H:%M:%S%z'), selected=selected)
    remote = Remote(args.host, directory)
    results = []
    lock = False
    try:
        remote.run('mkdir /tmp/wii-gcn-efb-matrix.lock')
        lock = True
        baseline = remote.state()
        require(baseline['module'] == 'unloaded', 'gcn_gx is already loaded; refusing to disturb an active test')
        metadata['baseline'] = baseline
        write_summary(directory, results, metadata)
        for index, spec in enumerate(selected, 1):
            case_dir = directory / spec['name']
            case_dir.mkdir()
            tag = '/tmp/gcn-matrix-' + uuid.uuid4().hex
            started_capture = False
            result = dict(case=spec['name'], status='ERROR', requested=args.iterations)
            try:
                state = remote.state()
                require(state['module'] == 'unloaded' and state['boot'] == baseline['boot'] and state['printk'] == baseline['printk'], 'remote baseline changed')
                batch_log_bytes = 450 if 'batch_snapshot' in spec['flags'] else 150
                needed = 1024 + args.iterations * (5200 if spec.get('native') else
                            spec['batches'] * batch_log_bytes + 800) // 1024
                require(state['free_kib'] >= needed, f'not enough /tmp log space: need {needed} KiB, have {state["free_kib"]}')
                (case_dir / 'before.json').write_text(json.dumps(state, indent=2) + '\n')
                if spec.get('deferred'):
                    remote.run(f"printf '<6>gcn-matrix-deferred begin {tag}\\n' > /dev/kmsg")
                else:
                    remote.run(f'nohup dmesg -W > {tag}.log 2>&1 < /dev/null & echo $! > {tag}.pid; sleep 0.2; kill -0 "$(cat {tag}.pid)"')
                    started_capture = True
                flags = ' '.join(f'efb_primitive_{flag}={int(flag in spec["flags"])}' for flag in FLAGS)
                flags += f' efb_primitive_focus_width={spec.get("focus_width", 8)}'
                flags += f' efb_primitive_focus_top={spec.get("focus_top", 52)}'
                flags += f' efb_primitive_focus_viewport_shift={spec.get("viewport_shift",0)}'
                flags += f' efb_primitive_focus_order={spec.get("focus_order",0)}'
                flags += f' efb_primitive_focus_span={spec.get("focus_span",64)}'
                client = 'tools/wii-gcn-efb-primitive-result.sh' if spec['flags'] else 'tools/wii-gcn-efb-clear-result.sh'
                command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                           '--client', client, '--client-args', f'{args.iterations} {spec["checker"]}'.strip(),
                           '--module-args', f'efb_clear_iterations={args.iterations} ' + flags]
                if spec.get('native'):
                    native_flags = ('scale_system_split=1 scale_offset_split=1 scale_native_trace=1 '
                                    'scale_native_preserve_fence=1 scale_native_split=1 '
                                    'scale_native_horizontal_split=1 scale_native_state_fence=1 '
                                    f'scale_native_single_quad={int(spec["single"])} '
                                    f'scale_identity_quad={int(spec.get("identity", False))} '
                                    f'scale_native_span={spec.get("span", 0)} '
                                    f'scale_native_batch_rows={spec.get("batch_rows", 80)} '
                                    f'scale_native_reverse={int(spec.get("reverse", False))} '
                                    f'scale_native_stop_on_error={int(spec.get("stop_on_error", False))} '
                                    f'scale_native_test_mismatch={spec.get("inject_sequence", 0)}')
                    if not spec.get('traced', True):
                        native_flags = '' if spec.get('use_default') else 'scale_identity_quad=1'
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'native-client'), '--quiet-kernel',
                               '--client-args', f'/dev/dri/card0 {args.iterations - 1} xrgb8888-native-tiled --offscreen' + (' --content-cycle' if spec.get('content') else ''),
                               '--module-args', native_flags]
                if spec.get('reduce'):
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--client-args', f'--reduce-{"content-" if spec.get("content") else ""}repeat {args.iterations}',
                               '--module-args', f'scale_trace=1 scale_efb_full=1 scale_split_reduce=1 scale_reduce_span={spec["span"]}']
                if spec.get('system'):
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--client-args', f'--system-enlarge-repeat {args.iterations}',
                               '--module-args', f'scale_system_trace=1 scale_system_split=1 scale_system_span={spec["span"]}']
                if spec.get('offset'):
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--client-args', f'--offset-enlarge-repeat {args.iterations}',
                               '--module-args', f'scale_offset_trace=1 scale_offset_split=1 scale_offset_span={spec["span"]}']
                if spec.get('regression'):
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--module-args', 'scale_bounded_final=1' if spec.get('bounded') else '']
                if spec.get('bounded_workload'):
                    selector = {'offset': '--offset-enlarge-repeat', 'system': '--system-enlarge-repeat',
                                'reduce-content': '--reduce-content-repeat'}[spec['bounded_workload']]
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--client-args', f'{selector} {args.iterations}', '--module-args', 'scale_bounded_final=1']
                    if spec.get('bounded_batch_quads'):
                        command[-1] += f' scale_bounded_batch_quads={spec["bounded_batch_quads"]}'
                    if spec.get('bounded_trace'):
                        command[-1] += ' scale_system_trace=1'
                    if spec.get('bounded_horizontal_split'):
                        command[-1] += ' scale_system_split=1'
                if spec.get('presentation'):
                    command = ['tools/wii-gcn-render-cycle.sh', '--host', args.host, '--module', str(module),
                               '--client', str(directory / 'render-client'), '--quiet-kernel',
                               '--client-args', f'/dev/dri/card0 {args.iterations-1} {spec["display_format"]}' + (' --boundary-checks' if spec.get('boundary_checks') else ''),
                               '--module-args', 'scale_bounded_final=1 scale_bounded_horizontal=1 scale_bounded_coord_cache=1 scale_bounded_log=0']
                    for name,value in (('scale_bounded_final','Y'),('scale_bounded_horizontal','Y'),
                                       ('scale_bounded_coord_cache','Y'),('scale_bounded_log','N')):
                        command += ['--expect-param', f'{name}={value}']
                if spec.get('mixed_offset'):
                    command[command.index('--client-args') + 1] = f'--offset-mixed-repeat {args.iterations}'
                if spec.get('system_baseline'):
                    command[-1] = ''
                if spec.get('system_content'):
                    command[command.index('--client-args') + 1] = f'--system-content-repeat {args.iterations}'
                if spec.get('system_profile'):
                    command[command.index('--client-args') + 1] = f'--system-profile-repeat {args.iterations}'
                if spec.get('system_sched'):
                    command[command.index('--client-args') + 1] = f'--system-sched{"-loop" if spec.get("system_sched_loop") else ""}-repeat {args.iterations}'
                if spec.get('bounded_horizontal'):
                    command[-1] += ' scale_bounded_horizontal=1'
                if spec.get('bounded_quiet'):
                    command[-1] += ' scale_bounded_log=0'
                if spec.get('coord_cache'):
                    command[-1] += ' scale_bounded_coord_cache=1'
                    command += ['--expect-param', 'scale_bounded_coord_cache=Y']
                if spec.get('native') or spec.get('regression') or spec.get('bounded'):
                    expect = 'Y' if spec.get('identity') or spec.get('regression') or spec.get('bounded') else 'N'
                    command += ['--expect-param', 'scale_system_split=Y' if spec.get('bounded_horizontal_split')
                                else 'scale_identity_quad=' + expect]
                if spec.get('bounded_quiet'):
                    for parameter in ('scale_bounded_final=Y', 'scale_bounded_horizontal=Y',
                                      'scale_bounded_log=N', 'scale_bounded_batch_quads=600'):
                        command += ['--expect-param', parameter]
                if spec.get('deferred'):
                    command += ['--defer-output', tag + '.client']
                if args.allow_dirty:
                    command.append('--allow-dirty')
                # The runner verifies every upload. Upload the tiny checker each case;
                # clear and primitive cases use different checkers at the same remote path.
                (case_dir / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
                rc = run_client(command, case_dir / 'client.txt', f'{index}/{len(selected)} {spec["name"]}', 1 if spec.get('regression') else args.iterations, args.timeout, spec.get('native', False))
                result['runner_status'] = rc
                if spec.get('deferred'):
                    remote.run(f"printf '<6>gcn-matrix-deferred end {tag}\\n' > /dev/kmsg; dmesg | sed -n '/gcn-matrix-deferred begin {tag.replace('/', chr(92) + '/')}/,$p' > {tag}.log")
                    client_hash = remote.run(f'sha256sum {tag}.client').decode().split()[0]
                    client_raw = remote.run(f'cat {tag}.client')
                    require(sha(client_raw) == client_hash, 'deferred client checksum mismatch')
                    require((case_dir / 'client.txt').read_bytes().endswith(client_raw), 'deferred client stream mismatch')
                    (case_dir / 'client-raw.txt').write_bytes(client_raw)
                    result['client_log_sha256'] = client_hash
                else:
                    remote.run(f'pid=$(cat {tag}.pid); test "$(cat /proc/$pid/comm)" = dmesg && kill "$pid"')
                    started_capture = False
                expected = remote.run(f'sha256sum {tag}.log').decode().split()[0]
                compressed = remote.run(f'gzip -c {tag}.log', timeout=180)
                (case_dir / 'kernel.txt.gz').write_bytes(compressed)
                raw = gzip.decompress(compressed)
                require(sha(raw) == expected, 'download checksum mismatch')
                if spec.get('deferred'):
                    require(raw.count(f'gcn-matrix-deferred begin {tag}'.encode()) == 1 and
                            raw.count(f'gcn-matrix-deferred end {tag}'.encode()) == 1,
                            'deferred kernel capture incomplete/overwritten')
                (case_dir / 'kernel.txt').write_bytes(raw)
                after = remote.state()
                (case_dir / 'after.json').write_text(json.dumps(after, indent=2) + '\n')
                require(after['module'] == 'unloaded' and after['boot'] == baseline['boot'] and after['printk'] == baseline['printk'], 'console/module/boot cleanup verification failed')
                result.update(audit_presentation(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('presentation') else audit_reduce(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('reduce') else audit_system(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('system') else audit_offset(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('offset') else audit_bounded_workload(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('bounded_workload') else audit_bounded_regression(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('bounded') else audit_regression(raw.decode(), (case_dir / 'client.txt').read_text(), args.iterations, rc)
                              if spec.get('regression') else audit_native(raw.decode(), (case_dir / 'client.txt').read_text(), spec, args.iterations, rc)
                              if spec.get('native') else audit(raw.decode(), spec, args.iterations, rc))
                result['kernel_sha256'] = expected
                # Delete only this suite's random-named temporary files after verified archival.
                remote.run(f'rm -- {tag}.log {tag}.client' if spec.get('deferred') else f'rm -- {tag}.log {tag}.pid')
            except (Exception, KeyboardInterrupt) as exc:
                result['status'] = 'ERROR'
                result['error'] = str(exc) or 'interrupted'
                result['remote_capture'] = tag + '.log'
                if started_capture:
                    try:
                        remote.run(f'pid=$(cat {tag}.pid); test "$(cat /proc/$pid/comm)" = dmesg && kill "$pid"')
                    except Exception:
                        pass
                # Preserve remote evidence and stop; do not continue with uncertain state.
            results.append(result)
            (case_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            write_summary(directory, results, metadata)
            print(f"{spec['name']}: {result['status']}  completed={result.get('completed', '?')}/{result['requested']}  raw={result.get('raw_errors', '?')} copy={result.get('copy_errors', '?')}" + (f"  {result['error']}" if 'error' in result else ''))
            if result['status'] == 'ERROR':
                break
    finally:
        if lock:
            try:
                remote.run('rmdir /tmp/wii-gcn-efb-matrix.lock')
            except Exception as exc:
                metadata['lock_cleanup_error'] = str(exc)
        metadata['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        write_summary(directory, results, metadata)
        files = sorted(p for p in directory.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
        (directory / 'SHA256SUMS').write_text(''.join(sha(p.read_bytes()) + '  ' + str(p.relative_to(directory)) + '\n' for p in files))
    print(f'Results: {directory / "summary.md"}')
    if len(results) != len(selected) or any(r['status'] == 'ERROR' for r in results) or metadata.get('lock_cleanup_error'):
        return 2
    return int(any(r['status'] == 'FAIL' for r in results))

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        sys.exit(2)
