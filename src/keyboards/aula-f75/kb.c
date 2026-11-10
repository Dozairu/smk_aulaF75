#include "kbdef.h"
#include "kb.h"
#include "keyboard.h"
#include "report.h"
#include "usb.h"
#include <stdint.h>
#include <stdbool.h>

#include "aula_encoder.h"
#include "aula_macro.h"
#ifdef RF_EUART0
#    include "aula_rf.h"
#endif

/*
 * Keyboard logic.
 *
 * Wireless goes through EUART0 (BK3632), not the bit-banged SPI of the Air60:
 * see `aula_rf.c`. The slide switch is sampled by `rf_task()`, which alone
 * decides the transport; here we only route reports.
 *
 * No OS-mode switch: its pin has not been identified. P0.0, P0.1, P0.5, P0.6,
 * P4.1, P4.4, and P7.7 remain unknown -- P7.4 and P4.5 are now identified as the
 * connection selector, and P4.7 as the serial link's "module ready" line.
 */

void kb_init(void)
{
#ifdef RF_EUART0
    rf_init();
#endif
}

void kb_update_switches(void)
{
}

extern void indicators_next_effect(void);
extern void indicators_next_color(void);
extern void indicators_toggle_direction(void);
extern void indicators_prev_effect(void);
extern void indicators_brightness_up(void);
extern void indicators_brightness_down(void);
extern void indicators_speed_up(void);
extern void indicators_speed_down(void);
#ifdef RF_EUART0
extern void indicators_toggle_rf_diag(void);
#endif

/*
 * The LNK_BT1..3 keycodes only select the Bluetooth slot: the transport itself
 * comes from the slide switch. Pressing the already-active slot restarts
 * pairing, like the factory shortcut that sets bit 0x2C.5.
 *
 * The six RGB keycodes were placed in the keymap from the start without
 * anything handling them: neither this file nor `layout_process_record`
 * recognized them, and they fell through to `send_keycode()` where no predicate
 * matched them. They now call the rendering engine.
 */
bool kb_process_record(uint16_t keycode, bool key_pressed)
{
    uint8_t slot;

    /* As long as auto-typing has not returned to idle, it swallows EVERYTHING:
     * this is what silences the key that stops it, and what prevents releasing
     * the activation combo from stopping it immediately. */
    if (aula_macro_intercept(key_pressed)) {
        return false;
    }

    if (keycode >= QK_USER && keycode < QK_USER + (KB_SAFE_RANGE - SAFE_RANGE)) {
        keycode = (uint16_t)(SAFE_RANGE + (keycode - QK_USER));
    }

    switch (keycode) {
        case FX_NEXT:
        case RGB_MODE_FORWARD:
            if (key_pressed) indicators_next_effect();
            return false;
        case FX_PREV:
        case RGB_MODE_REVERSE:
            if (key_pressed) indicators_prev_effect();
            return false;
        case BRI_UP:
        case RGB_VAI:
            if (key_pressed) indicators_brightness_up();
            return false;
        case BRI_DN:
        case RGB_VAD:
            if (key_pressed) indicators_brightness_down();
            return false;
        case SPD_UP:
        case RGB_SPI:
            if (key_pressed) indicators_speed_up();
            return false;
        case SPD_DN:
        case RGB_SPD:
            if (key_pressed) indicators_speed_down();
            return false;
        case CLR_NEXT:
        case RGB_HUI:
        case RGB_HUD:
            if (key_pressed) indicators_next_color();
            return false;
        case DIR_TOG:
        case RGB_TOG:
            if (key_pressed) indicators_toggle_direction();
            return false;
        case MACRO_TG:
            if (key_pressed) aula_macro_arm();
            return false;
#ifdef RF_EUART0
        case RF_DIAG:
            if (key_pressed) indicators_toggle_rf_diag();
            return false;
        case RF_PAIR:
            if (key_pressed) rf_request_pairing();
            return false;
#endif

        case LNK_BT1:
            slot = 1;
            break;
        case LNK_BT2:
            slot = 2;
            break;
        case LNK_BT3:
            slot = 3;
            break;
        default:
            return true;
    }

#ifdef RF_EUART0
    if (key_pressed) {
        if (rf_link() == RF_LINK_BT && rf_bt_slot() == slot) {
            rf_request_pairing();
        } else {
            rf_set_bt_slot(slot);
        }
    }
#else
    slot;
    key_pressed;
#endif
    return false;
}

void kb_update(void)
{
    aula_encoder_task();
    aula_macro_task();
#ifdef RF_EUART0
    rf_task();
#endif
}

void kb_send_report(__xdata report_keyboard_t *report)
{
#ifdef RF_EUART0
    if (rf_is_wireless()) {
        rf_send_report(report);
        return;
    }
#endif
    usb_send_report(report);
}

void kb_send_nkro(__xdata report_nkro_t *report)
{
#ifdef RF_EUART0
    if (rf_is_wireless()) {
        rf_send_nkro(report);
        return;
    }
#endif
    usb_send_nkro(report);
}

void kb_send_extra(__xdata report_extra_t *report)
{
#ifdef RF_EUART0
    if (rf_is_wireless()) {
        rf_send_extra(report);
        return;
    }
#endif
    usb_send_extra(report);
}
