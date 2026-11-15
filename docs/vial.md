# Vial

SMK can speak the [Vial](https://get.vial.today/) protocol on top of [VIA](via.md): the keyboard
serves its own layout definition (no JSON to load), and Vial GUI / [vial.rocks](https://vial.rocks)
can edit tap-hold, tap dance, combos, key overrides, QMK settings and lighting — on Linux, macOS and
Windows, and portable to and from any other Vial keyboard (File → Save / Load layout).

Currently ported board: **AULA F75** (`src/keyboards/aula-f75`).

## Building and flashing

```sh
nix develop
meson setup build-vial -Dvia=enabled -Dvial=enabled --buildtype=release
meson compile -C build-vial aula-f75_default_smk.hex
meson test -C build-vial action               # key engine unit tests (host gcc)
```

Two flash targets (both write over USB ISP with `sinowisp`):

| Target | Effect |
|---|---|
| `meson compile -C build-vial aula-f75_default_flash` | clean: keymap, macros, Vial settings and lighting back to defaults |
| `meson compile -C build-vial aula-f75_default_flash_keep` | keeps them: reads the current flash first, carries the saved-data sectors into the new image ([`utils/flash_keep.py`](../utils/flash_keep.py)); if the read fails, nothing is written |

`flash_keep` cannot carry data across a **storage version** change (see below): the firmware rejects
the old layout at boot and starts from defaults, exactly like a clean flash. Save your layout in
Vial first (File → Save current layout) and load it back after.

Without root, `sinowisp` needs access to the keyboard in both modes; on NixOS (where
`services.udev.extraRules` lands in `99-local.rules`, too late for `uaccess`) use a group:

```nix
services.udev.extraRules = ''
  KERNEL=="hidraw*", ATTRS{idVendor}=="258a", ATTRS{idProduct}=="010c", MODE="0660", GROUP="users"
  KERNEL=="hidraw*", ATTRS{idVendor}=="0603", ATTRS{idProduct}=="1020", MODE="0660", GROUP="users"
'';
```

### Enabling on a board

`-Dvial=enabled` needs `-Dvia=enabled`. A board opts in through its `meson.build` options:

| Option | Meaning |
|---|---|
| `vial_uid` | 8 random bytes (hex), fixed forever: Vial GUI identifies the keyboard model by it |
| `vial_unlock_rows`, `vial_unlock_cols` | the unlock chord (F75: Esc + Enter), held ~5 s when Vial asks |
| `vialrgb` | `'true'`: the board implements `vialrgb.h` (Vial's Lighting tab) |
| `vial_json` | definition file, default `vial.json` — compressed into the firmware by [`utils/vial_definition.py`](../utils/vial_definition.py) |

Boards without `vial_uid` stay plain VIA.

## What works

| Vial tab | Support |
|---|---|
| Keymap, Layers | 4 layers, every key + the encoder (ccw / press / cw) |
| Macros | VIA macros incl. Vial's delays and extended keycodes |
| Lighting (VialRGB) | effect, speed, brightness, colour picker — see below |
| Tap Dance | 8 entries: tap, hold, double tap, tap + hold, own tapping term |
| Combos | 8 entries, up to 4 keys each, any keycode as output |
| Key Overrides | 4 entries (trigger-down activation, see gaps) |
| QMK Settings | see below |
| Matrix tester | yes (when unlocked) |

### Key engine

[`src/smk/action.c`](../src/smk/action.c) is a portable re-implementation of the QMK action
semantics Vial expects (host-tested: `tests/host/test_action.c`):

- layers: `MO` `TG` `TO` `DF` `TT` `LM` `OSL`, `LT`, layer mask + default layer, transparent keys;
- tap-hold `MT` / `LT` / `TT` with tapping term, permissive hold, hold on other key press, retro
  tapping, quick tap term; one-shot mods / layers with timeout and tap toggle; modded keycodes;
- tap dance, combos (a buffering stage in front of the engine), key overrides;
- quantum keycodes: `QK_BOOT`, `EE_CLR`, Grave Escape, Space Cadet, Caps Word, Repeat, Alt Repeat
  (QMK's built-in pairs), Layer Lock, tri-layer (`FN_MO13` / `FN_MO23`), combo on/off, Magic
  keycodes, the `RGB_*` / `RM_*` lighting keys.

Vial hides **Repeat / Alt Repeat** from its picker on keyboards without an Alt Repeat table; assign
them with the **Any** key (`QK_REP`, `QK_AREP`).

### QMK settings

| Tab | Settings |
|---|---|
| Magic | swap Caps/LCtrl, Caps as Ctrl, swap Alt/GUI (L, R), swap Ctrl/GUI (L, R), no GUI, swap `` ` ``/Esc, swap `\`/Backspace, NKRO |
| Grave Escape | the four "always Escape with …" overrides |
| Tap-Hold | tapping term, permissive hold, hold on other key press, retro tapping, quick tap term, tapping toggle |
| Combo | combo term |
| One Shot Keys | tap toggle, timeout |

### Lighting (VialRGB)

[`aula_vialrgb.c`](../src/keyboards/aula-f75/aula_vialrgb.c) maps Vial onto the Aula effect engine.
Vial only knows QMK's effect names, so every Aula effect appears twice — single colour and rainbow —
under the closest QMK name:

| Aula effect | single colour | rainbow |
|---|---|---|
| Solid | Solid Color | Cycle All |
| Horizontal Wave | Gradient Left Right | Cycle Left Right |
| Vertical Wave | Gradient Up Down | Cycle Up Down |
| Radial | Band Spiral Val | Cycle Out In |
| Vertical Rainbow | Band Sat | Rainbow Moving Chevron |
| Key Wave | Band Pinwheel Val | Hue Wave |
| Shimmer | Breathing | Hue Breathing |
| Gaming (WASD) | Alphas Mods | Rainbow Beacon |
| Reactive | Solid Reactive Simple | Solid Reactive |
| Rain | Digital Rain | Raindrops |
| Twinkle | Pixel Rain | Jellybean Raindrops |
| Snake | Pixel Fractal | Rainbow Pinwheels |
| Ripple | Solid Splash | Splash |
| Lake | Solid Multisplash | Multisplash |

The colour picker sets colour mode 8 ("custom"): the hue goes through the factory colour wheel and
saturation blends toward white, like the stock colours (Vial's on-screen swatch will not match the
LEDs exactly). Brightness maps onto the 10 stock levels, speed onto the 5. The Fn lighting keys and
the VIA custom menu keep working; Vial reads their changes back. The indicator toggles and scroll
direction stay in the VIA custom menu (channel 3), which Vial has no UI for.

## Storage

The VIA store (4 × 512 B flash sectors at `0xE400`, mirrored in XRAM) gains a Vial area after the
encoder map:

| Bytes | Content |
|---|---|
| 32 | QMK settings, the VialRGB hue / saturation (layout in `vial.c`) |
| 8 × 10 | tap dances (`vial_tap_dance_entry_t`) |
| 8 × 10 | combos (`vial_combo_entry_t`) |
| 4 × 10 | key overrides (`vial_key_override_entry_t`) |

The structs are vial-qmk's, little-endian, and are also the wire format. Storage version history
(`dynamic_keymap.c`): 2 QMK settings, 3 + tap dance, 4 + combos / key override space, 5 32-byte
settings. A version change resets the store once (see Flashing). Factory reset (Fn+Esc held ~3 s)
or `EE_CLR` clears everything, including the Vial area and the engine's live state.

## Tools

- [`tools/vial_probe.py`](../tools/vial_probe.py) — read-only check of the whole Vial protocol over
  hidraw: VIA / Vial versions, UID, definition (optionally compared with a `vial.json`), encoder,
  unlock, QMK settings, every tap dance / combo / key override, feature flags, VialRGB.
- [`utils/flash_keep.py`](../utils/flash_keep.py) — the keep-settings flash.

## Known gaps

- **Auto Shift** and Vial's **Alt Repeat table**: left out for code space.
- **Key overrides**: no activation by pressing a required mod / releasing a negative mod after the
  trigger; no mods-only triggers.
- **VialRGB direct mode** (per-LED colour from the host): its frame buffer does not fit in XRAM.
- **Mouse keys, MIDI, joystick, steno, Unicode, leader, swap hands, dynamic macros, `QK_REBOOT`**:
  not implemented.
- **More than 4 layers**: each layer costs 192 B of XRAM; ~270 B are left.
- **Stale raw HID reply**: if a host tool sends a request and exits without reading the reply, the
  next tool reads that old reply first and stays one behind. `vial_probe.py` drains it on open; for
  Vial / VIA, replug the keyboard. (A firmware fix needs a way to withdraw a queued EP2 IN packet on
  the SH68F90, which is not documented.)

## Budget (AULA F75, Vial build)

Code 57,939 / 58,368 B, XRAM 3,828 / 4,096 B. Further features need code to be freed first.
