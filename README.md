# Vial for the AULA F75, on SMK

Open firmware for the **Epomaker × AULA F75** that speaks **[Vial](https://get.vial.today/)**: remap
it, set up tap-hold, tap dance, combos, key overrides and lighting from Vial or
[vial.rocks](https://vial.rocks) in a browser — on Linux, macOS and Windows — and move your layout to
and from any other Vial keyboard. No vendor software, no JSON to load: the keyboard serves its own
definition.

![The AULA F75 in Vial (vial.rocks)](docs/images/vial-keymap.png)

This is a downstream of **[SMK](https://github.com/karstx/smk)** (Small Mechanical Keyboard firmware,
for SinoWealth 8051 keyboards) and its [AULA F75 port](https://github.com/Dozairu/smk_aulaF75): it adds
a Vial layer and a QMK-style key engine on top. All the groundwork is theirs — see [Credits](#credits).

## ⚠️ Read this before flashing

***The code here is tested only on my own aula-f75, I am not liable for any damage done to your keyboard.***

Flashing replaces the keyboard's firmware. Done as below it is reversible, but only if you keep a
backup and know the way back.

**What you are risking.** `sinowisp` writes the new firmware *through the keyboard's own factory
bootloader* (the last 4 KB of flash, `0xF000–0xFFFF`), which it ***should*** never touch. As long as that
bootloader can be reached, the keyboard can always be flashed again — including back to stock.

**Before the first flash, back up the stock firmware** and keep the files somewhere safe, outside
this repository (they are AULA's, and they are your only copy):

```sh
sudo sinowisp read -d aula-f75 -s full     --format bin f75_stock_full.bin
sudo sinowisp read -d aula-f75 -s firmware --format bin f75_stock_firmware.bin
sha256sum f75_stock_*.bin > SHA256SUMS
```

**Going back to stock** at any time:

```sh
sudo sinowisp write -d aula-f75 --format bin --reboot true --force f75_stock_firmware.bin
```

**If a flash goes wrong:**

| What you see | What to do |
|---|---|
| The keyboard works (types, shows up over USB) | Nothing special: flash again. `sinowisp` asks the running firmware to jump into the bootloader. |
| Flash interrupted / keyboard dead, but it shows up as `0603:1020` when plugged in | The bootloader is waiting (it stays there when the firmware is incomplete). Flash again. |
| Nothing at all over USB, ever (firmware hangs before USB starts) | Only hardware recovery: open the case and use the JTAG pads with [sinodude](https://github.com/karstx/sinodude) — verified on this board with an ESP32-S3; see [docs/keyboards/aula-f75.md](docs/keyboards/aula-f75.md) ("JTAG hardware recovery"). |

**Don't:** unplug during a flash; flash a build made for another keyboard; flash on battery
(use the cable, switch on wired). This has been tested on one unit (classic F75); other hardware
revisions may differ. You do this at your own risk.

## What it adds

- **Vial protocol 6** with VialRGB: the keyboard serves its compressed
  definition, unlock with Esc + Enter, keymap / layers / encoder / macros / matrix tester.
- **A QMK-style key engine** ([`src/smk/action.c`](src/smk/action.c), portable C, host-tested):
  layers (`MO TG TO DF TT LM OSL`), mod-tap / layer-tap with tapping term, permissive hold, hold on
  other key, retro tapping, quick tap; one-shot mods and layers; modded keycodes.
- **Tap dance** (8), **combos** (8), **key overrides** (4) — configured from Vial's tabs.
- **QMK settings**: tap-hold, combo term, one-shot keys, Grave Escape, Magic (NKRO, Caps/Ctrl,
  Alt/GUI, Ctrl/GUI swaps…).
- **Quantum keycodes**: `QK_BOOT`, `EE_CLR`, Caps Word, Repeat / Alt Repeat, Layer Lock, tri-layer,
  Grave Escape, Space Cadet, combo on/off, the RGB / RM lighting keys.
- **Lighting from Vial**: every Aula effect (single colour and rainbow), speed, brightness, colour.
- **Keep your setup across flashes** (`*_flash_keep`), or flash clean (`*_flash`).
- **Wireless fixes**: 2.4 GHz / Bluetooth reports wait for the radio module's ACK and a lost frame is
  healed — no dropped taps, no stuck auto-repeating keys.
- Smaller things: a clearer charging / full / low-battery status LED, `-Drgb_default_off`, a
  media-first F-row (`-Dfn_row_media`), indicator toggle keys.

Everything about the Vial side — what works, settings, lighting names, storage, tools, known gaps:
[docs/vial.md](docs/vial.md). The keyboard port itself (wireless, RGB engine, encoder…):
[docs/keyboards/aula-f75.md](docs/keyboards/aula-f75.md).

## Getting started (AULA F75)

1. **Get the code and the tools.** The build environment comes from [Nix](https://nixos.org/) (with
   flakes enabled): `nix develop` gives SDCC, meson, `sinowisp` and the rest.

   ```sh
   git clone -b vial https://github.com/mathaimp/smk_aulaF75.git
   cd smk_aulaF75
   nix develop
   ```

2. **Back up the stock firmware** — see the warning above.

3. **Build.** `autotype` (Fn+Space anti-idle mode) and `rf_diag` (Fn+R radio diagnostics) are
   optional extras; leaving them out frees code space.

   ```sh
   meson setup build-vial -Dvia=enabled -Dvial=enabled --buildtype=release \
         -Dautotype=false -Drf_diag=false
   meson compile -C build-vial aula-f75_default_smk.hex
   ```

4. **Flash** over USB, cable in, switch on wired:

   ```sh
   meson compile -C build-vial aula-f75_default_flash        # clean: everything to defaults
   meson compile -C build-vial aula-f75_default_flash_keep   # keep your keymap and settings
   ```

   Without root this needs a udev rule (below); otherwise run the above with `sudo` — the commands
   are in [docs/vial.md](docs/vial.md).

5. **Open Vial** ([desktop](https://get.vial.today/) or [vial.rocks](https://vial.rocks) in a
   Chromium-based browser). The keyboard shows up as "SMK Keyboard".

**Linux permissions.** Vial and `sinowisp` need read/write access to the keyboard's `hidraw` nodes, in
normal mode (`258a:010c`) and in bootloader mode (`0603:1020`). On NixOS:

```nix
services.udev.extraRules = ''
  KERNEL=="hidraw*", ATTRS{idVendor}=="258a", ATTRS{idProduct}=="010c", MODE="0660", GROUP="users"
  KERNEL=="hidraw*", ATTRS{idVendor}=="0603", ATTRS{idProduct}=="1020", MODE="0660", GROUP="users"
'';
```

**Wireless.** The mode switch selects wired / 2.4 GHz / Bluetooth. Bluetooth: Fn+1/2/3 picks a slot,
the same key again (or Fn+`) starts pairing; the PC sees "AULA-F75 5.0 KB".

## Supported keyboards

Vial is enabled on the AULA F75 only. The other SMK boards still build here, with VIA where they
support it:

| Keyboard | MCU | Wireless | Extra | Details |
| -------- | --- | -------- | ----- | ------- |
| Epomaker × AULA F75 | SH68F90A / BYK916 | 2.4G, BT (BDR+BLE) | Rotary encoder, [Vial](docs/vial.md) | [Details](docs/keyboards/aula-f75.md) |
| NuPhy Air60 v1 | SH68F90A / BYK916 | 2.4G, BT (BDR+BLE) | | [Details](docs/keyboards/nuphy-air60.md) |
| E-YOOSO Z11 | SH68F90A / BYK901 | N/A | | [Details](docs/keyboards/eyooso-z11.md) |
| Genesis Thor 300 | SH68F881 / BYK801 | N/A | | [Details](docs/keyboards/genesis-thor-300.md) |
| CIY X77 | SH68F89 / BYK816 | N/A | External 24Cxx EEPROM | [Details](docs/keyboards/ciy-x77.md) |

## Programming tools

| Tool | Hardware | Interface | Notes |
| ---- | -------- | --------- | ----- |
| [sinowisp](https://github.com/karstx/sinowisp) | None, just USB | ISP | Writes the application through the board's ISP bootloader (stock or SMK's). Cannot write the bootloader region, so it can't recover a board whose bootloader is gone or unreachable. Used by the `*_flash` / `*_flash_keep` targets. |
| [sinodude](https://github.com/karstx/sinodude) + [sinodude-serial](https://github.com/karstx/sinodude/tree/main/firmware) | Arduino Nano (ATmega328P/PB), or an ESP32-S3 port | ICP / JTAG | Reads and writes the whole flash, including the bootloader: the hardware recovery path. Used by the `*_full_flash` targets. |
| [sinodude](https://github.com/karstx/sinodude) + SinoLink | SinoWealth SinoLink dongle | ICP | Same, through the vendor's dongle (`-Dicp_programmer=sinolink`). |
| ProWriter + SinoLink | SinoWealth SinoLink dongle | ICP | SinoWealth's own Windows tool; write `<board>_<layout>_full.hex`. |

SMK also ships [its own ISP bootloader](docs/bootloader.md) (hold **S + M + K** while plugging in to
reach ISP whatever the firmware does). It can only be installed over ICP/JTAG; the flow above keeps
the AULA factory bootloader instead.

## Development

```sh
meson setup build                                  # any board, default options
meson compile -C build nuphy-air60_default_smk.hex # <board>_<layout>_smk.hex
meson compile -C build nuphy-air60_default_flash   # write it with sinowisp
meson test -C build-vial action                    # key engine unit + stress tests (host gcc)
```

- **Options** (`meson configure`): `via`, `vial`, `nkro`, `sleep`, `rgb_default_off`,
  `fn_row_media`, `autotype`, `rf_diag`, … — see [`meson.options`](meson.options).
- **Simulator tests** (`tests/`, patched uCsim from the flake) exercise USB and the report path on
  the NuPhy Air60 target; run them against a debug build (`--buildtype=debug`).
- **[`tools/vial_probe.py`](tools/vial_probe.py)** checks the whole Vial protocol on a connected
  keyboard, read-only.
- **Debug console**: debug builds send their log as HID reports; `smk-console`
  ([tools/smk-console](tools/smk-console)) prints them:
  `cargo run --release --manifest-path tools/smk-console/Cargo.toml -- 258a:010c`.

More: [VIA](docs/via.md), [Vial](docs/vial.md), [ISP bootloader](docs/bootloader.md), platform notes
for the [SH68F90 / SH68F90A](docs/platforms/sh68f90.md), [SH68F881](docs/platforms/sh68f881.md) and
[SH68F89](docs/platforms/sh68f89.md).

## Credits

- **[SMK](https://github.com/karstx/smk)** by **Karolis Stasaitis ([karstx](https://github.com/karstx))**
  and contributors — the firmware this is built on: the SinoWealth 8051 platform, USB stack, ISP
  bootloader, build system and simulator tests, plus the
  [sinowisp](https://github.com/karstx/sinowisp) and [sinodude](https://github.com/karstx/sinodude)
  flashing tools. VIA / dynamic keymap support was started by aierm.
  [SMK on Discord](https://discord.gg/SZFBDBuxrK).
- **[smk_aulaF75](https://github.com/Dozairu/smk_aulaF75)** by **[Dozairu](https://github.com/Dozairu)**
  (with commits from james and Thaolia L.) — the AULA F75 port, reverse-engineered from a stock
  firmware dump: wireless (2.4 GHz / Bluetooth), the RGB engine and effects, encoder, battery,
  indicators, VIA menus.
- **[Vial](https://get.vial.today/)** and **[QMK](https://qmk.fm/)** — the protocol, GUI and the key
  behaviours this firmware follows, so layouts stay portable.
