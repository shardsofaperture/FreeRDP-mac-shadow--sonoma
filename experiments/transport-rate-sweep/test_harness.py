#!/usr/bin/env python3
"""Offline contract tests; never opens sockets or starts capture."""
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import tempfile
import unittest

import analyze
import discover
from auto_stop import AutoStop
from campaign_metrics import describe, settle_time


class HarnessTest(unittest.TestCase):
    def setUp(self):
        self.metadata = {
            'loopback_local': '127.0.0.1:53965',
            'loopback_remote': '127.0.0.1:3390',
            'external_local': '192.168.50.75:22',
            'external_remote': '172.59.25.22:1079',
            'listener_pid': 66212, 'sshd_pid': 69522,
        }

    def test_discovery_rows(self):
        rows = discover.records([
            'sshd 69522 Zach 9u IPv4 0x123 0t0 TCP '
            '127.0.0.1:53965->127.0.0.1:3390 (ESTABLISHED)',
            'sshd 69522 Zach 4u IPv4 0x456 0t0 TCP '
            '192.168.50.75:22->172.59.25.22:1079 (ESTABLISHED)',
        ])
        self.assertEqual([name for _, _, name in rows], [
            '127.0.0.1:53965->127.0.0.1:3390 (ESTABLISHED)',
            '192.168.50.75:22->172.59.25.22:1079 (ESTABLISHED)'])

    def test_nettop_and_queues(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'nettop.csv').write_text(
                'name,txbytes,rxbytes,rtt_avg,retransmits\n'
                'freerdp-shadow-.66212 127.0.0.1.3390->127.0.0.1.53965,5000000,0,,0\n'
                'sshd.69522 192.168.50.75.22->172.59.25.22.1079,5000000,0,70ms,0\n'
                'name,txbytes,rxbytes,rtt_avg,retransmits\n'
                'freerdp-shadow-cli.66212 127.0.0.1.3390->127.0.0.1.53965,200000,0,,0\n'
                'sshd.69522 192.168.50.75.22->172.59.25.22.1079,150000,0,80ms,0\n'
                'name,txbytes,rxbytes,rtt_avg,retransmits\n'
                'freerdp-shadow-cli.66212 127.0.0.1.3390->127.0.0.1.53965,100000,0,,0\n'
                'sshd.69522 192.168.50.75.22->172.59.25.22.1079,120000,0,90ms,1\n')
            loopback, external, rtts, retrans = analyze.parse_nettop(
                directory / 'nettop.csv', self.metadata)
            self.assertEqual(loopback, [200000, 100000])
            self.assertEqual(external, [150000, 120000])
            self.assertEqual(rtts, [80, 90])
            self.assertEqual(retrans, [0, 1])
            start = datetime(2026, 9, 22, tzinfo=timezone.utc)
            rows = []
            for index, send in enumerate((10000, 1000)):
                rows.append(json.dumps({'utc': (start + timedelta(seconds=index)).isoformat(),
                                        'error': '', 'returncode': 0,
                                        'rows': [
                                            f'tcp4 0 {send} 127.0.0.1.3390 127.0.0.1.53965 ESTABLISHED 0 0 66212',
                                            'tcp4 5000 0 127.0.0.1.53965 127.0.0.1.3390 ESTABLISHED 0 0 69522',
                                            'tcp4 0 1500 192.168.50.75.22 172.59.25.22.1079 ESTABLISHED 0 0 69522',
                                        ]}))
            (directory / 'queues.jsonl').write_text('\n'.join(rows) + '\n')
            samples, errors = analyze.queues(directory / 'queues.jsonl', self.metadata)
            self.assertFalse(errors)
            self.assertEqual(samples[0][1]['FreeRDP Send-Q'], 10000)
            self.assertEqual(samples[-1][1]['sshd external Send-Q'], 1500)

    def test_observed_macos_nettop_layout(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'nettop.csv'
            header = 'time,,interface,state,bytes_in,bytes_out,rx_dupe,rx_ooo,re-tx,rtt_avg\n'
            path.write_text(
                header +
                '07:11:11,freerdp-shadow-.66212,,,0,5000000,0,0,0,\n'
                '07:11:11,tcp4 127.0.0.1:3390<->127.0.0.1:53965,lo0,Established,0,5000000,0,0,0,1 ms\n'
                '07:11:11,sshd.69522,,,0,5000000,0,0,0,\n'
                '07:11:11,tcp4 192.168.50.75:22<->172.59.25.22:1079,en0,Established,0,5000000,0,0,0,70 ms\n' +
                header +
                '07:11:12,freerdp-shadow-.66212,,,0,153600,0,0,0,\n'
                '07:11:12,tcp4 127.0.0.1:3390<->127.0.0.1:53965,lo0,Established,0,153600,0,0,0,1 ms\n'
                '07:11:12,sshd.69522,,,0,155000,0,0,0,\n'
                '07:11:12,tcp4 192.168.50.75:22<->172.59.25.22:1079,en0,Established,0,155000,0,0,4092,90 ms\n')
            loopback, external, rtts, retrans = analyze.parse_nettop(path, self.metadata)
            self.assertEqual(loopback, [153600])
            self.assertEqual(external, [155000])
            self.assertEqual(rtts, [90])
            self.assertEqual(retrans, [4092])

    def test_bounded_auto_stop_decision(self):
        start = datetime(2026, 9, 23, tzinfo=timezone.utc)
        def qrows(values):
            return [(start + timedelta(seconds=i / 2),
                     {'FreeRDP Send-Q': value,
                      'sshd loopback Recv-Q': value,
                      'sshd external Send-Q': value})
                    for i, value in enumerate(values)]
        monitor = AutoStop(200, seen_samples=2,
                           baseline_rates=[30000, 32000, 120000, 30000] * 2,
                           baseline_queue=30000)
        self.assertIsNone(monitor.observe([0, 0, 120000, 30000, 120000],
                                          qrows([20000] * 8)))
        self.assertIsNone(monitor.observe([0, 0, 120000, 30000, 120000,
                                          170000, 180000, 175000], qrows([20000] * 8)))
        self.assertIsNone(monitor.observe([0, 0, 120000, 30000, 120000,
                                          170000, 180000, 175000, 1000, 1000, 1000],
                                          qrows([20000] * 8 + [0] * 6)))
        self.assertEqual(monitor.observe([0, 0, 120000, 30000, 120000,
                                          170000, 180000, 175000, 1000, 1000, 1000,
                                          1000, 1000, 1000], qrows([20000] * 8 + [0] * 8)),
                         'activity_then_drain')
        pressure = AutoStop(300)
        self.assertEqual(pressure.observe([], qrows([300000] * 4)),
                         'dangerous_external_queue')
        loopback_pressure = [(stamp, {**row, 'sshd external Send-Q': 0,
                                      'sshd loopback Recv-Q': 300000})
                             for stamp, row in qrows([0] * 4)]
        self.assertEqual(AutoStop(400).observe([], loopback_pressure),
                         'dangerous_loopback_queue')

    def test_comparable_metrics(self):
        self.assertEqual(describe([100, 200, 300, 400])['p95'], 400)
        start = datetime(2026, 9, 23, tzinfo=timezone.utc)
        samples = [(start + timedelta(seconds=i),
                    {'FreeRDP Send-Q': 0, 'sshd loopback Recv-Q': value,
                     'sshd external Send-Q': 0})
                   for i, value in enumerate([100000, 80000, 60000, 30000,
                                              30000, 30000, 30000, 30000, 30000])]
        self.assertEqual(settle_time(samples, 0), 3)


if __name__ == '__main__':
    unittest.main()
