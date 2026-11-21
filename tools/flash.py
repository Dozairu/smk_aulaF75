#!/usr/bin/env python3
"""Download and flash the AULA F75 Vial firmware. Python 3.9+, standard library only.

    python3 flash.py                 flash the latest release, settings reset to defaults
    python3 flash.py --keep          flash it but keep your keymap and settings
    python3 flash.py --full          the image with auto-type and radio diagnostics
    python3 flash.py --backup-only   save the stock firmware, flash nothing
    python3 flash.py --dry-run       download and check everything, touch no keyboard

What it does: fetches the release image from GitHub and checks it against the
release's SHA256SUMS, fetches the `sinowisp` flasher (a pinned version), and runs
it. Everything is kept under ~/.aula-f75-vial. Nothing is written to the keyboard
until you confirm.

Flashing replaces the keyboard's firmware. Read the README first, and back up the
stock firmware before the first flash (--backup-only).
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

REPO = 'Dozairu/smk_aulaF75'
DEVICE = 'aula-f75'

# The flasher version this script was written and tested against.
SINOWISP_VERSION = 'v2.1.0'
SINOWISP_TARGETS = {
    ('Linux', 'x86_64'): 'x86_64-unknown-linux-gnu',
    ('Darwin', 'x86_64'): 'x86_64-apple-darwin',
    ('Windows', 'AMD64'): 'x86_64-pc-windows-msvc',
}

# Saved-data range that --keep carries over a flash (see utils/flash_keep.py and
# meson.build: the dynamic keymap and settings sectors of the AULA F75 build).
KEEP_START, KEEP_END = 58368, 60928

HOME = Path.home() / '.aula-f75-vial'
USER_AGENT = 'aula-f75-vial-flash'


def say(msg=''):
    print(msg, flush=True)


def die(msg):
    sys.exit('error: ' + msg)


def fetch(url, to=None):
    """Returns the body as bytes, or saves it to `to`."""
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
    except (urllib.error.URLError, OSError) as e:
        die('could not download {}: {}'.format(url, e))
    if to is None:
        return data
    Path(to).parent.mkdir(parents=True, exist_ok=True)
    Path(to).write_bytes(data)
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def get_release(version):
    api = 'https://api.github.com/repos/{}/releases/'.format(REPO)
    api += 'latest' if version is None else 'tags/' + version
    return json.loads(fetch(api))


def get_sinowisp(workdir):
    key = (platform.system(), platform.machine())
    target = SINOWISP_TARGETS.get(key)
    if target is None:
        die('no ready-made sinowisp for {} {}. Build it from '
            'https://github.com/karstx/sinowisp (see the README) and put it on PATH, '
            'then use --sinowisp /path/to/sinowisp.'.format(*key))
    exe = 'sinowisp.exe' if platform.system() == 'Windows' else 'sinowisp'
    dest = workdir / 'sinowisp-{}'.format(SINOWISP_VERSION) / exe
    if dest.exists():
        return dest
    url = 'https://github.com/karstx/sinowisp/releases/download/{0}/sinowisp-{1}-{0}.tar.gz'.format(
        SINOWISP_VERSION, target)
    say('Downloading sinowisp {} ...'.format(SINOWISP_VERSION))
    archive = workdir / 'sinowisp.tar.gz'
    fetch(url, archive)
    # Pull out only the one binary, by name: never trust paths inside an archive.
    with tarfile.open(archive) as tar:
        member = next((m for m in tar.getmembers()
                       if m.isfile() and Path(m.name).name == exe), None)
        if member is None:
            die('{} not found in the sinowisp archive'.format(exe))
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(tar.extractfile(member).read())
    dest.chmod(dest.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    archive.unlink()
    return dest


def get_firmware(args, workdir):
    """Returns (hex_path, tag). Verified against the release's SHA256SUMS."""
    if args.hex:
        path = Path(args.hex)
        if not path.is_file():
            die('{} not found'.format(path))
        say('Using local image {} (not checked against any release).'.format(path))
        return path, None

    rel = get_release(args.version)
    tag = rel['tag_name']
    suffix = '-full.hex' if args.full else '.hex'
    name = 'aula-f75-vial-{}{}'.format(tag, suffix)
    assets = {a['name']: a['browser_download_url'] for a in rel['assets']}
    if name not in assets or 'SHA256SUMS' not in assets:
        die('release {} has no {} / SHA256SUMS'.format(tag, name))

    reldir = workdir / tag
    say('Release {}: downloading {} ...'.format(tag, name))
    fetch(assets[name], reldir / name)
    sums = fetch(assets['SHA256SUMS']).decode()
    expected = None
    for line in sums.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip('*').removeprefix('./') == name:
            expected = parts[0].lower()
    if expected is None:
        die('{} is not listed in SHA256SUMS'.format(name))
    actual = sha256(reldir / name)
    if actual != expected:
        (reldir / name).unlink()
        die('checksum mismatch for {} (expected {}, got {}); the download was deleted'.format(
            name, expected, actual))
    say('Checksum OK ({}).'.format(actual[:16] + '...'))
    return reldir / name, tag


