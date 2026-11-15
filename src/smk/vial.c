#include "vial.h"

#ifdef VIAL_ENABLE

#    include "dynamic_keymap.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "raw_hid.h"
#    include <string.h>

#    include VIAL_DEFINITION_HEADER

/*
 * Vial protocol, version 6 -- the subset that needs nothing beyond what SMK
 * already stores: identity, the compressed definition, the encoder map, and
 * the unlock handshake. Wire format follows vial-qmk quantum/vial.c: the
 * request buffer is edited in place and sent back, integers are little-endian
 * except keycodes, which are big-endian like the rest of VIA.
 *
 * Not yet backed by firmware features, so reported as empty:
 *   - QMK settings (0x09 query answers "none"),
 *   - dynamic entries (0x0D: zero tap dances / combos / key overrides).
 * Vial GUI hides the corresponding tabs.
 */

#    define VIAL_GET_KEYBOARD_ID       0x00u
#    define VIAL_GET_SIZE              0x01u
#    define VIAL_GET_DEFINITION        0x02u
#    define VIAL_GET_ENCODER           0x03u
#    define VIAL_SET_ENCODER           0x04u
#    define VIAL_GET_UNLOCK_STATUS     0x05u
#    define VIAL_UNLOCK_START          0x06u
#    define VIAL_UNLOCK_POLL           0x07u
#    define VIAL_LOCK                  0x08u
#    define VIAL_QMK_SETTINGS_QUERY    0x09u
#    define VIAL_DYNAMIC_ENTRY_OP      0x0Du

#    define DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES 0x00u

/* VIAL_UNLOCK_ROWS / _COLS come from the generated header (board options
 * vial_unlock_rows / vial_unlock_cols); the generator refuses an empty list. */
static const __code uint8_t unlock_rows[] = VIAL_UNLOCK_ROWS;
static const __code uint8_t unlock_cols[] = VIAL_UNLOCK_COLS;

#    define UNLOCK_KEYS ((uint8_t)sizeof(unlock_rows))

_Static_assert(sizeof(unlock_rows) == sizeof(unlock_cols), "vial unlock rows/cols must pair up");
_Static_assert(sizeof(unlock_rows) < 15u, "vial unlock status reports at most 14 keys");

static const __code uint8_t keyboard_uid[8] = VIAL_KEYBOARD_UID;

/* vial-qmk: 50 steps of 100 ms of continuous hold, ~5 s. */
#    define UNLOCK_COUNTER_MAX 50u
/* 100 ms in tick-ISR calls (~400 us each, 2.5 per ms). */
#    define UNLOCK_STEP_TICKS 250u

static __bit                     unlocked;
static __bit                     unlock_busy;
static __xdata uint8_t           unlock_counter;
static __xdata uint16_t          unlock_mark;
static volatile __xdata uint16_t ticks;

void vial_tick(void)
{
    ticks++;
}

static uint16_t ticks_now(void)
{
    uint16_t t;
    __critical
    {
        t = ticks;
    }
    return t;
}

bool vial_unlocked(void)
{
    return unlocked;
}

bool vial_unlock_in_progress(void)
{
    return unlock_busy;
}

bool vial_allowed_during_unlock(const __xdata uint8_t *data)
{
    if (data[0] != VIAL_PREFIX) {
        return false;
    }
    switch (data[1]) {
        case VIAL_GET_KEYBOARD_ID:
        case VIAL_GET_SIZE:
        case VIAL_GET_DEFINITION:
        case VIAL_GET_UNLOCK_STATUS:
        case VIAL_UNLOCK_START:
        case VIAL_UNLOCK_POLL:
            return true;
        default:
            return false;
    }
}

static bool unlock_keys_held(void)
{
    for (uint8_t i = 0; i < UNLOCK_KEYS; i++) {
        if (!(matrix_get_col(unlock_cols[i]) & (uint8_t)(1u << unlock_rows[i]))) {
            return false;
        }
    }
    return true;
}

static void unlock_poll(void)
{
    if (!unlock_busy) {
        return;
    }
    if (!unlock_keys_held()) {
        unlock_counter = UNLOCK_COUNTER_MAX;
        return;
    }
    const uint16_t now = ticks_now();
    if ((uint16_t)(now - unlock_mark) > UNLOCK_STEP_TICKS) {
        unlock_mark = now;
        if (--unlock_counter == 0) {
            unlock_busy = 0;
            unlocked    = 1;
        }
    }
}

