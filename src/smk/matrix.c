#include "matrix.h"
#include "dynamic_keymap.h"
#include "via_macro.h"
#include "report.h"
#include "debug.h"
#include "layout.h"
#include "user_layout.h"
#include "kb.h"
#include "user_matrix.h"
#include "kbdef.h"
#include "host.h"
#include "delay.h"
#include "indicators.h"
#include "sleep.h"
#include <stdlib.h>
#include <stdbool.h>

typedef uint8_t matrix_col_t;

matrix_col_t matrix[MATRIX_COLS];
matrix_col_t matrix_previous[MATRIX_COLS];

#ifdef MATRIX_INTERLEAVED_SCAN
/* Two-bit per-key vertical counter for 4-scan (~12 ms) release debounce. */
static matrix_col_t debounce_cnt0[MATRIX_COLS];
static matrix_col_t debounce_cnt1[MATRIX_COLS];
#endif

volatile bool matrix_updated;

static bool report_batching;
static bool report_dirty;

uint8_t action_layer;

uint8_t default_layer;

void matrix_init()
{
    action_layer    = 0;
    default_layer   = 0;
    matrix_updated  = false;
    report_batching = false;
    report_dirty    = false;

    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix[i]          = 0;
        matrix_previous[i] = 0;
#ifdef MATRIX_INTERLEAVED_SCAN
        debounce_cnt0[i]   = 0;
        debounce_cnt1[i]   = 0;
#endif
    }
}

static void emit_keyboard_report(void)
{
    if (report_batching) {
        report_dirty = true;
    } else {
        send_keyboard_report();
    }
}

void set_default_layer(uint8_t layer)
{
    default_layer = layer;
}

static uint16_t resolve_keycode(uint16_t base, uint8_t row, uint8_t col)
{
    if (!action_layer) {
        return base;
    }

    const uint16_t overlay = keymap_get_keycode(action_layer, row, col);
    return (overlay == KC_TRANSPARENT) ? base : overlay;
}

void send_keycode(uint16_t qcode, bool pressed)
{
    if (IS_MODIFIER_KEYCODE(qcode)) {
        if (pressed) {
            add_mods(MOD_BIT((uint8_t)(qcode & 0xFF)));
        } else {
            del_mods(MOD_BIT((uint8_t)(qcode & 0xFF)));
        }
        emit_keyboard_report();
        return;
    }

    if (IS_BASIC_KEYCODE(qcode)) {
        if (pressed) {
            add_key((uint8_t)(qcode & 0xFF));
        } else {
            del_key((uint8_t)(qcode & 0xFF));
        }
        emit_keyboard_report();
        return;
    }

    if (IS_SYSTEM_KEYCODE(qcode)) {
        host_system_send(pressed ? keycode_to_system(qcode) : 0);
        return;
    }

    if (IS_CONSUMER_KEYCODE(qcode)) {
        host_consumer_send(pressed ? keycode_to_consumer(qcode) : 0);
        return;
    }
}

static void process_key_state(uint8_t row, uint8_t col, bool pressed)
{
    const uint16_t base = keymap_get_keycode(default_layer, row, col);

    if (IS_QK_MOMENTARY(base)) {
        if (pressed) {
            action_layer = QK_MOMENTARY_GET_LAYER(base);
        } else {
            clear_keys();
            emit_keyboard_report();
            host_consumer_send(0);
            host_system_send(0);
            action_layer = 0;
        }
        return;
    }

    const uint16_t qcode = resolve_keycode(base, row, col);

#ifdef VIA_MACRO_ENABLE
    if (IS_QK_MACRO(qcode)) {
        if (pressed && qcode - QK_MACRO < DYNAMIC_MACRO_COUNT) {
            via_macro_play((uint8_t)(qcode - QK_MACRO));
        }
        return;
    }
#endif

    if (!kb_process_record(qcode, pressed)) {
        return;
    }

    if (!layout_process_record(qcode, pressed)) {
        return;
    }

    send_keycode(qcode, pressed);
}

