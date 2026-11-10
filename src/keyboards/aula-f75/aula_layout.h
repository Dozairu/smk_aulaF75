#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * "AZERTY compensation" mode for the AULA F75. Persisted in flash: the
 * keyboard boots back into the mode it was left in.
 *
 * The keyboard is a 75% ANSI board whose keycaps carry the US International
 * layout. When the host is configured for French AZERTY (Windows fr-FR), the
 * two clash: the key labeled `A` types `q`, the `1` key types `&`. This mode
 * shifts the translation burden onto the KEYBOARD: you type what is printed on
 * the keycaps, and the right character comes out without changing anything on
 * the PC.
 *
 * Three of the five US International dead keys delegate to the host's dead key
 * (`^`, `` ` ``, `~`): the keyboard emits the dead key and the host composes.
 *
 * Two require a state machine. The acute accent because fr-FR does not have
 * one -- `é` is a dedicated key there. The DIAERESIS because the host does have
 * one, but its standalone symbol is `¨` rather than `"`: verified on hardware,
 * it is the only one of the four whose bare character differs from US
 * International.
 *
 * The tick/task split is the same as in `aula_encoder.c` and `aula_macro.c`,
 * and for the same reason: `send_keyboard_report()` traverses the report stack,
 * which cannot be called from an ISR without an SDCC overlay collision --
 * something `utils/check_interrupts.py` fails the build to prevent.
 */

/* From `kb_process_record`: toggles the mode (when LAYOUT_AZ is pressed). */
void aula_layout_toggle(void);

/*
 * From `indicators_validate_settings()`, right after NVM is loaded.
 * NOT from `kb_init()`: that runs BEFORE `restore_settings()` in `main()`, so
 * there would be nothing to read yet.
 */
void aula_layout_restore(bool on);

/*
 * From `kb_process_record`, for EVERY key event. Returns true when the event
 * has been handled and should be swallowed.
 */
bool aula_layout_intercept(uint16_t qcode, bool pressed);

/*
 * `€` on `Fn + E`. Targeted at a FRENCH host, where it is AltGr+E -- and
 * therefore independent of the mode, which does not need to be enabled for this
 * to work.
 */
void aula_layout_euro(void);

/* From the LED subframe (ISR, ~420 us): advances the timers. */
void aula_layout_tick(void);

/* From the main loop: drains the output queue. */
void aula_layout_task(void);
