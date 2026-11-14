#include "aula_rf.h"

#include "sh68f90.h"
#include "interrupts.h"
#include "delay.h"
#include "usb.h"
#include "usbhw.h"
#include "keyboard.h"
#include "aula_status.h"
#include "settings.h"
#include "debug.h"
#include <string.h>

/*
 * AULA F75 EUART0 <-> BK3632 link.
 *
 * Transcribed from the factory firmware. Quoted addresses refer to
 * `docs/keyboards/aula-f75.md`, which carries the full reasoning.
 *
 * ESTABLISHED by disassembly: baud rate and registers, both handshakes, frame
 * format and checksum, command set, and the transport selector state machine
 * (`fcn.000084E9`) -- including the fact that command 0x01 with slot 0 is what
 * switches the module to 2.4 GHz.
 *
 * INFERRED, and flagged as such at every site: the exact byte layout of the
 * key report frame payloads (0x02 / 0x03), and the status-frame field that
 * signals "connected".
 *
 * Nothing has ever been flashed to the device: this code has not been executed.
 */

/* --------------------------------------------------------------------- link */

/*
 * Baud rate: SBRTH/SBRTL/SFINE recorded at 0xB1CE.
 *
 *   SBRT   = 0x7FFB (SBRTH bit 7 = SBRTEN, so the value is 0x7FFB)
 *   Baud   = Fsys / (16 * (32768 - SBRT) + SFINE)
 *          = Fsys / (16 * 5 + 12) = Fsys / 92
 *          = 24 MHz / 92 ~= 260,870 baud
 *
 * We keep the factory firmware literals rather than computing from a nominal
 * baud rate: the peer module dictates the value, and it does not match any
 * standard baud rate.
 */
#define RF_SBRTH 0xFF
#define RF_SBRTL 0xFB
#define RF_SFINE 0x0C
#define RF_SCON  0x50 /* mode 1, 8N1, reception enabled */

_Static_assert(FREQ_SYS / 92 > 255000 && FREQ_SYS / 92 < 267000, "FREQ_SYS incompatible with the factory divider (Fsys/92)");

/* Transmit buffer: 32 B, the exact size used by the factory firmware. */
#define RF_TX_MAX 32

/*
 * Reception: the factory firmware does NOT slice frames in its ISR.
 *
 * Its ISR (0xA544) pushes bytes into a 23-byte IDATA buffer (0x54-0x6A), drops
 * anything beyond that, and sets a simple "byte received" flag (0x24.4). A
 * deferred stage (`fcn.000005EA`) copies the 23 bytes into XDATA 0x0120, and
 * `euart0_parse` dispatches on byte 0 and verifies the checksum AT THE OFFSET
 * SPECIFIC TO EACH TYPE:
 *
 *   0x02  status frame  10 bytes, sum = 0x55 - sum[0..8],  compared with [9]
 *   0x08  bulk frame    22 bytes, sum = 0x55 - sum[0..20], compared with [21]
 *   0x03  announce      NO checksum verified, length not established
 *
 * An initial draft of this file waited for 22 bytes for ALL frames and summed
 * the first 21: the format of 0x08 alone. A ten-byte status frame therefore
 * stayed pending, bytes from the next frame filled out the buffer, the checksum
 * always failed, and the link was permanently declared dead.
 */
#define RF_RX_MAX 23 /* factory buffer, IDATA 0x54-0x6A */

/*
 * Lengths recorded ON HARDWARE, once byte loss was fixed. The length depends
 * on byte 1 -- the acknowledged command -- and not on the frame type alone.
 *
 *   02 06 00 00 00 02 dd 03 01 6a   TEN bytes: status probe response
 *   02 01 00 00 00 52               SIX: link-select ACK
 *   02 02 00 00 00 51               SIX: report ACK
 *   03 00 00 00 00 52               SIX: announce
 *
 * All verified by the factory checksum `0x55 - sum`. This port originally waited
 * for ten bytes on ANY frame of type 0x02: on a six-byte ACK it swallowed the
 * start of the next frame and never regained sync.
 */
#define RF_RX_LEN_STATUS 10
#define RF_RX_LEN_ACK    6
#define RF_RX_LEN_BULK   22

#define RF_RX_TYPE_STATUS   0x02
#define RF_RX_TYPE_ANNOUNCE 0x03
#define RF_RX_TYPE_BULK     0x08

#define RF_HDR      0x01 /* byte 0, constant in both directions */
#define RF_SUM_SEED 0x55 /* sum = 0x55 - sum, same constant in both directions */

/* Commands, recorded one by one across their twelve emitters. */
#define RF_CMD_LINK     0x01 /* slot + pairing flag */
#define RF_CMD_REPORT_L 0x02 /* long frame: 27 B payload */
#define RF_CMD_REPORT_S 0x03 /* short frame: 10 B payload */
#define RF_CMD_STATUS   0x06 /* status request -> 02 06 ... response */
#define RF_CMD_NAME     0x09 /* Bluetooth name, 32 bytes */
#define RF_CMD_SETTINGS 0x0B /* one parameter; sent when leaving wireless mode */
#define RF_CMD_WIRED    0x0E /* switch to wired mode */

/*
 * Not sent here, but recorded in the factory firmware: 0x04 (one parameter,
 * sent periodically with value 3), 0x08 (sub-command container), 0x0C (two
 * parameters, 7 and 8, sent from sleep preparation at `0x7D9B` when `[0x0E3B]`
 * is non-zero), and 0x0D (sending the battery percentage BACK to the module,
 * which is the one that gave it to us). They require data that this port does
 * not have.
 *
 * The RECEIVED 0x08 container carries thirteen sub-codes, all decoded: five
 * configuration page writes (0x01 remap, 0x02 per-key lighting, 0x03 eight
 * profiles, 0x04 settings, 0x09), seven reads (0x05, 0x41-0x44, 0x49, 0x4A),
 * and one action, 0x06, which immediately triggers a factory reset. None of
 * them affect pairing. This port implements none of them: it does not expose
 * its configuration to the module.
 */

/* Total lengths, checksum included. */
#define RF_LEN_SHORT     6
#define RF_LEN_NAME      32
#define RF_LEN_REPORT_L  30
#define RF_LEN_REPORT_S  13

/*
 * Command 0x01 flag: byte 2 on the wire. The factory firmware sends 0
 * everywhere except on one single path, traced end-to-end:
 *
 *   key 0x08 of the shortcut dispatcher (`0x4136`) -> arm `0x426A`, which
 *   requires the 2.4 GHz transport, resets hold counter `[0x0961:0x0962]` and
 *   sets bit `0x2C.5`; slow tick `0x870C` waits for that 16-bit counter to
 *   reach 3000, then `0x87BF` emits `rf_link_select(slot, 1)`.
 *
 * This is the SAME long-press counter as factory reset (key 0x04, bit `0x2B.7`),
 * and both bits are consumed one after the other in the same tick. Reading
 * "1 = restart pairing" remains *inferred* -- nothing in the 8051 code says
 * what the BK3632 does with it -- but it is now backed by a dedicated shortcut
 * rather than just the difference between two call sites.
 */
#define RF_LINK_SELECT  0
#define RF_LINK_PAIRING 1

static __xdata uint8_t          tx_buf[RF_TX_MAX];
static volatile __data uint8_t  tx_len;
static volatile __data uint8_t  tx_idx;
static volatile __bit           tx_busy;

static __xdata uint8_t         rx_buf[RF_RX_MAX];
/*
 * RAW STREAM. Per-frame capture is not enough: several frames arrive between
 * two passes of `rf_task()` -- the first capture showed `02 00 01 00 00 52`
 * TWICE in a single buffer -- and frame slicing is precisely what is under
 * suspicion. So we latch the first sixty-four bytes as they fall out of the
 * ISR, with no interpretation at all, and frame analysis is done off-chip.
 */
#if DEBUG == 1
#    define RF_CAP_RAW 64
static __xdata uint8_t cap_raw[RF_CAP_RAW];
static __xdata uint8_t cap_raw_n;
static __xdata uint8_t cap_raw_shown;
#endif

/* Raw counters, in XDATA: internal RAM only has nineteen bytes of headroom. */
static volatile __xdata uint8_t rx_total;
static volatile __xdata uint8_t rx_last;
static volatile __data uint8_t rx_idx;
static volatile __bit          rx_pending;

/*
 * Only tx_len/tx_idx/rx_idx stay in DATA: the ISR touches them on every byte.
 * Everything else goes to XDATA -- 8051 internal RAM is the scarce resource, and
 * at 260 kbaud a byte takes ~38 us, a thousand times the cost of an XDATA access.
 */
