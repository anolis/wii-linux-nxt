#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Audit isolated single-CPU scheduler windows; report scheduled time, not GPU time."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re


def require(value, message):
    if not value:
        raise ValueError(message)


def audit(trace, stats, client):
    for field in ('overrun', 'commit overrun', 'dropped events'):
        match = re.search(rf'^{field}: (\d+)$', stats, re.M)
        require(match and int(match[1]) == 0, f'missing/nonzero {field}')
    counts = re.search(r'entries-in-buffer/entries-written: (\d+)/(\d+)\s+#P:1', trace)
    require(counts and counts[1] == counts[2], 'trace overwritten or not single CPU')
    timings = re.findall(r'SYSTEM TIMING: iteration=(\d+) ns=(\d+) status=(-?\d+)', client)
    windows, active, parsed = [], None, 0
    totals, names = defaultdict(int), {}
    work = []
    for line in trace.splitlines():
        if not line or line.startswith('#'):
            continue
        m = re.match(r'\s*(.+)-(\d+)\s+\[(\d+)\]\s+\S+\s+(\d+)\.(\d+): (\w+): (.*)$', line)
        require(m, 'unparsed trace record')
        parsed += 1
        comm, pid, cpu, sec, frac, event, payload = m.groups()
        pid = int(pid)
        require(int(cpu) == 0, 'unexpected CPU')
        stamp = int(sec)*1000000000 + int(frac.ljust(9, '0'))
        names[pid] = comm.strip()
        marker = re.fullmatch(r'GCN (begin|end) iteration=(\d+)', payload) if event == 'tracing_mark_write' else None
        if marker and marker[1] == 'begin':
            require(active is None and int(marker[2]) == len(windows), 'wrong begin order')
            active = dict(iteration=len(windows), client_pid=pid, start_ns=stamp,
                          current=pid, last=stamp, scheduled=defaultdict(int), switches=0)
            continue
        if active:
            require(stamp >= active['last'], 'time reversed')
            active['scheduled'][active['current']] += stamp-active['last']
            active['last'] = stamp
        if marker:
            require(active and int(marker[2]) == active['iteration'] and pid == active['client_pid']
                    and active['current'] == pid, 'wrong end marker')
            i = active['iteration']
            require(i < len(timings) and timings[i][0] == str(i), 'window/client mismatch')
            duration = stamp-active['start_ns']
            require(duration >= int(timings[i][1]), 'window shorter than ioctl interval')
            scheduled = dict(active['scheduled'])
            require(sum(scheduled.values()) == duration, 'unaccounted window time')
            for task, ns in scheduled.items():
                if task != pid:
                    totals[task] += ns
            windows.append(dict(iteration=i, client_pid=pid, window_ns=duration,
                                ioctl_ns=int(timings[i][1]), scheduled_ns=scheduled,
                                other_scheduled_ns=duration-scheduled.get(pid,0), switches=active['switches']))
            active = None
        elif event == 'sched_switch':
            switch = re.fullmatch(r'prev_comm=(.*?) prev_pid=(\d+) prev_prio=(\d+) prev_state=(\S+) ==> next_comm=(.*?) next_pid=(\d+) next_prio=(\d+)', payload)
            require(switch, 'malformed switch')
            prev, nxt = int(switch[2]), int(switch[6])
            names[prev], names[nxt] = switch[1], switch[5]
            if active:
                require(prev == active['current'] and pid == prev, 'broken task continuity')
                active['current'] = nxt
                active['switches'] += 1
        elif event in ('workqueue_execute_start', 'workqueue_execute_end'):
            work.append(dict(pid=pid, event=event, detail=payload,
                             iteration=active['iteration'] if active else None))
        else:
            raise ValueError('unexpected trace event')
    require(active is None and len(windows) == len(timings) and windows, 'incomplete windows')
    require(parsed == int(counts[1]), 'trace entry count mismatch')
    return dict(windows=windows, other_tasks=[dict(pid=p, comm=names[p], scheduled_ns=n)
                for p,n in sorted(totals.items(), key=lambda item:item[1], reverse=True)],
                workqueue_events=work, entries=parsed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--stats', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    raw = args.trace.read_bytes()
    stats = args.stats.read_text()
    expected = re.search(r'^([0-9a-f]{64})  /sys/kernel/tracing/instances/wii-gcn-sched/trace$', stats, re.M)
    require(expected and hashlib.sha256(raw).hexdigest() == expected[1], 'device trace checksum mismatch')
    result = audit(raw.decode(), stats, args.client.read_text())
    with args.output.open('x') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    for row in result['other_tasks'][:10]:
        print(f"{row['comm']} pid={row['pid']}: {row['scheduled_ns']/1e6:.3f} ms")


if __name__ == '__main__':
    main()
