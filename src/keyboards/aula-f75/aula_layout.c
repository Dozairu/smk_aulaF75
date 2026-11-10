#include "aula_layout.h"
#include "aula_status.h"
#include "sh68f90.h"
#include "report.h"
#include "keycodes.h"
#include "settings.h"
#include <stdint.h>

/*
 * MODE PERSISTENCE.
 *
 * `user_settings.ul_speed` is unused on this keyboard: the shared struct
 * reserves it for underglow speed, which the F75 lacks, and its only consumer
 * is the NuPhy Air60. We store the layout mode there rather than growing
 * `user_settings_t` -- which would invalidate ALL saved settings on first boot,
 * since `nvm.c` checks the block length.
 *
 * Same trade-off and reasoning as `AULA_SETTINGS_BT_SLOT` in `aula_rf.c`.
 * The alias name ensures nobody reads `ul_speed` expecting a speed value.
 */
#define AULA_SETTINGS_AZ user_settings.ul_speed

/*
 * Translated range: `KC_A` through `KC_SLASH`, the only keycodes that produce
 * characters. Everything else passes through untouched -- arrows, F1-F12,
 * navigation cluster, modifiers, and all `_FN` layer keycodes, which are either
 * above `0x38` or past `SAFE_RANGE`.
 */
#define AZ_FIRST KC_A
#define AZ_LAST  KC_SLASH
#define AZ_N     (AZ_LAST - AZ_FIRST + 1)

#define AZ_SH  0x01u /* inject Shift */
#define AZ_AG  0x02u /* inject AltGr */
#define AZ_ACU 0x04u /* acute accent: dead key held by firmware */
#define AZ_DIA 0x08u /* diaeresis   : dead key held by firmware */
#define AZ_DEAD (AZ_ACU | AZ_DIA)
#define AZ_RAW 0x80u /* neither inject NOR mask -- shortcut path */

/* Both Shifts. The mask strips them from `real_mods`: on an AZERTY host, `!`
 * is unshifted whereas the keycap has it on Shift+1, and without masking the
 * key would output `1`. */
#define AZ_SHIFT_MODS ((uint8_t)MODS_SHIFT_MASK)

/* Any modifier OTHER than Shift: Ctrl, Alt, AltGr, GUI, on either side. */
#define AZ_OTHER_MODS ((uint8_t)~AZ_SHIFT_MODS)

typedef struct {
    uint8_t kc;
    uint8_t fl;
} az_ent_t;

/*
 * Translation table to French AZERTY (France), Windows.
 *
 * Indexed by `US keycode - KC_A`, then by Shift state. Each line's comment
 * shows what is PRINTED on the keycap, unshifted then shifted: that is the
 * desired output character, not the emitted keycode.
 *
 * Letters are nearly an identity map -- only `a<->q`, `z<->w`, and `m` move.
 * The real work is on the number row, where AZERTY puts symbols UNSHIFTED:
 * Shift must be injected to produce a digit.
 */
