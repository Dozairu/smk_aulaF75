# Vial on SMK — roadmap

What is left to do, with the numbers to decide by. Current state and how everything works:
[vial.md](vial.md).

## Budget (AULA F75, Vial build, `-Dautotype=false -Drf_diag=false`)

| | Used | Free |
|---|---|---|
| Code | 56,074 / 58,368 B | **2,294 B** |
| XRAM | 3,792 / 4,096 B | 304 B |
| Saved-data store | 1,784 / 2,016 B (4 × 512 B sectors at `0xE400`) | 232 B |

Code sizes below are estimates from SDCC's per-function listings of comparable code.

## Features that fit

| Feature | Code | Resets the Vial setup once? | Notes |
|---|---|---|---|
| **Auto Shift** | ~0.85 KB | no (settings bytes 14–15, 17 are free) | QSIDs 3 / 4 in QMK Settings, `KC_AS*` keycodes. A version with host tests was written and removed for space before `588dc41`; it was never committed, so it needs rewriting. |
| **Key override: late activations** | ~0.2 KB | no | "Activate when a required mod is pressed" / "when a negative mod is released" while the trigger is held; mods-only triggers. |
| **Vial's Alt Repeat table** | ~1.3–1.5 KB | yes (new store area, 8 × 6 B) | Brings Repeat / Alt Repeat back into Vial's picker (Vial shows them only when the keyboard has Alt Repeat entries). |
| **5th layer** | little | yes | 192 B XRAM of the 304 left, 192 B of store. |

Combinations: Auto Shift + key override extras (~1.05 KB) or Alt Repeat table + key override extras
(~1.6 KB) fit. Auto Shift + Alt Repeat table (~2.25 KB) needs more room, see below.

## More room

- **Store from 4 sectors to 3**: shrink the VIA macro buffer from 768 B to 480 B (still 16 macros).
  Frees **512 B of code** and 288 B of XRAM. Storage version bump (one reset). Not compatible with
  a 5th layer (the store would no longer fit 3 sectors).
- Already done: auto-type and the radio overlay as build options (`bee3cc1`), the VIA lighting menu
  dropped with VialRGB (`8acef1c`), flat keymap reset and table-driven Magic (`f6e9fb2`), Tier 1
  dead-code & ISR XRAM pruning (`-1,609 B` code, `-18 B` XRAM across `led_effect.c`, `report.c`,
  `action.c`, `user_matrix.c`, `flash.c`, `aula_fx.c`, `matrix.c`), and sharing SignalRGB's
  `direct_rgb` buffer for 80-key VialRGB direct mode.
- Not worth it: compiler flags (`--opt-code-size`: no gain on any module), compiling the engine
  without `--stack-auto` (~200 B for an ABI risk), rewriting the flash save / load (it is the
  power-loss protection).

## Bugs and hardening

- **Stale raw HID reply**: a request whose reply is never read leaves the next tool one reply
  behind. Needs an on-device experiment: can firmware withdraw a queued EP2 IN packet on the
  SH68F90 (clear `IEP2RDY`)? Test build + script; today the workaround is replugging.
- **Simulator test failing upstream**: `test_usb_ep0 ... test_led_report_with_wrong_length_stalls`
  fails on the commit this work started from too (not caused by Vial).
- **Wireless pacing numbers**: count ACK timeouts (e.g. on the Fn+R overlay in a `-Drf_diag=true`
  build) to confirm reports never wait the full 8 ms.

## Maybe

- **Latency measurement** in the cycle-accurate simulator (key down to EP1 report), to back the
  "well under 1 ms" engine estimate with a number.
- **Faster matrix scan** (today a full scan every 3 ms, presses taken on first sight): coupled to the
  LED PWM timing, so ~1 ms average gain for a risky rework. Not planned.
- **Upstream**: offer the Vial layer to the AULA F75 port and/or SMK once it has settled.
