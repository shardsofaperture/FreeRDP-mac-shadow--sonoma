#!/usr/bin/env python3
"""Offline transport/queue summary for one captured rate-sweep evidence directory."""
import argparse
import csv
from datetime import datetime
import json
import math
from pathlib import Path
import re
import statistics


def number(value):
    match = re.fullmatch(r'\s*([0-9]+(?:\.[0-9]+)?)(?:\s*(B|KiB|MiB|KB|MB|ms|us))?\s*', value or '')
    if not match:
        return None
    amount = float(match.group(1))
    unit = match.group(2)
    return amount * {'KiB': 1024, 'MiB': 1048576, 'KB': 1000, 'MB': 1000000,
                     'us': .001}.get(unit, 1)


def key(value):
    return re.sub(r'[^a-z0-9]', '', value.lower())


def field(row, names):
    normalized = {key(name): value for name, value in row.items() if name is not None}
    for name in names:
        if name in normalized:
            return number(normalized[name])
    return None


def summary(values):
    if not values:
        return 'unavailable'
    ordered = sorted(values)
    return (f'mean {statistics.mean(values):.0f}, median {statistics.median(values):.0f}, '
            f'p95 {ordered[math.ceil(.95 * len(ordered)) - 1]:.0f}, max {ordered[-1]:.0f} '
            f'B/s ({len(values)} one-second samples)')


def endpoint_token(endpoint):
    return endpoint.replace(':', '.')


def has_endpoint(text, endpoint):
    return endpoint in text or endpoint_token(endpoint) in text


def parse_nettop(path, metadata):
    header = None
    sample = -1
    loopback = []
    external = []
    rtts = []
    retrans = []
    owner_pid = None
    with path.open(newline='') as stream:
        rows = list(csv.reader(stream))
    for cells in rows:
        if not cells:
            continue
        normalized = {key(cell) for cell in cells}
        if normalized.intersection({'txbytes', 'bytesout', 'bytessent', 'sentbytes', 'txbyte'}):
            header = cells
            sample += 1
            owner_pid = None
            continue
        # nettop -d prints connection-lifetime totals in its first sample;
        # later samples are one-second deltas. Keep raw CSV but omit that seed.
        if sample == 0 or not header or len(cells) != len(header):
            continue
        row = dict(zip(header, cells))
        joined = ' '.join(cells)
        if re.search(r'freerdp-shadow[^, ]*\.' + str(metadata['listener_pid']) + r'\b', joined):
            owner_pid = metadata['listener_pid']
        elif re.search(r'sshd[. ]' + str(metadata['sshd_pid']) + r'\b', joined):
            owner_pid = metadata['sshd_pid']
        tx = field(row, ('txbytes', 'bytesout', 'bytessent', 'sentbytes', 'txbyte'))
        if tx is None:
            continue
        loop_pair = (metadata['loopback_local'], metadata['loopback_remote'])
        external_pair = (metadata['external_local'], metadata['external_remote'])
        if (all(has_endpoint(joined, endpoint) for endpoint in loop_pair) and
                (str(metadata['listener_pid']) in joined or owner_pid == metadata['listener_pid'])):
            loopback.append(tx)
        elif (all(has_endpoint(joined, endpoint) for endpoint in external_pair) and
              (str(metadata['sshd_pid']) in joined or owner_pid == metadata['sshd_pid'])):
            external.append(tx)
            rtt = field(row, ('rttavg', 'avgrtt', 'rtt'))
            if rtt is not None:
                rtts.append(rtt)
            count = field(row, ('retransmits', 'retransmit', 'retransmissions',
                                'rexmit', 'txretransmit', 'retx'))
            if count is not None:
                retrans.append(count)
    return loopback, external, rtts, retrans


