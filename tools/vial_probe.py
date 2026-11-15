#!/usr/bin/env python3
"""Talk the Vial protocol to an SMK keyboard over hidraw, read-only.

    python3 tools/vial_probe.py [path/to/vial.json]

Finds the raw HID interface (usage page 0xFF60) by its report descriptor,
then runs the handshake Vial GUI does and checks each answer:

  - VIA protocol version is 9
  - Vial keyboard id: protocol 6 + 8-byte UID (+ VialRGB: info, effect list, mode)
  - definition: size, every 32-byte page, LZMA-decompresses to JSON, and
    (if a vial.json is given) equals it
  - encoder 0 on layer 0, unlock status, QMK settings listed + readable,
    8 tap dances, 8 combos, 4 key overrides readable

Nothing is written to the keyboard. Needs read/write access to the hidraw node
(udev rule or ACL, as for VIA in the browser).
"""

import glob
import json
import lzma
import os
import select
import struct
import sys

REPORT = 32


def find_raw_hid():
    for node in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
        try:
            with open(os.path.join(node, 'device/report_descriptor'), 'rb') as f:
                desc = f.read(4)
        except OSError:
            continue
        if desc[:3] == b'\x06\x60\xff':
            return '/dev/' + os.path.basename(node)
    sys.exit('no raw HID interface (usage page 0xFF60) found -- is the keyboard plugged in by USB?')


class Dev:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        # Drop anything a previous session left queued.
        while select.select([self.fd], [], [], 0.05)[0]:
            os.read(self.fd, 64)

    def xfer(self, *payload):
        pkt = bytes(payload).ljust(REPORT, b'\x00')
        os.write(self.fd, b'\x00' + pkt)
        if not select.select([self.fd], [], [], 1.0)[0]:
            raise TimeoutError('no reply to ' + pkt[:4].hex(' '))
        return os.read(self.fd, 64)[:REPORT]


def check(ok, what):
    print(('  ok   ' if ok else '  FAIL ') + what)
    return ok