uint8_t matrix_get_col(uint8_t col)
{
    return (col < MATRIX_COLS) ? matrix[col] : 0;
}

#ifdef MATRIX_INTERLEAVED_SCAN
void matrix_scan_col(uint8_t col)
{
    if (col >= MATRIX_COLS) {
        return;
    }

    user_matrix_col_select(col);

    delay_us(6); // let the row lines settle before sampling
    const uint8_t sample1 = user_matrix_read_rows();
    delay_us(2);
    const uint8_t sample2 = user_matrix_read_rows();

    user_matrix_col_deselect(col);

    if (sample1 != sample2) {
        return;
    }

    const matrix_col_t raw = (matrix_col_t)~sample1;
    const matrix_col_t cur = matrix[col];
    if (raw == cur && debounce_cnt0[col] == 0 && debounce_cnt1[col] == 0) {
        return;
    }

    const matrix_col_t press    = (matrix_col_t)(raw & (matrix_col_t)~cur);
    const matrix_col_t rel_cand = (matrix_col_t)((matrix_col_t)~raw & cur);

    debounce_cnt0[col] &= rel_cand;
    debounce_cnt1[col] &= rel_cand;

    /* Asymmetric debounce: 0 ms eager press, 4-scan (~12 ms) deferred release. */
    matrix_col_t next = (matrix_col_t)(cur | press);

    const matrix_col_t done = (matrix_col_t)(rel_cand & debounce_cnt0[col] & debounce_cnt1[col]);
    next &= (matrix_col_t)~done;

    const matrix_col_t inc = (matrix_col_t)(rel_cand & (matrix_col_t)~done);
    debounce_cnt1[col] ^= (matrix_col_t)(debounce_cnt0[col] & inc);
    debounce_cnt0[col] ^= inc;
    debounce_cnt0[col] &= (matrix_col_t)~done;
    debounce_cnt1[col] &= (matrix_col_t)~done;

    if (next != cur) {
        matrix[col]    = next;
        matrix_updated = true;
    }
}
#else
void matrix_scan_full(void)
{
    indicators_pwm_disable();

    user_matrix_sinks_off();

    user_matrix_scan_pre();
    user_matrix_cols_deselect_all();

    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        user_matrix_col_select(col);

        delay_us(8); // let the row lines settle before sampling
        const uint8_t sample1 = user_matrix_read_rows();
        delay_us(4);
        const uint8_t sample2 = user_matrix_read_rows();

        if (sample1 == sample2) {
            matrix[col] = ~sample1;
        }

        user_matrix_col_deselect(col);
    }

    user_matrix_scan_post();

    indicators_pwm_enable();

    matrix_updated = true;
}
#endif

uint8_t matrix_task()
{
    if (!matrix_updated) {
        return false;
    }

    // Snapshot the scan-written matrix[], then diff it against
    // matrix_previous[]. No lock needed: each column byte reads atomically, so a
    // concurrent scan lands cleanly on one side of the read - at worst a
    // transition is split across two main-loop iterations, never lost.
    matrix_col_t snapshot[MATRIX_COLS];
    matrix_updated = false;
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        snapshot[i] = matrix[i];
    }

    bool matrix_changed = false;
    report_batching     = true;
    report_dirty        = false;

    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        const matrix_col_t current_col = snapshot[col];
        const matrix_col_t col_changes = current_col ^ matrix_previous[col];
        if (!col_changes) {
            continue;
        }
        matrix_changed = true;
        sleep_note_activity(); // a key changed state; reset the inactivity timer

        matrix_col_t row_mask = 1;
        for (uint8_t row = 0; row < MATRIX_ROWS; row++, row_mask <<= 1) {
            if (col_changes & row_mask) {
                const bool key_pressed = current_col & row_mask;
                process_key_state(row, col, key_pressed);
            }
        }

        matrix_previous[col] = current_col;
    }

    report_batching = false;
    if (report_dirty) {
        report_dirty = false;
        send_keyboard_report();
    }

    return matrix_changed;
}
