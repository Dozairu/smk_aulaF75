#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * AULA F75 status indicator -- the white LED between Esc and F1.
 *
 * This is not a key LED. It is wired to the GREEN and BLUE channels of matrix
 * position `[row 0][col 14]`, the encoder click position at the far end of the
 * keyboard; the RED channel of that same position drives the Caps Lock
 * indicator. Measured on hardware, see `indicators.c`.
 *
 * Both channels light the SAME white LED and their intensities add up: there is
 * therefore neither color nor a second indicator to work with. Patterns are
 * distinguished solely by their CADENCE -- hence one, two, or three flashes for
 * the three transports, which can be counted at a glance.
 *
 * ISR / main-loop split identical to `aula_encoder.c` and `aula_macro.c`: the
 * engine steps in the ISR, events are posted by the main loop.
 */

#define AULA_STATUS_WIRED     0 /* switch to wired           : 1 long flash    */
#define AULA_STATUS_24G       1 /* switch to 2.4 GHz         : 2 flashes       */
#define AULA_STATUS_BT        2 /* switch to Bluetooth       : 3 flashes       */
#define AULA_STATUS_PAIRING   3 /* pairing in progress       : fast blink      */
#define AULA_STATUS_LINKED    4 /* link established          : single breath   */
#define AULA_STATUS_LOST      5 /* link lost                 : 2 long flashes  */
#define AULA_STATUS_SLEEP_IN  6 /* entering sleep            : fade out        */
#define AULA_STATUS_SLEEP_OUT 7 /* waking from sleep         : fade in         */
#define AULA_STATUS_LOW_BATT  8 /* low battery               : slow pulse      */
#define AULA_STATUS_AZERTY_ON  9 /* AZERTY mode enabled      : 4 short flashes */
#define AULA_STATUS_AZERTY_OFF 10 /* AZERTY mode disabled    : 1 short flash   */
#define AULA_STATUS_AZERTY    11 /* AZERTY mode active       : steady dim glow */
#define AULA_STATUS_NONE      0xFF

/* From the main loop: plays a pattern, overriding whichever is currently running. */
void aula_status_event(uint8_t event);

/*
 * From `rf_task()`: detects transport, connection, and battery transitions, and
 * posts the corresponding event. All edge-detection logic lives here so that
 * `aula_rf.c` only has a single line to call.
 */
void aula_status_poll(uint8_t link, bool connected, bool low_batt);

/* True while a ONE-SHOT pattern is playing. Looping patterns -- pairing,
 * low battery -- do not count: they never finish on their own. */
bool aula_status_busy(void);

/*
 * Permanent indicator for AZERTY mode. This is a resting STATE, like low
 * battery -- and on a single white LED two resting states cannot be told apart.
 * Priority is therefore settled here, once and for all:
 *
 *   low battery  >  AZERTY active  >  off
 *
 * Battery comes first: it is urgent, whereas AZERTY mode has already been
 * announced by its toggle pattern.
 */
void aula_status_set_azerty(bool on);

/* True whenever the indicator is showing something, whether a one-shot pattern
 * or a resting state. Acts as a rendering gate: without it, patterns would be
 * invisible when the backlight is off -- right when you look at them. */
bool aula_status_lit(void);

/* From the ISR, when the refresh scan wraps around (~37.9 ms). */
void aula_status_tick(void);

/* From the ISR: intensity to write to the green and blue channels. */
uint8_t aula_status_level(void);
