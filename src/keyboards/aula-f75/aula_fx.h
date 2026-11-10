#pragma once

#include <stdint.h>

/*
 * Data and primitives for the stock firmware's rendering engines.
 *
 * This file only contains what was EXTRACTED from `assets/f75_full_jtag.bin`:
 * four tables copied verbatim, plus the pseudo-random generator whose role --
 * rather than polynomial -- is transcribed. Effect scheduling and state live in
 * `layouts/default/indicators.c`.
 *
 * Stock firmware key ID encoding: `col * 8 + row`, `0xFF` for "no key". This is
 * what `0x9659` unpacks into `(id >> 3, id & 7)` before calling
 * `rgb_key_suppressed`, and what the rings below use -- ring 0 is `0x3A` =
 * (col 7, row 2), the exact center documented in the reverse-engineering notes
 * for the wave effect.
 *
 * None of this has been executed: no firmware has ever been flashed to the
 * device.
 */

#define AULA_FX_COLS 15
#define AULA_FX_ROWS 6

/* Second stock palette, CODE 0x2D6D: 128 (R,G,B) triplets, 384 bytes.
 * Distinct from the 192-entry wheel in `aula_rgb.c` -- its ramps are more
 * saturated and its circle closes in 128 steps. Read by engine index 15. */
#define AULA_FX_PALETTE_SIZE 128
void aula_fx_palette(uint8_t index, uint8_t out[3]);

/*
 * Stock color modes: 0 to 6 are fixed colors (CODE 0xC800), 7 is rainbow,
 * which reads the hue wheel instead of the palette. The stock firmware stores
 * this mode in `b1[3:0]` of its per-effect record, and latches it in XRAM
 * 0x0897.
 */
#define AULA_FX_COLOR_MODES  8
#define AULA_FX_COLOR_WHEEL  ((uint8_t)(AULA_FX_COLOR_MODES - 1))
void aula_fx_color(uint8_t mode, uint8_t out[3]);

/* Concentric wave rings, CODE 0x2959: 9 groups of 13 IDs, `0xFF` = empty slot.
 * Read by engine index 1 (and index 17, which is the same engine with its
 * parameter fixed to 9). */
#define AULA_FX_RINGS      9
#define AULA_FX_RING_SLOTS 13
uint8_t aula_fx_ring(uint8_t ring, uint8_t slot);

/*
 * Stock key map, CODE 0xC500, indexed as `col * 6 + row`: returns the ID
 * `col * 8 + row`, or AULA_FX_NO_KEY when the grid slot has no key. Serves
 * as both a presence test and a spatial map -- see the comment in aula_fx.c.
 */
#define AULA_FX_NO_KEY ((uint8_t)0xFF)
uint8_t aula_fx_key_id(uint8_t col, uint8_t row);

/*
 * Spatial column of an electrical column: `aula_fx_key_id() >> 3`. Identity
 * everywhere except on row 4, which permutes.
 */
uint8_t aula_fx_render_col(uint8_t col, uint8_t row);

/* "Gaming" mask from CODE 0xCAFC: Esc, W A S D, and the arrow cluster.
 * Same row-mask encoding. */
uint8_t aula_fx_gaming(uint8_t col);

/*
 * Per-key phase field for effect 15, CODE 0x9FEA.
 *
 * It is not written by code, but by the C INITIALIZER: the Keil decompression
 * table at CODE 0x9FDE has only three records, and the third copies 126 bytes
 * to XDATA 0x0E49 at startup. `fcn @ 0xACA3` then copies them transposed
 * (stride 21 -> stride 6) into the phase plane at 0x0017. That explains why no
 * instruction in the image references 0x0E49 for writing: it is a constant,
 * not a variable.
 *
 * Values range from 1 to 127 -- matching the exact index range of the
 * 128-entry palette -- and form an angular sweep: phase increases along the
 * top row until wrapping past zero between columns 12 and 13, and decreases
 * downward to the left. A rotating wave, not a translating one.
 */
/*
 * Per-key phase for shimmer (stock effect 6): CODE 0x2A2E in rainbow mode,
 * CODE 0x2AAC in fixed-color mode. Laid out as `col * 6 + row`.
 */
uint8_t aula_fx_shimmer_phase(uint8_t col, uint8_t row, uint8_t rainbow);

/* Breathing ramp, CODE 0x29CE: 96 entries, 22 -> 255 -> 22. */
#define AULA_FX_BREATH_SIZE 96
uint8_t aula_fx_breath(uint8_t phase);

uint8_t aula_fx_keywave(uint8_t col, uint8_t row);

/* Pseudo-random draw, 0-255. */
uint8_t aula_fx_rand(void);
