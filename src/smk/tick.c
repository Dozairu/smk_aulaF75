#include "tick.h"
#include "systick.h"
#include "matrix.h"
#include "dynamic_keymap.h"
#include "via_macro.h"
#include "indicators.h"
#include "keyboard.h"
#include "settings.h"
#include "sleep.h"
#include "kbdef.h"
#include <stdbool.h>
#include <stdint.h>

// subframes must outnumber the scans, or the refresh rate is the scan rate divided by the subframe count.
#ifndef LED_SUBFRAMES_PER_SCAN
#    define LED_SUBFRAMES_PER_SCAN 1
#endif

#ifdef MATRIX_INTERLEAVED_SCAN
/*
 * Interleaved column-by-column mode:
 *   Every 200 us Timer2 slot scans one key column (~17-25 us) and immediately
 *   displays that same LED column for one hardware-timed 100 us PWM period,
 *   eliminating full-panel dark blanking gaps while delivering a 333 Hz (3.0 ms)
 *   physical matrix scan and RGB refresh with >60% CPU headroom for USB/EUART0.
 *   Every 2nd slot (400 us exact) drives settings/keymap/macro/encoder ticks,
 *   and every 32nd slot (6.4 ms) drives sleep_note_frame().
 */
static volatile uint8_t interleave_col;
static volatile bool    subtick_half;
static volatile uint8_t sleep_frame_div;

void tick_init(void)
{
    interleave_col  = 0;
    subtick_half    = false;
    sleep_frame_div = 0;

    systick_init();
    systick_arm(SYSTICK_SLOT_INTERLEAVED);
}

void tick_dispatch(void)
{
    indicators_pre_update();
    matrix_scan_col(interleave_col);
    (void)indicators_update_step(&keyboard_state, interleave_col);

    if (++interleave_col >= MATRIX_COLS) {
        interleave_col = 0;
    }

    subtick_half = !subtick_half;
    if (!subtick_half) {
        settings_tick();
#    ifdef DYNAMIC_KEYMAP_ENABLE
        dynamic_keymap_tick();
#    endif
#    ifdef VIA_MACRO_ENABLE
        via_macro_tick();
#    endif
        indicators_post_update();
    }

    if (++sleep_frame_div >= 32u) {
        sleep_frame_div = 0;
        sleep_note_frame(true);
    }
}
#else
static volatile bool    scan_due;
static volatile uint8_t subframes_since_scan;

void tick_init(void)
{
    scan_due             = true;
    subframes_since_scan = 0;

    systick_init();
}

static void run_matrix_scan(void)
{
    systick_arm(SYSTICK_SLOT_MATRIX_SCAN);
    matrix_scan_full();
}

static void run_led_subframe(void)
{
    systick_arm(SYSTICK_SLOT_LED_SUBFRAME);

    indicators_pre_update();
    const bool frame_wrapped = indicators_update_step(&keyboard_state, 0);
    indicators_post_update();

    sleep_note_frame(frame_wrapped);
}

void tick_dispatch(void)
{
    settings_tick();
#ifdef DYNAMIC_KEYMAP_ENABLE
    dynamic_keymap_tick();
#endif
#ifdef VIA_MACRO_ENABLE
    via_macro_tick();
#endif
    if (scan_due) {
        scan_due             = false;
        subframes_since_scan = 0;
        run_matrix_scan();
        return;
    }

    run_led_subframe();

    if (++subframes_since_scan >= LED_SUBFRAMES_PER_SCAN) {
        scan_due = true;
    }
}
#endif

void tick_pause(void)
{
    systick_pause();
}

void tick_resume(void)
{
    systick_resume();
}
