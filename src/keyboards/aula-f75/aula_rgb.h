#pragma once

#include <stdint.h>

#ifdef __SDCC
#    define AULA_RGB_XDATA __xdata
#else
#    define AULA_RGB_XDATA
#endif

/*
 * LED matrix for the AULA F75.
 *
 * Topology (inverse of the NuPhy Air60): the 15 matrix columns act as
 * multiplexing strobes, and the 18 PWM outputs drive the 6 LED rows x R/G/B.
 * On every column advance, all 18 duty cycles must be reloaded.
 */

#define AULA_RGB_ROWS     6
#define AULA_RGB_COLORS   3
#define AULA_RGB_COLS     15
#define AULA_RGB_CHANNELS (AULA_RGB_ROWS * AULA_RGB_COLORS) /* 18 */

/* Stock firmware PWM period: PWM0PERDH=0x04 @ 0x6707, PWM0PERDL=0xB0 @ 0x670D */
#define AULA_RGB_PERIOD 0x04B0u

/*
 * POLARITY AND PHASE OFFSET -- ESTABLISHED FROM THE DUMP, nothing inferred here.
 *
 * The stock firmware writes DUTY1 ONCE, during initialization (0x6713-0x68CD),
 * and never touches it again: out of all 36 references to DUTY1 registers in
 * the entire image, all 36 are inside that routine, and zero elsewhere. Only
 * DUTY2 is modulated, by the column loader at 0x6E61.
 *
 * Moreover, DUTY1 is neither 0 nor the period: it is a PER-CHANNEL constant
 * taken from CODE 0x2922 (18 bytes, `0xB4 + index`, indexed by
 * `row * 3 + color`). The 18 values written at init match this table
 * byte-for-byte, permuted by the load order of 0x6E61 -- the two findings
 * cross-check each other and lock down the channel <-> (row, color) mapping.
 *
 * The converter at 0x76C3 spells out the arithmetic:
 *
 *     DUTY2 = (value << 2) + phase[row * 3 + color]
 *
 * (`MOV B,#4 / MUL AB` followed by a 16-bit add of the table byte), stored in
 * BIG-ENDIAN order in the buffer at 0x05A2 + col * 36 copied by the loader.
 *
 * This establishes the POLARITY by monotonicity -- neither the datasheet nor
 * `pwm.h` states which of the two DUTY1/DUTY2 edges starts the pulse, and we
 * do not need them to. DUTY1 is fixed and DUTY2 grows with the intensity value;
 * under the opposite interpretation, value 1 would produce a 1196-tick pulse
 * and value 255 a 180-tick pulse, meaning brightness would DECREASE with value
 * and jump discontinuously at zero. Absurd. Thus the pulse spans from DUTY1 to
 * DUTY2, its width is `value << 2`, the phase offset cancels out of the perceived
 * brightness, and the duty cycle is DIRECT -- 0 = off (DUTY2 = DUTY1, zero
 * width), 255 = 1020/1200 or 85% of the period.
 *
 * An earlier version defined `AULA_RGB_DUTY_INVERTED 1` with DUTY1 = period and
 * `duty = period - value * 4.6875`. That was completely backwards: the panel
 * would have been at full brightness at rest and dark at maximum brightness.
 *
 * The 18 phases are staggered by one tick each; the stock firmware thus spreads
 * out the rising edges of the 18 channels instead of switching them all at once.
 */
#define AULA_RGB_PHASE_BASE 0xB4u
#define AULA_RGB_PHASE(channel) (uint16_t)(AULA_RGB_PHASE_BASE + (channel))

/* Stock hue wheel (CODE 0x2B2A), 192 entries. */
#define AULA_RGB_WHEEL_SIZE 192
void aula_rgb_wheel(uint8_t index, uint8_t out[3]);

/* Clears the entire framebuffer (all LEDs off). */
void     aula_rgb_clear(void);
void     aula_rgb_set(uint8_t row, uint8_t col, uint8_t red, uint8_t green, uint8_t blue);
uint16_t aula_rgb_duty(uint8_t channel, uint8_t value);

/* Loads all 18 duty cycles for the given column into the PWM registers,
 * in the order extracted from the stock firmware. */
void aula_rgb_load_column(uint8_t col);
