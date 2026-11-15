#include "action.h"
#include "keycodes.h"

/*
 * See action.h for the overview. Notes on the 8051 side of things:
 *  - every table lives in XDATA; nothing here touches IDATA except locals;
 *  - no struct assignment (field copies instead), no recursion: decisions push
 *    buffered events back onto the FRONT of the input queue and the single
 *    processing loop picks them up, so a replay can itself start a new
 *    tap-hold without nesting calls;
 *  - a key position is one byte, row << 4 | col (matrices up to 16 x 16).
 */

__xdata action_config_t action_config;

#define POS(r, c)  ((uint8_t)(((uint8_t)(r) << 4) | ((uint8_t)(c) & 0x0Fu)))
#define POS_ROW(p) ((uint8_t)((p) >> 4))
#define POS_COL(p) ((uint8_t)((p) & 0x0Fu))
#define POS_NONE   0xFFu

/* ------------------------------------------------------------ input queue */

#define QUEUE_LEN 16u
#define WAIT_LEN  8u

static __xdata uint8_t  q_pos[QUEUE_LEN];
static __xdata uint8_t  q_down[QUEUE_LEN];
static __xdata uint16_t q_time[QUEUE_LEN];
static __xdata uint8_t  q_head;
static __xdata uint8_t  q_count;

/* Events that arrived while a tap-hold key was undecided. */
static __xdata uint8_t  wb_pos[WAIT_LEN];
static __xdata uint8_t  wb_down[WAIT_LEN];
static __xdata uint16_t wb_time[WAIT_LEN];
static __xdata uint8_t  wb_count;

static void q_push_back(uint8_t pos, uint8_t down, uint16_t time)
{
    if (q_count >= QUEUE_LEN) {
        return; /* cannot happen: the queue never holds more than WAIT_LEN + 1 */
    }
    const uint8_t i = (uint8_t)((q_head + q_count) % QUEUE_LEN);
    q_pos[i]  = pos;
    q_down[i] = down;
    q_time[i] = time;
    q_count++;
}

static void q_push_front(uint8_t pos, uint8_t down, uint16_t time)
{
    if (q_count >= QUEUE_LEN) {
        return;
    }
    q_head        = (uint8_t)((q_head + QUEUE_LEN - 1u) % QUEUE_LEN);
    q_pos[q_head]  = pos;
    q_down[q_head] = down;
    q_time[q_head] = time;
    q_count++;
}

/* --------------------------------------------------------- held-key slots */

#define ROLE_PLAIN 0u /* resolved keycode pressed as-is                     */
#define ROLE_HOLD  1u /* tap-hold key resolved as hold                      */
#define ROLE_TAP   2u /* tap-hold key resolved as tap, still physically down */
#define ROLE_OSM   3u /* one-shot mod, held                                  */
#define ROLE_OSL   4u /* one-shot layer, held                                */

#define SLOTS 10u

static __xdata uint8_t  s_pos[SLOTS];   /* POS_NONE = free                   */
static __xdata uint8_t  s_role[SLOTS];
static __xdata uint8_t  s_intr[SLOTS];  /* another key went down meanwhile   */
static __xdata uint8_t  s_mods[SLOTS];  /* one-shot mods riding on this key  */
static __xdata uint16_t s_kc[SLOTS];

static uint8_t slot_find(uint8_t pos)
{
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (s_pos[i] == pos) {
            return i;
        }
    }
    return 0xFFu;
}

static uint8_t slot_add(uint8_t pos, uint16_t kc, uint8_t role)
{
    const uint8_t i = slot_find(POS_NONE);
    if (i != 0xFFu) {
        s_pos[i]  = pos;
        s_kc[i]   = kc;
        s_role[i] = role;
        s_intr[i] = 0;
        s_mods[i] = 0;
    }
    return i;
}

/* ------------------------------------------------------------------ state */

static __xdata uint8_t layer_state;
static __xdata uint8_t default_layer_state;

/* The one undecided tap-hold key. */
static __xdata uint8_t  tapping;
static __xdata uint8_t  tk_pos;
static __xdata uint16_t tk_kc;
static __xdata uint16_t tk_time;

/* Quick tap: the last tap-hold key that was tapped, and when it was released. */
static __xdata uint8_t  qt_pos = POS_NONE;
static __xdata uint16_t qt_time;

/* Armed one-shots. */
static __xdata uint8_t  osm_mods;
static __xdata uint16_t osm_time;
static __xdata uint8_t  osl_layer = 0xFFu;
static __xdata uint16_t osl_time;

static uint16_t elapsed(uint16_t since)
{
    return (uint16_t)(action_hal_timer() - since);
}

