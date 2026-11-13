#include "via.h"

#ifdef VIA_ENABLE

#    include "dynamic_keymap.h"
#    include "indicators.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "raw_hid.h"
#    include "settings.h"
#    include "tick.h"
#    include <string.h>

#    define VIA_ID_GET_PROTOCOL_VERSION           0x01u
#    define VIA_ID_GET_KEYBOARD_VALUE             0x02u
#    define VIA_ID_SET_KEYBOARD_VALUE             0x03u
#    define VIA_ID_DYNAMIC_KEYMAP_GET_KEYCODE     0x04u
#    define VIA_ID_DYNAMIC_KEYMAP_SET_KEYCODE     0x05u
#    define VIA_ID_DYNAMIC_KEYMAP_RESET           0x06u
#    define VIA_ID_CUSTOM_SET_VALUE               0x07u
#    define VIA_ID_CUSTOM_GET_VALUE               0x08u
#    define VIA_ID_CUSTOM_SAVE                    0x09u
#    define VIA_ID_EEPROM_RESET                   0x0Au
#    define VIA_ID_BOOTLOADER_JUMP                0x0Bu
#    define VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_COUNT 0x0Cu
#    define VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE 0x0Du
#    define VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_BUFFER 0x0Eu
#    define VIA_ID_DYNAMIC_KEYMAP_MACRO_SET_BUFFER 0x0Fu
#    define VIA_ID_DYNAMIC_KEYMAP_MACRO_RESET     0x10u
#    define VIA_ID_DYNAMIC_KEYMAP_GET_LAYER_COUNT 0x11u
#    define VIA_ID_DYNAMIC_KEYMAP_GET_ENCODER     0x14u
#    define VIA_ID_DYNAMIC_KEYMAP_SET_ENCODER     0x15u
#    define VIA_ID_DYNAMIC_KEYMAP_GET_BUFFER      0x12u
#    define VIA_ID_DYNAMIC_KEYMAP_SET_BUFFER      0x13u
#    define VIA_ID_UNHANDLED                      0xFFu

#    define VIA_KEYBOARD_VALUE_UPTIME             0x01u
#    define VIA_KEYBOARD_VALUE_LAYOUT_OPTIONS     0x02u
#    define VIA_KEYBOARD_VALUE_SWITCH_MATRIX_STATE 0x03u
#    define VIA_KEYBOARD_VALUE_FIRMWARE_VERSION   0x04u
#    define VIA_KEYBOARD_VALUE_DEVICE_INDICATION  0x05u
#    define VIA_KEYBOARD_VALUE_KEYCODES_VERSION   0x06u

