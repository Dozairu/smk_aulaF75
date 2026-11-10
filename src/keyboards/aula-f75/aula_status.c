#include "aula_status.h"
#include "sh68f90.h"
#include <stdint.h>

/*
 * Patterns, one intensity byte per step.
 *
 * ONE STEP = ONE WRAP OF THE REFRESH SCAN, i.e. ~37.9 ms. The engine does not
 * step any faster, and that is not a matter of convenience: the ninety cells
 * are repainted ONE PER SUB-FRAME, so changing the value anywhere other than at
 * wrap-around would land in the middle of a repaint cycle. Same constraint as
 * the auto-type core, same solution.
 *
 *   AULA_STATUS_WIRED        1 long flash           16 steps  0.61 s
 *   AULA_STATUS_24G          2 short flashes        17 steps  0.64 s
 *   AULA_STATUS_BT           3 short flashes        28 steps  1.06 s
 *   AULA_STATUS_PAIRING      fast blink              8 steps  0.30 s  (loops)
 *   AULA_STATUS_LINKED       single breath          24 steps  0.91 s
 *   AULA_STATUS_LOST         2 long flashes         32 steps  1.21 s
 *   AULA_STATUS_SLEEP_IN     fade out               21 steps  0.80 s
 *   AULA_STATUS_SLEEP_OUT    fade in                11 steps  0.42 s
 *   AULA_STATUS_LOW_BATT     slow pulse             40 steps  1.52 s  (loops)
 *   AULA_STATUS_AZERTY_ON    4 short flashes        24 steps  0.91 s
 *   AULA_STATUS_AZERTY_OFF   1 short flash           5 steps  0.19 s
 *   AULA_STATUS_AZERTY       steady dim glow         1 step           (loops)
 *
 * Everything fits in a single flat array with offset and length tables: an
 * array of `__code` pointers would cost more flash and an extra indirection on
 * every step, for nothing.
 */
static const __code uint8_t pat_data[227] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255,   0,   0,
      0,   0,   0, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255,   0,   0,   0,   0,   0, 255, 255, 255, 255,
    255, 255,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255,   0,   0,   0,   0,   0,  12,  30,
     55,  85, 120, 155, 190, 220, 240, 255, 255, 255, 240, 220,
    190, 155, 120,  85,  55,  30,  12,   0,   0, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255,   0,   0,   0,
      0,   0,   0,   0,   0, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 242, 228, 214, 200, 185, 170,
    155, 140, 124, 108,  92,  76,  62,  48,  36,  26,  17,  10,
      4,   0,   0,  20,  50,  90, 135, 180, 215, 240, 255, 255,
      0,   0,   4,  10,  18,  28,  40,  52,  60,  64,  60,  52,
     40,  28,  18,  10,   4,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,
    /* AZERTY_ON: four short flashes, 3 steps on / 3 steps off. */
    255, 255, 255,   0,   0,   0, 255, 255, 255,   0,   0,   0,
    255, 255, 255,   0,   0,   0, 255, 255, 255,   0,   0,   0,
    /* AZERTY_OFF: a single short flash. */
    255, 255, 255,   0,   0,
    /* AZERTY: a single looping step, hence a steady glow. Dim enough not to
     * be mistaken for a flash, bright enough to be visible in the dark. */
     40,
};

static const __code uint8_t pat_off[12] = {0,  16, 33,  61,  69,  93,
                                           125, 146, 157, 197, 221, 226,};
static const __code uint8_t pat_len[12] = {16, 17, 28, 8, 24, 32, 21, 11, 40, 24, 5, 1,};

/* Bit N set: pattern N loops instead of ending. Twelve patterns, hence
 * twelve bits: the mask does NOT fit in a single byte. */
#define PAT_LOOPING 0x0908u

#define PAT_LOOPS(p) ((PAT_LOOPING & (uint16_t)(1u << (p))) != 0u)

static __xdata uint8_t st_pat = AULA_STATUS_NONE;
static __xdata uint8_t st_pos;

/* Edge tracking for `aula_status_poll()`. `0xFF` forces the transport to be
 * announced on the very first pass: the keyboard reports what it boots into. */
static __xdata uint8_t prev_link = 0xFF;
static __bit           prev_conn;
static __bit           prev_low;
static __bit           low_batt;
static __bit           az_rest;

void aula_status_event(uint8_t event)
{
    /* `st_pat` and `st_pos` are read by the ISR on every sub-frame: writing
     * them non-atomically would sample a step from the wrong pattern. */
    ET2    = 0;
    st_pat = event;
    st_pos = 0;
    ET2    = 1;
}

/* What remains displayed when no one-shot pattern is playing. Priority is
 * documented in `aula_status.h`: two resting states on a single white LED
 * cannot be told apart, so one must be chosen. */
static void status_rest(void)
{
    if (low_batt) {
        st_pat = AULA_STATUS_LOW_BATT;
    } else if (az_rest) {
        st_pat = AULA_STATUS_AZERTY;
    } else {
        st_pat = AULA_STATUS_NONE;
    }
    st_pos = 0;
}

void aula_status_set_azerty(bool on)
{
    az_rest = on ? 1 : 0;
    if (!aula_status_busy()) {
        /* The toggle pattern is posted right after by `aula_layout.c`; this
         * resting state is what it will fall back to when it finishes. */
        ET2 = 0;
        status_rest();
        ET2 = 1;
    }
}

bool aula_status_lit(void)
{
    return st_pat != AULA_STATUS_NONE;
}

bool aula_status_busy(void)
{
    const uint8_t p = st_pat;

    if (p == AULA_STATUS_NONE) {
        return false;
    }
    return !PAT_LOOPS(p);
}

void aula_status_tick(void)
{
    const uint8_t p = st_pat;

    if (p == AULA_STATUS_NONE) {
        return;
    }
    if (++st_pos < pat_len[p]) {
        return;
    }
    st_pos = 0;
    if (!PAT_LOOPS(p)) {
        status_rest();
    }
}

uint8_t aula_status_level(void)
{
    const uint8_t p = st_pat;

    if (p == AULA_STATUS_NONE) {
        return 0;
    }
    return pat_data[(uint8_t)(pat_off[p] + st_pos)];
}

void aula_status_poll(uint8_t link, bool connected, bool low)
{
    /* Low battery does not trigger a one-shot pattern: it is a STATE that
     * holds the indicator at rest and yields to any event. */
    if (low != prev_low) {
        prev_low = low;
        low_batt = low;
        if (!aula_status_busy()) {
            status_rest();
        }
    }

    if (link != prev_link) {
        prev_link = link;
        /* `rf_link()` returns 0 for wired, 1 for 2.4 GHz, 2 for Bluetooth --
         * the exact same order as the first three patterns, by design. */
        aula_status_event((link <= AULA_STATUS_BT) ? link : (uint8_t)AULA_STATUS_WIRED);
        prev_conn = connected;
        return;
    }

    if (connected != prev_conn) {
        prev_conn = connected;
        aula_status_event(connected ? (uint8_t)AULA_STATUS_LINKED
                                    : (uint8_t)AULA_STATUS_LOST);
    }
}
