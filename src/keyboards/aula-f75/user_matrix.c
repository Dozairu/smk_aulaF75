#include "kbdef.h"
#include "gpio.h"
#include "user_matrix.h"

/*
 * 6 rows x 15 columns matrix scan.
 *
 * Column selection is ACTIVE-LOW, rows are read ACTIVE-LOW with pull-ups.
 * Scan order read from the stock firmware jump table @ 0x72F4.
 */

/* The 18 backlight PWM outputs: 6 LED rows x R/G/B */
#define LED_P1_MASK (uint8_t)0x3f
#define LED_P2_MASK (uint8_t)0x3f
#define LED_P3_MASK (uint8_t)0x3f

void user_matrix_cols_deselect_all(void)
{
    GPIO_HIGH(4, KB_C_P4_MASK);
    GPIO_HIGH(5, KB_C_P5_MASK);
    GPIO_HIGH(6, KB_C_P6_MASK);
}

void user_matrix_scan_pre(void)
{
    GPIO_OUTPUT(4, KB_C_P4_MASK);
    GPIO_OUTPUT(5, KB_C_P5_MASK);
    GPIO_OUTPUT(6, KB_C_P6_MASK);
}

void user_matrix_scan_post(void)
{
    GPIO_PULLUP_OFF(4, KB_C_P4_MASK);
    GPIO_INPUT(4, KB_C_P4_MASK);
    GPIO_PULLUP_OFF(5, KB_C_P5_MASK);
    GPIO_INPUT(5, KB_C_P5_MASK);
    GPIO_PULLUP_OFF(6, KB_C_P6_MASK);
    GPIO_INPUT(6, KB_C_P6_MASK);
}

/*
 * Snapshot of key states, for reactive backlighting.
 *
 * `src/smk/matrix.c` keeps its `matrix[]` array private and its hooks do not
 * receive the matrix position; `kb_process_record()` only sees a keycode. Yet
 * the reactive engine needs (row, col).
 *
 * However, the position passes through here twice per column: `col_select`
 * receives it, and `read_rows` is called right after. Remembering the former
 * and storing the latter is enough, without touching a single line of shared
 * code. Cost: fifteen bytes and one store per row read.
 *
 * Matrix scanning and LED sub-frames are mutually exclusive in time (`tick.c`
 * alternates them inside the same ISR), so there is no race condition.
 */
static __xdata uint8_t scan_pressed[MATRIX_COLS];
static uint8_t         scan_col;

uint8_t user_matrix_pressed(uint8_t col)
{
    return (col < MATRIX_COLS) ? scan_pressed[col] : 0;
}

void user_matrix_col_select(uint8_t col) // active-low: drive to 0
{
    scan_col = col;

    switch (col) {
        case 0:  KB_C0  = 0; break;
        case 1:  KB_C1  = 0; break;
        case 2:  KB_C2  = 0; break;
        case 3:  KB_C3  = 0; break;
        case 4:  KB_C4  = 0; break;
        case 5:  KB_C5  = 0; break;
        case 6:  KB_C6  = 0; break;
        case 7:  KB_C7  = 0; break;
        case 8:  KB_C8  = 0; break;
        case 9:  KB_C9  = 0; break;
        case 10: KB_C10 = 0; break;
        case 11: KB_C11 = 0; break;
        case 12: KB_C12 = 0; break;
        case 13: KB_C13 = 0; break;
        case 14: KB_C14 = 0; break;
    }
}

void user_matrix_col_deselect(uint8_t col)
{
    switch (col) {
        case 0:  KB_C0  = 1; break;
        case 1:  KB_C1  = 1; break;
        case 2:  KB_C2  = 1; break;
        case 3:  KB_C3  = 1; break;
        case 4:  KB_C4  = 1; break;
        case 5:  KB_C5  = 1; break;
        case 6:  KB_C6  = 1; break;
        case 7:  KB_C7  = 1; break;
        case 8:  KB_C8  = 1; break;
        case 9:  KB_C9  = 1; break;
        case 10: KB_C10 = 1; break;
        case 11: KB_C11 = 1; break;
        case 12: KB_C12 = 1; break;
        case 13: KB_C13 = 1; break;
        case 14: KB_C14 = 1; break;
    }
}

/*
 * Row N on bit N. Reproduces the exact calculation of the stock firmware
 * (0x73A6): P5 shifted left by one then masked with 0x30, combined with the
 * low 4 bits of P7, unused bits forced to 1 -- same convention as SMK, which
 * then inverts the sample.
 */
uint8_t user_matrix_read_rows(void)
{
    const uint8_t raw = (uint8_t)((P7 & 0x0f) | ((P5 << 1) & 0x30) | 0xc0);

    /* Active-low rows: a 1 bit in the snapshot means a pressed key.
     * `matrix.c` performs the same bitwise complement on its side. */
    if (scan_col < MATRIX_COLS) {
        scan_pressed[scan_col] = (uint8_t)(~raw) & 0x3f;
    }
    return raw;
}

void user_matrix_sinks_off(void)
{
    GPIO_LOW(1, LED_P1_MASK);
    GPIO_LOW(2, LED_P2_MASK);
    GPIO_LOW(3, LED_P3_MASK);
}