static __xdata rf_link_t link_state;
static __xdata uint8_t   bt_slot;
static __bit             link_connected;
static __bit             vbus_present;
static __bit             rf_charging;
static __xdata uint8_t   vbus_on_cnt;
static __xdata uint8_t   vbus_off_cnt;
static __xdata uint8_t   chg_on_cnt;
static __xdata uint8_t   chg_off_cnt;
static __xdata uint8_t   chg_poll_div;
static __xdata uint8_t   prev_batt_pct = 0xFFu;
static __xdata uint8_t   rx_flag_last;

__xdata uint8_t rf_bt_pair_slot;
__xdata uint8_t rf_bt_pair_fast;
__xdata uint8_t rf_charging_active;
__xdata uint8_t rf_vbus_active;

/*
 * Pending 0x01 command. The factory firmware sends it only once, when the
 * transport changes, and loses it if `P4.7` is low at that exact moment (its
 * transmit prologue then rejects the frame without retrying). We record the
 * intent instead, and `rf_task()` replays it as soon as the module declares
 * itself ready: this is the only place where this port departs from the factory
 * firmware to plug a hole, and it has no effect when the module answers right
 * away.
 */
static __bit           link_tx_pending;
static __xdata uint8_t link_tx_flag;

/*
 * Broadcast of both Bluetooth names, once per boot. The factory firmware sends
 * them from `main`, without caring whether the module is listening; we spread
 * them across two passes of `rf_task()`, as soon as `P4.7` allows transmission.
 *   0 -> BT 3.0 name to send   1 -> BT 5.0 name to send   2 -> done
 */
static __xdata uint8_t name_stage;

/* The persisted slot can only be read on the first pass of `rf_task()`. */
static __bit settings_restored;

/*
 * BLUETOOTH SLOT PERSISTENCE.
 *
 * `user_settings.rf_link` is unused on this keyboard. Elsewhere that field
 * carries the NuPhy Air60's `rf_mode_t` encoding, but its sole consumer,
 * `restore_rf_link()` in `src/main.c`, is guarded by `#ifdef RF_ENABLED` -- and
 * `RF_ENABLED` and `RF_EUART0` are mutually exclusive (`meson.build`). On the
 * F75 the transport comes from the hardware slide switch, never from NVM, so
 * nothing reads that field.
 *
 * We store the SLOT there, not the link: it is the only thing the slide switch
 * does not tell us, and without this it fell back to 1 on every boot. The alias
 * name is there so nobody reads `rf_link` expecting to find a link mode in it.
 */
#define AULA_SETTINGS_BT_SLOT user_settings.rf_link

/*
 * Presence probe, transcribed from `fcn.00003901`: command 0x06 goes out every
 * 100 ms (250 x 400 us sub-ticks) and, after ten unanswered probes (~1.0 s),
 * the link is declared lost and the receive buffer is flushed.
 *
 * Because the BK3632 only reports host HID LED state (`rx_buf[3]`: Num/Caps/
 * Scroll Lock) in response to `0x06`, Lock-key state changes also arm fast
 * follow-up probes (every ~15 ms for 75 ms, without counting against
 * `probe_misses`) so Caps Lock LED toggles update immediately without adding
 * extra UART/radio traffic during normal typing.
 */
#define RF_PROBE_TICKS    250u /* 250 * 400 us = 100 ms */
#define RF_PROBE_MISSES   10   /* 10 * 100 ms = 1.0 s disconnect tolerance */
#define RF_FOLLOWUP_TICKS 38u  /* 38 * 400 us ~= 15 ms */
#define RF_FOLLOWUP_COUNT 5u   /* +15..75 ms window after a Lock-key change */
static volatile __xdata uint8_t probe_ticks;
static volatile __bit           probe_due;
static volatile __xdata uint8_t followup_ticks;
static volatile __bit           followup_due;
static __xdata uint8_t          followup_left;
static __bit                    followup_arm_pending;
static __bit                    lock_key_down;
static __xdata uint8_t          probe_misses;
static __bit                    probe_answered;

/*
 * Battery gauge. The SH68F90A has no ADC -- verified, no `ADC*` register in
 * `sh68f90.h`; the BK3632 reports the raw value in bytes 6 and 7 of the status
 * frame, and the factory firmware converts it in `fcn.0000801F`, once every six
 * status frames.
 */
#define RF_BATT_FRAMES    6   /* factory cadence: one status frame out of six */
#define RF_BATT_EMPTY     715 /* XRAM 0x02CB: below this, percentage is zero */
#define RF_BATT_FULL      912 /* upper threshold of the hysteresis in fcn.00001DC3 */
#define RF_BATT_LOW       737 /* lower threshold of the same hysteresis */
#define RF_BATT_LOW_CLEAR 752 /* hysteresis release threshold (~19%) */

/*
 * Damping, transcribed from CODE 0xAF8D: the DISPLAYED percentage converges
 * toward the target one point at a time, and the table index is the gap divided
 * by ten. A deceleration curve: the closer to the target, the slower it steps.
 *
 * All twelve bytes were re-read FROM THE DUMP, because the disassembly notes
 * list them as `20 10 05 02 ...` -- a notation that looks hexadecimal when it is
 * actually decimal. The actual bytes are
 * `14 0a 05 02 02 02 02 02 02 02 02 02`, i.e. 20, 10, 5, then 2 ticks: the
 * PROSE in the disassembly notes was right, not the hexadecimal reading of its
 * listing.
 */
static const __code uint8_t batt_damping[12] = {
    20, 10, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};

static __xdata uint8_t  batt_frames;  /* counts status frames */
static __xdata uint8_t  batt_target;  /* target percentage */
static __xdata uint8_t  batt_shown;   /* displayed percentage, the converging value */
static __xdata uint8_t  batt_ticks;   /* damping counter */
static __bit            batt_low;
static __bit            batt_initialized;

__xdata uint8_t rf_batt_pct;

/*
 * Key report transmit queue, transcribed from `0x0C57`.
 *
 * The factory firmware keeps a circular queue of SIX slots of TWENTY-EIGHT
 * bytes, write index at `XRAM 0x0307`, read index at `0x030C`, consumed by
 * `hid_report_radio` (`fcn.000044F6`). Byte 0 of the slot IS the command byte:
 * type 2 -> 28 bytes copied then a 30-byte frame, type 3 -> 11 bytes then a
 * 13-byte frame.
 *
 * Without it, a keystroke sent while a burst is in flight, or with `P4.7` low,
 * is lost without a trace: `rf_send_payload_long()` returned `false` and `kb.c`
 * ignored the return value.
 *
 * Overflow: we overwrite the oldest entry, just like the factory firmware (its
 * write index wraps without consulting the read index). Six slots at 260 kbaud
 * represent about two milliseconds of buffer; filling them means the module is
 * no longer responding, in which case freshness matters more than completeness.
 */
#define RF_Q_SLOTS 6
#define RF_Q_SIZE  28
static __xdata uint8_t q_buf[RF_Q_SLOTS][RF_Q_SIZE];
static __xdata uint8_t q_head;
static __xdata uint8_t q_tail;
static __xdata uint8_t q_count;

/*
 * Selector debounce counters, one per position (0x08BD/0x02DF/0x0960).
 *
 * The factory firmware requires ten consecutive passes of its slow tick. SMK
 * has no equivalent timebase here -- `rf_task()` is called from the main loop,
 * whose cadence is uncalibrated. The threshold is therefore expressed in LOOP
 * PASSES, not milliseconds, and chosen generously: the counter's job is to
 * absorb contact bounce, and sliding a mode switch does not demand low latency.
 */
#define RF_DEBOUNCE 250
static __xdata uint16_t deb_wired;
static __xdata uint16_t deb_24g;
static __xdata uint16_t deb_bt;

/*
 * Snapshot read by the diagnostic overlay in `indicators.c`.
 *
 * An array rather than an accessor: the overlay runs in the Timer2 ISR, and
 * calling from the ISR into this module would make the called function share
 * the SDCC static overlay of main-loop functions -- which is exactly what
 * `utils/check_interrupts.py` fails the build to prevent.
 */
__xdata uint8_t rf_diag_state[RF_DIAG_FIELDS];

/* --------------------------------------------------------------------- ISR */

/*
 * Vector 0x6B (_INT_EUART0 = 13). The ISR only pushes the next byte and
 * collects incoming bytes: it calls no function shared with the main loop, so
 * SDCC local-variable overlaying stays sound (which is what the
 * `check_interrupts` target verifies).
 */
