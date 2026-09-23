#!/usr/bin/env python3
"""Build and bundle the production menu app without installing or launching it."""
import argparse
import hashlib
import json
import plistlib
import shlex
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SIGNING_IDENTITY = 'Apple Development: shardsofaperture (H7V72A5WH6)'
BUNDLE_ID = 'io.freerdp.shadow.sonoma.menu'
EXPERIMENT_VERSION = '0.2.0'
PRODUCTION_VERSION = '0.2.0'
PRODUCTION_LABEL = '0.2.0 — Fixed 250 KiB/s'
EXPERIMENT_ARMS = {
    'A': {'socket_kib': 0, 'rate_kib': 0, 'label': '0.2.0A'},
    'B': {'socket_kib': 32, 'rate_kib': 0, 'label': '0.2.0B'},
    'C': {'socket_kib': 16, 'rate_kib': 0, 'label': '0.2.0C'},
    'D': {'socket_kib': 4, 'rate_kib': 0, 'label': '0.2.0D'},
    'E': {'socket_kib': 8, 'rate_kib': 0,
          'label': '0.2.0E — Adaptive / 8K'},
    'F': {'socket_kib': 0, 'rate_kib': 150,
          'label': '0.2.0F — Default socket / Fixed 150 KiB/s'},
    'G': {'socket_kib': 32, 'rate_kib': 150,
          'label': '0.2.0G — 32K socket / Fixed 150 KiB/s'},
    'H': {'socket_kib': 0, 'rate_kib': 250,
          'label': '0.2.0H — Default socket / Fixed 250 KiB/s'},
    'I': {'socket_kib': 32, 'rate_kib': 250,
          'label': '0.2.0I — 32K socket / Fixed 250 KiB/s'},
    'J': {'socket_kib': 0, 'rate_kib': 175,
          'label': '0.2.0J — Fixed 175 KiB/s'},
    'K': {'socket_kib': 0, 'rate_kib': 200,
          'label': '0.2.0K — Fixed 200 KiB/s'},
    'L': {'socket_kib': 0, 'rate_kib': 150,
          'label': '0.2.0L — 150 KiB/s + bounded large-refresh burst',
          'large_refresh_burst': True},
    'M': {'socket_kib': 0, 'rate_kib': 150,
          'label': '0.2.0M — Default socket / Fixed 150 KiB/s / large-refresh diagnostic burst',
          'large_refresh_burst': True},
    'N': {'socket_kib': 0, 'rate_kib': 150,
          'label': '0.2.0N — Coverage scheduler / Fixed 150 KiB/s',
          'coverage_scheduler': True},
    'RateSweep': {'socket_kib': 0, 'rate_kib': 0,
                  'label': '0.2.0-RateSweep — select fixed rate at launch'},
}


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def output(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


def candidate_source_patch():
    patch = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=ROOT)
    untracked = output('git', '-C', ROOT, 'ls-files', '--others', '--exclude-standard').splitlines()
    sources = [name for name in untracked if not name.startswith('dist/')]
    for name in sources:
        item = ROOT / name
        if not item.is_file():
            continue
        result = subprocess.run(['git', 'diff', '--no-index', '--binary', '--', '/dev/null',
                                 name], cwd=ROOT, capture_output=True)
        if result.returncode not in (0, 1):
            raise RuntimeError(f'Cannot capture untracked source {name}: {result.stderr!r}')
        patch += result.stdout
    return patch, sources


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def signature_info(path):
    result = subprocess.run(['codesign', '-dv', '--verbose=4', str(path)], text=True,
                            capture_output=True, check=True)
    return result.stdout + result.stderr


def verify_signing_identity_available():
    identities = output('security', 'find-identity', '-v', '-p', 'codesigning')
    if SIGNING_IDENTITY not in identities:
        raise RuntimeError(f'Required signing identity is unavailable: {SIGNING_IDENTITY}')


def sign(path, identifier=None):
    args = ['codesign', '--force', '--sign', SIGNING_IDENTITY, '--timestamp=none']
    if identifier:
        args.extend(['--identifier', identifier])
    args.append(path)
    run(*args)


def verify_certificate_signature(path):
    info = signature_info(path)
    if 'Signature=adhoc' in info or f'Authority={SIGNING_IDENTITY}' not in info:
        raise RuntimeError(f'Certificate-backed signature verification failed: {path}\n{info}')
    team = re.search(r'^TeamIdentifier=(.+)$', info, re.MULTILINE)
    if not team or team.group(1) == 'not set':
        raise RuntimeError(f'Missing certificate team identifier for {path}\n{info}')


