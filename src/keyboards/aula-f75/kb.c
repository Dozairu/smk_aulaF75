#include "kbdef.h"
#include "kb.h"
#include "keyboard.h"
#include "matrix.h"
#include "report.h"
#include "settings.h"
#include "tick.h"
#include "usb.h"
#include <stdint.h>
#include <stdbool.h>

#include "aula_encoder.h"
#include "aula_macro.h"
#include "aula_status.h"
#include "indicators.h"
#include "vial.h"
#include "dynamic_keymap.h"
#include "vialrgb.h"
#include "action.h"
#ifdef RF_EUART0
#    include "aula_rf.h"
#endif
#ifdef VIA_ENABLE
#    include "dynamic_keymap.h"
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

void kb_update_switches(void) {}

extern void indicators_init(void);
extern void indicators_apply_defaults(void);
extern void indicators_confirm(uint8_t row, uint8_t col); /* row 0xFF = whole board */
extern void indicators_next_effect(void);
extern void indicators_next_color(void);
extern void indicators_toggle_direction(void);
extern void indicators_prev_effect(void);
extern void indicators_brightness_up(void);
extern void indicators_brightness_down(void);
extern void indicators_speed_up(void);
extern void indicators_speed_down(void);

/* QMK's RGB Matrix keycodes (QK_RGB_MATRIX_*), as Vial's Lighting keycodes. */
#define RM_ON   0x7840u
#define RM_OFF  0x7841u
#define RM_TOGG 0x7842u
#define RM_NEXT 0x7843u
#define RM_PREV 0x7844u
#define RM_HUEU 0x7845u
#define RM_HUED 0x7846u
#define RM_SATU 0x7847u
#define RM_SATD 0x7848u
#define RM_VALU 0x7849u
#define RM_VALD 0x784Au
#define RM_SPDU 0x784Bu
#define RM_SPDD 0x784Cu

#define FX_OFF_ID  14u /* AULA_FX_OFF (indicators.c) */
#define FX_LAKE_ID 9u

/* Lighting on / off (0 off, 1 on, 2 toggle): off remembers the effect. */
static __xdata uint8_t rgb_saved = FX_LAKE_ID;

static void rgb_power(uint8_t how)
{
#ifdef SIGNALRGB_ENABLE
    indicators_direct_clear();
#endif
    const uint8_t on = user_settings.led_effect != FX_OFF_ID;
    if (how == 2u) {
        how = on ? 0u : 1u;
    }
    if (how == 0u && on) {
        rgb_saved                = user_settings.led_effect;
        user_settings.led_effect = FX_OFF_ID;
    } else if (how == 1u && !on) {
        user_settings.led_effect = rgb_saved;
    } else {
        return;
    }
    tick_pause();
    indicators_init();
    tick_resume();
    settings_mark_dirty();
}
#ifdef RF_EUART0
extern void indicators_toggle_rf_diag(void);
extern void indicators_show_battery(bool held);
#endif

static __bit            win_lock;
static __bit            wasd_swap;
static __xdata uint8_t  os_mode; /* 0=Win, 1=Android, 2=Mac, 3=iOS */
static __bit            factory_rst_held;
static __xdata uint16_t factory_rst_start;

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
#ifndef NO_AUTOTYPE
    if (aula_macro_intercept(key_pressed)) {
        return false;
    }
