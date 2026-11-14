#pragma once

#include "sh68f90.h"
#include "keycodes.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Epomaker x AULA F75 (classic model, NOT the "F75 Ultra")
 *
 *   MCU      SinoWealth SH68F90A, IC marking BYK916
 *   USB      258A:010C  (read from factory flash at 0x5FCE)
 *   Wireless Beken BK3632, connected via EUART0 -- see note below
 *
 * WARNING: this port is INCOMPLETE by design. The pinout has not been
 * established and cannot be guessed from the factory firmware without further
 * work. Missing symbols intentionally trigger a compilation error: a file that
 * refuses to compile is a valid deliverable, a file that compiles and bricks
 * the keyboard is not.
 *
 * Reverse-engineering notes: docs/keyboards/aula-f75.md
 */

/* -------------------------------------------------------------------------
 * Matrix
 *
 * Established: OpenRGB's color table exposes num_leds = 90 and indexes LEDs
 * column-major -- index = col * 6 + row. The max index is 89 (right arrow)
 * and index 6 (between Esc and F1) is the F-row gap.
 * So the LED grid is 6 rows x 15 columns.
 *
 * NOT established: whether the key matrix shares this geometry. That is the
 * usual case on these keyboards, but the factory firmware did not confirm it --
 * no scan loop bound has been isolated. A 16th column without LEDs would
 * remain invisible to OpenRGB.
 * ------------------------------------------------------------------------- */
#define MATRIX_ROWS 6
#define MATRIX_COLS 15
#define SIGNALRGB_ENABLE 1

/* -------------------------------------------------------------------------
 * Pinout -- ESTABLISHED BY DISASSEMBLY, TRIPLE-CHECKED
 *
 * Corroborating sources:
 *  1. consolidated GPIO init in the factory firmware, fcn @ 0xA7EC:
 *       P0CR=0x9C  P1CR=0x3F  P2CR=0x3F  P3CR=0x3F
 *       P4CR=0x4D  P5CR=0x87  P6CR=0xFF  P7CR=0x60
 *     (CR bit set to 1 = output, cf. GPIO_OUTPUT in platform/sh68f90/gpio.h)
 *  2. column selection jump table @ 0x72F4: 20 slots of which 15 are
 *     populated, each handler releasing the previous column (setb) before
 *     selecting the next one (clr) -- slot 0 releases P4.3, closing the loop
 *     and establishing P4.3 as the last column.
 *  3. independent port tiagoluizo/smk@aula-f75-port, which arrives at the same
 *     pinout through separate analysis.
 *
 * Column selection ACTIVE LOW. Rows ACTIVE LOW (internal pull-ups).
 * ------------------------------------------------------------------------- */
#define AULA_F75_PINMAP_VERIFIED 1

/* Rows -- inputs. Confirmed: P7CR=0x60 leaves P7.0-3 as inputs,
 * P5CR=0x87 leaves P5.3 and P5.4 as inputs. */
#define KB_R0_P7_0 _P7_0
#define KB_R1_P7_1 _P7_1
#define KB_R2_P7_2 _P7_2
#define KB_R3_P7_3 _P7_3
#define KB_R4_P5_3 _P5_3
#define KB_R5_P5_4 _P5_4

#define KB_R0 P7_0
#define KB_R1 P7_1
#define KB_R2 P7_2
#define KB_R3 P7_3
#define KB_R4 P5_3
#define KB_R5 P5_4

/* Columns -- outputs, in scan order read from the table @ 0x72F4 */
#define KB_C0_P6_0  _P6_0
#define KB_C1_P6_1  _P6_1
#define KB_C2_P6_2  _P6_2
#define KB_C3_P6_3  _P6_3
#define KB_C4_P6_4  _P6_4
#define KB_C5_P6_5  _P6_5
#define KB_C6_P6_6  _P6_6
#define KB_C7_P6_7  _P6_7
#define KB_C8_P5_0  _P5_0
#define KB_C9_P5_1  _P5_1
#define KB_C10_P5_2 _P5_2
#define KB_C11_P5_7 _P5_7
#define KB_C12_P4_0 _P4_0
#define KB_C13_P4_2 _P4_2
#define KB_C14_P4_3 _P4_3