void vial_process(__xdata uint8_t *data)
{
    switch (data[1]) {
        case VIAL_GET_KEYBOARD_ID:
            memset(data, 0, RAW_HID_REPORT_SIZE);
            data[0] = VIAL_PROTOCOL_VERSION; /* u32 LE: 6, 0, 0, 0 */
            for (uint8_t i = 0; i < 8u; i++) {
                data[4u + i] = keyboard_uid[i];
            }
            data[12] = 0; /* no VialRGB (yet) */
            break;

        case VIAL_GET_SIZE:
            data[0] = (uint8_t)VIAL_DEFINITION_SIZE;
            data[1] = (uint8_t)(VIAL_DEFINITION_SIZE >> 8);
            data[2] = 0;
            data[3] = 0;
            break;

        case VIAL_GET_DEFINITION: {
            /* Page index, u16 LE (the GUI sends a u32; the high half is 0). */
            const uint16_t page  = (uint16_t)data[2] | (uint16_t)((uint16_t)data[3] << 8);
            const uint16_t start = (uint16_t)(page * RAW_HID_REPORT_SIZE);
            if (page >= (uint16_t)((VIAL_DEFINITION_SIZE + RAW_HID_REPORT_SIZE - 1u) / RAW_HID_REPORT_SIZE)) {
                break; /* past the end: echo, as vial-qmk does */
            }
            uint16_t n = (uint16_t)(VIAL_DEFINITION_SIZE - start);
            if (n > RAW_HID_REPORT_SIZE) {
                n = RAW_HID_REPORT_SIZE;
            }
            for (uint8_t i = 0; i < (uint8_t)n; i++) {
                data[i] = vial_definition[start + i];
            }
            break;
        }

        case VIAL_GET_ENCODER: {
            /* Direction 0 = counter-clockwise, 1 = clockwise, like QMK's encoder map. */
            const uint16_t ccw = dynamic_encoder_get_keycode(data[2], data[3], false);
            const uint16_t cw  = dynamic_encoder_get_keycode(data[2], data[3], true);
            data[0] = (uint8_t)(ccw >> 8);
            data[1] = (uint8_t)ccw;
            data[2] = (uint8_t)(cw >> 8);
            data[3] = (uint8_t)cw;
            break;
        }

        case VIAL_SET_ENCODER:
            dynamic_encoder_set_keycode(data[2], data[3], data[4] != 0,
                                        (uint16_t)((uint16_t)data[5] << 8) | data[6]);
            break;

        case VIAL_GET_UNLOCK_STATUS:
            memset(data, 0xFF, RAW_HID_REPORT_SIZE);
            data[0] = unlocked ? 1 : 0;
            data[1] = unlock_busy ? 1 : 0;
            for (uint8_t i = 0; i < UNLOCK_KEYS; i++) {
                data[2u + 2u * i] = unlock_rows[i];
                data[3u + 2u * i] = unlock_cols[i];
            }
            break;

        case VIAL_UNLOCK_START:
            unlock_busy    = 1;
            unlock_counter = UNLOCK_COUNTER_MAX;
            unlock_mark    = ticks_now();
            break;

        case VIAL_UNLOCK_POLL:
            unlock_poll();
            data[0] = unlocked ? 1 : 0;
            data[1] = unlock_busy ? 1 : 0;
            data[2] = unlock_counter;
            break;

        case VIAL_LOCK:
            unlocked = 0;
            break;

        case VIAL_QMK_SETTINGS_QUERY:
            /* No settings: 0xFFFF in the first slot ends the GUI's query loop. */
            memset(data, 0xFF, RAW_HID_REPORT_SIZE);
            break;

        case VIAL_DYNAMIC_ENTRY_OP:
            if (data[2] == DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES) {
                /* [0] tap dance, [1] combo, [2] key override, [3] alt repeat,
                 * [31] feature bits (Caps Word, Layer Lock): all zero. */
                memset(data, 0, RAW_HID_REPORT_SIZE);
            }
            break;

        default:
            break; /* unknown: echo */
    }
}

#endif