void via_process_packet(__xdata uint8_t *data)
{
    switch (data[0]) {
        case VIA_ID_GET_PROTOCOL_VERSION:
            data[1] = (uint8_t)(VIA_PROTOCOL_VERSION >> 8);
            data[2] = (uint8_t)VIA_PROTOCOL_VERSION;
            break;

        case VIA_ID_GET_KEYBOARD_VALUE:
            switch (data[1]) {
                case VIA_KEYBOARD_VALUE_LAYOUT_OPTIONS:
                case VIA_KEYBOARD_VALUE_FIRMWARE_VERSION:
                    memset(&data[2], 0, 4);
                    break;
                case VIA_KEYBOARD_VALUE_SWITCH_MATRIX_STATE: {
                    /* One big-endian 16-bit mask per row, starting at data[3] (VIA reads the payload from there). */
                    memset(&data[2], 0, RAW_HID_REPORT_SIZE - 2u);
                    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
                        const uint8_t pressed = matrix_get_col(col);
                        for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
                            if (pressed & (uint8_t)(1u << row)) {
                                const uint8_t idx = (uint8_t)(3u + row * 2u + (col < 8u ? 1u : 0u));
                                data[idx] |= (uint8_t)(1u << (col & 7u));
                            }
                        }
                    }
                    break;
                }
                case VIA_KEYBOARD_VALUE_KEYCODES_VERSION:
                    // QMK keycodes.h version 0.0.9, encoded as a 32-bit BCD value.
                    data[2] = 0x00;
                    data[3] = 0x00;
                    data[4] = 0x00;
                    data[5] = 0x09;
                    break;
                case VIA_KEYBOARD_VALUE_UPTIME:
                case VIA_KEYBOARD_VALUE_DEVICE_INDICATION:
                default:
                    data[0] = VIA_ID_UNHANDLED;
                    break;
            }
            break;

        case VIA_ID_SET_KEYBOARD_VALUE:
            if (data[1] != VIA_KEYBOARD_VALUE_LAYOUT_OPTIONS && data[1] != VIA_KEYBOARD_VALUE_DEVICE_INDICATION) {
                data[0] = VIA_ID_UNHANDLED;
            }
            break;

        case VIA_ID_DYNAMIC_KEYMAP_GET_KEYCODE: {
            uint16_t keycode = keymap_get_keycode(data[1], data[2], data[3]);
            data[4] = (uint8_t)(keycode >> 8);
            data[5] = (uint8_t)keycode;
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_SET_KEYCODE: {
            uint16_t keycode = (uint16_t)((uint16_t)data[4] << 8) | data[5];
            if (!dynamic_keymap_set_keycode(data[1], data[2], data[3], keycode)) {
                data[0] = VIA_ID_UNHANDLED;
            }
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_GET_ENCODER: {
            uint16_t keycode = dynamic_encoder_get_keycode(data[1], data[2], data[3] != 0);
            data[4]          = (uint8_t)(keycode >> 8);
            data[5]          = (uint8_t)keycode;
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_SET_ENCODER: {
            uint16_t keycode = (uint16_t)((uint16_t)data[4] << 8) | data[5];
            if (!dynamic_encoder_set_keycode(data[1], data[2], data[3] != 0, keycode)) {
                data[0] = VIA_ID_UNHANDLED;
            }
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_RESET:
            dynamic_keymap_reset();
            break;

        case VIA_ID_DYNAMIC_KEYMAP_GET_LAYER_COUNT:
            data[1] = dynamic_keymap_layer_count();
            break;

        case VIA_ID_DYNAMIC_KEYMAP_GET_BUFFER: {
            uint16_t offset    = (uint16_t)((uint16_t)data[1] << 8) | data[2];
            uint8_t  requested = data[3];
            uint8_t  size      = requested;
            if (size > RAW_HID_REPORT_SIZE - 4u) {
                size = RAW_HID_REPORT_SIZE - 4u;
            }
            dynamic_keymap_get_buffer(offset, size, &data[4]);
            data[3] = requested;
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_SET_BUFFER: {
            uint16_t offset    = (uint16_t)((uint16_t)data[1] << 8) | data[2];
            uint8_t  requested = data[3];
            uint8_t  size      = requested;
            if (size > RAW_HID_REPORT_SIZE - 4u) {
                size = RAW_HID_REPORT_SIZE - 4u;
            }
            dynamic_keymap_set_buffer(offset, size, &data[4]);
            data[3] = requested;
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_COUNT:
            data[1] = DYNAMIC_MACRO_COUNT;
            break;

        case VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE:
            data[1] = (uint8_t)(DYNAMIC_MACRO_BYTES >> 8);
            data[2] = (uint8_t)DYNAMIC_MACRO_BYTES;
            break;

        case VIA_ID_DYNAMIC_KEYMAP_MACRO_GET_BUFFER: {
            uint16_t offset    = (uint16_t)((uint16_t)data[1] << 8) | data[2];
            uint8_t  requested = data[3];
            uint8_t  size      = requested;
            if (size > RAW_HID_REPORT_SIZE - 4u) {
                size = RAW_HID_REPORT_SIZE - 4u;
            }
            memset(&data[4], 0, size);
            dynamic_macro_get_buffer(offset, size, &data[4]);
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_MACRO_SET_BUFFER: {
            uint16_t offset = (uint16_t)((uint16_t)data[1] << 8) | data[2];
            uint8_t  size   = data[3];
            if (size > RAW_HID_REPORT_SIZE - 4u) {
                size = RAW_HID_REPORT_SIZE - 4u;
            }
            dynamic_macro_set_buffer(offset, size, &data[4]);
            break;
        }

        case VIA_ID_DYNAMIC_KEYMAP_MACRO_RESET:
            dynamic_macro_reset();
            break;

        case VIA_ID_CUSTOM_GET_VALUE:
            switch (data[2]) {
                case 1:
                    data[3] = user_settings.led_effect;
                    break;
                case 2:
                    data[3] = user_settings.led_brightness;
                    break;
                case 3:
                    data[3] = user_settings.led_speed;
                    break;
                case 4:
                    data[3] = user_settings.ul_effect;
                    break;
                case 5:
                    data[3] = user_settings.ul_brightness;
                    break;
                case 6:
                    data[3] = user_settings.battery_indicator_on;
                    break;
                case 7:
                    data[3] = user_settings.led_color;
                    break;
                case 8:
                    data[3] = user_settings.ul_speed;
                    break;
                default:
                    data[0] = VIA_ID_UNHANDLED;
                    break;
            }
            break;

        case VIA_ID_CUSTOM_SET_VALUE:
            switch (data[2]) {
                case 1:
                    user_settings.led_effect = data[3];
                    indicators_validate_settings();
                    tick_pause();
                    indicators_init();
                    tick_resume();
                    settings_mark_dirty();
                    break;
                case 2:
                    user_settings.led_brightness = data[3];
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 3:
                    user_settings.led_speed = data[3];
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 4:
                    user_settings.ul_effect = data[3];
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 5:
                    user_settings.ul_brightness = data[3];
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 6:
                    user_settings.battery_indicator_on = data[3] ? 1 : 0;
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 7:
                    user_settings.led_color = data[3] ? 1 : 0;
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                case 8:
                    user_settings.ul_speed = data[3] ? 1 : 0;
                    indicators_validate_settings();
                    settings_mark_dirty();
                    break;
                default:
                    data[0] = VIA_ID_UNHANDLED;
                    break;
            }
            break;

        case VIA_ID_CUSTOM_SAVE:
            settings_mark_dirty();
            break;

#    ifdef SIGNALRGB_ENABLE
        case 0xF0u: /* SRGB_CMD_SET_LEDS: [0xF0, start_idx, count(<=9), R,G,B...] */
            if (data[2] > 9u) {
                data[0] = VIA_ID_UNHANDLED;
            } else {
                indicators_direct_set(data[1], data[2], &data[3]);
                data[1] = 0x00u;
            }
            break;

        case 0xF1u: /* SRGB_CMD_EXIT: return to onboard lighting */
            indicators_direct_clear();
            data[1] = 0x00u;
            break;
#    endif

        case VIA_ID_EEPROM_RESET:
        case VIA_ID_BOOTLOADER_JUMP:
        default:
            data[0] = VIA_ID_UNHANDLED;
            break;
    }
}

#endif
