#!/usr/bin/env python3
"""Flash new firmware over USB but keep the keyboard's saved data.

    flash_keep.py --sinowisp PATH --device NAME --keep START END firmware.hex

A plain `sinowisp write` rewrites the whole application area, which includes
the flash sectors where SMK keeps the VIA/Vial keymap, macros, encoder map,
Vial QMK settings and lighting settings -- so every flash starts from the
defaults. This does the same write, but first reads the current flash off the
keyboard and carries the sectors in [START, END) over into the new image.

Safe by construction:
  - the read happens first; if it fails, nothing is written;
  - it refuses to run if the new firmware itself has code in [START, END);
  - the firmware validates what it finds there (magic, version, sector
    count, checksum). Data from an older, incompatible layout is rejected at
    boot and replaced by defaults, exactly like a clean flash.

The range excludes the last sector before the bootloader (the boot marker and
the reset trampoline), so the ISP/JTAG layout difference sinowisp handles is
never involved.
"""

import argparse
import os
import subprocess
import sys
import tempfile


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
                sys.exit('{}:{}: not an Intel HEX record'.format(path, n))
            rec = bytes.fromhex(line[1:])
            if sum(rec) & 0xFF:
                sys.exit('{}:{}: bad checksum'.format(path, n))
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


def run(cmd):
    print('+', ' '.join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sinowisp', required=True)
    ap.add_argument('--device', required=True)
    ap.add_argument('--keep', nargs=2, required=True, metavar=('START', 'END'),
                    type=lambda s: int(s, 0), help='byte range to carry over, END exclusive')
    ap.add_argument('hex')
    args = ap.parse_args()

    start, end = args.keep
    if not 0 <= start < end <= 0x10000:
        sys.exit('flash_keep: bad keep range')

    image = parse_ihex(args.hex)
    overlap = [a for a in image if start <= a < end]
    if overlap:
        sys.exit('flash_keep: the new firmware has code at 0x{:04X}, inside the kept range '
                 '0x{:04X}-0x{:04X}; refusing'.format(min(overlap), start, end - 1))

    with tempfile.TemporaryDirectory(prefix='smk-flash-keep-') as tmp:
        dump = os.path.join(tmp, 'current.bin')
        merged = os.path.join(tmp, 'merged.hex')

        print('flash_keep: reading the current flash to keep 0x{:04X}-0x{:04X}'.format(start, end - 1))
        if run([args.sinowisp, 'read', '-d', args.device, '-s', 'firmware', '--format', 'bin', dump]) != 0 \
                or not os.path.exists(dump):
            sys.exit('flash_keep: read failed -- nothing was written')
        with open(dump, 'rb') as f:
            current = f.read()
        if len(current) < end:
            sys.exit('flash_keep: read returned {} bytes, need {} -- nothing was written'.format(len(current), end))

        kept = current[start:end]
        if all(b == 0xFF for b in kept):
            print('flash_keep: nothing saved on the keyboard (area erased); flashing as-is')
        else:
            for i, b in enumerate(kept):
                image[start + i] = b
        write_ihex(merged, image)

        print('flash_keep: writing new firmware + kept data')
        rc = run([args.sinowisp, 'write', '-d', args.device, '--format', 'ihex',
                  '--reboot', 'true', '--force', merged])
        if rc != 0:
            sys.exit('flash_keep: write failed (exit {}). The keyboard stays in ISP mode; '
                     'flash again (plain or keep) to recover'.format(rc))
    print('flash_keep: done')


if __name__ == '__main__':
    main()
