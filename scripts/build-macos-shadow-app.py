#!/usr/bin/env python3
"""Build and bundle the production menu app without installing or launching it."""
import argparse
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


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def output(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build-macos-shadow-production')
    parser.add_argument('--output', type=Path, default=ROOT / 'dist' / 'FreeRDP Shadow.app')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    destination = args.output.resolve()
    source = ROOT / 'packaging' / 'macos-shadow-menu'
    verify_signing_identity_available()
    run('cmake', '-S', ROOT, '-B', build, '-G', 'Ninja', '-C', source / 'production-cache.cmake',
        '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF')
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
        shutil.copy2(source / 'Info.plist', contents / 'Info.plist')
        metadata = plistlib.loads((contents / 'Info.plist').read_bytes())
        if metadata['CFBundleShortVersionString'] != metadata['CFBundleVersion']:
            raise RuntimeError('App version metadata does not match')
        print('Mac Shadow RDP — Build ' + metadata['CFBundleShortVersionString'])
        shutil.copy2(source / 'ShadowConfig.plist', resources / 'ShadowConfig.plist')
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
        # Safe smoke check: command-line information exits without opening a listener.
        version = subprocess.run([str(macos / server.name), '/version'],
                                 text=True, capture_output=True)
        # FreeRDP returns COMMAND_LINE_STATUS_PRINT_VERSION (-2003) modulo 256.
        if version.returncode != (-2003 & 255) or 'FreeRDP version' not in version.stdout:
            raise RuntimeError(f'Bundled server smoke check failed: {version.stdout}{version.stderr}')
        print(version.stdout.strip())
        backup = Path(temporary) / 'previous.app'
        if destination.exists():
            destination.rename(backup)
        try:
            app.rename(destination)
        except OSError:
            if backup.exists():
                backup.rename(destination)
            raise
    print(f'Built {destination}')


if __name__ == '__main__':
    main()
