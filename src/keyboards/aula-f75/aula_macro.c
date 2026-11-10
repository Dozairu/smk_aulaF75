#include "aula_macro.h"
#include "kbdef.h"
#include "keycodes.h"
#include "report.h"
#include "sh68f90.h"
#include "sleep.h"
#include <stdint.h>

/*
 * WAITING FOR FULL KEY RELEASE -- this is not a convenience feature.
 *
 * `Fn` is an `MO()` key: `process_key_state()` (src/smk/matrix.c) handles it
 * BEFORE calling `kb_process_record()` and returns early, so our hook never
 * sees it. Worse, releasing it calls `clear_keys()`, which wipes the keyboard
 * report without sending it. If the state machine started immediately upon
 * activation, the sequence `Fn v  Space v  Space ^  Fn ^` would wipe the report
 * right in the middle of the first letter.
 *
 * Hence the MACRO_WAIT state, entered TWICE: before the first keystroke, and
 * after a cancellation. It swallows all events until the matrix is completely
 * clear.
 *
 * `user_matrix_pressed()` (kbdef.h) returns this raw per-column state, already
 * masked to the six physical rows. Unlike the hook, it sees `Fn`: that is what
 * catches the "user presses only Fn" case while the mode is running.
 */
#define MACRO_OFF   0
#define MACRO_WAIT  1
#define MACRO_RUN   2
#define MACRO_FLUSH 3

#define STEP_KEY_DOWN 0
#define STEP_KEY_UP   1
#define STEP_BSP_DOWN 2
#define STEP_BSP_UP   3

/*
 * There is no free-running millisecond counter in this firmware. The LED
 * subframe is the only regular timebase: `src/smk/tick.c` alternates one matrix
 * scan with fifteen subframes, which amortizes the 400 us slot to ~420 us.
 * This is the same timebase already used by `aula_encoder.c`.
 *
 * The `uint32_t` intermediate is deliberate: `1500 * 1000` overflows a
 * `uint16_t`, and a slower rate requested in the future would silently
 * overflow. One second is ~2,381 subframes, so a 16-bit counter is required.
 */
#define MACRO_MS(ms) ((uint16_t)(((uint32_t)(ms) * 1000ul) / 420ul))

/*
 * The host polls the endpoint every millisecond (`bInterval = 1`). A press
 * shorter than one poll interval would be overwritten by its own release and
 * would NEVER be seen -- the classic pitfall of any synthetic event, which
 * lacks the natural duration of a finger on a key. Eight subframes come out to
 * ~3.4 ms, or at least three polls.
 */
#define MACRO_HOLD 8

#define MACRO_MIN  MACRO_MS(500)
#define MACRO_SPAN MACRO_MS(1000)

/*
 * Safeguard timeout for MACRO_WAIT. A stuck key would keep `matrix_busy()`
 * true forever: the state would never exit, and since it swallows EVERYTHING,
 * the keyboard would never type another letter until rebooted. After five
 * seconds we fall back to idle no matter what.
 *
 * Falling back to idle rather than starting: with a stuck key, starting would
 * loop endlessly between cancellation and restart. Accepted trade-off --
 * holding the activation combo for more than five seconds does nothing.
 */
#define MACRO_WAIT_MAX MACRO_MS(5000)

/* Decremented by the ISR, armed by the main loop.
 *
 * Placed in XDATA rather than internal RAM: this board only has seventeen bytes
 * of internal headroom versus over two thousand in XRAM, and the ISR already
 * accesses XRAM continuously for rendering. The overhead is one 16-bit
 * decrement per subframe, i.e. ~1 us out of a 400 us slot. Same trade-off as
 * `enc_state`/`enc_hist` in `aula_encoder.c`. */
static volatile __xdata uint16_t macro_wait;

/* The tick sets this bit rather than letting the main loop read the counter: a
 * bit test is atomic on 8051, whereas a 16-bit read is not. */
static volatile __bit macro_due;

static __xdata uint8_t  macro_state;
static __xdata uint8_t  macro_step;
static __xdata uint8_t  macro_letter;
static __xdata uint16_t macro_rnd;

/* MACRO_WAIT serves two purposes; this bit selects which state it transitions to. */
static __bit macro_go_after_wait;

void aula_macro_tick(void)
{
    if (macro_wait != 0) {
        macro_wait--;
        if (macro_wait == 0) {
            macro_due = 1;
        }
    }
}

bool aula_macro_active(void)
{
    return macro_state == MACRO_RUN;
}

static void macro_arm_timer(uint16_t subframes)
{
    ET2        = 0;
    macro_wait = subframes;
    macro_due  = 0;
    ET2        = 1;
}

static uint16_t macro_rand(void)
{
    macro_rnd ^= (uint16_t)(macro_rnd << 7);
    macro_rnd ^= (uint16_t)(macro_rnd >> 9);
    macro_rnd ^= (uint16_t)(macro_rnd << 8);
    return macro_rnd;
}

static bool matrix_busy(void)
{
    uint8_t col = MATRIX_COLS;

    while (col-- != 0) {
        if (user_matrix_pressed(col) != 0) {
            return true;
        }
    }
    return false;
}

static void macro_release_all(void)
{
    /* Without this, the current key remains held on the host side: there is no
     * physical finger to release it. */
    if (macro_letter != 0) {
        del_key(macro_letter);
        macro_letter = 0;
    }
    del_key((uint8_t)KC_BACKSPACE);
    send_keyboard_report();
}

