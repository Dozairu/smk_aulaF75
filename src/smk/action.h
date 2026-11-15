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

/* --------------------------------------------------------------- config */

#define ACTION_PERMISSIVE_HOLD         0x01u
#define ACTION_HOLD_ON_OTHER_KEY_PRESS 0x02u
#define ACTION_RETRO_TAPPING           0x04u

typedef struct {
    uint16_t tapping_term;   /* ms: hold decision for tap-hold keys        */
    uint16_t quick_tap_term; /* ms: re-press after a tap repeats the tap   */
    uint16_t osk_timeout;    /* ms: one-shot mod/layer expiry, 0 = never   */
    uint8_t  flags;          /* ACTION_PERMISSIVE_HOLD | ...               */
} action_config_t;

extern __xdata action_config_t action_config;

#define ACTION_TAPPING_TERM_DEFAULT 200u
#define ACTION_OSK_TIMEOUT_DEFAULT  5000u

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