void action_config_defaults(void)
{
    action_config.tapping_term   = ACTION_TAPPING_TERM_DEFAULT;
    action_config.quick_tap_term = ACTION_TAPPING_TERM_DEFAULT;
    action_config.osk_timeout    = ACTION_OSK_TIMEOUT_DEFAULT;
    action_config.flags          = 0;
}

void action_init(void)
{
    q_head = q_count = 0;
    wb_count         = 0;
    for (uint8_t i = 0; i < SLOTS; i++) {
        s_pos[i] = POS_NONE;
    }
    layer_state         = 0;
    default_layer_state = 1;
    tapping             = 0;
    qt_pos              = POS_NONE;
    osm_mods            = 0;
    osl_layer           = 0xFFu;
}

/* ----------------------------------------------------------------- layers */

static uint8_t layer_count(void)
{
    const uint8_t n = action_hal_layer_count();
    return n > 8u ? 8u : n;
}

static void layer_on(uint8_t l)
{
    if (l < layer_count()) {
        layer_state |= (uint8_t)(1u << l);
    }
}

static void layer_off(uint8_t l)
{
    if (l < 8u) {
        layer_state &= (uint8_t)~(1u << l);
    }
}

uint8_t action_layer_state(void)
{
    return (uint8_t)(layer_state | default_layer_state);
}

uint8_t action_highest_layer(void)
{
    const uint8_t st = action_layer_state();
    for (uint8_t l = 8; l-- > 0;) {
        if (st & (uint8_t)(1u << l)) {
            return l;
        }
    }
    return 0;
}

void action_set_default_layer(uint8_t layer)
{
    if (layer < layer_count()) {
        default_layer_state = (uint8_t)(1u << layer);
    }
}

static uint16_t resolve(uint8_t pos)
{
    const uint8_t st = action_layer_state();
    for (uint8_t l = layer_count(); l-- > 0;) {
        if (st & (uint8_t)(1u << l)) {
            const uint16_t kc = action_hal_keymap(l, POS_ROW(pos), POS_COL(pos));
            if (kc != KC_TRANSPARENT) {
                return kc;
            }
        }
    }
    return KC_NO;
}

/* ---------------------------------------------------------- keycode helpers */

/* QMK's 5-bit mod field (bit 4 = right hand) to an 8-bit HID mask. */
static uint8_t mods8(uint8_t m5)
{
    const uint8_t m = (uint8_t)(m5 & 0x0Fu);
    return (m5 & 0x10u) ? (uint8_t)(m << 4) : m;
}

static bool is_dual(uint16_t kc)
{
    return IS_QK_MOD_TAP(kc) || IS_QK_LAYER_TAP(kc) || IS_QK_LAYER_TAP_TOGGLE(kc);
}

static uint8_t dual_layer(uint16_t kc)
{
    return IS_QK_LAYER_TAP(kc) ? (uint8_t)((kc >> 8) & 0x0Fu) : (uint8_t)(kc & 0x1Fu);
}

/* Tap keycode of MT / LT; TT taps toggle instead (returns KC_NO). */
static uint16_t dual_tap_kc(uint16_t kc)
{
    return IS_QK_LAYER_TAP_TOGGLE(kc) ? KC_NO : (uint16_t)(kc & 0xFFu);
}

static void send(uint8_t pos, uint16_t kc, bool down)
{
    action_hal_event_pos(POS_ROW(pos), POS_COL(pos));
    action_hal_send(kc, down);
}

/* ------------------------------------------------------- press / release */

static void mark_interrupted(uint8_t except_pos)
{
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (s_pos[i] != POS_NONE && s_pos[i] != except_pos) {
            s_intr[i] = 1;
        }
    }
}

static void tap_press(uint8_t pos, uint16_t kc)
{
    if (IS_QK_LAYER_TAP_TOGGLE(kc)) {
        layer_state ^= (uint8_t)(1u << dual_layer(kc));
        layer_state &= (uint8_t)((1u << layer_count()) - 1u);
    } else if (dual_tap_kc(kc) != KC_NO) {
        send(pos, dual_tap_kc(kc), true);
    }
}

static void hold_on(uint16_t kc)
{
    if (IS_QK_MOD_TAP(kc)) {
        action_hal_mods(mods8((uint8_t)((kc >> 8) & 0x1Fu)), true);
    } else {
        layer_on(dual_layer(kc));
    }
}

static void hold_off(uint16_t kc)
{
    if (IS_QK_MOD_TAP(kc)) {
        action_hal_mods(mods8((uint8_t)((kc >> 8) & 0x1Fu)), false);
    } else {
        layer_off(dual_layer(kc));
    }
}

