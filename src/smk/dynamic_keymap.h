#pragma once

#include "kbdef.h"
#include "layout.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef DYNAMIC_KEYMAP_ENABLE

#ifndef DYNAMIC_KEYMAP_LAYER_COUNT
#    define DYNAMIC_KEYMAP_LAYER_COUNT 4u
#endif

#define DYNAMIC_KEYMAP_BYTES ((uint16_t)DYNAMIC_KEYMAP_LAYER_COUNT * MATRIX_ROWS * MATRIX_COLS * 2u)

/* VIA macros: DYNAMIC_MACRO_COUNT NUL-terminated strings packed back to back in one buffer that is persisted in the same flash sectors as the keymap, right after it. */
#ifndef DYNAMIC_MACRO_COUNT
#    define DYNAMIC_MACRO_COUNT 16u
#endif
#ifndef DYNAMIC_MACRO_BYTES
#    define DYNAMIC_MACRO_BYTES 768u
#endif

/* VIA encoder map: per layer, per encoder, two keycodes (counter-clockwise, clockwise). */
#ifndef DYNAMIC_ENCODER_COUNT
#    define DYNAMIC_ENCODER_COUNT 1u
#endif
#define DYNAMIC_ENCODER_BYTES ((uint16_t)DYNAMIC_KEYMAP_LAYER_COUNT * DYNAMIC_ENCODER_COUNT * 4u)

#define DYNAMIC_STORE_BYTES ((uint16_t)(DYNAMIC_KEYMAP_BYTES + DYNAMIC_MACRO_BYTES + DYNAMIC_ENCODER_BYTES))

void dynamic_keymap_init(void);
void dynamic_keymap_tick(void);
void dynamic_keymap_task(void);

uint8_t  dynamic_keymap_layer_count(void);
uint16_t keymap_get_keycode(uint8_t layer, uint8_t row, uint8_t col);
bool     dynamic_keymap_set_keycode(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);

uint8_t dynamic_keymap_get_buffer(uint16_t offset, uint8_t size, __xdata uint8_t *data);
uint8_t dynamic_keymap_set_buffer(uint16_t offset, uint8_t size, const __xdata uint8_t *data);

void dynamic_keymap_reset(void);

uint8_t dynamic_macro_get_buffer(uint16_t offset, uint8_t size, __xdata uint8_t *data);
uint8_t dynamic_macro_set_buffer(uint16_t offset, uint8_t size, const __xdata uint8_t *data);
void    dynamic_macro_reset(void);

uint16_t dynamic_encoder_get_keycode(uint8_t layer, uint8_t index, bool clockwise);
bool     dynamic_encoder_set_keycode(uint8_t layer, uint8_t index, bool clockwise, uint16_t keycode);
/* Single byte read for the macro player; returns 0 past the end of the buffer. */
uint8_t dynamic_macro_byte(uint16_t offset);

#else

inline uint16_t keymap_get_keycode(uint8_t layer, uint8_t row, uint8_t col)
{
    return keymaps[layer][row][col];
}

#endif