#define KB_C0  P6_0
#define KB_C1  P6_1
#define KB_C2  P6_2
#define KB_C3  P6_3
#define KB_C4  P6_4
#define KB_C5  P6_5
#define KB_C6  P6_6
#define KB_C7  P6_7
#define KB_C8  P5_0
#define KB_C9  P5_1
#define KB_C10 P5_2
#define KB_C11 P5_7
#define KB_C12 P4_0
#define KB_C13 P4_2
#define KB_C14 P4_3

/* Per-port masks, derived from the pins above */
#define KB_C_P4_MASK (uint8_t)(KB_C12_P4_0 | KB_C13_P4_2 | KB_C14_P4_3)
#define KB_C_P5_MASK (uint8_t)(KB_C8_P5_0 | KB_C9_P5_1 | KB_C10_P5_2 | KB_C11_P5_7)
#define KB_C_P6_MASK (uint8_t)(KB_C0_P6_0 | KB_C1_P6_1 | KB_C2_P6_2 | KB_C3_P6_3 | \
                               KB_C4_P6_4 | KB_C5_P6_5 | KB_C6_P6_6 | KB_C7_P6_7)
#define KB_R_P5_MASK (uint8_t)(KB_R4_P5_3 | KB_R5_P5_4)
#define KB_R_P7_MASK (uint8_t)(KB_R0_P7_0 | KB_R1_P7_1 | KB_R2_P7_2 | KB_R3_P7_3)

/* Backlight: 18 PWM outputs = 6 physical LED rows x R/G/B.
 * Confirmed by P1CR=P2CR=P3CR=0x3F (all 18 PWM pins set as outputs).
 * MCU mapping deduced from the NuPhy Air60: PWM0x<->P3_x, PWM1x<->P2_x,
 * PWM2x<->P1_x. The matrix columns serve as multiplexing selectors.
 *
 * There are NO LED_PWM_Cn macros here, unlike the other keyboards in SMK:
 * on those, a PWM channel drives an LED column, whereas here it drives a
 * (row, color). The channel -> register mapping lives in a single place,
 * `aula_rgb_load_column()`, in the factory firmware's loading order --
 * which is not the natural channel number order. A second table with a
 * different index origin would only have served to contradict itself.
 *
 * One full LED sweep -- all fifteen columns -- between two matrix scans,
 * like the genesis-thor-300, the only other keyboard in SMK that shares its
 * columns between scanning and LED multiplexing.
 *
 * Interleaved column scan (`MATRIX_INTERLEAVED_SCAN`): instead of blanking the
 * whole panel for a ~320 us full-matrix sweep every 6.4 ms, each 200 us Timer2
 * slot scans one key column (~20 us) and immediately enables hardware PWM on
 * that same column for one hardware-timed 100 us period (_INT_PWM0 parks all
 * three banks inside the 0..DUTY1 dead-time window). Effect math runs in
 * main()'s indicators_render() so the ISR stays short and never starves USB or
 * EUART0.
 */
#define LED_SUBFRAMES_PER_SCAN  MATRIX_COLS
#define MATRIX_INTERLEAVED_SCAN 1

/*
 * Per-column key state snapshot, bit N = row N pressed.
 * Fed by the scan hooks in `user_matrix.c`, consumed by the reactive engine:
 * `matrix.c` keeps its array private and its hooks do not pass the position,
 * whereas our own hooks do see it.
 */
uint8_t user_matrix_pressed(uint8_t col);

/* Ten brightness steps: the factory gain table at CODE 0x2937 has ten entries
 * (00 08 10 18 20 28 32 3c 46 50), bounded by CODE[0xA8D5+effect] = 9. */
#define LED_BRIGHTNESS_LEVELS 10