def queues(path, metadata):
    desired = {
        'FreeRDP Send-Q': (metadata['loopback_remote'], metadata['loopback_local'], 2),
        'sshd loopback Recv-Q': (metadata['loopback_local'], metadata['loopback_remote'], 1),
        'sshd external Send-Q': (metadata['external_local'], metadata['external_remote'], 2),
    }
    samples = []
    errors = set()
    for line in path.read_text().splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            # A live reader may encounter the writer's final partial line.
            continue
        if item.get('error'):
            errors.add(item['error'])
        values = {name: None for name in desired}
        for raw in item['rows']:
            fields = raw.split()
            if len(fields) < 6 or fields[5] != 'ESTABLISHED':
                continue
            for name, (local, remote, offset) in desired.items():
                if fields[3] == endpoint_token(local) and fields[4] == endpoint_token(remote):
                    values[name] = int(fields[offset])
        samples.append((datetime.fromisoformat(item['utc']), values))
    return samples, errors


def queue_report(samples):
    names = ('FreeRDP Send-Q', 'sshd loopback Recv-Q', 'sshd external Send-Q')
    for name in names:
        values = [row[name] for _, row in samples if row[name] is not None]
        if values:
            print(f'{name}: max {max(values)} B; end {values[-1]} B')
        else:
            print(f'{name}: unavailable')
    complete = [(stamp, row) for stamp, row in samples if all(row[name] is not None for name in names)]
    if not complete:
        print('Approximate drain: unavailable (no complete queue samples)')
        return
    peak_index = max(range(len(complete)), key=lambda i: sum(complete[i][1].values()))
    drained = next((stamp for stamp, row in complete[peak_index + 1:]
                    if all(row[name] <= 2048 for name in names)), None)
    if drained:
        duration = (drained - complete[peak_index][0]).total_seconds()
        print(f'Approximate drain: {duration:.2f} s from observed queue peak to all three queues <=2048 B')
    else:
        print('Approximate drain: not observed before capture stopped')


def analyze(directory):
    metadata = json.loads((directory / 'metadata.json').read_text())
    print(f'Evidence: {directory}')
    print(f'Rate selected: {metadata["selected_rate_kib_per_s"]} KiB/s = '
          f'{metadata["selected_rate_bytes_per_s"]} B/s')
    print(f'BuildLabel: {metadata["installed_build_label"]}; '
          f'runtime: {metadata["runtime_build_label"]}')
    print(f'Start UTC: {metadata["start_utc"]}; stop UTC: {metadata.get("stop_utc", "missing")}')
    print('Bytes: plaintext RDP on loopback; encrypted SSH output externally includes SSH/TCP overhead.')
    print('No client display or visible convergence timing is inferred from these transport counters.')
    loopback, external, rtts, retrans = parse_nettop(directory / 'nettop.csv', metadata)
    print(f'Loopback FreeRDP→sshd: {summary(loopback)}')
    print(f'External sshd→phone: {summary(external)}')
    if rtts:
        print(f'External sampled rtt_avg: mean {statistics.mean(rtts):.2f} ms; '
              f'max sampled average {max(rtts):.2f} ms')
    else:
        print('External RTT: unavailable in nettop CSV')
    if retrans:
        print(f'External nettop re-tx: raw sum {sum(retrans):.0f} across '
              f'{sum(value > 0 for value in retrans)} nonzero delta samples '
              '(field unit not established)')
    else:
        print('External retransmits: unavailable in nettop CSV')
    periods = [(i, int(incoming - outgoing)) for i, (incoming, outgoing)
               in enumerate(zip(loopback, external), 1)
               if incoming - outgoing > max(16384, outgoing * .2)]
    print(f'Loopback materially above external output: {len(periods)} aligned 1 s intervals '
          '(>16,384 B/s and >20% of external output); sample indices/byte gaps: '
          f'{periods[:20]}')
    samples, errors = queues(directory / 'queues.jsonl', metadata)
    queue_report(samples)
    if errors:
        print(f'Queue capture errors: {sorted(errors)}')
    stderr = (directory / 'nettop.stderr').read_text().strip()
    if stderr:
        print(f'nettop stderr: {stderr[:500]}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence_dir', type=Path)
    args = parser.parse_args()
    analyze(args.evidence_dir)
