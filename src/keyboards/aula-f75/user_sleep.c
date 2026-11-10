#include "kbdef.h"
#include "user_sleep.h"
#include "user_init.h"
#include "user_matrix.h"
#include "gpio.h"
#include "extint.h"
#include "aula_status.h"

#ifdef RF_EUART0
#    include "aula_rf.h"
#endif

#ifdef SLEEP_ENABLE

/*
 * Sleep -- transcribed from the factory firmware sequence.
 *
 *   fcn @ 0x006E  panel parking (rows as inputs + pull-up, columns low)
 *   0x7DCB-0x7DF1 parking of off-matrix pins
 *   0x7DF4        EXF1 = 0            (extint_wake_clear)
 *   0x7DF6        IENC = 0xF3         (identical to platform/sh68f90/extint.c)
 *   0x7DF9        EXF0 = 0x40         (ditto)
 *   0x7DFC        IEN0 |= 0x02        (EX4 = 1)
 *   0x7E34        PCON |= 0x02        (power-down)
 *
 * Wake mechanism: all columns are held LOW and rows are inputs with pull-ups.
 * Pressing any key pulls a row to 0, triggering INT4.
 *
 * The write order matches the factory firmware and must remain so: driving pins
 * low precedes switching them to outputs.
 *
 * WARNING: Unverified on hardware. Incorrect parking means a keyboard that never
 * wakes up and must be reflashed.
 */

/*
 * The mode tracks the selector live, as on the NuPhy Air60:
 * `sleep_task()` queries this hook on EVERY pass.
 *
 * The distinction is not cosmetic. In `USER_SLEEP_USB`, sleep entry is
 * triggered by bus suspend; in `USER_SLEEP_RF`, it is triggered by the
 * inactivity counter in `sleep.c` -- and outside RF mode that counter is reset
 * to zero on every pass, making it a no-op. Hardcoding `USB`, as this file
 * previously did, meant having no sleep at all on battery.
 */
user_sleep_mode_t user_sleep_supported(void)
{
#ifdef RF_EUART0
    return rf_is_wireless() ? USER_SLEEP_RF : USER_SLEEP_USB;
#else
    return USER_SLEEP_USB;
#endif
}

static void park_panel(void)
{
    /* Rows: input + pull-up (fcn @ 0x006E, 0x006E-0x0091) */
    GPIO_INPUT(7, KB_R_P7_MASK);
    GPIO_PULLUP_ON(7, KB_R_P7_MASK);
    GPIO_INPUT(5, KB_R_P5_MASK);
    GPIO_PULLUP_ON(5, KB_R_P5_MASK);

    /* Columns and auxiliary pins: drive low BEFORE switching to output
     * (0x0092-0x00B9), then set output direction (0x00BC onwards). */
    GPIO_LOW(6, KB_C_P6_MASK);
    GPIO_LOW(5, KB_C_P5_MASK);
    GPIO_LOW(4, (uint8_t)(KB_C_P4_MASK | _P4_5 | _P4_6));
    GPIO_LOW(7, (uint8_t)_P7_4);
    GPIO_LOW(0, (uint8_t)(_P0_5 | _P0_6 | _P0_7));

    GPIO_OUTPUT(6, KB_C_P6_MASK);
    GPIO_OUTPUT(5, KB_C_P5_MASK);
    GPIO_OUTPUT(4, (uint8_t)(KB_C_P4_MASK | _P4_5 | _P4_6));

    /* Off-matrix pins (0x7DCB-0x7DF1) */
    GPIO_PULLUP_OFF(0, (uint8_t)(_P0_0 | _P0_1));
    GPIO_OUTPUT(0, (uint8_t)(_P0_0 | _P0_1));
    GPIO_LOW(0, (uint8_t)(_P0_0 | _P0_1));

    GPIO_PULLUP_OFF(0, (uint8_t)(_P0_5 | _P0_6)); /* encoder phases */
    GPIO_OUTPUT(0, (uint8_t)(_P0_5 | _P0_6));
    GPIO_LOW(0, (uint8_t)(_P0_5 | _P0_6));

    GPIO_PULLUP_OFF(7, (uint8_t)(_P7_4 | _P7_7));
    GPIO_OUTPUT(7, (uint8_t)(_P7_4 | _P7_7));
    GPIO_LOW(7, (uint8_t)(_P7_4 | _P7_7));

    GPIO_PULLUP_OFF(4, (uint8_t)_P4_5);
    GPIO_OUTPUT(4, (uint8_t)_P4_5);
    GPIO_LOW(4, (uint8_t)_P4_5);

    P0_7 = 1;
    P7_6 = 0;
}

void user_sleep_prepare(void)
{
#ifdef RF_EUART0
    rf_sleep_prepare(); /* drains TX then disables EUART0, like 0x7D74 */
#endif
    park_panel();
    extint_wake_arm(); /* IENC=0xF3, EXF0=0x40, EX4=1 -- same values as factory */
}

/*
 * The sleep animation must play BEFORE `sleep_task()` shuts down the tick
 * and PWM -- afterwards, nothing can be displayed. Hence these three states
 * rather than a simple flag: `ARMED` starts the fade-out and holds off sleep,
 * `DONE` lets it proceed, and `user_sleep_cancel()` resets to `IDLE` as soon
 * as sleep is no longer due, catching the case where the user interrupts sleep
 * entry during the fade-out.
 */
#define SLEEP_ANIM_IDLE  0
#define SLEEP_ANIM_ARMED 1
#define SLEEP_ANIM_DONE  2

static __xdata uint8_t sleep_anim;

bool user_sleep_ready(void)
{
    if (sleep_anim == SLEEP_ANIM_IDLE) {
        aula_status_event(AULA_STATUS_SLEEP_IN);
        sleep_anim = SLEEP_ANIM_ARMED;
        return false;
    }
    if (sleep_anim == SLEEP_ANIM_ARMED) {
        if (aula_status_busy()) {
            return false;
        }
        sleep_anim = SLEEP_ANIM_DONE;
    }
    return true;
}

void user_sleep_cancel(void)
{
    sleep_anim = SLEEP_ANIM_IDLE;
}

void user_sleep_wake(void)
{
    sleep_anim = SLEEP_ANIM_IDLE;
    aula_status_event(AULA_STATUS_SLEEP_OUT);

    extint_wake_disable();
    user_gpio_init();
#ifdef RF_EUART0
    /*
     * AFTER `user_gpio_init()`, not before: that function hands P7.4 and P4.5
     * back to the selector (P4CR=0x4D, P7CR=0x60 leave them as inputs with
     * pull-ups), pins that `park_panel()` held as low outputs during sleep --
     * just like the factory firmware at 0x7DCB-0x7DF1.
     */
    rf_sleep_wake();
#endif
}

#endif // SLEEP_ENABLE