void rf_euart0_interrupt_handler(void) __interrupt(_INT_EUART0)
{
    if (TI) {
        TI = 0;
        if (tx_idx < tx_len) {
            SBUF = tx_buf[tx_idx++];
        } else {
            /*
             * End of burst: P0.2 switches back to input and is released high
             * (transcribed from the factory ISR, 0xA580).
             */
            P0CR &= (uint8_t)~0x04;
            P0_2    = 1;
            tx_busy = 0;
        }
    }

    if (RI) {
        RI = 0;
        /*
         * Push and flag, nothing else -- just like the factory ISR, which
         * silently drops anything beyond its buffer. Frame slicing belongs in
         * `rf_rx_consume()`, the only place that knows the per-type formats.
         */
        const uint8_t byte = SBUF;
        rx_total++;
        rx_last = byte;
#if DEBUG == 1
        if (cap_raw_n < RF_CAP_RAW) {
            cap_raw[cap_raw_n++] = byte;
        }
#endif
        if (rx_idx < RF_RX_MAX) {
            rx_buf[rx_idx++] = byte;
        }
        rx_pending = 1;
    }
}

/* ------------------------------------------------------------ transmission */

/*
 * Prologue shared by all twelve factory emitters: send nothing if a burst is
 * already in progress (bit 0x2C.1) or if the module is not ready (P4.7 low).
 *
 * BUT there are TWO gates, and distinguishing them is what gets the keyboard
 * out of silence. The factory firmware checks P4.7 before reports; it does NOT
 * check it before control commands -- it sends both Bluetooth names from `main`
 * (0x9148, 0x914D) with no guard at all. Treating control like a report creates
 * a deadlock: if P4.7 stays low, link-select -- the only command that actually
 * switches the module -- is never sent, so the module never activates, so P4.7
 * never goes high.
 *
 * Hence the fallback: control respects the line as long as it eventually rises,
 * then overrides it. An in-flight burst is never overwritten in either case.
 */
#define RF_GATE_REPORT 0
#define RF_GATE_CTRL   1

/* ~200 main-loop passes: a few tens of milliseconds. */
#define RF_CTRL_STALL 200

static __xdata uint8_t ctrl_stall;

static bool rf_can_send(uint8_t gate)
{
    if (tx_busy) {
        return false;
    }
    if (P4_7) {
        ctrl_stall = 0;
        return true;
    }
    if (gate != RF_GATE_CTRL) {
        return false;
    }
    if (ctrl_stall < RF_CTRL_STALL) {
        ctrl_stall++;
        return false;
    }
    return true;
}

/*
 * `len` is the TOTAL length, checksum included. Bytes 0 through len-2 must
 * already be placed in tx_buf.
 */
static bool rf_send_frame(uint8_t len, uint8_t gate)
{
    uint8_t sum = RF_SUM_SEED;

    if (!rf_can_send(gate) || len < 2 || len > RF_TX_MAX) {
        return false;
    }

    for (uint8_t i = 0; i < (uint8_t)(len - 1); i++) {
        sum -= tx_buf[i];
    }
    tx_buf[len - 1] = sum;

    tx_len = len;
    tx_idx = 1; /* byte 0 goes out immediately, the ISR chains the rest */
    tx_busy = 1;

    /*
     * Transmit request: P0.2 is pulled low BEFORE switching it to output, then
     * pulled low again -- the exact order of the factory firmware (0xAB13),
     * avoiding a spurious high glitch when changing direction.
     */
    P0_2 = 0;
    P0CR |= 0x04;
    P0_2 = 0;

    SBUF = tx_buf[0];
    return true;
}

static void rf_frame_begin(uint8_t cmd)
{
    memset(tx_buf, 0, RF_TX_MAX);
    tx_buf[0] = RF_HDR;
    tx_buf[1] = cmd;
}

static bool rf_send_short(uint8_t cmd, uint8_t p0, uint8_t p1, uint8_t gate)
{
    rf_frame_begin(cmd);
    tx_buf[2] = p0;
    tx_buf[3] = p1;
    return rf_send_frame(RF_LEN_SHORT, gate);
}

/*
 * Command 0x01: radio selector. THIS is what puts the module into 2.4 GHz mode
 * -- `fcn.0000ED89`, called via `fcn.0000EF5A`.
 *
 *   01 01 <flag> <slot> 00 <sum>
 *
 * Slot 0 is the 2.4 GHz dongle, 1 to 3 are the three Bluetooth slots.
 * Established by `fcn.0000870C`, which picks `slot = (transport == 2) ? bt_slot : 0`
 * -- so the only possible slot in mode 1 is 0.
 *
 * WARNING: THE ORDER OF THOSE TWO BYTES REALLY IS THIS ONE, and this file had it
 * swapped for a long time. `fcn.0000ED89` writes `IDATA[0x35] = R7` then
 * `IDATA[0x36] = R5`: byte 2 comes from R7, byte 3 from R5. And R5 is what
 * receives `bt_slot` across all four call sites that send it (`0x85D7`,
 * `0x44C0`, `0x87AD`, `0x06E6`), whereas R7 is 0 everywhere except on pairing
 * restart, where it is 1.
 *
 * The clincher is the link supervisor at `0x06E6`: in Bluetooth mode, when the
 * received state does not match, it re-requests with R5 = `bt_slot` and R7 = 0.
 * If R7 carried the slot, the keyboard would request the 2.4 GHz dongle on
 * every Bluetooth link failure -- switching radios because of a wrong radio.
 */
static bool rf_send_link(rf_slot_t slot, uint8_t flag)
{
    return rf_send_short(RF_CMD_LINK, flag, (uint8_t)slot, RF_GATE_CTRL);
}

/*
 * Bluetooth name -- command 0x09, transcribed from `fcn.0000A307`, sent with
 * `r5 = 32` at `0xA376`. Established format:
 *
 *   [0] 0x01   [1] 0x09   [2] profile   [3] 0x10 = 16 (length)
 *   [4..19] sixteen name bytes          [31] sum
 *
 * The factory firmware calls it TWICE from `main` (`0x9148`, `0x914D`), once
 * per profile, with two space-padded sixteen-character literals:
 * `CODE 0xAF6D` = "AULA-F75 3.0 KB " and `CODE 0xAF7D` = "AULA-F75 5.0 KB ".
 * Parameter `[2]` selects which one.
 *
 * The names are copied verbatim from the dump, trailing padding space included.
 */
#define RF_NAME_LEN 16
static const __code char rf_name_bt3[RF_NAME_LEN] = {
    'A', 'U', 'L', 'A', '-', 'F', '7', '5', ' ', '3', '.', '0', ' ', 'K', 'B', ' ',
};
static const __code char rf_name_bt5[RF_NAME_LEN] = {
    'A', 'U', 'L', 'A', '-', 'F', '7', '5', ' ', '5', '.', '0', ' ', 'K', 'B', ' ',
};

static bool rf_send_name(uint8_t profile, const __code char *name)
{
    rf_frame_begin(RF_CMD_NAME);
    tx_buf[2] = profile;
    tx_buf[3] = RF_NAME_LEN;
    for (uint8_t i = 0; i < RF_NAME_LEN; i++) {
        tx_buf[4 + i] = (uint8_t)name[i];
    }
    return rf_send_frame(RF_LEN_NAME, RF_GATE_CTRL);
}

static rf_slot_t rf_link_slot(void)
{
    return (link_state == RF_LINK_BT) ? (rf_slot_t)bt_slot : RF_SLOT_24G;
}

static void rf_queue_link(uint8_t flag)
{
    link_tx_pending = 1;
    link_tx_flag    = flag;
}

static bool rf_send_status_probe(void)
{
    return rf_send_short(RF_CMD_STATUS, 0, 0, RF_GATE_CTRL);
}

/* ------------------------------------------------------- key report frames */

/*
 * Both key report frames come out of the same circular queue in the factory
 * firmware (six 28-byte slots at `0x0C57`); byte 0 of the slot IS the command
 * byte, 2 or 3, and determines the length. Here SMK's queue is enough: we send
 * directly.
 *
 * RECORDED, no longer inferred: both factory emitters (`0x46C2` and `0x4730`)
 * copy the slot verbatim after header `0x01`, with byte 0 of the slot serving
 * as the command. The payload itself does NOT carry the HID Report ID -- see
 * `rf_send_extra()` below, and the short-frame section of
 * `docs/keyboards/aula-f75.md`.
 */
