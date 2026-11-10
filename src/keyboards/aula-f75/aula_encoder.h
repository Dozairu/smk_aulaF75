#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Rotary encoder knob for the AULA F75.
 *
 * The PRESS is an ordinary matrix position -- `[row 0][col 14]`, wired to
 * "mute" in the keymap. ROTATION, however, is outside the matrix: two
 * quadrature phases on `P0.5` and `P0.6`.
 *
 * Splitting this into two functions is not a stylistic choice.
 * `aula_encoder_sample()` lives in the Timer2 ISR and `aula_encoder_task()` in
 * the main loop: `host_consumer_send()` traverses the report stack, which
 * cannot be called from an ISR without causing an SDCC overlay collision --
 * something `utils/check_interrupts.py` fails the build to prevent.
 */

/* From the LED subframe (ISR, ~400 us): samples and accumulates. */
void aula_encoder_sample(void);

/* From the main loop: sends pending consumer reports. */
void aula_encoder_task(void);

/*
 * Rotation indicator, read from the rendering ISR. The keyboard beats like a
 * heart for ~1.5 s after the last detent, and the pace follows the direction:
 * turning up speeds up, turning down slows down. The level is RELATIVE --
 * nothing in the USB or radio protocol tells the keyboard the host's volume.
 */
bool aula_encoder_beating(void);

/* Pattern steps per scan wrap, in 1/256ths. 256 = pace of the auto-type
 * heartbeat. */
uint16_t aula_encoder_beat_rate(void);

/* Current step, 0 (lowest) to 7. The renderer derives the hue from it, so
 * COLOR indicates the level while PACE indicates the direction. */
uint8_t aula_encoder_beat_level(void);
