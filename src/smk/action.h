#pragma once

/*
 * Key action engine: layers, tap-hold, one-shot -- QMK semantics, sized for an
 * 8051 with 4 KiB of XRAM.
 *
 * The engine is PORTABLE C: no SFRs, no SDCC-only syntax beyond the storage
 * qualifiers below, which compile away on a host compiler. Everything it needs
 * from the firmware goes through the action_hal_* functions, which the
 * firmware implements in matrix.c and the host tests implement as fakes
 * (tests/host/test_action.c).
 *
 * Event flow:
 *   matrix_task() --action_event()--> queue --> [tap-hold decision] -->
 *   press/release handlers --action_hal_send()/action_hal_mods()--> reports
 * and action_task() from the main loop resolves timeouts (tapping term,
 * one-shot timeout).
 */

#include <stdbool.h>
#include <stdint.h>

#ifndef __SDCC
#    define __xdata
#    define __code const
#endif

/* Tap dance (TD(n) keycodes) and combos are compiled in with Vial, which
 * configures them. */
#if defined(VIAL_ENABLE) && !defined(ACTION_TAP_DANCE)
#    define ACTION_TAP_DANCE
#endif
#if defined(VIAL_ENABLE) && !defined(ACTION_COMBO)
#    define ACTION_COMBO
#endif
#if defined(VIAL_ENABLE) && !defined(ACTION_KEY_OVERRIDE)
#    define ACTION_KEY_OVERRIDE
#endif

/* --------------------------------------------------------------- config */

#define ACTION_PERMISSIVE_HOLD         0x01u
#define ACTION_HOLD_ON_OTHER_KEY_PRESS 0x02u
#define ACTION_RETRO_TAPPING           0x04u

typedef struct {
    uint16_t tapping_term;   /* ms: hold decision for tap-hold keys        */
    uint16_t quick_tap_term; /* ms: re-press after a tap repeats the tap   */
    uint16_t osk_timeout;    /* ms: one-shot mod/layer expiry, 0 = never   */
    uint16_t combo_term;     /* ms: all keys of a combo pressed within it  */
    uint8_t  flags;          /* ACTION_PERMISSIVE_HOLD | ...               */
} action_config_t;

extern __xdata action_config_t action_config;

#define ACTION_TAPPING_TERM_DEFAULT 200u
#define ACTION_OSK_TIMEOUT_DEFAULT  5000u
#define ACTION_COMBO_TERM_DEFAULT   50u

void action_config_defaults(void);

/* ----------------------------------------------------------------- API */

void action_init(void);

/* One physical key transition, in scan order. */
void action_event(uint8_t row, uint8_t col, bool pressed);

/* Timeouts. Call from the main loop; cheap when idle. */
void action_task(void);

/* Layer state, for indicators and the encoder. */
uint8_t action_layer_state(void);         /* bit n = layer n on (default layer included) */
uint8_t action_highest_layer(void);
void    action_set_default_layer(uint8_t layer);

/* True when nothing is held, buffered or armed (sleep / macro-mode gate). */
bool action_idle(void);

/* ------------------------------------------------- HAL (firmware supplies) */

uint16_t action_hal_keymap(uint8_t layer, uint8_t row, uint8_t col);
uint8_t  action_hal_layer_count(void);
uint16_t action_hal_timer(void);                    /* free-running ms */
void     action_hal_send(uint16_t keycode, bool pressed); /* a plain, resolved keycode */
void     action_hal_mods(uint8_t mods, bool on);     /* 8-bit HID modifier mask */
void     action_hal_event_pos(uint8_t row, uint8_t col);

#ifdef ACTION_TAP_DANCE
/* Tap dance entry n in Vial's layout: on tap, on hold, on double tap, on
 * tap + hold, tapping term (ms, 0 = the global one). False: no entry n. */
#    define TD_ON_TAP      0u
#    define TD_ON_HOLD     1u
#    define TD_ON_DOUBLE   2u
#    define TD_ON_TAP_HOLD 3u
#    define TD_TERM        4u
#    define TD_FIELDS      5u
bool action_hal_tap_dance(uint8_t index, __xdata uint16_t *entry);
#endif

#ifdef ACTION_COMBO
/* Combo entry n in Vial's layout: four input keycodes (KC_NO = unused), then
 * the output keycode. False: no entry n. Inputs are keycodes, matched against
 * what each key resolves to on the active layers when it is pressed. */
#    define COMBO_INPUTS 4u
#    define COMBO_OUTPUT 4u
#    define COMBO_FIELDS 5u
#    define COMBO_MAX    16u /* entries the engine can address */
bool action_hal_combo(uint8_t index, __xdata uint16_t *entry);
#endif

#ifdef ACTION_KEY_OVERRIDE
/* Key override entry n, vial_key_override_entry_t read as five u16 LE:
 *   [0] trigger keycode   [1] replacement keycode   [2] layer mask
 *   [3] trigger mods | negative mods << 8
 *   [4] suppressed mods | options << 8
 * Mods are 8-bit HID masks. False: no entry n. */
#    define KO_TRIGGER     0u
#    define KO_REPLACEMENT 1u
#    define KO_LAYERS      2u
#    define KO_MODS        3u
#    define KO_SUPPRESS    4u
#    define KO_FIELDS      5u
#    define KO_MAX         16u
/* options (vial-qmk vial_ko_option_*) */
#    define KO_OPT_ACT_TRIGGER_DOWN       0x01u
#    define KO_OPT_ACT_REQUIRED_MOD_DOWN  0x02u /* not supported yet */
#    define KO_OPT_ACT_NEGATIVE_MOD_UP    0x04u /* not supported yet */
#    define KO_OPT_ONE_MOD                0x08u
#    define KO_OPT_NO_REREGISTER_TRIGGER  0x10u
#    define KO_OPT_NO_UNREGISTER_ON_OTHER 0x20u
#    define KO_OPT_ENABLED                0x80u
bool action_hal_key_override(uint8_t index, __xdata uint16_t *entry);
#endif