static void press_plain(uint8_t slot, uint8_t pos, uint16_t kc)
{
    if (kc == KC_NO || kc == KC_TRANSPARENT) {
        return;
    }
    if (IS_QK_MOMENTARY(kc)) {
        layer_on((uint8_t)(kc & 0x1Fu));
    } else if (IS_QK_TOGGLE_LAYER(kc)) {
        const uint8_t l = (uint8_t)(kc & 0x1Fu);
        if (l < layer_count()) {
            layer_state ^= (uint8_t)(1u << l);
        }
    } else if (IS_QK_TO(kc)) {
        const uint8_t l = (uint8_t)(kc & 0x1Fu);
        if (l < layer_count()) {
            layer_state = (uint8_t)(1u << l);
        }
    } else if (IS_QK_DEF_LAYER(kc)) {
        action_set_default_layer((uint8_t)(kc & 0x1Fu));
    } else if (IS_QK_LAYER_MOD(kc)) {
        layer_on((uint8_t)((kc >> 5) & 0x0Fu));
        action_hal_mods(mods8((uint8_t)(kc & 0x1Fu)), true);
    } else if (IS_QK_ONE_SHOT_MOD(kc)) {
        s_role[slot] = ROLE_OSM;
        action_hal_mods(mods8((uint8_t)(kc & 0x1Fu)), true);
    } else if (IS_QK_ONE_SHOT_LAYER(kc)) {
        s_role[slot] = ROLE_OSL;
        layer_on((uint8_t)(kc & 0x1Fu));
    } else {
        /* An output key: it consumes armed one-shots. The mods stay down
         * until this key is released (stored in the slot). */
        if (osm_mods) {
            s_mods[slot] = osm_mods;
            action_hal_mods(osm_mods, true);
            osm_mods = 0;
        }
        send(pos, kc, true);
    }
}

static void release_plain(uint8_t slot, uint8_t pos, uint16_t kc)
{
    if (IS_QK_MOMENTARY(kc)) {
        layer_off((uint8_t)(kc & 0x1Fu));
    } else if (IS_QK_LAYER_MOD(kc)) {
        action_hal_mods(mods8((uint8_t)(kc & 0x1Fu)), false);
        layer_off((uint8_t)((kc >> 5) & 0x0Fu));
    } else if (IS_QK_TOGGLE_LAYER(kc) || IS_QK_TO(kc) || IS_QK_DEF_LAYER(kc) ||
               kc == KC_NO || kc == KC_TRANSPARENT) {
        /* nothing on release */
    } else {
        send(pos, kc, false);
        if (s_mods[slot]) {
            action_hal_mods(s_mods[slot], false);
        }
    }
}

static void do_press(uint8_t pos, uint16_t time)
{
    const uint16_t kc = resolve(pos);

    mark_interrupted(pos);

    /* One-shot layer: it applied to this key's lookup, now it is spent --
     * unless this key is itself a layer key (stack one-shots / MO). */
    if (osl_layer != 0xFFu && kc != KC_NO && !IS_QK_ONE_SHOT_LAYER(kc) &&
        !IS_QK_MOMENTARY(kc) && !IS_QK_ONE_SHOT_MOD(kc)) {
        layer_off(osl_layer);
        osl_layer = 0xFFu;
    }

    if (is_dual(kc)) {
        if (qt_pos == pos && elapsed(qt_time) < action_config.quick_tap_term) {
            /* Quick tap: tapped and pressed again right away -> the tap
             * keycode is held (auto-repeat), no hold decision. */
            if (slot_add(pos, kc, ROLE_TAP) != 0xFFu) {
                tap_press(pos, kc);
            }
            return;
        }
        tapping = 1;
        tk_pos  = pos;
        tk_kc   = kc;
        tk_time = time;
        return;
    }

    const uint8_t slot = slot_add(pos, kc, ROLE_PLAIN);
    if (slot == 0xFFu) {
        return; /* more than SLOTS keys down: ignore the extra */
    }
    press_plain(slot, pos, kc);
}

