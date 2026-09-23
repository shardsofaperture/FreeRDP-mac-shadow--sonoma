#!/usr/bin/env python3
"""Recompute comparable transport and queue metrics from the five raw captures."""
import csv
from datetime import datetime
import json
import math
from pathlib import Path
import re
import statistics
from zoneinfo import ZoneInfo

from analyze import parse_nettop, queues

ROOT = Path(__file__).resolve().parent
RUNS = {
    150: '20260923-071111-150KiB',
    200: '20260923-073051-200KiB',
    250: '20260923-073259-250KiB',
    300: '20260923-073511-300KiB',
    400: '20260923-073814-400KiB',
}
QUEUE_NAMES = ('FreeRDP Send-Q', 'sshd loopback Recv-Q', 'sshd external Send-Q')


def describe(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'mean': statistics.mean(values), 'median': statistics.median(values),
            'p95': ordered[math.ceil(.95 * len(ordered)) - 1],
            'max': ordered[-1], 'samples': len(values)}


def loopback_times(path, metadata):
    local_date = datetime.fromisoformat(metadata['start_utc']).astimezone(
        ZoneInfo('America/New_York')).date()
    header = None
    sample = -1
    owner = None
    result = []
    with path.open(newline='') as stream:
        rows = list(csv.reader(stream))
    for cells in rows:
        if 'bytes_out' in cells:
            header = cells
            sample += 1
            owner = None
            continue
        if sample <= 0 or not header or len(cells) != len(header):
            continue
        name = ' '.join(cells)
        if re.search(r'freerdp-shadow[^, ]*\.' + str(metadata['listener_pid']) + r'\b', name):
            owner = 'rdp'
        elif re.search(r'sshd\.' + str(metadata['sshd_pid']) + r'\b', name):
            owner = 'sshd'
        if (owner == 'rdp' and '127.0.0.1:3390' in name and
                metadata['loopback_local'] in name and 'Established' in name):
            clock = datetime.strptime(cells[0], '%H:%M:%S.%f').time()
            result.append(datetime.combine(local_date, clock,
                                           ZoneInfo('America/New_York')))
    return result


def settle_time(samples, start, threshold=32 * 1024):
    """Seconds to the first of six consecutive <=threshold queue observations."""
    for index in range(start, len(samples) - 5):
        block = samples[index:index + 6]
        if all(all(row[name] is not None and row[name] <= threshold
                   for name in QUEUE_NAMES) for _, row in block):
            return max(0, (block[0][0] - samples[start][0]).total_seconds())
    return None


