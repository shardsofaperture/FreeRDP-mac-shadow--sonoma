#!/usr/bin/env python3
"""Capture one large-window promotion; --auto-stop uses bounded telemetry recovery."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import plistlib
import signal
import subprocess
import sys
import threading
import time

from discover import discover
from analyze import parse_nettop, queues as read_queues
from auto_stop import AutoStop

ROOT = Path(__file__).resolve().parents[2]
INSTALLED = Path('/Users/zach/Applications/FreeRDP Shadow.app')
RATES = (150, 200, 250, 300, 400)
MARKERS = ('Mac bitmap pacer:', 'Mac accepted TCP SO_SNDBUF',
           'N coverage early', 'RateSweep')


def now():
    return datetime.now(timezone.utc).isoformat()


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def listener_pid():
    output = command('lsof', '-nP', '-t', '-iTCP@127.0.0.1:3390', '-sTCP:LISTEN')
    pids = set(output.splitlines())
    if len(pids) != 1:
        raise RuntimeError(f'Expected one RDP listener, found {pids}')
    return int(pids.pop())


def installed_state(rate):
    with (INSTALLED / 'Contents/Resources/ShadowConfig.plist').open('rb') as stream:
        config = plistlib.load(stream)
    with (INSTALLED / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    expected = f'--rate-sweep-kib={rate}'
    if (config.get('ExperimentalArm') != 'RateSweep' or
            config.get('GraphicsPacingMode') != 'FixedRuntime' or
            config.get('GraphicsRateKiBPerSecond') != 0 or
            config.get('SocketSendBufferKiB') != 0 or
            config.get('LargeRefreshBurst') is not False or
            config.get('CoverageScheduler') is not False or
            config.get('ListenerAddress') != '127.0.0.1' or
            config.get('ListenerPort') != 3390 or
            info.get('CFBundleIdentifier') != 'io.freerdp.shadow.sonoma.menu'):
        raise RuntimeError('Installed RateSweep configuration is unexpected')
    menu = command('pgrep', '-fl', 'FreeRDPShadowMenu')
    lines = [line for line in menu.splitlines() if
             str(INSTALLED / 'Contents/MacOS/FreeRDPShadowMenu') in line]
    if len(lines) != 1 or expected not in lines[0]:
        raise RuntimeError(f'Installed menu process does not select {rate} KiB/s: {lines}')
    pid = listener_pid()
    server_command = command('ps', '-p', str(pid), '-o', 'command=')
    if not server_command.startswith(str(INSTALLED / 'Contents/MacOS/freerdp-shadow-cli')):
        raise RuntimeError(f'Unexpected listener path: {server_command}')
    return {'selected_rate_kib_per_s': rate, 'selected_rate_bytes_per_s': rate * 1024,
            'installed_build_label': config['BuildLabel'],
            'runtime_build_label': f'0.2.0-RateSweep — Fixed {rate} KiB/s',
            'installed_app': str(INSTALLED), 'menu_command': lines[0],
            'listener_pid': pid, 'listener_command': server_command}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rate_kib', type=int, choices=RATES)
    parser.add_argument('--auto-stop', action='store_true',
                        help='Stop after substantial graphics activity and queue recovery')
    args = parser.parse_args()
    state = installed_state(args.rate_kib)
    if args.auto_stop:
        deadline = time.monotonic() + 30
        previous = None
        stable = 0
        while True:
            try:
                current = discover()
                stable = stable + 1 if current == previous else 1
                previous = current
                if stable >= 3:
                    tunnel = current
                    break
            except RuntimeError:
                stable = 0
                previous = None
            if time.monotonic() >= deadline:
                raise RuntimeError('RDP SSH tunnel did not stay connected for three seconds')
            time.sleep(1)
    else:
        tunnel = discover()
    root = ROOT / 'experiments/transport-rate-sweep/runs'
    root.mkdir(parents=True, exist_ok=True)
    directory = root / f'{datetime.now().strftime("%Y%m%d-%H%M%S")}-{args.rate_kib}KiB'
    directory.mkdir()
    metadata = {**state, **tunnel, 'start_utc': now(),
                'nettop_command': ['nettop', '-L', '0', '-d', '-x', '-n', '-m', 'tcp',
                                   '-p', str(state['listener_pid']), '-p', str(tunnel['sshd_pid']),
                                   '-s', '1']}
    (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    stop = threading.Event()
    children = []
    nettop_file = (directory / 'nettop.csv').open('w')
    nettop_error = (directory / 'nettop.stderr').open('w')
    nettop = subprocess.Popen(metadata['nettop_command'], stdout=nettop_file,
                              stderr=nettop_error, start_new_session=True)
    children.append(nettop)
    server_log = Path.home() / 'Library/Logs/FreeRDPShadow/server.log'
    tail = subprocess.Popen(['tail', '-n', '0', '-F', str(server_log)],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, bufsize=1, start_new_session=True)
    children.append(tail)

    def queue_loop():
        with (directory / 'queues.jsonl').open('w', buffering=1) as output:
            while not stop.is_set():
                stamp = now()
                result = subprocess.run(['netstat', '-anv', '-p', 'tcp'],
                                        capture_output=True, text=True)
                endpoints = (tunnel['loopback_local'], tunnel['external_local'],
                             tunnel['external_remote'])
                rows = [line for line in result.stdout.splitlines()
                        if any(endpoint.replace(':', '.') in line for endpoint in endpoints)]
                output.write(json.dumps({'utc': stamp, 'rows': rows,
                                         'error': result.stderr.strip(),
                                         'returncode': result.returncode}) + '\n')
                stop.wait(0.5)

    def log_loop():
        with (directory / 'server-relevant.log').open('w', buffering=1) as output:
            for line in tail.stdout:
                if stop.is_set():
                    break
                if any(marker in line for marker in MARKERS):
                    output.write(f'{now()} {line}')

    queues = threading.Thread(target=queue_loop, daemon=True)
    logs = threading.Thread(target=log_loop, daemon=True)
    queues.start()
    logs.start()
    try:
        time.sleep(1.0)
        if nettop.poll() is not None:
            raise RuntimeError(f'nettop exited early; see {directory / "nettop.stderr"}')
        if args.auto_stop:
            baseline_deadline = time.monotonic() + 30
            print('Measuring connected idle transport baseline before READY...', flush=True)
            while True:
                if nettop.poll() is not None:
                    raise RuntimeError('nettop exited before READY; inspect stderr')
                current = discover()
                if current['sshd_pid'] != tunnel['sshd_pid']:
                    raise RuntimeError('Forwarding sshd changed before READY; restart capture')
                if current != tunnel:
                    tunnel = current
                    metadata.update(tunnel)
                    (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
                    baseline_deadline = time.monotonic() + 30
                loopback, _, _, _ = parse_nettop(directory / 'nettop.csv', metadata)
                queue_samples, errors = read_queues(directory / 'queues.jsonl', metadata)
                if errors:
                    raise RuntimeError(f'netstat queue capture failed: {sorted(errors)}')
                recent_queues = queue_samples[-8:]
                if (len(loopback) >= 8 and len(recent_queues) == 8 and
                        all(all(value is not None for value in row.values())
                            for _, row in recent_queues) and
                        max(max(row.values()) for _, row in recent_queues) <= 128 * 1024):
                    break
                if time.monotonic() >= baseline_deadline:
                    raise RuntimeError('No stable connected baseline within 30 seconds; no READY issued')
                time.sleep(0.5)
            baseline_rates = loopback[-8:]
            baseline_queue = max(max(row.values()) for _, row in recent_queues)
            monitor = AutoStop(args.rate_kib, seen_samples=len(loopback),
                               baseline_rates=baseline_rates, baseline_queue=baseline_queue)
            metadata['baseline_rate_median_bps'] = sorted(baseline_rates)[len(baseline_rates) // 2]
            metadata['baseline_queue_max_bytes'] = baseline_queue
            metadata['activity_floor_bps'] = monitor.substantial_floor
            metadata['return_rate_ceiling_bps'] = monitor.idle_ceiling
            print(f'Baseline: median about {metadata["baseline_rate_median_bps"]} B/s; '
                  f'activity floor {monitor.substantial_floor} B/s; '
                  f'queue baseline max {baseline_queue} B', flush=True)
        print(f'Evidence: {directory}', flush=True)
        print(f'Rate: {args.rate_kib} KiB/s = {args.rate_kib * 1024} B/s; sshd PID: {tunnel["sshd_pid"]}', flush=True)
        metadata['ready_utc'] = now()
        (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'READY @ {args.rate_kib} KiB/s — perform the standard large-window promotion now.', flush=True)
        if not args.auto_stop:
            monitor = None
        ready_monotonic = time.monotonic()
        while not stop.wait(0.5):
            if nettop.poll() is not None:
                raise RuntimeError('nettop exited before Ctrl-C; inspect stderr')
            if monitor:
                if time.monotonic() - ready_monotonic > 180:
                    raise RuntimeError('Auto-stop timed out after 180 seconds; inspect partial evidence')
                try:
                    os.kill(state['listener_pid'], 0)
                except ProcessLookupError:
                    raise RuntimeError('Installed listener process exited')
                current = discover()
                if current != tunnel:
                    raise RuntimeError('RDP SSH tunnel changed after READY; run is not comparable')
                loopback, _, _, _ = parse_nettop(directory / 'nettop.csv', metadata)
                queue_samples, errors = read_queues(directory / 'queues.jsonl', metadata)
                if errors:
                    raise RuntimeError(f'netstat queue capture failed: {sorted(errors)}')
                if (len(queue_samples) >= 4 and
                        all(row['sshd loopback Recv-Q'] is None or
                            row['sshd external Send-Q'] is None
                            for _, row in queue_samples[-4:])):
                    raise RuntimeError('RDP SSH tunnel disappeared from netstat')
                reason = monitor.observe(loopback, queue_samples)
                if reason == 'dangerous_external_queue':
                    raise RuntimeError('External SSH Send-Q exceeded 256 KiB for four samples')
                if reason == 'dangerous_loopback_queue':
                    raise RuntimeError('sshd loopback Recv-Q exceeded the bounded pressure guard')
                if reason == 'activity_then_drain':
                    metadata['auto_stop_reason'] = reason
                    metadata['active_first_sample'] = monitor.first_active_sample
                    metadata['active_last_sample'] = monitor.last_active_sample
                    metadata['post_activity_quiet_samples'] = (
                        len(loopback) - monitor.last_active_sample - 1)
                    print('AUTO-STOP: substantial graphics activity and queue recovery observed.',
                          flush=True)
                    break
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for child in children:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
        for child in children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
        queues.join(timeout=5)
        logs.join(timeout=5)
        nettop_file.close()
        nettop_error.close()
        metadata['stop_utc'] = now()
        (directory / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'Evidence retained: {directory}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(f'capture failed: {error}', file=sys.stderr)
        sys.exit(1)