/* Pushes a report into the queue; byte 0 of the slot is the command. */
static void rf_queue_report(uint8_t cmd, const __xdata uint8_t *data, uint8_t len)
{
    __xdata uint8_t *slot = q_buf[q_head];

    if (len > (RF_Q_SIZE - 1)) {
        return;
    }

    if (q_count >= RF_Q_SLOTS) {
        q_tail = (uint8_t)((q_tail + 1u) % RF_Q_SLOTS);
        q_count--;
    }

    memset(slot, 0, RF_Q_SIZE);
    slot[0] = cmd;
    for (uint8_t i = 0; i < len; i++) {
        slot[1 + i] = data[i];
    }

    q_head = (uint8_t)((q_head + 1u) % RF_Q_SLOTS);
    q_count++;
}

/*
 * Flushes the queue. Only one frame goes out per pass: `rf_send_frame()` kicks
 * off the burst and the ISR finishes it, so `rf_can_send()` is false on the
 * next iteration.
 */
static void rf_queue_flush(void)
{
    while (q_count != 0 && rf_can_send(RF_GATE_REPORT)) {
        const __xdata uint8_t *slot = q_buf[q_tail];
        const uint8_t          len =
            (slot[0] == RF_CMD_REPORT_L) ? RF_LEN_REPORT_L : RF_LEN_REPORT_S;

        tx_buf[0] = RF_HDR;
        for (uint8_t i = 0; i < (uint8_t)(len - 2); i++) {
            tx_buf[1 + i] = slot[i];
        }
        if (!rf_send_frame(len, RF_GATE_REPORT)) {
            return; /* keep the slot for the next pass */
        }
        if (slot[0] == RF_CMD_REPORT_L && followup_arm_pending) {
            const bool et2       = ET2;
            followup_arm_pending = 0;
            ET2                  = 0;
            followup_ticks       = RF_FOLLOWUP_TICKS;
            followup_due         = 0;
            followup_left        = RF_FOLLOWUP_COUNT;
            ET2                  = et2;
        }
        q_tail = (uint8_t)((q_tail + 1u) % RF_Q_SLOTS);
        q_count--;
    }
}

static bool rf_report_has_lock(const __xdata report_keyboard_t *report)
{
    for (uint8_t i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        const uint8_t k = report->keys[i];
        if (k == KC_CAPS_LOCK || k == KC_SCROLL_LOCK || k == KC_NUM_LOCK) {
            return true;
        }
    }
    return false;
}

static bool rf_nkro_has_lock(const __xdata report_nkro_t *report)
{
    return ((report->bits[KC_CAPS_LOCK >> 3] & (uint8_t)(1u << (KC_CAPS_LOCK & 7u))) != 0u) ||
           ((report->bits[KC_SCROLL_LOCK >> 3] & (uint8_t)(1u << (KC_SCROLL_LOCK & 7u))) != 0u) ||
           ((report->bits[KC_NUM_LOCK >> 3] & (uint8_t)(1u << (KC_NUM_LOCK & 7u))) != 0u);
}

static void rf_note_lock_key(bool lock_now)
{
    if (lock_now != lock_key_down) {
        lock_key_down        = lock_now;
        followup_arm_pending = 1;
    }
}

void rf_send_report(__xdata report_keyboard_t *report)
{
    rf_note_lock_key(rf_report_has_lock(report));
    rf_queue_report(RF_CMD_REPORT_L, report->raw, KEYBOARD_REPORT_SIZE);
}

void rf_send_nkro(__xdata report_nkro_t *report)
{
    rf_note_lock_key(rf_nkro_has_lock(report));
    rf_queue_report(RF_CMD_REPORT_L, report->raw, NKRO_REPORT_SIZE);
}

/*
 * Short-frame payload, RECORDED in the disassembler at `0x45AB` -- no longer an
 * inference:
 *
 *   [0..1]  Consumer usage, sixteen bits, little-endian       (0x09BD, 0x09BE)
 *   [2]     System byte                                       (0x097E)
 *   [3..9]  mouse report, seven bytes                         (0x09B1)
 *
 * The HID Report ID is NOT there. Yet the factory Consumer buffer starts with
 * it -- `0x09BC` holds Report ID 2 -- and the radio producer DELIBERATELY skips
 * it: the frame's command byte acts as the selector, not the Report ID. Sending
 * `report->raw` as-is shifted everything by one byte, and the dongle read `2`
 * as the low byte of the usage. That is what made the encoder wheel and media
 * keys silent over 2.4 GHz and Bluetooth while regular typing went through.
 *
 * The System byte is INFERRED: its offset is established, but no System keycode
 * is wired in this keymap, so nothing has exercised it.
 */
#define RF_EXTRA_PAY 3

/* In XDATA rather than on the stack: `rf_queue_report()` takes an `__xdata *`,
 * and under `--stack-auto` a local array lives in internal RAM. */
static __xdata uint8_t extra_pay[RF_EXTRA_PAY];

void rf_send_extra(__xdata report_extra_t *report)
{
    extra_pay[0] = 0;
    extra_pay[1] = 0;
    extra_pay[2] = 0;

    if (report->report_id == REPORT_ID_CONSUMER) {
        extra_pay[0] = (uint8_t)(report->usage & 0xFFu);
        extra_pay[1] = (uint8_t)(report->usage >> 8);
    } else if (report->report_id == REPORT_ID_SYSTEM) {
        extra_pay[2] = (uint8_t)(report->usage & 0xFFu);
    } else {
        return; /* nothing else goes through this frame */
    }

    /* The seven mouse bytes stay zero: `rf_queue_report()` clears the slot, and
     * this keyboard has no mouse functionality. */
    rf_queue_report(RF_CMD_REPORT_S, extra_pay, RF_EXTRA_PAY);
}

/* --------------------------------------------------------------- reception */

/* Factory checksum: 0x55 - sum of the first `len` bytes, compared to byte `len`. */
static bool rf_rx_checksum_ok(uint8_t len)
{
    uint8_t sum = RF_SUM_SEED;

    for (uint8_t i = 0; i < len; i++) {
        sum -= rx_buf[i];
    }
    return sum == rx_buf[len];
}

/* Clears the buffer with the IRQ masked: otherwise a byte arriving between the
 * check and the reset would end up attributed to the next frame. */
/*
 * Removes `len` bytes from the front and keeps the remainder.
 *
 * `rf_rx_drop()` used to discard the ENTIRE buffer after each frame: when two
 * frames landed between two passes -- the normal case at 260 kbaud against a
 * millisecond-scale main loop -- the second one was lost. And on an unknown
 * type, dropping everything prevented any resynchronization: we now drop a
 * SINGLE byte and retry.
 */
static void rf_rx_take(uint8_t len)
{
    const bool armed = (IEN1 & _ES0) != 0;

    IEN1 &= (uint8_t)~_ES0;
    if (len >= rx_idx) {
        rx_idx = 0;
    } else {
        const uint8_t rest = (uint8_t)(rx_idx - len);
        for (uint8_t i = 0; i < rest; i++) {
            rx_buf[i] = rx_buf[len + i];
        }
        rx_idx = rest;
    }
    rx_pending = (rx_idx != 0);
    if (armed) {
        IEN1 |= _ES0;
    }
}

static void rf_rx_drop(void)
{
    const bool armed = (IEN1 & _ES0) != 0;

    IEN1 &= (uint8_t)~_ES0;
    rx_idx     = 0;
    rx_pending = 0;
    if (armed) {
        IEN1 |= _ES0;
    }
}

/*
 * Battery gauge, transcribed from `fcn.0000801F` -- called on one status frame
 * out of six, just like in the factory firmware.
 *
 * ESTABLISHED: the source (bytes 6 and 7 of the frame, little-endian -- byte 6
 * is the low byte), the floor at 715, the ceiling at 100%, the one-in-six frame
 * cadence, and the damping curve.
 *
 * INFERRED: the SLOPE. The factory firmware clamps to the floor and ceiling,
 * but the disassembly notes do not give the scaling factor. We anchor it on the
 * only two established constants of the same quantity -- the 715 floor and the
 * 912 upper threshold of the hysteresis in `fcn.00001DC3` -- yielding a straight
 * line between the two. A 10-bit conversion across a voltage divider makes the
 * hypothesis plausible, not certain.
 */
