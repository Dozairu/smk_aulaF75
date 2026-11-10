#pragma once

/*
 * AULA F75 wireless link: BK3632 connected over EUART0.
 *
 * Unrelated to `src/platform/bk3632/rf_controller.c`, which talks to the same SoC
 * over bit-banged SPI (NuPhy Air60). Here the transport is the SH68F90A's
 * EUART0, with its own framing and command set.
 *
 * Everything below comes from disassembling the factory firmware
 * (`docs/keyboards/aula-f75.md`). The code has been flashed and runs, but the
 * link itself has never been established: see the diagnostic overlay.
 */

#include <stdint.h>
#include <stdbool.h>
#include "report.h"

/*
 * Active transport. This is the factory firmware's LOCAL encoding (`XRAM 0x031B`),
 * not what goes over the wire -- see rf_slot_t.
 */
typedef enum {
    RF_LINK_WIRED = 0, /* USB: radio is silent, USB is initialized */
    RF_LINK_24G   = 1, /* 2.4 GHz dongle */
    RF_LINK_BT    = 2  /* Bluetooth, slot in rf_bt_slot() */
} rf_link_t;

/*
 * "Slot" byte of command 0x01, as sent over the wire.
 *
 * WARNING: this encoding is NOT that of rf_link_t, and the two collide on
 * value 0 -- which means "wired" in rf_link_t and "2.4 GHz" on the wire.
 * Never pass an rf_link_t where an rf_slot_t is expected. Conversion is done
 * in rf_link_slot().
 */
typedef enum {
    RF_SLOT_24G = 0,
    RF_SLOT_BT1 = 1,
    RF_SLOT_BT2 = 2,
    RF_SLOT_BT3 = 3
} rf_slot_t;

void rf_init(void);

/*
 * Call from the main loop, not from an ISR: samples the slide switch, applies
 * transport changes, and consumes received frames.
 */
void rf_task(void);

rf_link_t rf_link(void);
bool      rf_is_wireless(void);

/*
 * "The module answered a recent probe."
 *
 * This is NOT the factory firmware's connection flag: the status-frame field
 * that carries pairing was not isolated in `euart0_parse`. What is transcribed
 * is its presence probe (`fcn.00003901`, command 0x06 every hundred iterations,
 * three failures and the link is declared dead) — so the indicator drops, but
 * it says "the radio is responding", not "a host is paired".
 *
 * `rf_task()` copies it into `keyboard_state.connected`.
 */
bool rf_connected(void);

uint8_t rf_bt_slot(void);
void    rf_set_bt_slot(uint8_t slot);


/*
 * Reasserts the current link with the "pairing" flag of command 0x01.
 * The factory firmware does the same from its slow tick when the pairing
 * shortcut has set bit 0x2C.5 (`fcn.0000870C`).
 */
void rf_request_pairing(void);

/*
 * Call from `user_sleep_prepare()` / `user_sleep_wake()`, around
 * `power_enter_powerdown()`. No-op in wired mode.
 */
void rf_sleep_prepare(void);
void rf_sleep_wake(void);

/*
 * ---------------------------------------------------------------- DIAGNOSTIC
 *
 * State snapshot, populated on every pass of `rf_task()` and read by the
 * overlay in `indicators.c` (`Fn + R` key). This is the ONLY usable instrument
 * in the failing setup -- on battery, USB unplugged -- where the HID console
 * does not exist.
 *
 * An array rather than an accessor: the overlay is painted from the Timer2 ISR,
 * and calling into `aula_rf.c` from the ISR would make the module share the
 * main loop's SDCC static overlay.
 */
#define RF_DIAG_P74    0 /* selector, pin 1 */
#define RF_DIAG_P45    1 /* selector, pin 2 */
#define RF_DIAG_P47    2 /* "module ready" */
#define RF_DIAG_LINK   3 /* current rf_link_t */
#define RF_DIAG_TXPEND 4 /* link-select still pending transmission */
#define RF_DIAG_NAME   5 /* number of Bluetooth names sent, 0 to 2 */
#define RF_DIAG_TXBUSY 6 /* serial burst in flight */
#define RF_DIAG_CONN   7 /* module answered a recent probe */
#define RF_DIAG_QCOUNT 8 /* queued reports, 0 to 6 -- saturates if nothing goes out */
#define RF_DIAG_MISSES 9 /* unanswered status probes */
/*
 * Reception. These three split "did the module even open its mouth?" into two
 * distinct questions: if RXTOT stays at zero, nothing is arriving on the wire
 * and the fault is upstream of any decoding; if it climbs, the module is
 * talking and our frame parser is rejecting it.
 */
#define RF_DIAG_RXTOT  10 /* bytes received since boot, modulo 256 */
#define RF_DIAG_RXLAST 11 /* last byte received -- what the module says */
#define RF_DIAG_RXIDX  12 /* current fill level of the receive buffer */
#define RF_DIAG_FIELDS 13

extern __xdata uint8_t rf_diag_state[RF_DIAG_FIELDS];

void rf_send_report(__xdata report_keyboard_t *report);
void rf_send_nkro(__xdata report_nkro_t *report);
void rf_send_extra(__xdata report_extra_t *report);
