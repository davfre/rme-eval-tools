#!/usr/bin/env python3
"""Read-only overview of the local, installed and running driver."""
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MODULE = 'snd-usb-babyface-pro'


def read(*args):
    try:
        result = subprocess.run([str(a) for a in args], text=True, capture_output=True)
    except FileNotFoundError:
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def main():
    repo = ROOT / 'babyface-pro-linux'
    branch = read('git', '-C', repo, 'branch', '--show-current') or '(detached or unavailable)'
    commit = read('git', '-C', repo, 'rev-parse', '--short', 'HEAD') or 'unknown'
    print(f'Driver checkout: {branch} ({commit})')
    changes = read('git', '-C', repo, 'status', '--porcelain')
    if changes:
        print('Checkout changes:')
        for line in changes.splitlines():
            print('  ' + line)
    kernel = platform.release()
    print(f'Running kernel: {kernel}')
    local = repo / 'tools/kernel' / (MODULE + '.ko')
    built = read('modinfo', '-F', 'srcversion', local) if local.exists() else None
    vermagic = read('modinfo', '-F', 'vermagic', local) if built else None
    installed = read('modinfo', '-k', kernel, '-F', 'srcversion', MODULE)
    installed_path = read('modinfo', '-k', kernel, '-n', MODULE)
    loaded_file = Path('/sys/module/snd_usb_babyface_pro/srcversion')
    loaded = loaded_file.read_text().strip() if loaded_file.exists() else None
    print(f'\nBuilt locally: {built or "not built"}')
    if vermagic:
        print(f'  For kernel: {vermagic.split()[0]}')
    print(f'Installed for running kernel: {installed or "not found"}')
    if installed_path:
        print(f'  File: {installed_path}')
    print(f'Loaded now: {loaded or "not loaded"}')
    for label, left, right in [('Built vs installed', built, installed),
                               ('Installed vs loaded', installed, loaded),
                               ('Built vs loaded', built, loaded)]:
        if left and right:
            print(f'  {label}: {"match" if left == right else "DIFFERENT"}')
    if vermagic and vermagic.split()[0] != kernel:
        print('  Local build targets a different kernel.')
    if loaded and installed and loaded != installed:
        print('  The installed driver has not replaced the running module.')
    print('\nDKMS registrations:')
    dkms = read('dkms', 'status')
    if dkms is None:
        print('  DKMS unavailable or status query failed')
    else:
        entries = [line for line in dkms.splitlines() if line.startswith(MODULE + '/')]
        print('\n'.join('  ' + line for line in entries) if entries else '  None for Babyface')
    print('\nAudio services:')
    for service in ('pipewire.service', 'pipewire-pulse.service', 'wireplumber.service'):
        state = read('systemctl', '--user', 'show', service, '--property=ActiveState', '--property=SubState')
        values = dict(line.split('=', 1) for line in (state or '').splitlines() if '=' in line)
        print(f'  {service}: {values.get("ActiveState", "unavailable")} ({values.get("SubState", "unknown")})')


if __name__ == '__main__':
    main()
