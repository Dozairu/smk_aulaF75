#include "via_macro.h"

#ifdef VIA_MACRO_ENABLE

#    include "dynamic_keymap.h"
#    include "host.h"
#    include "keycodes.h"
#    include "layout.h"
#    include "report.h"
#    include "sleep.h"

/*
 * VIA macro player.
 *
 * Buffer format (same as QMK's dynamic keymap macros): DYNAMIC_MACRO_COUNT
 * strings back to back, each terminated by NUL. Inside a string:
 *   - printable ASCII, \n, \t, \b           typed as-is (US layout, Shift as needed)
 *   - 0x01 0x01 <kc8>                       tap basic keycode
 *   - 0x01 0x02 <kc8>                       press (hold) basic keycode
 *   - 0x01 0x03 <kc8>                       release basic keycode
 *   - 0x01 0x04 <digits> '|'                delay in milliseconds
 *   - 0x01 0x05/0x06/0x07 <lo> <hi>         tap/press/release with a 16-bit keycode
 *                                           (VIA protocol >= 12), incl. Shift+x style combos
 *
 * The player is a tiny cooperative state machine: `via_macro_task()` runs from
 * the main loop and does one action per call, then arms `macro_wait` (counted
 * down by `via_macro_tick()` from the tick ISR) before the next one.
 */

#    define SS_QMK_PREFIX 0x01u
#    define SS_TAP_CODE   0x01u
#    define SS_DOWN_CODE  0x02u
#    define SS_UP_CODE    0x03u
#    define SS_DELAY_CODE 0x04u
#    define SS_TAP_CODE16  0x05u
#    define SS_DOWN_CODE16 0x06u
#    define SS_UP_CODE16   0x07u

/* One tick slot is ~400 us (2.5 per millisecond); 12/5 slightly under-counts, so delays are never short. */
#    define MACRO_MS(ms) ((uint16_t)(((uint32_t)(ms) * 12ul) / 5ul))

/* A press shorter than one USB poll (1 ms) would never be seen by the host. */
#    define MACRO_HOLD_TICKS 8u

#    define SHIFT_FLAG 0x0200u /* QK_LSFT */

static volatile __xdata uint16_t macro_wait;
static volatile bool             macro_due;
static __xdata uint16_t          macro_pos;
static __xdata uint16_t          macro_pending;     /* 16-bit keycode to release after a tap */
static bool                      macro_running;
static bool                      macro_release_due; /* a tap is in flight, release comes next */

/* US layout ASCII -> (keycode | 0x80 when Shift is needed), for 0x20..0x7E. */
static const __code uint8_t ascii_lut[95] = {
    /* 20 */ 0x2C, 0x9E, 0xB4, 0xA0, 0xA1, 0xA2, 0xA4, 0x34, 0xA6, 0xA7, 0xA5, 0xAE, 0x36, 0x2D, 0x37, 0x38,
    /* 30 */ 0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0xB3, 0x33, 0xB6, 0x2E, 0xB7, 0xB8,
    /* 40 */ 0x9F, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92,
    /* 50 */ 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x2F, 0x31, 0x30, 0xA3, 0xAD,
    /* 60 */ 0x35, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    /* 70 */ 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0xAF, 0xB1, 0xB0, 0xB5,
};

void via_macro_tick(void)
{
    if (macro_wait != 0) {
        macro_wait--;
        if (macro_wait == 0) {
            macro_due = true;
        }
    }
}

static void macro_arm(uint16_t ticks)
{
    __critical
    {
        if (ticks == 0) {
            macro_wait = 0;
            macro_due  = true;
        } else {
            macro_wait = ticks;
            macro_due  = false;
        }
    }
}

/* Presses or releases a 16-bit keycode: optional modifiers in the high byte, then a basic/system/consumer keycode. */
static void macro_apply(uint16_t kc, bool down)
{
    const uint8_t basic = (uint8_t)(kc & 0xFFu);
    const uint8_t hi    = (uint8_t)(kc >> 8);

    if (hi >= 0x01u && hi <= 0x1Fu) {
        uint8_t mods = (uint8_t)(hi & 0x0Fu);
        if (hi & 0x10u) {
            mods = (uint8_t)(mods << 4);
        }
        if (down) {
            add_mods(mods);
        } else {
            del_mods(mods);
        }
    } else if (hi != 0) {
        return; /* quantum keycodes (layers, RGB, ...) are not playable from a macro */
    }

    if (basic != 0) {
        if (IS_MODIFIER_KEYCODE(basic)) {
            if (down) {
                add_mods(MOD_BIT(basic));
            } else {
                del_mods(MOD_BIT(basic));
            }
        } else if (IS_BASIC_KEYCODE(basic)) {
            if (down) {
                add_key(basic);
            } else {
                del_key(basic);
            }
        } else if (IS_SYSTEM_KEYCODE(basic)) {
            host_system_send(down ? keycode_to_system(basic) : 0);
            return;
        } else if (IS_CONSUMER_KEYCODE(basic)) {
            host_consumer_send(down ? keycode_to_consumer(basic) : 0);
            return;
        }
    }
    send_keyboard_report();
}

