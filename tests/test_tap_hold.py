"""The report path for keys the key engine decides on RELEASE, end to end in
the simulator: matrix scan -> matrix_task -> action engine -> send_keycode ->
EP1.

A tap of a mod-tap key is pressed AND released at the moment the key goes up,
inside one matrix_task() batch. Batching once merged the two into a single
report that no longer contained the key, so the host never saw the tap
(tapped Esc on LCTL_T(KC_ESC) did nothing). matrix.c flush_pending_press()
fixed that; this test keeps it fixed.

The NuPhy Air60's default keymap has no mod-tap key, so the test patches one
entry of `keymaps` (CODE) after boot: layer 0, row 2, col 1 (KC_A) becomes
MT(MOD_LSFT, KC_A). Keycodes are little-endian 16-bit words.
"""

import unittest

from devices import Air60Sim

MATRIX_COLS = 16          # src/keyboards/nuphy-air60/kbdef.h
MT_LSFT_A = 0x2204        # QK_MOD_TAP | MOD_LSFT << 8 | KC_A
KC_A = 0x04
LSFT = 0x02


class TestModTapTap(unittest.TestCase):
    def _patch_mod_tap(self, kb, col, row):
        addr = kb._a("keymaps") + (row * MATRIX_COLS + col) * 2
        kb.cmd("set mem rom 0x%x 0x%02x 0x%02x" % (addr, MT_LSFT_A & 0xFF, MT_LSFT_A >> 8))

    def test_tap_reaches_host_as_press_then_release(self):
        kb = Air60Sim()
        try:
            kb.boot(usb=True)
            kb.mark_usb_configured()
            self._patch_mod_tap(kb, 1, 2)

            kb.matrix.press(1, 2)
            # Undecided while held (well under the 200 ms tapping term): nothing
            # may go out yet -- not A, not Shift.
            before = len(kb.scan_for_report(max_hits=100))
            kb.matrix.release(1, 2)
            kb.scan_for_report(max_hits=400)          # release debounce, then the tap
            reps = kb.scan_for_report(max_hits=200)   # and its release
        finally:
            kb.close()

        new = reps[before:]
        self.assertTrue(any(r[2] == KC_A and r[0] == 0 for r in new),
                        "the tap must reach the host as its own report carrying KC_A "
                        "without Shift; got %s" % new)
        self.assertTrue(new and new[-1][0] == 0 and not any(new[-1][2:]),
                        "and end with everything released; got %s" % new)
        self.assertFalse(any(r[0] & LSFT for r in new),
                         "a tap must never press Shift; got %s" % new)


if __name__ == "__main__":
    unittest.main()
