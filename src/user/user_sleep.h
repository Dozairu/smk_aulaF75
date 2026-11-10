#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USER_SLEEP_NONE = 0,
    USER_SLEEP_RF   = 1,
    USER_SLEEP_USB  = 2,
} user_sleep_mode_t;

user_sleep_mode_t user_sleep_supported(void);
void              user_sleep_prepare(void);
void              user_sleep_wake(void);

/*
 * Is the board ready to enter sleep right now?
 *
 * `sleep_task()` stops the tick and PWM before calling `user_sleep_prepare()`,
 * so nothing is displayed from that point on: a board that wants to signal
 * sleep entry (e.g., fading out an indicator LED) must do so BEFORE then.
 * Returning `false` delays sleep by one main-loop pass while the display
 * engine continues running.
 *
 * `user_sleep_cancel()` is called on every pass where sleep is NOT due,
 * allowing a board to cleanly abort any pre-sleep animation if user activity
 * interrupts the transition.
 *
 * Boards without pre-sleep animation needs rely on the defaults in `src/user/`.
 */
bool user_sleep_ready(void);
void user_sleep_cancel(void);