static void rf_battery_sample(void)
{
    if (batt_initialized && ++batt_frames < RF_BATT_FRAMES) {
        return;
    }
    batt_frames = 0;

    const uint16_t raw = (uint16_t)((uint16_t)rx_buf[7] << 8) | rx_buf[6];

    if (raw <= RF_BATT_EMPTY) {
        batt_target = 0;
    } else if (raw >= RF_BATT_FULL) {
        batt_target = 100;
    } else {
        /* (912-715) * 100 = 19,700: the product fits in 16 bits, no need to
         * pull in SDCC's 32-bit division helper. */
        batt_target = (uint8_t)(((uint16_t)(raw - RF_BATT_EMPTY) * 100u) /
                                (uint16_t)(RF_BATT_FULL - RF_BATT_EMPTY));
    }

    if (raw < RF_BATT_LOW) {
        batt_low = 1;
    } else if (raw >= RF_BATT_LOW_CLEAR) {
        batt_low = 0;
    }

    if (!batt_initialized) {
        batt_initialized = 1;
        batt_shown       = batt_target;
        batt_ticks       = 0;
        return;
    }

    /* Damped convergence: one point at a time, stepping more slowly the closer
     * we get to the target. */
    if (batt_shown != batt_target) {
        const uint8_t gap   = (batt_shown > batt_target) ? (uint8_t)(batt_shown - batt_target)
                                                         : (uint8_t)(batt_target - batt_shown);
        const uint8_t index = (gap > 110) ? 11 : (uint8_t)(gap / 10u);

        if (++batt_ticks >= batt_damping[index]) {
            batt_ticks = 0;
            batt_shown = (batt_shown > batt_target) ? (uint8_t)(batt_shown - 1)
                                                    : (uint8_t)(batt_shown + 1);
        }
    } else {
        batt_ticks = 0;
    }
}

/*
 * Status frame handling, transcribed from the `byte[0] == 2` branch of
 * `euart0_parse`. This is not just decoding: it is also the factory firmware's
 * LINK SUPERVISOR, which reasserts the target link whenever the received state
 * does not match the requested transport.
 *
 *   if (transport == 2) {                       // Bluetooth
 *       if (slot == 0 || slot > 3) slot = 1;    // clamp to 1..3 in code
 *       if (frame[4] == slot && frame[5] != 0)  // connected
 *       else cmd_01(slot, 0);                   // otherwise re-request
 *   }
 *   else if (transport == 1)                    // 2.4 GHz
 *       if (frame[4] != 0 || frame[5] == 0) cmd_01(0, 0);
 *   else if (transport == 0 && frame[4] != 0x0A) cmd_0E();
 *
 * The wired branch is omitted: this port disables the EUART0 IRQ in wired mode,
 * so no status frame ever arrives there.
 *
 * `link_connected` thus stops being a proxy: it is now the module's true flag,
 * cross-checked against the expected slot.
 */
static void rf_status_apply(void)
{
    const uint8_t code = rx_buf[4];
    const uint8_t flag = rx_buf[5];

    rx_flag_last = flag;

    if (link_state == RF_LINK_BT) {
        if (bt_slot < RF_SLOT_BT1 || bt_slot > RF_SLOT_BT3) {
            bt_slot = RF_SLOT_BT1;
        }
        /* Stock `0x26E9`: `frame[5] == 3` means connected; `1` = pairing,
         * `2` = reconnecting, `0` = disconnected. Only re-request link select
         * if the module is on the wrong slot or idle (`flag == 0`). */
        if (code == bt_slot && flag == 3) {
            link_connected = 1;
        } else {
            link_connected = 0;
            if (code != bt_slot || flag == 0) {
                rf_queue_link(RF_LINK_SELECT);
            }
        }
    } else if (link_state == RF_LINK_24G) {
        if (code == 0 && flag >= 2) {
            link_connected = 1;
        } else {
            link_connected = 0;
            if (code != 0 || flag == 0) {
                rf_queue_link(RF_LINK_SELECT);
            }
        }
    }

    /* Stock firmware (`0x06B7`, `0x0716`, `0x248C`): `frame[3]` carries the host
     * HID LED bitmask (Num/Caps/Scroll Lock) in wireless status frames. */
    keyboard_state.led_state = link_connected ? (uint8_t)(rx_buf[3] & 0x07u) : 0;

    rf_battery_sample();
}

/*
 * Draining the receive buffer, along with its verdict.
 *
 * The module RESPONDS -- measured on hardware: the received-byte counter climbs
 * uninterrupted in both 2.4 GHz and Bluetooth modes. What was missing was
 * knowing what it says: without the raw bytes, we cannot tell a valid frame
 * that was misparsed apart from a misconfigured baud rate producing noise.
 */
/*
 * DEFERRED CAPTURE of the first received frames.
 *
 * Printing on the fly does not work: the console lives over USB, and wireless
 * mode is precisely when USB is unavailable -- on battery there is no cable,
 * and even plugged in, normal firmware disables the peripheral. Two captures
 * were lost because of that.
 *
 * We therefore record the FIRST THREE frames, never overwriting them, and
 * replay them as soon as the console becomes available again -- typically when
 * switching back to the wired position. Timing no longer matters.
 */
#if DEBUG == 1

#    define RF_CAP_FRAMES 3
#    define RF_CAP_LEN    12


static __xdata uint8_t cap_buf[RF_CAP_FRAMES][RF_CAP_LEN];
static __xdata uint8_t cap_len[RF_CAP_FRAMES];
static __xdata uint8_t cap_why[RF_CAP_FRAMES];
static __xdata uint8_t cap_n;
static __xdata uint8_t cap_shown;

static void rf_rx_dump(uint8_t verdict)
{
    uint8_t i;
    uint8_t len;

    if (cap_n >= RF_CAP_FRAMES) {
        return;
    }
    len = (rx_idx < RF_CAP_LEN) ? rx_idx : RF_CAP_LEN;
    for (i = 0; i < len; i++) {
        cap_buf[cap_n][i] = rx_buf[i];
    }
    cap_len[cap_n] = rx_idx;
    cap_why[cap_n] = verdict;
    cap_n++;
}

/* Replays eight raw bytes per pass, then the frames, when the console has room. */
static void rf_cap_replay(void)
{
    uint8_t i;
    uint8_t len;

    if (!console_is_drained()) {
        return;
    }
    if (cap_raw_shown < cap_raw_n) {
        dprintf("raw+%02u:", (unsigned)cap_raw_shown);
        for (i = 0; i < 8 && cap_raw_shown < cap_raw_n; i++) {
            dprintf(" %02x", (unsigned)cap_raw[cap_raw_shown++]);
        }
        dprintf("\r\n");
        return;
    }
    if (cap_shown >= cap_n) {
        return;
    }
    len = (cap_len[cap_shown] < RF_CAP_LEN) ? cap_len[cap_shown] : RF_CAP_LEN;
    dprintf("rx[%u] why=%u n=%u:", (unsigned)cap_shown, (unsigned)cap_why[cap_shown],
            (unsigned)cap_len[cap_shown]);
    for (i = 0; i < len; i++) {
        dprintf(" %02x", (unsigned)cap_buf[cap_shown][i]);
    }
    dprintf("\r\n");
    cap_shown++;
}

#else
#    define rf_rx_dump(verdict) ((void)0)
#    define rf_cap_replay()     ((void)0)
#endif

/* why= : 0 unknown type, 1 0x02 checksum/guards failed, 2 status OK, 3 announce, 4 bulk */
#define RF_WHY_TYPE  0
#define RF_WHY_BAD02 1
#define RF_WHY_OK02  2
#define RF_WHY_ANN   3
#define RF_WHY_BULK  4

static uint8_t rf_rx_frame_len(void)
{
    switch (rx_buf[0]) {
        case RF_RX_TYPE_STATUS:
            if (rx_idx < 2) {
                return RF_RX_LEN_ACK; /* wait for byte 1 before inspecting rx_buf[1] */
            }
            return (rx_buf[1] == RF_CMD_STATUS) ? RF_RX_LEN_STATUS : RF_RX_LEN_ACK;
        case RF_RX_TYPE_ANNOUNCE: return RF_RX_LEN_ACK;
        case RF_RX_TYPE_BULK:     return RF_RX_LEN_BULK;
        default:                  return 0; /* unknown type: stray byte */
    }
}

static void rf_rx_classify(void)
{
    const uint8_t type = rx_buf[0];
    if (type == RF_RX_TYPE_BULK) {
        /* Sub-command container: nothing in this port depends on it. */
        rf_rx_dump(RF_WHY_BULK);
        return;
    }
    if (type == RF_RX_TYPE_STATUS && rx_buf[1] == RF_CMD_STATUS && rx_buf[2] == 0) {
        rf_status_apply();
        rf_rx_dump(RF_WHY_OK02);
        return;
    }
    rf_rx_dump(RF_WHY_ANN);
}

