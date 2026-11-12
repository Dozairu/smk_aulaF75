#pragma once

#include <stdint.h>
#include <stdbool.h>

void    matrix_init();
uint8_t matrix_task();
uint8_t matrix_get_col(uint8_t col);
void    send_keycode(uint16_t qcode, bool pressed);

void matrix_scan_full();