def parse_ihex(path):
    """Returns {address: byte} for the data records of an Intel HEX file."""
    mem = {}
    base = 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(':'):
                die('{}:{}: not an Intel HEX record'.format(path, n))
            rec = bytes.fromhex(line[1:])
            if sum(rec) & 0xFF:
                die('{}:{}: bad checksum'.format(path, n))
            count, addr, rtype = rec[0], (rec[1] << 8) | rec[2], rec[3]
            data = rec[4:4 + count]
            if rtype == 0x00:
                for i, b in enumerate(data):
                    mem[base + addr + i] = b
            elif rtype == 0x01:
                break
            elif rtype == 0x02:
                base = ((data[0] << 8) | data[1]) << 4
            elif rtype == 0x04:
                base = ((data[0] << 8) | data[1]) << 16
    return mem


def write_ihex(path, mem):
    """Writes {address: byte} as 16-byte data records (addresses < 64 KiB)."""
    addrs = sorted(mem)
    lines = []
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = [mem[start]]
        i += 1
        while i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < 16 \
                and (start + len(chunk)) & 0xFFFF:
            chunk.append(mem[addrs[i]])
            i += 1
        rec = bytes([len(chunk), (start >> 8) & 0xFF, start & 0xFF, 0x00]) + bytes(chunk)
        lines.append(':' + (rec + bytes([(-sum(rec)) & 0xFF])).hex().upper())
    lines.append(':00000001FF')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def flash_keep(sinowisp, hex_path, dry):
    """Read current flash over ISP and carry [KEEP_START, KEEP_END) into hex_path."""
    image = parse_ihex(hex_path)
    overlap = [a for a in image if KEEP_START <= a < KEEP_END]
    if overlap:
        die('the new firmware has code at 0x{:04X}, inside the kept range '
            '0x{:04X}-0x{:04X}; refusing'.format(min(overlap), KEEP_START, KEEP_END - 1))

    with tempfile.TemporaryDirectory(prefix='smk-flash-keep-') as tmp:
        dump = Path(tmp) / 'current.bin'
        merged = Path(tmp) / 'merged.hex'
        say('Reading the current flash to keep 0x{:04X}-0x{:04X}'.format(KEEP_START, KEEP_END - 1))
        rc = run([sinowisp, 'read', '-d', DEVICE, '-s', 'firmware', '--format', 'bin', dump], dry)
        if dry:
            run([sinowisp, 'write', '-d', DEVICE, '--format', 'ihex', '--reboot', 'true', '--force', merged], dry)
            return 0
        if rc != 0 or not dump.exists():
            say('error: read failed -- nothing was written')
            return rc or 1
        current = dump.read_bytes()
        if len(current) < KEEP_END:
            say('error: read returned {} bytes, need {} -- nothing was written'.format(len(current), KEEP_END))
            return 1

        kept = current[KEEP_START:KEEP_END]
        if all(b == 0xFF for b in kept):
            say('Nothing saved on the keyboard (area erased); flashing as-is')
        else:
            for i, b in enumerate(kept):
                image[KEEP_START + i] = b
        write_ihex(merged, image)

        say('Writing new firmware + kept data')
        rc = run([sinowisp, 'write', '-d', DEVICE, '--format', 'ihex', '--reboot', 'true', '--force', merged], dry)
        if rc != 0:
            say('error: write failed (exit {}). The keyboard stays in ISP mode; '
                'flash again to recover'.format(rc))
        return rc