static void rf_rx_consume(void)
{
    /* Several frames may have landed: we process up to four per pass, enough
     * to keep up with the module's cadence without hogging the loop.
     * Flattened control flow (single stop flag, no continue/return inside the
     * loop) and switch/classification isolated in rf_rx_frame_len()/rf_rx_classify():
     * combined, they trip up the SDCC 4.5.0 optimizer (error 110). Logic is
     * identical to the original transcription. */
    uint8_t stop = 0;
    for (uint8_t guard = 0; !stop && guard < 4 && rx_pending; guard++) {
        uint8_t len = rf_rx_frame_len();
        if (len == 0) {
            rf_rx_take(1); /* stray byte: resync rather than dropping everything */
        } else if (rx_idx < len) {
            stop = 1;      /* incomplete frame: let the rest arrive */
        } else if (!rf_rx_checksum_ok((uint8_t)(len - 1))) {
            rf_rx_dump(RF_WHY_BAD02);
            rf_rx_take(1);
        } else {
            probe_answered = 1; /* valid checksum: the module spoke, and cleanly */
            rf_rx_classify();
            rf_rx_take(len);
        }
    }
}

/* --------------------------------------------------------- state machine */

static void rf_uart_init(void)
{
    SCON  = RF_SCON;
    SBRTH = RF_SBRTH;
    SBRTL = RF_SBRTL;
    SFINE = RF_SFINE;
    PCON |= _SSTAT; /* enable receive error flags */

    /*
     * HIGHEST PRIORITY FOR EUART0. This is the root fix.
     *
     * `tick.c` runs the FULL matrix scan and LED sub-frames INSIDE the Timer2
     * ISR. A scan takes ~320 us; at 260,870 baud a byte arrives every 38 us,
     * and the SH68F90 has no FIFO behind `SBUF`. At equal priority the serial
     * ISR cannot preempt: every matrix scan COSTS EIGHT BYTES.
     *
     * Measured on hardware. The module sends `02 00 01 00 00 52` and
     * `03 00 00 00 00 52` -- both with valid checksums -- and the received
     * stream only contained fragments: `02 00 00 52`, `02 00 52`,
     * `02 00 01 00`. No frame ever arrived intact, so none passed its checksum,
     * so `link_connected` never went high and the keyboard stayed silent.
     *
     * The factory firmware sets this exact bit: `0xECC5` writes `IPH1 = 0x42`
     * and `IPL1 = 0x41`, bit 6 in both registers, i.e. priority level 3.
     */
    IPH1 |= _ES0;
    IPL1 |= _ES0;
    SADDR = 0x00;
    SADEN = 0x00;

    TI = 0;
    RI = 0;

    tx_busy  = 0;
    tx_idx   = 0;
    tx_len   = 0;
    rx_idx     = 0;
    rx_pending = 0;
    rx_total   = 0;
    rx_last    = 0;

    /* Handshake idle: P0.2 as input, released high. */
    P0CR &= (uint8_t)~0x04;
    P0_2 = 1;
}

/*
 * Waits for the current burst to finish before disabling the IRQ: otherwise a
 * frame still in flight would leave tx_busy set forever and P0.2 stuck low as
 * an output, hanging the link. Bounded, because an absent module will never
 * respond.
 */
static void rf_tx_drain(void)
{
    /* 32 bytes at ~260 kbaud ~= 1.3 ms; ten milliseconds is plenty. */
    for (uint8_t i = 0; tx_busy && i < 10; i++) {
        delay_ms(1);
    }
    if (tx_busy) {
        tx_busy = 0;
        P0CR &= (uint8_t)~0x04;
        P0_2 = 1;
    }
}

/*
 * Radio arming, transcribed from factory thunk 0xEF40 -- four instructions,
 * called by `fcn.000084E9` at 0x8581 and 0x85D5:
 *
 *   ef40  ORL  IEN1,#0x40      IEN1 |= _ES0      -- EUART0 IRQ is armed
 *   ef43  ANL  USBCON,#0x7F    USBCON &= ~_ENUSB -- USB module is TURNED OFF
 *   ef46  SETB 0x3f            shared internal flag, irrelevant here
 *   ef48  MOV  R7,#0x14
 *   ef4a  LJMP 0xED03          ~20 ms
 *
 * The second instruction was missing in an initial draft of this file: in
 * wireless mode the factory firmware SHUTS DOWN the USB peripheral, rather than
 * merely ignoring the host.
 *
 * We call `usb_hw_deinit()` and NOT `usb_deinit()`. Both clear
 * `_ENUSB | _SW1CON | _SW2CON` and disarm the USB interrupt, but `usb_deinit()`
 * also resets the entire software state machine to zero -- including
 * `interface0_protocol`. Yet `host_nkro_active()` (`src/smk/host.c:12`) requires
 * `USB_PROTOCOL_REPORT`: zeroing it makes the keyboard fall back to 6KRO for
 * the entire duration of wireless mode, even though `NKRO_REPORT_BITS` is 20
 * specifically because it is "limited by wireless dongle hid descriptor"
 * (`report.h:10`).
 *
 * Shutting down only the hardware is therefore both more faithful to the
 * factory firmware -- which only executes `anl USBCON,#0x7F` -- and the only way
 * to keep NKRO working over the radio. Software state is rebuilt by
 * `usb_init()` when returning to wired mode.
 */
static void rf_radio_on(void)
{
    /*
     * Host LED state (Caps Lock, Num Lock, Scroll Lock) ONLY arrives through
     * `usb_ep0_out_irq()`. USB is about to be torn down right below: from that
     * point on we know nothing about what the host is doing, and leaving the
     * indicator lit on stale information would be worse than showing nothing.
     * On return to wired mode, the host resends its state during enumeration.
     */
    keyboard_set_led_state(0);

    IEN1 |= _ES0;
#ifndef RF_DEBUG_KEEP_USB
    usb_hw_deinit();
#endif
    delay_ms(20);
}

static void rf_radio_off(void)
{
    rf_tx_drain();
    IEN1 &= (uint8_t)~_ES0;
}

/*
 * Entering 2.4 GHz mode, transcribed from `fcn.000084E9` (branch of counter
 * 0x02DF). This answers the previously open question "what sets transport to 1":
 * nothing writes it directly; this branch of the selector does, and it notifies
 * the module via command 0x01 with slot 0.
 */
static void rf_probe_reset(void)
{
    const bool et2 = ET2;

    ET2                  = 0;
    probe_ticks          = 0;
    probe_due            = 0;
    followup_ticks       = 0;
    followup_due         = 0;
    ET2                  = et2;
    followup_left        = 0;
    followup_arm_pending = 0;
    lock_key_down        = 0;
    probe_misses         = 0;
    probe_answered       = 0;
}

static void rf_enter_24g(void)
{
    if (link_state == RF_LINK_WIRED) {
        delay_ms(20); /* 0xEF8D: guard delay before shutting down USB */
    }
    dprintf("rf enter 24g\r\n");
    link_state     = RF_LINK_24G;
    link_connected = 0;
    rf_probe_reset();
    rf_radio_on();
    rf_queue_link(RF_LINK_SELECT);
}

static void rf_enter_bt(void)
{
    if (link_state == RF_LINK_WIRED) {
        delay_ms(20);
    }
    dprintf("rf enter bt\r\n");
    link_state = RF_LINK_BT;
    if (bt_slot < RF_SLOT_BT1 || bt_slot > RF_SLOT_BT3) {
        bt_slot = RF_SLOT_BT1; /* factory clamp: 1..3 */
    }
    link_connected = 0;
    rf_probe_reset();
    rf_radio_on();
    rf_queue_link(RF_LINK_SELECT);
}

/*
 * Returning to wired mode, transcribed from `fcn.000084E9` @ 0x8543 and
 * `fcn.0000EED1`: notify the module (0x0E then 0x0B with 0), disable the
 * EUART0 IRQ, and reinitialize USB.
 */
static void rf_enter_wired(void)
{
    delay_ms(20);
    (void)rf_send_short(RF_CMD_WIRED, 0, 0, RF_GATE_CTRL);
    delay_ms(10);
    (void)rf_send_short(RF_CMD_SETTINGS, 0, 0, RF_GATE_CTRL);
    delay_ms(10);

    link_state      = RF_LINK_WIRED;
    link_connected  = 0;
    link_tx_pending = 0;
    rf_probe_reset();
    rf_radio_off();
#ifndef RF_DEBUG_KEEP_USB
    usb_init(); /* under KEEP_USB, USB was never shut down: restarting it would
                 * cut off the console at the worst possible moment. */
#endif
    /*
     * 200 ms blocking delay, just like the factory firmware. `delay_us` kicks
     * the watchdog on every microsecond, so the WDT will not bite -- but matrix
     * scanning and USB stop during that time. This is a mode switch, so it does
     * not happen while typing.
     */
    delay_ms(200);
}