#endif

    if (keycode >= QK_USER && keycode < QK_USER + (KB_SAFE_RANGE - SAFE_RANGE)) {
        keycode = (uint16_t)(SAFE_RANGE + (keycode - QK_USER));
    }

    switch (keycode) {
        case FX_NEXT:
        case RGB_MODE_FORWARD:
        case RM_NEXT:
            if (key_pressed) indicators_next_effect();
            return false;
        case FX_PREV:
        case RGB_MODE_REVERSE:
        case RM_PREV:
            if (key_pressed) indicators_prev_effect();
            return false;
        case BRI_UP:
        case RGB_VAI:
        case RM_VALU:
            if (key_pressed) indicators_brightness_up();
            return false;
        case BRI_DN:
        case RGB_VAD:
        case RM_VALD:
            if (key_pressed) indicators_brightness_down();
            return false;
        case SPD_UP:
        case RGB_SPI:
        case RM_SPDU:
            if (key_pressed) indicators_speed_up();
            return false;
        case SPD_DN:
        case RGB_SPD:
        case RM_SPDD:
            if (key_pressed) indicators_speed_down();
            return false;
        case CLR_NEXT:
            if (key_pressed) indicators_next_color();
            return false;
#ifdef VIALRGB_ENABLE
        /* Hue / saturation: steps of the VialRGB picker color (QMK's steps). */
        case RGB_HUI:
        case RM_HUEU:
            if (key_pressed) vialrgb_step(0, 8);
            return false;
        case RGB_HUD:
        case RM_HUED:
            if (key_pressed) vialrgb_step(0, -8);
            return false;
        case RGB_SAI:
        case RM_SATU:
            if (key_pressed) vialrgb_step(1, 17);
            return false;
        case RGB_SAD:
        case RM_SATD:
            if (key_pressed) vialrgb_step(1, -17);
            return false;
#else
        case RGB_HUI:
        case RGB_HUD:
            if (key_pressed) indicators_next_color();
            return false;
#endif
        case RGB_TOG:
        case RM_TOGG:
            if (key_pressed) rgb_power(2);
            return false;
        case RM_ON:
            if (key_pressed) rgb_power(1);
            return false;
        case RM_OFF:
            if (key_pressed) rgb_power(0);
            return false;
        case QK_BOOT:
#ifdef ISP_ENABLE
            if (key_pressed) usb_request_isp(); /* flashing mode, as sinowisp's command */
#endif
            return false;
        case QK_CLEAR_EEPROM:
            if (key_pressed) { /* factory reset, as holding Fn+Esc: next kb_update */
                factory_rst_held  = 1;
                factory_rst_start = (uint16_t)(timer_read() - 3000u);
            }
            return false;
        case DIR_TOG:
            if (key_pressed) indicators_toggle_direction();
            return false;
        case IND_STATUS:
        case IND_CAPS:
        case IND_CHARGE:
            /* The VIA menu's three indicator toggles, as keys (Vial has no
             * menu): status LED, Caps Lock LED, charging / low battery. */
            if (key_pressed) {
                uint8_t *t = (keycode == IND_STATUS) ? &user_settings.battery_indicator_on : (keycode == IND_CAPS) ? &user_settings.led_color : &user_settings.ul_speed;
                *t         = *t ? 0u : 1u;
                settings_mark_dirty();
                indicators_confirm(matrix_event_row, matrix_event_col);
            }
            return false;
        case MACRO_TG:
#ifndef NO_AUTOTYPE
            if (key_pressed) aula_macro_arm();
#endif
            return false;
#ifdef RF_EUART0
        case RF_DIAG:
#    ifndef NO_RF_DIAG
            if (key_pressed) indicators_toggle_rf_diag();
#    endif
            return false;
        case RF_PAIR:
            if (key_pressed) rf_request_pairing();
            return false;
        case BAT_SHOW:
            indicators_show_battery(key_pressed); /* shown while held */
            return false;
#else
        case BAT_SHOW:
            return false;
#endif
        case WIN_LOCK:
            if (key_pressed) {
                win_lock = !win_lock;
                if (win_lock) {
                    send_keycode(KC_LGUI, false);
                    send_keycode(KC_RGUI, false);
                }
            }
            return false;
        case WASD_SWAP:
            if (key_pressed) {
                wasd_swap = !wasd_swap;
            }
            return false;
        case OS_WIN:
            if (key_pressed) {
                os_mode = 0;
                aula_status_event(AULA_STATUS_LINKED);
                indicators_confirm(matrix_event_row, matrix_event_col);
            }
            return false;
        case OS_ANDROID:
            if (key_pressed) {
                os_mode = 1;
                aula_status_event(AULA_STATUS_LINKED);
                indicators_confirm(matrix_event_row, matrix_event_col);
            }
            return false;
        case OS_MAC:
            if (key_pressed) {
                os_mode = 2;
                aula_status_event(AULA_STATUS_LINKED);
                indicators_confirm(matrix_event_row, matrix_event_col);
            }
            return false;
        case OS_IOS:
            if (key_pressed) {
                os_mode = 3;
                aula_status_event(AULA_STATUS_LINKED);
                indicators_confirm(matrix_event_row, matrix_event_col);
            }
            return false;
        case FACTORY_RST:
            if (key_pressed) {
                if (!factory_rst_held) {
                    factory_rst_held  = 1;
                    factory_rst_start = timer_read();
                }
            } else {
                factory_rst_held = 0;
            }
            return false;

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
            if (win_lock && (keycode == KC_LGUI || keycode == KC_RGUI)) {
                return false;
            }
            if (wasd_swap) {
                uint16_t mapped = 0;
                switch (keycode) {
                    case KC_W:
                        mapped = KC_UP;
                        break;
                    case KC_A:
                        mapped = KC_LEFT;
                        break;
                    case KC_S:
                        mapped = KC_DOWN;
                        break;
                    case KC_D:
                        mapped = KC_RGHT;
                        break;
                    case KC_UP:
                        mapped = KC_W;
                        break;
                    case KC_LEFT:
                        mapped = KC_A;
                        break;
                    case KC_DOWN:
                        mapped = KC_S;
                        break;
                    case KC_RGHT:
                        mapped = KC_D;
                        break;
                    default:
                        break;
                }
                if (mapped != 0) {
                    send_keycode(mapped, key_pressed);
                    return false;
                }
            }
            if (os_mode >= 2) {
                uint16_t mapped = 0;
                switch (keycode) {
                    case KC_LALT:
                        mapped = KC_LGUI;
                        break;
                    case KC_LGUI:
                        mapped = KC_LALT;
                        break;
                    case KC_RALT:
                        mapped = KC_RGUI;
                        break;
                    case KC_RGUI:
                        mapped = KC_RALT;
                        break;
                    default:
                        break;
                }
                if (mapped != 0) {
                    send_keycode(mapped, key_pressed);
                    return false;
                }
            }
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
    if (factory_rst_held && (uint16_t)(timer_read() - factory_rst_start) >= 3000u) {
        factory_rst_held = 0;
        win_lock         = 0;
        wasd_swap        = 0;
        os_mode          = 0;
        indicators_apply_defaults();
        action_init(); /* key engine: no locked layer, Caps Word, one-shot... */
        clear_keys();  /* and whatever it had down, so nothing stays stuck */
        set_mods(0);
        send_keyboard_report();
#ifdef VIAL_ENABLE
        vial_store_defaults(); /* QMK settings back to defaults too */
        vial_store_apply();
#endif
#ifdef VIA_ENABLE
        dynamic_keymap_reset();
#endif
#ifdef VIA_MACRO_ENABLE
        dynamic_macro_reset();
#endif
        tick_pause();
        indicators_init();
        tick_resume();
        settings_mark_dirty();
        aula_status_event(AULA_STATUS_LOST);
        indicators_confirm(0xFFu, 0);
    }
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