def rpaths(binary):
    return re.findall(r'cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset',
                      output('otool', '-l', binary))


def dependencies(binary):
    return [line.strip().split(' (compatibility')[0]
            for line in output('otool', '-L', binary).splitlines()[1:]]


def verify_release(build):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.match(r'([^:#/][^:]*):[^=]+=(.*)', line)
        if match:
            cache[match[1]] = match[2]
    if cache.get('CMAKE_BUILD_TYPE') != 'Release':
        raise RuntimeError('Production requires CMAKE_BUILD_TYPE=Release')
    disabled = {'BUILD_TESTING', 'BUILD_BENCHMARK', 'BUILD_FUZZERS',
                'WITH_VERBOSE_WINPR_ASSERT', 'WITH_PROFILER', 'WITH_GPROF',
                'WITH_VALGRIND_MEMCHECK', 'WITH_STREAMPOOL_DEBUG',
                'WITH_CURSOR_DUMP', 'WITH_GFX_FRAME_DUMP'}
    disabled.update(key for key in cache if key.startswith(('WITH_DEBUG_', 'WITH_SANITIZE_')))
    for key in disabled:
        if cache.get(key, 'OFF').upper() not in ('OFF', 'FALSE', 'NO', '0', ''):
            raise RuntimeError(f'Development option enabled: {key}={cache[key]}')
    commands = json.loads((build / 'compile_commands.json').read_text())
    if not commands:
        raise RuntimeError('No compiler commands to verify')
    for command in commands:
        flags = shlex.split(command['command'])
        if '-DNDEBUG' not in flags or '-O3' not in flags:
            raise RuntimeError(f'Missing Release flags: {command["file"]}')
        if any(flag in ('-O0', '-Og', '-g', '-g2', '-g3', '-pg', '--coverage', '-UNDEBUG') or
               flag.startswith(('-fsanitize', '-fprofile', '-DWITH_VERBOSE_WINPR_ASSERT',
                                '-DWITH_DEBUG_', '-DDEBUG')) for flag in flags):
            raise RuntimeError(f'Development compiler flags: {command["file"]}')
    print(f'Verified Release (-O3 -DNDEBUG), runtime assertions/debug instrumentation off '
          f'across {len(commands)} compiler commands')


