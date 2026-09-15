#!/usr/bin/env python3
"""Install a committed driver snapshot through DKMS; never reload hardware."""
import argparse
import fcntl
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

MODULE = 'snd-usb-babyface-pro'
ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / 'babyface-pro-linux'
OPTIONS = Path('/etc/modprobe.d/snd-usb-babyface-pro-local.conf')
OPTION_TEXT = f'options {MODULE} frames_per_urb=32 nurbs=8\n'
POLICY = Path('/etc/dkms') / f'{MODULE}.conf'
KERNEL_PATTERN = r'^[0-9].*-cachyos$'
POLICY_TEXT = '# Local Babyface policy: regular CachyOS only, never LTS.\nBUILD_EXCLUSIVE_KERNEL="' + KERNEL_PATTERN + '"\n'


def target_kernel():
    kernel = platform.release()
    if not re.fullmatch(KERNEL_PATTERN, kernel):
        raise RuntimeError(f'This installer is restricted to the regular CachyOS kernel, not {kernel}. Boot the regular kernel first.')
    return kernel



def run(*args, capture=False):
    result = subprocess.run([str(a) for a in args], check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ''


def parse_status(text):
    records = []
    for line in text.splitlines():
        if not line.startswith(MODULE + '/'):
            continue
        identity, sep, status = line.partition(': ')
        fields = identity.split(', ')
        if not sep or status not in {'added', 'built', 'installed'} or len(fields) not in (1, 3):
            raise RuntimeError(f'Unrecognized DKMS status; resolve it first: {line}')
        records.append((fields[0].split('/', 1)[1], fields[1] if len(fields) == 3 else None, status))
    return records


def dkms(*args):
    run('sudo', 'dkms', *args)


def snapshot(commit, version):
    result = {}
    listing = run('git', '-C', REPO, 'ls-tree', '-r', commit, '--', 'tools/kernel', capture=True)
    for line in listing.splitlines():
        metadata, path = line.split('\t', 1)
        mode, kind, blob = metadata.split()
        if kind != 'blob' or mode not in ('100644', '100755'):
            raise RuntimeError(f'Unsupported source entry: {path}')
        relative = Path(path).relative_to('tools/kernel')
        data = subprocess.check_output(['git', '-C', str(REPO), 'show', f'{commit}:{path}'])
        if relative == Path('dkms.conf'):
            data, count = re.subn(rb'^PACKAGE_VERSION=.*$', f'PACKAGE_VERSION="{version}"'.encode(), data, flags=re.M)
            if count != 1:
                raise RuntimeError('Expected one PACKAGE_VERSION in dkms.conf')
        result[relative] = data
    if Path('dkms.conf') not in result or Path('Makefile') not in result:
        raise RuntimeError('The selected commit does not contain DKMS build files')
    return result


def check_source(source, files):
    if source.exists():
        for relative, expected in files.items():
            path = source / relative
            if path.is_symlink() or not path.is_file() or path.read_bytes() != expected:
                raise RuntimeError(f'Existing source differs from the selected commit: {path}. It was not overwritten.')


def verify(version, kernel):
    installed = run('modinfo', '-k', kernel, '-F', 'srcversion', MODULE, capture=True)
    built_dir = Path('/var/lib/dkms') / MODULE / version / kernel / platform.machine() / 'module'
    built = list(built_dir.glob(MODULE + '.ko*'))
    if len(built) != 1:
        raise RuntimeError(f'Cannot identify the built module in {built_dir}')
    expected = run('modinfo', '-F', 'srcversion', built[0], capture=True)
    if not installed or installed != expected:
        raise RuntimeError(f'Installed module does not match the selected build for {kernel}')
    print(f'Verified {kernel}: {installed}')


def install_kernels(version, kernels, records, verify_fn=verify):
    # Finish every build before disturbing any installed version.
    for kernel in kernels:
        if not any(v == version and k == kernel and s in ('built', 'installed') for v, k, s in records):
            dkms('build', '-m', MODULE, '-v', version, '-k', kernel)
    touched = []
    try:
        for kernel in kernels:
            if (version, kernel, 'installed') in records:
                verify_fn(version, kernel)
                continue
            previous = [v for v, k, s in records if k == kernel and s == 'installed' and v != version]
            touched.append((kernel, previous))
            for old in previous:
                dkms('uninstall', '-m', MODULE, '-v', old, '-k', kernel)
            dkms('install', '-m', MODULE, '-v', version, '-k', kernel)
            verify_fn(version, kernel)
    except (Exception, KeyboardInterrupt):
        print('Installation failed. Attempting to restore the previous installed versions.')
        for kernel, previous in reversed(touched):
            try:
                current = parse_status(run('dkms', 'status', capture=True))
                if (version, kernel, 'installed') in current:
                    dkms('uninstall', '-m', MODULE, '-v', version, '-k', kernel)
                for old in previous:
                    dkms('install', '-m', MODULE, '-v', old, '-k', kernel)
            except Exception as error:
                print(f'ROLLBACK FAILED for {kernel}: {error}. Check dkms status before rebooting.')
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', action='store_true', help='show the proposed steps without changing anything')
    args = parser.parse_args()
    for tool in ('git', 'dkms', 'modinfo', 'sudo'):
        if not shutil.which(tool):
            raise RuntimeError(f'{tool} is missing. Install DKMS and matching kernel headers first.')
    # Serialize this helper's operations without relying on the caller's shell.
    lock_dir = Path.home() / '.cache/rme-driver-install'
    lock_dir.mkdir(parents=True, exist_ok=True)
    with (lock_dir / 'lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('Another driver installer is running')
        execute(args)


def execute(args):
    if run('git', '-C', REPO, 'status', '--porcelain', '--', 'tools/kernel', capture=True):
        raise RuntimeError('Commit or set aside changes in tools/kernel before installing a snapshot.')
    commit = run('git', '-C', REPO, 'rev-parse', 'HEAD', capture=True)
    version = '0.1.0.local.' + commit[:12]
    branch = run('git', '-C', REPO, 'branch', '--show-current', capture=True) or '(detached HEAD)'
    kernels = [target_kernel()]
    missing = [k for k in kernels if not (Path('/usr/lib/modules') / k / 'build/Makefile').is_file()]
    if missing:
        raise RuntimeError('Install matching headers before continuing: ' + ', '.join(missing))
    records = parse_status(run('dkms', 'status', capture=True))
    old_versions = sorted({v for v, _, _ in records if v != version})
    # Never remove a registration that covers another kernel, including LTS.
    extra = [k for v, k, s in records if v != version and k and k not in kernels]
    if extra:
        raise RuntimeError('Older DKMS registrations reference kernels outside this plan: ' + ', '.join(extra))
    if POLICY.exists() and POLICY.read_text() != POLICY_TEXT:
        raise RuntimeError(f'Existing DKMS policy needs review: {POLICY}; it was not overwritten.')
    source = Path('/usr/src') / f'{MODULE}-{version}'
    files = snapshot(commit, version)
    check_source(source, files)
    registered = any(v == version for v, _, _ in records)
    if registered and not source.exists():
        raise RuntimeError(f'DKMS is registered but its source is missing: {source}')
    print(f'Branch: {branch}\nCommit: {commit}\nDKMS version: {version}\nRunning kernel: {platform.release()}')
    print('\nProposed steps:')
    print('  Source: ' + ('reuse verified snapshot' if source.exists() else f'copy committed source to {source}'))
    print('  Registration: ' + ('already present' if registered else 'add to DKMS'))
    for kernel in kernels:
        status = next((s for v, k, s in records if v == version and k == kernel), 'not built')
        action = {'installed': 'verify only', 'built': 'install and verify'}.get(status, 'build, install and verify')
        print(f'  {kernel}: {status}; {action}')
    for old in old_versions:
        print(f'  Replace {old} only after all new builds succeed; remove its registration after verification.')
    options_match = OPTIONS.exists() and OPTIONS.read_text() == OPTION_TEXT
    print('  Boot options: ' + ('32/8 already configured' if options_match else f'write 32/8 to {OPTIONS}'))
    print('\nSource snapshots are retained for rollback. This does not reload the driver or restart audio.')
    print('Only the running kernel is targeted. LTS is excluded from automatic DKMS builds too.')
    print('  DKMS policy: ' + str(POLICY))
    if args.plan:
        return
    if input('\nProceed? Type yes: ').strip() != 'yes':
        print('Cancelled. No installation changes made.')
        return
    run('sudo', '-v')
    with tempfile.TemporaryDirectory(prefix='babyface-dkms-') as staging:
        stage = Path(staging)
        if not source.exists():
            for relative, data in files.items():
                target = stage / 'source' / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            run('sudo', 'cp', '-aT', stage / 'source', source)
        check_source(source, files)
        if not registered:
            dkms('add', '-m', MODULE, '-v', version)
        policy = stage / 'dkms-policy.conf'
        policy.write_text(POLICY_TEXT)
        run('sudo', 'install', '-Dm644', policy, POLICY)
        install_kernels(version, kernels, records)
        # Do not leave competing versions registered for future autoinstall.
        for old in old_versions:
            dkms('remove', '-m', MODULE, '-v', old, '--all')
        if not options_match:
            if OPTIONS.exists():
                backup = OPTIONS.with_name(OPTIONS.name + '.before-' + version)
                if not backup.exists():
                    run('sudo', 'cp', '-a', OPTIONS, backup)
            config = stage / 'options.conf'
            config.write_text(OPTION_TEXT)
            run('sudo', 'install', '-Dm644', config, OPTIONS)
        for kernel in kernels:
            run('sudo', 'depmod', '-a', kernel)
    print('\nInstalled and verified for: ' + ', '.join(kernels))
    run('dkms', 'status')
    loaded = Path('/sys/module/snd_usb_babyface_pro/srcversion')
    if loaded.exists():
        print('Loaded source version: ' + loaded.read_text().strip())
        if platform.release() in kernels:
            print('Installed for running kernel: ' + run('modinfo', '-F', 'srcversion', MODULE, capture=True))
    else:
        print('The driver is not currently loaded.')
    print('Reboot with speakers off to test the installed driver. Check output levels before listening.')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError, EOFError) as error:
        print(f'\nStopped: {error}')
        raise SystemExit(1)
    except KeyboardInterrupt:
        print('\nInterrupted. Inspect dkms status before rebooting if installation had started.')
        raise SystemExit(130)
