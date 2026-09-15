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

    def test_whole_loop_tracks_work_started_before_ioctl(self):
        rows = [
            ('client-1', 'tracing_mark_write', 'GCN loop begin'),
            ('worker-2', 'workqueue_execute_start', 'work struct abc: function sdio_irq_work'),
            ('client-1', 'tracing_mark_write', 'GCN begin iteration=0'),
            ('client-1', 'sched_switch', 'prev_comm=client prev_pid=1 prev_prio=120 prev_state=R+ ==> next_comm=worker next_pid=2 next_prio=120'),
            ('worker-2', 'workqueue_execute_end', 'work struct abc: function sdio_irq_work'),
            ('worker-2', 'sched_switch', 'prev_comm=worker prev_pid=2 prev_prio=120 prev_state=S ==> next_comm=client next_pid=1 next_prio=120'),
            ('client-1', 'tracing_mark_write', 'GCN end iteration=0'),
            ('client-1', 'tracing_mark_write', 'GCN loop end'),
        ]
        trace = '# entries-in-buffer/entries-written: 8/8 #P:1\n' + '\n'.join(
            f'{task} [000] ...1. 1.{i:06d}: {event}: {payload}'
            for i, (task,event,payload) in enumerate(rows))
        client = 'SYSTEM TIMING: iteration=0 ns=3000 status=0'
        result = scheduler.audit(trace, self.stats, client)
        self.assertTrue(result['whole_loop'])
        self.assertEqual(result['windows'][0]['other_work_scheduled'], [
            dict(pid=2, function='sdio_irq_work', scheduled_ns=1000),
            dict(pid=2, function='unknown', scheduled_ns=1000)])
        for broken in (trace.replace('GCN loop end', 'GCN loop begin'),
                       trace.replace('workqueue_execute_end: work struct abc',
                                     'workqueue_execute_end: work struct def')):
            with self.assertRaises(ValueError):
                scheduler.audit(broken, self.stats, client)

    def test_real_whole_loop_with_nested_softirq_work(self):
        trace, stats, client = [gzip.decompress((FIXTURES / (name+'.txt.gz')).read_bytes()).decode()
            for name in ('scheduler-loop-trace','scheduler-loop-stats','scheduler-loop-client')]
        result = scheduler.audit(trace, stats, client)
        self.assertTrue(result['whole_loop'])
        self.assertEqual((len(result['windows']), result['entries']), (64,5279))
        for w in result['windows']:
            self.assertEqual(sum(e['scheduled_ns'] for e in w['other_work_scheduled']),
                             w['other_scheduled_ns'])
        slow = result['windows'][39]
        by_function = {e['function']:e['scheduled_ns'] for e in slow['other_work_scheduled']
                       if e['pid']==15274}
        self.assertEqual(by_function['sdio_irq_work'],9705000)
        self.assertEqual(by_function['drm_fb_helper_damage_work'],5011000)
        with self.assertRaises(ValueError):
            scheduler.audit(trace.replace('function tcp_tsq_workfn', 'function invalid', 1), stats, client)