static const __code az_ent_t az_map[AZ_N][2] = {
    /* a    A    */ {{0x14, 0}, {0x14, AZ_SH}},
    /* b    B    */ {{0x05, 0}, {0x05, AZ_SH}},
    /* c    C    */ {{0x06, 0}, {0x06, AZ_SH}},
    /* d    D    */ {{0x07, 0}, {0x07, AZ_SH}},
    /* e    E    */ {{0x08, 0}, {0x08, AZ_SH}},
    /* f    F    */ {{0x09, 0}, {0x09, AZ_SH}},
    /* g    G    */ {{0x0A, 0}, {0x0A, AZ_SH}},
    /* h    H    */ {{0x0B, 0}, {0x0B, AZ_SH}},
    /* i    I    */ {{0x0C, 0}, {0x0C, AZ_SH}},
    /* j    J    */ {{0x0D, 0}, {0x0D, AZ_SH}},
    /* k    K    */ {{0x0E, 0}, {0x0E, AZ_SH}},
    /* l    L    */ {{0x0F, 0}, {0x0F, AZ_SH}},
    /* m    M    */ {{0x33, 0}, {0x33, AZ_SH}},
    /* n    N    */ {{0x11, 0}, {0x11, AZ_SH}},
    /* o    O    */ {{0x12, 0}, {0x12, AZ_SH}},
    /* p    P    */ {{0x13, 0}, {0x13, AZ_SH}},
    /* q    Q    */ {{0x04, 0}, {0x04, AZ_SH}},
    /* r    R    */ {{0x15, 0}, {0x15, AZ_SH}},
    /* s    S    */ {{0x16, 0}, {0x16, AZ_SH}},
    /* t    T    */ {{0x17, 0}, {0x17, AZ_SH}},
    /* u    U    */ {{0x18, 0}, {0x18, AZ_SH}},
    /* v    V    */ {{0x19, 0}, {0x19, AZ_SH}},
    /* w    W    */ {{0x1D, 0}, {0x1D, AZ_SH}},
    /* x    X    */ {{0x1B, 0}, {0x1B, AZ_SH}},
    /* y    Y    */ {{0x1C, 0}, {0x1C, AZ_SH}},
    /* z    Z    */ {{0x1A, 0}, {0x1A, AZ_SH}},
    /* 1    !    */ {{0x1E, AZ_SH}, {0x38, 0}},
    /* 2    @    */ {{0x1F, AZ_SH}, {0x27, AZ_AG}},
    /* 3    #    */ {{0x20, AZ_SH}, {0x20, AZ_AG}},
    /* 4    $    */ {{0x21, AZ_SH}, {0x30, 0}},
    /* 5    %    */ {{0x22, AZ_SH}, {0x34, AZ_SH}},
    /* 6    ^    */ {{0x23, AZ_SH}, {0x2F, 0}},
    /* 7    &    */ {{0x24, AZ_SH}, {0x1E, 0}},
    /* 8    *    */ {{0x25, AZ_SH}, {0x31, 0}},
    /* 9    (    */ {{0x26, AZ_SH}, {0x22, 0}},
    /* 0    )    */ {{0x27, AZ_SH}, {0x2D, 0}},
    /* Entr Entr */ {{0x28, 0}, {0x28, 0}},
    /* Esc  Esc  */ {{0x29, 0}, {0x29, 0}},
    /* Bspc Bspc */ {{0x2A, 0}, {0x2A, 0}},
    /* Tab  Tab  */ {{0x2B, 0}, {0x2B, 0}},
    /* Spc  Spc  */ {{0x2C, 0}, {0x2C, 0}},
    /* -    _    */ {{0x23, 0}, {0x25, 0}},
    /* =    +    */ {{0x2E, 0}, {0x2E, AZ_SH}},
    /* [    {    */ {{0x22, AZ_AG}, {0x21, AZ_AG}},
    /* ]    }    */ {{0x2D, AZ_AG}, {0x2E, AZ_AG}},
    /* \    |    */ {{0x25, AZ_AG}, {0x23, AZ_AG}},
    /* ---  ---  */ {{0x32, 0}, {0x32, 0}},
    /* ;    :    */ {{0x36, 0}, {0x37, 0}},
    /* '    "    */ {{0x21, AZ_ACU}, {0x20, AZ_DIA}},
    /* `    ~    */ {{0x24, AZ_AG}, {0x1F, AZ_AG}},
    /* ,    <    */ {{0x10, 0}, {0x64, 0}},
    /* .    >    */ {{0x36, AZ_SH}, {0x64, AZ_SH}},
    /* /    ?    */ {{0x37, AZ_SH}, {0x10, AZ_SH}},
};

/*
 * Timebase: the LED subframe, ~420 us once matrix scans are amortized.
 * Same as `aula_encoder.c` and `aula_macro.c`, for the same reason -- there is
 * no free-running millisecond counter in this firmware.
 */
#define AZ_MS(ms) ((uint16_t)(((uint32_t)(ms) * 1000ul) / 420ul))

