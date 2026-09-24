#!/usr/bin/env python3
"""Check changed and new repository content for private infrastructure data."""
import argparse
import ipaddress
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_EMAILS = {b'mac-shadow-rdp-maintainers@users.noreply.github.com'}
DOC_NETS = tuple(ipaddress.ip_network(n) for n in (
    '192.0.2.0/24', '198.51.100.0/24', '203.0.113.0/24', '2001:db8::/32'))
IPV4 = re.compile(rb'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])')
IPV6 = re.compile(rb'(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){2,}[0-9A-Fa-f]{0,4}(?![0-9A-Fa-f:])')
EMAIL = re.compile(
    rb'(?i)(?<![\w.+-])[A-Z0-9._%+-]+@'
    rb'(?![0-9]+x\.(?:png|jpe?g|gif|tiff?)\b)[A-Z0-9.-]+\.[A-Z]{2,}(?![\w.-])')
MAC = re.compile(rb'(?i)(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])')
HOME = re.compile(
    rb'/' + rb'Users/' + rb'(?!<HOME_DIR>)[^/\s<>\x00]+|'
    rb'/' + rb'home/' + rb'(?!<HOME_DIR>|user(?:[/\s<>\\]|$))[^/\s<>\x00]+')
LOCAL_HOST = re.compile(rb'(?i)\b[a-z0-9][a-z0-9-]{0,62}\.(?:local|lan|home|internal)\b')
PRIVATE_KEY = re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC |DSA |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----')
CREDENTIAL = re.compile(rb'(?i)(?:password|passwd|api[_-]?key|access[_-]?token|client[_-]?secret)\s*[:=]\s*["\']?([A-Za-z0-9/+_=-]{16,})')
TOKEN = re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16})\b')
APPLE_IDENTITY = re.compile(rb'(?i)Apple ' + rb'Development:\s*(?!<SIGNER_NAME>)[^\r\n<]+')
TEAM_ID = re.compile(rb'(?i)\bTeamIdentifier\s*[:=]\s*(?!<TEAM_ID>)[A-Z0-9]{10}\b')
SSH_PORT_LITERAL = re.compile(rb'(?i)\bssh\b[^\r\n]{0,160}?\s-p(?:\s+|=)\d{1,5}\b')


def run_git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def safe_ip(value):
    if value == b'127.0.0.1':
        return True
    try:
        address = ipaddress.ip_address(value.decode('ascii'))
    except ValueError:
        return True
    return (not address.is_loopback and not address.is_unspecified and
            any(address.version == network.version and address in network for network in DOC_NETS))


def safe_local_host(data):
    for match in LOCAL_HOST.finditer(data):
        # FreeRDP uses a C union member named `internal` in cast helpers.
        # It is not a network hostname; allow only the return-expression form.
        member = b'cnv.' + b'internal'
        if match.group(0).lower() == member:
            before = data[max(0, match.start() - 12):match.start()].lower().rstrip()
            after = data[match.end():match.end() + 2]
            if before.endswith(b'return') and after.startswith(b';'):
                continue
        return True
    return False


def changed_lines(base):
    args = ['diff', '--no-ext-diff', '--no-color', '--unified=0']
    if base:
        args.extend([base, '--'])
    else:
        args.extend(['HEAD', '--'])
    patch = run_git(*args)
    found = []
    current_path = b'<unknown>'
    for line in patch.splitlines():
        if line.startswith(b'+++ b/'):
            current_path = line[6:]
        elif line.startswith(b'+') and not line.startswith(b'+++'):
            found.append((current_path, line[1:]))
    return found


def added_files(base):
    raw = run_git('ls-files', '--others', '--exclude-standard', '-z')
    files = {Path(name.decode('utf-8', 'replace')) for name in raw.split(b'\x00') if name}
    diff_args = ['diff', '--name-only', '-z']
    diff_args.append(base if base else 'HEAD')
    diff_args.append('--')
    names = run_git(*diff_args)
    files.update(Path(name.decode('utf-8', 'replace')) for name in names.split(b'\x00') if name)
    return files


def chunks(data):
    if b'\x00' in data:
        for match in re.finditer(rb'[\x20-\x7e]{4,}', data):
            yield match.group(0)
    else:
        yield from data.splitlines() or [data]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', help='scan additions relative to this base ref (use in CI or after commit)')
    args = parser.parse_args()
    findings = set()
    lines = changed_lines(args.base)
    file_paths = added_files(args.base)
    checked = 0

    def check(path, data):
        nonlocal checked
        checked += 1
        if HOME.search(data): findings.add((path, 'personal home-directory path'))
        if safe_local_host(data): findings.add((path, 'private hostname'))
        if MAC.search(data): findings.add((path, 'hardware MAC address'))
        if PRIVATE_KEY.search(data): findings.add((path, 'private key material'))
        if APPLE_IDENTITY.search(data): findings.add((path, 'personal Apple signing identity'))
        if TEAM_ID.search(data): findings.add((path, 'Apple Team ID'))
        if SSH_PORT_LITERAL.search(data): findings.add((path, 'literal SSH port'))
        for match in IPV4.finditer(data):
            if not safe_ip(match.group(0)):
                findings.add((path, 'non-placeholder IPv4 address'))
        for match in IPV6.finditer(data):
            if not safe_ip(match.group(0)):
                findings.add((path, 'non-placeholder IPv6 address'))
        if any(email.lower() not in ALLOWED_EMAILS for email in EMAIL.findall(data)):
            findings.add((path, 'email outside project noreply identity'))
        if CREDENTIAL.search(data) or TOKEN.search(data):
            findings.add((path, 'credential-like value'))

    for path, line in lines:
        check(path.decode('utf-8', 'replace'), line)
    for path in sorted(file_paths):
        check(str(path), str(path).encode('utf-8', 'replace'))
        target = ROOT / path
        if not target.is_file():
            continue
        try:
            data = target.read_bytes()
        except OSError:
            findings.add((str(path), 'unreadable content'))
            continue
        # New and binary files have no useful line-level diff, so scan all bytes.
        if path in {Path(n.decode('utf-8', 'replace')) for n in
                    run_git('ls-files', '--others', '--exclude-standard', '-z').split(b'\x00') if n} or b'\x00' in data:
            for chunk in chunks(data):
                check(str(path), chunk)

    if findings:
        for path, rule in sorted(findings):
            print(f'{path}: {rule}', file=sys.stderr)
        return 1
    print(f'Privacy scan passed ({checked} changed or new content chunks).')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