def confirm(question, yes):
    if yes:
        return True
    try:
        return input(question + ' [y/N] ').strip().lower() in ('y', 'yes')
    except EOFError:
        return False


def run(cmd, dry):
    say('$ ' + ' '.join(str(c) for c in cmd))
    if dry:
        say('  (dry run: not executed)')
        return 0
    return subprocess.run([str(c) for c in cmd]).returncode


def permission_hint():
    say()
    say('If that failed with "Permission denied":')
    if platform.system() == 'Linux':
        say('  install utils/70-aula-f75-vial.rules (see the README, "Linux permissions"),')
        say('  replug the keyboard, or run this script with sudo.')
    else:
        say('  try running the command from an administrator terminal.')
    say('Also check: cable plugged in, mode switch on wired.')


def main():
    if sys.version_info < (3, 9):
        die('Python 3.9 or newer is needed')

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', action='store_true',
                    help='keep keymap, macros and settings across the flash')
    ap.add_argument('--full', action='store_true',
                    help='the -full image (auto-type and radio diagnostics on)')
    ap.add_argument('--version', metavar='TAG', help='a release tag such as v0.1.0 (default: latest)')
    ap.add_argument('--hex', metavar='FILE', help='flash this local .hex instead of a release')
    ap.add_argument('--sinowisp', metavar='PATH', help='use this sinowisp instead of downloading one')
    ap.add_argument('--backup-only', action='store_true',
                    help='save the keyboard\'s current firmware to ./f75_backup and exit')
    ap.add_argument('--dry-run', action='store_true', help='download and check, but run nothing on the keyboard')
    ap.add_argument('--yes', '-y', action='store_true', help='skip the confirmation questions')
    args = ap.parse_args()

    HOME.mkdir(parents=True, exist_ok=True)
    sinowisp = Path(args.sinowisp) if args.sinowisp else get_sinowisp(HOME)

    if args.backup_only:
        out = Path('f75_backup')
        say('Reading the keyboard\'s firmware into {}/ ...'.format(out))
        say('(Only a backup of the STOCK firmware is a way back. If the keyboard already runs '
            'this firmware, the copy is of this firmware.)')
        if not args.dry_run:
            out.mkdir(exist_ok=True)
        rc = 0
        for section in ('full', 'firmware'):
            rc |= run([sinowisp, 'read', '-d', DEVICE, '-s', section, '--format', 'bin',
                       out / 'f75_{}.bin'.format(section)], args.dry_run)
        if rc:
            permission_hint()
            return 1
        if not args.dry_run:
            sums = ''.join('{}  {}\n'.format(sha256(out / n), n)
                           for n in ('f75_full.bin', 'f75_firmware.bin'))
            (out / 'SHA256SUMS').write_text(sums)
            say('Saved. Copy {}/ somewhere safe; it is your only way back.'.format(out))
        return 0

    hex_path, _tag = get_firmware(args, HOME)

    say()
    say('About to flash {} to the AULA F75 ({}).'.format(hex_path.name, 'settings kept' if args.keep
                                                          else 'settings reset to defaults'))
    say('Cable in, mode switch on wired. This replaces the keyboard\'s firmware.')
    if not confirm('Did you back up the stock firmware (or is this keyboard already running '
                   'this firmware)?', args.yes):
        say('Run  python3 flash.py --backup-only  first, on a keyboard with the stock firmware.')
        return 1
    if not confirm('Flash now?', args.yes):
        say('Cancelled; nothing was written.')
        return 1

    if args.keep:
        rc = flash_keep(sinowisp, hex_path, args.dry_run)
    else:
        rc = run([sinowisp, 'write', '-d', DEVICE, '--reboot', 'true', '--force', hex_path], args.dry_run)
    if rc:
        permission_hint()
        return rc
    if not args.dry_run:
        say()
        say('Done. Open https://vial.rocks (Chromium-based browser) or the Vial app; '
            'the keyboard shows up as "AULA F75".')
    return 0


if __name__ == '__main__':
    sys.exit(main())
