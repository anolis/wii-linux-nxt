#!/usr/bin/env python3
"""Regression checks using checksum-verified captures from actual Wii runs."""
import gzip
import importlib.util
from pathlib import Path
import unittest

TOOLS = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('matrix', TOOLS / 'wii-gcn-efb-matrix.py')
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
CASES = {c['name']: c for c in matrix.CASES}
FIXTURES = Path(__file__).parent / 'fixtures'

class AuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.good = gzip.decompress((FIXTURES / 'interior-pass.txt.gz').read_bytes()).decode()
        cls.bad = gzip.decompress((FIXTURES / 'even-failure.txt.gz').read_bytes()).decode()

    def test_real_pass_checks_black_edges_too(self):
        r = matrix.audit(self.good, CASES['interior'], 4, 0)
        self.assertEqual((r['status'], r['completed'], r['pixels_per_oracle']), ('PASS', 4, 1228800))

    def test_real_failure_is_a_result_not_infrastructure_error(self):
        r = matrix.audit(self.bad, CASES['interior-even'], 4, 1)
        self.assertEqual((r['status'], r['completed'], r['raw_errors'], r['copy_errors']), ('FAIL', 1, 2, 1))

    def test_real_pipeline_case_accepts_extra_state_command(self):
        log = gzip.decompress((FIXTURES / 'late-z-failure.txt.gz').read_bytes()).decode()
        r = matrix.audit(log, CASES['late-z-one-row'], 4, 1)
        self.assertEqual((r['status'], r['completed']), ('FAIL', 1))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['one-row'], 4, 1)

    def test_real_scissor_pass_checks_every_batch_command_length(self):
        log = gzip.decompress((FIXTURES / 'scissor-tall-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['scissor-tall-rows'], 8, 0)
        self.assertEqual((result['status'], result['completed']), ('PASS', 8))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['two-rows'], 8, 0)
        # Dropping scissor/header bytes in later batches must also be rejected.
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('bytes=890', 'bytes=776'),
                         CASES['scissor-tall-rows'], 8, 0)

    def test_cpu_write_cannot_pass_without_fill_evidence(self):
        log = gzip.decompress((FIXTURES / 'cpu-write-pass.txt.gz').read_bytes()).decode()
        self.assertEqual(matrix.audit(log, CASES['cpu-write'], 8, 0)['status'], 'PASS')
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('efb-cpu-write', 'missing-cpu-write'),
                         CASES['cpu-write'], 8, 0)

    def test_batch_snapshot_requires_complete_observations(self):
        log = gzip.decompress((FIXTURES / 'batch-snapshot-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['snapshot-two-rows'], 4, 1)
        self.assertEqual((result['status'], result['completed']), ('FAIL', 1))
        self.assertGreater(result['batch_observations_bad'], 0)
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('efb-batch-snapshot iteration=0 batch=0 ', 'missing '),
                         CASES['snapshot-two-rows'], 4, 1)

    def test_striped_pass_requires_pattern_oracle_marker(self):
        log = gzip.decompress((FIXTURES / 'striped-overlap-pass.txt.gz').read_bytes()).decode()
        self.assertEqual(matrix.audit(log, CASES['striped-overlap-two-rows'], 16, 0)['status'], 'PASS')
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['overlap-two-rows'], 16, 0)
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('efb-clear-stripes', 'missing-stripes'),
                         CASES['striped-overlap-two-rows'], 16, 0)

    def test_native_single_requires_geometry_and_client_completion(self):
        log = gzip.decompress((FIXTURES / 'native-single-pass.txt.gz').read_bytes()).decode()
        client = (FIXTURES / 'native-single-client.txt').read_text()
        result = matrix.audit_native(log, client, CASES['native-single'], 4, 0)
        self.assertEqual((result['status'], result['sequences']), ('PASS', 16))
        with self.assertRaises(ValueError):
            matrix.audit_native(log, client, CASES['native-baseline'], 4, 0)
        with self.assertRaises(ValueError):
            matrix.audit_native(log, client.replace('PASS offscreen', 'missing'),
                                CASES['native-single'], 4, 0)
        with self.assertRaises(ValueError):
            matrix.audit_native(log.replace('native-prior seq=1 snapshot=0', 'missing'),
                                client, CASES['native-single'], 4, 0)

    def test_production_pass_requires_full_client_oracle(self):
        log = gzip.decompress((FIXTURES / 'native-production-pass.txt.gz').read_bytes()).decode()
        client = (FIXTURES / 'native-production-client.txt').read_text()
        result = matrix.audit_native(log, client, CASES['native-identity-production'], 16, 0)
        self.assertEqual((result['status'], result['completed']), ('PASS', 16))
        self.assertIsNone(result['raw_errors'])
        with self.assertRaises(ValueError):
            matrix.audit_native(log, client.replace('frames=16', 'frames=15'),
                                CASES['native-identity-production'], 16, 0)

    def test_default_build_requires_live_parameter_evidence(self):
        log = gzip.decompress((FIXTURES / 'native-default-pass.txt.gz').read_bytes()).decode()
        client = (FIXTURES / 'native-default-client.txt').read_text()
        result = matrix.audit_native(log, client, CASES['native-default-production'], 16, 0)
        self.assertEqual((result['status'], result['completed']), ('PASS', 16))
        with self.assertRaises(ValueError):
            matrix.audit_native(log.replace('parameter scale_identity_quad verified Y', ''),
                                client, CASES['native-default-production'], 16, 0)

    def test_full_regression_requires_complete_client_result(self):
        log = gzip.decompress((FIXTURES / 'render-regression-pass.txt.gz').read_bytes()).decode()
        client = (FIXTURES / 'render-regression-client.txt').read_text()
        result = matrix.audit_regression(log, client, 16, 0)
        self.assertEqual((result['status'], result['completed'], result['requested']), ('PASS', 1, 1))
        with self.assertRaises(ValueError):
            matrix.audit_regression(log, client.replace('PASS: GCN render UAPI', ''), 16, 0)

    def test_white_background_requires_correct_oracle_mode(self):
        log = gzip.decompress((FIXTURES / 'white-background-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['white-background-two-rows'], 16, 1)
        self.assertEqual((result['status'], result['completed'], result['raw_errors']), ('FAIL', 5, 1))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['two-rows'], 16, 1)
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('efb-background-mode expected=00ffffff', ''),
                         CASES['white-background-two-rows'], 16, 1)

    def test_masked_write_requires_white_oracle_and_mask_command(self):
        log = gzip.decompress((FIXTURES / 'masked-write-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['masked-two-rows'], 16, 1)
        self.assertEqual((result['status'], result['completed'], result['raw_errors']), ('FAIL', 0, 2))
        for broken in (log.replace('efb-write-mask', 'missing-mask'),
                       log.replace('bytes=1161', 'bytes=1156'),
                       log.replace('efb-clear iteration=0 expected=00ffffff',
                                   'efb-clear iteration=0 expected=00ff0000')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['masked-two-rows'], 16, 1)

    def test_state_only_requires_zero_geometry_and_white_oracle(self):
        log = gzip.decompress((FIXTURES / 'state-only-pass.txt.gz').read_bytes()).decode()
        self.assertEqual(matrix.audit(log, CASES['state-only'], 16, 0)['status'], 'PASS')
        for broken in (log.replace('efb-no-draw', 'missing-mode'),
                       log.replace('quads=0', 'quads=16'),
                       log.replace('bytes=385', 'bytes=388')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['state-only'], 16, 0)

    def test_depth_rejection_requires_marker_and_extra_state(self):
        log = gzip.decompress((FIXTURES / 'z-never-two-rows-pass.txt.gz').read_bytes()).decode()
        self.assertEqual(matrix.audit(log, CASES['z-never-two-rows'], 16, 0)['status'], 'PASS')
        for broken in (log.replace('efb-reject-depth', 'missing-mode'),
                       log.replace('bytes=1161', 'bytes=1156')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['z-never-two-rows'], 16, 0)

    def test_late_rejection_requires_correct_timing_and_command_length(self):
        log = gzip.decompress((FIXTURES / 'late-reject-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['late-z-never-two-rows'], 16, 1)
        self.assertEqual((result['status'], result['completed'], result['raw_errors']), ('FAIL', 0, 3))
        for broken in (log.replace('early=0', 'early=1'),
                       log.replace('bytes=1166', 'bytes=1161'),
                       log + '\nefb-reject-depth compare=NEVER early=1 expected=00ffffff'):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['late-z-never-two-rows'], 16, 1)

    def test_alpha_rejection_requires_mode_and_white_oracle(self):
        log = gzip.decompress((FIXTURES / 'alpha-reject-pass.txt.gz').read_bytes()).decode()
        self.assertEqual(matrix.audit(log, CASES['alpha-never-two-rows'], 16, 0)['status'], 'PASS')
        for broken in (log.replace('efb-reject-alpha', 'missing-mode'),
                       log.replace('bytes=1166', 'bytes=1161'),
                       log.replace('efb-clear iteration=0 expected=00ffffff',
                                   'efb-clear iteration=0 expected=00ff0000')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['alpha-never-two-rows'], 16, 0)

    def test_alpha_threshold_reject_and_accept_use_distinct_oracles(self):
        for case, rc, status, completed in [('alpha-less-two-rows', 0, 'PASS', 16),
                                             ('alpha-gequal-two-rows', 1, 'FAIL', 2)]:
            log = gzip.decompress((FIXTURES / (case + '.txt.gz')).read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 16, rc)
            self.assertEqual((result['status'], result['completed']), (status, completed))
            for broken in (log.replace('efb-alpha-threshold', 'missing-mode'),
                           log.replace('bytes=1166', 'bytes=1161'),
                           log.replace('compare=LESS', 'compare=GEQUAL') if rc == 0 else
                           log.replace('compare=GEQUAL', 'compare=LESS')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, CASES[case], 16, rc)

    def test_mixed_alpha_requires_complete_partition_and_phase_evidence(self):
        for case, completed in [('alpha-mixed-two-rows', 7), ('alpha-mixed-reverse-two-rows', 4)]:
            log = gzip.decompress((FIXTURES / (case + '.txt.gz')).read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 16, 1)
            self.assertEqual((result['completed'], result['alpha_partition_totals']), (completed, [1, 0, 0, 0]))
            for broken in (log.replace('efb-alpha-partition iteration=0', 'missing-partition'),
                           log.replace('accepted_raw=1', 'accepted_raw=0'),
                           log.replace('phase_period=4', 'phase_period=2'),
                           log.replace('vertex_alpha=0-255', 'vertex_alpha=255')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, CASES[case], 16, 1)

    def test_logic_output_requires_operation_and_expected_color(self):
        for case, completed in [('logic-copy-two-rows', 1), ('logic-set-two-rows', 15)]:
            log = gzip.decompress((FIXTURES / (case + '.txt.gz')).read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 16, 1)
            self.assertEqual((result['status'], result['completed']), ('FAIL', completed))
            for broken in (log.replace('efb-logic', 'missing-operation'),
                           log.replace('bytes=1171', 'bytes=1166'),
                           log.replace('operation=SET', 'operation=COPY') if completed == 15 else
                           log.replace('operation=COPY', 'operation=SET')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, CASES[case], 16, 1)

    def test_logic_set_black_requires_verified_contrasting_background(self):
        log = gzip.decompress((FIXTURES / 'logic-set-black-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['logic-set-black-two-rows'], 16, 1)
        self.assertEqual((result['completed'], result['raw_errors']), (0, 4))
        with self.assertRaises(ValueError):
            matrix.audit(log.replace('efb-background-mode expected=00000000',
                                     'efb-background-mode expected=00ffffff'),
                         CASES['logic-set-black-two-rows'], 16, 1)

    def test_inversion_requires_complementary_output_and_background(self):
        for case, errors, expected in [('logic-invert-white-two-rows', 6, '00000000'),
                                        ('logic-invert-black-two-rows', 13, '00ffffff')]:
            log = gzip.decompress((FIXTURES / (case + '.txt.gz')).read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 16, 1)
            self.assertEqual((result['completed'], result['raw_errors']), (0, errors))
            for broken in (log.replace('operation=INVERT', 'operation=COPY'),
                           log.replace('efb-clear iteration=0 expected=' + expected,
                                       'efb-clear iteration=0 expected=00ff0000')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, CASES[case], 16, 1)

    def test_logic_clear_requires_zero_output_and_clear_operation(self):
        log = gzip.decompress((FIXTURES / 'logic-clear-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['logic-clear-two-rows'], 16, 1)
        self.assertEqual((result['completed'], result['raw_errors']), (2, 1))
        for broken in (log.replace('operation=CLEAR', 'operation=SET'),
                       log.replace('efb-clear iteration=0 expected=00000000',
                                   'efb-clear iteration=0 expected=00ffffff')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['logic-clear-two-rows'], 16, 1)

    def test_rgba6_requires_storage_marker_and_labels_precision(self):
        for case, completed in [('rgba6-logic-clear-two-rows', 4), ('rgba6-logic-set-black-two-rows', 1)]:
            log = gzip.decompress((FIXTURES / (case + '.txt.gz')).read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 16, 1)
            self.assertEqual((result['completed'], result['efb_precision']), (completed, 'rgba6-expanded-rgb888'))
            for broken in (log.replace('efb-storage', 'missing-format'),
                           log.replace('bytes=1176', 'bytes=1171')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, CASES[case], 16, 1)

    def test_focused_probe_requires_small_geometry_and_full_oracle(self):
        log = gzip.decompress((FIXTURES / 'focus-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-530-54'], 16, 0)
        self.assertEqual((result['completed'], result['draw_submissions'], result['pixels_per_oracle']),
                         (16, 16, 4915200))
        for broken in (log.replace('efb-focus', 'missing-scope'),
                       log.replace('quads=2', 'quads=16'),
                       log.replace('first=52 end=56', 'first=0 end=16'),
                       log.replace('pixels=307200', 'pixels=32')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['focus-530-54'], 16, 0)

    def test_wide_focus_requires_matching_coverage(self):
        log = gzip.decompress((FIXTURES / 'focus-wide-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-wide-530-54'], 16, 0)
        self.assertEqual((result['completed'], result['draw_submissions'], result['pixels_per_oracle']),
                         (16, 16, 4915200))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-530-54'], 16, 0)
        for broken in (log.replace('colored_pixels=2144', 'colored_pixels=32'),
                       log.replace('black_pixels=305056', 'black_pixels=307168'),
                       log.replace('width=536', 'width=640')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['focus-wide-530-54'], 16, 0)

    def test_wide_focus_failure_preserves_recurrent_pixel(self):
        log = gzip.decompress((FIXTURES / 'focus-wide-failure.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-wide-530-54'], 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['raw_errors'],
                          result['copy_errors'], result['first_pixel']),
                         ('FAIL', 0, 1, 1, '530,54'))

    def test_intermediate_focus_widths_require_exact_coverage(self):
        for width, status, completed, runner in ((128, 'PASS', 64, 0), (256, 'FAIL', 48, 1)):
            log = gzip.decompress((FIXTURES / f'focus-width-{width}.txt.gz').read_bytes()).decode()
            result = matrix.audit(log, CASES[f'focus-width-{width}'], 64, runner)
            self.assertEqual((result['status'], result['completed']), (status, completed))
            with self.assertRaises(ValueError):
                matrix.audit(log, CASES['focus-width-64'], 64, runner)
            with self.assertRaises(ValueError):
                matrix.audit(log.replace(f'colored_pixels={width * 4}', 'colored_pixels=32'),
                             CASES[f'focus-width-{width}'], 64, runner)

    def test_split_focus_requires_subdivision_and_same_coverage(self):
        log = gzip.decompress((FIXTURES / 'focus-split-128-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-split-128'], 1000, 0)
        self.assertEqual((result['completed'], result['draw_submissions'], result['pixels_per_oracle']),
                         (1000, 1000, 307200000))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-width-128'], 1000, 0)
        for broken in (log.replace('boundary=472', 'boundary=471'),
                       log.replace('bytes=595', 'bytes=499'),
                       log.replace('colored_pixels=512', 'colored_pixels=256')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['focus-split-128'], 1000, 0)

    def test_wide_split_requires_tail_coverage_and_eighteen_quads(self):
        log = gzip.decompress((FIXTURES / 'focus-split-536-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-split-536'], 1000, 0)
        self.assertEqual((result['completed'], result['draw_submissions']), (1000, 1000))
        for broken in (log.replace('quads=18', 'quads=16'),
                       log.replace('bytes=1267', 'bytes=595'),
                       log.replace('colored_pixels=2144', 'colored_pixels=2048')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['focus-split-536'], 1000, 0)

    def test_full_split_requires_full_coverage_and_all_batches(self):
        log = gzip.decompress((FIXTURES / 'full-split-64-pass.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['full-split-64'], 1000, 0)
        self.assertEqual((result['completed'], result['draw_submissions'], result['pixels_per_oracle']),
                         (1000, 30000, 307200000))
        for broken in (log.replace('quads=88', 'quads=16'),
                       log.replace('bytes=4627', 'bytes=1171'),
                       log.replace('pixels=307200', 'pixels=2144'),
                       log.replace('pieces_per_strip=10', 'pieces_per_strip=9')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, CASES['full-split-64'], 1000, 0)

    def test_full_split_color_and_stripes_keep_distinct_oracles(self):
        for case in ('full-split-color-64', 'full-split-striped-64'):
            log = gzip.decompress((FIXTURES / f'{case}-pass.txt.gz').read_bytes()).decode()
            result = matrix.audit(log, CASES[case], 1000, 0)
            self.assertEqual((result['completed'], result['draw_submissions'], result['pixels_per_oracle']),
                             (1000, 30000, 307200000))
            other = 'full-split-color-64' if 'striped' in case else 'full-split-striped-64'
            with self.assertRaises(ValueError):
                matrix.audit(log, CASES[other], 1000, 0)
            with self.assertRaises(ValueError):
                matrix.audit(log.replace('bytes=4612', 'bytes=4627'), CASES[case], 1000, 0)

    def test_native_spans_require_three_bounded_batches(self):
        for span in (64, 160):
            log = gzip.decompress((FIXTURES / f'native-span-{span}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'native-span-{span}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_native(log, client, CASES[f'native-span-{span}'], 16, 0)
            self.assertEqual((result['completed'], result['sequences']), (16, 64))
            self.assertEqual(result['final_draw_submissions'], 192)
            self.assertEqual(result['max_final_command_bytes'], 8 + 6400 * (320 // span))
            self.assertEqual(result['final_command_bytes'], 192 * result['max_final_command_bytes'])
            for broken in (log.replace('end=80', 'end=79'),
                           log.replace(f'span={span}', 'span=32'),
                           log.replace(f'bytes={8 + 80 * 80 * (320 // span)}', 'bytes=65536')):
                with self.assertRaises(ValueError):
                    matrix.audit_native(broken, client, CASES[f'native-span-{span}'], 16, 0)

    def test_offset_span_requires_scaled_and_preserved_pixel_coverage(self):
        for span in (64, 128):
            log = gzip.decompress((FIXTURES / f'offset-span-{span}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'offset-span-{span}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_offset(log, client, CASES[f'offset-span-{span}'], 16, 0)
            self.assertEqual((result['completed'], result['sequences'], result['final_draw_submissions']), (16,16,16))
            for broken in (log.replace('pixels=65536', 'pixels=20224'),
                           log.replace(f'span={span}', 'span=32'),
                           log.replace('stage=horizontal', 'stage=crop')):
                with self.assertRaises(ValueError):
                    matrix.audit_offset(broken, client, CASES[f'offset-span-{span}'], 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_offset(log, client.replace('OFFSET: 16/16 iterations', ''), CASES[f'offset-span-{span}'], 16, 0)

    def test_system_spans_require_six_batches_and_full_scaled_image(self):
        for span in (64, 320):
            log = gzip.decompress((FIXTURES / f'system-span-{span}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'system-span-{span}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_system(log, client, CASES[f'system-span-{span}'], 16, 0)
            self.assertEqual((result['completed'], result['final_draw_submissions']), (16,96))
            self.assertLess(result['max_final_command_bytes'], 65536)
            for broken in (log.replace('end=480', 'end=479'),
                           log.replace('pixels=307200', 'pixels=76800'),
                           log.replace(f'span={span}', 'span=32')):
                with self.assertRaises(ValueError):
                    matrix.audit_system(broken, client, CASES[f'system-span-{span}'], 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_system(log, client.replace('SYSTEM: 16/16 iterations',''), CASES[f'system-span-{span}'], 16, 0)

    def test_reduction_requires_all_pixels_and_exact_fifo_budget(self):
        for span in (64, 160):
            log = gzip.decompress((FIXTURES / f'reduce-span-{span}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'reduce-span-{span}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_reduce(log, client, CASES[f'reduce-span-{span}'], 16, 0)
            self.assertEqual((result['completed'], result['final_draw_submissions']), (16,16))
            self.assertLess(result['max_final_command_bytes'],65536)
            for broken in (log.replace('pixels=38400','pixels=19200'),
                           log.replace(f'span={span}','span=32'),
                           log.replace('source_mismatches=0','source_mismatches=1')):
                with self.assertRaises(ValueError):
                    matrix.audit_reduce(broken,client,CASES[f'reduce-span-{span}'],16,0)
            with self.assertRaises(ValueError):
                matrix.audit_reduce(log,client.replace('REDUCE: 16/16 iterations',''),CASES[f'reduce-span-{span}'],16,0)

    def test_reduction_content_schedule_is_replayable_and_complete(self):
        for span in (64,160):
            log = gzip.decompress((FIXTURES / f'reduce-content-{span}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'reduce-content-{span}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_reduce(log,client,CASES[f'reduce-content-{span}'],16,0)
            self.assertEqual(result['completed'],16)
            for broken in (client.replace('pattern=7','pattern=0'),
                           client.replace('seed=2654435769','seed=0'),
                           client.replace('iteration=15 pattern=7','iteration=14 pattern=7')):
                with self.assertRaises(ValueError):
                    matrix.audit_reduce(log,broken,CASES[f'reduce-content-{span}'],16,0)
            with self.assertRaises(ValueError):
                matrix.audit_reduce(log,client,CASES[f'reduce-span-{span}'],16,0)

    def test_focus_placement_requires_matching_oracle_and_batch_rows(self):
        for top, completed, rc in ((53,14,1),(60,64,0),(180,64,0)):
            log = gzip.decompress((FIXTURES / f'focus-top-{top}.txt.gz').read_bytes()).decode()
            result = matrix.audit(log,CASES[f'focus-wide-top-{top}'],64,rc)
            self.assertEqual(result['completed'],completed)
            for broken in (log.replace(f'top={top}', 'top=52'),
                           log.replace(f'first={top} end={top+4}', 'first=52 end=56')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken,CASES[f'focus-wide-top-{top}'],64,rc)
            with self.assertRaises(ValueError):
                matrix.audit(log,CASES['focus-wide-530-54'],64,rc)

    def test_compensated_viewport_keeps_physical_oracle_and_command_budget(self):
        for shift, completed in ((0,10),(32,37)):
            log = gzip.decompress((FIXTURES / f'focus-viewport-{shift}.txt.gz').read_bytes()).decode()
            result = matrix.audit(log,CASES[f'focus-viewport-{shift}'],64,1)
            self.assertEqual((result['completed'],result['first_pixel']), (completed,'530,54'))
            for broken in (log.replace(f'vertex_top={52-shift}', 'vertex_top=99'),
                           log.replace('physical_top=52','physical_top=84'),
                           log.replace('bytes=528','bytes=499')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken,CASES[f'focus-viewport-{shift}'],64,1)
            with self.assertRaises(ValueError):
                matrix.audit(log,CASES['focus-wide-530-54'],64,1)

    def test_padding_and_subdivision_have_matching_counts_but_distinct_markers(self):
        for case, completed, rc in (('focus-padded-536',17,1),('focus-split-536',64,0)):
            log = gzip.decompress((FIXTURES / f'{case}-matched.txt.gz').read_bytes()).decode()
            result = matrix.audit(log,CASES[case],64,rc)
            self.assertEqual(result['completed'],completed)
            self.assertIn('quads=18 bytes=1267',log)
            other = 'focus-split-536' if 'padded' in case else 'focus-padded-536'
            with self.assertRaises(ValueError):
                matrix.audit(log,CASES[other],64,rc)
            with self.assertRaises(ValueError):
                matrix.audit(log.replace('bytes=1267','bytes=499'),CASES[case],64,rc)

    def test_focused_order_requires_correct_traversal_marker(self):
        for order,marker in (('reverse','reverse'),('columns','column-major')):
            log = gzip.decompress((FIXTURES / f'focus-order-{order}.txt.gz').read_bytes()).decode()
            result = matrix.audit(log,CASES[f'focus-split-{order}'],64,0)
            self.assertEqual(result['completed'],64)
            for broken in (log.replace(f'order={marker}','order=row-major'),
                           log.replace('quads=18 bytes=1267','quads=2 bytes=499')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken,CASES[f'focus-split-{order}'],64,0)
            with self.assertRaises(ValueError):
                matrix.audit(log,CASES['focus-split-536'],64,0)

    def test_focused_span_requires_boundary_coverage_and_command_budget(self):
        for span, quads, size in ((96, 12, 979), (128, 10, 883), (256, 6, 691)):
            log = gzip.decompress((FIXTURES / f'focus-span-{span}.txt.gz').read_bytes()).decode()
            spec = CASES[f'focus-span-{span}']
            result = matrix.audit(log, spec, 64, 0)
            self.assertEqual((result['completed'], result['pixels_per_oracle']), (64, 19660800))
            self.assertIn(f'quads={quads} bytes={size}', log)
            for broken in (log.replace(f'boundary={span}', 'boundary=64'),
                           log.replace(f'span={span}', 'span=64'),
                           log.replace(f'bytes={size}', 'bytes=1267'),
                           log.replace('colored_pixels=2144', 'colored_pixels=2048')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, spec, 64, 0)
            with self.assertRaises(ValueError):
                matrix.audit(log, CASES['focus-split-536'], 64, 0)

    def test_focused_right_anchor_requires_remainder_and_budget(self):
        for span, size in ((96, 979), (128, 883), (256, 691)):
            log = gzip.decompress((FIXTURES / f'focus-right-{span}.txt.gz').read_bytes()).decode()
            spec = CASES[f'focus-right-{span}']
            remainder = 536 % span
            result = matrix.audit(log, spec, 64, 0)
            self.assertEqual(result['completed'], 64)
            self.assertIn('focus_right', matrix.FLAGS)
            for broken in (log.replace('side=right', 'side=left'),
                           log.replace(f'remainder={remainder}', f'remainder={span}'),
                           log.replace(f'boundary={remainder}', f'boundary={span}'),
                           log.replace(f'bytes={size}', 'bytes=499')):
                with self.assertRaises(ValueError):
                    matrix.audit(broken, spec, 64, 0)
            with self.assertRaises(ValueError):
                matrix.audit(log, CASES[f'focus-span-{span}'], 64, 0)

    def test_focused_right_anchor_preserves_verified_failure(self):
        log = gzip.decompress((FIXTURES / 'focus-right-256-fail.txt.gz').read_bytes()).decode()
        result = matrix.audit(log, CASES['focus-right-256'], 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 9, 10))
        self.assertEqual((result['raw_errors'], result['copy_errors'], result['first_pixel']),
                         (1, 1, '530,54'))
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-span-256'], 1000, 1)

    def test_right_128_reverse_failure_requires_order_and_anchor(self):
        log = gzip.decompress((FIXTURES / 'focus-right-128-reverse-fail.txt.gz').read_bytes()).decode()
        spec = CASES['focus-right-128-reverse']
        result = matrix.audit(log, spec, 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['first_pixel']),
                         ('FAIL', 21, '530,54'))
        for broken in (log.replace('order=reverse', 'order=row-major'),
                       log.replace('side=right', 'side=left'),
                       log.replace('bytes=883', 'bytes=1267')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, spec, 1000, 1)
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-right-128'], 1000, 1)

    def test_right_128_columns_requires_complete_ordered_capture(self):
        log = gzip.decompress((FIXTURES / 'focus-right-128-columns-pass.txt.gz').read_bytes()).decode()
        spec = CASES['focus-right-128-columns']
        result = matrix.audit(log, spec, 1000, 0)
        self.assertEqual((result['completed'], result['pixels_per_oracle']), (1000, 307200000))
        for broken in (log.replace('order=column-major', 'order=reverse'),
                       log.replace('quads=10 bytes=883', 'quads=18 bytes=1267')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, spec, 1000, 0)
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-right-128-reverse'], 1000, 0)

    def test_right_64_reverse_requires_combined_geometry(self):
        log = gzip.decompress((FIXTURES / 'focus-right-64-reverse-pass.txt.gz').read_bytes()).decode()
        spec = CASES['focus-right-64-reverse']
        result = matrix.audit(log, spec, 1000, 0)
        self.assertEqual((result['completed'], result['pixels_per_oracle']), (1000, 307200000))
        for other in ('focus-right-64', 'focus-split-reverse', 'focus-right-128-reverse'):
            with self.assertRaises(ValueError):
                matrix.audit(log, CASES[other], 1000, 0)

    def test_right_96_reverse_requires_verified_failure_and_exact_geometry(self):
        log = gzip.decompress((FIXTURES / 'focus-right-96-reverse-fail.txt.gz').read_bytes()).decode()
        spec = CASES['focus-right-96-reverse']
        result = matrix.audit(log, spec, 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 373, 374))
        self.assertEqual((result['raw_errors'], result['copy_errors'], result['first_pixel']), (1, 1, '530,54'))
        for broken in (log.replace('order=reverse', 'order=row-major'),
                       log.replace('span=96', 'span=64'),
                       log.replace('bytes=979', 'bytes=1267')):
            with self.assertRaises(ValueError):
                matrix.audit(broken, spec, 1000, 1)
        with self.assertRaises(ValueError):
            matrix.audit(log, CASES['focus-right-96'], 1000, 1)

    def test_native_two_batches_require_exact_rows_and_resource_totals(self):
        for span, batch_bytes in ((64, 48008), (160, 19208)):
            name = f'native-two-batches-{span}'
            log = gzip.decompress((FIXTURES / f'{name}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            spec = CASES[name]
            result = matrix.audit_native(log, client, spec, 16, 0)
            self.assertEqual((result['completed'], result['sequences'], result['final_draw_submissions']),
                             (16, 64, 128))
            self.assertEqual((result['max_final_command_bytes'], result['final_command_bytes']),
                             (batch_bytes, batch_bytes * 128))
            for broken in (log.replace('first=120 end=240', 'first=80 end=160'),
                           log.replace(f'bytes={batch_bytes}', 'bytes=32008'),
                           log.replace('native-span seq=1 batch=1', 'missing seq=1 batch=1')):
                with self.assertRaises(ValueError):
                    matrix.audit_native(broken, client, spec, 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_native(log, client, CASES[f'native-span-{span}'], 16, 0)

    def test_native_reverse_requires_every_batch_order_marker(self):
        for span, size in ((64, 48008), (160, 19208)):
            name = f'native-two-reverse-{span}'
            log = gzip.decompress((FIXTURES / f'{name}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            spec = CASES[name]
            result = matrix.audit_native(log, client, spec, 16, 0)
            self.assertEqual((result['completed'], result['sequences'], result['final_draw_submissions']),
                             (16, 64, 128))
            self.assertEqual(result['max_final_command_bytes'], size)
            for broken in (log.replace('order=reverse', 'order=row-major'),
                           log.replace('scope=within-batch', 'scope=all-batches'),
                           log.replace('native-span-order seq=1 batch=0', 'missing seq=1 batch=0')):
                with self.assertRaises(ValueError):
                    matrix.audit_native(broken, client, spec, 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_native(log, client, CASES[f'native-two-batches-{span}'], 16, 0)

    def test_native_intermediate_error_overrides_end_of_frame_client_pass(self):
        log = gzip.decompress((FIXTURES / 'native-reverse-160-intermediate-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'native-reverse-160-intermediate-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_native(log, client, CASES['native-two-reverse-160'], 256, 0)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 241, 256))
        self.assertEqual((result['first_error_sequence'], result['first_error_frame']), (967, 242))
        self.assertEqual(result['client_frames_completed'], 256)
        self.assertTrue(result['client_reported_pass'])
        self.assertEqual((result['raw_errors'], result['copy_errors']), (1, 1))

    def test_native_stop_controls_require_complete_partial_frame_and_injection(self):
        for sequence in (1, 3):
            name = f'native-stop-control-{sequence}'
            log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            spec = CASES[name]
            result = matrix.audit_native(log, client, spec, 16, 1)
            self.assertEqual((result['sequences'], result['completed'], result['checked'],
                              result['client_frames_completed']), (sequence, 0, 1, 0))
            self.assertTrue(result['stopped_on_error'])
            self.assertTrue(result['injected_oracle_error'])
            for broken in (log.replace('native-stop seq=', 'missing-stop seq='),
                           log.replace('native-test-mismatch seq=', 'missing-injection seq='),
                           log.replace(f'native-stop seq={sequence}', f'native-stop seq={sequence + 1}'),
                           log.replace(f'native-scale seq={sequence} origin=', f'missing-scale seq={sequence} origin=')):
                with self.assertRaises(ValueError):
                    matrix.audit_native(broken, client, spec, 16, 1)
            with self.assertRaises(ValueError):
                matrix.audit_native(log, client, CASES['native-stop-64'], 16, 1)

    def test_native_stop_mode_clean_run_has_no_stop_or_injection(self):
        log = gzip.decompress((FIXTURES / 'native-stop-64.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'native-stop-64-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_native(log, client, CASES['native-stop-64'], 16, 0)
        self.assertEqual((result['completed'], result['checked'], result['client_frames_completed']), (16, 16, 16))
        self.assertFalse(result['stopped_on_error'])
        self.assertFalse(result['injected_oracle_error'])
        with self.assertRaises(ValueError):
            matrix.audit_native(log + '\ngcn-gx: native-stop seq=64 reason=oracle-mismatch errno=84\n',
                                client, CASES['native-stop-64'], 16, 0)

    def test_native_real_stop_preserves_first_bad_partial_frame(self):
        log = gzip.decompress((FIXTURES / 'native-stop-160-real-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'native-stop-160-real-client.txt.gz').read_bytes()).decode()
        spec = CASES['native-stop-160']
        result = matrix.audit_native(log, client, spec, 256, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 7, 8))
        self.assertEqual((result['sequences'], result['first_error_sequence'], result['client_frames_completed']), (31, 31, 7))
        self.assertTrue(result['stopped_on_error'])
        self.assertFalse(result['injected_oracle_error'])
        self.assertEqual((result['raw_errors'], result['copy_errors']), (1, 1))
        with self.assertRaises(ValueError):
            matrix.audit_native(log.replace('native-stop seq=31', 'native-stop seq=32'), client, spec, 256, 1)
        with self.assertRaises(ValueError):
            matrix.audit_native(log, client, CASES['native-two-reverse-160'], 256, 1)

    def test_native_content_requires_complete_pattern_and_seed_schedule(self):
        for span in (64, 160):
            name = f'native-content-{span}'
            log = gzip.decompress((FIXTURES / f'{name}-pass.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            spec = CASES[name]
            result = matrix.audit_native(log, client, spec, 16, 0)
            self.assertEqual((result['completed'], result['sequences']), (16, 64))
            for broken in (client.replace('frame=7 pattern=7', 'frame=7 pattern=6'),
                           client.replace('seed=9e3779b9', 'seed=00000000'),
                           client.replace('native-content frame=15', 'missing frame=15')):
                with self.assertRaises(ValueError):
                    matrix.audit_native(log, broken, spec, 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_native(log, client, CASES[f'native-stop-{span}'], 16, 0)

    def test_native_content_failure_requires_schedule_through_stopped_frame(self):
        log = gzip.decompress((FIXTURES / 'native-content-160-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'native-content-160-fail-client.txt.gz').read_bytes()).decode()
        spec = CASES['native-content-160']
        result = matrix.audit_native(log, client, spec, 256, 1)
        self.assertEqual((result['completed'], result['checked'], result['first_error_sequence']), (7, 8, 32))
        self.assertTrue(result['stopped_on_error'])
        self.assertFalse(result['injected_oracle_error'])
        for broken in (client.replace('native-content frame=7', 'missing frame=7'),
                       client + '\ngcn-kms-flip-test: native-content frame=8 pattern=0 seed=8ff34781\n'):
            with self.assertRaises(ValueError):
                matrix.audit_native(log, broken, spec, 256, 1)

    def test_generic_bounded_regression_requires_real_calls_and_exact_batches(self):
        log = gzip.decompress((FIXTURES / 'bounded-final-regression.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'bounded-final-regression-client.txt.gz').read_bytes()).decode()
        spec = CASES['bounded-final-regression']
        result = matrix.audit_bounded_regression(log, client, spec, 1, 0)
        self.assertEqual((result['status'], result['bounded_calls'], result['final_draw_submissions']), ('PASS', 27, 37))
        self.assertEqual((result['max_final_command_bytes'], result['final_command_bytes']), (48008, 984936))
        for broken in (log.replace('width=106 src_height=47 dst_height=94 runs=47 quads=94',
                                   'width=106 src_height=47 dst_height=94 runs=46 quads=94'),
                       log.replace('bytes=48008', 'bytes=65536'),
                       log.replace('bounded-batch seq=1 batch=0', 'missing-batch seq=1 batch=0'),
                       log.replace('bounded-begin', 'missing-begin')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_regression(broken, client, spec, 1, 0)

    def test_bounded_workloads_require_complete_progress_and_geometry(self):
        for workload, prefix, pixels, batches in (
                ('offset', 'OFFSET', 65536, 16),
                ('system', 'SYSTEM', 307200, 64),
                ('reduce-content', 'REDUCE', 38400, 16)):
            name = f'bounded-{workload}'
            log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 0)
            self.assertEqual((result['status'], result['completed'], result['checked']), ('PASS', 16, 16))
            self.assertEqual(result['client_pixels_checked'], 16 * pixels)
            self.assertEqual(result['final_draw_submissions'], batches)
            self.assertIsNone(result['raw_errors'])
            self.assertIsNone(result['copy_errors'])
            for broken in (client.replace(f'{prefix}: 8/16 iterations', ''),
                           client.replace(f'{prefix}: 8/16', f'{prefix}: 9/16'),
                           client.replace('/16 iterations', '/17 iterations')):
                with self.assertRaises(ValueError):
                    matrix.audit_bounded_workload(log, broken, CASES[name], 16, 0)
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log, client, CASES[name], 17, 0)
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log.replace('x=0 ', 'x=1 ', 1), client, CASES[name], 16, 0)

    def test_bounded_content_requires_exact_pattern_and_seed(self):
        name = 'bounded-reduce-content'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        for broken in (client.replace('iteration=7 pattern=7', 'iteration=7 pattern=6'),
                       client.replace('seed=2654435769', 'seed=0'),
                       client.replace('REDUCE CONTENT: iteration=15', 'missing iteration=15')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log, broken, CASES[name], 16, 0)

    def test_bounded_workload_failure_excludes_last_attempt_from_completed(self):
        name = 'bounded-offset'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        # Mutate the real capture's final outcome to exercise failure accounting.
        client = client.replace('OFFSET: 16/16 iterations', '').replace(
            'PASS: GCN render UAPI', '1 render UAPI test(s) failed')
        result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 15, 16))
        self.assertEqual(result['client_pixels_checked'], 15 * 65536)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log, client.replace('OFFSET: 15/16 iterations', ''), CASES[name], 16, 1)

    def test_bounded_system_real_failure_keeps_pixel_and_exact_attempt_count(self):
        log = gzip.decompress((FIXTURES / 'bounded-system-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'bounded-system-fail-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_workload(log, client, CASES['bounded-system'], 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 51, 52))
        self.assertEqual(result['first_pixel'], '317,352')
        self.assertIn('got=0xdc9a expected=0xdc9e', result['first_record'])
        self.assertEqual(result['final_draw_submissions'], 208)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log, client, CASES['bounded-system-400'], 1000, 1)

    def test_bounded_400_real_failure_requires_six_batches_per_call(self):
        log = gzip.decompress((FIXTURES / 'bounded-system-400-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'bounded-system-400-fail-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_workload(log, client, CASES['bounded-system-400'], 1000, 1)
        self.assertEqual((result['status'], result['completed'], result['checked']), ('FAIL', 144, 145))
        self.assertEqual(result['first_pixel'], '285,368')
        self.assertEqual(result['final_draw_submissions'], 870)
        self.assertEqual(result['max_final_command_bytes'], 32008)
        self.assertEqual(result['final_command_bytes'], 145 * 192048)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log, client, CASES['bounded-system'], 1000, 1)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log.replace('bounded-batch seq=145 batch=5', 'missing'), client,
                                          CASES['bounded-system-400'], 1000, 1)

    def test_bounded_trace_localizes_real_horizontal_fault_and_requires_stop(self):
        log = gzip.decompress((FIXTURES / 'bounded-system-trace-fail.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'bounded-system-trace-fail-client.txt.gz').read_bytes()).decode()
        spec = CASES['bounded-system-trace']
        result = matrix.audit_bounded_workload(log, client, spec, 1000, 1)
        self.assertEqual((result['completed'], result['checked']), (656, 657))
        self.assertEqual((result['raw_errors'], result['copy_errors'], result['copy_differences']), (3, 3, 0))
        self.assertEqual(result['first_pixel'], '62,166')
        self.assertEqual([r[1] for r in result['stage_failures']], ['horizontal', 'final'])
        self.assertTrue(result['stopped_on_error'])
        for broken in (log.replace('bounded-stop', 'missing-stop'),
                       log.replace('system-scale-first', 'missing-first'),
                       log.replace('stage=crop pixels=76800', 'stage=crop pixels=76801'),
                       log.replace('split=0 quads=320', 'split=1 quads=640'),
                       log.replace('bytes=26574', 'bytes=52174'),
                       log.replace('system-scale seq=657 stage=final', 'missing-final')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(broken, client, spec, 1000, 1)

    def test_bounded_horizontal_split_requires_verified_parameter(self):
        name = 'bounded-system-horizontal-split'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_workload(log, client, CASES[name], 1000, 0)
        self.assertEqual((result['completed'], result['checked']), (1000, 1000))
        self.assertEqual(result['client_pixels_checked'], 307200000)
        self.assertIsNone(result['raw_errors'])
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log.replace('parameter scale_system_split verified Y', ''),
                                          client, CASES[name], 1000, 0)

    def test_render_regression_rejects_actual_provider_absent_pass(self):
        log = gzip.decompress((FIXTURES / 'render-provider-absent.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'render-provider-absent-client.txt.gz').read_bytes()).decode()
        self.assertIn('PASS: GCN render UAPI', client)
        with self.assertRaises(ValueError):
            matrix.audit_regression(log, client, 1, 0)

    def test_both_helpers_require_all_horizontal_geometry_and_batches(self):
        name = 'bounded-both-regression'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_regression(log, client, CASES[name], 1, 0)
        self.assertEqual((result['bounded_calls'], result['horizontal_draw_submissions']), (27, 27))
        self.assertEqual(result['max_horizontal_command_bytes'], 52174)
        self.assertEqual(result['horizontal_command_bytes'], 443258)
        for broken in (log.replace('bounded-horizontal-begin', 'missing-begin'),
                       log.replace('bounded-horizontal-batch', 'missing-batch'),
                       log.replace('height=240 runs=320 quads=640', 'height=240 runs=319 quads=640'),
                       log.replace('bytes=52174', 'bytes=65536')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_regression(broken, client, CASES[name], 1, 0)

    def test_both_helpers_require_expected_workload_geometry(self):
        for workload in ('system', 'reduce-content', 'offset'):
            name = f'bounded-both-{workload}'
            log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 0)
            self.assertEqual((result['completed'], result['horizontal_draw_submissions']), (16, 16))
            self.assertIsNone(result['raw_errors'])
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log.replace('bounded-horizontal-batch seq=16', 'missing'),
                                              client, CASES[name], 16, 0)

    def test_both_helpers_cross_horizontal_batch_inside_odd_height_column(self):
        log = gzip.decompress((FIXTURES / 'bounded-both-expanded-regression.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'bounded-both-expanded-regression-client.txt.gz').read_bytes()).decode()
        spec = CASES['bounded-both-regression']
        result = matrix.audit_bounded_regression(log, client, spec, 1, 0)
        self.assertEqual((result['bounded_calls'], result['horizontal_draw_submissions']), (28, 29))
        self.assertIn([255, 256, 255, 255, 765], result['horizontal_geometries'])
        self.assertIn('quads=125 before=0 bytes=10008 last=1', log)
        for broken in (log.replace('quads=125 before=0', 'quads=125 before=974'),
                       log.replace('quads=125 before=0 bytes=10008 last=1', 'quads=124 before=0 bytes=9928 last=1')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_regression(broken, client, spec, 1, 0)

    def test_system_timing_requires_all_attempts_and_summarizes_clean_calls(self):
        for name in ('system-baseline-timed', 'bounded-both-system-timed'):
            log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
            client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
            result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 0)
            self.assertEqual(result['ioctl_timed_clean_calls'], 16)
            self.assertEqual(result['ioctl_p95_ns'], max(result['ioctl_samples_ns']))
            for broken in (client.replace('SYSTEM TIMING: iteration=15', 'missing'),
                           client.replace('SYSTEM TIMING: iteration=0', 'SYSTEM TIMING: iteration=1'),
                           client.replace('status=0', 'status=-1')):
                with self.assertRaises(ValueError):
                    matrix.audit_bounded_workload(log, broken, CASES[name], 16, 0)
            # Synthetic final client mismatch: completed timings exclude attempt 16.
            failed = client.replace('SYSTEM: 16/16 iterations', '').replace(
                'PASS: GCN render UAPI', '1 render UAPI test(s) failed')
            result = matrix.audit_bounded_workload(log, failed, CASES[name], 16, 1)
            self.assertEqual((result['completed'], result['checked'], result['ioctl_timed_clean_calls']), (15,16,15))

    def test_system_content_requires_every_pattern_and_seed(self):
        name = 'bounded-both-system-content'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 0)
        self.assertEqual(result['client_pixels_checked'], 16 * 307200)
        for broken in (client.replace('iteration=7 pattern=7', 'iteration=7 pattern=6'),
                       client.replace('seed=2654435769', 'seed=0'),
                       client.replace('SYSTEM CONTENT: iteration=15', 'missing')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log, broken, CASES[name], 16, 0)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log, client, CASES['bounded-both-system-timed'], 16, 0)

    def test_system_profile_requires_cpu_records_and_preserves_signed_residual(self):
        name = 'bounded-both-system-profile'
        log = gzip.decompress((FIXTURES / f'{name}.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / f'{name}-client.txt.gz').read_bytes()).decode()
        result = matrix.audit_bounded_workload(log, client, CASES[name], 16, 0)
        self.assertEqual((len(result['cpu_samples_ns']), result['cpu_clock_resolution_ns']), (16,1))
        self.assertEqual(sum(result['voluntary_switches']), 0)
        for wall, cpu, residual in zip(result['ioctl_samples_ns'], result['cpu_samples_ns'], result['elapsed_minus_cpu_ns']):
            self.assertEqual(residual, wall-cpu)
        for broken in (client.replace('SYSTEM CPU: iteration=15', 'missing'),
                       client.replace('SYSTEM CPU: iteration=0', 'SYSTEM CPU: iteration=1'),
                       client.replace('resolution_ns=1', 'resolution_ns=0', 1)):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(log, broken, CASES[name], 16, 0)
        import re
        synthetic = re.sub(r'(SYSTEM CPU: iteration=0 ns=)\d+', r'\g<1>999999999', client)
        result = matrix.audit_bounded_workload(log, synthetic, CASES[name], 16, 0)
        self.assertLess(result['elapsed_minus_cpu_ns'][0], 0)
        with self.assertRaises(ValueError):
            matrix.audit_bounded_workload(log, client, CASES['bounded-both-system-content'], 16, 0)

    def test_deferred_capture_requires_complete_unique_markers(self):
        log = gzip.decompress((FIXTURES / 'system-profile-deferred.txt.gz').read_bytes()).decode()
        client = gzip.decompress((FIXTURES / 'system-profile-deferred-client.txt.gz').read_bytes()).decode()
        spec = CASES['bounded-both-system-profile-deferred']
        result = matrix.audit_bounded_workload(log, client, spec, 8, 0)
        self.assertEqual((result['completed'], result['capture_mode']), (8,'deferred'))
        for broken in (log.replace('gcn-matrix-deferred begin','missing'),
                       log.replace('gcn-matrix-deferred end','missing'),
                       log + log,
                       log.replace('gcn-matrix-deferred end /tmp/', 'gcn-matrix-deferred end /wrong/')):
            with self.assertRaises(ValueError):
                matrix.audit_bounded_workload(broken, client, spec, 8, 0)

    def test_truncated_capture_rejected(self):
        with self.assertRaises(ValueError):
            matrix.audit(self.good.split('efb-clear-result')[0], CASES['interior'], 4, 0)

    def test_missing_batch_rejected(self):
        incomplete = '\n'.join(line for line in self.good.splitlines() if 'iteration=0 batch=5 ' not in line)
        with self.assertRaises(ValueError):
            matrix.audit(incomplete, CASES['interior'], 4, 0)

    def test_wrong_oracle_rejected(self):
        with self.assertRaises(ValueError):
            matrix.audit(self.good, CASES['interior-even'], 4, 0)

    def test_nonzero_client_cannot_be_pass(self):
        with self.assertRaises(ValueError):
            matrix.audit(self.good, CASES['interior'], 4, 1)

    def test_success_client_cannot_hide_pixel_failure(self):
        with self.assertRaises(ValueError):
            matrix.audit(self.bad, CASES['interior-even'], 4, 0)

    def test_cleanup_absent_rejected(self):
        with self.assertRaises(ValueError):
            matrix.audit(self.good.replace('CPU console restored', ''), CASES['interior'], 4, 0)

    def test_repeated_color_hash_corruption_rejected(self):
        import re
        changed = re.sub(r'(iteration=2 batch=0 .*?hash=)[0-9a-f]+', r'\g<1>00000000', self.good)
        self.assertNotEqual(changed, self.good)
        with self.assertRaises(ValueError):
            matrix.audit(changed, CASES['interior'], 4, 0)

if __name__ == '__main__':
    unittest.main()