/* -------------------------------------------------------------------------
 * Wireless link
 *
 * The NuPhy Air60 talks to its BK3632 over bit-banged SPI (RF_BB_SPI_*,
 * src/platform/bb_spi.c). The AULA F75 does NOT work like that: its factory
 * firmware uses EUART0 (vector 13, SCON 0xD8 / SBUF 0xAA), in half-duplex --
 * the ISR toggles pin direction via P0CR and then P0.2.
 *
 * Consequence: src/platform/bk3632/rf_controller.c is NOT reusable as-is
 * for this keyboard. The EUART0 transport was therefore written separately in
 * aula_rf.c, and meson.build declares 'wireless': 'euart0' for this keyboard.
 *
 * Connection selector, three positions (established by fcn.000084E9):
 *   P7.4 = 0            -> 2.4 GHz (dongle)
 *   P7.4 = 1, P4.5 = 1  -> USB wired
 *   P7.4 = 1, P4.5 = 0  -> Bluetooth, slot selected by LNK_BT1..3
 * P4.7 is the "module ready" line: no frame is sent while it is low.
 *
 * STATUS: transcribed from disassembly, then CONFIRMED ON DEVICE -- typing via
 * the 2.4 GHz dongle and Bluetooth pairing. The initial failure was not in the
 * driver: `tick.c` scans the entire matrix inside the Timer2 ISR (~320 us) and,
 * at 260,870 baud, a byte arrives every 38 us with no FIFO on SBUF. At equal
 * priority EUART0 could not preempt and dropped eight bytes per scan.
 * `rf_uart_init()` therefore sets `IPH1/IPL1 |= _ES0`, like the factory
 * firmware (IPH1=0x42, IPL1=0x41). The RF_DIAG keycode stays: it is the tool
 * that revealed this, and the only one usable on battery.
 * ------------------------------------------------------------------------- */

enum custom_keycodes {
    FX_NEXT = SAFE_RANGE, /* next RGB effect */
    FX_PREV,              /* previous RGB effect */
    BRI_UP,               /* brightness + */
    BRI_DN,               /* brightness - */
    SPD_UP,               /* speed + */
    SPD_DN,               /* speed - */
    CLR_NEXT,             /* next color: 0-6 fixed, 7 rainbow */
    DIR_TOG,              /* scroll direction: factory bit 0x23 */

    /*
     * Bluetooth slots. The transport itself comes from the slide switch, not
     * from a key: these three ONLY select the slot, like command 0x01 in the
     * factory firmware. Pressing the already-active slot restarts pairing --
     * which is what the factory firmware does when bit 0x2C.5 is set (flag 1
     * of command 0x01, fcn.0000870C).
     */
    LNK_BT1,
    LNK_BT2,
    LNK_BT3,

    /*
     * Pairing restart, on `Fn + \`` -- the position of the factory shortcut
     * (key 0x08 of dispatcher 0x4136, arm 0x426A).
     *
     * DELIBERATE DIVERGENCE: the factory arm requires the 2.4 GHz transport
     * and therefore only re-pairs the dongle. Here the command is sent with
     * `rf_link_slot()`, which returns the slot of the CURRENT transport: the
     * same key re-pairs the dongle in 2.4 GHz mode and the active slot in
     * Bluetooth mode. No-op in wired mode.
     */
    RF_PAIR,

    /*
     * Radio diagnostic overlay on keys `1` to `0`. Wireless fails when the
     * keyboard is on battery with USB unplugged: the HID console does not
     * exist in that setup, whereas the LEDs do.
     */
    RF_DIAG,

    /*
     * "Auto-typing" mode, on `Fn + Space` AND `Fn + encoder press`.
     * Types a random letter then deletes it with Backspace, every 1 s +/- 500 ms,
     * until any key stops it. No factory equivalent. See `aula_macro.c`.
     */
    MACRO_TG,

    /*
     * Battery level overlay on keys `1` to `0` (row 1, cols 1 to 10).
     * Shows 1-10 keys in green/yellow/red for ~3 seconds on demand.
     */
    BAT_SHOW,

    /*
     * Stock firmware shortcuts recovered from dispatcher 0x412C:
     *   WIN_LOCK    0x01  Win key lock toggle
     *   WASD_SWAP   0x1C  WASD <-> Arrow keys swap toggle
     *   OS_WIN      0x18  Windows OS mode (standard Alt/Gui)
     *   OS_ANDROID  0x19  Android OS mode (standard Alt/Gui)
     *   OS_MAC      0x1A  Mac OS mode (swaps LAlt <-> LGui)
     *   OS_IOS      0x1B  iOS OS mode (swaps LAlt <-> LGui)
     *   FACTORY_RST 0x04  Reset settings, VIA keymap & macros (hold ~2s)
     */
    WIN_LOCK,
    WASD_SWAP,
    OS_WIN,
    OS_ANDROID,
    OS_MAC,
    OS_IOS,
    FACTORY_RST,

    KB_SAFE_RANGE,
};