static void macro_tap(uint16_t kc)
{
    macro_apply(kc, true);
    macro_pending     = kc;
    macro_release_due = true;
    macro_arm(MACRO_HOLD_TICKS);
}

static void macro_finish(void)
{
    macro_running     = false;
    macro_release_due = false;
}

void via_macro_play(uint8_t index)
{
    uint16_t pos = 0;
    uint8_t  skip = index;

    if (macro_running || index >= DYNAMIC_MACRO_COUNT) {
        return;
    }

    /* Find the start of the requested string: skip `index` NUL terminators. */
    while (skip != 0) {
        if (pos >= DYNAMIC_MACRO_BYTES) {
            return;
        }
        if (dynamic_macro_byte(pos++) == 0) {
            skip--;
        }
    }
    if (pos >= DYNAMIC_MACRO_BYTES || dynamic_macro_byte(pos) == 0) {
        return; /* empty macro */
    }

    macro_pos         = pos;
    macro_release_due = false;
    macro_running     = true;
    macro_arm(0);
}

void via_macro_task(void)
{
    uint8_t c;
    uint8_t action;

    if (!macro_running || !macro_due) {
        return;
    }

    sleep_note_activity();

    if (macro_release_due) {
        macro_release_due = false;
        macro_apply(macro_pending, false);
        macro_arm(MACRO_HOLD_TICKS);
        return;
    }

    c = dynamic_macro_byte(macro_pos++);
    if (c == 0) {
        macro_finish();
        return;
    }

    if (c != SS_QMK_PREFIX) {
        if (c >= 0x20u && c <= 0x7Eu) {
            const uint8_t e = ascii_lut[c - 0x20u];
            macro_tap((uint16_t)((e & 0x80u) ? SHIFT_FLAG : 0u) | (uint16_t)(e & 0x7Fu));
        } else if (c == '\n') {
            macro_tap(KC_ENTER);
        } else if (c == '\t') {
            macro_tap(KC_TAB);
        } else if (c == '\b') {
            macro_tap(KC_BACKSPACE);
        }
        return;
    }

    action = dynamic_macro_byte(macro_pos++);
    switch (action) {
        case SS_TAP_CODE:
        case SS_DOWN_CODE:
        case SS_UP_CODE: {
            const uint8_t kc = dynamic_macro_byte(macro_pos++);
            if (kc == 0) {
                macro_finish();
            } else if (action == SS_TAP_CODE) {
                macro_tap(kc);
            } else {
                macro_apply(kc, action == SS_DOWN_CODE);
                macro_arm(MACRO_HOLD_TICKS);
            }
            break;
        }

        case SS_TAP_CODE16:
        case SS_DOWN_CODE16:
        case SS_UP_CODE16: {
            const uint8_t  lo = dynamic_macro_byte(macro_pos++);
            const uint8_t  hi = dynamic_macro_byte(macro_pos++);
            const uint16_t kc = (uint16_t)(((uint16_t)hi << 8) | lo);
            if (action == SS_TAP_CODE16) {
                macro_tap(kc);
            } else {
                macro_apply(kc, action == SS_DOWN_CODE16);
                macro_arm(MACRO_HOLD_TICKS);
            }
            break;
        }

        case SS_DELAY_CODE: {
            uint32_t ms = 0;
            for (;;) {
                const uint8_t d = dynamic_macro_byte(macro_pos++);
                if (d < '0' || d > '9') {
                    break; /* '|' terminator (or end of buffer) */
                }
                ms = ms * 10u + (uint8_t)(d - '0');
                if (ms > 10000ul) {
                    ms = 10000ul;
                }
            }
            macro_arm(MACRO_MS(ms));
            break;
        }

        default:
            macro_finish(); /* unknown action: stop rather than type garbage */
            break;
    }
}

#endif
