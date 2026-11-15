#pragma once

#include <stdint.h>
#include <stdbool.h>

void    matrix_init();
uint8_t matrix_task();
uint8_t matrix_get_col(uint8_t col);
void    send_keycode(uint16_t qcode, bool pressed);

/* Matrix position of the key event being processed, valid inside
 * kb_process_record() / layout_process_record(): lets a keyboard give
 * per-key feedback (e.g. blink the key that switched a mode). */
extern uint8_t matrix_event_row;
extern uint8_t matrix_event_col;

void matrix_scan_full();
void matrix_scan_col(uint8_t col);