/* The host polls every millisecond (`bInterval = 1`): a press followed by its
 * release in the same pass would NEVER be seen. Eight subframes come out to
 * ~3.4 ms, or at least three polls. Same value as `ENC_HOLD`. */
#define AZ_HOLD 8
#define AZ_GAP  4

/*
 * Dead-key timeout safeguard. US International has no timeout, but a forgotten
 * `'` would swallow the next keystroke -- hours later. After three seconds, the
 * standalone symbol is emitted.
 */
#define AZ_DEAD_MAX AZ_MS(3000)

/* Output queue. Power of two: pointer advances via bitmask. */
#define AZ_Q 4

/*
 * Tracking of held translated keys. Used for more than just counting: releasing
 * a key must remove the ACTUALLY emitted keycode from the report. Without this,
 * releasing Shift before the key (`Shift+1` produces `!` = `KC_SLASH`, then
 * releasing the key would see the unshifted state and remove `KC_1`) would
 * leave a stuck key.
 */
#define AZ_T 8

static __bit az_on;

static __xdata uint8_t az_t_us[AZ_T]; /* held US keycode, 0 = free */
static __xdata uint8_t az_t_kc[AZ_T]; /* actually emitted keycode */
static __xdata uint8_t az_down;
static __xdata uint8_t az_hold_fl = AZ_RAW;

static __xdata uint8_t az_q_kc[AZ_Q];
static __xdata uint8_t az_q_fl[AZ_Q];
static __xdata uint8_t az_q_head;
static __xdata uint8_t az_q_tail;
static __xdata uint8_t az_play; /* 0 idle, 1 press active, 2 gap */
static __xdata uint8_t az_cur;

/* Decremented by the ISR, armed by the main loop. Placed in XDATA: this board
 * only has seventeen bytes of internal RAM headroom, and the ISR already
 * accesses XRAM continuously for rendering. */
static volatile __xdata uint16_t az_wait;
static volatile __xdata uint16_t az_dead_to;

/* The ISR raises these bits instead of letting the main loop read the counters:
 * a bit test is atomic on 8051, whereas a 16-bit read is not -- and a word read
 * straddling a decrement could falsely read as zero. */
static volatile __bit az_due;
static volatile __bit az_dead_due;

/* Pending dead key: 0, AZ_ACU, or AZ_DIA -- the exact flag values from the
 * table itself, so no translation is needed between them. */
static __xdata uint8_t az_dead;

static void az_mods_apply(uint8_t fl)
{
    uint8_t w;

    if (fl & AZ_RAW) {
        set_mods_mask(0xFF);
        set_weak_mods(0);
        return;
    }

    set_mods_mask((uint8_t)~AZ_SHIFT_MODS);

    w = 0;
    if (fl & AZ_SH) w |= (uint8_t)MOD_BIT(KC_LEFT_SHIFT);
    if (fl & AZ_AG) w |= (uint8_t)MOD_BIT(KC_RIGHT_ALT);
    set_weak_mods(w);
}

/* At-rest modifier injection uses that of the LAST key press, restored to raw
 * once the last translated key is released. */
static void az_mods_rest(void)
{
    az_mods_apply(az_down != 0 ? az_hold_fl : (uint8_t)AZ_RAW);
}

static void az_arm(uint16_t n)
{
    az_due = 0;
    ET2    = 0;
    az_wait = n;
    ET2    = 1;
}

static void az_push(uint8_t kc, uint8_t fl)
{
    const uint8_t next = (uint8_t)((az_q_tail + 1u) & (AZ_Q - 1u));

    /* Queue full: drop the character rather than overwriting the one currently
     * playing. Reaching this would require typing over two hundred characters
     * per second -- the queue is only used for dead-key resolutions. */
    if (next == az_q_head) {
        return;
    }
    az_q_kc[az_q_tail] = kc;
    az_q_fl[az_q_tail] = (uint8_t)(fl & (AZ_SH | AZ_AG));
    az_q_tail          = next;
}