static void do_release(uint8_t pos)
{
    const uint8_t slot = slot_find(pos);
    if (slot == 0xFFu) {
        return;
    }
    const uint16_t kc   = s_kc[slot];
    const uint8_t  role = s_role[slot];
    const uint8_t  intr = s_intr[slot];
    s_pos[slot] = POS_NONE;

    switch (role) {
        case ROLE_PLAIN:
            release_plain(slot, pos, kc);
            break;

        case ROLE_HOLD:
            hold_off(kc);
            if ((action_config.flags & ACTION_RETRO_TAPPING) && !intr && dual_tap_kc(kc) != KC_NO) {
                send(pos, dual_tap_kc(kc), true);
                send(pos, dual_tap_kc(kc), false);
            }
            break;

        case ROLE_TAP:
            if (dual_tap_kc(kc) != KC_NO) {
                send(pos, dual_tap_kc(kc), false);
            }
            qt_pos  = pos;
            qt_time = action_hal_timer();
            break;

        case ROLE_OSM: {
            const uint8_t m = mods8((uint8_t)(kc & 0x1Fu));
            action_hal_mods(m, false);
            if (!intr) {
                osm_mods |= m; /* tapped alone: arm for the next key */
                osm_time = action_hal_timer();
            }
            break;
        }

        case ROLE_OSL:
            if (intr) {
                layer_off((uint8_t)(kc & 0x1Fu));
            } else {
                osl_layer = (uint8_t)(kc & 0x1Fu); /* stays on for the next key */
                osl_time  = action_hal_timer();
            }
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------- tap-hold decision */

static void requeue_waiting(void)
{
    for (uint8_t i = wb_count; i-- > 0;) {
        q_push_front(wb_pos[i], wb_down[i], wb_time[i]);
    }
    wb_count = 0;
}

static void decide_hold(void)
{
    tapping = 0;
    if (slot_add(tk_pos, tk_kc, ROLE_HOLD) != 0xFFu) {
        hold_on(tk_kc);
    }
    requeue_waiting();
}

static void decide_tap(void)
{
    tapping = 0;
    if (slot_add(tk_pos, tk_kc, ROLE_TAP) != 0xFFu) {
        tap_press(tk_pos, tk_kc);
    }
    requeue_waiting();
}

static bool waiting_has_press(uint8_t pos)
{
    for (uint8_t i = 0; i < wb_count; i++) {
        if (wb_pos[i] == pos && wb_down[i]) {
            return true;
        }
    }
    return false;
}

static void wait_add(uint8_t pos, uint8_t down, uint16_t time)
{
    wb_pos[wb_count]  = pos;
    wb_down[wb_count] = down;
    wb_time[wb_count] = time;
    wb_count++;
}

static void handle(uint8_t pos, uint8_t down, uint16_t time)
{
    if (!tapping) {
        if (down) {
            do_press(pos, time);
        } else {
            do_release(pos);
        }
        return;
    }

    if (pos == tk_pos) {
        if (!down) {
            /* Released before the term: a tap. Order: tap press, everything
             * that was buffered, then this release (QMK's order). */
            q_push_front(pos, down, time);
            decide_tap();
        }
        return;
    }

    if (down) {
        if ((action_config.flags & ACTION_HOLD_ON_OTHER_KEY_PRESS) || wb_count >= WAIT_LEN) {
            q_push_front(pos, down, time);
            decide_hold();
        } else {
            wait_add(pos, down, time);
        }
        return;
    }

    /* A release while undecided. */
    if (waiting_has_press(pos)) {
        if (action_config.flags & ACTION_PERMISSIVE_HOLD) {
            /* A key pressed AND released inside the hold: permissive hold. */
            q_push_front(pos, down, time);
            decide_hold();
        } else if (wb_count < WAIT_LEN) {
            wait_add(pos, down, time);
        } else {
            q_push_front(pos, down, time);
            decide_hold();
        }
    } else {
        /* Key was already down before the tap-hold key: let it go now. */
        do_release(pos);
    }
}

static void process(void)
{
    while (q_count) {
        const uint8_t  pos  = q_pos[q_head];
        const uint8_t  down = q_down[q_head];
        const uint16_t time = q_time[q_head];
        q_head = (uint8_t)((q_head + 1u) % QUEUE_LEN);
        q_count--;
        handle(pos, down, time);
    }
}

void action_event(uint8_t row, uint8_t col, bool pressed)
{
    q_push_back(POS(row, col), pressed ? 1u : 0u, action_hal_timer());
    process();
}

void action_task(void)
{
    if (tapping && elapsed(tk_time) >= action_config.tapping_term) {
        decide_hold();
        process();
    }
    if (action_config.osk_timeout != 0) {
        if (osm_mods && elapsed(osm_time) >= action_config.osk_timeout) {
            osm_mods = 0;
        }
        if (osl_layer != 0xFFu && elapsed(osl_time) >= action_config.osk_timeout) {
            layer_off(osl_layer);
            osl_layer = 0xFFu;
        }
    }
}

bool action_idle(void)
{
    if (tapping || wb_count || osm_mods || osl_layer != 0xFFu) {
        return false;
    }
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (s_pos[i] != POS_NONE) {
            return false;
        }
    }
    return true;
}
