#pragma once

#include <stdbool.h>

/*
 * "Auto-type" mode for the AULA F75.
 *
 * Types a random letter, immediately deletes it with Backspace, and repeats
 * every 1 s +/- 500 ms. The host's text remains unchanged, while keeping the
 * user session active. There is no such feature in the stock firmware.
 *
 * Splitting this into two functions is not a stylistic choice, but the same
 * constraint as in `aula_encoder.c`: `send_keyboard_report()` traverses the
 * report stack, which cannot be called from an ISR without an SDCC overlay
 * collision -- something `utils/check_interrupts.py` fails the build to
 * prevent.
 */

/* From `kb_process_record`: arms the mode (when MACRO_TG is pressed). */
void aula_macro_arm(void);

/*
 * From `kb_process_record`, for EVERY key event. Returns true when the event
 * must be swallowed -- silencing the key press that cancels the mode, and
 * preventing the release of the activation combo from immediately stopping it.
 */
bool aula_macro_intercept(bool pressed);

/* From the ISR (LED rendering): is the mode currently typing? */
bool aula_macro_active(void);

/* From the LED subframe (ISR, ~420 us): advances the timer. */
void aula_macro_tick(void);

/* From the main loop: sends reports and monitors for cancellation. */
void aula_macro_task(void);