static bool az_track_add(uint8_t us, uint8_t kc)
{
    for (uint8_t i = 0; i < AZ_T; i++) {
        if (az_t_us[i] == 0) {
            az_t_us[i] = us;
            az_t_kc[i] = kc;
            az_down++;
            return true;
        }
    }
    return false;
}

static uint8_t az_track_del(uint8_t us)
{
    for (uint8_t i = 0; i < AZ_T; i++) {
        if (az_t_us[i] == us) {
            const uint8_t kc = az_t_kc[i];
            az_t_us[i] = 0;
            if (az_down != 0) az_down--;
            return kc;
        }
    }
    return 0;
}

static void az_reset(void)
{
    az_arm(0);
    ET2        = 0;
    az_dead_to = 0;
    ET2        = 1;
    az_dead_due = 0;
    az_dead     = 0;

    az_q_head = 0;
    az_q_tail = 0;
    az_play   = 0;

    for (uint8_t i = 0; i < AZ_T; i++) {
        az_t_us[i] = 0;
    }
    az_down    = 0;
    az_hold_fl = AZ_RAW;
    az_mods_apply(AZ_RAW);

    /* Any translated key still held would remain in the report with the old
     * layout's keycode: start fresh with an empty report. */
    clear_keys();
    send_keyboard_report();
}

void aula_layout_toggle(void)
{
    az_on = !az_on;
    az_reset();

    AULA_SETTINGS_AZ = az_on ? 1u : 0u;
    settings_mark_dirty(); /* `settings_task()` writes flash on the next pass */

    aula_status_set_azerty(az_on ? true : false);
    aula_status_event(az_on ? (uint8_t)AULA_STATUS_AZERTY_ON
                            : (uint8_t)AULA_STATUS_AZERTY_OFF);
}

void aula_layout_restore(bool on)
{
    az_on = on ? 1 : 0;
    aula_status_set_azerty(on);
    /* No toggle animation at boot: the steady glow suffices to show which mode
     * the keyboard woke up in, and the transport announcement would override it
     * anyway. */
}

void aula_layout_euro(void)
{
    az_push(KC_E, AZ_AG);
}