/*
 * Three-position slide switch.
 *
 *   P7.4 = 0            -> 2.4 GHz
 *   P7.4 = 1, P4.5 = 1  -> wired
 *   P7.4 = 1, P4.5 = 0  -> Bluetooth
 *
 * Both pins come from `jb 0xF8.4` and `jb 0xB0.5` at the top of `fcn.000084E9`;
 * 0xF8 is P7 and 0xB0 is P4 according to `sh68f90.h`. Until now they were
 * listed among the port's "unknown role" inputs.
 */
static rf_link_t rf_read_selector(void)
{
    if (!P7_4) {
        return RF_LINK_24G;
    }
    return P4_5 ? RF_LINK_WIRED : RF_LINK_BT;
}

static void rf_sample_selector(void)
{
    if (P7_4) {
        if (P4_5) {
            deb_wired++;
            deb_24g = 0;
            deb_bt  = 0;
        } else {
            deb_bt    = deb_bt + 1;
            deb_wired = 0;
            deb_24g   = 0;
        }
    } else {
        deb_24g   = deb_24g + 1;
        deb_wired = 0;
        deb_bt    = 0;
    }

    if (deb_wired >= RF_DEBOUNCE) {
        deb_wired = 0;
        if (link_state != RF_LINK_WIRED) {
            rf_enter_wired();
        }
    } else if (deb_24g >= RF_DEBOUNCE) {
        deb_24g = 0;
        if (link_state != RF_LINK_24G) {
            rf_enter_24g();
        }
    } else if (deb_bt >= RF_DEBOUNCE) {
        deb_bt = 0;
        if (link_state != RF_LINK_BT) {
            rf_enter_bt();
        }
    }
}

/* ------------------------------------------------------------------ public */

void rf_init(void)
{
    link_state      = RF_LINK_WIRED;
    bt_slot         = RF_SLOT_BT1;
    name_stage        = 0;
    settings_restored = 0;
    link_connected  = 0;
    link_tx_pending = 0;
    rf_probe_reset();
    batt_frames      = 0;
    batt_target      = 0;
    batt_shown       = 0;
    batt_ticks       = 0;
    batt_low         = 0;
    batt_initialized = 0;
    rf_batt_pct      = 0;
    q_head          = 0;
    q_tail          = 0;
    q_count         = 0;
    deb_wired = deb_24g = deb_bt = 0;
    ctrl_stall = 0;

    rf_uart_init();
    IEN1 &= (uint8_t)~_ES0; /* armed only when a wireless mode is active */

    /*
     * The initial transport is READ FROM THE SELECTOR, not assumed to be wired.
     *
     * The factory firmware does otherwise: it starts from `g_transport = 0` and
     * lets the debounce in `fcn.000084E9` converge. This port does not
     * reproduce that behavior, because while converging `kb_send_report()`
     * would push keystrokes over USB even though the slide switch says
     * "wireless". The selector is the sole authority on the transport: we might
     * as well query it right away.
     *
     * The ordering in `main()` allows it: `rf_init()` is called by `kb_init()`,
     * i.e. after `usb_init()` and before `usb_wait_for_enumeration()`. Shutting
     * down USB here does not hang anything -- that wait returns after 500 ms
     * (`ENUM_NO_HOST_MS`) when no SETUP packet arrives.
     */
    switch (rf_read_selector()) {
        case RF_LINK_24G:
            rf_enter_24g();
            break;
        case RF_LINK_BT:
            rf_enter_bt();
            break;
        case RF_LINK_WIRED:
        default:
            break;
    }
}

void rf_tick(void)
{
    if (++probe_ticks >= RF_PROBE_TICKS) {
        probe_ticks = 0;
        probe_due   = 1;
    }
    if (followup_ticks != 0u) {
        if (--followup_ticks == 0u) {
            followup_due = 1;
        }
    }
}

/*
 * Presence probe: periodic command 0x06 every 100 ms, three failures and the
 * link is declared dead. Fast follow-up probes after key reports reuse the
 * same command without contributing to `probe_misses`.
 */
static void rf_probe_task(void)
{
    if (probe_due) {
        if (!rf_send_status_probe()) {
            return; /* retry on next pass once tx_busy clears */
        }
        probe_due    = 0;
        followup_due = 0;

        if (probe_answered) {
            probe_misses = 0;
        } else if (probe_misses < RF_PROBE_MISSES) {
            probe_misses++;
        }

        if (probe_misses >= RF_PROBE_MISSES) {
            dprintf("rf link lost (p47=%u txb=%u q=%u)\r\n", (unsigned)P4_7,
                    (unsigned)tx_busy, (unsigned)q_count);
            probe_misses   = 0;
            link_connected = 0;
            /* The factory firmware releases the transmit line here (0x3D5A). */
            rf_tx_drain();
            /*
             * And we discard whatever is lingering in the receive buffer: a
             * truncated frame whose type announces a length that will never arrive
             * would stall the buffer indefinitely, since `rf_rx_consume()` waits
             * for the full byte count.
             */
            rf_rx_drop();
        }

        probe_answered = 0;
        return;
    }

    if (followup_due) {
        if (!rf_send_status_probe()) {
            return;
        }
        followup_due = 0;
        if (followup_left != 0u) {
            followup_left--;
            if (followup_left != 0u) {
                const bool et2 = ET2;
                ET2            = 0;
                followup_ticks = RF_FOLLOWUP_TICKS;
                ET2            = et2;
            }
        }
    }
}

/*
 * `rf_init()` is called from `kb_init()`, hence BEFORE `settings_load()`: it
 * cannot know the persisted slot yet. That is the same ordering constraint that
 * gives the NuPhy Air60 a separate `restore_rf_link()` alongside `rf_init()`.
 * The first pass of `rf_task()`, however, runs in the main loop, so after
 * settings are loaded. It also runs BEFORE the command queue is flushed for the
 * first time, and `rf_link_slot()` only reads the slot when actually sending:
 * the already-queued 0x01 command will therefore go out with the right slot.
 */
static void rf_restore_settings(void)
{
    const uint8_t slot = AULA_SETTINGS_BT_SLOT;

    if (slot >= RF_SLOT_BT1 && slot <= RF_SLOT_BT3) {
        bt_slot = slot;
    } else {
        AULA_SETTINGS_BT_SLOT = RF_SLOT_BT1;
    }
}

/*
 * Traces transitions, not steady state: printing on every pass would flood the
 * console. Only STABLE fields trigger a print -- `q_count` and `probe_misses`
 * change constantly in wireless mode and are printed without being watched.
 *
 * Compiles to nothing outside debug builds (`debug.h`).
 */
#if DEBUG == 1
static __xdata uint8_t diag_prev[RF_DIAG_FIELDS];

static void rf_diag_trace(void)
{
    uint8_t i;
    bool    changed = false;

    for (i = 0; i < RF_DIAG_FIELDS; i++) {
        /* The report queue changes on every keystroke: watching it would flood
         * the console without teaching us anything about the link. */
        /* The receive counters have served their purpose -- they proved that
         * the module talks. Watching them now would flood the console and
         * prevent replaying captured frames, which is what matters. */
        if (i != RF_DIAG_QCOUNT && i != RF_DIAG_RXTOT && i != RF_DIAG_RXLAST &&
            i != RF_DIAG_RXIDX && diag_prev[i] != rf_diag_state[i]) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }
    /*
     * The console buffer is 128 bytes and `console_putc` SILENTLY DROPS bytes
     * when it overflows. The startup banner and `diag_task` dump saturate it:
     * without this wait, the very first transition -- the most interesting one,
     * showing which state the selector was read in at boot -- was thrown away.
     * `diag.c` paces itself the exact same way.
     *
     * `diag_prev` is only updated when actually printing: a transition can
     * therefore be delayed, never lost.
     */
    if (!console_is_drained()) {
        return;
    }
    for (i = 0; i < RF_DIAG_FIELDS; i++) {
        diag_prev[i] = rf_diag_state[i];
    }
    dprintf("rf 74=%u 45=%u 47=%u link=%u pend=%u name=%u txb=%u conn=%u q=%u miss=%u "
            "rx=%u last=%02x idx=%u\r\n",
            (unsigned)rf_diag_state[RF_DIAG_P74], (unsigned)rf_diag_state[RF_DIAG_P45],
            (unsigned)rf_diag_state[RF_DIAG_P47], (unsigned)rf_diag_state[RF_DIAG_LINK],
            (unsigned)rf_diag_state[RF_DIAG_TXPEND], (unsigned)rf_diag_state[RF_DIAG_NAME],
            (unsigned)rf_diag_state[RF_DIAG_TXBUSY], (unsigned)rf_diag_state[RF_DIAG_CONN],
            (unsigned)rf_diag_state[RF_DIAG_QCOUNT], (unsigned)rf_diag_state[RF_DIAG_MISSES],
            (unsigned)rf_diag_state[RF_DIAG_RXTOT], (unsigned)rf_diag_state[RF_DIAG_RXLAST],
            (unsigned)rf_diag_state[RF_DIAG_RXIDX]);
}
#else
#    define rf_diag_trace() ((void)0)
#endif

