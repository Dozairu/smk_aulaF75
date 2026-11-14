# Epomaker × AULA F75 (classic model)

> ## ✅ 2026-09-11 — WIRELESS WORKS, and the root cause wasn't in the protocol
>
> **Keystrokes confirmed over the 2.4 GHz dongle, Bluetooth pairing confirmed** -- both transports,
> after the first radio bring-up on the device. What was blocking wasn't the pinout, the baud rate, or the frame format: **EUART0 was dropping
> eight bytes on every matrix scan.**

## Wireless bring-up — what the hardware revealed

Initial symptom: 2.4 GHz **and** Bluetooth fail identically, backlight intact,
keystrokes lost. With both transports failing together, the fault lay in the shared path.

### Root cause: an interrupt priority

`tick.c` runs the **full matrix scan** and the LED subframes **inside the Timer2 ISR**.
A scan takes ~320 µs. At 260,870 baud, a byte arrives every **38 µs**, and the SH68F90 has
no buffer behind `SBUF`. At equal priority, the serial ISR does not preempt: **every matrix scan
costs eight bytes.**

The proof is in the raw stream, latched inside the ISR with zero interpretation:

```
02 00 00 52 │ 03 00 52 │ 02 06 00 00 00 02 dd 03 │ 02 00 01 00 │ 02 00 01 00 00 52
03 00 00 00 00 52 │ 02 00 01 00 │ 02 00 52 │ 02 00 01 │ 02 00
```

`02 00 01 00 00 52` and `03 00 00 00 00 52` have **valid** checksums (`0x55 − Σ`). Everything else
— `02 00 00 52`, `02 00 52`, `02 00 01 00` — is merely a **subsequence** of them. The module was transmitting
properly; we were dropping bytes.

**The factory firmware sets the exact bit that was missing, and this page had already noted it** without
drawing the conclusion: `0xECC5` writes `IPH1 = 0x42` and `IPL1 = 0x41`, bit 6 in both,
putting EUART0 at **priority level 3**. SMK did not configure any interrupt priorities.

```c
IPH1 |= _ES0;
IPL1 |= _ES0;
```

After the fix, on the same stream: **zero rejected frames, zero link drops.**

### Received frame format — measured, no longer inferred

The length depends on **byte 1**, which echoes the acknowledged command, not solely on the frame type.

| Frame | Length | Meaning |
| --- | --- | --- |
| `02 06 00 00 00 02 dd 03 01 6a` | **10** | response to status probe `0x06` |
| `02 01 00 00 00 52` | **6** | link-select acknowledgment |
| `02 02 00 00 00 51` | **6** | key report acknowledgment |
| `03 00 00 00 00 52` | **6** | announcement |

Factory checksum `0x55 − Σ` verified on each one. On the 2.4 GHz status response, `[4] = 0` and `[5] = 2` (in Bluetooth mode, stock `0x265D–0x26E9` uses `[5] = 3` for connected and `1`/`2`/`0` for pairing/reconnecting/disconnected) — and `[6:7] = dd 03` gives a
battery reading of **989**, above the "full" threshold of 912.

The port was expecting **ten** bytes for every type `0x02` frame: on a six-byte acknowledgment, it
swallowed the start of the next frame and never resynchronized.

### Three structural flaws exposed by the same measurement

1. **Link-select was held hostage by the Bluetooth names.** Command `0x01` is the only one that
   switches the module, and it sat behind two 32-byte transmissions in an `if / else if` chain.
   A single failure froze `name_stage`. The factory firmware sends its names from `main`, **unguarded**.
2. **`link_tx_pending` was cleared before transmission**, whose return value was discarded: a rejected command
   was lost forever.
3. **`rf_rx_drop()` discarded the entire buffer** after each frame. At 260 kbaud against a main loop
   running in milliseconds, two frames routinely arrive back-to-back: the second one was
   lost. And on a stray byte, flushing everything prevented any resynchronization.

`P4.7` was also measured **high**: the "module ready" line never blocked anything. The guard
remains on reports, but control commands bypass it after `RF_CTRL_STALL` attempts — without this
fallback, a stuck-low line would make the fault impossible to diagnose.

### The tooling that made this possible

**The SMK HID console** (`src/smk/console.c`, vendor page `0xFF31`, usage `0x74`, report ID 7):
it existed, but had never been used on this keyboard. PC reader in `tools/smk_console.py`.

⚠️ **An upstream SMK bug made it unusable on Windows.** The console collection only
declared its `INPUT`, whereas host attachment goes through `SET_REPORT(FEATURE, id 7)`.
Linux passes any `SET_REPORT` through hidraw so the bug goes unnoticed there; **Windows
validates against the descriptor and rejects it** — measured: `-1` at every length, across both vendor
collections. Adding a `FEATURE` item in `src/smk/usb.c` got the console streaming.

**The `Fn + R` LED overlay** paints the driver state onto keys `1` through `0`. It is the only
instrument usable on battery with USB unplugged — that is, in the exact setup that was failing.

**Deferred capture**: the firmware records the first 64 received bytes and replays them when the
console becomes available again. Without it, hand timing became the dominant variable and
two captures were lost.




## Caps Lock — the indicator that didn't exist

`indicators_update_step()` received `keyboard_state_t *keyboard` and discarded it with a
`(void)keyboard; /* status indicators are not ported yet */`. So it wasn't a
bug: the function had simply never been written for this board.

The state comes from the host HID LED report, stored by `usb_ep0_out_irq()` (`src/smk/usb.c`) into
`keyboard_state.led_state` — bit 0 `Num Lock`, **bit 1 `Caps Lock`**, bit 2 `Scroll Lock`.

### The indicator isn't a key LED, and isn't even an LED: it's a CHANNEL

Established **on the device**, by scanning matrix positions and then probing channel by channel — not by
inference, and contrary to two written claims in our own sources.

Position `[row 0][col 14]` — electrically the encoder press, top right — carries a
full RGB position whose **three channels drive separate status indicators physically located
elsewhere on the board**:

| Channel | Indicator |
| --- | --- |
| **red** | **`Caps Lock`**, to the left of the `Caps Lock` key |
| green | the indicator between `Esc` and `F1` |
| blue | the same indicator between `Esc` and `F1` |

Status indicators are single-color LEDs: the factory uses a spare RGB slot as
**three independent drivers**. Writing white to this position lights up both indicators at once —
which is exactly what happened on the first try, as reported by the user.

**Two claims on this page and in `layout.c` were wrong**:

| What was written | Measurement |
| --- | --- |
| "the encoder press has no LED" | the **position** has one, with three useful channels; it's the **key** that has nothing under its keycap |
| "this keyboard has no dedicated indicator" | it has **at least two** |

And this explains OpenRGB's count: its `aula_f75_layout` table only maps
**eighty** of the ninety LEDs to a key. The remaining **ten** — indices 6, 23, 29,
41, 47, 65, 71, 75, 76, 84 — are not grid holes; they are real LEDs without a keycap
above them. That simple recount would have been enough to find the indicator; instead, the search went through a
dump scan (inconclusive: a byte scan without instruction alignment confuses `53 92 FD`, an
`ANL`, with a `MOV P7.5,C`) and then a test of five spare pins, both unnecessary.

Rendering consequence: this cell must be painted **explicitly, channel by channel**. Otherwise,
lighting effects make both indicators blink at random — which they had been doing since the very first flash.

### The `Esc`/`F1` indicator: a white LED, hence rhythm and nothing else

Measured: the **green** and **blue** channels light up the **same white LED** and their intensities
**add up**. There is neither color nor a second indicator to exploit — the three "levels" visible during
probing are simply three points on a continuous scale. Patterns can therefore only be
distinguished by their **rhythm**, hence one, two, or three flashes for the three transports: counting
flashes can be read at a glance, a difference in duration cannot.

`aula_status.c` holds the pattern player. Nine patterns in `__code`, 197 bytes, in a **single
flat array** with offset and length tables: an array of `__code` pointers would cost
more flash and an extra indirection on every step.

**One step = one wrap-around of the regeneration scan**, ~37.9 ms. Same constraint as the auto-type
heartbeat, and for the same reason: the ninety cells are repainted one per
subframe, so changing the value anywhere other than at wrap-around would land mid-repaint cycle.

⚠️ The looping pattern mask is `0x0108` — **nine patterns, hence nine bits**. It does not fit
in a byte; the first draft declared it as `uint8_t` and silently lost the
low-battery warning.

### The sleep pattern required three lines in SMK core

`sleep_task()` (`src/smk/sleep.c`) shuts down the tick and PWM **before** calling `user_sleep_prepare()`
: nothing can be displayed from that point on, so a sleep fade-out cannot play there.

The sleep interface gains two hooks, implemented across all six keyboards:

| Hook | Role |
| --- | --- |
| `bool user_sleep_ready(void)` | returning `false` delays sleep by one loop pass, keeping the display engine alive |
| `void user_sleep_cancel(void)` | called on every pass where sleep is **not** due |

`user_sleep_cancel()` is not cosmetic: without it, a sleep sequence interrupted by a keystroke
mid-fade would leave the board's state machine armed, and **the next sleep entry
would proceed without a pattern**. The other five keyboards return `true` and do nothing.

### Charging and wireless host LEDs (decoded & implemented)

**Charging (`P3.4`, `P7.6`, `P7.7` — `0x970E–0x97AA`)**. Disassembly of the stock firmware establishes the exact roles:
- `P3.4` (or wired mode) is **VBUS** (active HIGH, debounced 3 samples).
- `P7.6` is an **output** (charger enable, active LOW: `0` when VBUS is present, `1` on battery).
- `P7.7` is an **input** (charger status, active LOW: `20` samples low = charging, `200` samples high or VBUS absent = charge complete / not charging).
- `rf_charge_poll()` in `aula_rf.c` implements this, caps `rf_batt_pct` at `99%` while charging (snapping to `100%` at `>= 95%` when charge-complete), and plays a smooth breath (`AULA_STATUS_LINKED`) on the `Esc`/`F1` LED while charging.

**Wireless host LEDs (`frame[3]` — `0x06B7`, `0x0716`, `0x248C`)**. In 10-byte EUART0 status frames, **`frame[3]`** carries the host HID LED bitmask (`0x01` Num Lock, `0x02` Caps Lock, `0x04` Scroll Lock). `rf_status_apply()` copies `rx_buf[3] & 0x07` into `keyboard_state.led_state` when `link_connected` is set (and zeroes it when disconnected).

**Caps Lock indicator reliability & latency fixes (wired + wireless):**
- **Wired USB `EP0` `SET_REPORT` (`src/sino51lib/sh68f90/usbhw.c`, `src/smk/usb.c`):** `_INT_USB` (`_EUSB`) is raised to **Priority 1** (above `Timer2` at 0, matching stock `0xECC5`) so `Timer2` ticks cannot delay `SETUP` + `DATA OUT` stages, and `usb_ep0_out_irq()` defers `SET_EP0_OUT_RDY` until after `GET_EP0_OUT_CNT()` and `EP0_OUT_BUF` have been read.
- **Wireless `EUART0` status polling (`src/keyboards/aula-f75/aula_rf.c`):** Because the BK3632 reports `rx_buf[3]` only in response to `0x06` status probes (not unsolicited), `rf_rx_frame_len()` waits for `rx_idx >= 2` before inspecting `rx_buf[1]`, `rf_probe_task()` retries immediately on `tx_busy`, `rf_tick()` (called from the `400 µs` sub-tick in `indicators_post_update()`) paces steady-state `0x06` polling at **`100 ms`** (`RF_PROBE_TICKS = 250`) with a **`1.0 s`** (`RF_PROBE_MISSES = 10`) disconnect tolerance, and Lock-key report transitions (`KC_CAPS_LOCK`, `KC_SCROLL_LOCK`, `KC_NUM_LOCK`) arm a non-strike **`+15..75 ms`** (`5 × ~15 ms`) fast follow-up poll so Caps Lock toggles appear immediately without adding polling traffic during regular typing.
- **Immediate LED rendering (`indicators.c`):** `indicators_render()` updates `(CAPS_ROW, CAPS_COL)` directly on every pass so `duty_cache[14]` reflects Caps Lock changes immediately even when RGB effects are `Off`.

**BT/2.4G link state (`frame[5]` — `0x265D–0x26E9`)**. Stock treats `frame[5] == 3` as connected (`0x0E3B = 1`), `1` as pairing, `2` as reconnecting, and `0` as disconnected. `aula_rf.c` now checks `flag == 3` in BT mode and drives per-key `1`/`2`/`3` pairing/reconnect flashing in `indicators.c`.

All three indicators (**Status (`Esc/F1`)**, **Caps Lock**, and **Charging & Low Battery**) are toggleable in VIA (`via.json`, channel `3`, IDs `6–8`).

**Order in the rendering chain** (`led_regen_one()`): keyless positions, **status indicators**,
radio diagnostics, auto-type heartbeat, lighting effects. Indicators come first because they are not
keys: nothing else must ever write to this cell.




## Auto-type — design, and the two traps it uncovered

⚠️ **No factory equivalent.** `Fn + Space` or `Fn + encoder press` starts a mode that types a
random letter, erases it with Backspace, and repeats after a random delay between
0.5 and 1.5 s. Any key stops it, and that key types nothing. The entire keyboard pulses
in a heartbeat red while active. See `aula_macro.c`.

**The heartbeat indicator, and why its tempo isn't free.** Two beats per cycle — the first
strong, the second 227 ms later and weaker — followed by a long rest at a dim glow: 909 ms per
heartbeat, or 66 beats per minute. The twenty-four-entry intensity table lives in
`indicators.c`, in `__code`.

The table step advances on the **wrap-around of the regeneration scan**, not at a freely chosen
rate. The ninety cells are repainted **one per subframe**, never all at
once: an unaligned cadence would drop the value change right in the middle of a repaint
cycle and the panel would show a seam, half on the previous beat and half on the next
beat — all the more visible because the attack is sharp (thirty steps apart between two entries). By
advancing on the wrap-around, the entire panel always carries the same value. Ninety
subframes of ~421 µs make ~37.9 ms, and twenty-four steps land on a believable heart rate.

The values are true intensities: `aula_rgb_duty()` is linear (`value << 2`).

The point of recording it here isn't the feature itself: it is **two properties of SMK's core**
that it exposed, which will trap the next feature of this kind.

**1. `kb_process_record()` does not see `MO()` keys.** `process_key_state()`
(`src/smk/matrix.c`) handles the `IS_QK_MOMENTARY` branch **before** calling the hook, and returns
early. A hook that claims to see "all keys" therefore never sees `Fn`. Worse, releasing
`Fn` calls `clear_keys()`, which **clears the keyboard report without sending it** — desynchronizing
any state that believes it is holding a key down.

Consequence for this mode: the activation sequence `Fn↓ Space↓ Space↑ Fn↑` would have cleared the report
in the middle of the first letter. Hence a wait state that only starts the state machine once the
matrix is **completely released**.

**2. `user_matrix_pressed()` is the raw state, and it sees everything.** Declared in `kbdef.h`, fed by
`user_matrix_read_rows()`, already masked to the six real rows (`| 0xc0` before complement, otherwise
bits 6 and 7 of the complement would be spurious). That is what catches `Fn`, invisible to the hook.
**No modification to SMK core was therefore needed** — no need to export `matrix[]` or
`action_layer`, which are not in `matrix.h`.

**The exit erase, and why it cannot be synchronous.** Upon cancellation, a letter
is *on screen* if it has been typed without yet being erased — substates `STEP_KEY_UP` and
`STEP_BSP_DOWN`, and only those. The mode must then send the Backspace it didn't have time
to send, to leave the text intact.

This Backspace **cannot be sent all at once upon cancellation**: the host only polls the endpoint
every millisecond (`bInterval = 1`), and a press immediately followed by its release would be
overwritten in the endpoint buffer without ever being seen. This is the exact same trap that `MACRO_HOLD` documents
for normal keystrokes. Hence a dedicated `MACRO_FLUSH` state: a pause to let the ongoing
release go out, then the press, the hold, the release — about seven milliseconds, followed by
waiting for full matrix release.

**The missing safeguard.** A state that swallows all keys until full release locks up
permanently if a key gets stuck: the keyboard would type nothing until a
reboot. Five seconds maximum wait, then return to idle no matter what — and return to
idle rather than starting, otherwise a stuck key would loop cancellation and restart endlessly.

**Timebase.** There is no free millisecond counter. The LED subframe (~420 µs
once matrix scans are amortized) is the only steady cadence, the one already used by
`aula_encoder.c`. One second equals ~2,381 subframes: a 16-bit counter is mandatory, and the tick
sets a bit rather than letting the loop read the counter — a bit test is atomic on 8051,
a 16-bit read is not.

**Randomness.** Xorshift16 seeded on activation from live `TH2`/`TL2`. The moment of activation is
chosen by a human: it is the MCU's only entropy source, and it suffices for a non-cryptographic
draw.

**What the mode does not guarantee.** The letter is canceled by Backspace, so text remains
unchanged — **unless the foreground application treats `Backspace` as something other than deleting
a character** (browser set to "navigate back", incremental search, game hotkey).
This is the only feature on this keyboard that sends keystrokes to the host without anyone touching
a key.

**And it prevents sleep.** `sleep_note_activity()` is called on every step, otherwise the mode stops
on its own after about **2 min 15 s** on battery. In wireless mode, it therefore drains the battery for as long as it
runs. This is a deliberate choice, not an oversight.

⚠️ **The sleep timeout is easy to miscalculate.** `SLEEP_TIMEOUT` is 21,000 *frames*, and
the frame counted by `sleep.c` is the one returned by `indicators_update_step()`: it wraps every
**fifteen** passes (`led_col` cycles through `LED_COLS`), i.e., one full panel multiplex
cycle — **not** the ninety subframes of an animation frame. One frame is therefore
15 × 400 µs + one matrix scan (~320 µs) ≈ **6.3 ms**, and the timeout 21,000 × 6.3 ms ≈ **133 s**.
Confusing the two frames gives thirteen minutes instead of two — a mistake that was made and fixed
right here.




## The short frame — the Report ID was one byte too many

**Symptom:** in 2.4 GHz and Bluetooth modes, the encoder wheel and media keys did nothing,
even though typing worked. Yet both go through the exact same queue and transmitter.

This page previously listed the payload layout as **inferred** — "placing it at
offset 0 is the most natural choice, not a reading". It has now been read.

### The frame, read from the disassembler

`euart0_send` is at **`0xAB0F`**, length in `r5`, tag in `r7`. Five callers, and
their lengths identify each one:

| Call site | `r5` | Command |
| --- | --- | --- |
| `0x46C2` | 30 | `0x02` — long report |
| `0x4730` | 13 | `0x03` — short report |
| `0xA376` | 32 | `0x09` — Bluetooth name |
| `0xB0B8` | 23 | `0x08` — container |
| `0xED53` | 6 | short commands |

Both report transmitters do **the exact same thing**, up to length:

```asm
mov r0, #0x33 ; mov @r0, #0x01      ; frame[0] = header 0x01
src = 0x0C57 + idx * 0x1C           ; queue slot, 28 bytes
inc r0                              ; destination = IDATA 0x34
memcpy(dst, src, 0x1C or 0x0B)      ; 28 bytes (long) or 11 (short)
euart0_send(0x1E or 0x0D, 6)
```

Byte 0 of the slot IS the command, copied verbatim into `frame[1]`. **Our
`rf_queue_flush()` was already building this frame byte for byte.** The bug was elsewhere.

### The producer, and the skipped byte

The short-frame producer is at **`0x45AB`**:

```asm
slot[0] = 0x03
slot[1] = [0x09BD]                  ; Consumer usage, low byte
slot[2] = [0x09BE]                  ; Consumer usage, high byte
slot[3] = [0x097E]                  ; System byte
slot[4..10] = memcpy(0x09B1, 7)     ; mouse report
```

The factory Consumer buffer is `0x09BC`, and **`0x09BC` holds Report ID 2** — this page
had already established that through two independent paths. The radio producer starts at `0x09BD`: it
**skips the Report ID**. The frame's command byte acts as the selector instead.

The long-report producer confirms the rule (`0x451A`): `slot[1] = [0x08B2]`, the modifier
mask, followed by five keys from `0x08B4`. No Report ID there either.

**Our code was sending `report->raw`**, i.e., `[Report ID, usage_lo, usage_hi]`. Everything was
shifted by one byte and the dongle read `2` as the low byte of the usage. The keyboard report, by contrast,
never had a Report ID in 6KRO — hence typing worked while volume did not.

> **Methodological lesson.** Both paths were symmetrical in our code and static reasoning
> could not tell them apart: both the keyboard report *and* the Consumer report started "in the most
> natural way". Only the asymmetry in the dump — one starts at `0x08B2`, the other at `0x09BD` and not
> `0x09BC` — gave the answer. An inference explicitly marked as such ended up costing a bug; at least it
> was found right where the page said to look.

### What remains inferred

The **position** of the System byte (`slot[3]`) is established, but no System keycode is wired
in this layout: nothing has exercised it. The seven mouse bytes remain zero — this keyboard
has no mouse.


## `AULA_FX_LAKE` — the wave radiating from the struck key

⚠️ **This effect is NOT a transcription.** Nothing like it exists in the factory firmware:
its concentric wave `0x746A` radiates from a **fixed** center tabulated at `CODE 0x2959`, and its
reactive engine `0x64F1` lights up the struck key **without propagating it**. This effect combines both, and is
an original creation — requested as "a wave of light, as if tapping on a flat lake".

### The mechanism

Four simultaneous **drops**, sixteen bytes of XDATA. Each remembers its origin, its age in
frames, and its starting hue:

```c
static __xdata uint8_t drop_col[4], drop_row[4], drop_age[4], drop_hue[4];
```

- **on keystroke** — `led_react_poll()` calls `lake_drop()`: uses a free slot, otherwise the oldest
  drop is replaced. The impact point lights up immediately, age starts at 1.
- **on every frame** — age advances by one step; the drop is freed at 25, once the wave has left the
  keyboard.
- **on every cell** — the cell is relit to full intensity if its **distance** to an origin equals
  that drop's exact age. That forms the wavefront.

The **intensity plane decay**, already in place, creates the wake behind the front. Nothing to
add: the trail fades on its own.

### Two choices that make the visual difference

**Distance weights rows TWICE as much as columns.** The generated geometry places columns every
17 units and rows every 42: one row step equals roughly two and a half column steps.
Without weighting, the wave would be a squashed diamond instead of a circle.

```c
return (uint8_t)(dc + (uint8_t)(dr << 1));
```

**The wake decays three times faster than elsewhere — 24 instead of 8.** At 8 per frame, the wake
lasts 32 frames whereas the wave takes 25 frames to cross the keyboard: the wake would catch up with the
front and you would see an expanding lit disk, not a ripple. At 24, the wake is about ten frames wide.

The hue also shifts as it travels outward, `drop_hue + age × 3`: over the 24 steps of travel, one third of
the color wheel. Two rapid keystrokes start from different hues since each samples `led_phase` at
the moment of impact.

### The cost, and why it fits inside the ISR

Checking the wavefront is done **one cell at a time**, inside `led_regen_one()` — four
subtractions and four comparisons per subframe. No full scan of all ninety cells
at once, which would have weighed heavily in a 400 µs slot.

Total cost: **+550 bytes of flash, +16 bytes of XDATA**, internal RAM margin and stack
unchanged.

### It is the default effect

`indicators_apply_defaults()` selects `AULA_FX_LAKE`, and validation also falls back to it when a
saved setting is out of bounds.

## The factory Fn layer, decoded — and what it confirms

`CODE 0xCC00` is the **base layer**, `CODE 0xD000` is the **Fn layer**: four bytes per key,
indexed as `col × 6 + row`. The proof fits in a single position: index 44 (column 7, row 2, the
**U** key) carries usage `0x18` = U in the first, and `0x46` = **Print Screen** in the second.

A user of the keyboard provided the lead — "Fn + U used to take a screenshot". The dump
proves them right to the exact byte.

### What the Fn layer independently validates

Records with `b0 = 0x07` belong to the **shortcut dispatcher** `0x412C`, where `b3` is
the lookup key. Their contents match **to the letter** the `0x4136` table decoded separately:

| Position | `b3` | Key in table `0x4136` | Previously established role |
| --- | --- | --- | --- |
| Fn + `Esc` | `0x04` | `0x41B3` | **factory reset** (long press) |
| Fn + `1` | `0x05` | `0x41C8` | Bluetooth slot 1 |
| Fn + `2` | `0x06` | `0x41FE` | Bluetooth slot 2 |
| Fn + `3` | `0x07` | `0x4233` | Bluetooth slot 3 |
| Fn + `` ` `` | `0x08` | `0x426A` | **re-enter pairing** |

Five keys, five arms, in the exact same order. The shortcut table had been read from the
inline dispatcher; here it is confirmed from the **layout data**, a completely unrelated
source.

Similarly, `b0 = 0x08` dispatches to the class dispatcher `0xA674` on `b1`, and the positions match:
`b1 = 3` with `b2` set to 1 or 2 on Fn + `Up`/`Down` and Fn + `F5`/`F6`, `b1 = 4` with `b2` set to 1 or 2 on
Fn + `Right`/`Left`. Two "+ / −" pairs on two distinct classes, the exact shape
expected for brightness and speed. And Fn + `F4` carries `b1 = 8`, the class whose dispatcher
executes `lcall 0x91D3` — **restoring default colors**.

### Correction: `b0 = 0x0D` is the Fn key itself

This page previously attributed "two held state bits" to `0x0D`, noting that bits `0x44` and `0x43`
are the ones tested by the arm of effect `0x2D`. The origin is now clear: **position c8 r5
carries `b0 = 0x0D` in BOTH layers**, and c8 r5 is the **Fn** key.

`0x0D` is therefore **layer hold**, and bits `0x44`/`0x43` are the "Fn is held" state.
Effect `0x2D` does not react to some obscure flag: it reacts to the user holding Fn.

### Complete inventory of the factory Fn layer

| Key | Fn + … |
| --- | --- |
| `Esc` | shortcut `0x04` — **factory reset**, long press |
| `` ` `` | shortcut `0x08` — **re-enter pairing** |
| `1` `2` `3` | shortcuts `0x05` `0x06` `0x07` — Bluetooth slots |
| `F1` | consumer — browser, home |
| `F2` | consumer — mail |
| `F3` | class 4, usage `0x2B` (Tab) |
| `F4` | class 8 — **restore default colors** (`0x91D3`) |
| `F5` `F6` | class 3, parameters 2 and 1 |
| `F7` `F8` `F9` | consumer — previous track, play/pause, next track |
| `F10` | consumer — mute |
| `F11` `F12` | consumer — volume − and + |
| `Tab` | class 2 |
| `Q` `W` `E` | shortcuts `0x19` `0x18` `0x1A` — OS modes (Android, Windows, Mac) |
| `G` `B` | shortcuts `0x0A` `0x11` — `0x11` = battery bar / charging animation (`0x42B1`) |
| **`U`** | **Print Screen** |
| **`I`** | **Scroll Lock** |
| **`O`** | **Pause** |
| `\` | class 0 via `b0 = 0x08` |
| `Del` | Insert |
| `End` | Home |
| `Up` `Down` | class 3, parameters 1 and 2 |
| `Left` `Right` | class 4, parameters 2 and 1 |

### What the port adopted from it

**Three keys added and flashed**: `Fn + U` → Print Screen, `Fn + I` → Scroll Lock,
`Fn + O` → Pause. They were missing, and they match the factory layout.

**Additional stock shortcuts decoded & added (VIA-configurable)**:
`FACTORY_RST` (`Fn + Esc`, hold ~2s), `RF_PAIR` (`Fn + ~`), `BAT_SHOW` (`Fn + B`), `WIN_LOCK` (`Fn + LGui`), `WASD_SWAP` (`Fn + T`), `OS_WIN` (`Fn + W`), `OS_ANDROID` (`Fn + Q`), `OS_MAC` (`Fn + E`), and `OS_IOS` (assignable in VIA).

**What remains unimplemented**: stock per-key flash color recording/restoration (`Fn + F4`, `0x91D3`) and a native OpenRGB controller (host-driven per-key RGB is now handled via SignalRGB over Raw HID below).

> **Layout note.** The port puts brightness on `Fn + [` / `]` and speed on
> `Fn + ←` / `→`. The factory puts brightness on `Fn + Up` / `Down` and speed on
> `Fn + Left` / `Right`. These are two design choices, not a bug — but if the original
> ergonomics are desired, they are now documented.

## SignalRGB per-key RGB & responsiveness optimizations

### Per-key direct RGB for SignalRGB (`SIGNALRGB_ENABLE`)

Because upstream VIA has no graphical per-key RGB editor, the firmware exposes a fire-and-forget Raw HID direct-RGB protocol over the shared `0xFF60:0x0061` endpoint (`src/smk/via.c`, `src/smk/raw_hid.c`), paired with a drop-in SignalRGB plugin (`tools/signalrgb/Aula_F75_SMK.js`):

- **Protocol (`0xF0` / `0xF1`):** `0xF0` (`[0xF0, start_idx, count <= 9, R, G, B...]`) streams 9 LEDs per 32-byte packet (10 packets for the 6×15 grid, column-major `col * 6 + row`); `0xF1` exits direct mode immediately. Both return `false` from `via_process_packet()` so `raw_hid_task()` re-arms `EP2_OUT_RDY` **without** sending an EP2 IN reply — preventing unread `0xF0` replies from shifting `usevia.app` WebHID responses out of sync when both apps are open.
- **XRAM frame buffer & zero-math ISR path (`indicators.c`):** Colors live solely in `direct_rgb[6][15][3]` (`270 B` XRAM, zero flash wear). `led_regen_one()` outputs `direct_rgb` directly when `direct_active` is set, keeping Caps Lock, `Esc`/`F1` status, battery bar, BT pairing, and macro overlays active on top.
- **Safe fallback & wireless transition:** `direct_timeout` (`480` frames ≈ 3 s) restores `user_settings.led_effect` if SignalRGB closes unexpectedly; `0xF1` clears `direct_active` and calls `aula_rgb_clear()`, `Fn` effect switching overrides direct mode immediately, and `rf_radio_on()` calls `indicators_direct_clear()` when switching to 2.4 GHz or Bluetooth so frozen frames never persist into wireless modes. With `MATRIX_INTERLEAVED_SCAN`, `direct_active` and `direct_timeout` live purely in the main loop, so `indicators_direct_set()` / `indicators_direct_clear()` run without disabling interrupts.
- **EP2 OUT burst & PWM phase-lock fix (`src/smk/raw_hid.c`, `indicators.c`):** SignalRGB streams `10` consecutive 32-byte `0xF0` reports per frame over Priority-1 `_INT_USB`. Previously, copying `EP2_OUT_BUF` inside `raw_hid_receive_irq()` combined with non-atomic `PWMx0CON` startup delayed Priority-0 `Timer2` slots past the `100.0 µs` PWM window, causing a visible right-to-left column refresh wave during static and animated SignalRGB profiles. Deferring the 32-byte `EP2_OUT_BUF` copy from `raw_hid_receive_irq()` (`request_pending = true` while `EP2 OUT` remains NAKed) to `raw_hid_task()` in the main loop and wrapping the 4 `PWM00CON..PWM20CON` startup writes in a 12-instruction (`< 1 µs`) `__critical` section in `indicators_pwm_enable()` eliminated the refresh wave across all SignalRGB modes while remaining harmless to Priority-3 `_INT_EUART0` (`38.3 µs`/byte at `260,870 baud`). SignalRGB streaming is verified on hardware (`0xF0`/`0xF1` fire-and-forget handling avoids shifting VIA's HID IN queue).

### Responsiveness & memory tuning

- **Debounced settings writes (`src/smk/settings.c`, `tick.c`, `sleep.c`):** `settings_mark_dirty()` defers `settings_save()` by `1500` ticks (~600 ms) instead of erasing a 512 B flash sector on every VIA slider step; `__critical` guards prevent torn 16-bit/flag updates against Timer 2, and `sleep_task()` calls `settings_flush()` before `tick_pause()` so pending edits persist prior to sleep.
- **Main-loop RF / battery throttling (`src/keyboards/aula-f75/aula_rf.c`):** `rf_charge_poll()` now runs every 64th `rf_task()` pass (giving real millisecond-scale contact debounce for the 20/200-sample thresholds), and `keyboard_state.battery_level` caches `prev_batt_pct` to skip 16-bit division when percentage is unchanged.
- **Reclaimed EP0 XRAM (`src/smk/usb.c`):** Shrunk `scratch[512]` to `scratch[128]` (384 B reclaimed), keeping total footprint at **`3,565 / 4,096` B XRAM** (`531` B free) and **`43,294 / 58,368` B Flash** with `check_interrupts` clean (`14` vectors, `48` ISR / `296` main functions).

### Interleaved column scanning & hardware-timed PWM multiplexing (`MATRIX_INTERLEAVED_SCAN`)

In the initial sequential schedule, `tick_dispatch()` alternated 15 `400 µs` LED subframes (`6.00 ms`) with one blocking 15-column `matrix_scan_full()` pass (`~320 µs`), coupling key-scan frequency directly to the RGB refresh rate (`6.32 ms` = `158.2 Hz`) and turning all LEDs off for `~320 µs` once per frame.

Enabling `MATRIX_INTERLEAVED_SCAN` in `src/keyboards/aula-f75/kbdef.h` decouples key latency and main-loop RGB rendering from column multiplexing by interleaving **one column scan + one hardware-timed `100.0 µs` PWM illumination window** into each `200 µs` `Timer2` tick:

1. **Interleaved per-column scanning (`src/smk/tick.c`, `src/smk/matrix.c`):**
   - `Timer2` runs at a uniform **`200 µs` period** (`RELOAD_INTERLEAVED = 0xFE70`, `5,000 Hz`).
   - On each `200 µs` tick (`col = 0..14`), `indicators_subframe()` parks PWM (`PWM_CON_PARKED`), `matrix_scan_col(col)` drives only column `col` LOW, waits `6 µs` for row pull-ups to settle, samples rows twice (`2 µs` apart) with a `sample1 == sample2` glitch filter while PWM is parked (`0 mA` LED current), and then `aula_rgb_load_column(col)` loads the 18 PWM duties and starts the hardware-timed illumination pulse on that same active column.
   - **Full-matrix scan rate doubles** from `158.2 Hz` (`6.32 ms`) to **`333.3 Hz` (`3.00 ms` = `15 × 200 µs`)**, while eliminating the once-per-frame `320 µs` dark gap.
2. **Asymmetric eager-press / per-key debounced-release & HID report coalescing (`src/smk/matrix.c`, `src/smk/model.c`):**
   - **Press (`0 -> 1`):** Registers **eagerly (`0 ms` debounce delay)** on the first glitch-checked scan and sets `debounce_cnt = 3` (`4` scans = `12.0 ms`) using a 2-bit vertical counter (`debounce_cnt0[15]`, `debounce_cnt1[15]` — `30 B` XRAM total for all 90 matrix positions).
   - **Release (`1 -> 0`):** Requires **4 consecutive released scans (`12.0 ms`)** before clearing `matrix_debounced[col]`, filtering mechanical switch bounce and electrical release chatter.
   - **HID report batching (`report_batching` / `report_dirty`):** `matrix_task()` brackets its 90-key delta scan in `model_begin_report_batch()` / `model_end_report_batch()` so multi-key rollover or chord changes within a scan coalesce into a **single** USB / 2.4G / BT HID report transmission.
3. **Hardware-timed `100.0 µs` PWM window via `_INT_PWM0` (`src/sino51lib/pwm.c`, `indicators.c`):**
   - Following the stock AULA F75 firmware (`0x6C42`–`0x6D4A`), `_INT_PWM0` is enabled (`IEN1 |= _EPWM0`) at **priority level 2** (`IPH1 |= _EPWM0; IPL1 &= ~_EPWM0` — above `Timer2` at priority 0, below `EUART0` at priority 3).
   - Each column's PWM period runs for **exactly one hardware period (`0x04B0` = `1200` ticks = `100.0 µs`)**. When `PWM0` wraps at `100.0 µs`, `pwm_interrupt_handler()` fires immediately inside the `0..15.0 µs` (`CNT < DUTY1`, `0x00B4..0x00C5`) dead-time window — while all 18 PWM outputs are still electrically LOW (`0V`) — parks `PWM00CON..PWM20CON`, and deselects all 15 column pins (`P6 = 0xFF`, `P5 |= 0x87`, `P4 |= 0x0D`) before `CNT` reaches `0x00B4`.
   - **Exact 5-cycle lockstep across `PWM0`, `PWM1`, `PWM2`:** Both `indicators_pwm_enable()` and `pwm_interrupt_handler()` write the same constant byte (`0x89` on start, `0x81` on stop) to `PWM00CON`, `PWM10CON`, `PWM20CON` at `0, +5, +10` oscillator cycles before arming `PWM_INT_ENABLE` (`0xC9`) on `PWM00CON`, ensuring all three banks run for the exact same `2400`-oscillator-cycle (`1200` PWM tick) window with zero inter-bank phase drift.
4. **Early-exit dirty check & main-loop `indicators_render()` (`aula_rgb.c`, `indicators.c`):**
   - `duty_cache[15][18]` (`540 B` XRAM) stores pre-calculated 16-bit `DUTY2` words per column, and `aula_rgb_set()` skips disabling `ET2` and rewriting XRAM whenever all three duties `(d0, d1, d2)` match `duty_cache[col]`, while `aula_rgb_load_column(col)` writes the 18 hardware `DUTY2` registers directly in the stock P3-rotated order.
   - Effect math (`led_effect_rgb()`, `render_row()`, overlays) runs in the **main loop** inside `indicators_render()`, paced by `regen_steps_pending` (`400 µs` sub-ticks, preserving identical animation speeds).

| Metric | Sequential Scan + ISR Effect Math (Before) | Interleaved Scan + Hardware-Timed PWM (After) | Improvement |
| --- | --- | --- | --- |
| **Full-Matrix Scan Period** | `6.32 ms` (`158.2 Hz`) | **`3.00 ms` (`333.3 Hz`)** | **2.11× faster scan rate (`-52.5%` period)** |
| **Key Press Debounce Delay** | None (single-scan raw pass-through) | **`0 ms` eager press** (`6 + 2 µs` glitch-checked) | **Immediate press registration preserved** |
| **Key Release Debounce Protection** | `0 ms` (vulnerable to switch chatter) | **`12.0 ms` (`4` scans)** per-key vertical counter | **Eliminates release chatter / double-taps** |
| **Worst-Case Key Press Latency** | `~6.32 ms` | **`~3.00 ms`** | **`-3.32 ms` (`2.11×` lower latency)** |
| **Average Key Press Latency** | `~3.16 ms` | **`~1.50 ms`** | **`-1.66 ms` (`2.11×` lower latency)** |
| **Multi-Key HID Report Coalescing** | 1 USB/RF report per changed key | **1 coalesced report per scan sweep** | **Eliminates multi-key report bursts** |
| **`Timer2` ISR Period & Worst-Case Duration** | `400 µs` period / `~320 µs` max (`80%` of slot) | **`200 µs` period / `~42–58 µs` max (`21–29%` of slot)** | **5.5× shorter peak ISR blocking** |
| **Per-Column PWM Illumination Window** | Software-timed (`~350–400 µs`, variable with ISR jitter) | **Hardware-timed `100.0 µs` (`1200` ticks, zero jitter)** | **Zero PWM truncation or column brightness wave** |
| **RGB Frame Refresh Rate** | `158.2 Hz` (interrupted by `320 µs` scan blackout) | **`333.3 Hz` (`3.00 ms` uniform cycle)** | **2.11× higher refresh, zero scan blackout** |

- **Lighting cleanup (`indicators.c`, `via.json`):** Removed the redundant `AULA_FX_SNAKE_RGB` effect (identical to `AULA_FX_SNAKE` in all 7 fixed-color modes and differing only in random vs. stepping hue in rainbow mode; stock `0x26` is an internal one-shot transition), compacting VIA `Effect` IDs to `0..14` (`Off = 14`).

> ## ✅ 2026-09-11 — first successful flash, bootloader preserved
>
> **Code from this repository ran on the device.** The `aula-f75-probe` board target — USB and ISP return,
> **no GPIOs driven** — was flashed using `sinowisp write -d aula-f75 --force`, and verified by a
> full readback:
>
> | Check | Result |
> | --- | --- |
> | enumeration | `258a:010c` **`product="SMK Keyboard"`** |
> | return to ISP | **three times in a row** |
> | bootloader | bytes `61440`–`65535` **identical** to `assets/f75_bootloader.bin` |
> | application area | **zero byte** difference from the intended image |
> | real JTAG view | `0x0000 = 02 F0 00` → **the bootloader**, `0xEFFB = 02 00 DE` → SMK entry point |
>
> The reset vector points to the factory bootloader and the trampoline points to our firmware: the
> factory structure, with our code at the end. `sinowisp` places the trampoline itself from the reset
> vector — nothing extra had to be done.
>
> **Everything else on this page remains unverified on hardware.** The probe drives no GPIOs: the
> matrix, backlight, radio, sleep, and PWM polarity have not been exercised by anything. The terms
> "transcribed", "established", and "verified" in this document refer to the dump and disassembly, not
> the physical device.

> **Status: incomplete port, does not compile, never flashed.**
> The pinout is not established. `kbdef.h` intentionally refuses to compile.

## Identity

| | |
| --- | --- |
| MCU | SinoWealth **SH68F90A**, IC marking **BYK916** |
| USB | **258A:010C** — read from factory flash, device descriptor at `0x5FCE` |
| Flash | 61,440 B firmware + 4,096 B ISP bootloader |
| Wireless | Beken **BK3632**, connected via **EUART0** |
| LED grid | **6 rows × 15 columns** = 90 positions, 80 keys |

⚠️ Not to be confused with the **AULA F75 Ultra** (`FFFE:00A9`), which uses a Westberry WB32FQ95
ARM Cortex-M and runs QMK.

## What is established

| Item | Value | Evidence |
| --- | --- | --- |
| VID / PID | `0x258A` / `0x010C` | device descriptor read in flash at `0x5FC6`, VID/PID at `0x5FCE` |
| LED grid | 6 × 15 = 90 | `num_leds` read live by the OpenRGB SDK; index = col × 6 + row; max index 89 |
| Key count | 80 | 80 LEDs named `Key: …`; the layout places 80 out of 90, leaving 10 holes |
| Layout | 75% ANSI | derived from `aula_f75_layout` (OpenRGB), cross-checked against the hole count |
| Wireless transport | EUART0 | ISR vector 13 = `_INT_EUART0`, `SCON` 0xD8 / `SBUF` 0xAA, half-duplex via `P0CR` + P0.2 |

## Pinout — ESTABLISHED

Cross-checked across four independent sources. **The firmware compiles**:
15,208 B of ROM out of 60,416 (25.1%), 835 B of XRAM out of 4,096.

### Sources

1. **Consolidated GPIO init in the factory firmware**, `fcn @ 0xA7EC`:
   `P0CR=0x9C  P1CR=0x3F  P2CR=0x3F  P3CR=0x3F  P4CR=0x4D  P5CR=0x87  P6CR=0xFF  P7CR=0x60`
   (bit set to 1 = output, see `GPIO_OUTPUT` in `platform/sh68f90/gpio.h`)
2. **Column selection jump table** `@ 0x72F4`: 20 slots, 15 populated, the last 5
   all pointing to the null handler. Each handler releases the previous column
   (`setb`) then selects the next one (`clr`) — slot 0 releases `P4.3`, which closes
   the loop and establishes `P4.3` as the last column.
3. **Row reading routine** `@ 0x73A6`:
   `mov a,P5 ; add a,ACC ; anl a,#0x30 ; mov r7,a ; mov a,P7 ; anl a,#0x0F ; orl a,r7 ; orl a,#0xC0`
4. **Independent port** [`tiagoluizo/smk@aula-f75-port`](https://github.com/tiagoluizo/smk/tree/aula-f75-port),
   which arrives at the exact same pinout through a separate analysis.

### Result

| | Pins |
| --- | --- |
| **Rows** (inputs, active low) | `P7.0` `P7.1` `P7.2` `P7.3` `P5.3` `P5.4` |
| **Columns** (outputs, active low, in scan order) | `P6.0`…`P6.7`, `P5.0` `P5.1` `P5.2` `P5.7`, `P4.0` `P4.2` `P4.3` |
| **Backlight** | 18 PWM outputs = 6 LED rows × R/G/B: `PWM00`-`PWM05` (P3), `PWM10`-`PWM15` (P2), `PWM20`-`PWM25` (P1) |

The row byte places row N on bit N (`P7 & 0x0F` for rows 0-3, `P5` shifted by one
bit and masked with `0x30` for rows 4-5), with unused bits forced to 1 — the exact convention
expected by `src/smk/matrix.c`, which then inverts the sample.

**15 vs 16 columns question: settled.** The table `@ 0x72F4` sizes 20 slots but
only populates 15. There really are **15** physical columns.

## What remains UNESTABLISHED

| Item | Status |
| --- | --- |
| Key matrix ↔ LED grid mapping | ✅ **verified** by independent HID capture — see below |
| Loading order of the 18 PWM channels | ✅ **extracted and implemented** (`aula_rgb.c`) |
| Channel ↔ (row, color) mapping | ✅ **established via the OpenRGB driver** |
| Wiring rendering into the SMK loop | ✅ **resolved** — LED subframes in `tick.c`, see below |
| Encoder rotation | ✅ **implemented** — quadrature on `P0.5`/`P0.6`, volume ± (`src/keyboards/aula-f75/aula_encoder.c`) |
| Pins with unknown roles | ❓ `P0.0` `P0.1` `P4.1` `P4.4` `P5.5` `P5.6` — `P7.4`/`P4.5` are the connection mode switch, `P4.7` the "module ready" line, `P3.4` VBUS detect, `P7.6` charger enable (active LOW), `P7.7` charger status (active LOW) |
| Sleep | ✅ **implemented**, USB **and radio** — transcribed from factory firmware, untested on hardware |
| 2.4 GHz / Bluetooth wireless | ✅ **verified on hardware** (`aula_rf.c`, EUART0) — see top banner |

## Matrix: 81 keys, not 80 — the encoder

An **independent physical HID capture**
([tiagoluizo/smk](https://github.com/tiagoluizo/smk/blob/aula-f75-port/docs/superpowers/evidence/2026-08-07-aula-f75-keymap-capture.md))
mapped the 6×15 matrix to unique HID usages `0x04..0x5D`. Compared row by row against the
layout derived from OpenRGB, it **matches position for position** — with one difference:

**There are 81 switches, not 80.** Position `[row 0][col 14]` is the **encoder press**.
It is a real matrix position, invisible to OpenRGB because it has **no LED** — hence
90 LEDs for 81 keys, and 9 genuinely unused coordinates.

**Rotation** is outside the matrix. Poller `@ 0x7928`, called from the tick:

```asm
0x792b   mov  a, P0
0x792f   swap a          ; P0.5 -> bit1, P0.6 -> bit2
0x7930   rrc  a          ; -> bit0, -> bit1
0x7931   anl  a, #0x03   ; keep the two phases
```

→ **phases on `P0.5` and `P0.6`**, both configured as inputs under `P0CR = 0x9C`. Verified here, and
consistent with the independently extracted GPIO init. Not implemented in this port.

## RGB: why the Air60 rendering cannot be reused

The two keyboards do not share the **same LED matrix topology**, and that is the blocker, not a
wiring detail.

| | NuPhy Air60 | AULA F75 |
| --- | --- | --- |
| LED columns | **PWM** channels (`LED_PWM_C0` = `PWM40`, …) | matrix columns in **plain GPIO**, multiplex selectors |
| LED rows | plain GPIO pins (`RGB_R0R`, `RGB_R0G`, …) | the **18 PWM channels** = 6 rows × R/G/B |

It is the exact opposite. SMK's default renderer (`src/user/indicators_render.c`) is an
empty stub, and the Air60's (`layouts/default/indicators.c`) is written for its own topology.
**This port therefore has no RGB** — `LED_PWM_C0..C17` in `kbdef.h` are declared but unused
until a dedicated `indicators.c` is written.

### Channel loading order — extracted

`fcn @ 0x6E61` reads **36 consecutive bytes** from the framebuffer (XRAM base `0x05A2` + col × 36) and
writes them into the `DUTY2` registers, in **big-endian** order (high byte first), alternating the
two DPTRs via `INSCON`:

| Bytes | Channels | Pins |
| --- | --- | --- |
| 0-11 | `PWM20`…`PWM25` | P1.0…P1.5 |
| 12-23 | `PWM10`…`PWM15` | P2.0…P2.5 |
| 24-29 | **`PWM03`, `PWM04`, `PWM05`** | P3.3…P3.5 |
| 30-35 | **`PWM00`, `PWM01`, `PWM02`** | P3.0…P3.2 |

**Only one irregularity, and it is deliberate: a rotation by 3 within the P3 group** — `PWM03-05`
are loaded *before* `PWM00-02`. The addresses were cross-checked against the `0xFF80-0xFFFF` XDATA map
in the SH68F90 CV2.0 datasheet.

### Channel ↔ (row, color) mapping — established via the OpenRGB driver

**OpenRGB's code *is* the documented protocol** — no need to sniff USB.
`SinowealthKeyboard10cController::SetLEDsDirect` builds a **520-byte** feature report:

```c
buf[0x00] = 0x06;                      // Report ID 6
buf[0x01] = 0x08;                      // command: write color table
buf[0x04] = 0x01;                      // address bytes
buf[0x06] = 0x7A; buf[0x07] = 0x01;    // length = 0x017A = 378
buf[0x08 + i*3] = R, G, B;             // 3 bytes per LED, i = LED index
```

Two corrections to [xevrion's blog post](https://xevrion.dev/blogs/aula-f75-linux-reverse-engineering):
the command is **`0x08`**, not `0x0A`, and colors are **3 bytes** per LED, not 4.

The LED index is that of the layout table, i.e., **`col × 6 + row`**. Per column,
the host therefore sends 18 bytes: `row0 R,G,B`, `row1 R,G,B`, … Hence **`channel = row × 3 + color`**,
and combining this with the loading order:

| Channels | Rows | Pins |
| --- | --- | --- |
| 0-5 | rows 0 and 1 | `P1.0`…`P1.5` (`PWM20`…`PWM25`) |
| 6-11 | rows 2 and 3 | `P2.0`…`P2.5` (`PWM10`…`PWM15`) |
| 12-14 | row 4 | `P3.3`…`P3.5` (`PWM03`…`PWM05`) |
| 15-17 | row 5 | `P3.0`…`P3.2` (`PWM00`…`PWM02`) |

**Two rows per port, six pins each.** The regularity of the result is an argument in itself, and
it explains the apparent rotation of the P3 group noted in the loading table.

**This is no longer an inference.** The factory converter `0x765B` reads the 8-bit framebuffer at
`0x0152 + col × 18 + row × 3 + color` and writes the 16-bit buffer at
`0x05A2 + col × 36 + row × 6 + color × 2`: the **exact same** `(row, color)` indexing on both
sides, without permutation. And the phase table `CODE 0x2922`, also indexed as `row × 3 +
color`, matches byte for byte the `DUTY1` values set at init, permuted by the exact
loading order above. The mapping is read directly, not assumed; the hardware test required
by the previous draft of this page is no longer necessary.

### Architectural mismatch with SMK — resolved

Column-by-column rendering requires reloading all 18 duties **on every column advance**. In the
factory firmware, the `_INT_PWM0` ISR owns the column advance — and takes the opportunity to
read the matrix rows along the way (`0x73A6`).

This page long concluded that we had to choose between "letting the ISR take over the columns
like the factory firmware" and "hooking the reload into the scan loop", and that this choice could not
be validated without hardware. **Both branches were wrong, and the scheduler we needed
already existed in SMK.**

`src/smk/tick.c` alternates, from the Timer2 ISR, **one matrix scan** followed by
`LED_SUBFRAMES_PER_SCAN` **LED subframes**:

```c
void tick_dispatch(void) {
    if (scan_due) { scan_due = false; subframes_since_scan = 0; run_matrix_scan(); return; }
    run_led_subframe();
    if (++subframes_since_scan >= LED_SUBFRAMES_PER_SCAN) scan_due = true;
}
```

The port sets `LED_SUBFRAMES_PER_SCAN = MATRIX_COLS`: fifteen subframes, one per LED column,
between two key scans. Rendering therefore does not hook *into* the scan — it lives alongside it,
in the time slot that `tick.c` reserves for it (`RELOAD_LED_SUBFRAME` = 400 µs vs. ~320 µs for a
scan). `matrix.c` is untouched, and `pwm_interrupt_handler` remains empty.

Electrical coexistence, meanwhile, was already solved: `matrix_scan_full()` brackets its scan
with `indicators_pwm_disable()` and `indicators_pwm_enable()` **because LED current
couples into row sensing**. All it took was implementing both functions.

A precedent existed in the tree and had gone unnoticed: the **genesis-thor-300** also shares
its matrix columns with its LED multiplexing, sets `LED_SUBFRAMES_PER_SCAN MATRIX_COLS`, and
measures its budget — *"one effect evaluation per subframe: six, one per row, does not fit and
starves the USB interrupt"*. The port initially reused its structure, one single effect evaluation per
subframe, replacing its whole-frame dithering with true hardware PWM, and subsequently evolved to
`MATRIX_INTERLEAVED_SCAN` (see [Interleaved column scanning & hardware-timed PWM multiplexing](#interleaved-column-scanning--hardware-timed-pwm-multiplexing-matrix_interleaved_scan) above), where each `200 µs` `Timer2` tick scans one column while PWM is parked, loads pre-computed `DUTY2H/L` bytes, and runs a hardware-timed `100.0 µs` PWM window terminated by `_INT_PWM0` while effect evaluation runs in the main loop.

#### Polarity, settled from the dump

This section previously treated polarity as undecidable and the port isolated it in a constant
`AULA_RGB_DUTY_INVERTED`. **The question was decidable, and the answer is the opposite of what
had been chosen.** Three independent findings lock it down.

**1. `DUTY1` is written once, and holds a per-channel constant.** Out of all 36 references to
the `DUTY1` registers across the entire image (`PWMnnDUTY1H` at `0xFFB8`–`0xFFC9`, `DUTY1L` at
`0xFFA0`–`0xFFB1`), **all 36 are in the init routine `0x6713`–`0x68CD`, and zero elsewhere**. The
column loader only modulates `DUTY2`. The 18 values:

| Bank | Channels | `DUTY1H` | `DUTY1L` |
| --- | --- | --- | --- |
| PWM0 | `PWM00`…`PWM05` | `0x00` | `C3 C4 C5 C0 C1 C2` |
| PWM1 | `PWM10`…`PWM15` | `0x00` | `BA BB BC BD BE BF` |
| PWM2 | `PWM20`…`PWM25` | `0x00` | `B4 B5 B6 B7 B8 B9` |

Neither `0` nor `0x04B0`: eighteen consecutive values `0xB4`–`0xC5`, one per channel.

**2. The same table exists in flash, and its permutation matches the loader.** `CODE 0x2922` holds
18 bytes, `0xB4 + index`, indexed as `row × 3 + color`. Sorted, this set is **identical** to
the `DUTY1` values above; the permutation between the two is **identical** to the loading order
of `fcn @ 0x6E61` (`buf[0..5]→PWM20..25`, `[6..11]→PWM10..15`, `[12..14]→PWM03..05`,
`[15..17]→PWM00..02`). Along the way, this cross-check confirms the channel ↔ (row, color) mapping
and the P3 group rotation, previously deduced from the OpenRGB driver's ordering.

**3. The 8 → 16-bit conversion — the former "only real gap" — is at `0x765B`–`0x770C`.** A single
function touches both bases: it writes the 8-bit framebuffer (`0x0152 + col × 18 +
row × 3 + color`) and then, directly, the 16-bit buffer (`0x05A2 + col × 36 +
row × 6`, **big-endian**). Its arithmetic, at `0x76C3`:

```
76C3  mov  a,r3           ; the 8-bit value
76C4  mov  b,#0x04
76C7  mul  ab             ; r6:r7 = v x 4
76CB  mov  a,r5           ; the row
76CC  mov  b,#0x03
76CF  mul  ab
76D0  add  a,#0x22        ; dptr = 0x2922 + row x 3
76D5  addc a,#0x29
76DA  movc a,@a+dptr      ; the channel phase
76DD  add  a,r7           ; r6:r7 += phase   (16-bit addition)
```

That is:

> **`DUTY2 = (value << 2) + phase[row × 3 + color]`**, with `DUTY1` fixed at that exact same phase.

**Consequence, by monotonicity.** Neither the datasheet nor `src/platform/sh68f90/pwm.h` states
which of the two edges opens the pulse — and we don't need them to. `DUTY1` is fixed and `DUTY2` grows
with the value; under the opposite reading, value 1 would yield a pulse of 1196 ticks and
value 255 a pulse of 180, meaning brightness would **decrease** with the value, with a discontinuity at zero.
Absurd. The pulse therefore spans from `DUTY1` to `DUTY2`, its width is `value << 2`, and the duty
cycle is **non-inverted (direct)**: 0 = off (zero width, `DUTY2 = DUTY1`), 255 = 1020/1200 or 85%
of the period. The phase offset, one tick per channel, staggers the rising edges of the 18 channels
instead of having them coincide; it is invisible in the light output.

The clue pointing toward direct polarity — `user_matrix_sinks_off()` pulls all 18 pins low —
was therefore right, and this page's earlier conclusion ("common anode, PWM acts as sink")
was wrong. `AULA_RGB_DUTY_INVERTED` has been removed from the port: there is no longer a choice to make.

**A single deliberate departure from the factory firmware: clamping.** At value 255, the sum reaches up to
`1020 + 197 = 1217`, or 17 ticks beyond the period. The factory firmware does not clamp it; the port does,
because `led_effect_rgb()` genuinely produces 255 and a `DUTY2` greater than the period has
undefined behavior in the datasheet. Clamping only kicks in from value 251 upward, and
only on the three channels of row 5.

### What is established on the PWM side

- The MCU has **25 channels**: PWM0 (00-05), PWM1 (10-15), PWM2 (20-25), PWM3 (30-33), PWM4 (40-42).
- The **18** used for RGB are PWM0/1/2, confirmed by `P1CR=P2CR=P3CR=0x3F` in the factory
  init.
- The pin↔channel mapping is deduced from the NuPhy Air60: `PWM0x↔P3_x`, `PWM1x↔P2_x`,
  `PWM2x↔P1_x`, `PWM4x↔P5_x`. **Deduced, not confirmed by datasheet.**
- **Factory PWM period: `0x04B0` = 1200**, read at `0x6707` (`PWM0PERDH=0x04`) and `0x670D`
  (`PWM0PERDL=0xB0`). The `tiagoluizo` port reprograms its period to `0x0400` = 1024 and computes
  its duty cycles accordingly (`0x0400 - (v << 2)`) — internally consistent, but **not
  the factory value**, and its inversion is contradicted by the `DUTY1` findings above. Its
  `v << 2` scaling factor, on the other hand, is the exact factory factor.
- **Direct (non-inverted)** duty cycle: `DUTY2 = (v << 2) + phase`, `DUTY1` = phase, hence pulse
  width `v << 2` — 0 = off, 255 = 85% of the period. Established from the dump (see above);
  an earlier draft of this page concluded "inverted", which was wrong.
- Factory `PWM00CON` = `0x89` (`0x68D3`) = `PWM_MODE_ENABLE | PWM_SS | prescaler 1`.

## `fcn.0000AF99` — publishing the RGB effect, protected against USB

Called from `main()` (`0x9197`) and from `fcn.000005EA`, the large periodic function.

```asm
0xaf99   jnb  0x2c.7, 0xafc3     ; nothing to do if the flag is not set
0xaf9c   clr  0x2c.7             ; acknowledge
0xaf9e   anl  IEN1, #0xfe        ; mask EUSB -- the USB interrupt
0xafa1   mov  dptr,#0x0e24 ; movx a,@dptr
0xafa5   cjne a, #0x20, 0xafb6   ; special case if effect equals 32
0xafa8     mov r4,#0xca ; rrc a  ; 202 and 101
0xafb3     lcall 0xaac1
0xafb6   clr  0x24.5
0xafb8   mov  dptr,#0x0e24 ; movx a,@dptr
0xafbc   mov  dptr,#0x009d ; movx @dptr,a    ; publish the effect
0xafc0   orl  IEN1, #0x01        ; unmask EUSB
0xafc3   ret
```

`IEN1` bit 0 = **`EUSB`**, confirmed twice: SMK header (`#define _EUSB (1u << 0)`) and datasheet
(`IEN1 A9H : - ES0 EPWM4 EPWM3 EPWM2 EPWM1 EPWM0 EUSB`).

**This is a critical section.** `0x0E24` holds the *pending* effect, `0x009D` the *current* effect, and
the copy is performed with USB masked because the USB path also touches `0x009D`. Key-driven settings
and host-driven settings contend for the same variable; this function arbitrates.

### `0xA4BD` — the second setter of `0x2C.7`, resolved

`0x2C.7` ("publish effect") has two producers. The first, `0x0A71`, is the user path
already documented. Here is the second.

#### How we get there: a jump-table dispatcher on `XRAM 0x0D17`

`0xA674`:

```asm
mov dptr,#0x0d15 ; movx a,@dptr ; jnz 0xa67d
jb  0x2a.3, exit
mov dptr,#0x0d17 ; movx a,@dptr
cjne a,#0x0a, .+3 ; jnc exit        ; reject 0x0D17 >= 10
mov dptr,#0xa68d
mov r0,a ; add a,r0 ; add a,r0      ; index x 3
jmp @a+dptr
```

Table of 10 3-byte entries at `0xA68D`:

| `0x0D17` | target |
| --- | --- |
| 0, 9 | `ljmp 0x83D2` |
| 1 | — |
| 2 | `ljmp 0x9363` |
| 3 | `ljmp 0x8C0F` |
| 4 | `0xA6C6` |
| **5, 6, 7** | **`ljmp 0xA468`** |
| 8 | `0xA6D2` |

`XRAM 0x0D17` is therefore an index from 0 to 9, and values 5, 6, and 7 share the same handler —
which is why `0xA468` re-tests them one by one internally. Every other branch is guarded
by `jb 0x2a.5` and `jb 0x23.3`; branch 5/6/7 is not, because `0xA468` manages the
`0x2A.5` lock itself. See below for what `0x0D17` actually is.

### `XRAM 0x0D17` — a key's action class, read from a flash table

`0x0D17` is not a variable computed by the firmware: it is a **field extracted from a table**.

#### The extraction idiom

Eleven sites write `0x0D17`, all with the exact same prologue. The two accessors decode it:

```asm
fcn.00004DEC:                  ; load 4 bytes of CODE @ DPTR
    clr a  ; movc a,@a+dptr ; mov r4,a
    mov a,#1 ; movc a,@a+dptr ; mov r5,a
    mov a,#2 ; movc a,@a+dptr ; mov r6,a
    mov a,#3 ; movc a,@a+dptr ; mov r7,a

fcn.00004DC5:                  ; r4:r5:r6:r7 >>= r0   (shift by r0 BITS, across 32 bits)
    mov a,r0 ; jz done
loop: rrc on r4, r5, r6, r7 ; djnz r0, loop
```

`fcn.00004DC5` therefore takes a number of **bits**, not bytes: `r0 = 8` and `r0 = 16` extract
byte 2 and byte 1 of a 4-byte record, respectively. In `fcn.00007ECA`:

```asm
mov B,#4 ; mov a,r3 ; mul ab
mov DPL,a ; mov a,B ; addc a,#0xce ; mov DPH,a   ; DPTR = 0xCE00 + index x 4
lcall fcn.00004DEC
mov r0,#0x08 ; lcall fcn.00004DC5 ; movx [0x0d16], r7
...
mov r0,#0x10 ; lcall fcn.00004DC5 ; movx [0x0d17], r7
```

| Variable | Shift | Record byte |
| --- | --- | --- |
| `0x0D15` | 0 | b3 |
| `0x0D16` | 8 | b2 |
| `0x0D17` | **16** | **b1** |
| — | 24 | b0 |

The index comes from `XRAM 0x02E0`, written by `fcn.00003108` (`0x3190`) from the event
counter `IDATA 0x09`.

#### Table `0xCE00` — verified on the dump

Excerpt from the first 40 entries of `assets/f75_firmware.bin`:

| idx | offset | b0 b1 b2 b3 | `0x0D17` | dispatch |
| --- | --- | --- | --- | --- |
| 0 | `0xCE00` | `00 00 00 29` | 0 | `0x83D2` |
| 1 | `0xCE04` | `00 00 00 35` | 0 | `0x83D2` |
| 2 | `0xCE08` | `00 00 00 2b` | 0 | `0x83D2` |
| 3 | `0xCE0C` | `00 00 00 39` | 0 | `0x83D2` |
| 4 | `0xCE10` | `00 02 00 00` | 2 | `0x9363` |
| 5 | `0xCE14` | `00 01 00 00` | 1 | — |
| 11 | `0xCE2C` | `00 04 00 00` | 4 | `0x8EC4` |
| 12 | `0xCE30` | `02 00 00 70` | 0 | `0x83D2` |
| 17 | `0xCE44` | `00 08 00 00` | 8 | `lcall 0x91D3` |
| 36 | `0xCE90` | `08 03 02 00` | 3 | `0x8C0F` |

**Across all non-null entries, `b1` is 0, 1, 2, 3, 4, or 8 — never ≥ 10.** The
`cjne a,#0x0A ; jnc exit` bound in the dispatcher is never violated by the factory table. This is an
empirical validation of the decoding: if the wrong byte had been extracted, the distribution
would have no reason to fall within bounds.

And when `b1 == 0`, **`b3` contains a keyboard HID usage code**: `0x29` Esc, `0x35`
grave/backtick, `0x2B` Tab, `0x39` Caps Lock, `0x04`–`0x1D` letters, `0x1E`–`0x23` digits.
Two entries carry `b0 = 0x02` — the **LeftShift** bit of the HID modifier byte.

#### Interpretation

The record is therefore `[modifiers, action class, parameter, HID usage]`:

| Byte | Variable | Role |
| --- | --- | --- |
| b0 | — | HID modifiers (`0x02` = LeftShift observed) |
| b1 | `0x0D17` | **action class**, index into dispatcher `0xA68D` |
| b2 | `0x0D16` | parameter (routed to `0x031F` when class equals 9) |
| b3 | `0x0D15` | HID usage code |

Dispatcher `0xA674` actually tests `0x0D15` first (`jnz`), and `0x83D2` — class 0 —
repeats the exact same test before branching to the transmission path at `0x845F`. Class 0 = "emit
this HID usage"; classes 1 through 8 are special actions, including 5/6/7 for the temporary effect
described above.

> *Inferred:* that this table is the keyboard's **remap table**. What is established is its
> structure, its addressing, the extraction, and the fact that `b3` holds HID usages.

#### Consequence: the table is rewritable

`0xCE00` falls inside **page 103**, right in the middle of the IAP zone `0xC600`–`0xEBFF` established
earlier — and page 103 happens to be one of the seven non-blank pages in the dump. The firmware can
therefore rewrite this table using its own routine `fcn.0000AAC1`, without going through ISP.

This is the most promising entry point for key remapping: it requires neither reflashing the
firmware nor porting anything — only reaching the right command in the configuration
protocol. **Not attempted: nothing has been flashed.**

#### `fcn @ 0xA468` — two symmetrical transitions around effect `0x2D`

The whole routine is locked by `0x2A.5`, and exits immediately if `0x28.2` is set.

**Entry, `0xA483`** — if `0x0D17 ∈ {5, 7}`, current effect `0x009D == 0x20`, lock free:

```asm
setb 0x24.5      ; flag tested by euart0.parse (0x0632, 0x07A5)
setb 0x2a.5      ; lock "temporary effect active"
setb 0x24.1
setb 0x27.7
movx [0x0e24], a ; a == 0x20: the effect to restore, stored in "pending effect"
movx [0x009d], #0x2d   ; current effect forced to 0x2D
```

**Exit, `0xA4BD`** — if (`0x0D17 == 7` or `== 6`), current effect `== 0x2D`, lock set:

```asm
setb 0x2c.7      ; publish effect      <-- the second producer
clr  0x2a.5      ; release lock
clr  0x24.5
movx [0x08c0], [0x0d18]
movx [0x08c1], #0x00
movx [0x08c2], #0x02
setb 0x27.4      ; doorbell
clr  0x2c.0      ; re-enable EUART0 parsing
```

Effect `0x2D` is therefore a **temporary effect**: the firmware stashes the current effect `0x20` into
`0x0E24`, switches to `0x2D` for as long as the mode warrants it, and republishes upon exit. It uses the
exact same "pending effect → current effect" mechanism as the user path, triggered by a
mode change rather than a keystroke.

#### `clr 0x2C.0` — what exiting unblocks

`isr.timer2` tests this bit at `0xA504`:

```asm
jb 0x2c.0, 0xa51c      ; jumps over the `lcall euart0.parse` at 0xA519
```

As long as `0x2C.0` is set, **received EUART0 frames are not parsed**. Exiting the
temporary mode re-enables parsing. The link between RGB and wireless therefore runs through here as well.

#### `0x08C0` is not an RGB buffer: it is a HID report

Eight sites write the same 3-byte structure to `0x08C0` and then set `0x27.4` (`0x84D6`,
`0x8900`, `0x8CE5`, `0x8F94`, `0x9412`, `0xA4C3`, `0xB041`, plus `0x05AB`). It is a shared
idiom, not a quirk of this path.

`fcn.00006ACF` is the **only** consumer of `0x27.4` — a dispatcher that walks the
pending flags in priority order:

```asm
jnb  0x27.4, next
setb 0x26.7
clr  0x27.4
movx [0x0f0c], #0x02
movx [0x08bf], #0x03          ; a = 2 then inc a
movx [0x0f0f], #0x01
movx [0x0f10], #0x08
mov  a, #0xbf
ljmp 0x6c07
```

`0x0F0F..0x0F11 = 01 08 BF` is a **generic pointer** to `0x08BF` — the exact address of the
buffer written right above. The next flag (`0x2A.6`) follows the same pattern with
`01 09 80` → `0x0980`. And byte 0 of the buffer equals `0x0F0C + 1`: **3** here, **4** for the
next one. These are **HID Report IDs**.

The "message" from `0xA4BD` is therefore the HID report `03 <0x0D18> 00 02` pushed to the host —
a state-change notification, not a color.

### How the firmware selects an effect

Complete chain, from setting to application:

```
config byte, HIGH nibble (4 bits)
        |  swap a ; anl a,#0x0f          @ 0x07F8
        v
   XRAM 0x038D      effect sub-index, 0..15
        |  add a,#0x20                   @ 0x0A77
        v
   XRAM 0x0E24      PENDING effect
        |  setb 0x2C.7 ; lcall 0xAF99    @ 0x0A71, 0x0A7D
        v
   XRAM 0x009D      CURRENT effect       (copied with USB masked)
        |
        v
   tables 0xA8BC / 0xA8D5   ->  mode parameters
```

**The sub-index fits in a nibble**, extracted via `swap a ; anl a,#0x0f`: hence **16 possible
effects**, shifted into **32..47** by `+ 0x20`. That explains the `cjne a, #0x20` in
`fcn.0000AF99`: it is not a magic value, it is the **lower bound of the range**.

Three paths write `0x0E24`:

| Site | Role |
| --- | --- |
| `0x0A79` | **user path** — `0x038D + 32`, followed by immediate publication |
| `0x9184` | **boot restore** — re-reads `0x009D` right after `fn.fx_init` (`0xA283`), in `main()` |
| `0xA48B` | **tick path** — tests `0x009D == 0x20` then sets four flags (`0x24.5`, `0x2A.5`, `0x24.1`, `0x27.7`) |

`0x038D` is read at 18 locations and written at 3 (`0x02B3`, `0x034E`, `0x07FC`). The reads compare it
to `8` (`0x0B1F`), add `-2` to it (`0x0DB5`), or decrement it — these are the
"next / previous effect" navigation actions.

Alongside it, `0x038E` and `0x038F` form a small configuration block written right after
(`0x038E = 0`, `0x038F = r5`), unidentified.

### The animation scheduler — `0x1AF0` and table `0x1B13`

Selecting an effect does not animate it. Animation is paced by a scheduler located at the end of the
large RGB function (Ghidra `0x1108`–`0x1DC2`, r2 `fcn.00001111`), and **neither tool had
surfaced it**: the body is reached via a computed jump, so r2 leaves it outside any function and Ghidra
leaves the bytes undefined. It can be read in disassembly starting from a known anchor.

#### The three variables

| Address | Role | Written by |
| --- | --- | --- |
| `0x0300:0x0301` | **animation tick**, 16-bit big-endian | incremented by `isr.pwm4` (`0x8188`), reset to zero by `0x1D69` |
| `0x08BB:0x08BC` | **period** of the current effect | `0x1AE2`, right before the dispatch: high byte = 0, low byte = `r7` |
| `0x097C` | secondary counter | incremented by `isr.pwm4` (`0x8196`) |

`isr.pwm4` increments `0x0301` and only touches `0x0300` on overflow — so it really is a 16-bit
counter, and since the period fits in `r7`, it remains **under 256**.

> `0x08BC` is never reached by any `mov dptr, #0x08bc`: it is only read via `inc dptr`. Searching for
> the address of the low byte of a 16-bit word yields nothing — this is the computed-pointer trap,
> already encountered on the settings block.

#### What precedes the dispatch — table `0x1744`, nineteen seeders

The dispatch described below is not the entry point. It is preceded by a **second
dispatcher**, at `0x1741`, which branches on `[0x0ED0]` — the **requested** effect — rather than `0x0896`,
the **active** effect. It uses the same inline dispatcher `0x4E45` as the received `0x08` container, and
entries are read the same way: `(addr_hi, addr_lo, key)`, terminator `00 00`, followed by
the default address — here **`0x1AEA`**, i.e., *the rendering dispatch itself*. An effect without a seeder
therefore falls straight through into rendering.

##### The skeleton, identical across all nineteen

```asm
20 21 03        jb  0x21, +3          ; is the reseed bit set?
02 1a ea        ljmp 0x1AEA           ; no -> rendering dispatch, without seeding anything
c2 21           clr 0x21              ; yes: consume the request
d2 22           setb 0x22             ; enable rendering
c2 32 / c2 55   clr ...               ; clear the effect's state bits
12 xx xx        lcall <SEEDER>        ; specific to the effect
90 0d 9d e0     a = [0x0D9D]          ; the SPEED
90 xx xx        dptr = <effect-specific period table>
02 1a e0        ljmp 0x1AE0           ; movc, then [0x08BB:0x08BC] = period, then dispatch
```

Bit `0x21` is the one raised by `0x16EC` when the effect changes, and `0x0D9D` is the speed. Three
facts follow from this, which no other part of this page provided:

- **the seeder only runs once per effect change**, not on every frame;
- **the period depends on BOTH the effect and the speed**, via a two-dimensional lookup;
- **four effects also perform per-frame work** prior to the dispatch (see below).

##### The table

| Effect | Arm | Cleared bits | Seeder | Period table |
| --- | --- | --- | --- | --- |
| `0x00` | `0x1781` | `0x32`, `0x55` | `0xEDE7(0)` then `0xEE6A` | — |
| `0x01` | `0x179A` | `0x55` | `0xEF85` | — |
| `0x02` | `0x17AC` | `0x32`, `0x55` | `0xEFB8` | index 2 |
| `0x03` | `0x17C4` | `0x32`, `0x55` | `0xEFB8` | index 3 |
| `0x04` | `0x17DC` | *(inside seeder)* | `0xA94A` | index 4 |
| `0x05` | `0x183D` | `0x55` | `0xEF32` | index 0 |
| `0x06` | `0x1853` | *(inside seeder)* | `0xEE62` + `0xA791` | index 5 |
| `0x07` | `0x1866` | *(inside seeder)* | `0xA94A` | index 6 |
| `0x08` | `0x18F3` | *(inside seeder)* | `0xEE62` + `0xEF4D` | — |
| `0x0A` | `0x1902` | `0x55`, `0x32` | `0x50E2` | index 8 |
| `0x0B` | `0x191A` | `0x55`, `0x32` | `0xEFB8` | index 9 |
| `0x0C` | `0x1932` | `0x55`, `0x32` | `0xEE6A` | index 10 |
| `0x0D` | `0x19C6` | `0x55`, `0x32` | `0xA846` | index 11 |
| `0x0F` | `0x1A9D` | `0x32`, `0x55` | **`0xACA3`** — the transposer | index 12 |
| `0x10` | `0x1AB4` | `0x32`, `0x55` | `0xEFBE` | index 13 |
| `0x11` | `0x1ACB` | `0x32`, `0x55` | `0xEF85` | index 14 |
| `0x20` | `0x19DE` | `0x55` | `0x95A4(0)` | — |
| `0x26` | `0x19F2` | `0x55` | `0x50E2` | — |
| `0x2D` | `0x1A04` | — | `0x94E6` | — |
| *default* | `0x1AEA` | — | — | — |

**Keys `0x09` and `0x0E` are absent**, just like entries 9 and 14 in the rendering table
`0x1B13`. Two independent tables skipping the exact same two effects: those two slots
are empty in the factory firmware, not a gap in our reading.

##### The period tables — `CODE 0x2FE9`, fifteen 5-byte tables

Indexed by speed `[0x0D9D]`, from 0 (slow) to 4 (fast). The period goes into `[0x08BB:0x08BC]`,
and the rendering handler only runs when the tick reaches it.

| Index | Address | Values | Effect |
| --- | --- | --- | --- |
| 0 | `0x2FE9` | `120 100 80 50 20` | `0x05` |
| 1 | `0x2FEE` | `60 50 40 30 20` | **never referenced** |
| 2 | `0x2FF3` | `45 35 25 15 5` | `0x02` |
| 3 | `0x2FF8` | `45 35 25 15 6` | `0x03` |
| 4 | `0x2FFD` | `80 60 40 20 8` | `0x04` |
| 5 | `0x3002` | `20 15 10 5 1` | `0x06` |
| 6 | `0x3007` | `88 68 48 28 8` | `0x07` |
| 7 | `0x300C` | `115 100 85 60 30` | **never referenced** |
| 8 | `0x3011` | `120 90 70 45 1` | `0x0A` |
| 9 | `0x3016` | `30 24 18 12 6` | `0x0B` |
| 10 | `0x301B` | `32 24 18 16 6` | `0x0C` |
| 11 | `0x3020` | `115 95 75 55 30` | `0x0D` |
| 12 | `0x3025` | `32 24 16 8 1` | `0x0F` |
| 13 | `0x302A` | `46 36 26 16 6` | `0x10` |
| 14 | `0x302F` | `50 40 30 20 8` | `0x11` |

A search for all `mov dptr` loads targeting `0x2FE9`–`0x3033` across the entire image yields
fourteen sites: the thirteen arms above, plus `0xAC25` which reuses index 0. **Indices 1
and 7 are not loaded anywhere.** Six effects have no table (`0x00`, `0x01`, `0x08`, `0x20`,
`0x26`, `0x2D`): their cadence is handled by their own engine.

##### The four arms that work on every frame

`0x04`, `0x07`, `0x0C`, and `0x2D` do more than just seed. After setting the period, and **on every
pass**, they read the last seen key and project it onto the grid:

```c
if ([0x0EE1] == 0) {                       /* a key is pending */
    id = CODE[0xC500 + [0x0EE0]];          /* grid index col*6+row -> id col*8+row */
    [0x0ED1] = id & 7;                     /* row */
    [0x0ED2] = id >> 3;                    /* column */
    ...
}
```

This is the same `0xC500` table from which this page derives the presence map, read here for what it
really is: **a conversion from grid index to factory ID**, with `0xFF` for the six
keyless slots.

| Arm | What it does with the key |
| --- | --- |
| `0x04` | sets bit `1 << row` in **two** maps: `0x013B + col` and `0x0001 + col` |
| `0x07` | sets this bit in **all four**: `0x08C6`, `0x02E8`, `0x013B`, `0x0001`, all `+ col` |
| `0x0C` | tests for `0xFF` first, then reads `CODE[0x2EED + row*21 + col]` and `CODE[0x2F6B + …]` into `[0x0ED5]`/`[0x0ED6]`, and calls `0x5114` |
| `0x2D` | same read of both tables, then, if bits `0x44` and `0x43` are set, calls `0x8A1A` with `[0x0ED5]*6 + 0x17 + [0x0ED6]` |

The four maps in `0x04` and `0x07` are the exact ones this page attributes to the four-direction
automaton of effects 4, 7, and 13 — **and arm `0x07` feeds all four whereas
arm `0x04` only feeds two**, which finally distinguishes two effects that the rendering table
otherwise points to the exact same engine `0x5BE9`.

And `0x2EED` reappears here through an independent path, with its stride of 21 bytes per row: the
corrected base of table A is confirmed a second time.

##### What the port takes from this — done

`indicators.c` previously paced its animations with an invented divider, `16 >> led_speed`, and a
decay of `4 << led_speed`. Both have been replaced with the factory data.

**The unit is the millisecond.** The factory systick is 1 ms, and the anchor is the factory reset
shortcut: its counter must reach **3000** ticks (`0x870C`), and a long press is three
seconds.

**The effect → table mapping comes from dispatcher `0x1744`**, not guesswork: each
seeder loads its table, and the engine reached by the rendering handler identifies which factory effect
the port transcribes.

| Our effect | Factory effect | Engine | Table |
| --- | --- | --- | --- |
| `FX_RADIAL` | 1 | `0x746A` | none — **fixed period 10**, hardcoded at `0x1B61` |
| `FX_HORIZONTAL` | 2 | `0x7C12` | `0x2FF3` |
| `FX_VERTICAL` | 11 | `0x6C9B` | `0x3016` |
| `FX_SOLID` | 3 | `0x9DA4` | `0x2FF8` |
| `AULA_FX_REACTIVE` | 12 | `0x64F1` | `0x301B` |
| `AULA_FX_RAIN` | 5 | `0x9B2B` | `0x2FE9` |
| `AULA_FX_TWINKLE` | 8 | `0xAC1C` | **none** — *choice*: reuse the other twinkle's table |
| `AULA_FX_SNAKE` | 10 | `0x8DDF` | `0x3011` |
| ~~`AULA_FX_SNAKE_RGB`~~ *(removed)* | `0x26` | `0x1D05` | **none** — *choice*: reuse snake's table, same engine (later removed as redundant) |
| `AULA_FX_RIPPLE` | 17 | `0x746A` | `0x302F` |
| `AULA_FX_KEYWAVE` | 15 | `0x82A5` | `0x3025` |
| `AULA_FX_VRAINBOW` | 16 | `0x9659` | `0x302A` |
| `AULA_FX_GAMING` | `0x20` | `0x95A4` | **none** — static image, *choice* |

Three rows out of thirteen are a design choice, noted as such in the code; the other ten are the
exact bytes from the dump.

**The frame gate accumulates milliseconds**, without division: each scan adds `38` and
subtracts the period when reached. Fractional periods are therefore honored —
rain at speed 0 falls every **3.16** frames, not 3.

**Decay becomes a constant, and it is the right one.** The factory renderer computes
`base × intensity >> 5`: intensity only carries **thirty-one** levels, and `0x64F1` subtracts
**one per rendered frame**. On our 0–255 plane, that equals `255 / 31 = 8`, independent of
speed — all response to `SPD_UP`/`SPD_DN` now goes through the period, just like in the
factory firmware. Decay is consequently gated on the frame, no longer on the scan.

##### What this architecture cannot render

The factory firmware redraws the entire panel on every frame and goes down to **1 ms**. Here, one frame costs a
full regeneration scan — fifteen 400 µs subframes plus one scan slot, six times, i.e.
**38.5 ms**. Any shorter period is clamped to one frame:

| Effect | Scans per frame, speeds 0 → 4 |
| --- | --- |
| `RAIN` | 3.16 · 2.63 · 2.11 · 1.32 · 1.00 |
| `SNAKE` | 3.16 · 2.37 · 1.84 · 1.18 · 1.00 |
| `RIPPLE` | 1.32 · 1.05 · 1.00 · 1.00 · 1.00 |
| the other nine | ≈ 1 across the board |

The gradient survives where the factory is slow, and flattens out where it is fast. **That is our
floor, not theirs.**

That is also why `led_speeds[]` continues to vary the **phase step** whereas the factory
keeps a fixed step: our frame rate can only vary by a factor of three, and without a variable step,
`SPD_UP` and `SPD_DN` would barely be noticeable. That point is a deliberate choice, documented in the code.

And the gain is real: at the slowest speed, the port previously advanced one frame every **616 ms**
versus the factory's **46 ms** — sixteen times too slow. It now runs at 38.5 ms.

#### The dispatch

```asm
0x1AE2  mov dptr,#0x08BB ; clr a ; movx @dptr,a ; inc dptr ; mov a,r7 ; movx @dptr,a
0x1AEA  jb  0x24.2, 0x1AF0        ; otherwise, nothing
0x1AED  ljmp 0x1DC2
0x1AF0  mov dptr,#0x0896 ; movx a,@dptr    ; a = current effect
0x1AF4  add a,#0xE0 ; jnz +3 ; ljmp 0x1DAB ; effect 0x20
0x1AFB  add a,#0xFA ; jnz +3 ; ljmp 0x1D05 ; effect 0x26
0x1B02  add a,#0x26                        ; a restored
0x1B04  cjne a,#0x12,$+3 ; jc 0x1B0C ; ljmp 0x1DC2   ; bound: 18 effects
0x1B0C  mov dptr,#0x1B13 ; mov r0,a ; add a,r0 ; add a,r0   ; a = 3 x idx
0x1B12  jmp @a+dptr
```

The chain `add a,#0xE0 / add a,#0xFA / add a,#0x26` sums to zero: the first two terms
test `0x20` and `0x26` without clobbering the accumulator. **It is not arithmetic, it is two
disguised `if` statements** — a compiler idiom that fools a quick reading.

#### Table `0x1B13` — eighteen handlers

Each handler compares the tick to the period, and only acts if `tick ≥ period`; otherwise it jumps
to `0x1DC2` (`ret`). When it acts, it calls the renderer and then jumps to the shared tail `0x1D69`,
which **resets the tick to zero**.

| idx | Handler | Threshold | Counter `0x097C` | Renderer |
| --- | --- | --- | --- | --- |
| 0 | `0x1B49` | immediate **100** | — | `0xEE6A` |
| 1 | `0x1B61` | immediate **10** | — | `0x746A`, step `0x08C3` bounded to 9 |
| 2 | `0x1B8D` | `0x08BB` | — | **`0x7C12`** |
| 3 | `0x1BAB` | `0x08BB` | — | `0x9DA4` |
| 4 | `0x1BC9` | `0x08BB` | ≥ 16 → `0x64F1` | `0x5BE9` |
| 5 | `0x1BF5` | `0x08BB` | ≥ 11 → `0x64F1` | `0x9B2B` |
| 6 | `0x1C21` | `0x08BB` | — | `0x60C9` |
| 7 | `0x1C3F` | `0x08BB` | ≥ 16 → `0x64F1` | `0x5BE9` |
| 8 | `0x1C6B` | — | — | `ljmp 0xAC1C` (trampoline) |
| 9 | `0x1DC2` | — | — | **`ret` — inactive effect** |
| 10 | `0x1C6E` | `0x08BB` | ≥ 16 → `0x64F1` | `0x8DDF` |
| 11 | `0x1C9A` | `0x08BB` | — | `0x6C9B` |
| 12 | `0x1CBC` | `0x08BB` | — | `0x64F1` |
| 13 | `0x1CDA` | `0x08BB` | ≥ 3 → `0x64F1` | `0x5BE9` |
| 14 | `0x1DC2` | — | — | **`ret` — inactive effect** |
| 15 | `0x1D51` | `0x08BB` | — | `0x82A5` |
| 16 | `0x1D71` | `0x08BB` | — | `ljmp 0x9659` |
| 17 | `0x1D8D` | `0x08BB` | — | inline body |

#### The eight missing engines — decoded

This page previously listed eight handlers as "undecoded". They are now decoded. The table above
names them; here is what they do, and what reading them revealed along the way.

| idx | Engine | Visible effect |
| --- | --- | --- |
| 5 | `0x9B2B` | **rain**: a droplet falls down each column, one row per frame; a randomly chosen column is relaunched when idle; the trail comes from the `0x64F1` fade |
| 8 | `0xAC1C` → `0x4EA9`/`0x62E6` | **twinkle**: randomly chosen keys set to full intensity |
| 10 | `0x8DDF` | **snake**: a dot traverses the grid in boustrophedon, reverses its horizontal direction at the edge and steps down one row, reverses its vertical direction at the top and bottom |
| 15 | `0x82A5` | **wave** over a second 128-hue palette, per-key phase advanced by one step per frame (`0xEDA2`) |
| 16 | `0x9659` | **vertical rainbow**: hue `+25` per row modulo 192, scrolling base — six rows span 150 of the 192 entries |
| 17 | `0x1D8D` | the concentric wave of engine 1 (`0x746A`), **parameter fixed at 9** — not a separate engine |
| `0x20` | `0x1DAB` → `0x95A4` | **gaming mode**: static image from `CODE 0xCA00`, red and green planes zeroed |
| `0x26` | `0x1D05` → `0x8DDF` | the snake in **color mode 7** (random hue) with brightness forced to 4 |

##### The two-plane rendering model

This is the key architectural finding, and it holds across all engines, not just the eight:

- **`XRAM 0x0428`** — the **base color** of each key, `col × 18 + row × 3 + component`.
- **`XRAM 0x0017`** — the **intensity** of each key, `col × 6 + row`, 126 bytes.
- **`fcn @ 0x62E6`** — the renderer: `base × intensity >> 5` into `0x0152`, which then goes out to PWM.

This is the exact model the port already used for the reactive engine alone. Hence the
structure adopted: **six effects share the same mechanics and differ only in their seeder**
(struck keys, droplets, random, snake, multicolor snake, rings).

##### `0x0428` is a mirror of `0x0152`

It was previously an open question whether these two 378-byte buffers had the same layout. They
do: `fcn @ 0x8B29` writes **the exact same color into both**, with the **same** indexing
`col × 18 + row × 3 + component`. The channel ↔ (row, color) mapping is therefore
verified across two independent write paths, not just one.

##### The "gaming" image confirms the entire matrix map

The blue plane at `CODE 0xCAFC` lights up nine positions: `(c0,r0)`, `(c2,r2)`, `(c1,r3)`, `(c2,r3)`,
`(c3,r3)`, `(c13,r4)`, `(c12,r5)`, `(c13,r5)`, `(c14,r5)`. Mapped onto
`layouts/default/layout.c`, these are **Esc, W, A, S, D, ↑, ←, ↓, →**. Nine coincidences on a
data path completely unrelated to matrix scanning: this is the best available confirmation
of the complete matrix layout.

##### Three copied tables

- **Secondary palette, `CODE 0x2D6D`** — 128 `(R,G,B)` triplets, 384 bytes. Distinct from the 192-entry
  color wheel. Its bounds are fixed by contiguity: the wheel ends at `0x2D6A`, the ring map
  starts at `0x2EED = 0x2D6D + 384`.
- **Wave rings, `CODE 0x2959`** — 9 groups of 13 IDs, `0xFF` for empty.
  The encoding is `col × 8 + row`: ring 0 is `0x3A` = (col 7, row 2), the center
  this page had already identified. **These are not distances** — ring 8 contains `(2,0)`,
  which no metric would place there. It is a hand-authored propagation order.
- **Presence map, `CODE 0xC500`** — six slots in the 6 × 15 grid have no key.
  Its row 4 permutes the columns, **in the exact same way** as row 4 of table `0x2EED`: two
  independent findings hitting the exact same row quirk.

##### The pseudo-random number generator, `fcn @ 0xA997`

32-bit Galois LFSR, right-shifting, mask `0xCC4C4ECE`, state in `XRAM 0x0F67`, re-seeded
to `0xA5A5` if it hits zero, sixteen steps per call; followed by `0x4D52` (signed division) for the
modulo. Six callers, including rain (`rand % 15`) and color mode 7 (`rand % 192`).

The port transcribes the **behavior** and substitutes the **generator**: reproducing the polynomial would not
yield the same sequence without the same seed and call order, and would cost stack space
inside the ISR for an observable property that simply amounts to "a random column".

##### Speed drives motion, not just hue

A trap in the port, not in the dump. SMK's `led_speeds[]` only advances the animation phase, so
without care `SPD_UP`/`SPD_DN` would change the *color* of the rain and snake rather than their
*pace*. The factory firmware, by contrast, gates all its rendering on `tick ≥ period`, where the period is pulled from
`CODE 0x2FE9` according to the speed.

The port therefore gates the seeder on a frame divider `16 >> speed` (16, 8, 4, 2, 1) and scales
the intensity plane decay accordingly, `4 << speed` (4, 8, 16, 32, 64). Their product stays
constant: the **trail keeps the same length** — roughly four keys — across all speeds,
and only the motion speeds up.

##### The phase field of effect 15 — the writer of `0x0E49` does not exist

This page previously listed "the writer of `XRAM 0x0E49`" as an open question. **There isn't one, and that is the
answer**: the plane is a *constant*, not a variable.

The search initially produced what looked like a dead end: across all 126 bytes of
`0x0E49`–`0x0EC6`, there is **only a single reference in the entire 64 KB image**, and it is the read by the
transposer `0xACAC`. Three addressing forms could still have hidden a write; all three are
ruled out:

| Form | Verdict |
| --- | --- |
| `MOV DPTR,#0x0E49` | no sites — the range `0x0E4A`–`0x0EC7` is not referenced anywhere |
| `ADD A,#0x49` / `ADDC A,#0x0E` | a single site, `0xACAC`, and it is a read |
| `MOVX @Ri` (page `P2`) | impossible: `P2` is only written twice, once in the bootloader, and the other (`0xA824`) zeroes the port at boot. `P2` is 0, so `MOVX @Ri` only ever sees `0x00xx` |

The boot sequence gives the answer. `startup_c51` (`0x9C74`) clears **all 4,096 bytes of XDATA**
(`0x9C7A`–`0x9C86`) and then jumps to the Keil initializer unpacker, which reads the table at `CODE 0x9FDE`.
That table has **only three records**:

| Record | Control | Size | Destination |
| --- | --- | --- | --- |
| `0x9FDE` | `0x41` | 1 B | `XDATA 0x08C4` |
| `0x9FE2` | `0x41` | 1 B | `XDATA 0x0C36` |
| `0x9FE6` | `0x60` | **126 B** | **`XDATA 0x0E49`**, data at `CODE 0x9FEA` |

The decoding is **self-validating**: the `0x00` terminator lands at the exact end of the third
record, at `0xA068`. No room for any offset misinterpretation.

The 126 bytes, reshaped as `[row][col]` with the stride of 21 used by `0xACA3`:

```
l0: 52 53 54 55 57 59 5c 62 69 73 78 7c 7f 01 03 | 05 06 07 08 51 51
l1: 50 50 51 52 53 55 56 5f 6c 78 7f 01 03 06 07 | 08 09 09 0a 4f 4f
l2: 4d 4e 4e 4f 4f 50 52 5a 7d 06 08 09 09 0a 0b | 0b 0b 0c 0c 4d 4d
l3: 48 47 46 45 44 43 42 34 17 11 10 0f 0f 0f 0e | 0e 0e 0d 0d 4a 49
l4: 45 44 43 42 40 3d 34 27 1b 18 16 13 13 12 12 | 12 11 10 0f 47 46
l5: 44 42 40 3e 36 30 29 1f 19 17 16 15 14 14 13 | 12 11 11 10 46 45
```

Values **1 to 127** — the exact range of an index into the 128-entry palette, confirming our
reading of the engine. And the field is **not uniform**: it is an **angular sweep**. The phase
increases along the first row until wrapping through zero between columns 12 and 13, and decreases
downward on the left. A wave that **rotates** rather than translating.

The port transposes the table once and for all at compile time (`aula_fx_keywave()`, 90 bytes
for the active columns) and computes `key_phase + frame_counter` — strictly
equivalent to the one-step-per-frame-per-key advance performed by `0xEDA2`, with zero state to maintain.

Two details that matter for rendering:

- **The transposer is called per effect, not once and for all.** `0x1AA8` is preceded by a
  `JNB 0x21` and followed by sibling branches at `0x1AB2`, `0x1AC4`, and `0x1AD0` that call `0xEFBE` and
  `0xEF85` with the same prologue: it is a dispatcher, one branch per effect, each reseeding its
  own plane. The field therefore restarts from the dump **on every selection** of the effect. The port resets the
  phase to zero in `fx_reset()` for the same reason.
- **The seam between column 14 and column 0 is normal.** The sweep closes back on itself
  within the 21-column space thanks to the six columns 15–20 (`51 51`, `4f 4f`, `4d 4d`…), which
  do not exist on this keyboard. Across fifteen columns, the phase jumps from `0x03`–`0x13` to `0x52`–`0x44`
  on wrap-around — and the factory firmware makes the exact same jump on this exact hardware. It
  is not a porting artifact.

**Effect 15 is therefore completely transcribed.** The diagonal approximation that the port previously assumed has been
removed.

Outside the table, the two special effects:

| Effect | Handler | What it does |
| --- | --- | --- |
| `0x20` | `0x1DAB` | immediate threshold **100** |
| `0x26` | `0x1D05` | `0x097C ≥ 5` → forces **`0x0D19 = 4`** (brightness) then `0x64F1`; thereafter immediate threshold **1** |

`0x64F1` appears seven times: it is the shared renderer, called either as a sub-step on the secondary
counter or as the main renderer (idx 12). Indices **9 and 14 are explicitly empty** —
two reserved, unimplemented effect slots.

**All eighteen handlers are now decoded and ported** (excluding the two empty ones). The
details of the last eight follow.

#### `0x1BA5`, in particular

This is the active line of the index **2** handler:

```asm
0x1B8D  mov dptr,#0x08BB ; movx a,@dptr ; mov r6,a   ; period, high byte
0x1B92  inc dptr ; movx a,@dptr ; mov r7,a           ; period, low byte
0x1B95  clr c
0x1B96  mov dptr,#0x0301 ; movx a,@dptr ; subb a,r7
0x1B9B  mov dptr,#0x0300 ; movx a,@dptr ; subb a,r6
0x1BA0  jnc 0x1BA5
0x1BA2  ljmp 0x1DC2       ; tick < period: nothing to do
0x1BA5  lcall 0x7C12      ; render effect 2
0x1BA8  ljmp 0x1D69       ; reset tick to zero
```

**What jumps to it:** the `jnc` at `0x1BA0`, when `tick ≥ period`. And what reaches the block: the computed
jump `jmp @a+dptr` at `0x1B12`, via entry 2 of the table at `0x1B13`. No direct `lcall` or `ljmp`
— which is why a cross-reference scan found nothing.

This closes the open question regarding `fcn.00007C12`: it is not an orphan routine,
it is **the rendering engine for effect index 2**, and it re-reads `0x0896` at `0x7C33` and `0x7C6F`
to look up its palette.

> **Ghidra correction.** `get_xrefs_to 0x7C12` reports a `COMPUTED_JUMP` coming from `0x7B24`,
> the `jmp @a+dptr` in `fcn.00007AAD`. That is false: the jump table for that instruction is at `0x7B25`, it has
> **thirteen entries** (`0x7B25`–`0x7B48`), and **none of them equals `0x7C12`** — they all point
> between `0x7B4C` and `0x7B8A`. Ghidra's table recovery overflows, and it is this invented
> target that causes it to merge `0x7AAD` and `0x7C12` into a single function.

### The two rendering engines — `0x64F1` and `0x7C12`

The scheduler calls sixteen rendering routines. Two of them carry the two animation
families, and they can finally be read in C.

#### First, the helper: `fcn.00004E39`, the Keil pointer helper

```asm
0x4E39  mul ab ; add a,DPL ; mov DPL,a ; mov a,B ; addc a,DPH ; mov DPH,a ; ret
```

**`dptr += a × b`**, in seven bytes, called **94 times**. All array indexing with a non-trivial
stride goes through it. The decompiler renders it as `FUN_CODE_4e39(3, row)` in the middle of an
address expression: it is not a domain function call, it is the `[]` operator. Naming it
`dptr_add_mul` makes all the RGB decompilations readable at once.

#### `0x64F1` — per-key decay

Double loop over the entire matrix: `0x0EE2` = column **0..14**, `0x0EE3` = row **0..5**.

| Array | Address | Contents |
| --- | --- | --- |
| exclusion mask | `0x0BFF + col` | one byte per column; the key is processed if bit `1 << row` is **zero** |
| per-key color | `0x0428 + col × 18 + row × 3` | three bytes; **378 B allocated** (21 × 18), `0x0428`–`0x05A1` |
| per-key intensity | `0x0017 + col × 6 + row` | one byte; **126 B allocated** (21 × 6) |

For each selected key:

```c
component = (color[i] * intensity) >> 5;     /* i = 0,1,2 */
rgb_key_suppressed(row, col);
if (!suppressed) { rgb_apply_brightness(); rgb_pixel_write(); }

if (g_palette_effect_idx == 13)  intensity = (intensity < 2) ? 0 : intensity - 2;
else                             if (intensity) intensity -= 1;
```

**This is the reactive engine**: each key holds its own color and its own intensity, and
the intensity **decays by one step per frame** — by two steps for effect 13, which therefore fades out twice
as fast. The `>> 5` makes the intensity a **32-level** scale.

The three arrays are only reached via computed pointers: `dptr_refs` on `0x0428`, `0x0017`,
or `0x0BFF` returns **nothing**. They were only found through decompilation.

> **Correction.** I had previously written `0x0C17` for the intensity array. It is **`0x0017`**:
> `mul ab (b=6) ; add a,#0x17 ; clr a ; addc a,#0x00` — the high byte is **zero**. The `0x0C`
> came from the neighboring exclusion mask (`add a,#0xFF ; addc a,#0x0C`), which really is at `0x0BFF`.
> The decompiler's `CONCAT11` renders these two cases almost identically; only the constants in the
> disassembly separate them.

> **Corrected size.** I had reported 270 and 90 bytes, based on the loop in `0x64F1` which only
> iterates over fifteen columns. The actual allocation is **twenty-one** columns, just like the LED table:
> `rgb_clear_key_state` (`0x9078`) clears both of them up to `0x15`. The color buffer therefore ends
> at `0x05A1`, **right before** the PWM frame buffer at `0x05A2` — the two are contiguous.

#### `0x7C12` — scrolling gradient (effect index 2)

A single loop over columns **0..14** — no per-key state. The color is chosen once
per column, according to the color mode `0x011C`:

| `0x011C` | Source |
| --- | --- |
| `< 7` | the effect palette: `CODE 0xC800 + [0x0896] × 21 + mode × 3` |
| `== 7` | the **hue wheel**: `CODE 0x2B2A + phase × 3` |
| `> 7` | no color written |

The wheel has **192 three-byte entries** (`0x2B2A`–`0x2D69`), and the phase advances by **11 per
column** wrapping modulo 192 (`add a,#0x0B` then, if `≥ 0xC0`, `add a,#0x40`). That is what
produces the rainbow spread across the keyboard.

Next, `fcn.0000A17B([0x08C3], 0xC480)` applies the scroll position, followed by
`rgb_apply_brightness`. The inner loop iterates over the six rows and only draws if the position
exists:

```c
if (CODE[0xC500 + col * 6 + row] != 0xFF) { ... }
```

##### `CODE 0xC500` — the key presence map

Fifteen columns of six bytes, `0xFF` = no key at this position.

```
col  0 : 00 01 02 03 04 05        col  8 : 40 41 42 43 4c 45
col  1 : ff 09 0a 0b 14 0d        col  9 : 48 49 4a 4b 54 4d
col  2 : 10 11 12 13 1c 15        col 10 : 50 51 52 53 5c 55
col  3 : 18 19 1a 1b 24 ff        col 11 : 58 59 5a 5b 64 ff
col  4 : 20 21 22 23 2c ff        col 12 : 60 61 62 63 0c 65
col  5 : 28 29 2a 2b 34 2d        col 13 : 68 69 6a 6b 6c 6d
col  6 : 30 31 32 33 3c ff        col 14 : 70 71 72 73 74 75
col  7 : 38 39 3a 3b 44 ff
```

**Six holes out of ninety positions, leaving 84 slots**. The values follow
`col × 8 + row`, with visible exceptions (`0x14` in col 1, `0x0C` in col 12) — these are
relocated positions.

#### The two color tables do not have the same byte order

Verified instruction by instruction, not inferred from C:

| Table | `+0` | `+1` | `+2` |
| --- | --- | --- | --- |
| palette `0xC800` | `0x011D` | `0x0000` | `0x0E25` |
| per-key color `0x0428` | `0x011D` | `0x0000` | `0x0E25` |
| hue wheel `0x2B2A` | `0x0E25` | `0x0000` | `0x011D` |

**The wheel is stored in reverse order compared to the palette.** The middle byte is the same in all three cases.
The next section establishes that `0x011D` is red, `0x0000` is green, and `0x0E25` is blue: the
first two tables are therefore in `(R,G,B)` order and the wheel is in `(B,G,R)` order.

#### Which one is red — settled

The question remained open: three components, no proof of their color. The complete chain
closes it, with every link verified in the disassembly.

**Link 1 — `rgb_apply_brightness` (`0xA0F0`).** Three multiplications, each followed by a
division by 80 (`mov r5,#0x50 ; lcall math_div16by8`):

| Source | 16-bit result |
| --- | --- |
| `0x011D` | `0x0E46:0x0E47` |
| `0x0000` | `0x0E42:0x0E43` |
| `0x0E25` | `0x0E3F:0x0E40` |

The gain comes from `CODE 0x2937 + [0x0D19]`, and is at most 80: the quotient therefore fits in a
single byte, the low byte.

> **Correction.** An earlier note paired `0x0000, 0x011D, 0x0E25 → 0x0E46, 0x0E42, 0x0E3F`,
> in that order. That is wrong: `0x011D` goes to `0x0E46`, `0x0000` to `0x0E42`. The first two
> are crossed — and this is the exact pairing on which everything else depends.

**Link 2 — `rgb_pixel_write` (`0x761E`).** It reads only the low byte of each
quotient, and stores them at three consecutive offsets:

```asm
0x765B  add a,#0x52   ; dptr = 0x0152 + col x 18, then += row x 3
0x7678  mov a,r3      ; r3 = [0x0E47]   -> offset +0
0x7684  add a,#0x53   ; +1
0x7695  mov a,r5      ; r5 = [0x0E43]   -> offset +1
0x76A4  add a,#0x54   ; +2
0x76C1  mov a,r7      ; r7 = [0x0E40]   -> offset +2
```

**Link 3 — the destination table is already known.** `XRAM 0x0152 + col × 18 + row × 3`,
i.e. 21 × 18 = **378 bytes** (`0x0152`–`0x02CB`): the exact table populated by host command `0x08`
sub-index 2, and from which opcode `0x42` reads back 378 bytes. The OpenRGB driver writes
`buf[0x08 + i*3] = R, G, B` to it with `i = col × 6 + row`.

**Conclusion.** Offset `+0` of this table is red, therefore:

| Address | Component | Scaled output |
| --- | --- | --- |
| **`0x011D`** | **red** | `0x0E46:0x0E47` |
| **`0x0000`** | **green** | `0x0E42:0x0E43` |
| **`0x0E25`** | **blue** | `0x0E3F:0x0E40` |

The labels `g_color_r` and `g_color_g` were therefore **swapped** in the Ghidra setup;
fixed in `tools/gh_setup.py`, along with `g_scaled_r` / `g_scaled_g`.

From this follows the byte order of the three tables:

| Table | Actual order |
| --- | --- |
| palette `0xC800` | **(R, G, B)** |
| per-key color `0x0428` | **(R, G, B)** |
| hue wheel `0x2B2A` | **(B, G, R)** |

The "red green blue yellow magenta cyan white" table of the factory palette is therefore **correct**.

##### Cross-check: the wheel read as (B, G, R) is a hue circle

```
phase   0  ->  R=  0 G=  1 B=255     blue
phase  32  ->  R=  0 G=255 B=248     cyan
phase  64  ->  R=  1 G=255 B=  0     green
phase  96  ->  R=255 G=248 B=  0     yellow
phase 128  ->  R=255 G=  0 B=  1     red
phase 160  ->  R=248 G=  0 B=255     magenta
```

Six segments of 32 steps, and **all 192 entries have a maximum of 255** — constant saturation,
the signature of a hue wheel. With a step of 11 per column, the fifteen columns cover
165 of the 192 phases: the gradient makes almost a full turn across the width of the keyboard.

### The other two engines — `0xEE6A` and `0x746A`

#### `0xEE6A` — effect index 0 is "off"

Twelve bytes, two calls:

```asm
0xEE6A  clr a ; mov r3,a ; mov r5,a ; mov r7,a
0xEE6E  lcall rgb_fill_solid        ; (0, 0, 0)
0xEE71  clr a ; mov r7,a
0xEE73  ljmp  rgb_clear_key_state   ; (0)
```

**`rgb_fill_solid` (`0xADA2`)** takes a triplet in `r7`/`r5`/`r3`, places it in `0x0EE3`–`0x0EE5`,
then sweeps **21 columns × 6 rows** and writes each position via `rgb_key_suppressed` followed by the
write chain (`0x7631`). It resets `RSTSTAT` (SFR `0xB1`) to zero **on every iteration** — a
watchdog kick, as the 126-position loop is long enough to need it. Eight
other call sites invoke it, all grouped at `0x8736`–`0x8785`: those are the solid colors for the
indicator keys.

**`rgb_clear_key_state` (`0x9078`)** writes its argument into **both arrays of the reactive
engine** — the three color bytes at `0x0428 + col × 18 + row × 3` and the intensity at
`0x0017 + col × 6 + row` — across 21 columns and 6 rows.

> So effect index 0 **does not animate anything**: it turns off the LED table and zeroes out the state of the
> reactive engine every 100 frames. It is the "backlight off" mode, reasserted
> periodically rather than set once.

#### `0x746A` — concentric wave

Called with `r7 = [0x08C3]`, the radius, which the index 1 handler increments on each frame and
**clamps to 9**. This radius becomes the upper bound of the outer loop: `[0x0EE3]` runs from 0 to `radius − 1`.

Each iteration reads a **group** from the table `CODE 0x2959`, thirteen bytes per group, `0xFF` = empty
slot. The values are **packed positions `col × 8 + row`** — the same encoding as the
`0xC500` map.

| Group | Positions | Meaning |
| --- | --- | --- |
| 0 | 1 | `c7,r2` — **the center** |
| 1 | 6 | the immediate ring |
| 2 | 11 | |
| 3–7 | 11 to 13 | |
| 8 | 12 | the edge |

Nine groups, `0x2959`–`0x29CD`. **It is a wave radiating from the center of the keyboard** (column 7,
row 2) and expanding by one ring per frame up to the edge. The hue wheel phase advances by
**13 per ring**, coloring each ring differently.

The two tables `CODE 0x2DED` and `CODE 0x2F6B`, indexed as `(v & 7) × 21 + (v >> 3)`, are the same
as those used by `0x7C12` — converting position → coordinates passed to `rgb_key_suppressed`.

#### The two engines do not read the wheel in the same order

Verified in the disassembly on both sides:

| Function | `+0` | `+1` | `+2` | Order |
| --- | --- | --- | --- | --- |
| `0x746A` (`0x74AD`, `0x74C2`, …) | `0x011D` | `0x0000` | `0x0E25` | **(R, G, B)** |
| `0x7C12` (`0x7CA4`, `0x7CB9`, `0x7CD1`) | `0x0E25` | `0x0000` | `0x011D` | **(B, G, R)** |

Yet their *palette* branches are **identical** — both store `+0` into `0x011D`
(`0x7508` and `0x7C4F`). Only the wheel branch diverges.

> Two possible readings: either this is deliberate and one of the two effects displays the rainbow
> with red and blue swapped, or it is an oversight in the original firmware. **The dump alone
> cannot settle this** — one would have to see both effects running. I point it out because
> anyone reimplementing these effects will reproduce one or the other without knowing it.


#### `0x0EE4` is not a shared flag

`0x64F1` writes `1` to it and uses it as a **mask seed** (`1 << row`); `0x7C12` writes `0` to it
and then uses it as the **hue wheel phase**. Same byte, two unrelated uses — variable
reuse by the compiler. Concluding that it was a shared state flag would have been a
natural and mistaken assumption.

#### `fcn.0000598F` — the suppression filter

Called by both engines with `(row, col)`, it returns a "do not light" flag in `R4`.
Its body is a long sequence of `if (row == a && col == b && condition) return`,
with conditions testing `g_transport`, `0x0328` (current Bluetooth slot), `0x030A`, `0x09BA`, and
about ten IDATA bits. **This is the indicator-key logic**: keys that display the
paired slot or connection mode are excluded from the animation so they can be driven separately.

#### Another boundary discrepancy

r2 gives `0x64F1` **eleven bytes** — it cuts off at `0x64FB` because `0x64FC` is the target of a jump
from `0x66F5`. Ghidra gives `0x64F1`–`0x66F8`, i.e. **520 bytes**, and Ghidra is right:
the body continues without a `ret`. This is the exact opposite of the `0x7AAD` case, where r2 was right. **Neither
tool is authoritative; you have to follow the control flow.**

### `0x9DA4` and `0x5BE9` — solid color cycling and the cellular automaton

#### First, `rgb_scroll_step` (`0xA3F4`), used by both

```asm
0xA3F4  jnb 0x24.3, 0xA42D        ; bit selects the DIRECTION
        ; forward direction, step depends on effect [0x0896]:
        ;   0x0A        -> +20
        ;   0x0B, 0x11  -> +2
        ;   otherwise   -> +1
0xA421  mov dptr,#0x08C3 ; movx a ; clr c ; subb a,r7
0xA427  jc  0xA462 ; clr a ; movx @dptr,a     ; if position >= r7, wrap to 0
```

So **`rgb_scroll_step(n)` advances `g_scroll_pos` (`0x08C3`) modulo `n`**, by an effect-specific
step, and `0x24.3` reverses the direction — that is the "direction" setting in the configuration block.
`0x9DA4` and `0x746A` call it with `0xC0` (192, the wheel size), and `0x7C12` with `0x80`.

#### `0x9DA4` — whole matrix in a single color (effect index 3)

One hundred forty-nine bytes, and the simplest structure of the four engines seen so far:

```c
rgb_scroll_step(0xC0);
hue = wheel[g_scroll_pos];          /* CODE 0x2B2A + pos * 3, (R,G,B) order */
rgb_apply_brightness();             /* only once, outside the loop */
for each col 0..14, each row 0..5:
    if (CODE[0xC500 + col*6 + row] != 0xFF)
        rgb_key_suppressed(...) ; if not suppressed: rgb_pixel_write()
```

**The color is computed once, before the loop** — the entire keyboard takes on the same hue, which
cycles through the wheel. This effect **never checks `0x011C`**: it ignores the color mode and
always uses the wheel.

#### `0x5BE9` — a four-direction propagation automaton (effects 4, 7, and 13)

Two hundred sixty-five bytes, shared by three handlers (`0x1BEF`, `0x1C65`, `0x1D00`).
It does not draw a static shape: it **evolves a state** across **four bitmaps** of fifteen
bytes each, one byte per column, six active bits per byte (one row per bit).

| Base | Propagation direction | Operation, per frame |
| --- | --- | --- |
| `0x0001` | to the **right** | `A[0x0F − c] = A[0x0E − c]`, edge cleared to zero |
| `0x013B` | to the **left** | `B[c − 1] = B[c]`, edge cleared to zero |
| `0x08C6` | **upward** | `C[c] >>= 1` |
| `0x02E8` | **downward** | `D[c] <<= 1` |

The base addresses are confirmed in the disassembly (`add a,#0xE8 ; addc a,#0x02`, `mov a,#0xC6 ; addc a,#0x08`,
`mov a,#0x3B ; addc a,#0x01`) — the C output alone was not enough, as the borrow adjustment in `CONCAT11`
is easy to misread.

The frame unfolds in three phases:

1. **Draw** — union of the four bitmaps; for each bit set, the position tables
   `CODE 0x2DED` and `CODE 0x2F6B` yield the coordinates, followed by one of two drawing paths depending on an
   IDATA bit: either `fcn.00005114(1, …)` alone, or the three calls `0x5114(0, …)`, `0xAEB3(0, …)`, and
   `0x7108(…)`.
2. **Propagate** — the four operations from the table above, plus clearing the two edges
   (`0x0001` and `0x0149`).
3. **Recombine** — `A[c] |= C[c] | D[c]` and `B[c] |= C[c] | D[c]`, which launches the vertical
   wavefronts back out to the left and right: the propagation becomes **diagonal**.

If no bit was set (`0x0EE7` remains 1), an IDATA flag is raised — the animation signals
that it has died out.

##### Effect 13 is self-feeding

```c
if (g_palette_effect_idx == 13 && ++[0x08BE] > 5) {
    [0x08BE] = 0;
    bit = alternate(1, 0x20);                    /* row 0 or row 5 */
    [0x0007] |= bit; [0x0141] |= bit;            /* column 6 of all four bitmaps */
    [0x02EE] |= bit; [0x08CC] |= bit;
}
```

Every six frames, a seed is injected **at column 6**, alternating between row 0 and
row 5. The other three effects sharing this engine do not have this injection: their seed
comes from elsewhere — presumably from a keypress. **This is the engine for ripples propagating
outward from keys**, and effect 13 is its self-running variant.

> The four `+6` addresses (`0x0007`, `0x0141`, `0x02EE`, `0x08CC`) land at `base + 6` for all
> four bases: a cross-check that confirms the base address readings.

### `0x60C9` and `0x6C9B` — per-key phase and per-column gradient

#### The common helper: `fcn.0000EDA2`

```asm
0xEDA2  jnb 0x24.3, 0xEDAF
        inc r5 ; if r5 >= r7 -> r5 = 0          ; forward direction
0xEDAF  dec r5 ; if underflow -> r5 = r7 - 1    ; backward direction
```

**Advances a value by one step, modulo `r7`, in the direction given by `0x24.3`.** It is
`rgb_scroll_step` applied to an arbitrary byte instead of the global position — and it uses the same
direction bit, so the user setting affects both.

#### `0x60C9` — each key has its own phase (effect index 6)

Five hundred forty-one bytes. A 15 × 6 double loop, and for each key:

```c
phase = state[0x0017 + col*6 + row];
EDA2(phase, mode == 7 ? 0xC0 : 0x60);      /* advance by one step, modulo */
state[0x0017 + col*6 + row] = phase;
g_scroll_pos = phase;
color = (mode == 7) ? wheel[phase]                     /* modulo 192 */
                    : palette[fx][mode] then gain(0x08C3);   /* modulo 96 */
if position < 15: rgb_key_suppressed(...) ; else nothing
    -> rgb_apply_brightness() ; write via 0x7619
```

**The array at `0x0017` is therefore not "intensity" in general**: it is a per-key state
byte whose meaning depends on the effect. `0x64F1` stores a decaying intensity there; `0x60C9`
stores a rotating phase there. Same array, two meanings — just like `0x0EE4` above.

Visual result: each key cycles through the wheel at its own offset, and all of them advance by one step
per frame. A shimmer, not a scroll.

#### `0x6C9B` — per-column gradient (effect index 11)

Four hundred fifty-four bytes, two nearly identical halves selected by `0x011C`:

```c
rgb_scroll_step(mode == 7 ? 0xC0 : 0x60);   /* once */
phase = g_scroll_pos;
for each column 0..14:
    color = (mode == 7) ? wheel[phase] : palette[fx][mode] then gain(phase);
    phase += (mode == 7) ? 7 : 10;   wrapping at 0xC0 / 0x60
    rgb_apply_brightness();
    for each row 0..5: if position < 15 -> 0x5968 then, if not suppressed, 0x7616
```

This belongs to **the same family as `0x7C12`** — one color per column, with the phase advancing by a fixed step
from one column to the next — but with a different step size:

| Effect | Function | Step per column | Modulo |
| --- | --- | --- | --- |
| 2 | `0x7C12` | 11 | 192 |
| 11 | `0x6C9B` | 7 (wheel) / 10 (palette) | 192 / 96 |

Across fifteen columns: `0x7C12` covers 165 phases out of 192, while `0x6C9B` covers 105 — a tighter
gradient, less than a full turn.

#### By the way: the entry-point ladder of pixel writing

`0x6C9B` calls neither `rgb_key_suppressed` nor `rgb_pixel_write` directly, calling `0x5968` and
`0x7616` instead. These are not separate routines: they are **higher entry points into the
same functions**.

| Entry | What it does before falling through to the next |
| --- | --- |
| `0x7616` | `mov dptr,#0x0EE4` — reads coordinates from `0x0EE4`/`0x0EE5` |
| `0x7619` | `movx a,@dptr ; inc dptr` — caller has already set `dptr` |
| `0x761B` | `mov r7,a ; movx a,@dptr ; mov r5,a` |
| `0x761E` | loads all three scaled components |
| `0x7631` | loads only the third component |
| `0x7636` | the body: bounds check, address calculation, write |

Each caller enters at the level matching what it already holds in registers. This is a classic space-saving
idiom in C51 — and it is **why r2 slices this area into half a dozen two- or three-byte
"functions"**: they are entry labels, not separate routines.
The same explanation applies to `0x5968`, which precedes `rgb_key_suppressed` and prepares the coordinate read
from `CODE 0x2F6B` for it.

#### And the wheel, again

`0x60C9` and `0x6C9B` both read `+0` → `0x011D`, i.e. **(R, G, B)** — verified at `0x6CC3`.
That makes **five** engines using `(R,G,B)` against **a single one**, `0x7C12`, using `(B,G,R)`. The hypothesis of
a firmware oversight clearly wins out over that of an intentionally reversed
rainbow.

> **This point will never be settled from the dump alone, and we can now explain why.** All 192
> triplets at `0x2B2A` have been extracted and inspected: the first channel starts at 255 while the
> second ramps up, then the first ramps down, then the third ramps up, and so on — a perfectly regular
> hue circle, with non-linear ramps (1, 3, 5, 7, 10, 14, 18, 22, 26, 32, 38,
> 44…), i.e. perceptually weighted.
>
> However, **a hue wheel is symmetric under swapping the outer channels**. Read as `(R,G,B)`, the
> table traverses red → yellow → green → cyan → blue → magenta; read as `(B,G,R)`, it traverses
> the exact same circle in the opposite direction. Both are valid hue wheels and **no structural
> property of the data distinguishes them**: this is not a lack of information about this table,
> it is an intrinsic property of the mathematical object.
>
> The question is therefore not "which colors" but "which way the rainbow rotates", and
> the cost of getting it wrong is limited to the direction of rotation. The port adopts `(R,G,B)`, matching
> five of the six engines.
>
> **A second, weaker but independent argument points the same way.** The fixed-color
> palette at `CODE 0xC800` is read by the same engines, in the same format, and contains
> `ff 00 00 · 00 ff 00 · 00 00 ff · ff ff 00 · ff 00 ff · 00 ff ff · ff ff ff`. The *set* of seven
> colors is also symmetric under R↔B swap — so that alone does not settle it either. But their
> **order** speaks: read as `(R,G,B)` it is red, green, blue, then the three pairwise secondary colors,
> then white — the canonical enumeration. Read as `(B,G,R)`, the same table gives blue, green, red,
> cyan, magenta, yellow, white: an order nobody writes spontaneously. That is a convention,
> not a proof; it merely adds its weight to the engines' vote.

#### Color selection — all eight modes, ported

The factory renderer does not always go through the hue wheel. Each effect carries a 4-bit **color mode**,
`b1[3:0]`, latched into `XRAM 0x0897`, and the engines can be summed up in a single line
(`0x60C9`, `0x6C9B`):

```c
color = (mode == 7) ? wheel[phase] : palette[effect][mode];
```

- **modes 0 to 6** — the seven fixed colors at `CODE 0xC800`, twenty-one bytes per effect. Notable
  fact: **all eighteen effects carry the exact same twenty-one bytes**. Indexing
  by effect exists in the data format but serves no purpose; a single seven-color table suffices.
- **mode 7** — rainbow, the only mode that reads the hue wheel.

Previously, the port only rendered mode 7. All eight modes are now supported under the `CLR_NEXT` keycode. The mode
lives in `user_settings.ul_effect`, the byte reserved in the shared struct for underglow
lighting — which this keyboard lacks: reusing it avoids growing `user_settings_t`, which would
reset **all** saved settings back to defaults on first boot since `nvm.c`
checks the struct length.

Direct consequence: `FX_SOLID` no longer renders a hardcoded white — an invention now removed —
but the selected color, or the hue wheel at the current phase in rainbow mode. White remains available as
mode 6.

#### The three color tables, verified byte for byte

| Table | Address | Size | Role |
| --- | --- | --- | --- |
| hue wheel | `CODE 0x2B2A` | 192 × 3 = 576 B | rainbow, mode 7 |
| second wheel | `CODE 0x2D6D` | 128 × 3 = 384 B | per-key wave (effect 15) |
| fixed colors | `CODE 0xC800` | 7 × 3 = 21 B | modes 0 to 6 |

All three are copied **verbatim** into the port, and a byte-for-byte comparison against
`assets/f75_full_jtag.bin` passes for all three. The second wheel closes the gap cleanly:
`0x2D6D + 384 = 0x2EED`, the exact base address of table A of the phase field — two independent
findings that meet with neither overlap nor gap.

##### `CODE 0x2D6D` has only a single reader

Searching the entire image for the computed-addressing pattern — `add a,#0x6D` followed by
`addc a,#0x2D`, the only way to reach this base — finds **one and only one site: `0x8325`**,
inside the wave engine `0x82A5`. The port uses it in the exact same place and nowhere else. It
is therefore not a general-purpose palette, but **the dedicated wheel for a single effect**.

The engine confirms the size a second way: it advances the per-key phase via
`0xEDA2(phase, 0x80)` — **modulo 128**, the exact number of entries in the table. The port
wraps with `& 127`.

And the three bytes separating the two wheels, `CODE 0x2D6A` = `ff ff ff`, are read by
no one: `0x2B2A + 192 × 3 = 0x2D6A`, `0x2D6A + 3 = 0x2D6D`. An isolated white triplet between the two tables
that no modulo-192 index can ever reach.

##### Tables `0x2EED` and `0x2F6B` — the spatial map

Both tables consist of six rows of twenty-one columns, indexed as `row × 21 + col`, and the
wave engine reads them as follows: its loop iterates `col` from 0 to 14, computes the effect on that
index, and then **writes the pixel at column `A[row][col]` and row `B[row][col]`**.
They therefore map the LOOP position to the OUTPUT position.

Extracted from the dump, they are unexpectedly simple:

```
table A, CODE 0x2EED
  r0 :  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19 20
  r1 :  same
  r2 :  same
  r3 :  same
  r4 :  0 12  1  2  3  4  5  6  7  8  9 10 11 13 14 15 16 17 18 19 20   <<<
  r5 :  same

table B, CODE 0x2F6B
  r0 : twenty-one zeros ... r5 : twenty-one fives
```

- **`B` is simply the row index**: `B[row × 21 + col] == row` across all 126
  positions, verified. It transforms nothing.
- **`A` is the identity on five rows**, while **row 4 permutes**: loop position 1 outputs
  to column **12**, and loop positions 2 through 12 output to columns 1 through 11.
- **Exactly 90 of the 126 entries in `A` are less than 15** — the six rows of fifteen
  columns of the matrix. The six columns from 15 to 20 are padding, and the `< 15` check in the
  engine is there for them; since the loop stops at 14, it never triggers in this firmware.

Column 12 of row 4, placed spatially between columns 0 and 1, is **the slot for the
extra key on ISO layouts** — the one next to Left Shift. The map reserves space for it
even when the compiled layout does not populate it.

> **Two factory tables, two different spaces.** `CODE 0xC500` also has a permutation on its
> row 4, and it is not the same one. This is not a contradiction: `0xC500` converts a **scan
> index** into a key identifier for keypress handlers, whereas `0x2EED` converts a
> **render loop position** into an output column. Two conversions, two uses, no reason
> for them to coincide.

##### What the port does with it

The geometry generated by `utils/led_geometry_gen` for this keyboard assumes a **uniform** grid:

```c
static const __code uint8_t axis_x[15] = {0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238};
```

— meaning electrical column = spatial column. Table A shows that this is false on row 4.

Because the port iterates over the **output** column, it needs the **inverse** of table A. That inverse is
transcribed into `aula_fx_render_col()`, constructed mechanically and verified in both directions against
the dump, and it now feeds `led_effect_index()` for SMK's three geometric effects —
radial, horizontal gradient, and vertical gradient. Without it, a key on row 4 was painted in the
wrong place.

**Table B is not transcribed**, and that is a finding, not a shortcut: copying 126 bytes
that have been verified to equal the row index would be dead data. The row is passed through
as-is.

##### Resolved: `0xC500` IS the inverse of table A

The caveat noted above — "two factory tables, two spaces" — falls away. `CODE 0xC500` does not have
90 entries, but **126**, and its bound can be read directly in the code: the key queue at `XRAM 0x039F` only
accepts an entry if it is **less than 126** (`0x1670`), before storing it in
`[0x0EE0]` and setting `[0x0EE1] = 0`. The port had only copied the first 90 entries.

When read across all 126 entries, indexed as `col × 6 + row`, the table describes **twenty-one columns** —
the exact same width as tables A and B — and goes up to key ID `0xA4` at column 20:

```
c 0: 00 01 02 03 04 05        c14: 70 71 72 73 74 75
c 1: .. 09 0a 0b 14 0d        c15: 78 79 7a 7b 7c 7d   <<< beyond this keyboard
c 2: 10 11 12 13 1c 15        c16: 80 81 82 83 84 85
...                           c20: .. a1 a2 .. a4 ..
```

**This keyboard only has fifteen columns**; the next six belong to a wider model from the
same family. That is exactly what the `< 15` check in the rendering engine tests — a guard which, because the loop
stops at 14, never triggers here.

And across the fifteen active columns, the verification is conclusive:

| Check | Result |
| --- | --- |
| `id >> 3` against the inverse of table A | **identical** across all 90 positions |
| `id & 7` against the row itself | **identical** — the same result as table B |
| present IDs | **84, all distinct**: a bijection onto the physical keys |
| presence mask by index vs. by ID | **identical** — the permutation never moves a missing key |

`0xC500` and `0x2EED` are therefore not two competing maps; they are **a map and its inverse**:
`0xC500` maps from electrical column to spatial column, and `0x2EED` maps the way back.

#### The phase field `0x9FEA`, and its two siblings

##### `0x9FEA` — verified, and its transposer decoded

The transposer at `0xACA3` can be read instruction by instruction, unambiguously fixing both layouts:

```asm
0xACA3  r7 = 0 ; r6 = 0
0xACA7  a = r6 ; b = 21 ; mul ab ; dptr = 0x0E49 + r6*21 ; dptr += r7
0xACBF  r5 = [dptr]                          ; source: row*21 + col
0xACC1  a = r7 ; b = 6 ; mul ab ; dptr = 0x0017 + r7*6 ; dptr += r6
0xACD9  [dptr] = r5                          ; destination: col*6 + row
0xACDB  inc r6 ; cjne r6,#6  -> loop         ; r6 is the ROW, 0..5
0xACE0  inc r7 ; cjne r7,#21 -> loop         ; r7 is the COLUMN, 0..20
```

Thus **`CODE 0x9FEA` is laid out as `row × 21 + col`** and the plane at `0x0017` is laid out as
`col × 6 + row`: the transposer does literally what its name says. The port transposes at
compile time, and its 90 values are **byte-for-byte identical** to
`CODE[0x9FEA + row × 21 + col]` across the fifteen active columns.

The Keil init record is self-validating: its header is `7e 0e 49` — **126 bytes to `0x0E49`** —
and the `0x00` terminator of the table lands at `0xA068`, which is `0x9FEA + 126`. Zero room
for an offset error.

##### Two siblings, and the seeder that chooses between them

`0x9FEA` is not the only phase field. The seeder for factory effect **6** — the shimmer
at `0x60C9` — is `0xA791`, and it loads the `0x0017` plane from **one of two tables, depending on the
color mode** `[0x011C]`:

```asm
0xA795  a = [0x011C] ; cjne a,#7 -> 0xA7AA
0xA79C  dptr = 0x2A2E + col*6 + row          ; mode 7: rainbow
0xA7AA  dptr = 0x2AAC + col*6 + row          ; modes 0-6: fixed color
0xA7DD  [0x0017 + col*6 + row] = value       ; NO transposition here
```

Both tables are 126 bytes long and are **already in the destination layout** — unlike
`0x9FEA`, they do not need the transposer.

**And their value ranges validate the moduli that this page had identified separately:**

| Table | Mode | Values | Modulus applied by the engine |
| --- | --- | --- | --- |
| `CODE 0x2A2E` | 7, rainbow | 1 → **191** | `0xC0` = 192 — i.e. **max + 1** |
| `CODE 0x2AAC` | 0 to 6, fixed | 0 → **95** | `0x60` = 96 — i.e. **max + 1** |

The seeder thus picks the table whose range matches the exact modulus that the engine will
apply. Two independent findings — the engine's `0xEDA2(phase, 0xC0 / 0x60)` call and the contents of the
tables — close the loop on each other.

**Their shape is a chevron.** Rows 0 to 2 share one ramp, and rows 3 to 5 share the mirrored
ramp:

```
0x2A2E   r0-r2 :   1   6  11  16  21 ...  99      (+5 per column)
         r3-r5 : 191 190 188 185 180 ... 103      (-5 per column)

0x2AAC   r0-r2 :   0   7  14  21  28 ...  95      (+7 per column)
         r3-r5 :  84  77  70  63  56 ...          (-7 per column)
```

Note: `0x2A2E` sweeps all twenty-one columns in a single chevron — its two halves
meet around 99 and 103 at column 20. `0x2AAC`, by contrast, **restarts at zero at column 15** on
its upper half while its lower half continues descending. Observed, not explained.

##### The data block is completely contiguous

The eight tables in this region follow one another without a single byte of padding or overlap:

| Address | Size | Contents | End |
| --- | --- | --- | --- |
| `0x2A2E` | 126 | effect 6 phase field, mode 7 | `0x2AAC` |
| `0x2AAC` | 126 | effect 6 phase field, fixed color | `0x2B2A` |
| `0x2B2A` | 576 | hue wheel, 192 × 3 | `0x2D6A` |
| `0x2D6A` | 3 | isolated white triplet, read by no one | `0x2D6D` |
| `0x2D6D` | 384 | second wheel, 128 × 3 | `0x2EED` |
| `0x2EED` | 126 | table A | `0x2F6B` |
| `0x2F6B` | 126 | table B | `0x2FE9` |
| `0x2FE9` | 75 | periods, 15 × 5 | `0x3034` |

Every boundary lands squarely on the next. This is the strongest cross-validation in this entire
chapter: eight sizes deduced independently, none overflowing, none leaving a gap.

##### Factory effect 6 is ported — `AULA_FX_SHIMMER`

Both sibling tables are now in the port, along with their engine. The engine at `0x60C9` was decoded
instruction by instruction:

```asm
0x60DE  a = [0x011C] ; cjne a,#7 -> 0x6191        ; branch on color mode
; -- rainbow branch --
0x6105  r5 = [0x0017 + col*6 + row]               ; THIS key's phase
0x6107  r7 = 0xC0 ; lcall 0xEDA2                   ; advance, modulo 192
0x6150  dptr = 0x2B2A + phase*3                    ; hue wheel
; -- fixed-color branch --
0x61B3  r7 = 0x60 ; lcall 0xEDA2                   ; advance, modulo 96
0x61FD  dptr = 0xC800 + effect*21 + mode*3         ; fixed color
0x626A  dptr = 0x08C3 ; lcall 0xA177               ; breathing gain
```

**`0xEDA2` holds a discovery**: it is not a simple increment. It tests bit `0x23` —
the very bit populated by `0x1166` from `b0[7]` of the per-effect record — and **decrements**
when that bit is set. It is the **scroll direction**.

##### Bit `0x23` — two sources, three consumers, and now ported

An exhaustive search across the image yields **six sites**, and not one more:

| Site | Role |
| --- | --- |
| `0x1121` | `a = [0x0315] ; rlc a ; mov 0x23,c` — **global source**, bit 7 of the persisted setting |
| `0x1172` | same pattern on `0x0345 + effect*2` — **per-effect source**, `b0[7]` |
| `0x7474` | `clr 0x23` — the concentric wave **forces forward direction** |
| `0xEF8A` | `clr 0x23` — the seeder `0xEF85` also clears it |
| `0xEDA2` | per-key phase advance: decrements if set, wrapping to `modulus - 1` |
| `0xA3F4` | `rgb_scroll_step`: advances the global position `[0x08C3]` by a **per-effect** step (20 for effect 10, 2 for `0x11`, 1 otherwise), forward or backward |
| `0x5C98` | four-direction automaton: two different subroutines depending on direction |

And `[0x0315]` can now be read in full: **bits 3–0 are the color mode** (`0x1138` extracts `& 0x0F` into
`[0x011C]`), **bit 7 is the direction**. The "direction" field of the settings table thus finds its
consumer, and `b0[7]` — long noted as a "bit with no reader" — finds a second one.

The port now exposes it under the `DIR_TOG` keycode, globally like `[0x0315]`, stored in
`user_settings.ul_brightness` — for the same reason as the color mode: a byte that the shared
struct reserves for underglow lighting, which this keyboard does not have.

##### A bug found while porting direction: the `led_phase` seam

`led_phase` is a **single byte**: it wraps at 256. Consumers whose modulus divides 256 never
notice it — the wave wraps at 128, and SMK's geometry operates on 0–255. But the vertical
rainbow wraps at **192** and the shimmer at 192 or 96, and `256 % 192 = 64`: **on every
wrap-around, their hue jumped by a third of the wheel**. At the slowest speed, a visible jump
every ten seconds.

The factory firmware does not suffer from this bug because it stores the phase **per key** and increments it
directly modulo 192 or 96 — it has no 8-bit frame counter.

A second counter, `fx_phase192`, wrapped at 192 — which is divisible by 96 — fixes both. One byte of
XDATA, and both moduli become seamless.

**`0xA177` implements the breathing ramp**: `component × CODE[0x29CE + phase] / 255`, 96 entries,
**22 → 255 over forty-eight steps and back down**. Its floor is not zero, but 22: in fixed-color
mode, a key never turns completely off.

The port transcribes all three tables — `0x2A2E`, `0x2AAC`, `0x29CE` — **verified byte for byte**,
and applies the same trick as for the wave: since all keys advance by the same step, the current phase
equals `seed + frame_counter`, requiring no per-key state to be maintained. Its period comes from `CODE 0x3002`,
the one loaded by the effect 6 seeder.

> **An index shift to keep in mind.** `AULA_FX_OFF` moves from 13 to 14. A saved setting that
> was 13 — "off" — now selects the shimmer. Harmless beyond the first
> boot.

##### The block extends further back, yielding two more tables

While looking for the breathing ramp, the boundary `0x29CE + 96 = 0x2A2E` pulled the thread further upstream. The
contiguous block actually comprises **eleven** tables:

| Address | Size | Contents | End |
| --- | --- | --- | --- |
| `0x2937` | 10 | global brightness ramp | `0x2941` |
| **`0x2941`** | **24** | **the seven fixed colors + black** — eight triplets | `0x2959` |
| `0x2959` | 117 | wave rings, 9 × 13 | `0x29CE` |
| **`0x29CE`** | **96** | **breathing ramp** | `0x2A2E` |
| `0x2A2E` | 126 | shimmer phase, mode 7 | `0x2AAC` |
| `0x2AAC` | 126 | shimmer phase, fixed color | `0x2B2A` |
| `0x2B2A` | 576 | hue wheel, 192 × 3 | `0x2D6A` |
| `0x2D6A` | 3 | isolated white triplet | `0x2D6D` |
| `0x2D6D` | 384 | second wheel, 128 × 3 | `0x2EED` |
| `0x2EED` | 126 | table A | `0x2F6B` |
| `0x2F6B` | 126 | table B | `0x2FE9` |
| `0x2FE9` | 75 | periods, 15 × 5 | `0x3034` |

`0x2941` is **a second copy of the `0xC800` palette**, in the same order, with black appended as an
eighth entry. Eleven tables, **eleven exact boundaries**, not a single byte of slack from `0x2937` to `0x3034`.

##### The six columns beyond 15 — a 113-key model

The factory grid is 6 × 21. This keyboard uses fifteen of those columns; here is what the other
six contain, and what that tells us.

**The shape.** Counting the non-empty positions in `CODE 0xC500`:

```
c 0 ######   c 5 ######   c10 ######   c15 ######  <-- beyond this keyboard
c 1 .#####   c 6 #####.   c11 #####.   c16 ######
c 2 ######   c 7 #####.   c12 ######   c17 .#####
c 3 #####.   c 8 ######   c13 ######   c18 .####.
c 4 #####.   c 9 ######   c14 ######   c19 .#####
                                       c20 .##.#.
```

**84 keys across columns 0 to 14, 29 across columns 15 to 20: 113 in total.** The firmware on this
F75 carries the complete map of a 113-key model from the same family — a full-size/numpad
layout, of which the F75 is the fifteen-column cut-down.

**No permutation beyond column 14.** There, the IDs in `0xC500` equal
`col × 8 + row` exactly, and tables A (`0x2EED`) and B (`0x2F6B`) are the identity. The permutation
on row 4 is confined to the first fifteen columns: it really is a detail of the 75% cut,
not a property of the larger model.

**Yet they are not dummy padding columns.** Three findings prove this:

| Structure | Size | What columns 15–20 contain |
| --- | --- | --- |
| phase field `CODE 0x9FEA` | **126 bytes** | continuous values from 15 to 71 — **not a single zero across all 126 entries**, the angular sweep continues |
| wave rings `CODE 0x2959` | 9 × 13 | **eight IDs from columns 15 and 16**: `0x7A`–`0x7D`, `0x80`–`0x85` |
| "gaming" mask `CODE 0xCAFC` | **126 bytes** | nothing — all nine lit keys lie within columns 0 to 14 |

And the XRAM structures are sized for 126, not 90:

- the color table at `0x0152` is **378 bytes = 126 × 3**, which is exactly what opcode `0x42` reads
  back (27 blocks × 14);
- the per-key state plane at `0x0017` is **126 bytes**, `0x0017` + `col × 6 + row`;
- the key queue index is bounded to **126** at `0x1670`.

Consequence for the port: `aula_fx_ring()` yields key IDs **that this keyboard does not
have**. `spark_seed()` filters them out via its column bound — it already did so, and that is now stated
explicitly in `ripple_step()` rather than being a fortunate side effect.

##### Correction: `CODE 0xCAFC` is not three color planes

This page and the port previously described the "gaming" preset as **three fifteen-byte planes in which
only the blue plane was non-zero**, resulting in a hardcoded blue rendering. That is wrong, and a search proves it
conclusively: **the fifteen-byte sequence used by the port does not exist anywhere in
the firmware image.** It was derived, not transcribed.

`0xCAFC` is **a single byte per key**, 126 bytes in all, indexed as `col × 6 + row`, equal to `0xFF`
on lit keys and zero elsewhere. Its nine non-zero positions are at indices
0, 9, 14, 15, 21, 77, 82, 83, 89 — i.e. `(0,0) (1,3) (2,2) (2,3) (3,3) (12,5) (13,4) (13,5) (14,5)`,
which correspond to **Esc, A, W, S, D, and the four arrow keys**. The part that held up still holds: nine
positions landing squarely on those keys in the compiled layout is no coincidence.

What did not hold up is the color: **there is no color information in this table**. The blue was
an invention born of a misreading. The effect now uses the active color mode, like every other
effect; blue remains available as mode 2.

The per-column reduction kept by the port is sound — when rebuilt from the actual 126 bytes,
it produces the exact same fifteen values.

##### Consequence for the port: two derived tables become one direct transcription

`aula_fx.c` previously carried two **derived** tables — a 15-byte presence mask and a 90-byte
spatial column map obtained by inverting table A. Both are replaced by the
**90 bytes of `CODE 0xC500` copied verbatim**, from which both can be derived on the fly:

- presence: `aula_fx_key_id(col, row) == 0xFF`;
- spatial column: `aula_fx_key_id(col, row) >> 3`.

Less data, zero derivation, and the byte-for-byte comparison against the dump passes. The presence
check benefits along the way: a direct lookup instead of a column mask followed by a bit shift.

##### What the wave engine does around the palette

`0x82A5` does not paint the entire grid. For each position, it reads **table A** (`CODE 0x2EED`,
`row × 21 + col`) and **only renders if the value is less than fifteen**, then still passes
through `rgb_key_suppressed` (`0x598F`) using **table B** (`CODE 0x2F6B`). `0x6C9B` performs the same check
before writing.

The port already checked key presence in its intensity-plane **seeders**, but not in its
**continuous** rendering paths — wave, vertical rainbow, solid, SMK geometry — which painted the six
positions of the 6 × 15 grid that have no physical key, paying for a color lookup for
nothing. The check now sits at the top of `led_regen_one()`, driven by the `CODE 0xC500` map that
this page already holds.

### `XRAM 0x009D` — RGB effect index

Twenty sites access it. Three converging clues identify it:

- `0x4494` writes `0x20` (32) to it — the exact value tested by `fcn.0000AF99`
- `0x52CC` and `0x5377` compare it against `9`
- `0x15E7`, `0x15FA`, `0x8C30`, `0x8C47` use it to **index into two tables in flash**

| Base | Contents | Range |
| --- | --- | --- |
| `0xA8BC` | `00`, then `04` × 23, then `00` | index 0–24 |
| `0xA8D5` | `00`, then `09` × 32 | index 0–32 |

Code resumes at `0xA8F6`. Index 0 yields zero, and all others yield a constant: these are
**per-mode parameters**, with mode 0 handled specially — presumably "off". The index goes
up to at least 32, which covers the `0x20` written at `0x4494`.

*Remaining inference: that these constants are RGB effect parameters. The convergence is
strong — bounded index, mode 0 equal to zero, written from the USB path, arbitrated against the USB ISR — but
none has been traced all the way to a PWM register.*

## Sleep — implemented

Transcribed from the factory sequence. The wake mechanism is: **all columns held
LOW, all rows configured as inputs with pull-ups**. Pressing any key pulls a row to 0, which
triggers INT4.

| Step | Factory address |
| --- | --- |
| Panel parking (rows input+pull-up, columns low then output) | `fcn @ 0x006E` |
| Off-matrix parking: P0.0/0.1, P0.5/0.6, P7.4/7.7, P4.5 → output low; P0.7 high; P7.6 low | `0x7DCB`–`0x7DF1` |
| `EXF1 = 0` | `0x7DF4` |
| `IENC = 0xF3` | `0x7DF6` |
| `EXF0 = 0x40` | `0x7DF9` |
| `IEN0 \|= 0x02` (EX4) | `0x7DFC` |
| `PCON \|= 0x02` (power-down) | `0x7E34` |

**`IENC = 0xF3` and `EXF0 = 0x40` are the exact values that `platform/sh68f90/extint.c`
documents** as "transcribed from factory firmware" — an independent confirmation on this device.
`extint_wake_arm()` can therefore be reused as-is.

`fcn @ 0x006E` is also a **fourth confirmation of the pinout**: it literally enumerates the
6 rows (`P7CR`/`P7PCR` bits 0–3, `P5CR`/`P5PCR` bits 3–4) followed by the 15 columns, in order.

⚠️ **Untested on hardware.** Incorrect pin parking = a keyboard that never wakes up.

### Radio sleep, added afterwards

The port previously returned `USER_SLEEP_USB` **hardcoded**, with the comment "No wireless on this
port". The consequence was not merely cosmetic: `sleep.c` decides whether to sleep based on
**USB bus suspend** in `USB` mode, and on its **inactivity timer** in `RF` mode — and outside
`RF` mode it resets that timer on every pass. In other words, **the keyboard had no
sleep mode at all when running on battery.**

The hook now tracks the mode switch live, just as the NuPhy Air60 does for its own switch.
`sleep_task()` polls it on every pass, so sliding the switch changes the sleep criterion
immediately.

Around `power_enter_powerdown()`, two transcribed additions:

| Stage | Factory firmware | Port |
| --- | --- | --- |
| before | `fcn.00007D74` / `fcn.00009E39` disable EUART0 (SCON, IEN1) | `rf_sleep_prepare()`: drains TX, then `IEN1 &= ~ES0` and `SCON = 0` |
| after | both wake paths (`0x7E85`, `0x9EBF`) re-call init `0xB1C2` | `rf_sleep_wake()`: `rf_uart_init()` then re-enables the IRQ |

Draining TX beforehand is an addition: a burst cut short by entering sleep would leave
`tx_busy` set and **`P0.2` stuck low as an output**, holding the radio module in a transmit-request
state throughout sleep.

And waking up reasserts the transport without waiting for the next poll — following the factory
supervisor's rule ("not connected, so ask again"), applied at a moment when we know the cached
state is no longer valid.

> **A wrongly flagged issue.** An earlier review concluded that `park_panel()` had a bug because it
> set `P7.4` and `P4.5` — the two mode-switch pins — to output low. Upon verification,
> that parking sequence is **transcribed from the factory firmware** (`0x7DCB`–`0x7DF1`), and `user_gpio_init()`
> restores them to the mode switch on wake: `P4CR=0x4D` and `P7CR=0x60` leave both as inputs, with
> pull-ups enabled (`P4PCR=0x6F`, `P7PCR=0xDF`). Nothing needed fixing — only an ordering constraint to
> respect: calling `rf_sleep_wake()` after `user_gpio_init()`.

## The BK3632 — what we know, and with what degree of certainty

### Established on THIS device (by disassembly or direct observation)

- **It is present on the PCB.** Identified by the `sinowisp` maintainer from the PCB photos in
  issue #96: "another package variant for the **BK3632**".
- **It handles all wireless functionality.** That is why OpenRGB support is **wired-only**: the
  driver talks to the SH68F90A, which does not manage the radio.
- **It communicates with the 8051 over EUART0** — vector 13 (`_INT_EUART0`), `SCON` 0xD8 / `SBUF` 0xAA,
  pins `P5.5` (TXD) and `P5.6` (RXD), frames `01 <cmd> <param> <len> …` of 6 to 32 bytes at
  **260,870 baud, 8N1**.
- **It holds the Bluetooth names.** Command `0x09`, sent twice from `main()`, pushes
  `"AULA-F75 3.0 KB "` and `"AULA-F75 5.0 KB "` to it from the 8051's flash (`0xAF6D`).

### BK3633 datasheet — primary source for the family

The **BK3633 V0.9** datasheet (26 pages, Beken) documents the direct sibling of the BK3632. Note:
**this is not the BK3632**, and figures may differ between family members.

| | BK3633 (datasheet) |
| --- | --- |
| Core | **32-bit RISC**, up to **80 MHz** |
| RAM | **80 KB** data memory |
| Flash | **500 KB** programmable |
| Radio | Bluetooth **5.2 dual-mode** + proprietary 2.4 GHz |
| Power consumption | ~5 mA active, < 1 µA deep sleep |
| Packages | QFN32 4×4, QFN40 5×5, QFN48 6×6 |
| Interfaces | **2 UARTs and UART download**, I²C, SPI up to 96 MHz, USB host and device, 6 PWMs, 10-bit ADC, secure JTAG, AES128, eFUSE |

⚠️ **Correction**: an earlier version of this document stated "ARM9, 20 KB RAM, 160 KB
flash" for the BK3632. Those numbers came from a **search engine summary**, not from a
datasheet. The BK3633 datasheet gives very different figures. The exact specifications
of the **BK3632** remain unverified.

### Three dump interfaces named in the datasheet

> "At the beginning of the chip starts up, the chip will enter **programming mode, JTAG mode or
> normal** according received command from **Mode Selecting Pin**."

Table 4 of the datasheet, GPIO multiplexing:

| GPIO | Normal | PROGRAM Mode | JTAG Mode |
| --- | --- | --- | --- |
| `GPIOA[0]` | `UART_TX` / SCL / USBDN | **`DL_UART_TX`** | — |
| `GPIOA[1]` | `UART_RX` / SDA / USBDP | **`DL_UART_RX`** | — |
| `GPIOA[3]` | SDA | — | `JTAG_NTRST` |
| `GPIOA[4]` | `SPI_SCK` | `SPI_MOSI` | `JTAG_TDI` |
| `GPIOA[5]` | `SPI_MOSI` | `SPI_MISO` | `JTAG_TDO` |
| `GPIOA[6]` | `SPI_MISO` / PWM[5] | `SPI_SCK` | `JTAG_TCK` |
| `GPIOA[7]` | `SPI_NSS` / PWM[4] | `SPI_CS` | `JTAG_TMS` |
| `GPIOB[6]` / `[7]` | `UART2_TX` / `UART2_RX` | — | — |

**"DL" = DownLoad.** UART download therefore goes through `GPIOA[0]` and `GPIOA[1]`.

On the **QFN32**, these two pins are pins **23 (`P00_USBDN`)** and **24 (`P01_USBDP`)** — multiplexed
with the USB D−/D+ lines.

Three avenues therefore exist, all documented: **UART**, **SPI**, and **JTAG**. The selection
mechanism — "according received command from Mode Selecting Pin" at startup — is **not detailed**
in this abbreviated datasheet. However, the wording matches what is already known: on the BK7231 you
spam `0xD2` over SPI during reset, and the BIM waits for a `LINK_CHECK` over UART.

### ⚠️ Potential lockout: encryption and permanently disableable interfaces

Section 3.8 of the datasheet, "Code Encryption and System Security":

> "There is one times NVM for code encryption and system security. Each unit can have different
> password for code encryption, where hardware will do the decryption automatically.
> **The download and debug interface could be closed permanently by user** to keep system security.
> […] Once the access right is changed, **no roll back is possible** to provide permanent security
> of the system."

Three implications, and they take precedence over everything above:

1. **The flash may be encrypted**, with a per-unit password and on-the-fly hardware
   decryption. A successful dump might only yield ciphertext.
2. **Both the download AND debug interfaces can be permanently disabled** by the
   manufacturer via one-time NVM (eFUSE-style). That would shut down UART, JTAG, and SPI all at once.
3. **It is irreversible.**

**We do not know whether Aula/SinoWealth locked the BK3632 on this keyboard.** The only way to find out
is to try.

The good news: testing this is **inexpensive and non-destructive**. A USB-TTL adapter on
`GPIOA[0]`/`GPIOA[1]`, a `01 E0 FC 01 00` (`LINK_CHECK`) frame repeated during reset, and we will know.
Silence = likely locked, or wrong entry sequence. Response = the door is open.

Many inexpensive consumer devices do not bother locking the chip. Still, until
tested, **Path B remains hypothetical, regardless of tooling**.

### Inferred from its close relative, the BK3432 — **not the same chip**

The BK3432 SDK (`Cdreamyao/tuya_ble_sdk_Demo_Project_bk3432`) provides, as a primary source:

| | Value | Source |
| --- | --- | --- |
| Core | **ARM9E-S**, little-endian, 60 MHz | `<Cpu>CPUTYPE(ARM9E)</Cpu>` in the Keil project |
| Vector table | classic ARM: reset, undefined, swi, pabort, dabort, reserved, irq, fiq | `boot_vectors.s` |
| Memory map | ROM `0x00000000`, RAM `0x00400000`, AHB peripherals `0x00800000+` | `BK3432_reg.h` |
| Bootloader | **BIM**, with UART download | `bim_uart.h`, and the SDK PDF |

**It really is an ARM9, not a Cortex-M** — the 8-entry vector table with `FIQ` proves it.
This corroborates the "ARM9" claim from the manufacturer summary, at least for the BK3432.

**Implication for any reverse-engineering effort**: this is not a 61 KB 8051 target like the SH68F90A,
but **160 KB of ARM9 code** — a completely different architecture requiring its own toolchain.

## Wireless — transport characterized, protocol unresolved

### What is established

**Full-duplex EUART0** link to the BK3632:

| Element | Value |
| --- | --- |
| Frame | **variable length**, 6 to 32 bytes (`r5` before the call) |
| Byte 0 | `0x01` (constant header) |
| Byte 1 | command code — **`0x04`, `0x05`, `0x08`, `0x09`** observed |
| Bytes 2+ | payload |

Lengths observed at call sites: `0x1E` (30) @ `0x46C2`, `0x0D` (13) @ `0x4730`,
`0x20` (32) @ `0xA376`, `0x17` (23) @ `0xB0B8`, `0x06` @ `0xED53`. The 32-byte maximum
matches the exact size of the TX buffer, which ends right before the RX buffer.
| TXD / RXD | **`P5.5`** / **`P5.6`** (separate pins: full-duplex link) |
| Handshake | **`P0.2`** — pulled low before transmission (`0xAB13`), released at end of frame by the ISR (`0xA580`) |
| Format | mode 1, **8N1**, `Baud = Fsys / 92` = **~260,870 baud** (`0xB1CE`) |
| Send routine | `fcn @ 0xAB08` (zero payload) / `0xAB09` (payload in A) → `0xAB0F` |
| Buffers | TX in IDATA `0x33`–`0x52` (**32 B**), RX in IDATA `0x54` (23 B) |
| TX busy flag | `0x2C.1` |

Command `0x09` is sent from `main()` (`0x9148`, `0x914D`) — probably link
initialization.

### No embedded BK3632 firmware — verified

Tested hypothesis: could the 8051 embed the BK3632 firmware image and push it over the serial link?
**No**, and this is measured, not assumed.

1. **Available space.** The factory firmware is 61,440 B and its last meaningful byte is at
   `0xEFC3`: only **60 free bytes** remain. The BK3632 has **160 KB** of flash. A complete image cannot
   fit.
2. **Entropy profile.** Scanning in 512 B blocks: **no block exceeds 6.53**. There is no
   compressed or encrypted region. ARM9 code would sit around 6 — whereas the only non-8051-code
   regions have *very low* entropy instead: `0xB200`–`0xC400` (entropy 1.10, **87% zeros**) and
   `0xC600`–`0xEC00` (1.20, **93% zeros**). Those are sparse tables — bitmaps and per-layer
   keymaps — not code.
3. **Link throughput.** The TX buffer is 32 bytes and the longest observed frame is 32 bytes.
   Nothing points to a bulk transfer path.

Conclusion: the BK3632 firmware resides **inside the BK3632**. To obtain it, one must read it off
the chip (Path B below); to understand the protocol, it is simpler to sniff the serial link
(Path A).

### All functions touching EUART0 — exhaustive scan

Full-image scan across all seven EUART0 SFRs (`SCON` D8H, `SBUF` AAH, `SADDR` ABH,
`SADEN` ACH, `SBRTH` ADH, `SBRTL` AEH, `SFINE` AFH), plus the enable and priority bits,
cross-checked against a byte-by-byte scan of all 8051 direct-operand opcodes.

| Function | Role | SFRs touched |
| --- | --- | --- |
| `0xB1C2` | **init** — baud rate, mode, enable | SBRTH, SBRTL, SFINE, SCON, PCON, IEN1 |
| `0xA544` | **EUART0 ISR** (vector `0x6B`) | SCON.RI/TI, SBUF, SCON |
| `0xAB0F` | generic transmitter, variable length | SBUF |
| `0xACE6` | short-frame transmitter (6 B) | SBUF |
| `0xEF40` | **wireless entry**: `orl IEN1,#0x40` **then `anl USBCON,#0x7F`**, `setb 0x27.7`, `delay(20)` (2 calls from `0x84E9`) | IEN1, **USBCON** |
| `0xECC5` @ `0xEDE0` | priority: `IPH1 = 0x42`, `IPL1 = 0x41` → bit 6 set in both, **priority level 3** | IPH1, IPL1 |
| `0x7D74`, `0x9E39` | sleep: disable EUART0, re-call `0xB1C2` on wake | SCON, IEN1 |
| `0x84E9` @ `0x8548` | disables EUART0 | IEN1 |
| `0x41F2`, `0x4227`, `0x425D` | enable EUART0 after setting XDATA `0x0319` and `0x031B` | IEN1 |
| `0x9156` | disables EUART0 if XDATA `0x031B == 0` | IEN1 |

Callers of init `0xB1C2`: `main` (`0x9143`) and **both** wake paths (`0x7E85`,
`0x9EBF`). The link is therefore completely reconfigured every time the keyboard wakes from sleep.

`XDATA 0x0319` (values 1/2/3) and `XDATA 0x031B` govern activation: in all likelihood they represent
the **wireless mode selector** (BT1/BT2/BT3 or BT/2.4G/USB), though their exact
semantics were **not yet resolved** at this stage.

> **Methodological pitfall.** Three apparent "`SBUF` accesses" — `0xB1ED`, `0xECD0`, `0x7E62` — are
> **false positives**: they are the operand bytes of `lcall 0xABAA` (`12 ab aa`), which a raw byte scan
> misreads as `mov r3, 0xAA`. Any naive SFR scan on this image falls into this trap.

#### The interrupt vector — and the correction it forces

`_INT_USB = 7`, and the firmware indeed places `ljmp isr.usb` at `0x03 + 8×7 = 0x3B`. The
index → vector mapping is thus confirmed, and it is **not** that of a classic 8051:

| Vec | n | Source | Target |
| --- | --- | --- | --- |
| `0x03` | 0 | **TIMER2** | `0xA4D7` |
| `0x0B` | 1 | INT4 | `0xEF94` |
| `0x13` | 2 | INT3 | `0xEF9A` |
| `0x1B` | 3 | INT2 | `0xEFA0` |
| `0x3B` | 7 | USB | `0xAC60` |
| `0x43` | 8 | PWM0 | `0x72BA` |
| `0x4B` | 9 | PWM1 | `0xEE12` |
| `0x53` | 10 | PWM2 | `0xEE26` |
| `0x63` | 12 | PWM4 | `0x816B` |
| `0x6B` | 13 | **EUART0** | **`0xA544`** |

`0x23` (SCM), `0x2B` (LPD), `0x33` (SPI), `0x5B` (PWM3), and `0x7B` are unused: the
firmware places ordinary code or data across them.

⚠️ The labels `vec.int0` / `vec.timer0` / `vec.int1` / `vec.timer1` and `isr.int0 @ 0xA4D7`
in the `tools/f75_r2.py` harness originally used classic 8051 names — **wrong on this MCU**.
Fixed in the same commit as this section.

#### Baud rate — the formula, not a guess

`0xB1C2` writes `SBRTH = 0xFF`, `SBRTL = 0xFB`, `SFINE = 0x0C`. `SBRTH.7` is `SBRTEN`, so
`SBRT = 0x7FFB = 32763`. For modes 1 and 3, the datasheet gives:

```
BaudRate = Fsys / (16 × (32768 − SBRT) + SFINE)
         = Fsys / (16 × 5 + 12)
         = Fsys / 92
```

`Fsys` is established by two concurring sources:

- the firmware writes `PLLCON = 0x03` (PLLON + PLLFS) and `CLKCON = 0x0C` (HFON + FS, with
  `CLKS[1:0] = 00`, i.e. no division). Datasheet: PLLFS = 1 → "PLL 的二分频作为
  OSCSCLK" (PLL divided by 2 used as OSCSCLK), and the clock tree diagram shows **PLL = 48 MHz** → OSCSCLK = 24 MHz;
- SMK's `meson.build`: `'sh68f90' : { 'freq_sys' : 24000000 }`.

**Baud rate = 24,000,000 / 92 ≈ 260,870 baud.** This is not a standard baud rate, and there is no
nominal standard value to round it to: the link is proprietary on both ends.

#### `SCON = 0x50` + `SSTAT` — the top three bits are error flags

`0xB1CE` writes `SCON = 0x50` → `SM1 = 1`, `REN = 1`: **mode 1, 8-bit, 1 stop bit**. Then `0xB1DE`
executes `orl PCON, #0x40`, i.e. **`SSTAT = 1`**. The datasheet is explicit:

> SSTAT — 0：SCON[7:5] 工作方式作为 SM0，SM1，SM2 ／ 1：访问状态位（FE，RXOV，TXCOL）

With `SSTAT = 1`, `SCON[7:5]` are therefore no longer the mode bits, but **FE** (framing error),
**RXOV** (receive overflow), and **TXCOL** (transmit collision) — and they "只能通过软件
清零" (can only be cleared by software). That is the exact purpose of the tail of
the ISR, which would otherwise be incomprehensible:

```asm
mov a, SCON
anl a, #0xE0        ; FE | RXOV | TXCOL
jz  done
anl SCON, #0x1F     ; clear the three error flags
```

#### ISR `0xA544` — IDATA memory layout

```asm
; --- receive ---
jnb  SCON.RI, tx
clr  SCON.RI
mov  r0,#0x70 ; mov a,@r0     ; RX write index
subb a,#0x17  ; jnc done      ; buffer full (23 B) -> drop byte
mov  a,#0x54  ; add a,r7      ; buf[idx]
mov  @r0, SBUF
inc  @r0                      ; idx++
setb 0x24.4                   ; "byte received"

; --- transmit ---
tx: jnb SCON.TI, done
clr  SCON.TI
mov  r1,#0x71 ; mov a,@r1     ; TX index
mov  r0,#0x75 ; subb a,@r0    ; compare against length - 1
jc   next
clr  0x2C.1                   ; transmission complete
anl  P0CR, #0xFB              ; P0.2 back to input
setb P0.2                     ; ... and pulled high
sjmp done
next: inc @r0 ; mov a,@r0 ; add a,#0x33 ; mov SBUF, [a]
```

| IDATA | Role |
| --- | --- |
| `0x33`–`0x52` | **TX** buffer (32 B max, ends right before the RX buffer) |
| `0x54`–`0x6A` | **RX** buffer, 23 bytes — guard `idx ≥ 0x17` |
| `0x70` | RX write index |
| `0x71` | TX send index |
| `0x74` | `r7` from the last send call (frame tag) |
| `0x75` | TX length **− 1** |

The 23 bytes of the RX buffer match the exact 23 bytes that `fcn.000005EA` copies
to XDATA `0x0120` — an independent cross-check of the memory layout.

#### `P0.2` — the handshake line, mechanism now proven

The role of `P0.2` was previously asserted without proof (and initially mischaracterized as a
half-duplex direction line). Both ends of the sequence settle it:

| Stage | Code | Effect |
| --- | --- | --- |
| start of TX (`0xAB13`, `0xACEA`) | `clr P0.2` ; `orl P0CR,#0x04` ; `clr P0.2` | `P0.2` switches **to output** and is driven **low** |
| end of TX (ISR, `0xA580`) | `anl P0CR,#0xFB` ; `setb P0.2` | `P0.2` switches back **to input**, released **high** |

Datasheet, `P0CR` register (E1H): "0：输入模式" (0: input mode) — so 1 = output. `P0.2` is indeed a
**transmit request / wake** line to the BK3632, held low throughout the entire burst.

#### Checksum — same constant for both TX and RX

`0xAB0F` sums bytes `0x33 .. 0x33+len−2` and writes `0x55 − Σ` as the **last** byte
of the frame (`0xAB2E`). This is the **exact same `0x55` constant** previously observed on incoming
responses — cross-validating both directions of the link.

#### The two transmitters

- **`fcn.0000AB0F`** — generic. Input: `r5` = total length, frame already assembled in IDATA
  `0x33`. It computes the checksum, places it in the last byte, zeroes the indices, and primes the transfer by writing
  `SBUF = IDATA[0x33]`; the ISR sends the rest. Five callers (`0x46C2`, `0x4730`, `0xA376`,
  `0xB0B8`, `0xED53`).
- **`fcn.0000ACE6`** — fixed 6-byte frame `01 <r7> <r5> 00 00 <sum>`. Its only two
  callers are **inside the parser `fcn.000005EA`** (`0x078A`, `0x0811`): this is therefore the
  response path. *Inferred reading*: a leading `0x01` serves here as a short acknowledgment, in
  contrast to the 10-byte incoming responses starting with `0x02`.

#### Full receive-flag chain

```
EUART0 ISR  (0xA544, vec 0x6B)   setb 0x24.4          "byte received"
PWM0 ISR    (0x72BA, vec 0x43)   0x24.4 -> 0x2D.7     relayed at 0x7444
TIMER2 ISR  (0xA4D7, vec 0x03)   lcall 0x05EA @ 0xA519
fcn.000005EA                     IDATA 0x54 (23 B) -> XDATA 0x0120
```

The parser therefore does **not** run in the main loop, but inside the **TIMER2 ISR**.

### Bridging EUART0 ↔ HID stack

Reports go to **two mutually exclusive destinations**, and the switch comes down to a single bit.

```
key event
  |
  +-- fcn.00003108  /  fcn.000068E8       the two schedulers
        |
        +-- fcn.000044F6 --> euart0.send  --------------------> RADIO
        |     guards: 0x2A.4, 0x27.0, 0x2A.6, then P4.7
        |
        +-- fcn.0000EF7B
              jb 0x2D.4, ret              <== the switch
              lcall fcn.0000AA79
              lcall fcn.00006ACF --> 0x6C07 --> EP2_IN_BUF ----> USB
```

In `fcn.000068E8`, the two calls follow one another directly: `lcall fcn.000044F6` at `0x69C2`, then
`lcall fcn.0000EF7B` at `0x69C7`. Each path carries its own guard; it is not an `if/else`,
but two independently guarded paths.

#### `0x2D.4` — the bit that cuts off USB

> **Correction.** I initially presented `0x2D.4` as "the radio/USB switch". That is accurate
> as to its **effect**, but wrong as to its **cause**: it is set not by a mode selection, but
> by hysteresis on an analog quantity received from the radio (see `fcn.00001DC3`
> below). The actual mode selector is `XRAM 0x031B`.

`fcn.0000EF7B` is just ten bytes long:

```asm
jb    0x2d.4, ret        ; radio active -> no USB report
lcall fcn.0000aa79
lcall fcn.00006acf       ; HID -> USB dispatcher
```

Nine other functions read it (`0x0F77`, `0x1632`, `0x1F14`, `0x1F5B`, `0x1FF7`, `0x5995`,
`0x68EC`, `0x9A82`). It is written **only in `fcn.00001DC3`**: `setb` at `0x1F25`, `clr` at
`0x1EF5` and `0x1F50`. A single owner for this state, making it a natural observation
point for the output path.

#### The USB path: `fcn.00006ACF` → `0x6C07`

`fcn.00006ACF` scans pending report flags in decreasing priority order (`0x27.4`,
`0x2A.6`, `0x29.0`, …). For each one, it sets up three values and jumps to `0x6C07`:

| XRAM | Role |
| --- | --- |
| `0x0F0C` | report index |
| `0x0F0F`–`0x0F11` | generic pointer to the source buffer |
| buffer`[0]` | **Report ID** = `0x0F0C` + 1 |

`0x6C07` does the rest:

```asm
movx [0x0f49] = 0x11 ; [0x0f4a] = 0x80    ; destination = 0x1180
movx [0x0f0b] = 0                          ; counter
loop:
  a = [0x0f0c] ; mov dptr,#0x6045 ; movc a,@a+dptr   ; report length
  ...byte-by-byte copy from generic pointer to 0x1180 + i...
mov  IEP2CNT, a        ; SFR 0x9D
orl  EP2CON, #0x04     ; SFR 0x9A  -> arm endpoint
```

**`0x1180` is `EP2_IN_BUF`** (`_SBUF(0x1180) EP2_IN_BUF[64]` in the SMK header). `0x6C07` is
therefore purely USB: it contains no radio branch.

The table at `0x6045` gives the total length for each report index:

| index `0x0F0C` | 0 | 1 | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- | --- | --- |
| length | 3 | 2 | **4** | 16 | 8 | 8 |

Index 2 is 4 bytes long — matching the exact report built by `0xA4BD`, which writes `0x08BF`
(Report ID 3) plus `0x08C0`–`0x08C2`: four bytes, no more, no less.

#### The radio path: `fcn.000044F6`

It reads an index at `XRAM 0x0307`, addresses a **28-byte** record at
`0x0C57 + index × 28`, and sends two frame sizes:

| Call site | copied payload | `r5` (frame size) |
| --- | --- | --- |
| `0x46C2` | 28 B (`fcn.00004C4B`, `r7 = 0x1C`) | **30** |
| `0x4730` | 11 B (`r7 = 0x0B`) | **13** |

This is the only one of the five `euart0.send` call sites reached from the report chain.
The other four carry configuration, not keystrokes:

| Caller | Frame | Contents |
| --- | --- | --- |
| `fcn.0000A307` @ `0xA376` | 32 B | command `0x09`, Bluetooth name |
| `fcn.0000B093` @ `0xB0B8` | 23 B | command `0x08`, 19 B payload |
| `fcn.0000ED3B` @ `0xED53` | 6 B | command `0x06`, `01 06 00 00 00 <ck>` |

> Clarification to an earlier note: in `euart0.send`, **`r7` is not the command byte** —
> it is stored in IDATA `0x74` as a tag. The command byte is byte 1 of the frame,
> assembled by the caller. `fcn.0000ED3B` illustrates this: `r7 = 0x0A`, whereas the frame carries `0x06`.

#### `P4.7` — the second handshake line, an input

Every transmission is preceded by `jnb 0xb0.7, exit` (SFR `0xB0` = `P4`). Eleven sites test it:
`0x44F6`, `0xB093`, `0xB06A`, `0xB0BC`, `0xED3B`, `0xED89`, `0xEC85`, `0xEC68`, plus
`isr.pwm0` (`0x743D`), `fcn.0000801F` (`0x8152`), and `fcn.0000AE50` (`0xAE5B`).

**There is not a single `setb` or `clr` on `0xB0.7` in the entire image** — the firmware never writes to it. It is
therefore an **input**, driven by the BK3632. Together with `P0.2` as an output (transmit request), the
link has **two handshake lines**, and `P4.7` does not appear in any matrix column —
consistent.

`fcn.0000B093` combines both guards, showing the canonical form of a transmitter:

```asm
jb  0x2c.1, ret     ; a transmission is already in progress
jnb 0xb0.7, ret     ; the radio module is not ready
```

### The three functions of the subsystem: `1DC3`, `44F6`, `6ACF`

| Function | Size | Blocks | Role |
| --- | --- | --- | --- |
| `fcn.00001DC3` | 2700 B | 219 | power management — **sole writer of `0x2D.4`** |
| `fcn.000044F6` | 844 B | 53 | report transmitter to the radio |
| `fcn.00006ACF` | 460 B | 33 | report dispatcher to USB |

#### `fcn.00001DC3` — hysteresis, not a mode selector

Called from a single site (`fcn.00008FA7` @ `0x8FE6`). Its entry guards lead to four
sub-paths; the one that drives `0x2D.4` is reached when `0x26.0` is **clear**:

```asm
0x1f05  ; 16-bit compare [0x02E6]:[0x02E7] against 0x02E1 = 737
        jc 0x1f17                       ; value < 737
0x1f14  jnb 0x2d.4, 0x1f2e              ; otherwise, reset counter to zero
0x1f17  [0x097b]++ ; if >= 0xC8 (200):
0x1f25     setb 0x2d.4                  ; <== trip
0x1f27     setb 0x29.1 ; lcall fcn.0000EE6A

0x1f33  ; compare the same value against 0x0390 = 912
0x1f43  [0x097f]++ ; if >= 0x64 (100):
0x1f50     clr 0x2d.4                   ; <== recover
0x1f52     clr 0x2b.3
```

Two thresholds, two debounce counters, and a deadband from 737 to 912: this is **hysteresis**,
not mode switching.

**Where the measured value comes from.** `XRAM 0x02E6:0x02E7` is written by **`euart0.parse`**
(`0x0677`, `0x0683`, `0x0691`) from the byte at `XRAM 0x0127` — **offset 7 of the received frame**
copied to `0x0120` — and initialized to `0x03xx` by `vec.reset` (`0x9136`). The value therefore comes
from the **BK3632**, not the 8051.

That matches the silicon: the SH68F90 **has no ADC**. The datasheet only gives it
a low-voltage detector (`LPDCON` 0xB3, `LPDSEL` 0x89). Any analog measurement must
come from the radio chip.

`fcn.00001DC3` compares this value against **four stepped thresholds**:

| Site | Threshold | Decimal |
| --- | --- | --- |
| `0x1F06` | `0x02E1` | 737 |
| `0x23D8` | `0x030D` | 781 |
| `0x2329` | `0x034E` | 846 |
| `0x227A` | `0x0390` | 912 |

A monotonic scale within a range consistent with a **10-bit conversion** (0–1023),
measured by the BK3632 and transmitted over EUART0.

> *Inferred:* that this is the **battery voltage** and that the four thresholds are the
> steps of a battery gauge. What is established: the origin (EUART0 frame, byte 7), the threshold
> scale, the hysteresis, and the fact that `0x2D.4` has only a single writer.

#### `fcn.00006ACF` — two guards, then six slots

```asm
mov a, EP2CON            ; SFR 0x9A
jnb ACC.2, next          ; if endpoint is still armed -> exit
ljmp exit
next:
a = [0x031b] ; jz chain  ; ljmp exit     ; send nothing if 0x031B != 0
```

A non-zero `0x031B` is enough to cut off all USB transmissions — a more direct gate than `0x2D.4`,
and the exact same variable that governs EUART0 activation in `0x41F2`/`0x4227`/`0x425D`.

Next comes a **decreasing-priority chain**. Each link clears its flag,
sets `0x26.7`, writes the index, the Report ID at the head of the buffer, and a generic pointer, then
jumps to the copy routine:

| Order | Flag | `0x0F0C` | Buffer | Report ID | Length |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x2A.0` | 0 | `0x09BC` | 2 | 3 |
| 2 | `0x27.3` | 1 | `0x097D` | 1 | 2 |
| 3 | `0x27.4` | 2 | `0x08BF` | 3 | 4 |
| 4 | `0x2A.6` | 3 | `0x0980` | 4 | 16 |
| 5 | `0x29.0` | 4 | `0x09B0` | 7 | 8 |
| 6 | `0x2B.2` | 5 | `0x0095` | 6 | 8 |

**Six links — and the length table at `0x6045` has exactly six valid entries**
(`03 02 04 10 08 08`) before its terminating `0xFF`. The two mutually confirm each other, which
also validates our reading of the table.

Note that the Report ID does not follow the index sequentially: index 0 carries ID 2, and index 1 carries ID 1.
Links 3 and 4 compute it via `inc a`, while the others hardcode it.

#### `fcn.000044F6` — two symmetric halves, one shared exit

```
0x44F6  jb 0x2A.4 / jb 0x27.0 / jb 0x2A.6  -> half A (0x4502)
                                   else    -> half B (0x45A2)

half A (0x4502-0x45A2)   index [0x0307], record 0x0C57 + n x 28
half B (0x45A2-0x4652)   three guards then parallel body

0x4652  convergence
0x4675    jb 0xB0.7          <-- P4.7: radio module must be ready
0x467B    ... -> euart0.send  r5 = 30   (28 B payload)
0x46EB    ... -> euart0.send  r5 = 13   (11 B payload)
```

The two halves are structurally identical (136- and 143-byte blocks) and converge on
a single transmission block. This is the only one of the five `euart0.send` entry points reached
from the report chain.

### `XRAM 0x031B` — the active transport

**57 references** across the image: this is the central variable of the wireless subsystem. Only three
values occur: `0`, `1`, and `2`.

#### What the value controls

| Site | Test | Effect when `0x031B == 0` |
| --- | --- | --- |
| `usb.init` @ `0xEC00` | `jnz` | USB is initialized (`USBADDR`) **only** in this case |
| `fcn.00006ACF` @ `0x6AD7` | `jz` | no USB HID report is sent if the value is non-zero |
| `isr.timer2` @ `0xA513` | `jz` | `euart0.parse` is called **only** if the value is non-zero |
| `vec.reset` @ `0x9150` | `jnz` | otherwise `anl IEN1,#0xBF` — the EUART0 IRQ stays disabled |
| `fcn.00001DC3` @ `0x1EFC` | `jnz` | power hysteresis only runs in wireless mode |
| `fcn.00003108` @ `0x310E` | `cjne #0x02` | in mode 2, one more element is scanned (`inc 0x0E`) |

> Clarification to the previous section: the call to `euart0.parse` in `isr.timer2` has **two**
> guards, not one — `jb 0x2c.0` at `0xA504` and `jz` on `0x031B` at `0xA513`, both branching to
> the same exit at `0xA51C`.

#### Returning to wired mode — `fcn.000084E9` @ `0x8543`

```asm
clr   a
movx  [0x031b], a       ; transport = 0
anl   IEN1, #0xBF       ; disable EUART0 IRQ
lcall usb.init          ; reinitialize USB
```

#### Switching to Bluetooth — three identical blocks

`0x41D4`, `0x420A`, `0x423F`, identical up to the slot number:

```asm
jnb 0x29.5, abandon ; jnb 0x2a.5, abandon
a = [0x031B] ; xrl #0x02 ; jz suite     ; requires ALREADY being in mode 2
a = [0x0319] ; xrl #0x0N ; jz fin       ; already on this slot -> do nothing
[0x031B] = 2
[0x0319] = N            ; dec a / movx a / inc a, with a = 2  ->  1, 2, 3
orl  IEN1, #0x40        ; enable EUART0 IRQ
anl  USBCON, #0x7F      ; disable USB
setb 0x27.7 ; setb 0x26.5
```

The slot number is produced by the only difference between the three blocks: `dec a`, `movx a`,
`inc a` applied to `a = 2`. Therefore **`XRAM 0x0319` is the Bluetooth slot, 1 to 3**, and these three
blocks are the *slot-switching* shortcuts — they require mode 2 and do not enter it.

The symmetry with returning to wired mode is exact: `IEN1.6` enabled vs. disabled, `USBCON.7` disabled
vs. `usb.init`.

#### Mode 1

`0x4276` requires `[0x031B] == 1`, zeroes a 16-bit counter at `0x0961`, and sets `0x2C.5` —
a trigger, not a mode entry. `euart0.parse` (`0x070E`) and `fcn.000084E9` (`0x8576`)
handle mode 1 distinctly from mode 2, each with its own `xrl`.

> *Inferred:* 0 = wired USB, 1 = 2.4 GHz, 2 = Bluetooth — this is the usual tri-mode structure
> of these keyboards. *Established:* three values; mode 2 carries a slot 1–3; mode 0 is the one where
> USB is initialized and wireless is completely shut off.

#### Resolved, completely: what enters 2.4 GHz mode

Open item #1 — "no direct write of the value 1 to `g_transport`" — is now closed,
and not only via the persisted block path described below. There is a **hardware selector**,
and it has its own routine.

##### `fcn.000084E9` — the three-position slide switch

Called from `tick_slow` (`0x9074`). It samples **two pins**:

```asm
0x84E9  jb  0xF8.4, 0x84FE     ; 0xF8 = P7  ->  P7.4
0x84FE  jb  0xB0.5, 0x8512     ; 0xB0 = P4  ->  P4.5
```

> Ghidra names them `F8_4` and `T1`: `T1` is the classic name of bit `0xB0.5` when `0xB0` is mistaken
> for `P3`. That is the SFR map trap again — on this MCU, `0xB0` is `P4`.

| `P7.4` | `P4.5` | Position | Counter |
| --- | --- | --- | --- |
| 1 | 1 | **wired** | `0x08BD` |
| 1 | 0 | **Bluetooth** | `0x0960` |
| 0 | — | **2.4 GHz** | `0x02DF` |

Each position has its own counter; the other two are zeroed on each pass. **Ten
consecutive passes** trigger the switchover. It is a debounce filter, not a timer.

These two pins were among the "unknown role" inputs in the factory GPIO init. Seven remain:
`P0.0`, `P0.1`, `P0.5`, `P0.6`, `P4.1`, `P4.4`, `P7.7`.

##### The three branches

```c
/* 2.4 GHz */
if (g_transport == 0) { setb 0x27.7; delay(20); }   /* 0xEF8D */
g_transport = 1;
IEN1 |= 0x40; USBCON &= 0x7F; setb 0x27.7; delay(20);   /* 0xEF40 */
rf_link_select(0, 0);                               /* <<< slot 0 */

/* Bluetooth */
g_transport = 2;
if (g_bt_slot < 1 || g_bt_slot > 3) g_bt_slot = 1;
IEN1 |= 0x40; USBCON &= 0x7F; setb 0x27.7; delay(20);
rf_link_select(g_bt_slot, 0);

/* wired */
setb 0x27.7; delay(20);
rf_notify_wired();                                  /* 0xEED1 */
g_transport = 0;
IEN1 &= 0xBF;
usb_init();
delay(200);
```

`fcn.0000EED1` is the polite exit sequence: **command `0x0E`**, ten milliseconds, **command
`0x0B` with parameter 0**, ten milliseconds.

##### The two switchover delay thunks — and turning off USB

An earlier draft of this page described `0xEF40` as a simple IRQ re-arming. That is wrong,
and the difference matters for a port. Both thunks, disassembled in full:

```asm
0xEF8D  setb 0x3f           ; = 0x27.7
        mov  r7,#0x14
        ljmp 0xED03         ; ~20 ms

0xEF40  orl  IEN1,#0x40     ; EUART0 IRQ is armed
        anl  USBCON,#0x7F   ; <<< the USB module turns OFF
        setb 0x3f
        mov  r7,#0x14
        ljmp 0xED03         ; ~20 ms
```

`0xED03` is the delay loop (`r7` on entry, two nested levels). `0x3f` is
the *bit* address of `0x27.7`, hence the notation used above.

So **in wireless mode, the factory firmware shuts off the USB peripheral**; it does not merely
ignore the host. The three slot-switching shortcuts (`0x41D4`, `0x420A`, `0x423F`) already did
this explicitly — what is new is that the *slide switch* path does it too,
via this thunk, and that is the path the port follows.

##### `fcn.00003901`: command `0x06` is a presence probe

Re-read in C, the idle branch of the sequencer (documented below) is not a one-off status
request but a **three-strike link keepalive**:

```c
if (g_state == 0 && !flag_7_2) {
    if (++[0x0150] > 99) {
        [0x0150] = 0;
        euart0_cmd06();                 /* command 0x06 */
        if (++IDATA[0x30] > 2) {        /* three unanswered probes */
            IDATA[0x30] = 0;
            0x2C.1 = 0;                 /* "link alive" drops */
            P0CR &= 0xFB; P0_2 = 1;     /* the TX line is released */
        }
    }
}
```

Releasing `P0.2` is the exact same action the ISR performs at the end of a burst: the probe also serves
as a fallback recovery if a burst was lost. This is the mechanism the port transcribes to give
meaning to its "connected" indicator — and that is why this indicator **drops** on its own.

##### `fcn.0000EF5A` → `fcn.0000ED89` — the command that selects the radio

```asm
0xED89  jb  0x2c.1, ret        ; no transmission in progress
0xED8C  jnb P4.7,   ret        ; module ready
        IDATA[0x33] = 0x01     ; header
        IDATA[0x34] = 0x01     ; command
        IDATA[0x35] = R7       ; slot
        IDATA[0x36] = R5       ; flag
0xED9E  lcall euart0_send      ; six bytes
```

Or on the wire:

```
01 01 <flag> <slot> 00 <0x55 - sum>
```

> **Correction.** This page initially wrote `01 01 <slot> <flag>`. The order is the reverse, and
> the assembly above already says so: `IDATA[0x35] = R7` **then** `IDATA[0x36] = R5`. What receives
> `g_bt_slot` is **R5**, across all four call sites that send it — `0x85D7` (Bluetooth entry), `0x44C0`,
> `0x87AD`, `0x06E6` (link supervisor). R7 is `0` everywhere except at `0x87BD`, where it is `1`.
>
> What settles it is `0x06E6`: in Bluetooth mode, when the received state does not match the expected slot, the
> supervisor re-requests with `R5 = g_bt_slot` and `R7 = 0` (`0x072C`: `ff` = `MOV R7,A` with `A = 0`).
> If R7 carried the slot, the keyboard would request the **2.4 GHz dongle** on every Bluetooth link
> failure: it would switch radios because of a wrong radio. The port was sending them in the wrong
> order; this is fixed in `rf_send_link()`.

**The slot is the radio encoding, and it does not match `g_transport`**:

| Slot | Radio |
| --- | --- |
| `0` | **2.4 GHz dongle** |
| `1` `2` `3` | Bluetooth, slots 1 to 3 |

The proof fits in three lines of `tick_slow_wireless` (`0x870C`):

```c
if (g_transport == 2)      slot = g_bt_slot;   /* 0x87AD */
else if (g_transport == 1) slot = 0;           /* 0x87BB */
else                       /* nothing */;
rf_link_select(slot, 1);                       /* 0x87BF, R7 = 1 */
```

In mode 1, the only possible slot is **0**. And entering mode 1 in `fcn.000084E9` calls
`rf_link_select(0, 0)` with hardcoded arguments. Both call sites agree.

> **Watch out for the encoding collision.** `g_transport` is 0 for *wired*; the slot is 0
> for *2.4 GHz*. The two fields cross on the most dangerous value. The SMK port
> defines two distinct types for this reason.

The second argument is **0** when entering a mode and **1** from the slow tick, gated on bit
`0x2C.5` set by the pairing shortcut (`0x4287`). Hence the reading "1 = restart
pairing" — consistent across both sites, though that is their only difference: *inferred*.

##### `fcn.0000870C` — the wireless slow tick

Three timed actions, two of which are new:

- **`0x2B.7`** → factory reset. Full details are below under "No pairing erasure,
  but a re-pairing trigger": it is more than just `settings_save` and visual feedback.
- **`0x2C.5`** → link reassertion, above.
- a counter of roughly thirty-two passes → **command `0x04` with parameter 3**.

#### Resolved: no pairing erasure, but a re-pairing trigger

This page long listed "pairing erasure" as an open item, with only one
candidate, `fcn.0000929E`, "whose `0xFF`-filled area is unspecified". **That candidate is
eliminated, and the question has a two-part answer**:

- **there is no erasure** — this firmware never sends anything to the BK3632 that
  could destroy a stored bond;
- **there is a pairing restart**, on a dedicated shortcut, and its path is now
  traced end to end: it is command `0x01` with its flag set to `1`.

##### `fcn.0000929E` is the startup RAM initialization

Its sole caller is `0x910D`, **in `main`, unconditionally**, between `0xECC5` and
`settings_restore` (`0xA283`). The regions it fills have now been read: `0x0964` and `0x0994`
(21 bytes each) to `0xFF`, `0x089D` (21 bytes), `0x009E` and `0x0DA1` (126 bytes each) to zero,
plus 9 bytes at `0x08B2` and 2 at `0x097D`. These are the per-key color arrays in XRAM, set
to their neutral state before settings are read back from flash. Nothing to do with
Bluetooth.

##### What the reset shortcut actually does

Two triggers set bit `0x2B.7`, and the consumer is `0x8725` in the wireless slow tick:

| Trigger | Site | Behavior |
| --- | --- | --- |
| Keyboard shortcut | key `0x04` -> arm `0x41B3`, `0x41BF` sets the bit then zeroes counter `[0x0961:0x0962]` | the consumer requires this 16-bit counter to reach **3000**: it is a **long press** |
| Received command | `0x0C53`, reached via subcode **`0x06`** of container `0x08` | preloads the counter to **3001**, forces prescaler `[0x08DB]` to **10**, and sets the bit: **immediate, host-driven** reset |
| Abort | `0x44AE` clears the bit | key released too early |

> **Correction.** This page initially wrote "subcode `0x05`", parsing the inline table at
> `0x0859` as *(key, address)* triplets. It is the reverse — see "The thirteen arms of received container
> `0x08`" below: dispatcher `0x4E45` reads *(address_hi, address_lo, key)*, with the key
> **last**. All keys in that table were therefore shifted by one entry. The trigger for
> `0x0C53` is **`0x06`**, and `0x05` arms a completely different branch (`0x0C4A`, a read).
>
> The justification given at the time — "the set of subcodes matches the one documented on the
> transmit side" — was wrong and has been removed: the thirteen **received** subcodes
> (`01`–`06`, `09`, `41`–`44`, `49`, `4A`) are not the **transmitted** set (`05`, `0A`, `41`–`44`, `49`,
> `4A`). They overlap without coinciding.

The prescaler `[0x08DB]` is what makes it *immediate*: `tick_slow` (`0x8FA7`) only performs its
work when `[0x08DB]` reaches **10**, and `0x819C` increments it by only one per tick. Setting it
to 10 passes the guard on the very next tick instead of waiting ten ticks. It is the counterpart to the counter
preloaded to 3001: one bypasses the long press, the other bypasses the wait.

This bit comes from the **shortcut dispatcher** `0x4136`, a call to the inline dispatcher `0x4E45`
followed by seventeen `(address_hi, address_lo, key)` triplets — that is indeed the order, verified
on the reset arm, whose tabulated address `0x41B3` lands squarely on the
two `JNB 0x4D` / `JNB 0x55` guards preceding the `SETB 0x2B.7` at `0x41BF`:

| Key | Arm | Role |
| --- | --- | --- |
| `0x04` | `0x41B3` | **factory reset** — sets `0x2B.7` |
| `0x05` `0x06` `0x07` | `0x41C8` `0x41FE` `0x4233` | Bluetooth slots **1**, **2**, **3** (`g_transport = 2`, `g_bt_slot = 1/2/3`) |
| `0x08` | `0x426A` | **re-pairing** — sets `0x2C.5` |
| `0x00` `0x01` `0x02` `0x0A` `0x0B` `0x11` `0x18`–`0x1D` | `0x4170` … `0x43D5` | other shortcuts |

The three Bluetooth arms can be read at a glance: each writes `g_transport = 2` with `A = 2`,
then reuses that same `A` for the slot — `14` (`DEC A`) for slot 1, nothing for slot 2, `04`
(`INC A`) for slot 3.

And here is the complete sequence, `0x8728`–`0x8799`, with **all eighteen calls enumerated**:

| Call | Role |
| --- | --- |
| `0xEF8D`, `0xEF46` | mark settings dirty + 20 ms delay |
| `0xADA2` | `rgb_fill_solid(0,0,0)` — black |
| `0xA069` | `settings_save` |
| **`0x91D3`** | restores default per-key colors from `CODE 0xCB7A` to `0x09BF`, `0x0A3D`, `0x0ABB` |
| **`0x77A0`** | restores the **factory keymap**: 512-byte blocks from `CODE 0xB400`, `0xB600`, `0xB800`… to `0x09BF`, then written to IAP pages `0x66`, `0x67`, `0x68` via `0xAAC1` |
| `0xADA2` ×6 + `0xED03` ×7 | blue 200 ms, black, green, black, red, black |

Then `CLR 0x1B` (backlight off), `SETB 0x51` and `SETB 0x21` (reload and reseed).

**None of these eighteen calls touches EUART0.** Not a single frame, not a single entry in the transmit
queue. It is a **layout and lighting** reset, not a pairing reset.

##### The second half of the same tick: restarting pairing

`0x8725` tests `0x2B.7` and, **if it is clear, jumps directly to `0x8799`** — that is, into
the remainder of the tick, not out of it. And `0x8799` tests `0x2C.5`. Both shortcuts therefore share the
long-press counter **and** the tick that consumes them, in this order:

```asm
0x870C  if [0x0961:0x0962] < 3000 -> 0x87F6      ; long press not yet reached
0x8721  counter = 0
0x8725  jnb 0x2b.7, 0x8799                       ; no reset -> try pairing
0x8728  ...  factory reset ...
0x8793  clr 0x2c.5                               ; <-- and it CANCELS pending pairing
0x8799  jnb 0x2c.5, 0x87C2
0x879C  [0x0E3B] = 0 ; clr 0x2c.5 ; delay 20 ms
0x87A6  if g_transport == 2 -> R5 = g_bt_slot
0x87B4  else if g_transport == 1 -> R5 = 0
0x87BD  R7 = 1
0x87BF  lcall 0xEF5A                             ; 01 01 01 <slot> 00 <sum>
```

Arm `0x426A`, which sets `0x2C.5`, **requires `g_transport == 1`**, i.e., 2.4 GHz. In practice,
the only frame emitted by this path is therefore `01 01 01 00 00 <sum>`: *restart pairing with the
dongle*. The `CLR 0x2C.5` at `0x8793` tells the rest of the story — when both bits are set, the
reset wins and pairing is abandoned, not queued.

A second consumer of `0x2C.5` exists at `0x44BD`, and it emits `R7 = 0`: a simple
reconnection. It requires `g_transport == 2`, whereas the only setter of the bit requires `g_transport == 1`.

> **Resolved.** Changing transport between setting and reading the bit is not hypothetical: that is
> what the Bluetooth slot keys do. `0x44BD` is the **key-release path** of
> `0x412C`, reached when usage `b3` is `5`, `6`, or `7` — the three arms that write
> `g_transport = 2`. The sequence is therefore: long press on the pairing key in 2.4 GHz mode (the bit
> is set), then press on a Bluetooth slot key (transport switches to 2), then
> release — and `0x44BD` sends the reconnection and clears the bit. See "The fourth dispatcher"
> below.

Moreover, `0xEF5A` is not just a transmitter: after `0xED89`, it clears bit `0x69` and reloads
`IDATA[0x18] = 6`, `IDATA[0x17] = 0xFF` — resetting the link supervisor from scratch.

##### Why the negative conclusion is solid

Pairings live in the BK3632, not in the SH68F90: erasing them would require a command over
EUART0. Yet all **eleven** transmitted commands have an established semantics, and the last one whose
trigger this page had left as "—" is now located: **`0x0C` is sent at `0x7D9B`,
during sleep preparation** (`0x7D74`), with parameters `7` and `8`, when `[0x0E3B]` is non-zero.
Codes `0x05`, `0x07`, and `0x0A` are **never** sent. And the sub-opcodes of container
`0x08` (`0x05`, `0x0A`, `0x41`–`0x44`, `0x49`, `0x4A`) are all data transfers from
`CODE` or the IAP region.

That leaves no candidate command **for erasing**. The port had chosen **not to
invent** an erase command; that was not merely cautious, it was right. The only
pairing action this firmware knows how to perform is the restart above, and it uses a
command the port already sends.

##### What the port could adopt, and does not

Factory reset itself is portable — SMK already has `settings`, `indicators_apply_defaults()`,
and a compiled-in default keymap. That would be a **new feature**, not the answer to
the question asked, and it is not added here.

On the code side, two things remain, and only one required a change:

- **nothing to port for erasure** — it does not exist;
- **re-pairing already exists** in `aula_rf.c` under the name `RF_LINK_PAIRING`, but it was sent with
  the two bytes swapped. This is now fixed: `rf_send_link()` places the flag in byte 2 and
  the slot in byte 3. The bug had no effect in 2.4 GHz mode, where both are zero; in Bluetooth mode it
  sent `01 01 <slot> 00`, requesting the dongle along with a spurious flag.

#### Resolved: `0x031B` is a field in a persisted settings block

The remaining open question — "no direct write of the value 1" — has a clean explanation:
`0x031B` is not an isolated variable; it is **byte 14 of a 128-byte block** that lives in
three places.

```
flash page 99 (0xC600, 128 B)
      |   fcn.0000A611     generic flash reader
      v                    base 0x0ED6:7, dest 0x0ED8:9, count 0x0EDA:B, movc @a+dptr
XRAM 0x09BF   staging block, 128 B
      |   fn.fx_init  @ 0xA2BB-0xA2D1     [0x09BF + i] -> [0x030D + i]
      v
XRAM 0x030D   LIVE block, 128 B
```

and the write-back path:

```
XRAM 0x030D --fcn.0000A069 @ 0xA083/0xA0C2--> XRAM 0x09BF --iap.save--> flash page 99
```

`fn.fx_init` is called twice by `vec.reset` (`0x9110`, `0x9174`): restoration indeed
takes place at startup. And right before saving, `fcn.0000A069` explicitly refreshes both
fields:

```asm
[0x09CB] = [0x0319]      ; Bluetooth slot     (offset 12)
[0x09CD] = [0x031B]      ; active transport   (offset 14)
mov r5,#0x63 ; r6,#0xC6  ; page 99 = 0xC600
lcall iap.save
```

The arithmetic closes the loop: `0x030D + 12 = 0x0319`, `0x030D + 14 = 0x031B`, and
`0x09BF + 12 = 0x09CB`, `0x09BF + 14 = 0x09CD`.

**Why the search saw nothing.** The live block is addressed **via computed pointer**
(`mov a,#0x0d ; add a,r7 ; mov DPL,a ; mov a,#0x03 ; addc a,r6 ; mov DPH,a`) from four sites,
and the staging block from roughly thirty. A generic "write config byte N" routine
therefore sets `0x031B` without ever emitting `mov dptr, #0x031b`. The value 1 arrives that way, or via
flash restoration — not via a hardcoded constant.

#### Verification against the dump

Block read at `0xC600` in `assets/f75_firmware.bin`:

```
0xC600  00 03 03 02 00 00 04 04 07 00 04 20 01 00 00 00
0xC610  00 00 00 00 02 01 00 ff 02 00 00 00 01 00 03 01
...
0xC670  09 37 09 37 04 09 04 04 04 04 04 04 04 04 5a a5
```

| Offset | Live XRAM | Value | Role |
| --- | --- | --- | --- |
| 12 | `0x0319` | **1** | Bluetooth slot |
| 14 | `0x031B` | **0** | active transport — **wired** |
| 126–127 | `0x038B`–`0x038C` | `5A A5` | validity marker |

`0x031B = 0`: the keyboard was in **wired mode** when the dump was taken — which is converter-accurate,
since `sinowisp` read it over USB. The entire chain flash → `0x09BF` → `0x030D` is confirmed
end to end by an observable value.

The `5A A5` at the tail of the block is the classic signature of a validity marker: it allows the
firmware to distinguish a written block from an erased page.

> This page 99 is the same one saved by `fcn.0000A069`, and it already appeared
> among the seven non-blank pages noted during the entropy analysis of the IAP region. The two
> observations, made independently, converge.

#### Mapping the block

The length is confirmed on both sides: `fn.fx_init` (`0xA292`) and `fcn.0000A5AD` (`0xA5B5`)
call `fcn.0000A611` with the same arguments — `r4:r5 = 0x09BF`, `r3:r2 = 0x0080`,
`r6:r7 = 0xC600`. **128 bytes**, in both directions.

**22 offsets out of 128 are addressed directly** via `mov dptr, #0x03NN`, all between 0 and 32.
The rest are only reached via computed pointer.

| Off | XRAM | Dump | Refs | What the firmware does with it |
| --- | --- | --- | --- | --- |
| 0 | `0x030D` | `00` | 20 | read very widely |
| 1 | `0x030E` | `03` | 4 | counter decremented then tested (`0x88D9`, `0x88E7`) |
| 3 | `0x0310` | `02` | 1 | `fcn.00003108`: scan bound → `IDATA 0x0E` |
| 5 | `0x0312` | `00` | 6 | flag (`jz`) |
| 6 | `0x0313` | `04` | 4 | copied to `0x0D19` |
| 7 | `0x0314` | `04` | 2 | copied to `0x0D9D` |
| 8 | `0x0315` | `07` | 4 | packed field (`anl #0x0F`, `anl #0x80`) |
| 9 | `0x0316` | `00` | 13 | **source of `XRAM 0x009D`** — the RGB effect |
| 10 | `0x0317` | `04` | 11 | range 0–14 (`cjne a, #0x0E`) |
| 11 | `0x0318` | `20` | 3 | paired with offset 10 in `fn.fx_init` and `fcn.0000A069` |
| **12** | `0x0319` | `01` | 16 | **Bluetooth slot** |
| **14** | `0x031B` | `00` | 57 | **active transport** |
| 15 | `0x031C` | `00` | 2 | written at `0x4197`, read by `fcn.00008FA7` |
| 16 | `0x031D` | `00` | 2 | same, `0x41AC` / `0x8FDA` |
| 17 | `0x031E` | `00` | 1 | read by `fcn.00008FA7` |
| 18 | `0x031F` | `00` | 3 | receives `0x0D16` when action class is 9 |
| 22 | `0x0323` | `00` | 3 | flag |
| 24 | `0x0325` | `02` | 1 | read by `fcn.00008FA7` |
| 26 | `0x0327` | `00` | 8 | flag |
| 27 | `0x0328` | `00` | 23 | tested `cjne a, #0x01` |
| 28 | `0x0329` | `01` | 1 | modulo-4 cyclic counter (`0x84C6`) |
| 32 | `0x032D` | `00` | 2 | |

Offsets 2, 4, 13, 19–21, 23, 25, 29–31 have no direct references and are 0 in the
dump: reserved, or only reached via the configuration protocol.

#### Two arrays, addressed via computed pointer

Searching for the pattern `mov DPL,a ; mov a,#0x03 ; addc a,rX ; mov DPH,a` yields the bases:

| Base | Offset | Stride | Verified index |
| --- | --- | --- | --- |
| `0x0345` / `0x0346` | 56 / 57 | **2** | `[0x009D] × 2` at `0x1166`, `0x8F5F`, `0x93F0` |
| `0x0362` | 85 | 1 | `[0x009D]` at `0x114E` |

The stride-2 array is a **two-byte per-effect record**, with packed
fields:

```asm
0x1170  movx a,@dptr ; rlc a ; mov 0x24.3, c   ; bit 7 of byte 0 -> flag 0x24.3
0x117E  movx a,@dptr ; anl a,#0x0f             ; low nibble of byte 1 -> 0x011C
0x8F6A  movx a,@dptr ; anl a,#0x8f             ; bits 7 and 3-0 of byte 1
```

And the tail of the dump shows this exact pairwise regularity:

```
off 56   ff ff
off 58   09 34 | 09 37 | 09 37 | 09 37 | 09 37 | 09 37
off 70   00 34 | 09 37 x13
off 98   07 47 | 07 47 | 07 44 x8
off 116  04 09 | 04 04 x5
off 126  5a a5
```

#### Resolution: two effect families, and a selector

The tension came from a false assumption on my part — that `XRAM 0x009D` was uniformly
`0x20`-based. It is not. `fn.fx_init` (`0xA2EC`) and `fcn.0000A069` (`0xA0D9`) carry the
exact same selector:

```asm
mov dptr,#0x0316 ; movx a,@dptr    ; offset 9
setb c ; subb a,#0x00              ; carry iff [0x0316] == 0
jc  prendre_0317
mov dptr,#0x0318                   ; [0x0316] != 0  ->  offset 11
sjmp ecrire
prendre_0317:
mov dptr,#0x0317                   ; [0x0316] == 0  ->  offset 10
ecrire:
movx a,@dptr ; movx [0x009d], a
```

There are therefore **two effect families**, and offset 9 tells which one is active:

| Offset | Role | Range | Dump |
| --- | --- | --- | --- |
| 9 | family selector | 0 / ≠0 | `0x00` |
| 10 | **family A** effect | 0–14 (`cjne a, #0x0E`) | `0x04` |
| 11 | **family B** effect | `0x20`+ | `0x20` |

On the dumped keyboard, the selector is 0 → **`XRAM 0x009D = 4`, family A**. That was the missing
piece: the values `0x20` / `0x26` / `0x2D` I had noted all belong to
family B, and had led me to overgeneralize.

#### The array aligns exactly

With the index `[0x009D] × 2` on base `0x0345`:

| Family | Index | Covered offsets | |
| --- | --- | --- | --- |
| A | 0 – 14 | **56 – 85** | entirely **within** the block |
| B | `0x20` – `0x2F` | 120 – 151 | **overflows** the 128 bytes |

Family A covers precisely the structured region of the dump (offsets 58–85), and is the one that gets
persisted. Family B records fall outside the block and live only in XRAM.

Decoding the record for the current effect (index 4 → offset **64**, bytes `09 37`):

| Field | Value | Destination |
| --- | --- | --- |
| `b0 & 0x1F` | 9 | `XRAM 0x0D19` (`0x1197`) |
| `b0` bit 7 | 0 | flag `0x24.3` (`0x1170`) |
| `b1 & 0x0F` | 7 | `XRAM 0x011C` (`0x117E`) |
| `b1 & 0x8F` | `0x07` | `0x8F6A` |

Two parameters per effect, 5 bits and 4 bits wide, plus two flag bits — the expected shape of a
*speed / brightness* pair with flags, even if the names still need confirmation.

#### `0x0D19` and `0x011C` — speed and color

Both destinations of the per-effect record are identified.

##### `XRAM 0x0D19` — **brightness** (27 references)

`fcn.0000A0F0` — called from twelve rendering sites — uses it as an index:

```asm
mov dptr,#0x0d19 ; movx a,@dptr
mov dptr,#0x2937 ; movc a,@a+dptr    ; gain = table[speed]
movx [0x0ef1], a ; mov r5, a
... lcall fcn.0000EF66   (r6:r7 = r7 x r5, 8x8 -> 16 multiplication)
... lcall fcn.00000056   with r5 = 0x50 = 80  (division)
```

Table `0x2937`:

```
00 08 10 18 20 28 32 3c 46 50 | ff
 0  8 16 24 32 40 50 60 70 80   end
```

**Ten entries, indices 0 to 9** — which matches the `subb a, #0x09` bound noted at
`0x7A18` and `0x7A86`. The gain ranges from 0 to 80, and dividing by 80 turns it into a 0-to-1 factor
applied to all three channels (`0x011D` → `0x0E46`, `0x0000` → `0x0E42`, `0x0E25` → `0x0E3F`).

The maximum is **per-effect**, read from a CODE table:

```asm
0x8C30  mov dptr,#0x009d ; movx a,@dptr
0x8C34  mov dptr,#0xa8d5 ; movc a,@a+dptr   ; bound = CODE[0xA8D5 + effect]
0x8C39  ... compares and clamps 0x0D19
```

Default value **4** (`0x1D10`, `0x1D31`). Value in the dump: **9**, which is the maximum.

> **Correction.** I initially named `0x0D19` "speed". It is **brightness**.
> `fcn.00000056` is indeed a division (`div ab`, verified), so `fcn.0000A0F0` computes
> `x × table[0x0D19] / 80` — a factor from 0 to 1 in ten steps. And the three quantities it scales
> are `0x0000`, `0x011D`, and `0x0E25`, all three written **right after the palette
> lookup** in `fcn.00004EA9` (`0x4ED9`, `0x4F1F`): they are the three color channels —
> **green**, **red**, and **blue** respectively, see *Which one is red — settled*.
> Three channels, a single factor — that is a brightness control, not speed.

##### `XRAM 0x011C` — color mode (30 references)

Six rendering functions — `fcn.00004EA9`, `0x5114`, `0x60C9`, `0x6C9B`, `0x7108`, `0x746A`, plus
`0xA791` — all begin with the same check:

```asm
mov dptr,#0x011c ; movx a,@dptr ; mov r7,a
xrl a,#0x07 ; jz  saute            ; 0x011C == 7 -> bypass
mov dptr,#0x0896 ; movx a,@dptr
mov 0xf0,#0x15 ; mul ab            ; x 21
add a,#0x00 ; mov DPL,a
mov a,0xf0 ; addc a,#0xc8 ; mov DPH,a   ; 0xC800 + n x 21
mov 0xf0,#0x03 ; mov a,r7 ; lcall fcn.00004E39   ; x 3
```

**21 bytes = seven RGB triplets**, and the index within the record is `0x011C × 3`. The table
is in **flash page 100 (`0xC800`)**, in the IAP region. The dump gives, identically for every
profile:

| Index | Color |
| --- | --- |
| 0 | `#FF0000` red |
| 1 | `#00FF00` green |
| 2 | `#0000FF` blue |
| 3 | `#FFFF00` yellow |
| 4 | `#FF00FF` magenta |
| 5 | `#00FFFF` cyan |
| 6 | `#FFFFFF` white |

So **`0x011C` from 0 to 6 selects a fixed color from the palette, and 7 is the
"no fixed color" case** — rainbow mode, the one that skips the palette lookup. Default
**7** (`0x1D2B`); value in the dump: **7**.

Its maximum is also per-effect, in `CODE[0xA8A3 + effect]` (`0x939F`, `0x93B4`).

#### `0x0D9D` — speed, and `0x0896` — per-effect palette

##### `XRAM 0x0D9D` — speed (26 references)

Restored from **offset 7** of the block (`0x1130`) and saved back to it (`0x8F4B`), bounded by
`CODE[0xA8BC + effect]` = 4, using the same increment/clamp mechanics as the others
(`0x8EED`–`0x8F30`).

Its defining feature: it indexes **fifteen CODE tables of five decreasing values**, each
hardcoded into a different rendering function.

| Table | Values |
| --- | --- |
| `0x2FE9` | 120 100 80 50 20 |
| `0x2FEE` | 60 50 40 30 20 |
| `0x2FF3` | 45 35 25 15 5 |
| `0x2FF8` | 45 35 25 15 6 |
| `0x2FFD` | 80 60 40 20 8 |
| `0x3002` | 20 15 10 5 1 |
| `0x3007` | 88 68 48 28 8 |
| `0x300C` | 115 100 85 60 30 |
| `0x3011` | 120 90 70 45 1 |
| `0x3016` | 30 24 18 12 6 |
| `0x301B` | 32 24 18 16 6 |
| `0x3020` | 115 95 75 55 30 |
| `0x3025` | 32 24 16 8 1 |
| `0x302A` | 46 36 26 16 6 |
| `0x302F` | 50 40 30 20 8 |

**Fifteen tables — and family A has exactly fifteen effects (0 to 14).** Five values, and the
maximum is 4. Decreasing: the higher the index, the shorter the period. This is speed,
with a range specific to each effect.

Value in the dump: **4**, which is the maximum.

##### `XRAM 0x0896` — the effect whose palette is read (23 references)

> **Its sole writer is identified**: `0x16EC`, the effect-change latch. See below,
> "The writer of `0x0896` — it is a latch, not a free variable".

The rendering functions compute `0xC800 + [0x0896] × 21`. The values against which `0x0896` is compared
are telling: `xrl a,#0x0d` (`0x5E0C`, `0x6630`), `cjne a,#0x0A` (`0xA3F7`, `0xA42D`),
`xrl a,#0x01` (`0x7476`), and `add a,#0xE0` — i.e., a comparison against `0x20`. **These are the
ranges of the two effect families.**

Verification against the dump, assuming `0x0896` = effect index:

| Effect | Address | Page | Palette |
| --- | --- | --- | --- |
| `0x00` | `0xC800` | 100 | red green blue yellow magenta cyan white |
| `0x04` | `0xC854` | 100 | *same* |
| `0x0E` | `0xC926` | 100 | *same* |
| `0x20` | `0xCAA0` | 101 | all black |
| `0x2D` | `0xCBB1` | 101 | all black |

The fifteen effects in family A each carry their own seven-color palette, all set
to the factory palette; those in family B are zeroed — they do not use a palette, which
is consistent with their lower color maximum (4 instead of 7). The addresses fall
entirely within pages 100 and 101 of the IAP region.

##### The writer of `0x0896` — it is a latch, not a free variable

This page long carried "the writer of `XRAM 0x0896`" as an open question, noting that
the animation dispatcher branches on `0x0896` **and not** on `0x009D`. The two questions are really
one, and the answer settles both together.

Out of **23 references** to `0x0896`, **only one is a write**: `0x16EC`.

```
16A6  jb   0x51, 0x16D7            ; already marked "needs reload"
16A9  mov  dptr,#0x0ED0 ; movx a,@dptr ; mov r6,a
16AE  mov  dptr,#0x0896 ; movx a,@dptr
16B2  cjne a,r6,   0x16D7          ; did the effect change?
16B5  mov  dptr,#0x0897 ; movx a,@dptr
16B9  cjne a,r7,   0x16D7          ; and the second parameter?
...                                 ; same for 0x0ED3/0x09AF (brightness) and 0x0ED4/0x0306 (speed)
16D4  ljmp 0x1AEA                  ; nothing changed -> normal rendering
16D7  clr  0x51
16DE  mov  dptr,#0x0896 ; movx a,@dptr ; cjne a,r6, 0x16EC
16E5  mov  dptr,#0x0897 ; movx a,@dptr ; xrl a,r7 ; jz 0x1702
16EC  mov  dptr,#0x0896 ; mov a,r6 ; movx @dptr,a    <== THE SOLE WRITE
16F1  mov  dptr,#0x0897 ; mov a,r7 ; movx @dptr,a
16F6  setb 0x21                    ; reseed the per-effect plane
16F8  clr  0x22
16FC  lcall 0xEDE7
16FF  lcall 0xEE6A                 ; and clear the panel
```

It is a **comparator-latch**: it only copies if the value has changed, and a change
triggers two things — bit `0x21`, which commands reseeding of the per-effect plane (that is the bit
`0x1A9E` checks before calling the transposer `0xACA3`), and a full clear of the panel.

**And the source is indeed `0x009D`.** Thirty bytes higher:

```
160D  mov dptr,#0x009D ; movx a,@dptr
1611  mov dptr,#0x0ED0 ; movx @dptr,a     ; [0x0ED0] = [0x009D]
1615  ... [0x0ED3] = [0x0D19] (brightness), [0x0ED4] = [0x0D9D] (speed)
162A  jnb 0x1B, +5 : [0x0ED0] = 0
1632  jnb 0x6C, +5 : [0x0ED0] = 0
163A  jnb 0x5B, +5 : [0x0ED0] = 0
```

Hence the complete four-stage chain:

> **`0x009D`** (*selected* effect, persistent setting, twelve writers including the shortcuts that set
> `0x20`, `0x26`, and `0x2D`) **→ `0x0ED0`** (*requested* effect, copied on every pass) **→ three gates**
> that force it to 0 **→ `0x0896`** (*active* effect, latched on change).

The three gates are status bits, and they explain why the effective value can differ
from the setting:

| Bit | Role | Writers |
| --- | --- | --- |
| `0x1B` (`0x23.3`) | **backlight on** | toggled by a shortcut (`b2 1b` at `0x42AC`, followed by marking "settings dirty"), cleared on wireless sleep (`0x8797`), set again on wake (`0x8816`) |
| `0x6C` (`0x2D.4`) | the bit that shuts off USB, already documented below — **sole writer: `fcn.00001DC3`**, the power manager | `0x1F25` / `0x1EF5`, `0x1F50` |
| `0x5B` (`0x2B.3`) | second power bit, written in the same function | `0x1F8E` / `0x1EF7`, `0x1F52`, `0x7EBF` |

**That is why the dispatcher reads `0x0896` and not `0x009D`**: `0x009D` is what the user
chose, `0x0896` is what should actually be displayed after passing through the power gates and
the backlight switch. It was not an orphan variable; it was the output latch
of a comparator.

##### `0x0897`, the other half of the latch

The comparator checks **a pair**, not a single value: `cjne a,r7` at `0x16B9`, and `R7` comes from
`0x1619` — `mov dptr,#0x011C ; movx a,@dptr ; mov r7,a`. `0x011C` is the **color mode** (the one
`0x1D2B` forces to 7 for random selection, and which `0x5108` consults). `R7` survives intact up to the
comparator.

So `0x0897` is the **latched copy of the color mode**, and the latch covers the
*(effect, color mode)* pair. That makes sense: changing the color mode requires the same reseeding and
clearing as changing the effect. The engines themselves read the current mode directly from `0x011C` —
`0x0897` only serves to detect changes.

##### `0x26` and `0x2D` are not effects, they are transient states

Three shortcuts write a literal value into `0x009D`: `0x20`, `0x26`, and `0x2D`. The last
two are **symmetrical**, and they save the previous state first:

| Site | What it does |
| --- | --- |
| `0x9184` | `[0x0E24] = current effect` then `[0x009D] = 0x26` |
| `0xA48B` | `[0x0E24] = current effect` then `[0x009D] = 0x2D` — and only if the current effect is `0x20` (`cjne a,#0x20` at `0xA47E`) |
| `0x1D40` | `[0x009D] = [0x0E24]` — the restoration, inside the `0x26` handler |
| `0xAFB8` | `[0x009D] = [0x0E24]` — the second restoration, preceded by a `cjne a,#0x20` check on the saved value |

**`0x26` is a transition animation.** Its handler `0x1D37` reads `XRAM 0x0ECD` — the vertical
direction of the snake — and **as soon as it returns to zero**, i.e., when the sweep has finished
traversing the grid, it clears the panel, restores `[0x0E24]`, and sets the reload flag again.
The factory firmware uses it as visual flair when switching modes, not as a user-selectable effect.

**`0x2D` renders nothing at all.** The dispatcher only checks `0x20` and `0x26` before its
`cjne a,#0x12` bound; `0x2D` exceeds it and falls through to `0x1DC2`, a `ret`. Its palette row is
moreover completely black. It is a state where the animation engine deliberately goes silent, entered
from gaming mode and restored by `0xAFB8`.

**Consequence for the port**: `AULA_FX_SNAKE_RGB` was initially exposed as a permanent effect in `indicators.c`, and later removed as redundant with `AULA_FX_SNAKE` (identical in all fixed-color modes). As for `0x2D`, it has no
counterpart and needs none: displaying nothing is `AULA_FX_OFF`.

##### What this changes for the port: nothing, and that in itself is a result

The port recomputes on every pass from `user_settings.led_effect`, and `fx_reset()` — called on
every effect change — plays the exact role of the `SETB 0x21` + `LCALL 0xEE6A` pair
(reseeding the seeder state, then `aula_rgb_clear()`). The three gates have their equivalents
elsewhere in SMK: turning lighting off is the `AULA_FX_OFF` effect, and sleep already goes through
`indicators_pwm_disable()` from `sleep.c` and `user_sleep_prepare()`. Not a single line to add —
convergence is verified, not assumed.

##### Offset 5 is the global / per-effect switch

```asm
0x111B  mov dptr,#0x0312 ; movx a,@dptr
0x111F  jz 0x1144                        ; == 0 -> PER-EFFECT parameters
        ... reads offsets 8, 6, 7  (0x0315, 0x0313, 0x0314)  -> global
0x1144  ... reads the per-effect array
```

The dump gives offset 5 as **0**, so the keyboard was in **per-effect** mode — which explains
why the effective brightness is 9 (from the record) rather than 4 (the global copy at
offset 6).

##### Bits 6-4 of `b1` — this is indeed speed

> **Correction of a correction.** In the previous pass I removed this lead, declaring it
> false. It was right: I had only searched for masks `0x0F` and `0x8F`, and missed
> `anl a, #0xF0` at `0x11A8`.

A **fourth** access to `b1`, in the "per-effect" branch of `fcn.00001111`:

```asm
0x119D  mov a,#0x46 ; add a,r5 ; ...      ; DPTR = 0x0346 + 2e   (b1)
0x11A7  movx a,@dptr
0x11A8  anl a,#0xf0                        ; bits 7-4
0x11AF  mov r0,#0x04 ; lcall fcn.00004DD8  ; right shift by 4 bits
0x11B4  mov dptr,#0x0d9d ; ...             ; -> speed
```

`fcn.00004DD8` shifts `r4:r5:r6:r7` right by `r0` bits — the counterpart of `fcn.00004DC5`
already encountered for the descriptor table. And writing it back, at `0x8F6A`, is the exact
inverse:

```asm
anl  a, #0x8f                   ; keeps bit 7 and bits 3-0 of b1
mov  dptr,#0x0d9d ; movx a,@dptr
mov  B,#0x10 ; mul ab           ; speed x 16
orl  a, r7                      ; b1 = (b1 & 0x8F) | (speed << 4)
```

Since speed is bounded to 4, `speed << 4` never exceeds `0x40` and therefore never touches
bit 7. **In practice, `b1[6:4]` is speed**, and `b1[7]` is a spare bit that the write
preserves.

#### Complete symmetry

| Parameter | Global | Per-effect | Per-effect bound |
| --- | --- | --- | --- |
| brightness | offset 6 — `0x0313` | `b0[4:0]` | `CODE[0xA8D5 + e]` = 9 |
| speed | offset 7 — `0x0314` | `b1[6:4]` | `CODE[0xA8BC + e]` = 4 |
| color | offset 8 — `0x0315[3:0]` | `b1[3:0]` | `CODE[0xA8A3 + e]` = 7 |
| direction | offset 8 — `0x0315[7]` | `b0[7]` | flag `0x24.3` |

Each parameter therefore exists in **two copies**, and offset 5 dictates which one is authoritative.

##### `XRAM 0x0312` (offset 5) — the switch

Six references, **all `jz` reads**, and — like `0x031B` — no direct write: it
is set by the configuration protocol via the block pointer.

| Site | Role |
| --- | --- |
| `0x111B` | restoration: global branch or per-effect branch |
| `0x7990`, `0x7A26` | same, in `fcn.00007928` |
| `0x8C85` | increment **brightness** — persist to `0x0313` or to `b0` |
| `0x8F45` | increment **speed** — to `0x0314` or to `b1[6:4]` |
| `0x93CE` | increment **color** — to `0x0315` or to `b1[3:0]` |

All three key handlers thus consult the same bit to know **where to write** the
modified value. Dump: **0** → per-effect mode.

##### `XRAM 0x0315` (offset 8) — the global counterpart of `b1`

```asm
0x1121  movx a,@dptr ; rlc a ; mov 0x24.3, c    ; bit 7  -> direction
0x1138  movx a,@dptr ; anl a,#0x0f ; -> 0x011C  ; bits 3-0 -> color
0x93D4  a = [0x0315] & 0x80 ; a |= [0x011C] ; [0x0315] = a     ; write-back
```

Dump: `0x07` → color 7 (rainbow), direction 0. Consistent with the per-effect record
(`b1 = 0x37` → color 7) — both copies agree.

##### Flag `0x24.3` — animation direction

Seven sites. Written from bit 7 of the setting (`0x1126` in global mode, `0x1172` in per-effect mode), cleared
by two rendering functions (`0x7474`, `0xEF8A`), read by three. The most telling is
`fcn.0000A3F4`:

```asm
0xA3F4  jnb 0x24.3, 0xA42D
0xA3FF  mov dptr,#0x08c3 ; movx a,@dptr ; add a,#0x14    ; +20
...
0xA435  mov dptr,#0x08c3 ; movx a,@dptr ; add a,#0xec    ; -20
```

**`+20` on one side, `−20` on the other**, on the same position counter `0x08C3` (which wraps
on `cjne a,#0x11`, i.e., 17). It is a **scrolling direction**.

##### Final readout of the dumped keyboard

| | Global | Per-effect (effect 4, `09 37`) |
| --- | --- | --- |
| brightness | 4 | **9** |
| speed | 4 | **3** |
| color | 7 (rainbow) | **7** (rainbow) |
| direction | 0 | **0** |

Offset 5 = 0, so the **per-effect** values apply.

##### Three per-effect bound tables

`0xA8A3`, `0xA8BC`, `0xA8D5` — spaced by `0x19` = 25:

| Base | Bound for | Family A value | Family B value |
| --- | --- | --- | --- |
| `0xA8A3` | `0x011C` (color) | 7 | 4 |
| `0xA8BC` | `0x0D9D` (speed) | 4 | — |
| `0xA8D5` | `0x0D19` (brightness) | 9 | 9 |

All three assignments are verified by direct disassembly: `0x8C34` for `0xA8D5`,
`0x939F` for `0xA8A3`, `0x15EB` / `0x8EF4` / `0x8F0B` for `0xA8BC`.

##### The record can now be read in full

For the current effect of the dumped keyboard (index 4, offset 64, bytes `09 37`):

| Bits | Value | Meaning |
| --- | --- | --- |
| `b0[4:0]` | 9 | **brightness**, at maximum |
| `b0[7]` | 0 | flag `0x24.3` |
| `b1[3:0]` | 7 | **color** = rainbow |
| `b1[6:4]` | 3 | not read by the identified masks — **unassigned** |
| `b1[7]` | 0 | flag (mask `0x8F`) |

#### And `5A A5`

The argument "this could be record index 35" is significantly weakened:
family B records overflow the block anyway (effect `0x26`, the default value
on reset, lands at offset 132), so the last two bytes of the block carry no
useful effect data. Combined with a complementary bit pattern (`0101 1010` / `1010 0101`) at the very
end of the structure, the **validity marker** reading is the best supported. It remains unproven.

### Reception: parser and response format

Reception takes place in **two stages**.

**1. The ISR only signals.** At the end of the **PWM0 ISR**
(vector `0x43` → `0x72BA`), at `0x7444`:

```asm
jnb  0x24.4, ...   ; hardware "data received" flag, set by the EUART0 ISR
clr  0x24.4        ; acknowledge
setb 0x2d.7        ; deferred processing flag
```

**2. `fcn.000005EA` processes outside interrupt context** (`0x05F3`):

```asm
clr   0x2d.7                 ; acknowledge
mov   r1, #0x54              ; source: RX buffer in IDATA 0x54
mov   r7, #0x17              ; 23 bytes
lcall fcn.00004c4b           ; copy to XDATA 0x0120
mov   r0,#0x70 ; mov a,@r0   ; RX write index
```

**Received frames are therefore parsed in XDATA `0x0120`.**

#### Response format

```
[0] = 0x02        RESPONSE header   (transmitted commands use 0x01)
[1] = command     e.g. 0x06
[2] = status / parameter
[3..8] = data
[9] = checksum
```

**Verified checksum** (`0x0650`):

```asm
clr  c
mov  a, #0x55
subb a, 0x08        ; 0x55 - sum(bytes 0..8)
mov  dptr,#0x0129 ; movx a,@dptr   ; byte [9]
xrl  a, 0x08 ; jz                  ; otherwise the frame is rejected
```

→ **`checksum = 0x55 − Σ(bytes 0..8)`**, **10-byte** frames.

After validation, the fields are dispatched: `[4]` → XRAM `0x09BA`, `[5]` → XRAM `0x09AC`,
`[7]` → chain passing through `0x02E6`.

#### Multiple frame types

Byte `[0]` is dispatched on **`0x02`, `0x03`, `0x08`, and `0x15`** (`0x0612`, `0x077D`, `0x0797`,
`0x07BD`). Byte `[2]` is then tested against `0x08`, `0x05`, `0x40`.

For type `0x03`: sets `0x2C.3`, zeroes XRAM `0x0150`, calls `fcn.0000ACE6` with `r7 = 0xF0`,
then loads `IDATA 0x18 = 0x06` and `IDATA 0x17 = 0xFF`.

#### 🔑 Wireless controls RGB

In the branch around `0x07D5`–`0x07FC`, the parser reads bytes **`[3]`, `[4]`, `[5]`** of the
received frame, then writes:

```asm
0x07f0   mov dptr,#0x038e ; clr a ; movx @dptr,a    ; 0x038E = 0
0x07f5   inc dptr ; mov a,r5 ; movx @dptr,a         ; 0x038F = r5
0x07f8   mov a,r7 ; swap a ; anl a,#0x0f            ; high nibble
0x07fc   mov dptr,#0x038d ; movx @dptr,a            ; 0x038D = effect sub-index
```

**`0x038D` is the RGB effect sub-index** (see the selection chain above). A frame coming
from the BK3632 can therefore **change the lighting effect**. In other words, the wireless path relays
configuration to the 8051 — presumably from a host application over Bluetooth or the
2.4 GHz dongle.

This is the third effect source, alongside the Fn keys and the USB path.

### The decoded remap table — factory layout

The table at `0xCE00` (page 103) contains **84 non-zero records out of 128**, within
indices 0 to 89. Read **column by column**, six rows per column, they give:

```
      c0    c1    c2    c3    c4    c5    c6     c7     c8    c9    c10   c11   c12   c13   c14
 L0   Esc   .     Lum+  Lum-  Tab   H     cl3/2  cl3/1  Prec  Play  Suiv  Muet  Vol-  Vol+  (07)
 L1   `     1     2     3     4     5     6      7      8     9     0     -     =     BSpc  Del
 L2   Tab   Q     W     E     R     T     Y      U      I     O     P     [     ]     \     PgUp
 L3   Caps  A     S     D     F     G     H      J      K     L     ;     '     #     Ent   PgDn
 L4   MOD2  Z     X     C     V     B     N      M      ,     .     /     MOD32 \|    Up    End
 L5   MOD1  MOD4  MOD8  .     .     Spc   .      .      Fn?   MOD16 .     .     Left  Down  Rght
```

**Rows 1 to 4 are the exact QWERTY block**, column by column, without a single anomaly. Row
5 is the bottom row, row 0 the function row. This validates at a stroke the
6 × 15 geometry and the scan order established elsewhere — matrix, pinout, and keymap all match.

#### Correction: `b0` is not a modifier mask

I had read the record as `[modifiers, class, parameter, usage]`, with `0x02` =
LeftShift. That is wrong, and the 24-bit shift shows why:

```asm
mov r0, #0x18 ; lcall fcn.00004DC5      ; 24-bit shift
mov dptr, #0x0d18 ; mov a, r7 ; movx @dptr, a
```

`b0` goes to **`XRAM 0x0D18`** — the byte I had already identified as the one copied into the
HID report buffer (`0x0D18 → 0x08C0`). It is therefore a **report selector**, not a mask.

Which explains the entries that made no sense as modifiers: `b0 = 0x02` with
`b3 = 0xB5` cannot be "LeftShift + keyboard usage 0xB5", because `0xB5` is not a
keyboard usage. It is a **Consumer** usage:

| `b3` | Consumer Usage |
| --- | --- |
| `0x6F` / `0x70` | screen brightness − / + |
| `0xB5` / `0xB6` | next / previous track |
| `0xCD` | play / pause |
| `0xE2` | mute |
| `0xE9` / `0xEA` | volume + / − |

**The function row therefore carries media controls, not F1–F12** — the factory setting has
the Fn layer inverted, which is common practice on these keyboards.

The record finally reads:

```
b0  report selector   -> 0x0D18   (nine values, all decoded: see below)
b1  action class      -> 0x0D17
b2  parameter         -> 0x0D16
b3  usage             -> 0x0D15
```

#### The fourth dispatcher — `0x8845`, nine report selectors

The line above previously read "others TBD". That is no longer needed: `b0` is the key for a
**fourth call** to the inline dispatcher `0x4E45`, at `0x8845`, and its nine entries enumerate
the exact values `b0` can take.

##### The chain, from key press to dispatcher

```asm
0x699A  a = [0x030D] ; a += a ; dptr = 0xD800 + [0x030D] * 512   ; active PROFILE page
0x69B4  a = [0x02E0]                                             ; key index
0x69B9  lcall 0x8818
        0x8818  mov b,#4 ; lcall 0x4E39      ; dptr += index * 4  -> the record
        0x881E  lcall 0x4DEC                 ; load the 4 bytes
        0x8821  mov r0,#0x18 ; lcall 0x4DC5  ; shift by 24 bits
        0x882A  movx [0x0D18], r7            ; b0
        0x8844  a = [0x0D18] ; lcall 0x4E45  ; <<< dispatch on b0
```

`b1` has already been extracted by the caller (`0x6992`, 16-bit shift) — both dispatchers
therefore operate on the **same four-byte record**, with `b0` choosing the path and `b1`
the action within that path. Only two callers: `0x69B9` and `0x6A87`.

Each arm begins with the same guards as the shortcut dispatcher — `0x4D` and `0x55`
set ⇒ abort — encoded sometimes as `JNB`+`LJMP`, sometimes as a short `JB`: it is the same condition.

##### The nine paths

| `b0` | Arm | Path |
| --- | --- | --- |
| `0x00` | `0x7018` | **keyboard report** |
| `0x01` | `0x85FB` | **mouse report**, via a thirteen-entry jump table |
| `0x02` | `0x001E` | **Consumer report** |
| `0x03` | `0xA37E` | **radio transmit queue** |
| `0x04` | `0xB041` | three-byte raw report |
| `0x07` | `0x412C` | **keyboard shortcuts** |
| `0x08` | `0xA674` | **action-class dispatcher** on `b1` |
| `0x09` | *inline `0x88B8`* | maintains a 0–3 value in `[0x030E]` |
| `0x0D` | `0xAD64` | sets two held status bits |
| *default* | `0x8919` | **`RET`** — an unknown `b0` does nothing |

##### What each path does, and what it confirms

**`b0 = 0` — keyboard report.** Here `b1` serves as a **mask**: `[0x08B2] |= b1` on press,
`[0x08B2] &= ~b1` on release; `b3` and `b2` go into the report via `0x8CF8` (press) or
`0x3034` (release). This is the path that **validates the "six modifiers" reading** in the next
section: positions carrying a power of two in `b1` go through here, and this is where the
mask is applied.

**`b0 = 1` — mouse report.** A jump table at `0x865E`, thirteen entries indexed by
`b1 - 1`. Full details below.

**`b0 = 2` — Consumer report.** `[0x09BD] = b3`, `[0x09BE] = b2`, bit `0x50` set. This is the exact
structure described elsewhere on this page: `0x09BC` holds Report ID 2 and `0x09BD:0x09BE`
holds the 16-bit Consumer usage. Both findings converge.

**`b0 = 3` — radio transmit queue.** Writes `[0x02CE]`–`[0x02D2]`, then copies 8 then 16 bytes
via `memcpy_idata` into a **28-byte per-host** structure indexed by `g_radio_host_idx`,
starting at `0x0C57`. **This is the six-slot, 28-byte ring buffer** identified on the radio side
of this page, reached here via a completely different path.

**`b0 = 4` — raw report.** `[0x08C0] = b0`, `[0x08C1] = b2`, `[0x08C2] = b3` on press, zeros on
release, bit `0x3C` set.

**`b0 = 7` — shortcuts.** `0x412C` calls `0x4E45` with **`b3`** as the key: this is the
`0x4136` dispatcher described above. **The shortcut key is therefore the usage byte of
the remap record**, which closes the loop: keys `0x00`–`0x1D` of that table are
`b3` values in the profile page.

Its **release** path branches on `b3`:

| Released `b3` | Effect |
| --- | --- |
| `4` | zeroes the long-press counter (aborts reset) |
| `5`, `6`, `7` | if `g_transport == 2` and `0x2C.5` is set: `rf_link_select(bt_slot, 0)`, then clears the bit |
| `0x0A` | zeroes `[0x0C52:0x0C53]` |
| `0x1D` | mute / effect family |

**`b0 = 8` — class dispatcher** `0xA674`, already described above: classes 0 to 9, where 5/6/7
share `fx_mode_toggle`.

**`b0 = 9` — a 0–3 value in `[0x030E]`**, with four operations depending on `b1`: `0` loads `b2`,
`1` increments capped at 3, `2` decrements floored at 0, `3` cycles 0→1→2→0. Then
`[0x08C0..0x08C2] = 9, 0, 1`.

> **`[0x030E]` is never read anywhere.** Searching for all `mov dptr,#0x030E` accesses across the entire
> image yields **four sites, all inside this arm**, and the address is out of reach of `MOVX @Ri`
> since `P2` is only ever zero. This `b0` therefore maintains a setting that nothing consumes in
> this firmware. Observed, not explained — and not to be confused with `[0x030D]`, which is read at
> eighteen sites, all using the same `0xD800 + [0x030D] * 512` idiom.

**`b0 = 0x0D` — two held bits.** Depending on whether `b1 == 0` or not, and on press vs. release, sets or
clears bits **`0x44`** and **`0x43`**. These are **the exact two bits that the effect
`0x2D` arm (`0x1A04`) checks** before calling `0x8A1A`. A key on this path therefore gates the
per-frame work of that effect — two separately decoded tables locking together.

#### The last three jump tables — `0x391C`, `0x0495`, `0x5817`

The inventory previously listed three tables "outside the key-press path, undecoded". They are now
decoded, and they fit together: **one is the transfer sequencer, and the other two are
the USB counterpart to the radio `0x08` container**.

##### `0x391C` — the sequencer, ten handlers

`FUN_CODE_3901`, called by `euart0_parse`, branches on `[0x0D9E] - 1`, bounded to 10:

| Handler | Arm | State |
| --- | --- | --- |
| 1 – 7 | `0x393A`, `0x3975`, `0x3A25`, `0x3AB5`, `0x3BA7`, `0x3C42`, `0x3CD0` | already described above |
| **8**, **9** | `0x3D25` | **the common tail — they do not exist** |
| **10** | `0x3D1D` | `[0x0D9E] = 0` then `lcall 0xB093`: **explicit end of transfer** |

The open question "sequencer handlers 8, 9, and 10" is therefore settled: two of them are not
handlers at all — the table routes them to the same address as out-of-bounds — and the
third is the closing step, which disarms the sequencer and sends a final `0x08` frame.

> Note on opcode `0x4A`. This page previously wondered whether it might be the end-of-transfer marker. That is
> less likely now that a dedicated closing handler has been identified. The question remains
> open, but on a different footing.

##### Where `IDATA[0x77]` comes from — the USB path

The next two tables branch on `IDATA[0x77]`, which six sites **read** and no direct idiom
writes. The decompiler settles it: `FUN_CODE_0200` — called by `FUN_CODE_9926`, itself
called by **`isr_usb`** — copies the endpoint buffer `XDATA 0x1100` to `IDATA` **starting at
`0x76`**. `IDATA[0x77]` is therefore **the command byte of a report received on EP0**, and `IDATA[0x78]`
is the following byte.

This is the software path of the keyboard — the one OpenRGB uses to talk to the device.

##### `0x0495` — the eight WRITE commands

Index = `IDATA[0x77] - 3`, keys **3 to 10**. Value `0xAA` is caught before the table, at `0x0473`.

| Key | Arm | Effect |
| --- | --- | --- |
| `3` | `0x04AD` | `CODE` base according to `[0x038D]` — `0xCC00`, `0xD000`, `0xD400`, `0xD800` — then `0xEE4E` |
| `4` | `0x04DF` | `0xEEF4`: erase + write of **page 99** (`0xC600`) |
| `5` | `0x04E5` | `0xDC00` + sequence × 512, then `0xEE4E` |
| `6` | `0x0505` | `0xCA00` + sequence × 512, buffer copy, then `0xEE4E` |
| **`7`** | `0x05E9` | **nothing** — same address as out-of-bounds |
| `8` | `0x056E` | in wired mode: sets two bits and zeroes `[0x0D9F:0x0DA0]` |
| **`9`** | `0x05E9` | **nothing** |
| `10` | `0x0584` | `0xC800` + sequence × 512, then `0xEE4E` |
| `0xAA` | `0x05A8` | `[0x08C0] = IDATA[0x77]`, `[0x08C1] = IDATA[0x78]`, `[0x08C2] = 0` — an **echo** |

##### `0x5817` — the nine READ commands

Index = `IDATA[0x77] - 0x82`, keys **`0x82` to `0x8A`**. Each arm loads a source pointer into
`[0x0F45:0x0F46]`, copies the length from `[0x038E:0x038F]` into `[0x0F3A:0x0F3B]`, sets
`[0x0F4D] = 1`, and arms **EP0** (`EP0CON |= 4`). The response therefore goes out over the control
endpoint, not an interrupt channel.

| Key | Arm | Source |
| --- | --- | --- |
| `0x82` | `0x5832` | template `CODE 0xB3E3` or `0xB3FC` depending on `[0x038D]` |
| `0x83` | `0x5852` | `0xCC00` / `0xD000` / `0xD400` / `0xD800` + sequence × 512 — the **four remap profiles** |
| `0x84` | `0x58AA` | `0xC600` + sequence × 512 — the **settings block** |
| `0x85` | `0x58BB` | `0xDC00` + sequence × 512 |
| `0x86` | `0x58CB` | `0xCA00` + sequence × 512 — **per-key lighting** |
| `0x87` | `0x58DB` | two **status** bytes built in RAM (`[0x0095]`, `[0x0096]`) |
| `0x88` | `0x591A` | `(sequence × 2 + 1) << 8 \| 0x52` — i.e., `0x0152` at sequence 0 |
| `0x89` | `0x5949` | common tail only: arms EP0 without changing the source |
| `0x8A` | `0x592F` | `0xC800` + sequence × 512 |

##### What these two tables establish

**The base addresses are the exact same ones as in the `0x08` container received over radio** — `0xC600`, `0xC800`,
`0xCA00`, `0xCC00`/`0xD000`/`0xD400`/`0xD800`, `0xDC00` — with the same slicing by sequence
number and the same flash writer `0xEE4E` → `0xAAC1`.

The keyboard therefore exposes **the same configuration over two transports**, using two distinct command
sets: `0x01`–`0x09` and `0x41`–`0x4A` on the radio side, `3`–`10` (write) and `0x82`–`0x8A` (read)
on the USB side. This is not a redundant readout: they are two different physical paths,
decoded separately, that land on the exact same flash pages.

And both tables have their own gaps — keys `7` and `9` in `0x0495`, handlers `8` and `9` in
`0x391C` — which land on the out-of-bounds address. The firmware reserves codes it does not
implement.

#### Table `0x865E` — the thirteen mouse actions

`b0 = 1` leads to `0x85FB`, which separates press from release on bit `0x42`, then, on press,
branches through a computed jump table:

```asm
0x8643  a = [0x0D17] ; dec a ; cjne a,#0x0D ; jc 0x8650   ; bound: b1-1 < 13
0x864D  ljmp 0x86D0                                       ; out of bounds -> tail
0x8650  dptr = 0x865E ; b = 3 ; mul ab ; ... ; jmp @a+dptr
```

##### What it is, proven by the HID descriptor

The five variables these arms write — `[0x09B1]`, `[0x09B2:0x09B3]`, `[0x09B4:0x09B5]`,
`[0x09B6]`, `[0x09B7]` — are the **mouse report**. The HID descriptor, at `CODE 0x5F7D`, spells it
out word for word:

```
05 01 09 02        Usage Page (Generic Desktop), Usage (Mouse)
a1 01  85 07       Collection (Application), Report ID 7
09 01  a1 00       Usage (Pointer), Collection (Physical)
05 09  19 01 29 05   Usage Page (Button), Usage Min 1 .. Max 5   <-- FIVE buttons
75 01 95 05 81 02    5 bits, + 95 03 81 01: three padding bits
05 01  16 00 80 26 ff 7f   Logical Min -32768, Max 32767
09 30 09 31  75 10 95 02 81 06   Usage X, Usage Y, SIXTEEN bits each, relative
15 81 25 7f  09 38  75 08 95 01 81 06   Wheel, eight bits, -127..127
05 0c  0a 38 02  ...                    Usage Page (Consumer), AC Pan
```

The correspondence is exact, field for field:

| Report field | Size | Variable |
| --- | --- | --- |
| buttons 1 to 5 | 5 bits | `[0x09B1]` |
| X | 16 bits | `[0x09B2]` (low byte) `:` `[0x09B3]` (high byte) |
| Y | 16 bits | `[0x09B4]` `:` `[0x09B5]` |
| wheel | 8 bits | `[0x09B6]` |
| AC Pan | 8 bits | `[0x09B7]` |

The little-endian byte order is verified by converter `0x568C`, which sign-extends an 8-bit
delta from `[0x0F66]`: it writes `0xFF` or `0x00` into `[0x09B3]` depending on `ACC.7`, then writes the delta
into `[0x09B2]`.

##### The thirteen actions

| `b1` | Arm | Action |
| --- | --- | --- |
| `1` | `0x8685` | **button 1** — `[0x09B1] \|= 0x01` |
| `2` | `0x8685` | **button 2** — `\|= 0x02` |
| `3` | `0x8685` | **button 3** — `\|= 0x04` |
| `4` | `0x8685` | **button 4** — `\|= 0x08` |
| `5` | `0x8685` | **button 5** — `\|= 0x10` |
| `6` | `0x8699` | **AC Pan +1** — `[0x09B7] = 0x01` |
| `7` | `0x869E` | **AC Pan −1** — `[0x09B7] = 0xFF` |
| `8` | `0x86A3` | **wheel +1** — `[0x09B6] = 0x01` |
| `9` | `0x86AB` | **wheel −1** — `[0x09B6] = 0xFF` |
| `10` | `0x86B3` | **X = −1** — `[0x09B2:0x09B3] = 0xFF, 0xFF` |
| `11` | `0x86B8` | **X = +1** — `= 0x01, 0x00` |
| `12` | `0x86BD` | **Y = −1** — `[0x09B4:0x09B5] = 0xFF, 0xFF` |
| `13` | `0x86C7` | **Y = +1** — `= 0x01, 0x00` |

The first five share a single body because they differ only by bit position: `0x8685` reads
`CODE 0xEF04[b1-1]`, complements it, and applies it with `ORL`.

##### `CODE 0xEF04` — one table, two uses

```
fe fd fb f7 ef df bf 7f | 01 02 04 08 10 20 40 80
```

Eight **clear** masks followed by eight **set** masks. The press path reads the
clear mask and `CPL`s it to obtain the set mask; the release path
uses it as-is. A single sixteen-byte table serves both directions.

##### `b2` is the repeat period

After the arm, the tail `0x86D0` runs **if `b2 ≥ 2`**:

```asm
[0x0C37 + (b1-1)] = b2 * 2      ; repeat countdown
[0x0390 + (b1-1)] = b3
[0x0C45 + (b1-1)] = b3
setb 0x41                       ; repeat armed
```

And on release, if `b2 == 0xFF`, it zeroes `[0x0C37 + (b1-1)]` and `[0x0390 + (b1-1)]`. Buttons
(`b1 ≤ 5`) additionally have their bit cleared in `[0x09B1]`.

This countdown is consumed by a **twin block**: `0x7B17`, with its own jump table at
`0x7B25`, **thirteen entries of the same shape and the same functional targets**, which reapplies the action
as long as the key is held and decrements `[0x0C37 + (b1-1)]` at `0x7BAE`. `b2` is therefore the
**auto-repeat cadence** of the mouse action, in units of two ticks.

##### Inventory of jump tables in the image

With this one, the count is complete. **Four** inline tables read by dispatcher `0x4E45` —
`0x0859`, `0x1744`, `0x4139`, `0x8848` — and **five** `LJMP` tables addressed via
`jmp @a+dptr`:

| Site | Table | Entries | Subject |
| --- | --- | --- | --- |
| `0x0487` | `0x0495` | 8 | — |
| `0x390E` | `0x391C` | 10 | — |
| `0x5809` | `0x5817` | 9 | — |
| `0x7B17` | `0x7B25` | 13 | mouse repeat |
| `0x8650` | `0x865E` | 13 | **mouse actions** |

The first three remain undecoded; they are not on the key-press path.

#### The six modifiers, and resolving the tension on `0x0D17`

Six positions carry a **power of two** in the action class, with no usage:

| Position | Class | Key |
| --- | --- | --- |
| L4 c0 | 2 | Left Shift |
| L4 c11 | 32 | Right Shift |
| L5 c0 | 1 | Left Ctrl |
| L5 c1 | 4 | Left Win |
| L5 c2 | 8 | Left Alt |
| L5 c9 | 16 | Right Ctrl |

These are **the exact six modifier keys**, and nothing else in the table carries a
power of two. This resolves the long-standing tension on `0x0D17`: the field is indeed
used **both** as a class index (0 to 9, via jump table `0xA68D`) and as a
**mask** (the `0x08B2 &= ~0x0D17` at `0x7058`) — depending on whether it designates an action or a
modifier. The `< 10` bound of the dispatcher lets 1, 2, 4, and 8 through, but rejects 16 and 32, which
exist only on the mask path.

> *Note:* the bit order does not follow the HID modifier bit order (`0x04` lands on Win and
> `0x08` on Alt, whereas HID specifies the reverse). The mask is applied to `0x08B2`, an internal state
> byte — its numbering has no reason to match the HID report's, and the
> conversion happens elsewhere.

#### Two special entries

- **L5 c8** (index 53): `0D 00 00 00` — report selector `0x0D`, no class, no
  usage. The position corresponds to **Fn** on a 75% ANSI layout, a key that the firmware handles
  internally without ever emitting it.
- **L0 c14** (index 84): `07 00 00 1D` — report `0x07`, usage `0x1D`. This is the position that
  the matrix analysis had assigned to **the encoder press**.

*These two readings are inferred from position; the contents of the records themselves are read directly.*

#### `cl3/2` and `cl3/1` — RGB brightness

The two entries `08 03 02 00` and `08 03 01 00` on the function row are action class
**3**, whose handler is `0x8C0F`. The C equivalent is unambiguous:

```c
if (g_key_hid_usage != 0) goto rapport;     /* an entry carrying a usage skips all of this */
switch (g_key_param) {                       /* b2 of the record */
  case 1:  if (brightness < tbl_max_brightness[effect]) brightness++;  break;   /* increase */
  case 2:  if (brightness != 0)                          brightness--;  break;   /* decrease */
  case 0:  brightness++; if (brightness > max) brightness = 0;          break;   /* cycle */
}
```

So **`cl3/1` = brightness +**, **`cl3/2` = brightness −**, and parameter `0` would cycle. On the
function row, these are the two backlight brightness keys.

Reaching either limit (0 or maximum) sets `0x2B.6` and loads `0x0D1B = 6` — an end-of-travel
signal, likely the confirmation blink.

This handler **confirms three readings established elsewhere**, via independent paths:

1. the maximum indeed comes from `tbl_max_brightness[effect]`, table `0xA8D5`;
2. persistence follows the exact switch at offset 5:

```c
if (g_settings_per_effect == 0) {
    if (g_effect_family_sel == 0)  fx_params[effect].b0 = (b0 & 0x80) | brightness;
    else                           [0x0362 + effect]    = brightness;
} else                             g_global_brightness  = brightness;
```

3. and the `0x80` mask in this write **preserves bit 7 of `b0`** — the direction flag whose
   existence I had deduced from the read side (`rlc a ; mov 0x24.3, c`). The write side confirms it:
   it is indeed a separate field, preserved across brightness changes.

> One nuance: reading masks `b0 & 0x1F`, while writing only preserves bit 7 and thus leaves
> seven usable bits. Since brightness is bounded to 9, the difference never manifests.

### Frame `0x03` — connection announcement

This is the shortest of the parser's three branches, and **the only one that verifies no
checksum** — frames `0x02` and `0x08` both verify it.

```asm
0x0778  a = [0x0120]              ; byte 0 of the frame
0x077D  cjne a, #0x03, autre
0x0780  setb 0x2C.3
0x0782  clr a ; movx [0x0150], a  ; idle counter reset to zero
0x0787  r5 = 0 ; r7 = 0xF0
0x078A  lcall euart0_reply        ; -> frame 01 F0 00 00 00 <ck>
0x078D  IDATA[0x18] = 0x06        ; bank 3, R0
0x0790  IDATA[0x17] = 0xFF        ; bank 2, R7
0x0793  ljmp sortie
```

The two register writes are not decorative: each has **a single, identified
consumer**, and that is what reveals the meaning of the frame.

| Write | Consumer | Effect |
| --- | --- | --- |
| `[0x0150] = 0` | idle branch of `0x3901` | resets the deadline for the next `0x06` status request to one hundred ticks |
| `BANK3_R0 = 6` | tail of `euart0_parse`: `if (++R0 > 5) { R0 = 0; 0x801F(); }` | **the very next frame triggers the battery routine** |
| `BANK2_R7 = 0xFF` | `0x801F`: `if (R7 == 0xFF) { R7 = [0x0151]; set flag }` | **the displayed gauge jumps straight to the actual value** instead of converging gradually |

In other words: upon receiving a `0x03` frame, the keyboard resets the link timer,
**bypasses gauge damping** so that it immediately displays the true
level, schedules an immediate battery report, and sends an acknowledgment.

This is the behavior of a **(re)connection announcement**: you do not slowly ramp a battery
gauge when the link has just been established, you display it right away. The absence of a checksum points
the same way — a minimal service frame.

*The name "connection announcement" is inferred from these three effects; the effects themselves are read directly.*

#### The two acknowledgments

`euart0_reply(r7, r5)` builds `01 <r7> <r5> 00 00 <sum>`. The only two calls in the parser:

| Site | Transmitted frame | Trigger |
| --- | --- | --- |
| `0x078A` | `01 F0 00 00 00 <ck>` | `0x03` frame received |
| `0x0811` | `01 F1 00 00 00 <ck>` | `0x08` frame whose `byte[2] & 0x7F != 8` |

`0xF0` and `0xF1` are therefore two **acknowledgment codes** in a range distinct from ordinary
commands (`0x01`–`0x0E`).

#### A flag that is never consulted

`0x2C.3` is set here, and cleared in only one place — `fcn.0000929E` (`0x935E`), an initialization
routine that fills a region with `0xFF`, writes `[0x0C44] = 2`, and also clears `0x26.7`.

**No bit instruction tests it, and no byte read does either.**

> **Correction.** I had previously noted three `orl a, 0x2c` instructions at `0x7AD0`, `0x7AEE`, and `0x7BF1`, and
> concluded there might be an aggregate test. Those were **three false positives**. Verified by disassembly:
>
> ```
> 0x7ACF  MOV A, #0x45      ; bytes 74 45
> 0x7AD1  ADD A, R4         ; byte  2C
> ```
>
> The bytes `45 2C` are the immediate operand of `mov a,#0x45` followed by the opcode of `add a, r4`. The idiom
> `mov a,#0x45 ; add a,r4` builds a pointer to `0x0C45 + r4`, and it appears three times.
> After instruction-by-instruction validation: **zero** accesses to byte `0x2C` across the entire
> image.

`0x2C.3` is therefore **a dead flag**: set upon receiving a `0x03` frame, cleared by the
initialization routine `fcn.0000929E`, and never consulted — neither by bit nor by byte.

### The last two points: opcode `0x4A` and bit `b1[7]`

#### `0x4A` is a status query, not an end of transfer

This page previously assumed "probably an end of transfer", because the opcode arms a single
two-byte block whose payload is zeroed. Handler 7 (`0x3CD0`) tells a different story:

```c
[0x0D9E] = 0;                                  /* disarmed upfront: single pass */
IDATA[0x35] = 0x13;                            /* response subcode */
IDATA[0x36] = 0x4A;                            /* opcode echo */
IDATA[0x37] = [0x02E2];                        /* block count */
IDATA[0x38] = [0x0D14];                        /* sequence number */
IDATA[0x39] = ([0x038D] << 4) | [0x038F];      /* sub-index and length */
memset(&IDATA[0x3A], 0, 15);                   /* <<< the "zero-filled payload" */
IDATA[0x3A] = [0x0C36];
IDATA[0x3B] = 0;
if (!bit 0x2D.3) IDATA[0x3B]  = 0x01;
if ( bit 0x26.0) IDATA[0x3B] |= 0x10;
... sum over IDATA[0x35..0x48], then euart0_send
```

The fifteen-byte `memset` followed by populating **two** bytes completely explains the observation that
had led to the hypothesis. And immediately disarming `[0x0D9E]` confirms that there is never a
second block: it is not a transfer, it is a response.

**The cross-check is clean**: USB command `0x87` (`0x58DB`, table `0x5817`) builds **the exact
same two bytes**, under the same conditions, in `[0x0095]`/`[0x0096]`, and sets a length of `0x0002`.
`0x4A` is the radio counterpart of a status readout already present on the USB side — consistent with the rest:
every operation exists on both transports.

What the two bytes carry:

| Byte | Contents |
| --- | --- |
| 1 | `[0x0C36]` — **battery percentage**. Established: `0x8162` copies `IDATA[0x17]` there right after `0x815D` sends it to the module via **command `0x0D`**, whose role this page has already identified |
| 2 | two flags — bit 0 when `0x2D.3` is low, bit 4 when `0x26.0` is high |

Both bits are set and cleared in `0x970E–0x97AA`: `0x26.0` (`bit 0x30`) is **VBUS present** (`P3.4`, debounced 3 samples) and drives charger enable `P7.6` (active LOW), while `0x2D.3` (`bit 0x6B`) is **charging in progress** from `P7.7` (active LOW, debounced 20 samples low / 200 samples high).

And the caveat noted earlier falls away on its own: the end-of-transfer handler does exist, but
it is **handler 10**, not opcode `0x4A`.

#### `b1[7]` has no reader, and the negative is bounded

Bit 7 of `b1`, the per-effect parameter byte, is preserved by the `0x8F` mask when writing
speed. No reader had been identified. There is none, and this time the search is exhaustive.

The two per-effect bytes live at **`0x0345 + effect × 2`** (`b0`) and **`0x0346 + effect × 2`**
(`b1`) — the addressing can be read in plain sight at `0x8F5F`:

```asm
mov dptr,#0x009d ; movx a,@dptr ; add a,acc   ; effect x 2
mov a,#0x46 ; add a,r7 ; mov dpl,a
mov a,#0x03 ; addc a,r6 ; mov dph,a           ; DPTR = 0x0346 + effect*2
```

**No `mov dptr,#0x0340`–`#0x0370` exists anywhere in the entire image**, and the only other base calculation
in `0x03xx` targets `0x030D` (three sites, unrelated). The region is therefore reachable only via this
idiom, and all twelve of its sites can be enumerated:

| Site | Byte | Operation |
| --- | --- | --- |
| `0x1166` | `b0` | `rlc a ; mov 0x23,c` — **`b0[7]` to flag `0x24.3`** |
| `0x79C6`, `0x7A5C`, `0x8CBB` | `b0` | `anl a,#0x80` then write-back |
| `0x79D4`, `0x7A6A`, `0x8CC9` | `b0` | read |
| `0x1174` | `b1` | `anl a,#0x0F` — **color** |
| `0x8F5F`, `0x8F88` | `b1` | `anl a,#0x8F` then `\| (speed << 4)` |
| `0x93F0`, `0x9404` | `b1` | `anl a,#0xF0` then `\| color` |

Three masks touch `b1`: `0x0F` discards it, `0x8F` and `0xF0` preserve it. **None of them
tests it** — not a single `jb acc.7`, not a single `anl a,#0x80` on this byte. The contrast with `b0[7]`,
which has an explicit reader at `0x1166`, makes the negative conclusion all the sharper.

Which does not mean the bit is useless. Both bytes live in the **persisted settings
block**, which the host reads and writes — USB command `0x84`, page 99, and its radio counterpart. `b1[7]` is
therefore a bit that the firmware **carries and preserves for the host software**, without ever
consulting it itself. That is the only reading compatible with the facts.

### `fcn.00007AAD` — the function that contained those bytes

Called from `main` (`0x919A`), guarded by `0x28.1` and `0x29.0`. It scans **three parallel
arrays** indexed by the same counter:

| Base | Role |
| --- | --- |
| `0x0C37 + i` | per-entry state / flag |
| `0x0C45 + i` | current value |
| `0x0390 + i` | target value |

The loop is bounded to **thirteen entries** — the tail of the function makes this unambiguous:

```asm
0x7C08  inc r4
0x7C09  mov a, r4
0x7C0A  xrl a, #0x0d      ; 13 entries
0x7C0C  jz  0x7C11
0x7C0E  ljmp 0x7ABD       ; next iteration
0x7C11  ret
```

> **Solved: this is the mouse auto-repeater.** The three arrays are the exact ones populated by the
> mouse-report press tail (see "The `0x865E` table"): `[0x0C37 + i]` receives `b2 × 2`, the repeat
> countdown; `[0x0C45 + i]` and `[0x0390 + i]` receive `b3`. The thirteen slots are the **thirteen
> mouse actions**, `i = b1 - 1` ∈ 0–12.
>
> And the jump table that the decompiler failed to normalize is `0x7B25`. Its thirteen targets are
> **byte-for-byte those of `0x865E`** — twelve out of thirteen are identical across their first five
> bytes, and the thirteenth, the button arm, differs only in the registers used (`R6`/`R4` instead
> of `R7`/`R6`). This is not a transition queue: it is the **periodic replay of a held mouse
> action**.
>
> The pattern "if the state is set and the current value is zero, copy the desired value" then reads
> with no mystery left: the countdown reaches zero, the action is re-emitted, and the counter is
> reloaded.

#### Function boundaries: the two tools diverge, and r2 is right

Ghidra gives `0x7AAD` a body spanning `0x7AAD`–`0x7D73`, or 711 bytes. r2 splits the same range in
two: `fcn.00007AAD` (245 useful B) and `fcn.00007C12` (354 B). **Disassembly decides in favor of
r2** — there is a `ret` at `0x7C11`, and `0x7C12` is the target of an `lcall`. These are two
routines, not one.

Why Ghidra merges them: it assigns `0x7C12` a `COMPUTED_JUMP` coming from the `jmp @a+dptr` at
`0x7B24` — a destination that the actual table, thirteen entries at `0x7B25`, does not contain. Its
table recovery overflows. The only true caller of `0x7C12` is the site at `0x1BA5`, which r2 places
outside any function (`fcn.0000131c`, the nearest one upstream, is **two bytes** long) and whose
bytes Ghidra did not disassemble. With no visible caller on one side and an imaginary caller on the
other, the two tools err in opposite directions.

> This site is now identified: it is index handler 2 of the RGB animation scheduler. See
> *The animation scheduler — `0x1AF0` and the `0x1B13` table*.

```asm
0x1B9B  mov dptr, #0x0300   ; 16-bit counter
0x1B9E  movx a, @dptr
0x1B9F  subb a, r6
0x1BA0  jnc  0x1BA5
0x1BA2  ljmp 0x1DC2         ; threshold not reached
0x1BA5  lcall fcn.00007C12  ; sole caller
```

> **Reading pitfall.** `axt @ 0x7c12` reports "CALL XREF from `fcn.0000131c` @ +0x889". That is
> **nearest-flag naming**, not containment: `fcn.0000131c` is two bytes long and obviously does not
> contain a site located 2,185 bytes further down. Concluding "`fcn.0000131c` calls `0x7C12`" would
> be wrong. Always verify containment (`aflj` and compare `offset`/`size`) before attributing a call
> to a named function.

Neither tool is authoritative on boundaries. Here r2 is right; elsewhere it can be the reverse.


### `fcn.00007928` — the rotary encoder decoder

Called from `isr_pwm4` (`0x8284`), so sampled on every tick.

```c
DAT_EXTMEM_0F6D = (P0 >> 5) & 3;          /* two bits: P0.5 and P0.6 */
if (0x0F6D == 0x0F6C) return;              /* no change */
hist = (nouveau & 3) | (hist << 2) & 0x3C; /* three two-bit states */
if (hist == 0x0B || hist == 0x34)  ... one direction ...
if (hist == 0x38 || hist == 0x07)  ... the other ...
0x0F6C = 0x0F6D;                           /* save state */
```

This is a classic **quadrature decoder**: two bits, a six-bit sliding history, and four recognized
transition patterns — `0x0B` / `0x34` in one direction, `0x07` / `0x38` in the other.

> **Pinout addition.** The encoder is on **`P0.5`** and **`P0.6`**. Neither pin appeared anywhere in
> the map established so far, which only mentioned `P0.2` (the EUART0 handshake) on `P0`. No
> conflict: rows, columns, PWM, and UART are all elsewhere.

#### Two behaviors, selected by offset 26 of the settings block

```asm
mov dptr, #0x0327 ; movx a, @dptr ; jnz  <brightness path>
```

**`XRAM 0x0327` ≠ 0 → RGB brightness.** Increments or decrements `g_rgb_brightness`, clamped to 9,
then persists via the exact same dispatch as action class 3: per-effect table (`0x0345 + effect × 2`,
`0x80` mask preserved), `0x0362 + effect`, or the global copy.

**`XRAM 0x0327` = 0 → volume.** This is the value in the dump, so the factory default behavior:

```asm
jnb 0x25.6, fin                 ; no pending event
clr 0x25.6
mov dptr, #0x09BD
jnb 0x25.7, +3 ; a = 0xE9       ; direction 1
               a = 0xEA         ; direction 2
movx [0x09BD] = a ; [0x09BE] = 0
setb 0x2A.0
```

`0xE9` and `0xEA` are the **Consumer Volume Up and Volume Down** usages. And `0x2A.0` is precisely
the flag for the first link in the HID report chain, whose buffer is **`0x09BC`** and whose length
is **3** in the `0x6045` table.

Everything cross-checks: `0x09BC` carries Report ID 2, `0x09BD:0x09BE` the sixteen-bit Consumer
usage, making three bytes — the advertised length. The reading of the length table done much earlier
is confirmed here through a completely independent path.

> **So on this unit, the knob controls volume**, and a persistent setting (offset 26 of the block)
> switches it to backlight brightness.

`IDATA 0x6F` serves as a repeat counter, compared against 10 — handling auto-repeat when turning the
knob quickly.

> *Correction, closed:* I had placed the three `orl a, 0x2c` instructions in `fcn.00007928`, then in
> "the following function(s)". In reality **those three instructions do not exist**: they are
> operand bytes misread as opcodes. See *A flag that is never read* and `fcn.00007AAD`, above.

### What remains open on EUART0

The subsystem is covered end-to-end: registers, baud rate, ISR, both handshakes, the eleven
transmitted commands, the three received frame types, the internal protocol of command `0x08`, its
sequencer, and the flash readback map.

> **Correction of an earlier note.** My initial notes listed four dispatched frame types: `0x02`,
> `0x03`, `0x08`, and `0x15`. `0x15` does not exist: it is `xrl a,#0x15 ; orl a, r6`, the bound of
> a 16-bit loop over **21 bytes** — the span of the checksum. Frame byte 0 is only compared in two
> places, `0x060E` (`0x02`) and `0x0778` (`0x03`), with everything else falling through to the
> `0x08` branch.

Remaining items, in decreasing order of interest:

| Item | What is known |
| --- | --- |
| Switching to 2.4 GHz mode | no direct write of value 1 to `g_transport` |
| `0xF0` / `0xF1` parameters of `euart0_reply` | two calls from the parser, values not interpreted |
| Opcode `0x4A` | **solved — it is a status request**, see below |
| Sequencer handlers 8, 9, 10 | `0x3D25` and `0x3D1D`, emit no opcode |

### What the decompiler adds on `euart0_parse`

The firmware was reloaded into **Ghidra 12.1.3** (`8051:BE:16:default`), with the `EXTMEM` space
mapped and names ported over. The C output for `euart0_parse` yields three things the disassembly
had not revealed.

#### The checksum is indeed verified on receive

```c
BANK1_R0 = 0;
do { BANK1_R0 += frame[i]; i++; } while (i != 0x15);
BANK1_R0 = 0x55 - BANK1_R0;
if ((DAT_EXTMEM_0135 ^ BANK1_R0) == 0) { /* frame accepted */ }
```

Twenty-one bytes summed, `0x55 − Σ`, compared against byte 21 of the frame. **The same constant
`0x55` is therefore checked in both directions** — previously this had only been established on
transmit.

#### A sub-index in the high nibble of byte 5

```c
bVar10 = frame[5];                    /* XDATA 0x0125 */
DAT_EXTMEM_038f = bVar10 & 0xf;       /* length */
DAT_EXTMEM_038d = bVar10 >> 4;        /* sub-index */
```

`0x038D` is the exact address that the effect-selection chain uses as a sub-index — the reading
"high nibble of a config byte" is confirmed, and its **source is a radio frame**.

#### Two sub-commands of frame `0x08`

| Sub-index | Effect |
| --- | --- |
| `2` | fills an **entire** color table with a single RGB triplet |
| `1` | **fragmented** transfer into the working settings block |

Sub-index 2 — the loop writes the **same** three bytes (`frame[6..8]`) everywhere:

```c
for (n = 0; n != 0x15; n++)
  for (m = 0; m != 6; m++)
    *(0x0152 + n*0x12 + m*3 + k) = frame[6+k];
```

That is a table in **XDATA `0x0152`–`0x02CB`**, 378 bytes: 21 rows of 18 bytes, six triplets per
row. It stops right before `0x02CC`, which `bt_set_name` uses — the boundary is consistent.

> *Note without deciding:* 21 × 6 = **126 entries**, whereas the F75 only has 90 matrix positions
> (6 × 15). The table is therefore either oversized for the family of keyboards sharing this
> firmware, or indexed by something other than (row, column).

Sub-index 1 — the write goes to `0x09BF + offset`, i.e. the **working settings staging block**
already identified, with `frame[4]` as sequence number (checking `frame[4] == previous + 1`, stored
in `0x09AA`), `frame[5] & 0x0F` as length, and a bound on `0x76` = 118. This is the path through
which the host — or the mobile app over Bluetooth — rewrites persisted settings.

#### Confirmed frame types

| Byte 0 | Handling |
| --- | --- |
| `0x02` | main branch |
| `0x03` | sets `0x2C.3`, clears `0x0150`, replies via `euart0_reply` |
| `0x08` | checksum verification followed by the sub-commands above |

### The command table — established in the decompiler

All twelve transmitters gated by `P4.7` have been decompiled. The command byte is byte 1 of the
frame, placed by the caller in IDATA `0x34`; `euart0_send(length, tag)` does the rest.

| Cmd | Sender | Len. | Payload | Trigger |
| --- | --- | --- | --- | --- |
| `0x01` | `0xED89` | 6 | slot, 0 | radio not connected (`euart0_parse`, `0xEF5A`) |
| `0x02` | `hid_report_radio` | **30** | 28 B pulled from queue | type 2 HID report |
| `0x03` | `hid_report_radio` | **13** | 11 B pulled from queue | type 3 HID report |
| `0x04` | `0xB06A` | 6 | one parameter | `0x2C.4` set |
| `0x06` | `0xED3B` | 6 | `00 00 00` | **status request** → reply `02 06 …` |
| `0x08` | `0xB093` | 23 | container, sub-code in `[3A]` | 9 callers |
| `0x09` | `0xA307` | 32 | Bluetooth name | `main` |
| `0x0B` | `0xB0BC` | 6 | one parameter | `0xEED1` |
| `0x0C` | `0xAE50` | 6 | two parameters | — |
| `0x0D` | `0xB108` | 6 | battery percentage | `0x801F`, one status frame in six |
| `0x0E` | `0xEC85` | 6 | — | wired mode |

All follow the same prologue, which explains the two guards noted earlier:

```c
if (_c_1 != 1 && RD != 0) {        /* no transmission in progress, module ready */
    memset(IDATA 0x33, 0, 0x20);
    euart0_tx_buf = 1;             /* byte 0: constant header */
    DAT_INTMEM_34 = <command>;
    euart0_send(...);
}
```

`RD` is the generic 8051 name Ghidra gives bit `0xB7`: it is **`P4.7`**, the "module ready" line.
Ghidra does not rename bits in the `BITS` space from the SFR map.

#### The `0x08` container

`euart0_cmd08` carries a sub-message. From `hid_report_radio`, when the queue is empty:

```c
IDATA[0x35] = 0x13; [0x36] = 10; [0x37] = 1; [0x38] = 0; [0x39] = 4;
IDATA[0x3A] = <sub-code>;           /* 5 = battery, 7 = settings */
IDATA[0x3B] = <value>;              /* 0x0C36 (percentage) or g_settings */
IDATA[0x3C] = bit0 if not full, bit4 if charging;
euart0_cmd08();
```

Site `0x393E`, on the other hand, sets `[35] = 0x13, [36] = 5, [37] = 1, [39] = 0x0A` and then
copies ten bytes from `CODE:0xB3DC` — same envelope, different sub-message.

#### The internal structure of command `0x08`

> **Correction.** I initially presented `IDATA[0x3A]` as "the sub-code". That is wrong: `0x3A` is
> the **first payload byte**. The discriminator is `IDATA[0x36]`, and `IDATA[0x35]` is not a marker
> but a **length** — `0x13` = 19, the exact number of bytes that `euart0_cmd08` sums
> (`IDATA 0x35..0x47`).

```
[0x34] = 0x08            command
[0x35] = 0x13 = 19       payload length
[0x36] = opcode          <-- the discriminator
[0x37] = echo of 0x02E2  from byte 3 of the received request
[0x38] = echo of 0x0D14  from byte 4 of the received request
[0x39] = (n << 4) | 0x038F   low nibble: requested length
[0x3A .. 0x47]           payload, up to 14 bytes
[0x48] partial sum       [0x49] frame checksum
```

Bytes 3 and 4 of the request are **echoed back verbatim**: this is a paired request/response
protocol, not a stream.

| Opcode `[0x36]` | Payload | Sent by |
| --- | --- | --- |
| `0x05` | template `CODE:0xB3DC`, 10 B | handler 1 |
| `0x0A` | first byte = 5 (battery) or 7 (settings) | `hid_report_radio` |
| `0x41` | 14 B from `CODE:0xD800` | handler 2 |
| `0x42` `0x43` `0x44` | 14 B from a base in the IAP area | handlers 3, 4, 5 |
| `0x49` `0x4A` | same | handlers 6, 7 |

Opcode `0x0A` therefore does carry a second level in its first payload byte — that is where 5
(battery) and 7 (startup settings) live. The other opcodes do not have one.

#### The `0x3901` sequencer

The seven handlers are the entries of a jump table, selected by `XRAM 0x0D9E`:

```c
if ((0x0D9E - 1) > 9) { /* idle */ }
else jump  CODE[0x391C + (0x0D9E - 1) * 3];
```

Ten three-byte entries at `0x391C`:

| `0x0D9E` | Handler | Emitted opcode |
| --- | --- | --- |
| 1 | `0x393A` | `0x05` |
| 2 | `0x3975` | `0x41` |
| 3 | `0x3A25` | `0x42` |
| 4 | `0x3AB5` | `0x43` |
| 5 | `0x3BA7` | `0x44` |
| 6 | `0x3C42` | `0x49` |
| 7 | `0x3CD0` | `0x4A` |
| 8, 9 | `0x3D25` | — |
| 10 | `0x3D1D` | — |

**The idle branch (`0x0D9E == 0`) is what keeps the link alive**: it increments a counter (`0x0150`)
and, every hundred ticks, sends `euart0_cmd06` — the status request whose reply was decoded above.
After three passes, it releases the handshake:

```c
if (++counter > 99) {
    counter = 0;
    euart0_cmd06();
    if (++DAT_INTMEM_30 > 2) { DAT_INTMEM_30 = 0; _c_1 = 0; P0CR &= 0xFB; P0_2 = 1; }
}
```

The handlers **chain** into one another by rewriting `0x0D9E` (`0x3CCE`, `0x3CD4`) and reset it to
zero upon completion (`0x393E`): a single request triggers a burst of replies, one per tick.

#### The eight bytes `0x41` does not read — and `5A A5` finally settled

Handler 2 also redispatches on `0x038D`, but across **four** bases spaced **1024** bytes apart:

| `0x038D` | Base | Page |
| --- | --- | --- |
| 0 | `0xCC00` | 102 |
| 1 | `0xD000` | 104 |
| 2 | `0xD400` | 106 |
| 3 | `0xD800` | 108 |

And it reads **504 = 512 − 8** bytes from them. The eight skipped bytes are the tail of the page.
Here they are for base `0xD800`:

```
offset 504..511 :  00 00 00 00 00 00 5a a5
```

**The only non-zero bytes in the entire page are this `5A A5`** — and they fall precisely in the
region that the firmware refrains from reading back.

##### The marker, swept across the entire IAP area

| Page | `5A A5` marker | Non-zero bytes |
| --- | --- | --- |
| 99 | offset **126** (end of the 128 B block) | 87 |
| 100 | offset 506 | 282 |
| 101 | — | 18 |
| **102 – 109** | offset **510** (last two bytes) | 86, 100, 75, 57, 2, 2, 2, 2 |
| 110 – 117 | — | **0** |

Three independent observations converge:

1. the pattern sits at the **end of the page** (offset 510) across eight consecutive pages;
2. opcode `0x41` stops at **512 − 8**, so **right before** it;
3. the pages carrying it have content; those without it are **completely blank**.

> **Settled.** `5A A5` is a **page validity stamp**, and the readback path knows about it: it
> returns the data and leaves the tail behind. I had flagged it twice as the "best-supported
> reading, unproven" — `504 = 512 − 8` is the structural proof that was missing, because it comes
> from the code rather than from the binary pattern.

The two bytes are bitwise complements (`0101 1010` / `1010 0101`), the classic form of a validity
stamp: a flash erase leaves `FF FF` or `00 00`, never this pair.

##### What this changes about the profiles

Pages **102 to 109 are marked valid**, while pages **110 to 117 are not** and are blank. Yet opcode
`0x43` reads `0xDC00`–`0xEA00`, i.e. **pages 110 to 117** — the unmarked ones — whereas `0x41` reads
four of the eight marked ones.

The `0xD800 + n × 512` grid established from offset 0 of the settings block remains exact as
**addressing**. But the actual data boundary lies elsewhere: it runs between page 109 and page 110,
not at `0xD800`.

##### Why `0x41` appears to skip every other page — solved

It skips none: the "missing" pages are covered by **other opcodes**. The bases of the last three
handlers complete the map.

| Opcode | Base | Page(s) | Blocks × 14 | Content |
| --- | --- | --- | --- | --- |
| `0x44` | `0xC600` | 99 | 10 → **140** | **settings block** |
| `0x49` | `0xC800` | 100 | 35 → **490** | palettes |
| `0x42` | `0xCA00` | 101 | 27 → **378** | **color table** |
| `0x41` | `0xCC00` +1024 | 102, 104, 106, 108 | 36 → **504** | — |
| `0x43` | `0xDC00` +512 | 110 – 117 | 37 → **512** | — |
| `0x4A` | *none* | — | 1 × 2 | payload zeroed |

Two counts verify themselves:

- `0x44` advertises **10 = ⌈128 / 14⌉**, and the settings block is exactly **128 bytes**. This is
  the **third** independent derivation of that size, after the copy loops in `settings_restore` /
  `settings_save` and the length argument of `flash_read_block`.
- `0x42` advertises 378, the exact size of the color table filled by the write path.

##### What truly remains, and why it is more interesting

The map covers pages 99 to 117 **except 103, 105, 107, and 109**. Yet these are not empty pages:
they carry the `5A A5` stamp and, for page 103, **one hundred non-zero bytes — the `0xCE00` remap
table**.

The explanation lies in the stride of `0x41`: its regions are **1024 bytes** wide, so each one spans
*two* pages — 102–103, 104–105, 106–107, 108–109. The "missing" pages are the **second half** of its
regions. It only advertises 504 bytes, which stops within the first page.

And the addressing has no bounds check:

```asm
a = [0x0D14] ; B = 14 ; mul ab      ; offset = sequence x 14
pointer = base + offset             ; no bounds check
```

The count is **advisory**: it is returned to the host in `IDATA[0x37]`, and nothing in the firmware
prevents requesting a higher sequence number. `0xCC00 + 36 × 14 = 0xCDF8` — sequence 36 already
straddles page 103, and sequences 37 to 73 would cover it completely.

> **So the remap table is readable via opcode `0x41`, sub-index 0, at sequences ≥ 36** — beyond the
> advertised count. This is verifiable: one would simply need to request those sequences. Untested,
> as nothing has ever been sent to the device.

#### Opcode `0x43` — a second level of dispatch, and what the 518 bytes actually mean

Handler 4 does not transmit directly: it **redispatches** on `XRAM 0x038D`, the sub-index that
`euart0_parse` extracted from the high nibble of byte 5 of the request.

```asm
0x3AB5  movx a, [0x038D] ; mov r7, a
0x3ABA  cjne a, #0x08, +3 ; jnc exit        ; rejects >= 8
0x3ABF  mov dptr, #0x3AC6 ; a = a*3 ; jmp @a+dptr
```

Eight entries at `0x3AC6`, each setting the high byte of the payload pointer:

| `0x038D` | Base | Page |
| --- | --- | --- |
| 0 | `0xDC00` | 110 |
| 1 | `0xDE00` | 111 |
| 2 | `0xE000` | 112 |
| 3 | `0xE200` | 113 |
| 4 | `0xE400` | 114 |
| 5 | `0xE600` | 115 |
| 6 | `0xE800` | 116 |
| 7 | `0xEA00` | 117 |

**These are the exact pages for profiles 2 through 9.** Their layout had been established elsewhere,
from the `0xD800 + n × 512` calculation on offset 0 of the settings block — two unrelated functions
yielding the exact same grid. The 512-byte stride and the upper bound `0xEA00` (= profile 9) match.

Opcode `0x43` is therefore **the readback of a remap profile**, with the sub-index choosing which
one.

##### The 518 bytes do not exist

Access is **random, not sequential**: the pointer is recomputed from the base on every request,
rather than advanced.

```asm
a = [0x0D14] ; B = 14 ; mul ab        ; offset = sequence x 14
[0x013A] += offset_lo ; [0x0139] += offset_hi     ; pointer = base + sequence x 14
```

And the reply sends back what the host needs to drive its loop:

```asm
IDATA[0x37] = [0x02E2]    ; total number of blocks
IDATA[0x38] = [0x0D14]    ; current block number
```

The count `[0x02E2]` is therefore a **loop ceiling**, not a byte size. And it reads as follows:

| Opcode | Blocks | × 14 | Division | Actual region size |
| --- | --- | --- | --- | --- |
| `0x41` | 36 | 504 | exact | **504 B** |
| `0x42` | 27 | 378 | exact | **378 B** |
| `0x43` | 37 | 518 | **remainder 6** | **512 B** — `⌈512 / 14⌉ = 37` |
| `0x44` | 10 | 140 | exact | **140 B** |
| `0x49` | 35 | 490 | exact | **490 B** |

**`0x43` is the only one whose count does not divide evenly.** It is precisely the one that reads a
full 512-byte page: 37 blocks of 14 cover it, with the last block overshooting by six bytes that the
host ignores. The other four come out even, so their regions are genuinely 504, 378, 490, and
140 bytes — not full pages.

Which corrects the reading from the previous commit: I spoke of "518 bytes" as a region size. It is
a **rounded-up block count**, and the region is 512 bytes.

In passing, this also clarifies opcode `0x41`: its region is **504 bytes on the dot**, not a full
page. Eight bytes short of a 512-byte page — reserved for something else, or simply outside the
readback range.

#### What the payload contains

The payload pointer lives in `XRAM 0x0139:0x013A` and receives `0xD000`, `0xD400`, or `0xD800`
depending on the handler — **pages in the IAP area**. The firmware thus sends back, fourteen bytes
at a time, the contents of its configuration flash.

This is the **read counterpart** to the fragmented transfer already identified on receive (frame
`0x08`, sub-index 1, into `g_settings_staging`). The radio link carries a protocol for both reading
*and* writing the configuration area, in 14-byte blocks, with a sequence number.

#### The thirteen arms of the received `0x08` container

`euart0_parse` dispatches received `0x08` frames via a call to the inline dispatcher `0x4E45`, at
`0x0856`. The dispatcher reads in one piece:

```asm
0x4E45  pop DPH ; pop DPL          ; DPTR = return address = start of inline data
0x4E49  mov R0,A                   ; R0 = lookup key
0x4E4B  movc a,@a+dptr             ; hi
0x4E4C  jnz 0x4E60                 ; valid entry -> compare
0x4E50  movc a,@a+dptr (offset 1)  ; lo
0x4E51  jnz 0x4E60
0x4E53  inc dptr ; inc dptr        ; hi == lo == 0: end of table, default address follows
0x4E55  <load hi/lo into DPTR and jmp @a+dptr>
0x4E60  movc a,@a+dptr (offset 2)  ; the KEY
0x4E63  xrl a,R0 ; jz 0x4E55       ; found -> jump to THIS entry's address
0x4E66  inc dptr x3 ; sjmp 0x4E4A  ; next entry
```

**Entries are `(addr_hi, addr_lo, key)` — with the key last**, the table ends with a `00 00`
address, and the default address follows the terminator. This format holds across all **four** call
sites of `0x4E45` in the image: `0x0856` (13 entries, default `0x0F6A`), `0x1741` (19 entries,
default `0x1AEA` — effects), `0x4136` (17 entries, default `0x44F5` — shortcuts), and `0x8845`
(9 entries, default `0x8919`).

Two independent cross-checks close the reading. Arm `0x0C4A` ends with `ljmp 0x0F6A`, i.e. **the
default address of its own table**; and the seven opcodes that this page had already deduced via a
byte search for `90 0d 9e` (`0x05` at `0x0C4A`, then `0x41`…`0x4A`) land on the exact keys yielded
by this parse.

##### The table

| Key | Arm | Direction | Role |
| --- | --- | --- | --- |
| `0x01` | `0x0884` | write | **remap** profile — `CODE` base selected by sub-index `[0x038D]`: `0`→`0xCC00`, `1`→`0xD000`, `2`→`0xD400`, `3`→`0xD800` |
| `0x02` | `0x09BD` | write | **per-key lighting** profile — base `0xCA00`, sub-index `0` only; **no flash write** |
| `0x03` | `0x0AAC` | write | eight profiles — bases `0xDC00`, `0xDE00`, `0xE000`, `0xE200`, `0xE400`, `0xE600`, `0xE800`, `0xEA00` |
| `0x04` | `0x0BB0` | write | **settings** block — fixed page, committed via `0xEEF4` |
| `0x05` | `0x0C4A` | read | arms handler **1** in `[0x0D9E]` (template `0xB3DC`) |
| **`0x06`** | `0x0C53` | action | **immediate factory reset** |
| `0x09` | `0x0C68` | write | fixed base `0xC800` |
| `0x41` | `0x0D10` | read | handler 2 — 36 blocks × 14 = **504 B** |
| `0x42` | `0x0D2B` | read | handler 3 — 27 × 14 = **378 B** (the color table) |
| `0x43` | `0x0D45` | read | handler 4 — 37 × 14 = **518 B** |
| `0x44` | `0x0D5F` | read | handler 5 — 10 × 14 = **140 B** |
| `0x49` | `0x0D79` | read | handler 6 — 35 × 14 = **490 B** |
| `0x4A` | `0x0D93` | read | handler 7 — 1 block of **2 B**; also forces `[0x08DB] = 10` |
| — | `0x0F6A` | — | **default**: parser exit, no-op |

##### The common skeleton of the five write arms

`0x0884`, `0x09BD`, `0x0AAC`, `0x0BB0`, and `0x0C68` share the exact same body, up to the base
address and commit step:

```c
if (sub_index_selected)                     /* [0x038D] selects a CODE base */
    { [0x0139:0x013A] = base; }

if ([0x0D14] == 0)                          /* first block in the sequence */
    memcpy_code(0x09BF, base, 512);         /* prime the buffer with the current page */

[0x0139:0x013A] = [0x0D14] * 14;            /* offset = sequence number x 14 */
memcpy(0x09BF + offset, 0x0126, [0x038E:0x038F]);     /* the received block */

if ([0x0D14] == [0x02E2])                   /* last block -> commit */
    <arm-specific commit>;

for (i = 2; i < 0x16; i++)                  /* ack: the request echoed back verbatim */
    IDATA[0x33 + i] = XRAM[0x011F + i];
euart0_cmd08(...);
if (!wired) { P0CR &= 0xFB; P0.2 = 1; }     /* release the transmit handshake */
```

Priming is what makes the protocol safe: the 512-byte buffer at `XRAM 0x09BF` is first filled with
**the current contents of the page**, and only the bytes actually transmitted overwrite it. A
partial transfer therefore does not destroy the rest of the page. Arms `0x03` and `0x04` do not
prime — they rewrite the entire page.

The commit step differs:

| Arm | Commit |
| --- | --- |
| `0x01`, `0x03`, `0x09` | `0xEE4E` → `iap_save(page = base_hi >> 1)` → `0xAAC1`, **the exact same IAP page writer as the factory reset** |
| `0x02` | **no flash write**: sets bit `0x2C.7`, writes `[0x0E24] = 0x20 + [0x038D]`, then calls `0xAF99` — i.e. *switches to effect `0x20 + sub-index`*. Keys `0x20`, `0x26`, and `0x2D` of the `0x1744` effect table are those exact effects |
| `0x04` | `0xEEF4`, which unrolls the IAP sequence by hand: `EA = 0`, `matrix_cols_release(99)`, `iap_erase(99)`, `iap_write_page(src = 0x09BF, dst = 0xC600, 512)`, `iap_lock()`, `EA` restored |

`page = base_hi >> 1` places all of these destinations inside the **99–117** range that this page
identified as the emulated EEPROM: `0xC600` → 99, `0xC800` → 100, `0xCA00` → 101, `0xCC00` → 102,
and the eight profiles of arm `0x03` on **110 to 117**, the exact end of the range. The write
protocol and the flash map, established separately, meet on the exact same boundary.

##### What this changes for the pairing negative result

All thirteen arms have now been opened one by one. **None touches EUART0 other than through the
`euart0_cmd08` acknowledgment, which echoes the received request verbatim.** None sends command
`0x01`, nor anything else capable of destroying a stored bond. The negative result holds in both
directions.

One nuance to the earlier statement that "the sub-opcodes of container `0x08` are all data
transfers": **twelve out of thirteen** are (five writes, seven reads). The thirteenth, `0x06`, is an
**action** — the factory reset. And arm `0x01` does more than transfer data: it also selects a base
via the sub-index.

#### What arms the sequencer — solved

The three writes Ghidra listed were internal to the sequencer. Searching the entire image for the
pattern `90 0d 9e` turns up **thirteen** hits, including seven in a range that the reference list
did not cover: `0x0C4A`, `0x0D23`, `0x0D3E`, `0x0D58`, `0x0D72`, `0x0D8C`, `0x0DA6`.

These addresses fall **inside the body of `euart0_parse`** — they all end with `ljmp 0x0F6A`, the
parser's exit label. The function Ghidra had reconstructed simply did not span this range, so no
xref had been recorded.

> **Methodological pitfall.** A cross-reference list is only complete for code that has actually
> been disassembled. Here a raw byte search caught what the reference graph missed — just as with
> computed pointers, but for a different reason: the range really was code, it just did not belong
> to any function.

Six of the seven sites are the exact same block, up to the counts:

```asm
movx [0x02E2], #<n>      ; number of blocks to transfer
movx [0x0D14], #0        ; sequence number, reset to zero
movx [0x038E], #0        ; block length, high byte
movx [0x038F], #14       ; block length
movx [0x0D9E], #<h>      ; arm handler h
ljmp 0x0F6A              ; parser exit
```

| Site | Handler | Opcode | Blocks | Bytes/block | **Total** |
| --- | --- | --- | --- | --- | --- |
| `0x0C4A` | 1 | `0x05` | — | — | template `0xB3DC` |
| `0x0D10` | 2 | `0x41` | 36 | 14 | **504** |
| `0x0D2B` | 3 | `0x42` | 27 | 14 | **378** |
| `0x0D45` | 4 | `0x43` | 37 | 14 | **518** |
| `0x0D5F` | 5 | `0x44` | 10 | 14 | **140** |
| `0x0D79` | 6 | `0x49` | 35 | 14 | **490** |
| `0x0D93` | 7 | `0x4A` | 1 | **2** | **2** |

**The total for opcode `0x42` is 378 bytes — the exact size of the color table**
(`XRAM 0x0152`–`0x02CB`) that the write path fills via frame `0x08` sub-index 2. Both directions of
the protocol cross-check on the same structure, established independently.

The 504 bytes of opcode `0x41` match the 512-byte profile page at `0xD800`, within eight bytes.

And these are **the exact same three variables** — `0x02E2` (count), `0x0D14` (sequence), `0x038F`
(length) — that `euart0_parse` populates from bytes 3, 4, and 5 of a received `0x08` frame.
Depending on the path, the keyboard either **replies** to a transfer requested by the host, or
**initiates** its own.

The loop is therefore complete:

```
received frame --> euart0_parse sets 0x02E2 / 0x0D14 / 0x038F / 0x0D9E
                                  |
                        next tick v
                 FUN_CODE_3901 : jump table on 0x0D9E
                                  |
                                  v
                 cmd 0x08, opcode, 14 bytes of flash, sequence incremented
                                  |
                         0x0D9E reset to 0 --> idle branch
                                               (cmd 0x06 every 100 ticks)
```

##### `CODE:0xB3DC` — a template, not a string

Site `0x393E` builds the envelope and then copies the table:

```asm
IDATA[0x35] = 0x13 ; [0x36] = 0x05 ; [0x37] = 0x01 ; [0x38] = 0 ; [0x39] = 0x0A
r2 = 7
loop: IDATA[0x33 + r2] = CODE[0xB3DC + r2] ; r2++  until r2 == 17
```

The starting index matters: `IDATA[0x33 + 7]` is `IDATA[0x3A]`, **the sub-code slot**. The table is
therefore not a ten-character string — it is a **pre-composed sub-message**:

```
idx  0  1  2  3  4  5  6 | 7  | 8  9 10 11 12 13 14 15 16
     ff 00 ff ff ff ff ff| 03 | 00 00 00 00 cd 01 00 00 00
                          ^sub-code     ^
```

Sub-code **3**, payload `00 00 00 00 CD 01 00 00 00`. The first seven bytes are never read by this
site.

> *Note, without drawing a conclusion:* byte `0xCD` in the fifth position of the payload is also the
> internal identifier that the OpenRGB driver associates with the AULA F75
> (`{ 0xCD, { "AULA F75", aula_f75_layout } }`). However, `0xCD` **appears nowhere else as an
> immediate** in the image, and this table has only one reader: nothing in the code links the two.
> It is a coincidence to check, not a fact.

##### Sub-code `7` is a one-shot transmission at startup

`0x2B.5` is set in only one place — `vec.reset`, at `0x917B`:

```asm
jb    0x2B.1, next
lcall settings_restore        ; 0xA283
setb  0x2A.1
clr   0x2C.2
setb  0x2B.5                  ; <-- arms sub-code 7 transmission
```

It is armed **right after restoring settings from flash**, and cleared by whichever of the two
transports sends it first (`hid_report_radio` at `0x47B0`, `hid_report_usb` at `0x6BDD`). At
startup, once its settings have been read back, the keyboard announces them **once** to the radio
module.

### The keyboard has ten remap profiles in flash

The payload of sub-code 7 is `g_settings[0]`, i.e. **offset 0** of the settings block — the most
referenced field in the block (20 sites) and the only one I had not yet identified. The C code gives
it away:

```asm
a = [0x030D]
add  a, a                     ; x 2
DPL = 0
DPH = 0xD8 + a                ; DPTR = 0xD800 + index x 512
...
mov  B, #4                    ; then indexed by event x 4
```

**`DPTR = 0xD800 + index × 512`**, followed by an offset of `event × 4`: that is the exact shape of
the four-byte remap descriptor. `XRAM 0x030D` is therefore a **profile index**, and each profile
occupies a 512-byte page starting at `0xD800`.

| Profile | Address | Page | Non-zero bytes |
| --- | --- | --- | --- |
| 0 | `0xD800` | 108 | 2 |
| 1 | `0xDA00` | 109 | 2 |
| 2–9 | `0xDC00`–`0xEBFF` | 110–117 | **0** |

**The end of profile 9 lands on `0xEBFF` — the exact upper bound of the IAP area.** That bound had
been established independently from the `subb a, #0x76` check in `iap_erase`, which rejects pages
beyond 117. Both meet to the exact byte: the writable region is sized for **settings page +
palettes + base keymap + ten profiles**, with not a single byte to spare.

The profile path is gated by **offset 22** of the block (`XRAM 0x0323`):

```asm
mov dptr, #0x0323 ; movx a, @dptr ; jz  ...   ; if 0 -> skip profile
```

In the dump, offset 0 and offset 22 are both `0x00`, and eight of the ten pages are blank: **no
profile is configured**, and the base remap table at `0xCE00` applies.

> *Inferred:* that these pages are remap tables. *Established:* the `0xD800 + n × 512 + event × 4`
> addressing, the exact match with the IAP upper bound, and the fact that the path is disabled on
> this unit.

### The received status frame — `02 06 …`

This is the `byte[0] == 2` branch of `euart0_parse`, the last one remaining to be read. It is
guarded twice before any processing:

```c
if (frame[2] != 0)      return;    /* byte 2 required to be 0 */
if ((frame[1] ^ 6) != 0) return;   /* byte 1 required to be 6 */
sum = 0x55 - Σ frame[0..8];
if (frame[9] != sum)     return;   /* checksum over NINE bytes */
```

Ten bytes, checksum over the first nine — confirming the format noted earlier while adding the two
constraints. This is the **reply to command `0x06`**: request and reply carry the same code.

| Byte | Destination | Role |
| --- | --- | --- |
| 3 | `0x0F41` | — |
| 4 | `0x09BA` | status code — `0x0A` when wired, `0x0B` otherwise, compared against slot in Bluetooth |
| 5 | `0x09AC` | **"connected" flag** |
| 6–7 | `0x02E7`:`0x02E6` | **battery gauge, 16-bit little-endian** |

> **Correction.** I had read the gauge as `byte × 256` from the disassembly. The C output shows the
> final assignment: `0x02E6` (high byte) receives `frame[7]`, `0x02E7` (low byte) receives
> `frame[6]`. It is a **true 16-bit field**, which fits the 737 / 781 / 846 / 912 thresholds and a
> 10-bit ADC reading much better.

Processing then depends on the active transport:

```c
if (g_transport == 2) {                     /* Bluetooth */
    if (g_bt_slot == 0 || g_bt_slot > 3) g_bt_slot = 1;   /* clamped to 1..3 */
    if (frame[4] == slot && frame[5] != 0)  /* connected */ ;
    else  cmd_01(g_bt_slot, 0);
}
else if (g_transport == 1) {                /* 2.4 GHz */
    if (frame[4] != 0 || frame[5] == 0) cmd_01(0, 0);
}
else if (g_transport == 0 && frame[4] != 0x0A) cmd_0E();   /* wired */
```

**The Bluetooth slot is clamped to 1–3 by the code itself** — until now that range had only been a
deduction from the three selection blocks.

And the function's tail releases the handshake:

```c
if (_c_1 != 1) { P0CR &= 0xFB; P0_2 = 1; }   /* P0.2 as input, released high */
```

### `0x801F` — the battery gauge, and the `0xAF8D` curve

Called once every six status frames. It converts the raw reading into a percentage (`0x0151`, capped
at 100, zero below `0x02CB` = 715), and then **slews a displayed percentage** toward that target,
one step at a time:

```c
delta = |displayed - target|;
delay = CODE[0xAF8D + delta/10];
if (++counter >= delay) { counter = 0; displayed += ±1; }
```

`CODE:0xAF8D` is **twelve bytes long on the dot**:

```
20 10 05 02 02 02 02 02 02 02 02 02
```

A **decelerating damping curve**: the closer the displayed value gets to the actual reading, the
more it slows down.

> This curve had been flagged as unexplained since the start of the analysis. Now it is explained.

Delta of 0–9 → 20 ticks per step; 10–19 → 10; 20–29 → 5; above that → 2.

> **The listing above is in DECIMAL, not hexadecimal**, unlike most excerpts on this page. The port
> got tripped up by this: it "corrected" the prose to 32, 16, 5, 2 assuming `0x20 0x10 0x05 0x02`,
> then re-read the dump. The actual bytes are `14 0a 05 02 02 02 02 02 02 02 02 02` — **20, 10, 5,
> then 2**. The prose was right, the correction was wrong, and the dump settled it. Lesson learned:
> on this page, check the radix before correcting a gloss.

The convergence direction is selected by `0x26.0` (`bit 0x30`, VBUS/USB power present), using two separate counters (`0x08C5` when
rising, `0x08DC` when falling) and special handling at 100% controlled by `0x2D.3` (`bit 0x6B`, charging in progress — caps at 99% while charging and snaps to 100% at `>= 95%` when charge completes).

### The radio transmit queue — correction

`0x0C57` is **not** "one 28-byte record per paired host", as I had assumed. It is a **six-slot
ring buffer of 28-byte entries**:

| | |
| --- | --- |
| Write index | `g_radio_host_idx` (`XRAM 0x0307`) |
| Read index | `XRAM 0x030C` |
| Wraparound | `if (idx > 5) idx = 0` — **six** slots |
| Slot byte 0 | type: **2** or **3**, which becomes the command byte |

On transmit, the type determines the frame size:

```c
if (type == 2) { memcpy(IDATA 0x34, slot, 0x1C); euart0_send(0x1E, 6); }  /* 28 B -> 30 */
if (type == 3) { memcpy(IDATA 0x34, slot, 0x0B); euart0_send(0x0D, 6); }  /* 11 B -> 13 */
```

And the flags that feed the queue are **the exact same ones** as in the USB chain of
`hid_report_usb`: `0x2A.4`, `0x27.0`, and `0x2A.6` produce type 2; `0x2A.0` and `0x29.0` produce
type 3. Both transports share the exact same event source — closing the plumbing described earlier.


> **Ported.** `aula_rf.c` adopts the queue as-is: six 28-byte slots, separate read and write
> indices, byte 0 of the slot serving as the command byte, and the same 28 → 30-byte frame / 11 →
> 13-byte frame split. Without it, any keystroke emitted while a burst was in flight, or while
> `P4.7` was low, was **silently dropped**: `rf_send_payload_long()` returned `false` and `kb.c`
> ignored the return value.
>
> Overflow overwrites the oldest entry, just like the factory firmware — its write index wraps
> around without checking the read index. Six slots represent roughly two milliseconds of buffer at
> 260 kbaud; filling them means the module has stopped responding, in which case freshness wins.
>
> **What is NOT carried over: the Air60's key-release hammering.** Its SPI driver hammers the final
> key release six times, alternating between an empty report and a report carrying `0x01` in
> `keys[0]` (*ErrorRollOver*), so that neither gets deduplicated downstream. That is a workaround
> for a bus with no application-level acknowledgment, and **nothing equivalent was observed in the
> F75's factory firmware**. The queue addresses the same failure mode at the source: a release that
> cannot go out immediately is queued and replayed. Inventing release hammering by analogy would
> have been ungrounded code.

### Frame grammar — established

The frame format is decoded, and one command is decoded semantically.

```
[0] = 0x01        constant header
[1] = command
[2] = parameter / sub-command
[3] = payload length
[4..] = data
```

#### Command `0x09` — set Bluetooth name

Built in `fcn @ 0xA307`, called **twice from `main()`** (`0x9148` and `0x914D`):

```asm
mov @r0,#0x01     ; [0] header
mov @r0,#0x09     ; [1] command
mov @r0,#0x01     ; [2] profile (0x00 in the alternate branch, depending on r7)
mov @r0,#0x10     ; [3] length = 16
mov dptr,#0xaf6d  ; copy 16 bytes from flash
```

And `0xAF6D` contains, in plain ASCII:

```
AF6D  "AULA-F75 3.0 KB "
AF7D  "AULA-F75 5.0 KB "
```

**The keyboard's two Bluetooth advertising names**, 16 characters each, padded with a trailing
space. Parameter `[2]` selects which one — hence the two calls from `main()`.

#### The `0xAF6D` – `0xAF98` data region

Fully bounded block: code resumes at `0xAF99`. **Four readers**, all identified via their DPTR
loads.

| Address | Size | Content | Read by |
| --- | --- | --- | --- |
| `0xAF6D` | 16 B | `"AULA-F75 3.0 KB "` | `0xA321` — command `0x09` |
| `0xAF7D` | 16 B | `"AULA-F75 5.0 KB "` | `0xA348` — command `0x09` |
| `0xAF8D` | 12 B | `20, 10, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2` | `0x808C` and `0x8114` |

> **Ported.** `aula_rf.c` sends both names at startup, one per `rf_task()` pass as soon as `P4.7`
> allows transmission — whereas the factory firmware sends them from `main` without checking whether
> the module is listening. All sixteen bytes are copied from the dump, trailing padding space
> included.
>
> **The Bluetooth slot is now persisted**, which it previously was not: it used to reset to 1 on
> every boot. It is stored in `user_settings.rf_link`, which is otherwise unused on this keyboard —
> elsewhere that field holds the Air60's `rf_mode_t` encoding, but its sole consumer,
> `restore_rf_link()` in `src/main.c`, is guarded by `#ifdef RF_ENABLED`, and `RF_ENABLED` and
> `RF_EUART0` are mutually exclusive. No field is added to `user_settings_t`, which would have
> invalidated saved settings across all five boards: `nvm.c` checks the stored struct length and has
> no migration path.
>
> We store the **slot** there, not the link mode: on the F75 the transport comes from the physical
> mode switch, and the slot is the only thing the switch does not tell us.

The 12-byte table is a **decreasing-threshold acceleration curve**:

```asm
mov  B, #0x0A ; div ab      ; index = (value - X) / 10
mov  dptr, #0xaf8d ; movc   ; threshold = table[index]
movx a, @dptr               ; counter (XRAM 0x08C5, or 0x08DC for the other reader)
clr c ; subb a, r7 ; jc     ; counter < threshold -> exit
clr a ; movx @dptr, a       ; otherwise reset to zero
inc  0x17                   ; and advance by one step
```

It takes **20 passes for the first step, then 10, then 5, then 2** indefinitely. Two distinct
counters (`0x08C5` and `0x08DC`) share the same curve. What it ultimately drives is not established
here — one would need to trace the consumers of `IDATA 0x17`.

*Note: the bytes `30 67 27 c2 67 53 a9 fe` following the table are not data but **code** —
`30 67 27` disassembles to `jnb 0x2c.7, 0xafc3`, the start of `fcn.0000AF99`. They looked like a
Bluetooth MAC address; they are not.*

#### Other spotted commands

| Cmd | Where | Shape | Interpretation |
| --- | --- | --- | --- |
| `0x04` | `0xB084` | `01 04 <byte>` | one parameter read from XRAM `0x0EF6` |
| `0x06` | `0xED41` | `01 06 00 00 00`, length 6 | zero payload — probe or status request? |
| `0x08` | `0xB09E` | length 23 | filled from IDATA `0x48` |
| `0x09` | `0xA307` | length 32 | **Bluetooth name** (above) |

*Correction: an earlier version of this document attributed `0x05` to site `0xED53`. That is
wrong — `0xED53` is the transmit call, and the frame it sends carries command `0x06`, built right
before at `0xED41`.*

*And the "HCI over UART" lead is ruled out: the BK3432 SDK does describe an HCI transport for its
bootloader, but `01 09 01 10` does not parse as HCI (the opcode would be invalid). This is a
proprietary protocol — whose grammar is now known.*

### Two paths to obtain the semantics

#### A. Sniffing the EUART0 link — **recommended, and WITHOUT FLASHING**

**No flashing at all. The keyboard runs its factory firmware.** We passively tap the wire between
the 8051 and the BK3632. Zero risk to the device.

##### Where to probe

The datasheet gives the EUART0 pins:

| Signal | Pin | Direction |
| --- | --- | --- |
| **TXD** | **P5.5** | 8051 → BK3632 |
| **RXD** | **P5.6** | BK3632 → 8051 |

✅ **Pin remapping ruled out by verification.** The SH68F90 can relocate its EUART0 to `P3.3`/`P3.4`
(`TXD_M`/`RXD_M`) by writing `0x5A` to the **`MAPPING` register (SFR `0x8A`)**, whose reset value
is `0x00`. Searching the entire factory firmware for any write to `0x8A` — `mov`, `anl`, `orl` —
returns **zero hits**. `MAPPING` therefore stays at `0x00` and the UART remains on its default pins.

This was a necessary check: `P3.3` and `P3.4` also carry `PWM03` and `PWM04`, which are backlight
channels 12 and 13 (row 4, red and green). Remapping would have meant those pins were shared,
invalidating both the UART probe point and the RGB table.
| handshake | `P0.2` | pulled low before each frame, released at the end (optional 3rd channel) |

⚠️ **Correction**: an earlier version of this document described the link as *half-duplex with
direction on `P0.2`*. That is wrong. `TXD` and `RXD` are **two separate pins** — the link is
full-duplex. `P0.2` is an attention signal to the BK3632, not a direction selector:
`clr P0.2` + `P0CR |= 0x04` (output) before transmitting (`0xAB13`), then `P0CR &= ~0x04` (input) +
`setb P0.2` at end of frame, inside the ISR (`0xA580`).

A welcome practical consequence: **two analyzer channels capture both directions separately**, with
no demultiplexing needed.

##### How to configure the analyzer

Configuration found in the factory firmware (`0xB1CE`):

```asm
mov SCON,  #0x50   ; mode 1: asynchronous, 10 bits (1 start, 8 data, 1 stop) = 8N1, REN=1
mov SBRTH, #0xFF   ; SBRTEN=1, SBRT[14:8]=0x7F
mov SBRTL, #0xFB   ; SBRT[7:0]=0xFB      -> SBRT = 0x7FFB = 32763
mov SFINE, #0x0C   ; BFINE = 12
```

Datasheet formula: `Baud = Fsys / (16 × (32768 − SBRT) + BFINE)`

→ `Fsys / (16 × 5 + 12)` = **`Fsys / 92`** = **260,870 baud**.

**`Fsys` = 24 MHz, confirmed by cross-check.** SMK's UART driver (`platform/sh68f90/uart.c`)
implements the exact same formula; with `FREQ_SYS = 24000000` and `UART_BPS = 260870` it computes
`SBRT_INT = 5` → `SBRT = 32763 = 0x7FFB` → `SBRTH = 0xFF`, `SBRTL = 0xFB`, and
`SFINE = 24000000/260870 − 80 = 12 = 0x0C`. **All three registers match the factory firmware to the
bit.** A different `Fsys` value would not reproduce them.

**In practice, do not rely solely on this calculation:** a logic analyzer measures the width of the
shortest bit and deduces the baud rate. PulseView (sigrok) and Saleae do this automatically. The
calculation serves to verify that the measurement makes sense.

##### What to capture with — a logic analyzer is not mandatory

| Tool | Cost | What it gives | Limitations |
| --- | --- | --- | --- |
| **FTDI USB-TTL adapter** | ~€3 | decoded UART stream, directly | one channel per adapter; **3.3 V mandatory** |
| Logic analyzer | ~€10 | both channels + exact timing + `P0.2` | — |
| Arduino / Pi Pico | ~€5 | same as FTDI, with timestamps and arbitrary baud rate | requires programming |
| **USBPcap + Wireshark** | **€0** | the **host ↔ keyboard** protocol (Report ID 6) | **not** the 8051 ↔ BK3632 link |

**A USB-TTL adapter offers the best effort-to-result ratio**, and many people already have one on
hand.

The non-standard baud rate is not an obstacle with an **FTDI** chip: its baud generator computes
`baud = 3,000,000 / divisor` with a fractional divisor in 1/8 increments. For 260,870 baud the
divisor is `11.5` — achievable with zero error. For 130,435 baud it is `23` — also exact. On Linux,
`stty -F /dev/ttyUSB0 260870` is all it takes. A CH340 is less reliable at exotic baud rates.

⚠️ **Wiring, RECEIVE-ONLY:**

- Adapter `RX` on `P5.5` (or `P5.6`), **common ground** with the keyboard.
- **Never connect the adapter's `TX`.** Injecting data there would disrupt the link, and a 5 V
  adapter would destroy a 3.3 V input.
- A single adapter only captures one direction. Use two adapters, or two successive passes.

##### And the zero-hardware option

**USBPcap + Wireshark** (Windows) or `usbmon` + `tshark` (Linux) capture USB traffic between the PC
and the keyboard — **for free, with zero soldering**. However, that is a **different layer**: it
shows the HID Report ID 6 channel (`0x04` write config, `0x84` read, `0x0a` color table…), not the
EUART0 frames going to the BK3632.

That makes it the right tool for finishing the documentation of the **configuration protocol** — the
one from xevrion's article, covering RGB and key remapping. It is not the wireless protocol. The two
efforts are distinct, and this one costs nothing.

##### What to capture

What we already know to look for (see the transport table above): frames starting with `0x01`,
second byte = command (`0x04`, `0x05`, `0x08`, `0x09` observed), variable length from 6 to 32 bytes.

Sequences to record, each in isolation:

1. Plugging in over USB then switching to Bluetooth — command `0x09` is sent from `main()`
   (`0x9148`), so traffic goes out right at initialization.
2. Pairing a Bluetooth host.
3. Switching to 2.4 GHz.
4. A few key presses in each mode — to isolate the "HID report" frame.
5. Entering sleep and waking up.

Cross-referencing these captures against the five transmit sites already located (`0x46C2` 30 B,
`0x4730` 13 B, `0xA376` 32 B, `0xB0B8` 23 B, `0xED53` 6 B) links each command to its trigger.


The transport is already characterized (above). All it takes is a logic analyzer on the data line of
the 8051 ↔ BK3632 link while operating the keyboard: toggling USB / BT / 2.4 GHz, pairing, pressing
keys. Frames are 6 bytes long, start with `0x01`, and pin `P0.2` indicates activity — so we know
what to look for and how to separate the two directions.

**Cost: a logic analyzer and a test point.** No flashing, no desoldering, no new binary to
disassemble. This is the path that yields the protocol *as it is actually spoken*.

#### A-bis. Probing the BK3632 from custom firmware — no external hardware

Idea: instead of tapping the wire from outside, run **our own firmware** on the 8051 and have it
talk to the BK3632, logging everything over SMK's USB console.

**⚠️ Passive listening would NOT work.** If our firmware is running, the factory firmware is not:
nobody is sending commands anymore, and the BK3632 is a slave waiting for instructions. We would
hear nothing.

**What does work is active probing.** We know the frame format and command codes from the factory
firmware. Our firmware can therefore **replay the exact sequences** that the factory firmware builds
and log the replies. The physical BK3632 serves as an oracle.

##### All the tooling already exists in SMK

| Component | Where |
| --- | --- |
| UART driver (init, `putc`, `getc`) | `src/platform/sh68f90/uart.c` |
| Debug console over USB HID | `src/smk/console.c`, `dprintf()` |
| Host-side reader | `tools/smk-console` |
| Return to ISP over USB | `src/smk/usb.c:442` → `isp_jump()` |

Settings to configure: `UART_BPS = 260870`, `UART_RX_EN = 1`, and console output routed to USB
(not UART, since the UART is used to talk to the BK3632).

##### Sequences to replay

The five located transmit sites give the frames to reproduce, along with their lengths:

| Site | Length | Command | Known trigger |
| --- | --- | --- | --- |
| `0xA376` | 32 | `0x09` | called from `main()` (`0x9148`) — link init |
| `0x46C2` | 30 | ? | — |
| `0xB0B8` | 23 | `0x08` | — |
| `0x4730` | 13 | ? | — |
| `0xED53` | 6 | `0x05` | — |

The exact payload contents can be read from the disassembly at each site.

##### Risk profile — **flashing required**

This is the only path in this section that requires flashing. Mitigating factors:

- **Minimal firmware**: USB + UART only, no matrix, no RGB, no sleep. Less code means fewer chances
  of hanging before USB enumerates.
- **Two ISP recovery paths verified on this unit**: the bootloader checks `0xEFFB` at reset
  (`0xF024`), and `isp_jump()` works via the magic entry point `0xFF00`.
- **Factory image on hand**: `assets/f75_full.bin`, MD5 recorded, restorable.

Remaining risks: **`sinowisp write` has never been tested on this model** (unchecked box in issue
#96), and there is no external backup programmer.

##### Comparison

| | External probe (USB-TTL / analyzer) | Custom firmware |
| --- | --- | --- |
| Flashing | **none** | **required** |
| Hardware | ~€3 adapter + 2 solder points | none |
| Observes | the **actual** factory dialogue | only what we trigger |
| Risk | zero | potential brick |

**The two approaches are complementary.** An external probe shows what the factory firmware *truly*
says, in order and in context. Custom firmware lets us *query* the BK3632 freely, including with
commands the factory firmware never sends. Starting with the external probe remains wiser: it yields
ground truth at zero risk.

#### The reference BYK916 ICP path — **does NOT apply to the F75**

The reverse-engineering notes in [`swiftgeek/hykker-re`, issue #4](https://github.com/swiftgeek/hykker-re/issues/4)
document the BYK916 conventions and point out an appealing lead:

> `CSNA` / `CLKA` / `MOSIA` / `MISOA` — SPI/JTAG interface for BK3632, **could be used for ICP/ISP of
> flash in BK3632**.

On the reference design, the 8051 is wired to the BK3632 over SPI, which would provide ready-made
ICP access. **Verified on this keyboard: that is not the case.**

Default SPI pins on the SH68F90, per the datasheet:

| Signal | Pin |
| --- | --- |
| `SCK` | `P7.4` |
| `SS` | `P7.3` (also `INT45`) |
| `MOSI` | `P7.2` (also `INT44`) |
| `MISO` | `P7.1` (also `INT41`) |

Yet on the F75, **`P7.1`, `P7.2`, and `P7.3` are matrix rows R1, R2, and R3** — established by four
independent sources (scan table `0x72F4`, sleep parking `0x006E`, GPIO init `0xA7EC`, read routine
`0x73A6`). Those pins carry the keyboard matrix, not an SPI bus.

And the firmware confirms this itself: `fcn.0000EFA6`, called from init `fcn.0000ECC5`, executes

```asm
clr a ; mov SPCON, a ; mov SPSTA, a ; ret
```

which **explicitly disables the SPI peripheral at startup**. Exhaustive search: there is no other
access to `SPCON`, `SPSTA`, or `SPDAT` anywhere in the firmware.

**Conclusion: the F75 connects to its BK3632 via EUART0 only.** ICP through the 8051's SPI bus is a
dead end — those pins are taken by the matrix.

*(The BK3632's own SPI/JTAG pins — `GPIOA[3..7]` according to the BK3633 datasheet — obviously still
exist on the package. But nothing says they are routed to accessible pads on this PCB.)*

### In passing: the reference RGB wiring confirms the inversion

The same issue lists the BYK916 "reference" RGB wiring:

```
Row0: VR0->P4_1  VG0->P6_0  VB0->P4_0
Row1: VR1->P0_4  VG1->P6_1  VB1->P0_3
Row2: VR2->P6_7  VG2->P6_2  VB2->P6_6
Row3: VR3->P0_2  VG3->P6_3  VB3->P5_7
Row4: VR4->P4_5  VG4->P6_4  VB4->P4_6
Row5: VR5->P4_3  VG5->P6_5  VB5->P4_4
```

That is **the exact pinout** of the NuPhy Air60 (`RGB_R0R P0_4`, `RGB_R0G P6_1`, `RGB_R0B P0_3`, …),
up to a one-row shift. The Air60 therefore follows the reference design.

The F75, by contrast, places its **matrix columns on P6/P5/P4** and its **18 RGB channels on the
P1/P2/P3 PWM pins**. That is the inverted topology already documented earlier — and this issue
provides external confirmation: the F75 deliberately departs from the reference design.

#### B-bis. The Beken UART bootloader — the most promising path for a dump

⚠️ **We do not have the BK3632 firmware.** It sits inside the chip; obtaining it *is* the goal of
this section. What follows comes from the **BK3432** SDK
(`Cdreamyao/tuya_ble_sdk_Demo_Project_bk3432`), not from a dump.

##### The BIM exposes a UART bootloader

`bk3432/projects/bim/app/bim_uart.h` declares:

```c
typedef enum _UART_CMD_STATE {
    UART_CMD_STATE_HEAD, UART_CMD_STATE_OPCODE_ONE, UART_CMD_STATE_OPCODE_TWO,
    UART_CMD_STATE_LENGTH, UART_CMD_STATE_CMD, UART_CMD_STATE_CMD_FLASH,
    UART_CMD_STATE_LENGTH_FLASH_LEN0, UART_CMD_STATE_LENGTH_FLASH_LEN1,
    UART_CMD_STATE_LENGTH_FLASH_SCMD, UART_CMD_STATE_PAYLOAD, ...
} UART_CMD_STATE;

#define LINK_CHECK_CMD     0x00
#define CRC_CHECK_CMD      0x10
#define SET_RESET_CMD      0x0E
#define SET_BAUDRATE_CMD   0x0F
#define STAY_ROM_CMD       0xAA
```

Frame layout: `HEAD, OPCODE_ONE, OPCODE_TWO, LENGTH, CMD, [payload]`, with dedicated sub-states for
flash commands.

##### These are the EXACT SAME codes as the BK7231

`BK7231GUIFlashTool/BK7231Flasher/Flashers/BK7231Flasher.cs` builds its frames as follows:

```csharp
ret[0] = 0x01; ret[1] = 0xe0; ret[2] = 0xfc; ret[3] = len; ret[4] = cmd;
enum CommandCode { LinkCheck = 0, ..., CheckCRC = 0x10, SetBaudRate = 0x0f, ... }
```

`HEAD = 0x01`, `OPCODE_ONE = 0xE0`, `OPCODE_TWO = 0xFC` — matching the named states of the BK3432.
And `LinkCheck = 0x00`, `SetBaudRate = 0x0F`, `CheckCRC = 0x10` **match the BIM `#define`s**.

**The Beken family therefore shares its UART bootloader protocol.**

##### Why this changes path B

| | SPI path (described below) | UART path |
| --- | --- | --- |
| Hardware | CH341 (~€5) | **USB-TTL adapter (~€3)** |
| Physical access | SPI pads + CEN + VPP on a **QFN32** | **the UART lines, already located** |
| Entry sequence | unknown for the BK3632 | known codes, worth trying |
| Desoldering | likely | **no** |

The UART lines are the ones connecting the 8051 to the BK3632 — the exact same lines we would probe
to sniff the conversation (path A). **A single physical access point serves both purposes.**

BIM UART configuration, taken from `bim_uart.c`: **115,200 baud, 8 data bits, EVEN parity, 1 stop
bit** (`data_len 0x3`, `parity_en 0x1`, `parity_mode 0x1`, `stop_bits 0x0`). Even parity is unusual
and therefore a good discriminator: if sending `01 E0 FC 01 00` at 8E1 gets a reply, we are talking
to the bootloader.

The SDK also bundles `bk3432/doc/BK3432 Download by UART User's Guide V3.0.pdf`, which
**contradicts Tuya's documentation** ("BK3432 only supports firmware flashing through SPI").

##### What remains uncertain

- `uart_cmd_dispath()` is **not** in the shipped sources — it is declared in the header but likely
  lives in ROM. The command codes come from the header and the BK7231 cross-check.
- The BK3632 is not the BK3432.
- Entering the bootloader requires keeping the chip in ROM at boot — `STAY_ROM_CMD 0xAA` hints at a
  mechanism, undocumented here.
- On the keyboard, the BK3632's UART is wired to the 8051: we would likely need to keep the 8051
  quiet during the attempt.

#### B. Dumping the BK3632 firmware over SPI — heavier

The BK3632 is an **ARM9** with 20 KB of RAM, 160 KB of flash, BLE 5.0 + proprietary 2.4 GHz.

On its sibling the **BK3432**, the [Tuya documentation](https://developer.tuya.com/en/docs/iot/burn-and-authorize-BK3432-chip?id=Katc9lq3p4w1t)
gives the pinout: MOSI/MISO on **P0.4/P0.5**, two control lines on **P0.6/P0.7**, **VPP on RST**,
plus 3 V and ground. It requires a *BEKEN SPI flasher* dongle and the *HID Download Tool* in
"SPI SOFT" mode. The firmware is split into three parts: boot, stack, and app.

What that concretely entails for the F75:

1. Reaching the `P0.4`–`P0.7`, `RST`, `VCC`, and `GND` pads of a **QFN32** on the PCB — magnifier,
   fine probes, or even desoldering.
2. A programmer. Either the vendor *BEKEN SPI flasher* dongle, or the community path described
   below — **though the BK3632 init sequence is not established**.
3. Then disassembling a 160 KB **ARM9** binary: a brand-new target.

##### The Beken "SPI flash mode" method, and its limitation

Beken's SPI mode is documented in detail for the **BK7231T**
([elektroda, p.kaczmarek2](https://www.elektroda.com/rtvforum/topic3931424.html)). The principle:
the chip **exposes its own internal flash as an SPI slave memory**, which identifies itself as an
EN25QH16B.

```
SPI mode 3, 30 kHz          (higher frequencies fail)
CEN low -> wait 1 s -> CEN high
send 0xD2 x 250             -> reply: one 0xD2 followed by 249 x 0x00
identify: 9F 00 00 00       -> 00 15 70 1C
```

After that, standard SPI flash commands apply: `0x03` page read, `0x02` page program, `0x20` sector
erase, **256-byte** pages, 3-byte addresses. Writing requires erasing first.

Pins on the BK7231: `P20`–`P23` = SCK, CSN, SI, SO, plus **CEN** (reset).

Recommended tooling today: [`BK7231GUIFlashTool`](https://github.com/openshwprojects/BK7231GUIFlashTool)
with a **CH341 programmer** (~€5), in "Beken SPI" mode rather than generic mode, with **CH341 pin D2
wired to CEN** to drive reset. That is much more accessible than the Raspberry/Banana Pi setup from
the original article, or the vendor dongle.

**⚠️ No tool *declares* support for the BK3xxx family — but the SPI entry code is generic.**

Verified:
[`BK7231GUIFlashTool`](https://github.com/openshwprojects/BK7231GUIFlashTool) (428 ★, maintained,
last push 2026-08-10) lists its supported modes — BK7231M/N/T/U, BK7236, BK7238, BK7252, BK7252N,
BK7258, "Beken SPI CH341", and "Generic SPI CH341" — covering **only the BK72xx WiFi family**. A
search for `BK3xxx` across all of its C# source files returns **zero hits**.

The "Generic SPI CH341" mode is meant for reading an *external* SPI flash chip (SOIC8 package). The
BK3632's flash is **internal**: reading it requires the SPI slave mode entry sequence, which is
documented only for the BK72xx family.

#### Nuance: the Beken SPI entry sequence is parameterized

`BK7231Flasher/Flashers/SPIFlasher_Beken.cs` is **90 lines long** and turns out to be largely
chip-agnostic:

```csharp
ChipReset()                  // CEN low via CH341 D2 -> 100 ms -> CEN high
BK_EnterSPIMode(byte data)   // <-- the magic byte is a PARAMETER
    // sends `data` x 250 (10 blocks of 25)
    // then 9F 00 00 00
    // succeeds if resp[0] != 0x00 AND resp[1..3] == 0x00
    // then flushes: 25,000 zeros
Sync()                       // 10 attempts of (reset + entry), 1 s apart
```

Two points matter:

1. **`0xD2` is not hardcoded** — it is the argument to `BK_EnterSPIMode`. Trying another byte value
   is a one-line change.
2. **The success check does not require ID `00 15 70 1C`** — only "first byte non-zero, next three
   bytes zero". It would therefore accept the ID of a different chip.

The tool does not *claim* support for the BK3xxx family, but nothing in this code path technically
rules it out. **Trying it costs a CH341 (~€5) and a trivial code edit.**

#### What remains uncertain

- Does the BK3632 even expose an SPI slave mode? Unestablished.
- Tuya's BK3432 documentation mentions **VPP on RST**, suggesting a mechanism different from a
  simple CEN toggle — possibly a programming voltage.
- The SPI pins already differ between the BK7231 (`P20`–`P23`) and the BK3432 (`P0.4`–`P0.7`). Those
  of the BK3632 are unknown.

*(`BK7231Flasher.cs` itself implements the BK72xx **UART bootloader** protocol — `01 e0 fc <len>
<cmd>` frames — unrelated to the SPI path.)*

**Whether this carries over to the BK34xx family is NOT established.** The exact question was asked
on that thread on July 21, 2025 — "I have a bk3432 chip […] is the initialization sequence the
same?" — and the only reply, in October 2025, was "Just try". Nobody has posted a confirmation.

Two clues that it **differs**: the SPI pins are `P20`–`P23` on the BK7231 but `P0.4`–`P0.7` on the
BK3432, and VPP goes through RST. The BK3632 is yet another member of the family.

And at the end of the road, we would get the protocol implementation rather than live traffic —
whereas path A gives the live traffic directly.

**Conclusion: path A first.** Path B is only justified if we want to modify the BK3632 firmware
itself.

*(The elektroda threads cited here are unreachable from this environment — systematic timeouts,
likely datacenter IP filtering. Read them from a browser.)*

### What is ported, and what is not

The five objections that used to sit here have been lifted one by one through the work above: the
command codes have their semantics, the received status frame is decoded, and mode entry is settled
(mode switch on `P7.4`/`P4.5`, command `0x01`). The fifth objection remains, and cannot be lifted by
disassembly: **nothing has been flashed, so nothing has been executed.**

SMK's `src/platform/bk3632/rf_controller.c` assumes the Air60's bit-banged SPI and cannot be reused
here; the EUART0 transport was therefore written separately. `meson.build` declares
`'wireless': 'euart0'` for this keyboard, which adds `-DRF_EUART0=1` and rejects any `debug_sink`
other than `console` — EUART0 can only have one master.

| File | Contents |
| --- | --- |
| `src/keyboards/aula-f75/aula_rf.c` | the driver: ISR, frames, checksum, mode-switch state machine, presence probe |
| `src/keyboards/aula-f75/aula_rf.h` | the API, keeping the two slot encodings distinct (`rf_link_t` vs. `rf_slot_t`, which collide on value 0) |
| `src/keyboards/aula-f75/kb.c` | HID report routing, and `LNK_BT1..3` on `Fn`+`1/2/3` |
| `src/platform/sh68f90/interrupts.h` | claims the `_INT_EUART0` vector, with a build error if `DEBUG_SINK_UART` also claims it |

Three deliberate departures from the factory firmware, each commented in the code:

1. **The initial transport is read from the mode switch**, not assumed to be wired. The factory
   firmware starts at `g_transport = 0` and lets debouncing converge; during those ten ticks,
   keystrokes would go out over USB even when the physical slider says "wireless".
2. **Command `0x01` is queued**, rather than fired once and forgotten. The factory firmware drops it
   if `P4.7` happens to be low at that exact instant; `rf_task()` replays it as soon as the module
   reports ready.
3. **`usb_hw_deinit()` instead of a bare `anl USBCON,#0x7F`**: shuts down the module the same way,
   plus disarms the USB interrupt — without which SMK's ISR would keep firing on a powered-down
   peripheral.

#### Three bugs fixed after the fact

Cross-reading the driver against this page caught three bugs, including one that made link detection
completely non-functional.

**1. Received frame length depends on the frame type.** The initial draft waited for 22 bytes on
every frame and summed the first 21 — the format of `0x08` alone. A ten-byte status frame therefore
sat incomplete in the buffer until bytes from the next frame filled it up, the checksum always
failed, and the presence probe declared the link permanently dead.

The factory firmware does not split frames in its ISR either: it pushes bytes into a 23-byte IDATA
buffer and drops any overflow; `euart0_parse` then dispatches on byte 0 and checks the checksum
**at the offset appropriate for that frame type**. The port now does the same, including the two
factory guards on bytes 1 and 2 of the status frame.

**2. `usb_deinit()` killed NKRO in wireless mode.** It resets the entire USB state machine to zero,
including `interface0_protocol`, whereas `host_nkro_active()` requires `USB_PROTOCOL_REPORT`. The
keyboard therefore fell back to 6KRO as soon as it entered wireless mode, even though
`NKRO_REPORT_BITS` is set to 20 *("limited by wireless dongle hid descriptor")*. `usb_hw_deinit()`
only shuts down the hardware — closer to the factory `anl USBCON,#0x7F`, and NKRO survives.

**3. Two bugs in `aula_rgb.c`**, still inactive at this stage: `value * (0x04B0 / 255)` — integer
division evaluates to **4**, so maximum brightness gave a duty cycle of 180 instead of 0; and two
72-byte `const __xdata` address tables were replaced with the `SET_PWM_DUTY_2` tokens from
`platform/sh68f90/pwm.h`.

Measured build cost (SDCC 4.5.0, zero warnings, `check_interrupts` OK):
**+251 B flash**, **+4 B XDATA**, **3 B internal RAM** — internal RAM margin goes from 22 down to
19 bytes, stack stays at 222.

## Flash-emulated EEPROM — IAP

The F75 has no dedicated EEPROM (SMK's `settings_store` is `flash` for the `sh68f90`). Persistent
settings are written **into the program flash itself**, via an IAP routine traced end-to-end below.

### `fcn.0000AAC1` — saving a settings page

Twelve callers, including the EUART0 parser cluster (`0x77CE`, `0x77FF`, `0x7830`, `0x7861`,
`0x7892`, `0x78C3`, `0x78F4`) and `fcn.0000AF99` (publishing an RGB effect, at `0xAFB3`): this is
the routine that **makes a setting permanent**, whether it came from wireless or from keystrokes.

```asm
mov  dptr,#0x0F18 ; save r6, r7, r5 to 0x0F18-0x0F1A
mov  c, EA ; mov 0x2E.5, c   ; save EA state
clr  EA                      ; all interrupts disabled
lcall fcn.0000EEE3           ; release matrix (see below)
movx a, [0x0F1A] ; -> 0x0302 ; page number
lcall fcn.0000AEE4           ; erase page
...
mov  r3,#0x01 ; r2,#0x09 ; r1,#0xBF    ; source = 0x09BF
movx [0x0F20] = 0x02, [0x0F21] = 0x00  ; length = 0x0200 = 512
lcall fcn.00000181           ; write page
movx [0x0302] = 0
lcall fcn.0000EDBB           ; relock
mov  c, 0x2E.5 ; ...         ; restore EA
```

### `fcn.0000AEE4` — the bounds check, and thus the map of the region

```asm
mov a,r7 ; setb c ; subb a,#0x62 ; jc  ret   ; reject page < 99
mov a,r7 ;         subb a,#0x76 ; jc  body   ; reject page > 117
...
add  a, 0xE0                                 ; direct E0H = ACC -> a = r7 × 2
mov  0xF7, a                                 ; XPAGE = page × 2
mov  r7,#0xE6 ; lcall fcn.0000989F           ; trigger erase
```

`XPAGE` (F7H) receives `page × 2`, i.e. the high byte of the byte address: **one page is 512
bytes** — which is the exact `sector_size` declared by SMK for the `sh68f90`.

| | |
| --- | --- |
| Allowed pages | **99 to 117** (bounds verified in code, not inferred) |
| Addresses | **`0xC600` – `0xEBFF`** |
| Size | 19 pages × 512 B = **9,728 bytes** |

The region falls entirely within the 61,440 bytes of the firmware image (`0xEBFF < 0xF000`):
consistent.

`fcn.0000989F` then writes `IB_CON1..IB_CON5` (F2H–F6H) after checking **a second time** that
`XPAGE / 2` lies within `[99, 117]` and that `EA` is zero — a double guard against out-of-bounds
erasure.

### Verification against the dump: the region is live, and half-used

Entropy per 512-byte page across `assets/f75_firmware.bin`:

| Page | Address | Entropy | Zero bytes |
| --- | --- | --- | --- |
| 98 | `0xC400` | 6.53 | 46 | *(code — outside region)* |
| 99 | `0xC600` | 1.20 | 425 |
| 100 | `0xC800` | 1.10 | 230 |
| 101 | `0xCA00` | 0.22 | 494 |
| 102–105 | `0xCC00`–`0xD3FF` | 1.0 – 1.9 | 412 – 455 |
| 106–109 | `0xD400`–`0xDBFF` | 0.04 | 510 |
| 110–117 | `0xDC00`–`0xEBFF` | **0.00** | **512** |
| 118 | `0xEC00` | 6.40 | 11 | *(code — outside region)* |

Pages 98 and 118, just outside the bounds, are dense code (entropy > 6); pages 99 through 117 have
low entropy and consist mostly of zeros. **The bounds deduced from the code match the exact
code/data boundary observed in the dump** — an independent confirmation. The last eight pages are
completely blank: the region is oversized relative to what the factory firmware actually stores
there.

Page 101 (`0xCA00`) is the one saved by `fcn.0000AF99`: the constants `r6 = 0xCA` (202 = 101 × 2)
and `r5 = 101` at site `0xAFB3` finally make sense — that is **the page where the current RGB effect
is persisted**.

### `fcn.0000EEE3` — fifth confirmation of the column pinout

Called right before disabling interrupts, it releases the matrix — scanning being suspended during
flash writes:

```asm
mov 0xC0, #0xFF   ; P6 = 0xFF    -> columns P6.0 .. P6.7
orl 0x88, #0x87   ; P5 |= 0x87   -> columns P5.0, P5.1, P5.2, P5.7
orl 0xB0, #0x0D   ; P4 |= 0x0D   -> columns P4.0, P4.2, P4.3
```

The three masks reproduce **the exact same** 15-column map established elsewhere. That makes a fifth
independent source, found without even looking for it.

> ⚠️ This routine has **never been executed**: nothing has been flashed to the device. Everything
> above comes from disassembly, not runtime observation.

## Recovery: what the bootloader truly guarantees

Verified by disassembling the bootloader from **this** unit (`assets/f75_full.bin`, region
`0xF000-0xFFFF`). Two recovery paths exist.

### 1. At reset — the bootloader decides

```asm
0xf000   clr EA ; mov SP,#0x70 ; clock init
0xf017   mov 0x3b,#0xef ; mov 0x3c,#0xfb   ; DPTR <- 0xEFFB
0xf024   movc a, @a+dptr                   ; read byte at 0xEFFB
0xf025   xrl  a, #0x02                     ; LJMP opcode?
0xf027   jz   0xf053                       ; yes -> jump to firmware
         ...                               ; no  -> init USB + PLL, STAY IN ISP
```

The bootloader **always** runs first (`0x0000: LJMP 0xF000`). If the byte at `firmware_size-5` is
not `0x02`, it enumerates over USB in ISP mode. And `sinowisp write` **never** touches the
bootloader region.

**Limitation**: a properly linked SMK firmware *will* have that `0x02`. This path therefore only
saves us from a truncated or corrupted write, not from a firmware that boots and then crashes.

### 2. From the firmware — the magic entry point `0xFF00`

```asm
0xff00   xrl a, #0x5a     ; A must be 0x5A
0xff02   jnz 0xff36
0xff04   mov a, B         ; B must be 0xA5
0xff06   xrl a, #0xa5
0xff08   jnz 0xff36
0xff0a   clr EA ; mov SP,#0x70
0xff15   mov USBCON,a ; USBIF1,a ; USBIF2,a ; USBIE1,a ; USBIE2,a ; USBADDR,a
```

This is **the exact protocol** of SMK's `isp_jump()` (`src/platform/sh68f90/isp.c`:
`mov B,#0xa5 ; mov A,#0x5a ; ljmp 0xff00`), which SMK already exposes over USB
(`src/smk/usb.c:442`). **This bootloader implements it**, so the software escape hatch will work —
provided the flashed firmware's USB stack actually starts up.

### Residual risk & JTAG hardware recovery (`sinodude` on ESP32-S3)

| Scenario | Recoverable? |
| --- | --- |
| Truncated image, no `0x02` at `0xEFFB` | ✅ bootloader stays in ISP on plug-in |
| Firmware boots and USB enumerates | ✅ `isp_jump()` over USB |
| **Firmware boots but hangs before its USB stack (or corrupt reset vector)** | ✅ **recovered via JTAG (`sinodude` on ESP32-S3)** |

When a flashed image hangs before USB starts or corrupts the reset vector / trampoline so the board cannot enter ISP over USB, the SH68F90A's 4-wire JTAG interface (`TCK`, `TMS`, `TDI`, `TDO` + `GND` on the PCB debug pads) provides full hardware recovery.

**Verified on this AULA F75 hardware using an ESP32-S3 (`ESP32-S3-Zero`) port of `sinodude`:**
- Because the ESP32-S3 operates at native **`3.3 V` CMOS logic**, it connects directly to the AULA F75 JTAG pads with **zero external resistors or Zener diodes** (unlike 5 V AVR boards):
  - `GPIO4` → `TCK`
  - `GPIO5` → `TMS`
  - `GPIO6` → `TDI`
  - `GPIO7` → `TDO`
  - `GND` → `GND`
- The ESP32-S3 port implements the exact `sinodude` USB-serial protocol (`115200 8N1` over the ESP32-S3 native USB CDC-ACM port, enumerating with `VID_16C0&PID_05DF` for `avrdude -c sinodude`) and the 3-step `calibration()` entry timing window while the keyboard is power-cycled.
- Full flash recovery (`aula-f75_default_full.hex`, containing both the SMK firmware and the factory `0xF000–0xFFFF` ISP bootloader + `0x0000: LJMP 0xF000` reset vector and `0xEFFB` trampoline) was tested and verified on the physical board via:
  ```sh
  avrdude -C avrdude.conf -c sinodude -p sh68f90a -P COM7 -U flash:w:aula-f75_default_full.hex:i
  ```

Safety net status:

| | |
| --- | --- |
| Factory firmware dump | ✅ `assets/f75_full.bin`, bootloader bit-for-bit identical to upstream reference |
| Bootloader preserved by `sinowisp write` | ✅ verified on hardware, `0xF000+` region untouched |
| Return to ISP at reset | ✅ verified on this unit (`0xF024`) |
| Software return to ISP (`isp_jump`) | ✅ verified on this unit (`0xFF00`) |
| External JTAG programmer (`sinodude`) | ✅ **verified on this unit** using an ESP32-S3 (`esp32s3`) port (`GPIO4..7` direct 3.3 V JTAG, `avrdude -c sinodude -p sh68f90a`) |
| `sinowisp` write tested on this model | ✅ **verified on this unit** across multiple firmware builds |