static void macro_cancel(void)
{
    /*
     * A letter is ON SCREEN if it has been typed and not yet erased, i.e.,
     * between its key-down and the Backspace key-down -- which corresponds to
     * substates STEP_KEY_UP and STEP_BSP_DOWN, and only those. Once
     * STEP_BSP_DOWN has sent its press, the deletion is already in flight:
     * nothing left to clean up. And in STEP_KEY_DOWN nothing has been typed yet.
     *
     * Evaluated BEFORE `macro_release_all()`, which zeroes `macro_letter`.
     */
    const bool letter_pending = (macro_step == STEP_KEY_UP || macro_step == STEP_BSP_DOWN);

    macro_release_all();

    if (letter_pending) {
        /*
         * The exit Backspace CANNOT be sent from here in one shot: the host
         * only polls every millisecond, and a press immediately followed by its
         * release would be overwritten in the endpoint buffer without ever
         * being seen. It therefore goes through the state machine with the same
         * hold time as normal keystrokes -- preceded by a pause so the release
         * just queued by `macro_release_all()` goes out first.
         */
        macro_step  = 0;
        macro_state = MACRO_FLUSH;
        macro_arm_timer(MACRO_HOLD);
        return;
    }

    macro_arm_timer(MACRO_WAIT_MAX);
    macro_state         = MACRO_WAIT;
    macro_go_after_wait = 0;
}

void aula_macro_arm(void)
{
    if (macro_state != MACRO_OFF) {
        return;
    }

    /* Seed sampled on the fly from Timer2. The activation instant is chosen by
     * a human: that is the only entropy source on this MCU, and more than
     * enough for a draw with no cryptographic requirements. */
    macro_rnd = (uint16_t)(((uint16_t)TH2 << 8) | TL2);
    if (macro_rnd == 0) {
        macro_rnd = 0xA5A5u;
    }

    macro_letter        = 0;
    macro_state         = MACRO_WAIT;
    macro_go_after_wait = 1;
    macro_arm_timer(MACRO_WAIT_MAX);
}

bool aula_macro_intercept(bool pressed)
{
    if (macro_state == MACRO_OFF) {
        return false;
    }
    if (macro_state == MACRO_RUN && pressed) {
        macro_cancel();
    }
    return true;
}

void aula_macro_task(void)
{
    if (macro_state == MACRO_OFF) {
        return;
    }

    if (macro_state == MACRO_WAIT) {
        if (matrix_busy()) {
            if (!macro_due) {
                return;
            }
            macro_state = MACRO_OFF; /* stuck key: see MACRO_WAIT_MAX */
            return;
        }
        if (macro_go_after_wait) {
            macro_state = MACRO_RUN;
            macro_step  = STEP_KEY_DOWN;
            macro_due   = 1; /* fire first keystroke immediately */
        } else {
            macro_state = MACRO_OFF;
        }
        return;
    }

    if (macro_state == MACRO_FLUSH) {
        if (!macro_due) {
            return;
        }
        macro_due = 0;

        if (macro_step == 0) {
            add_key((uint8_t)KC_BACKSPACE);
            send_keyboard_report();
            macro_arm_timer(MACRO_HOLD);
            macro_step = 1;
        } else {
            del_key((uint8_t)KC_BACKSPACE);
            send_keyboard_report();
            macro_arm_timer(MACRO_WAIT_MAX);
            macro_state         = MACRO_WAIT;
            macro_go_after_wait = 0;
        }
        return;
    }

    /* `Fn` alone does not pass through `kb_process_record`: it gets caught
     * here, along with any other key the hook might have missed. */
    if (matrix_busy()) {
        macro_cancel();
        return;
    }

    if (!macro_due) {
        return;
    }
    macro_due = 0;

    /* Without this, the mode would stop on its own after ~2 min 15 s on
     * wireless: `sleep.c` enters sleep after 21,000 frames, and a frame is
     * fifteen subframes plus one matrix scan (~6.3 ms) -- NOT the ninety
     * subframes of an animation frame. Accepted trade-off: while the mode is
     * running, the keyboard never sleeps and drains the battery. */
    sleep_note_activity();

    switch (macro_step) {
        case STEP_KEY_DOWN:
            macro_letter = (uint8_t)((uint8_t)KC_A + (uint8_t)(macro_rand() % 26u));
            add_key(macro_letter);
            send_keyboard_report();
            macro_arm_timer(MACRO_HOLD);
            macro_step = STEP_KEY_UP;
            break;

        case STEP_KEY_UP:
            del_key(macro_letter);
            macro_letter = 0;
            send_keyboard_report();
            macro_arm_timer((uint16_t)(MACRO_MIN + (macro_rand() % MACRO_SPAN)));
            macro_step = STEP_BSP_DOWN;
            break;

        case STEP_BSP_DOWN:
            add_key((uint8_t)KC_BACKSPACE);
            send_keyboard_report();
            macro_arm_timer(MACRO_HOLD);
            macro_step = STEP_BSP_UP;
            break;

        default:
            del_key((uint8_t)KC_BACKSPACE);
            send_keyboard_report();
            macro_arm_timer((uint16_t)(MACRO_MIN + (macro_rand() % MACRO_SPAN)));
            macro_step = STEP_KEY_DOWN;
            break;
    }
}