/*
 * Charger control and status, transcribed from stock `0x970E-0x97AA`:
 *   - `P3_4` (VBUS) debounced 3 samples -> `vbus_present`
 *   - `P7_6` (charger enable, active LOW): 0 when VBUS present, 1 on battery
 *   - `P7_7` (charger status, active LOW): 20 samples low = charging,
 *     200 samples high (or VBUS absent) = charge complete / not charging.
 */
static void rf_charge_poll(void)
{
    const bool vbus_raw = (link_state == RF_LINK_WIRED) || (P3_4 != 0);

    if (vbus_raw) {
        vbus_off_cnt = 0;
        if (vbus_on_cnt < 3) {
            vbus_on_cnt++;
        }
        if (vbus_on_cnt >= 3) {
            vbus_present = 1;
        }
    } else {
        vbus_on_cnt = 0;
        if (vbus_off_cnt < 3) {
            vbus_off_cnt++;
        }
        if (vbus_off_cnt >= 3) {
            vbus_present = 0;
        }
    }

    P7_6 = vbus_present ? 0 : 1;

    if (!vbus_present) {
        chg_on_cnt  = 0;
        chg_off_cnt = 0;
        rf_charging = 0;
        return;
    }

    if (!P7_7) {
        chg_off_cnt = 0;
        if (chg_on_cnt < 20) {
            chg_on_cnt++;
        }
        if (chg_on_cnt >= 20) {
            rf_charging = 1;
        }
    } else {
        chg_on_cnt = 0;
        if (chg_off_cnt < 200) {
            chg_off_cnt++;
        }
        if (chg_off_cnt >= 200) {
            rf_charging = 0;
        }
    }
}

void rf_task(void)
{
    if (!settings_restored) {
        settings_restored = 1;
        rf_restore_settings();
    }

    rf_rx_consume();
    rf_sample_selector();

    if (rf_is_wireless()) {
        /*
         * Link-select goes FIRST, and its pending flag is only cleared on
         * SUCCESS. That fixes two bugs at once:
         *
         *  - it used to sit BEHIND both Bluetooth names, held hostage to their
         *    completion. A single failure froze `name_stage`, and the only
         *    command that actually switches the module was never sent --
         *    whether the hardware was fine or not;
         *  - `link_tx_pending` used to be cleared BEFORE sending, and the
         *    return value was discarded: a rejected command was lost for good,
         *    even though queuing existed specifically to retry it.
         *
         * The factory firmware sends its names from `main`, outside any queue.
         */
        if (link_tx_pending) {
            if (rf_send_link(rf_link_slot(), link_tx_flag)) {
                link_tx_pending = 0;
                dprintf("rf link sent slot=%u flag=%u\r\n", (unsigned)rf_link_slot(),
                        (unsigned)link_tx_flag);
            }
        } else if (name_stage < 2) {
            if (rf_send_name(name_stage, (name_stage == 0) ? rf_name_bt3 : rf_name_bt5)) {
                name_stage++;
            }
        }
        rf_queue_flush();
        rf_probe_task();
    }

    /*
     * `paired` is not populated: NO field in the status frame carries pairing
     * state -- the concept appears nowhere in the disassembly notes, and
     * inferring it from `connected` would be a fabrication. `keyboard.c` leaves
     * it at zero.
     *
     * `battery_level` uses the 0-7 scale required by `keyboard.h`, not a
     * percentage: that is the contract expected by SMK's indicator readers.
     */
    if ((++chg_poll_div & 0x3Fu) == 0u) {
        rf_charge_poll();
    }
    rf_charging_active = rf_charging ? 1 : 0;
    rf_vbus_active     = vbus_present ? 1 : 0;
    if (link_state == RF_LINK_BT && !link_connected) {
        rf_bt_pair_slot = bt_slot;
        rf_bt_pair_fast = (rx_flag_last != 2) ? 1 : 0;
    } else {
        rf_bt_pair_slot = 0;
    }

    {
        uint8_t pct = batt_shown;
        if (rf_charging && pct >= 100u) {
            pct = 99u;
        } else if (vbus_present && !rf_charging && pct >= 95u) {
            pct = 100u;
        }
        rf_batt_pct = pct;
    }
    keyboard_state.rf_link       = (uint8_t)rf_link();
    keyboard_state.connected     = rf_connected() ? 1 : 0;
    if (rf_batt_pct != prev_batt_pct) {
        prev_batt_pct = rf_batt_pct;
        keyboard_state.battery_level = (uint8_t)(((uint16_t)rf_batt_pct * 7u + 50u) / 100u);
    }
    keyboard_state.low_power     = batt_low ? 1 : 0;

    /* The status LED tracks these four. All edge detection lives in
     * `aula_status.c`: here we only publish the state. */
    aula_status_poll(keyboard_state.rf_link, link_connected, batt_low, rf_charging);

    rf_diag_state[RF_DIAG_P74]    = P7_4 ? 1 : 0;
    rf_diag_state[RF_DIAG_P45]    = P4_5 ? 1 : 0;
    rf_diag_state[RF_DIAG_P47]    = P4_7 ? 1 : 0;
    rf_diag_state[RF_DIAG_LINK]   = (uint8_t)link_state;
    rf_diag_state[RF_DIAG_TXPEND] = link_tx_pending ? 1 : 0;
    rf_diag_state[RF_DIAG_NAME]   = name_stage;
    rf_diag_state[RF_DIAG_TXBUSY] = tx_busy ? 1 : 0;
    rf_diag_state[RF_DIAG_CONN]   = link_connected ? 1 : 0;
    rf_diag_state[RF_DIAG_QCOUNT] = q_count;
    rf_diag_state[RF_DIAG_MISSES] = probe_misses;
    rf_diag_state[RF_DIAG_RXTOT]  = rx_total;
    rf_diag_state[RF_DIAG_RXLAST] = rx_last;
    rf_diag_state[RF_DIAG_RXIDX]  = rx_idx;

    rf_diag_trace();
    rf_cap_replay();
}

rf_link_t rf_link(void)
{
    return link_state;
}

bool rf_is_wireless(void)
{
    return link_state != RF_LINK_WIRED;
}

bool rf_connected(void)
{
    return link_connected;
}

uint8_t rf_bt_slot(void)
{
    return bt_slot;
}

void rf_set_bt_slot(uint8_t slot)
{
    if (slot < RF_SLOT_BT1 || slot > RF_SLOT_BT3) {
        return;
    }
    bt_slot = slot;

    AULA_SETTINGS_BT_SLOT = slot;
    settings_mark_dirty();

    if (link_state == RF_LINK_BT) {
        link_connected = 0;
        rf_queue_link(RF_LINK_SELECT);
    }
}



/*
 * Sleep. The factory firmware disables EUART0 before going to sleep
 * (`fcn.00007D74` and `fcn.00009E39`, touching SFRs SCON and IEN1) and
 * **reconfigures the link from scratch on wake**: both wake-up paths, `0x7E85`
 * and `0x9EBF`, call the init routine `0xB1C2` again. That is exactly what
 * `rf_uart_init()` does.
 *
 * We also drain any ongoing transmission: a burst interrupted by entering sleep
 * would leave `tx_busy` set and `P0.2` stuck low as an output, holding the
 * module in request state throughout sleep.
 */
void rf_sleep_prepare(void)
{
    if (!rf_is_wireless()) {
        return;
    }
    rf_tx_drain();
    IEN1 &= (uint8_t)~_ES0;
    SCON = 0;
}

void rf_sleep_wake(void)
{
    if (!rf_is_wireless()) {
        return;
    }
    rf_uart_init();
    IEN1 |= _ES0;

    /*
     * Pre-sleep link state is no longer valid. We apply the factory
     * supervisor's rule -- "not connected, so re-request the transport" --
     * without waiting for the next probe, which is slow.
     */
    link_connected = 0;
    rf_probe_reset();
    rf_queue_link(RF_LINK_SELECT);
}

void rf_request_pairing(void)
{
    if (rf_is_wireless()) {
        rf_queue_link(RF_LINK_PAIRING);
        /* Looping pattern: holds until the link comes up, at which point the
         * rising edge of `connected` replaces it with the confirmation breath. */
        aula_status_event(AULA_STATUS_PAIRING);
    }
}
