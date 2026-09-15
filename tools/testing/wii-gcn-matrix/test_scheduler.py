import gzip
import importlib.util
from pathlib import Path
import unittest

TOOLS = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('scheduler', TOOLS / 'wii-gcn-scheduler-audit.py')
scheduler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scheduler)
FIXTURES = Path(__file__).parent / 'fixtures'

class SchedulerTests(unittest.TestCase):
    def setUp(self):
        self.trace, self.stats, self.client = [gzip.decompress((FIXTURES / (name+'.txt.gz')).read_bytes()).decode()
            for name in ('scheduler-trace','scheduler-stats','scheduler-client')]

    def test_complete_real_capture_identifies_scheduled_competitors(self):
        result = scheduler.audit(self.trace, self.stats, self.client)
        self.assertEqual((len(result['windows']), result['entries']), (64,891))
        self.assertEqual(result['other_tasks'][0]['comm'], 'kworker/0:1')
        self.assertEqual(result['other_tasks'][1]['comm'], 'dmesg')
        self.assertEqual(result['other_tasks'][0]['scheduled_ns'], 99983000)
        for window in result['windows']:
            self.assertEqual(sum(window['scheduled_ns'].values()), window['window_ns'])

    def test_loss_and_broken_windows_are_rejected(self):
        for broken in (self.trace.replace('GCN end iteration=63','GCN end iteration=62'),
                       self.trace.replace('prev_pid=11392', 'prev_pid=1', 1),
                       self.trace.replace('891/891', '890/891'),
                       self.trace.replace('GCN begin iteration=0','GCN begin iteration=1')):
            with self.assertRaises(ValueError):
                scheduler.audit(broken, self.stats, self.client)
        for broken in (self.stats.replace('overrun: 0', 'overrun: 1', 1),
                       self.stats.replace('dropped events: 0','dropped events: 1')):
            with self.assertRaises(ValueError):
                scheduler.audit(self.trace, broken, self.client)

    def test_missing_client_window_is_rejected(self):
        with self.assertRaises(ValueError):
            scheduler.audit(self.trace, self.stats, self.client.replace('SYSTEM TIMING: iteration=63','missing'))

    def test_workqueue_capture_preserves_function_evidence_without_assigning_unknown_work(self):
        trace, stats, client = [gzip.decompress((FIXTURES / (name+'.txt.gz')).read_bytes()).decode()
            for name in ('scheduler-workqueue-trace','scheduler-workqueue-stats','scheduler-workqueue-client')]
        result = scheduler.audit(trace, stats, client)
        self.assertEqual((len(result['windows']),result['entries']), (64,1257))
        starts = [e for e in result['workqueue_events'] if e['event']=='workqueue_execute_start']
        self.assertEqual(sum('function sdio_irq_work' in e['detail'] for e in starts),64)
        slow = result['windows'][63]
        self.assertEqual(slow['scheduled_ns'][10121],25955000)
        self.assertFalse(any(e['iteration']==63 for e in starts))