def calculate(rate):
    directory = ROOT / 'runs' / RUNS[rate]
    metadata = json.loads((directory / 'metadata.json').read_text())
    loopback, external, rtts, retrans = parse_nettop(directory / 'nettop.csv', metadata)
    samples, errors = queues(directory / 'queues.jsonl', metadata)
    complete = [(stamp, row) for stamp, row in samples
                if all(row[name] is not None for name in QUEUE_NAMES)]
    maxima = {name: max((row[name] for _, row in samples if row[name] is not None),
                        default=None) for name in QUEUE_NAMES}
    ending = {name: next((row[name] for _, row in reversed(samples)
                          if row[name] is not None), None) for name in QUEUE_NAMES}
    paired = list(zip(loopback, external))
    gaps = [incoming - outgoing for incoming, outgoing in paired
            if incoming - outgoing > max(16384, outgoing * .2)]
    peak_index = max(range(len(complete)), key=lambda i: max(complete[i][1].values())) if complete else None
    peak_settle = settle_time(complete, peak_index) if peak_index is not None else None
    active_end_queue = None
    post_active_settle = None
    active_loopback = None
    active_external = None
    active_ratio = None
    first_index = metadata.get('active_first_sample')
    end_index = metadata.get('active_last_sample')
    if first_index is not None and end_index is not None and first_index <= end_index:
        active_loopback = loopback[first_index:end_index + 1]
        active_external = external[first_index:end_index + 1]
        matched = list(zip(active_loopback, active_external))
        if matched and sum(incoming for incoming, _ in matched):
            active_ratio = (sum(outgoing for _, outgoing in matched) /
                            sum(incoming for incoming, _ in matched))
    times = loopback_times(directory / 'nettop.csv', metadata)
    if end_index is not None and end_index < len(times) and complete:
        active_end = times[end_index]
        nearest = min(complete, key=lambda item: abs((item[0] - active_end).total_seconds()))
        if abs((nearest[0] - active_end).total_seconds()) <= 1.0:
            active_end_queue = nearest[1]
        after = next((i for i, (stamp, _) in enumerate(complete) if stamp >= active_end), None)
        if after is not None:
            post_active_settle = settle_time(complete, after)
    result = {
        'rate_kib_per_s': rate, 'requested_bytes_per_s': rate * 1024,
        'run_directory': str(directory), 'start_utc': metadata['start_utc'],
        'ready_utc': metadata.get('ready_utc'), 'stop_utc': metadata['stop_utc'],
        'rdp_plaintext_bytes_per_s': describe(loopback),
        'ssh_encrypted_bytes_per_s': describe(external),
        'external_to_input_ratio_paired_samples':
            (sum(out for _, out in paired) / sum(inc for inc, _ in paired)
             if paired and sum(inc for inc, _ in paired) else None),
        'detected_high_rate_phase_plaintext_bytes_per_s': describe(active_loopback),
        'detected_high_rate_phase_external_bytes_per_s': describe(active_external),
        'detected_high_rate_phase_external_to_input_ratio': active_ratio,
        'max_queues_bytes': maxima, 'end_queues_bytes': ending,
        'queue_at_end_of_detected_high_rate_phase_bytes': active_end_queue,
        'peak_to_32k_sustained_first_sample_seconds': peak_settle,
        'post_high_rate_to_32k_sustained_first_sample_seconds': post_active_settle,
        'rtt_avg_ms': {'mean': statistics.mean(rtts), 'max_sampled_average': max(rtts)}
        if rtts else None,
        'nettop_re_tx_raw_sum': sum(retrans),
        'nettop_re_tx_nonzero_samples': sum(value > 0 for value in retrans),
        'material_input_external_gap_intervals': len(gaps),
        'max_material_gap_bytes_per_s': max(gaps, default=0),
        'sshd_loopback_recvq_samples_above_64k':
            sum(row['sshd loopback Recv-Q'] is not None and
                row['sshd loopback Recv-Q'] > 65536 for _, row in samples),
        'sshd_loopback_recvq_samples_above_128k':
            sum(row['sshd loopback Recv-Q'] is not None and
                row['sshd loopback Recv-Q'] > 131072 for _, row in samples),
        'sshd_loopback_recvq_samples_above_256k':
            sum(row['sshd loopback Recv-Q'] is not None and
                row['sshd loopback Recv-Q'] > 262144 for _, row in samples),
        'queue_capture_errors': sorted(errors),
        'auto_stop_reason': metadata.get('auto_stop_reason'),
        'detected_high_rate_first_sample': metadata.get('active_first_sample'),
        'detected_high_rate_last_sample': end_index,
    }
    return result


if __name__ == '__main__':
    all_metrics = {str(rate): calculate(rate) for rate in RUNS}
    for rate, metrics in all_metrics.items():
        directory = Path(metrics['run_directory'])
        (directory / 'metrics.json').write_text(json.dumps(metrics, indent=2) + '\n')
    (ROOT / 'campaign-metrics.json').write_text(json.dumps(all_metrics, indent=2) + '\n')
    for rate, metrics in all_metrics.items():
        print(rate, metrics['rdp_plaintext_bytes_per_s']['p95'],
              metrics['ssh_encrypted_bytes_per_s']['p95'],
              metrics['external_to_input_ratio_paired_samples'],
              metrics['max_queues_bytes'],
              metrics['peak_to_32k_sustained_first_sample_seconds'])