def main():
    path = find_raw_hid()
    print('raw HID:', path)
    d = Dev(path)
    good = True

    r = d.xfer(0x01)
    via = (r[1] << 8) | r[2]
    good &= check(via == 9, 'VIA protocol version = {} (want 9)'.format(via))

    r = d.xfer(0xFE, 0x00)
    proto, uid = struct.unpack('<IQ', r[:12])
    vialrgb = r[12] == 1
    good &= check(proto == 6, 'Vial protocol = {} (want 6), uid = {:016x}, vialrgb = {}'.format(proto, uid, r[12]))

    r = d.xfer(0xFE, 0x01)
    size = struct.unpack('<I', r[:4])[0]
    good &= check(proto == 6 and 0 < size < 16384, 'definition size = {} bytes'.format(size))

    blob = b''
    page = 0
    while proto == 6 and len(blob) < size:
        blob += d.xfer(0xFE, 0x02, *struct.pack('<I', page))
        page += 1
    blob = blob[:size]
    try:
        definition = json.loads(lzma.decompress(blob))
        good &= check(True, 'definition decompresses: {} pages, "{}", matrix {}x{}'.format(
            page, definition.get('name'), definition['matrix']['rows'], definition['matrix']['cols']))
        if len(sys.argv) > 1:
            with open(sys.argv[1], encoding='utf-8') as f:
                good &= check(json.load(f) == definition, 'definition equals ' + sys.argv[1])
    except Exception as e:  # noqa: BLE001 -- report and carry on
        good &= check(False, 'definition decompresses ({})'.format(e))

    r = d.xfer(0xFE, 0x03, 0, 0)
    ccw, cw = struct.unpack('>HH', r[:4])
    good &= check(r[0] != 0xFF or r[1] != 0x03, 'encoder 0, layer 0: ccw=0x{:04X} cw=0x{:04X}'.format(ccw, cw))

    r = d.xfer(0xFE, 0x05)
    keys = [(r[i], r[i + 1]) for i in range(2, 30, 2) if r[i] != 0xFF]
    good &= check(r[0] in (0, 1) and r[1] in (0, 1) and len(keys) > 0, 'unlock status: unlocked={} in_progress={} keys={}'.format(r[0], r[1], keys))

    names = {1: "grave escape overrides", 2: "combo term", 5: "one-shot tap toggle",
             6: "one-shot timeout", 20: "tapping toggle", 21: "magic", 7: "tapping term", 22: "permissive hold",
             23: "hold on other key press", 24: "retro tapping", 25: "quick tap term"}
    widths = {2: 2, 6: 2, 7: 2, 21: 4, 25: 2}
    supported, cur = [], 0
    while cur != 0xFFFF:
        r = d.xfer(0xFE, 0x09, *struct.pack("<H", cur))
        ids = [int.from_bytes(r[i:i + 2], "little") for i in range(0, 32, 2)]
        supported += [q for q in ids if q != 0xFFFF]
        cur = max(ids)
    good &= check(sorted(supported) == sorted(names), "QMK settings listed: {}".format(supported))
    for q in supported:
        r = d.xfer(0xFE, 0x0A, *struct.pack("<H", q))
        v = int.from_bytes(r[1:1 + widths.get(q, 1)], "little")
        good &= check(r[0] == 0, "  {} (qsid {}) = {}".format(names.get(q, "?"), q, v))

    r = d.xfer(0xFE, 0x0D, 0x00)
    r_entries = r
    n_td, n_combo = r[0], r[1]
    good &= check(n_td == 8 and n_combo == 8 and r[2] == 4 and r[3] == 0 and r[31] == 3,
                  'dynamic entries: TD/combo/KO/alt-repeat = {}, features (caps word, layer lock) = {}'.format(list(r[:4]), r[31]))
    for i in range(n_td):
        t = d.xfer(0xFE, 0x0D, 0x01, i)
        tap, hold, dbl, taphold, term = struct.unpack('<5H', bytes(t[1:11]))
        good &= check(t[0] == 0, '  TD({}): tap {:04X} hold {:04X} double {:04X} tap+hold {:04X} term {}'.format(
            i, tap, hold, dbl, taphold, term))
    t = d.xfer(0xFE, 0x0D, 0x01, n_td)
    good &= check(t[0] == 0xFF, '  TD({}) past the end refused'.format(n_td))
    for i in range(n_combo):
        t = d.xfer(0xFE, 0x0D, 0x03, i)
        *keys, output = struct.unpack('<5H', bytes(t[1:11]))
        good &= check(t[0] == 0, '  combo {}: {} -> {:04X}'.format(
            i, ' + '.join('{:04X}'.format(k) for k in keys if k) or '-', output))
    t = d.xfer(0xFE, 0x0D, 0x03, n_combo)
    good &= check(t[0] == 0xFF, '  combo {} past the end refused'.format(n_combo))
    n_ko = r_entries[2]
    for i in range(n_ko):
        t = d.xfer(0xFE, 0x0D, 0x05, i)
        trig, repl, layers = struct.unpack('<3H', bytes(t[1:7]))
        tmods, neg, sup, opt = t[7], t[8], t[9], t[10]
        good &= check(t[0] == 0, '  key override {}: {:04X} -> {:04X} layers {:04X} mods {:02X} neg {:02X} '
                      'suppress {:02X} options {:02X}{}'.format(i, trig, repl, layers, tmods, neg, sup, opt,
                                                              '' if opt & 0x80 else ' (off)'))
    t = d.xfer(0xFE, 0x0D, 0x05, n_ko)
    good &= check(t[0] == 0xFF, '  key override {} past the end refused'.format(n_ko))

    if vialrgb:
        r = d.xfer(0x08, 0x40)
        good &= check(r[2] | r[3] << 8 == 1, 'VialRGB protocol = {}, max brightness = {}'.format(r[2] | r[3] << 8, r[4]))
        ids, cur = [], 0
        while True:
            r = d.xfer(0x08, 0x42, *struct.pack('<H', cur))
            page = [v for v in struct.unpack('<15H', bytes(r[2:32])) if v != 0xFFFF]
            if not page:
                break
            ids += page
            cur = page[-1]
        # Off (0) is never listed (vial-qmk pages ids > 0; the GUI adds Off itself).
        good &= check(len(ids) == 28 and ids == sorted(ids) and 0 not in ids,
                      'VialRGB effects ({}): {}'.format(len(ids), ids))
        r = d.xfer(0x08, 0x41)
        mode, speed, h, s, v = r[2] | r[3] << 8, r[4], r[5], r[6], r[7]
        good &= check(mode == 0 or mode in ids, 'VialRGB mode = {}, speed {}, hsv ({}, {}, {})'.format(mode, speed, h, s, v))

    r = d.xfer(0x11)
    good &= check(r[1] == 4, 'layer count = {}'.format(r[1]))

    print('\nall good' if good else '\nSOMETHING FAILED')
    sys.exit(0 if good else 1)


if __name__ == '__main__':
    main()
