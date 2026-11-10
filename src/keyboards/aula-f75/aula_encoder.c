#include "aula_encoder.h"
#include "sh68f90.h"
#include "host.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Quadrature decoder, transcribed from `fcn.00007928`.
 *
 *   state = (P0 >> 5) & 3           two bits: P0.5 and P0.6
 *   hist  = (state & 3) | (hist << 2) & 0x3C
 *   0x0B, 0x34 -> one direction     0x07, 0x38 -> the other
 *
 * Both pins are already configured as inputs with pull-ups by the stock
 * setup replicated verbatim in `user_init.c`: `P0CR = 0x9C` leaves bits 5
 * and 6 cleared, and `P0PCR = 0xF8` sets them to 1. Nothing to configure here.
 *
 * The stock firmware sends Consumer usages `0xE9` and `0xEA` -- Volume Up and
 * Volume Down -- through buffer `0x09BD`. A persistent setting (block offset 26)
 * switches the knob to backlight brightness; its value in the dump is 0, so
 * volume. We follow the dump.
 */

#define ENC_VOL_UP   0x00E9
#define ENC_VOL_DOWN 0x00EA

/*
 * The host polls the endpoint every millisecond (`bInterval = 1`). A press
 * shorter than one poll interval would be overwritten by its own release and
 * would NEVER be seen -- the classic pitfall of any synthetic event, which
 * lacks the natural duration of a finger on a key.
 *
 * Counted in LED subframes, i.e., ~420 us each once matrix scans are amortized:
 * eight ticks held gives ~3.4 ms, four ticks of silence ~1.7 ms. One detent
 * therefore takes ~5 ms, capping at two hundred detents per second -- well
 * beyond the reach of a physical knob.
 */
#define ENC_GAP  4
#define ENC_HOLD (ENC_GAP + 8)

/*
 * KNOB HEARTBEAT.
 *
 * The pattern is the same heartbeat as auto-type, but its PACE follows the
 * rotation direction: turning up speeds it up, turning down slows it down. This
 * is the only possible feedback for this action -- the host never tells the
 * keyboard its current volume, so the displayed level is RELATIVE, not the
 * actual host volume.
 *
 * The eight rates form a geometric progression with a ratio of ~1.159: the ear
 * and eye perceive tempo as a ratio rather than a difference, so steps spaced
 * equally in ratio read as equally spaced.
 *
 * 256 = one pattern step per scan wrap (~37.9 ms). A full cycle is 24 steps,
 * so cycle_ms = 24 x 256 x 37.9 / rate: the table spans from 2.8 s at the
 * lowest setting to 1.0 s at the highest.
 */
#define ENC_BEAT_LEVELS 8
#define ENC_BEAT_START  3

/* ~1.5 s after the last detent, in ~420 us subframes. */
#define ENC_BEAT_HOLD 3570u

static const __code uint16_t enc_beat_rates[ENC_BEAT_LEVELS] = {
    83, 96, 111, 129, 150, 174, 201, 233,
};

/* Touched only from the ISR -- `aula_encoder_sample()` writes them, the LED
 * renderer reads them, and both live inside the Timer2 ISR. No critical section
 * needed, and no 16-bit read can straddle a write. */
static __xdata uint8_t  beat_lvl = ENC_BEAT_START;
static __xdata uint16_t beat_to;

static volatile __data uint8_t enc_hold;
static volatile __data int8_t  enc_pending;
static volatile __bit          enc_release_due;

/* In XDATA: these two are only touched by the ISR, which already accesses XRAM
 * for rendering. Internal RAM only has nineteen bytes of headroom on this
 * board, which are better spent elsewhere. */
static __xdata uint8_t enc_state;
static __xdata uint8_t enc_hist;

void aula_encoder_sample(void)
{
    uint8_t now;

    if (beat_to != 0) {
        beat_to--;
    }

    if (enc_hold != 0) {
        enc_hold--;
        if (enc_hold == ENC_GAP) {
            enc_release_due = 1;
        }
    }

    now = (uint8_t)((P0 >> 5) & 0x03u);
    if (now == enc_state) {
        return;
    }
    enc_hist  = (uint8_t)((now & 0x03u) | (uint8_t)((enc_hist << 2) & 0x3Cu));
    enc_state = now;

    /*
     * Detents are queued rather than dropped when the main loop is slower than
     * the knob. The cap prevents a noisy scan from filling the counter and
     * scrolling the volume on its own.
     */
    if (enc_hist == 0x0B || enc_hist == 0x34) {
        if (enc_pending < 8) enc_pending++;
        if (beat_lvl < (uint8_t)(ENC_BEAT_LEVELS - 1)) beat_lvl++;
        beat_to = ENC_BEAT_HOLD;
    } else if (enc_hist == 0x07 || enc_hist == 0x38) {
        if (enc_pending > -8) enc_pending--;
        if (beat_lvl != 0) beat_lvl--;
        beat_to = ENC_BEAT_HOLD;
    }
}

bool aula_encoder_beating(void)
{
    return beat_to != 0;
}

uint16_t aula_encoder_beat_rate(void)
{
    return enc_beat_rates[beat_lvl];
}

uint8_t aula_encoder_beat_level(void)
{
    return beat_lvl;
}

void aula_encoder_task(void)
{
    int8_t pending;

    if (enc_release_due) {
        enc_release_due = 0;
        host_consumer_send(0);
        return;
    }
    if (enc_hold != 0) {
        return;
    }

    /* `enc_pending` is incremented by the ISR and decremented here: the
     * read-modify-write must be protected, otherwise a detent is lost when
     * both happen together. */
    ET2     = 0;
    pending = enc_pending;
    if (pending > 0) {
        enc_pending--;
    } else if (pending < 0) {
        enc_pending++;
    }
    ET2 = 1;

    if (pending > 0) {
        host_consumer_send(ENC_VOL_UP);
        enc_hold = ENC_HOLD;
    } else if (pending < 0) {
        host_consumer_send(ENC_VOL_DOWN);
        enc_hold = ENC_HOLD;
    }
}