bool aula_layout_intercept(uint16_t qcode, bool pressed)
{
    uint8_t idx, kc, fl, mods;

    if (!az_on) {
        return false;
    }
    if (qcode < AZ_FIRST || qcode > AZ_LAST) {
        return false;
    }
    idx = (uint8_t)(qcode - AZ_FIRST);

    if (!pressed) {
        /* Remove the ACTUALLY emitted keycode. If it wasn't tracked -- played
         * via the queue, or pressed before the mode was enabled -- remove the
         * raw keycode: harmless if it isn't in the report. */
        kc = az_track_del((uint8_t)qcode);
        del_key(kc != 0 ? kc : (uint8_t)qcode);
        az_mods_rest();
        send_keyboard_report();
        return true;
    }

    mods = get_mods();

    /*
     * The decision is made on `real_mods`, the PHYSICAL state, and before any
     * write: this mode itself injects AltGr, which Windows sees as Ctrl+Alt,
     * so a guard inspecting the outgoing report would trigger on its own
     * injection on the next keystroke.
     */
    if (mods & AZ_OTHER_MODS) {
        /* Shortcut: translate the letter position, neither inject nor mask
         * modifiers. `Ctrl+A` becomes `Ctrl+KC_Q`, `Ctrl+1` stays `Ctrl+1`. */
        az_dead = 0;
        kc      = az_map[idx][0].kc;
        if (!az_track_add((uint8_t)qcode, kc)) {
            return false;
        }
        az_hold_fl = AZ_RAW;
        az_mods_apply(AZ_RAW);
        add_key(kc);
        send_keyboard_report();
        return true;
    }

    {
        const uint8_t st = (mods & AZ_SHIFT_MODS) ? 1u : 0u;
        kc = az_map[idx][st].kc;
        fl = az_map[idx][st].fl;

        if (az_dead != 0) {
            const uint8_t d = az_dead;

            az_dead     = 0;
            ET2         = 0;
            az_dead_to  = 0;
            ET2         = 1;
            az_dead_due = 0;

            if (d == AZ_DIA) {
                /*
                 * French fr-FR has a REAL diaeresis dead key -- Shift + `KC_LBRC`
                 * -- and the host handles composition. However, its diaeresis
                 * followed by space outputs `¨`, NOT `"`: it is the only one of
                 * the host's four dead keys whose standalone character differs
                 * from US International, and thus the only one that cannot be a
                 * simple pass-through. Verified on hardware.
                 */
                /* The lower bound is already guaranteed: the function returns
                 * early above for any `qcode` below `AZ_FIRST`, which equals
                 * `KC_A`. Re-testing it would cause the optimizer to elide the
                 * check, which `--Werror` turns into an error (SDCC 110). */
                if (qcode <= KC_Z) {
                    az_push(0x2Fu, AZ_SH); /* dead diaeresis: host composes */
                    az_push(kc, fl);
                } else if (qcode == KC_SPACE) {
                    az_push(0x20u, 0); /* `"` -- unshifted on AZERTY */
                } else {
                    az_push(0x20u, 0);
                    az_push(kc, fl);
                }
                return true;
            }

            /* French fr-FR has NO acute accent dead key: `é` and `ç` are
             * dedicated keys, and `á í ó ú ý` cannot be produced. For everything
             * else, fall back to US International behavior -- apostrophe
             * followed by the character. */
            if (st == 0 && qcode == KC_E) {
                az_push(0x1Fu, 0); /* é */
            } else if (st == 0 && qcode == KC_C) {
                az_push(0x26u, 0); /* ç */
            } else if (qcode == KC_SPACE) {
                az_push(0x21u, 0); /* ' */
            } else {
                az_push(0x21u, 0);
                az_push(kc, fl);
            }
            return true;
        }

        if (fl & AZ_DEAD) {
            az_dead     = (uint8_t)(fl & AZ_DEAD);
            ET2         = 0;
            az_dead_to  = AZ_DEAD_MAX;
            ET2         = 1;
            az_dead_due = 0;
            return true; /* dead key: nothing emitted yet */
        }

        /* While the queue is playing, route through it: otherwise a natural
         * keystroke would jump ahead of a synthetic character still in flight. */
        if (az_play != 0 || az_q_head != az_q_tail) {
            az_push(kc, fl);
            return true;
        }

        /* Direct path: holding the key holds the translated keycode, so host
         * key repeat works naturally. */
        if (!az_track_add((uint8_t)qcode, kc)) {
            return false;
        }
        az_hold_fl = fl;
        az_mods_apply(fl);
        add_key(kc);
        send_keyboard_report();
        return true;
    }
}

void aula_layout_tick(void)
{
    if (az_wait != 0) {
        az_wait--;
        if (az_wait == 0) {
            az_due = 1;
        }
    }
    if (az_dead_to != 0) {
        az_dead_to--;
        if (az_dead_to == 0) {
            az_dead_due = 1;
        }
    }
}

void aula_layout_task(void)
{
    if (az_dead_due) {
        az_dead_due = 0;
        if (az_dead != 0) {
            /* Standalone symbol: `'` for acute, `"` for diaeresis. */
            az_push(az_dead == AZ_DIA ? 0x20u : 0x21u, 0);
            az_dead = 0;
        }
    }

    if (az_play == 0) {
        if (az_q_head == az_q_tail) {
            return;
        }
        az_cur = az_q_kc[az_q_head];
        az_mods_apply(az_q_fl[az_q_head]);
        add_key(az_cur);
        send_keyboard_report();
        az_play = 1;
        az_arm(AZ_HOLD);
        return;
    }

    if (!az_due) {
        return;
    }
    az_due = 0;

    if (az_play == 1) {
        del_key(az_cur);
        send_keyboard_report();
        az_play = 2;
        az_arm(AZ_GAP);
        return;
    }

    az_play   = 0;
    az_q_head = (uint8_t)((az_q_head + 1u) & (AZ_Q - 1u));
    if (az_q_head == az_q_tail) {
        az_mods_rest();
    }
}