def verify_production_config(metadata, config):
    if (ROOT / '.source_tag').read_text().strip() != PRODUCTION_VERSION:
        raise RuntimeError('Production source version must match app version')
    expected_metadata = {
        'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleShortVersionString': PRODUCTION_VERSION,
        'CFBundleVersion': PRODUCTION_VERSION,
        'FreeRDPShadowBuildLabel': PRODUCTION_LABEL,
    }
    expected_config = {
        'BuildLabel': PRODUCTION_LABEL,
        'ExperimentalArm': 'Production',
        'PacingDiagnostics': False,
        'SocketSendBufferKiB': 0,
        'GraphicsPacingMode': 'Fixed',
        'GraphicsRateKiBPerSecond': 250,
        'GraphicsBurstBytes': 0,
        'LargeRefreshBurst': False,
        'CoverageScheduler': False,
        'ListenerAddress': '127.0.0.1',
        'ListenerPort': 3390,
        'Security': 'rdp',
        'MaxConnections': 1,
        'AutomaticClientProfile': True,
    }
    for name, expected in expected_metadata.items():
        if metadata.get(name) != expected:
            raise RuntimeError(f'Production Info.plist {name}: expected {expected!r}')
    if metadata.get('FreeRDPShadowExperimentalArm'):
        raise RuntimeError('Production Info.plist must not select an experiment')
    for name, expected in expected_config.items():
        if config.get(name) != expected:
            raise RuntimeError(f'Production ShadowConfig.plist {name}: expected {expected!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-macos-shadow-production')
    parser.add_argument('--output', type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--experimental-arm', choices=EXPERIMENT_ARMS,
                           help='Package self-contained 0.2.0 pacing experiment settings')
    selection.add_argument('--release-candidate', action='store_true',
                           help='Package the corrected published-damage M policy as the next unused RC')
    parser.add_argument('--skip-version-smoke', action='store_true',
                        help='Do not execute the bundled CLI (for package-only experiments)')
    args = parser.parse_args()
    if args.release_candidate and args.output:
        parser.error('--release-candidate selects its own unused dist path')
    build = args.build_dir.resolve()
    candidate_number = None
    capture_provenance = args.release_candidate or args.experimental_arm in ('N', 'RateSweep')
    if args.release_candidate:
        candidate_number = 1
        while any((ROOT / 'dist' /
                   f'FreeRDP Shadow 0.2.0-rc{candidate_number}{suffix}').exists()
                  for suffix in ('.app', '.source.patch', '.sha256.txt')):
            candidate_number += 1
        destination = (ROOT / 'dist' /
                       f'FreeRDP Shadow 0.2.0-rc{candidate_number}.app').resolve()
    elif args.output:
        destination = args.output.resolve()
    elif args.experimental_arm:
        suffix = ('-RateSweep' if args.experimental_arm == 'RateSweep'
                  else args.experimental_arm)
        destination = (ROOT / 'dist' /
                       f'FreeRDP Shadow {EXPERIMENT_VERSION}{suffix}.app').resolve()
    else:
        destination = (ROOT / 'dist' / f'FreeRDP Shadow {PRODUCTION_VERSION}.app').resolve()
    if args.experimental_arm in ('N', 'RateSweep') and any(path.exists() for path in
                                            (destination, Path(str(destination.with_suffix('')) + '.source.patch'),
                                             Path(str(destination.with_suffix('')) + '.sha256.txt'))):
        raise RuntimeError(f'Preserving existing diagnostic candidate: {destination}')
    if capture_provenance:
        source_patch, untracked_sources = candidate_source_patch()
        source_revision = output('git', '-C', ROOT, 'rev-parse', 'HEAD').strip()
        source_branch = output('git', '-C', ROOT, 'branch', '--show-current').strip()
        source_status = output('git', '-C', ROOT, 'status', '--short')
        source_patch_hash = hashlib.sha256(source_patch).hexdigest()
    source = ROOT / 'packaging' / 'macos-shadow-menu'
    verify_signing_identity_available()
    cmake_args = ['cmake', '-S', ROOT, '-B', build, '-G', 'Ninja', '-C',
                  source / 'production-cache.cmake', '-DCMAKE_BUILD_TYPE=Release',
                  '-DBUILD_TESTING=OFF']
    cmake_args.append('-DWITH_JSONC_REQUIRED=ON')
    run(*cmake_args)
    verify_release(build)
    run('cmake', '--build', build, '--target', 'freerdp-shadow-cli', '-j', '6')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shadow-bundle-', dir=destination.parent) as temporary:
        app = Path(temporary) / destination.name
        contents = app / 'Contents'
        macos = contents / 'MacOS'
        frameworks = contents / 'Frameworks'
        resources = contents / 'Resources'
        for directory in (macos, frameworks, resources):
            directory.mkdir(parents=True)
        run('xcrun', 'clang', '-fobjc-arc', '-O3', '-DNDEBUG', '-DNS_BLOCK_ASSERTIONS=1', '-Wall', '-Wextra', '-Wpedantic',
            '-framework', 'AppKit', '-framework', 'ApplicationServices', '-framework', 'CoreGraphics',
            '-framework', 'ServiceManagement', '-o', macos / 'FreeRDPShadowMenu',
            source / 'FreeRDPShadowMenu.m')
        metadata = plistlib.loads((source / 'Info.plist').read_bytes())
        config = plistlib.loads((source / 'ShadowConfig.plist').read_bytes())
        if not args.experimental_arm and not args.release_candidate:
            verify_production_config(metadata, config)
        if args.experimental_arm or args.release_candidate:
            arm = f'RC{candidate_number}' if args.release_candidate else args.experimental_arm
            experiment = EXPERIMENT_ARMS['M' if args.release_candidate else arm].copy()
            if args.release_candidate:
                experiment['label'] = (f'0.2.0-rc{candidate_number} — Published-region '
                                       'quiet-rearmed burst / Fixed 150 KiB/s base')
            cap_kib = experiment['socket_kib']
            rate_kib = experiment['rate_kib']
            build_label = experiment['label']
            pacing_mode = ('FixedRuntime' if arm == 'RateSweep' else
                           'Fixed' if rate_kib else 'Adaptive')
            burst_bytes = (max(rate_kib * 1024 // 10, 64 * 64 * 4 + 30 + 64)
                           if rate_kib else 0)
            large_refresh_burst = experiment.get('large_refresh_burst', False)
            coverage_scheduler = experiment.get('coverage_scheduler', False)
            diagnostic_m = arm == 'M' or args.release_candidate
            burst_rate = 600 if diagnostic_m else 300
            burst_duration = 500 if diagnostic_m else 150
            burst_cap = 256 * 1024 if diagnostic_m else 48 * 1024
            metadata['CFBundleShortVersionString'] = EXPERIMENT_VERSION
            metadata['CFBundleVersion'] = EXPERIMENT_VERSION
            metadata['FreeRDPShadowBuildLabel'] = build_label
            metadata['FreeRDPShadowExperimentalArm'] = arm
            metadata['FreeRDPShadowPacingDiagnostics'] = True
            metadata['FreeRDPShadowSocketSendBufferKiB'] = cap_kib
            metadata['FreeRDPShadowGraphicsPacingMode'] = pacing_mode
            metadata['FreeRDPShadowGraphicsRateKiBPerSecond'] = rate_kib
            metadata['FreeRDPShadowGraphicsBurstBytes'] = burst_bytes
            metadata['FreeRDPShadowLargeRefreshBurst'] = large_refresh_burst
            metadata['FreeRDPShadowCoverageScheduler'] = coverage_scheduler
            if large_refresh_burst:
                metadata['FreeRDPShadowLargeRefreshBurstRateKiBPerSecond'] = burst_rate
                metadata['FreeRDPShadowLargeRefreshBurstDurationMs'] = burst_duration
                metadata['FreeRDPShadowLargeRefreshBurstByteCap'] = burst_cap
                metadata['FreeRDPShadowLargeRefreshBurstCooldownMs'] = 1000
                metadata['FreeRDPShadowLargeRefreshDamagePercent'] = 25
            config.update({
                'BuildLabel': build_label,
                'ExperimentalArm': arm,
                'PacingDiagnostics': True,
                'SocketSendBufferKiB': cap_kib,
                'GraphicsPacingMode': pacing_mode,
                'GraphicsRateKiBPerSecond': rate_kib,
                'GraphicsBurstBytes': burst_bytes,
                'LargeRefreshBurst': large_refresh_burst,
                'CoverageScheduler': coverage_scheduler,
                'ListenerAddress': '127.0.0.1',
                'ListenerPort': 3390,
                'Security': 'rdp',
                'MaxConnections': 1,
                'AutomaticClientProfile': True,
            })
            if large_refresh_burst:
                config.update({
                    'LargeRefreshBurstRateKiBPerSecond': burst_rate,
                    'LargeRefreshBurstDurationMs': burst_duration,
                    'LargeRefreshBurstByteCap': burst_cap,
                    'LargeRefreshBurstCooldownMs': 1000,
                    'LargeRefreshDamagePercent': 25,
                })
        with (contents / 'Info.plist').open('wb') as plist:
            plistlib.dump(metadata, plist, sort_keys=False)
        if capture_provenance:
            provenance = {
                'head': source_revision,
                'branch': source_branch,
                'dirtyStatus': source_status,
                'sourcePatchSHA256': source_patch_hash,
                'untrackedSourceFiles': untracked_sources,
                'buildDirectory': str(build),
                'configuration': 'Release; WITH_JSONC_REQUIRED=ON; BUILD_TESTING=OFF',
                'policy': ('RateSweep: one launch-selected fixed rate from '
                           '150, 200, 250, 300, 400 KiB/s; ordinary F bitmap traversal; '
                           'no burst; system-default socket buffer'
                           if args.experimental_arm == 'RateSweep' else
                           'N: coverage traversal for broad published bitmap damage; '
                           'fixed 153600 B/s; no burst; system-default socket buffer'
                           if args.experimental_arm == 'N' else
                           'published-region M: base 153600 B/s; burst 614400 B/s, '
                           '500 ms, 262144 admitted bytes, 1000 ms cooldown, 25% threshold, '
                           '2000 ms quiet rearm; local queue entry/stop guards; '
                           'system-default socket buffer'),
            }
            (resources / 'SourceProvenance.json').write_text(
                json.dumps(provenance, indent=2, sort_keys=True) + '\n')
        if metadata['CFBundleShortVersionString'] != metadata['CFBundleVersion']:
            raise RuntimeError('App version metadata does not match')
        print('Mac Shadow RDP — Build ' +
              metadata.get('FreeRDPShadowBuildLabel', metadata['CFBundleShortVersionString']))
        with (resources / 'ShadowConfig.plist').open('wb') as plist:
            plistlib.dump(config, plist, sort_keys=False)
        shutil.copy2(ROOT / 'LICENSE', resources / 'FreeRDP-LICENSE.txt')
        server = build / 'server/shadow/cli/freerdp-shadow-cli'
        main_rpaths = rpaths(server)
        copied = {}
        names = {}

        def resolve(dependency, origin):
            if dependency.startswith('@rpath/'):
                suffix = dependency[len('@rpath/'):]
                candidates = [path + '/' + suffix for path in rpaths(origin) + main_rpaths]
            else:
                candidates = [dependency]
            for candidate in candidates:
                candidate = candidate.replace('@loader_path', str(origin.parent))
                candidate = candidate.replace('@executable_path', str(server.parent))
                if Path(candidate).is_file():
                    return Path(candidate).resolve()
            raise RuntimeError(f'Cannot resolve {dependency} from {origin}')

        def bundle(origin, target):
            origin = origin.resolve()
            if origin in copied:
                return copied[origin]
            if target.name in names and names[target.name] != origin:
                raise RuntimeError(f'Duplicate library name: {target.name}')
            names[target.name] = origin
            copied[origin] = target
            shutil.copy2(origin, target)
            target.chmod(target.stat().st_mode | 0o200)
            for dep in dependencies(origin):
                if dep.startswith(('/System/Library/', '/usr/lib/')):
                    continue
                dependency = resolve(dep, origin)
                if dependency == origin:  # LC_ID_DYLIB is also printed by otool -L.
                    continue
                bundled = bundle(dependency, frameworks / dependency.name)
                run('install_name_tool', '-change', dep,
                    '@executable_path/../Frameworks/' + bundled.name, target)
            if target.suffix == '.dylib':
                run('install_name_tool', '-id', '@rpath/' + target.name, target)
            for path in set(rpaths(target)):
                run('install_name_tool', '-delete_rpath', path, target)
            run('strip', '-x', target)
            sign(target)
            return target

        bundle(server, macos / server.name)
        # Include dependency licensing files installed by the package manager.
        licenses = resources / 'ThirdPartyLicenses'
        for origin in copied:
            if str(origin).startswith(str(build)):
                continue
            prefix = origin.parent.parent
            for pattern in ('LICENSE*', 'COPYING*', 'NOTICE*', 'AUTHORS*'):
                for license_file in prefix.glob(pattern):
                    if license_file.is_file():
                        licenses.mkdir(exist_ok=True)
                        shutil.copy2(license_file, licenses / (prefix.name + '-' + license_file.name))
        run('plutil', '-lint', contents / 'Info.plist', resources / 'ShadowConfig.plist')
        sign(macos / 'FreeRDPShadowMenu', BUNDLE_ID)
        sign(app, BUNDLE_ID)
        run('codesign', '--verify', '--deep', '--strict', app)
        verify_certificate_signature(app)
        verify_certificate_signature(macos / 'FreeRDPShadowMenu')
        for binary in copied.values():
            verify_certificate_signature(binary)
        for binary in copied.values():
            for dep in dependencies(binary):
                if not dep.startswith(('/System/Library/', '/usr/lib/', '@executable_path/', '@rpath/')):
                    raise RuntimeError(f'Unbundled dependency: {binary}: {dep}')
        if not args.skip_version_smoke and not args.experimental_arm and not args.release_candidate:
            # Safe smoke check: command-line information exits without opening a listener.
            version = subprocess.run([str(macos / server.name), '/version'],
                                     text=True, capture_output=True)
            # FreeRDP returns COMMAND_LINE_STATUS_PRINT_VERSION (-2003) modulo 256.
            if version.returncode != (-2003 & 255) or 'FreeRDP version' not in version.stdout:
                raise RuntimeError(f'Bundled server smoke check failed: {version.stdout}{version.stderr}')
            print(version.stdout.strip())
        backup = Path(temporary) / 'previous.app'
        if args.experimental_arm in ('N', 'RateSweep') and destination.exists():
            raise RuntimeError(f'Preserving existing diagnostic candidate: {destination}')
        if destination.exists():
            destination.rename(backup)
        try:
            app.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise
    print(f'Built {destination}')
    if capture_provenance:
        stem = destination.with_suffix('')
        patch_path = Path(str(stem) + '.source.patch')
        hashes_path = Path(str(stem) + '.sha256.txt')
        patch_path.write_bytes(source_patch)
        hashed = [destination / 'Contents/Info.plist',
                  destination / 'Contents/Resources/ShadowConfig.plist',
                  destination / 'Contents/Resources/SourceProvenance.json',
                  destination / 'Contents/MacOS/FreeRDPShadowMenu',
                  destination / 'Contents/MacOS/freerdp-shadow-cli', patch_path]
        hashes_path.write_text(''.join(f'{sha256(path)}  {path}\n' for path in hashed))
        print(f'Provenance: {patch_path} (SHA-256 {source_patch_hash})')
        print(f'Hashes: {hashes_path}')


if __name__ == '__main__':
    main()
