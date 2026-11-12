#include "aula_encoder.h"
#include "sh68f90.h"
#include "host.h"
#include "report.h"
#include "keycodes.h"
#include "kb.h"
#include "layout.h"
#include "dynamic_keymap.h"
#ifdef VIA_MACRO_ENABLE
#    include "via_macro.h"
#endif
#include <stdint.h>
#include <stdbool.h>

/*
 * Quadrature decoder, transcribed from `fcn.00007928`.
 *
 *   state = (P0 >> 5) & 3           two bits: P0.5 and P0.6
 *   hist  = (state & 3) | (hist << 2) & 0x3C
 *   0x0B, 0x34 -> one direction     0x07, 0x38 -> the other
 *
 * Both pins are already configured as inputs with pull-ups by the stock
 * setup replicated verbatim in `user_init.c`: `P0CR = 0x9C` leaves bits 5
 * and 6 cleared, and `P0PCR = 0xF8` sets them to 1. Nothing to configure here.
 *
 * The stock firmware sends Consumer usages `0xE9` and `0xEA` -- Volume Up and
 * Volume Down. Here each direction sends the keycode stored in the VIA encoder map
 * (default: Volume Up / Down), looked up on the active layer. A persistent setting (block offset 26)
 * switches the knob to backlight brightness; its value in the dump is 0, so
 * volume. We follow the dump.
 */

/*
 * The host polls the endpoint every millisecond (`bInterval = 1`). A press
 * shorter than one poll interval would be overwritten by its own release and
 * would NEVER be seen.
 *
 * Counted in LED subframes (~420 us each): eight ticks held gives ~3.4 ms,
 * four ticks of silence ~1.7 ms.
 */
#define ENC_GAP  4
#define ENC_HOLD (ENC_GAP + 8)

extern uint8_t action_layer;

static volatile __data uint8_t enc_hold;
static volatile __data int8_t  enc_pending;
static volatile __bit          enc_release_due;

/* Keycode currently held on the host side, so it can be released. */
static __xdata uint16_t enc_sent;

/* In XDATA: only touched by the ISR, which already accesses XRAM for rendering. */
static __xdata uint8_t enc_state;
static __xdata uint8_t enc_hist;

void aula_encoder_sample(void)
{
    uint8_t now;

    if (enc_hold != 0) {
        enc_hold--;
        if (enc_hold == ENC_GAP) {
            enc_release_due = 1;
        }
    }

    now = (uint8_t)((P0 >> 5) & 0x03u);
    if (now == enc_state) {
        return;
    }
    enc_hist  = (uint8_t)((now & 0x03u) | (uint8_t)((enc_hist << 2) & 0x3Cu));
    enc_state = now;

    /* Detents are queued rather than dropped; the cap prevents a noisy scan
     * from filling the counter. */
    if (enc_hist == 0x0B || enc_hist == 0x34) {
        if (enc_pending < 8) enc_pending++;
    } else if (enc_hist == 0x07 || enc_hist == 0x38) {
        if (enc_pending > -8) enc_pending--;
    }
}

static void enc_send(uint16_t kc, bool down)
{
    uint8_t basic;
    uint8_t hi;

    if (kc == 0) {
        return;
    }
#ifdef VIA_MACRO_ENABLE
    if (IS_QK_MACRO(kc)) {
        if (down && (kc - QK_MACRO) < DYNAMIC_MACRO_COUNT) {
            via_macro_play((uint8_t)(kc - QK_MACRO));
        }
        return;
    }
#endif
    if (!kb_process_record(kc, down)) {
        return;
    }

    basic = (uint8_t)(kc & 0xFFu);
    hi    = (uint8_t)(kc >> 8);
    if (hi >= 0x01u && hi <= 0x1Fu) {
        uint8_t mods = (uint8_t)(hi & 0x0Fu);
        if (hi & 0x10u) {
            mods = (uint8_t)(mods << 4);
        }
        if (down) add_mods(mods); else del_mods(mods);
    } else if (hi != 0) {
        return;
    }

    if (basic != 0) {
        if (IS_MODIFIER_KEYCODE(basic)) {
            if (down) add_mods(MOD_BIT(basic)); else del_mods(MOD_BIT(basic));
        } else if (IS_BASIC_KEYCODE(basic)) {
            if (down) add_key(basic); else del_key(basic);
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

static uint16_t enc_lookup(bool clockwise)
{
    uint16_t kc = dynamic_encoder_get_keycode(action_layer, 0, clockwise);

    if (kc == KC_TRANSPARENT) {
        kc = dynamic_encoder_get_keycode(0, 0, clockwise);
    }
    return kc;
}

void aula_encoder_task(void)
{
    int8_t pending;

    if (enc_release_due) {
        enc_release_due = 0;
        enc_send(enc_sent, false);
        enc_sent = 0;
        return;
    }
    if (enc_hold != 0) {
        return;
    }

    /* `enc_pending` is incremented by the ISR and decremented here: the
     * read-modify-write must be protected. */
    ET2     = 0;
    pending = enc_pending;
    if (pending > 0) {
        enc_pending--;
    } else if (pending < 0) {
        enc_pending++;
    }
    ET2 = 1;

    if (pending != 0) {
        enc_sent = enc_lookup(pending > 0);
        enc_send(enc_sent, true);
        enc_hold = ENC_HOLD;
    }
}