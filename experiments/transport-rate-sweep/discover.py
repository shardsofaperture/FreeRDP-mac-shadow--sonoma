#!/usr/bin/env python3
"""Find the sshd process owning the current RDP loopback tunnel."""
import json
import re
import subprocess
import sys


def lsof(*args):
    result = subprocess.run(['lsof', '-nP', *args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or 'lsof found no matching TCP connection')
    return result.stdout.splitlines()[1:]


def records(lines):
    for line in lines:
        fields = line.split(None, 8)
        if len(fields) == 9 and fields[7] == 'TCP':
            yield fields[0], int(fields[1]), fields[8]


def discover():
    matches = set()
    for process, pid, name in records(lsof('-iTCP@127.0.0.1:3390', '-sTCP:ESTABLISHED')):
        match = re.fullmatch(r'127\.0\.0\.1:(\d+)->127\.0\.0\.1:3390 \(ESTABLISHED\)', name)
        if process == 'sshd' and match:
            matches.add((pid, int(match.group(1))))
    if len(matches) != 1:
        raise RuntimeError(f'Expected one RDP tunnel sshd, found {sorted(matches)}')
    pid, ephemeral = matches.pop()
    external = set()
    for process, owner, name in records(lsof('-a', '-p', str(pid), '-iTCP', '-sTCP:ESTABLISHED')):
        if process != 'sshd' or owner != pid or '->' not in name:
            continue
        endpoints = name.split(' ', 1)[0]
        local, remote = endpoints.split('->', 1)
        if local == f'127.0.0.1:{ephemeral}' and remote == '127.0.0.1:3390':
            continue
        if not local.startswith(('127.', '[::1]')) and not remote.startswith(('127.', '[::1]')):
            external.add((local, remote))
    if len(external) != 1:
        raise RuntimeError(f'Expected one external SSH connection for sshd {pid}, found {sorted(external)}')
    local, remote = external.pop()
    return {'sshd_pid': pid, 'loopback_local': f'127.0.0.1:{ephemeral}',
            'loopback_remote': '127.0.0.1:3390', 'external_local': local,
            'external_remote': remote}


if __name__ == '__main__':
    try:
        print(json.dumps(discover(), indent=2, sort_keys=True))
    except RuntimeError as error:
        print(f'discovery failed: {error}', file=sys.stderr)
        sys.exit(1)
