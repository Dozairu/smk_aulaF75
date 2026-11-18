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
#define ROLE_KO    5u /* key override active: s_kc is the replacement        */

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

#ifdef ACTION_COMBO
/* A fired combo is a virtual key at row 14, column = combo index: it goes
 * through the queue, slots and tap-hold logic like a real key, and resolve()
 * returns the combo's output keycode for it. */
#    define COMBO_ROW    0x0Eu
#    define IS_VPOS(p)   (POS_ROW(p) == COMBO_ROW)
#    define COMBO_BUF    4u  /* presses held back while a combo may form */
#    define COMBO_SWLEN  10u /* combo keys still down after their combo fired */
static __xdata uint16_t ce[COMBO_FIELDS]; /* scratch: one entry */
static __xdata uint8_t  cb_pos[COMBO_BUF];
static __xdata uint16_t cb_kc[COMBO_BUF];
static __xdata uint16_t cb_time[COMBO_BUF];
static __xdata uint8_t  cb_n;
static __xdata uint8_t  sw_pos[COMBO_SWLEN]; /* their releases are swallowed */
static __xdata uint8_t  sw_idx[COMBO_SWLEN];
static __xdata uint16_t combo_down; /* bit n: combo n's output is pressed */
#endif

#ifdef ACTION_KEY_OVERRIDE
/* The one active key override (see "key overrides" below). */
static __xdata uint16_t ko_e[KO_FIELDS]; /* its entry; scratch while searching */
static __xdata uint16_t ko_trigger;      /* the keycode it replaced            */
static __xdata uint8_t  ko_slot = 0xFFu; /* slot of the trigger key; 0xFF: none */
static __xdata uint8_t  ko_lifted;       /* suppressed mods taken off the report */
static __xdata uint8_t  ko_prev_role;    /* original slot role before override */
#endif

#ifdef ACTION_EXTRAS
#    define ROLE_SC 6u /* Space Cadet key, held: its hold mod is down        */
#    define MOD_SHIFT 0x22u
#    define CW_IDLE_MS 5000u
static __xdata uint8_t  llock;          /* Layer Lock: locked layers           */
static __xdata uint8_t  combos_off;     /* CMB_OFF                             */
static __xdata uint8_t  cw_on;          /* Caps Word active                    */
static __xdata uint16_t cw_time;        /* ...last key, for its idle timeout   */
static __xdata uint16_t rep_kc;         /* Repeat: last output keycode          */
static __xdata uint8_t  rep_mods;       /* ...and the mods held with it         */
static __xdata uint8_t  rep_busy;       /* a Repeat / Alt Repeat is being sent  */
static __xdata uint16_t sc_time;        /* Space Cadet: when its key went down */
static __xdata uint8_t  osm_lock;       /* one-shot mods locked by tap toggle  */
static __xdata uint8_t  osm_taps;
static __xdata uint8_t  osm_tap_m;
static __xdata uint8_t  tt_pos = POS_NONE; /* TT taps toggling the layer       */
static __xdata uint8_t  tt_n;
static __xdata uint16_t tt_time;

/* Space Cadet (QK_SPACE_CADET_*, 0x7C18..0x7C1E): hold mod, tap mod, tap key.
 * Held: the hold mod; tapped alone: tap mod + key (QMK's defaults). */
static const __code uint8_t sc_keys[7][3] = {
    {0x01, 0x02, KC_9},   /* SC_LCPO  LCtrl  / (   */
    {0x10, 0x20, KC_0},   /* SC_RCPC  RCtrl  / )   */
    {0x02, 0x02, KC_9},   /* SC_LSPO  LShift / (   */
    {0x20, 0x20, KC_0},   /* SC_RSPC  RShift / )   */
    {0x04, 0x02, KC_9},   /* SC_LAPO  LAlt   / (   */
    {0x40, 0x20, KC_0},   /* SC_RAPC  RAlt   / )   */
    {0x20, 0x20, KC_ENT}, /* SC_SENT  RShift / Shift+Enter (tap mod = hold mod) */
};
#endif

#ifdef ACTION_TAP_DANCE
/* The one tap dance in progress (see "tap dance" below). */
static __xdata uint16_t td_e[TD_FIELDS];
static __xdata uint8_t  td_pos = POS_NONE; /* the dancing key; POS_NONE = none */
static __xdata uint8_t  td_count;
static __xdata uint8_t  td_held;
static __xdata uint16_t td_time;
#endif

static uint16_t elapsed(uint16_t since)
{
    return (uint16_t)(action_hal_timer() - since);
}

void action_config_defaults(void)
{
    action_config.tapping_term   = ACTION_TAPPING_TERM_DEFAULT;
    action_config.quick_tap_term = ACTION_TAPPING_TERM_DEFAULT;
    action_config.osk_timeout    = ACTION_OSK_TIMEOUT_DEFAULT;
    action_config.combo_term     = ACTION_COMBO_TERM_DEFAULT;
    action_config.flags          = 0;
#ifdef ACTION_EXTRAS
    action_config.magic              = ACTION_MAGIC_DEFAULT;
    action_config.grave_esc          = 0;
    action_config.osk_tap_toggle     = 0;
    action_config.tapping_toggle     = ACTION_TAPPING_TOGGLE_DEF;
#endif
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
#ifdef ACTION_TAP_DANCE
    td_pos = POS_NONE;
#endif
#ifdef ACTION_KEY_OVERRIDE
    ko_slot = 0xFFu;
#endif
#ifdef ACTION_EXTRAS
    llock = combos_off = cw_on = rep_busy = osm_lock = osm_taps = 0;
    rep_kc  = KC_NO;
    tt_pos  = POS_NONE;
#endif
#ifdef ACTION_COMBO
    cb_n       = 0;
    combo_down = 0;
    for (uint8_t i = 0; i < COMBO_SWLEN; i++) {
        sw_pos[i] = POS_NONE;
    }
#endif
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
#ifdef ACTION_EXTRAS
    if (llock & (uint8_t)(1u << l)) {
        return; /* Layer Lock: stays on until unlocked */
    }
#endif
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
#ifdef ACTION_COMBO
    if (IS_VPOS(pos)) {
        return action_hal_combo(POS_COL(pos), ce) ? ce[COMBO_OUTPUT] : KC_NO;
    }
#endif
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
    if (POS_ROW(pos) < ACTION_ENC_ROW) { /* virtual positions (encoder, combo) keep last matrix pos */
        action_hal_event_pos(POS_ROW(pos), POS_COL(pos));
    }
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

static void press_output(uint8_t slot, uint8_t pos, uint16_t kc);

/* The tap of a tap-hold key (slot role ROLE_TAP). MT / LT taps go through the
 * output stage like any key (one-shot mods, Caps Word, Repeat, Magic...). */
static void tap_press(uint8_t slot, uint8_t pos, uint16_t kc)
{
    if (IS_QK_LAYER_TAP_TOGGLE(kc)) {
#ifdef ACTION_EXTRAS
        /* TT: toggles after action_config.tapping_toggle taps in a row. */
        tt_n    = (tt_pos == pos && elapsed(tt_time) < action_config.tapping_term) ? (uint8_t)(tt_n + 1u) : 1u;
        tt_pos  = pos;
        tt_time = action_hal_timer();
        if (tt_n < action_config.tapping_toggle) {
            return;
        }
        tt_n = 0;
#endif
        layer_state ^= (uint8_t)(1u << dual_layer(kc));
        layer_state &= (uint8_t)((1u << layer_count()) - 1u);
#ifdef ACTION_EXTRAS
        llock &= layer_state;
#endif
    } else if (dual_tap_kc(kc) != KC_NO) {
        press_output(slot, pos, dual_tap_kc(kc));
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

/* -------------------------------------------------------- key overrides
 *
 * vial-qmk / QMK key overrides, the common subset: when an output key is
 * pressed whose keycode is an enabled override's trigger, with the trigger
 * mods held (all of them, or any one with KO_OPT_ONE_MOD), none of the
 * negative mods, and the highest active layer in the layer mask, the
 * suppressed mods come off the report and the replacement is sent instead.
 * It ends when the trigger key is released, when the mods stop matching (the
 * trigger is then pressed again unless KO_OPT_NO_REREGISTER_TRIGGER), or when
 * another key goes down (unless KO_OPT_NO_UNREGISTER_ON_OTHER).
 * Not supported: activation by a mod press / negative mod release while the
 * trigger is already held, and mods-only triggers (trigger KC_NO).
 *
 * "Mods held" is worked out from the engine's own held keys, not from the
 * report, so a mod that is let go during the override is never put back.
 */
#if defined(ACTION_KEY_OVERRIDE) || defined(ACTION_EXTRAS)
static uint8_t held_mods(void)
{
    uint8_t m = osm_mods;
#    ifdef ACTION_EXTRAS
    m |= osm_lock;
#    endif
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (s_pos[i] == POS_NONE) {
            continue;
        }
        const uint16_t kc = s_kc[i];
        m |= s_mods[i];
        switch (s_role[i]) {
            case ROLE_PLAIN:
                if (IS_MODIFIER_KEYCODE(kc)) {
                    m |= (uint8_t)MOD_BIT(kc);
                } else if (IS_QK_MODS(kc)) {
                    m |= mods8((uint8_t)QK_MODS_GET_MODS(kc));
                } else if (IS_QK_LAYER_MOD(kc)) {
                    m |= mods8((uint8_t)(kc & 0x1Fu));
                }
                break;
            case ROLE_HOLD:
                if (IS_QK_MOD_TAP(kc)) {
                    m |= mods8((uint8_t)((kc >> 8) & 0x1Fu));
                }
                break;
            case ROLE_OSM:
                m |= mods8((uint8_t)(kc & 0x1Fu));
                break;
#    ifdef ACTION_EXTRAS
            case ROLE_SC:
                m |= sc_keys[(uint8_t)(kc - QK_SPACE_CADET_LEFT_CTRL_PARENTHESIS_OPEN)][0];
                break;
#    endif
            default:
                break;
        }
    }
    return m;
}
#endif

#ifdef ACTION_KEY_OVERRIDE
/* ko_e's mod condition against the held mods. */
static bool ko_mods_match(void)
{
    const uint8_t mods = held_mods();
    const uint8_t trig = (uint8_t)ko_e[KO_MODS];
    if (mods & (uint8_t)(ko_e[KO_MODS] >> 8)) {
        return false;
    }
    if ((uint8_t)(ko_e[KO_SUPPRESS] >> 8) & KO_OPT_ONE_MOD) {
        return trig == 0 || (mods & trig);
    }
    return (uint8_t)(mods & trig) == trig;
}

static bool ko_try(uint8_t slot, uint8_t pos, uint16_t kc)
{
    const uint16_t layer = (uint16_t)(1u << action_highest_layer());
    if (ko_slot != 0xFFu) {
        return false; /* one at a time; ko_e belongs to the active one */
    }
    for (uint8_t idx = 0; idx < KO_MAX; idx++) {
        if (!action_hal_key_override(idx, ko_e)) {
            break;
        }
        const uint8_t opt = (uint8_t)(ko_e[KO_SUPPRESS] >> 8);
        if ((opt & (KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN)) != (KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) ||
            ko_e[KO_TRIGGER] != kc || !(ko_e[KO_LAYERS] & layer) || !ko_mods_match()) {
            continue;
        }
        ko_trigger   = kc;
        ko_slot      = slot;
        ko_lifted    = (uint8_t)(held_mods() & (uint8_t)ko_e[KO_SUPPRESS]);
        ko_prev_role = s_role[slot];
        s_role[slot] = ROLE_KO;
        s_kc[slot]   = ko_e[KO_REPLACEMENT];
        if (ko_lifted) {
            action_hal_mods(ko_lifted, false);
        }
        send(pos, s_kc[slot], true);
        return true;
    }
    return false;
}

/* End the active override: replacement up, lifted mods that are still held
 * back on, and the trigger down again if asked and its key is still held. */
static void ko_end(uint8_t pos, bool reregister)
{
    const uint8_t slot = ko_slot;
    ko_slot            = 0xFFu;
    send(pos, s_kc[slot], false);
    const uint8_t back = (uint8_t)(ko_lifted & held_mods());
    if (back) {
        action_hal_mods(back, true);
    }
    if (s_pos[slot] != POS_NONE) { /* key still down */
        /* Restore original role (ROLE_PLAIN or ROLE_TAP); without
         * re-registering, release sends an extra key-up for the trigger (which
         * the report ignores) and still frees any one-shot mods riding on it. */
        s_role[slot] = ko_prev_role;
        s_kc[slot]   = ko_trigger;
        if (reregister) {
            send(pos, ko_trigger, true);
        }
    }
}
#endif

/* ------------------------------------------------- QMK quantum keycodes
 *
 * The keycodes Vial's picker offers beyond layers / tap-hold, with QMK's
 * default behaviour: Magic (keymap_config swaps), Grave Escape, Space Cadet,
 * Caps Word, Repeat / Alt Repeat, Layer Lock, tri-layer (FN_MO13 / FN_MO23),
 * combo on/off. QK_BOOT, EE_CLR and the RGB keys are not engine
 * business: they go out to action_hal_send() for the keyboard to handle.
 */
#ifdef ACTION_EXTRAS
/* Magic swaps as {from, to, bits}: the first rule whose bit is set applies
 * (QMK keymap_config order); a GUI result is dropped by MAGIC_NO_GUI. */
static const __code uint8_t magic_rules[][3] = {
    {KC_CAPS, KC_LCTL, 0x03}, /* SWAP_CAPS_LCTL | CAPS_AS_CTRL  (low byte) */
    {KC_LCTL, KC_CAPS, 0x01}, /* SWAP_CAPS_LCTL                            */
    {KC_LALT, KC_LGUI, 0x04}, /* SWAP_LALT_LGUI                            */
    {KC_RALT, KC_RGUI, 0x08}, /* SWAP_RALT_RGUI                            */
    {KC_LGUI, KC_LALT, 0x04},
    {KC_RGUI, KC_RALT, 0x08},
    {KC_GRV, KC_ESC, 0x20},   /* SWAP_GRV_ESC                              */
    {KC_ESC, KC_GRV, 0x20},
    {KC_BSLS, KC_BSPC, 0x40}, /* SWAP_BSLS_BSPC                            */
    {KC_BSPC, KC_BSLS, 0x40},
    {KC_LCTL, KC_LGUI, 0x81}, /* bit 7 set: high byte -- SWAP_LCTL_LGUI    */
    {KC_RCTL, KC_RGUI, 0x82}, /*                         SWAP_RCTL_RGUI    */
    {KC_LGUI, KC_LCTL, 0x81},
    {KC_RGUI, KC_RCTL, 0x82},
};

static uint16_t magic_remap(uint16_t kc)
{
    const uint16_t m = action_config.magic;
    if (kc > 0xFFu) {
        return kc;
    }
    for (uint8_t i = 0; i < sizeof(magic_rules) / 3u; i++) {
        const uint8_t bits = magic_rules[i][2];
        const uint8_t on   = (bits & 0x80u) ? (uint8_t)(m >> 8) & bits & 0x7Fu : (uint8_t)m & bits;
        if (on && (uint8_t)kc == magic_rules[i][0]) {
            kc = magic_rules[i][1];
            break;
        }
    }
    if ((m & MAGIC_NO_GUI) && (kc == KC_LGUI || kc == KC_RGUI)) {
        return KC_NO;
    }
    return kc;
}

/* Magic keycodes 0x7000 + n: op (set / clear / toggle) on one of the masks. */
#    define MG_SET 0x40u
#    define MG_CLR 0x80u
#    define MG_TOG 0xC0u
static const __code uint16_t magic_masks[12] = {
    MAGIC_SWAP_CAPS_LCTL, MAGIC_CAPS_AS_CTRL,   MAGIC_SWAP_LALT_LGUI, MAGIC_SWAP_RALT_RGUI,
    MAGIC_NO_GUI,         MAGIC_SWAP_GRV_ESC,   MAGIC_SWAP_BSLS_BSPC, MAGIC_NKRO,
    MAGIC_SWAP_LCTL_LGUI, MAGIC_SWAP_RCTL_RGUI,
    MAGIC_SWAP_LALT_LGUI | MAGIC_SWAP_RALT_RGUI, MAGIC_SWAP_LCTL_LGUI | MAGIC_SWAP_RCTL_RGUI,
};
static const __code uint8_t magic_ops[30] = {
    MG_SET | 0, MG_CLR | 0, MG_TOG | 0, MG_CLR | 1,  MG_SET | 1,  /* 00 caps/ctrl  */
    MG_SET | 2, MG_CLR | 2, MG_SET | 3, MG_CLR | 3,               /* 05 alt/gui    */
    MG_CLR | 4, MG_SET | 4, MG_TOG | 4,                           /* 09 no gui     */
    MG_SET | 5, MG_CLR | 5, MG_SET | 6, MG_CLR | 6,  MG_TOG | 6,  /* 0C grv, bsls  */
    MG_SET | 7, MG_CLR | 7, MG_TOG | 7,                           /* 11 nkro       */
    MG_SET | 10, MG_CLR | 10, MG_TOG | 10,                        /* 14 alt/gui    */
    MG_SET | 8, MG_CLR | 8, MG_SET | 9, MG_CLR | 9,               /* 17 ctl/gui    */
    MG_SET | 11, MG_CLR | 11, MG_TOG | 11,                        /* 1B ctl/gui    */
};

static void magic_key(uint8_t n)
{
    if (n >= sizeof(magic_ops)) {
        return; /* EE_HANDS and beyond: no meaning here */
    }
    const uint8_t  op   = magic_ops[n];
    const uint16_t mask = magic_masks[op & 0x0Fu];
    if ((op & MG_TOG) == MG_SET) {
        action_config.magic |= mask;
    } else if ((op & MG_TOG) == MG_CLR) {
        action_config.magic &= (uint16_t)~mask;
    } else {
        action_config.magic ^= mask;
    }
    action_hal_config_changed();
}

/* Caps Word: 2 = shift it, 1 = goes on unshifted, 0 = ends Caps Word. */
static uint8_t cw_class(uint16_t kc)
{
    if ((kc >= KC_A && kc <= KC_Z) || kc == KC_MINS) {
        return 2;
    }
    if ((kc >= KC_1 && kc <= KC_0) || kc == KC_BSPC || kc == KC_DEL || kc == LSFT(KC_MINS) ||
        IS_MODIFIER_KEYCODE(kc)) {
        return 1;
    }
    return 0;
}

#endif

/*
 * Output stage: a key that ends up as a keycode for the host (basic, modded,
 * consumer, macro, keyboard-specific). Magic, armed one-shot
 * mods, Caps Word, Repeat bookkeeping and key overrides apply here. The final
 * keycode is kept in the slot, so the release sends exactly what was pressed.
 */
static void press_output(uint8_t slot, uint8_t pos, uint16_t kc)
{
#ifdef ACTION_EXTRAS
    kc         = magic_remap(kc);
    s_kc[slot] = kc;
    if (kc == KC_NO) {
        return;
    }
#else
    s_kc[slot] = kc;
#endif
    /* Armed one-shot mods: down until this key is released. */
    if (osm_mods) {
        s_mods[slot] |= osm_mods;
        action_hal_mods(osm_mods, true);
        osm_mods = 0;
    }
#ifdef ACTION_EXTRAS
    if (cw_on) {
        const uint8_t c = cw_class(kc);
        if (c == 0 || elapsed(cw_time) >= CW_IDLE_MS || (held_mods() & (uint8_t)~MOD_SHIFT)) {
            cw_on = 0;
        } else {
            cw_time = action_hal_timer();
            if (c == 2 && !(held_mods() & MOD_SHIFT)) {
                s_mods[slot] |= 0x02u;
                action_hal_mods(0x02u, true);
            }
        }
    }
    if (!rep_busy && !IS_MODIFIER_KEYCODE(kc)) {
        rep_kc   = kc;
        rep_mods = held_mods();
    }
#endif
#ifdef ACTION_KEY_OVERRIDE
    if (ko_try(slot, pos, kc)) {
        return;
    }
#endif
    send(pos, kc, true);
}

#ifdef ACTION_EXTRAS
/* Alt Repeat: the alternate of the last key, from QMK's built-in pairs
 * (arrows, Home/End, PgUp/PgDn, media, browser, brightness, Ctrl+Z/Y); the
 * last key's mods carry over. (Vial's own Alt Repeat table is not offered:
 * no code space left for it.) Returns KC_NO if there is none. */
static const __code uint8_t arep_pairs[][2] = {
    {KC_LEFT, KC_RGHT}, {KC_UP, KC_DOWN},   {KC_HOME, KC_END},  {KC_PGUP, KC_PGDN},
    {KC_VOLU, KC_VOLD}, {KC_MNXT, KC_MPRV}, {KC_WBAK, KC_WFWD}, {KC_BRIU, KC_BRID},
};

static uint16_t arep_lookup(uint8_t *out_mods)
{
    uint8_t  mods = rep_mods;
    uint16_t kc   = rep_kc;
    if (IS_QK_MODS(kc)) { /* e.g. LCTL(KC_Z) on a key: its mods count too */
        mods |= mods8((uint8_t)QK_MODS_GET_MODS(kc));
        kc = (uint16_t)(kc & 0xFFu);
    }
    *out_mods = mods;
    if ((mods & 0x11u) && (kc == KC_Y || kc == KC_Z)) {
        return (uint16_t)(kc ^ (KC_Y ^ KC_Z)); /* Ctrl+Z <-> Ctrl+Y */
    }
    for (uint8_t i = 0; i < sizeof(arep_pairs) / 2u; i++) {
        if (kc == arep_pairs[i][0]) {
            return arep_pairs[i][1];
        }
        if (kc == arep_pairs[i][1]) {
            return arep_pairs[i][0];
        }
    }
    return KC_NO;
}

/* Repeat / Alt Repeat: kc with mods (those not already held), as a key of
 * its own; it is not recorded as the new "last key". */
static void rep_press(uint8_t slot, uint8_t pos, uint16_t kc, uint8_t mods)
{
    if (kc == KC_NO) {
        return;
    }
    mods &= (uint8_t)~held_mods();
    if (mods) {
        s_mods[slot] = mods;
        action_hal_mods(mods, true);
    }
    rep_busy = 1;
    press_output(slot, pos, kc);
    rep_busy = 0;
}

static void tri_update(void)
{
    if ((layer_state & 0x06u) == 0x06u) {
        layer_on(3);
    } else {
        layer_off(3);
    }
}

/* Quantum keycodes the engine handles itself. False: not one of them. */
static bool extra_press(uint8_t slot, uint8_t pos, uint16_t kc)
{
    if (kc >= QK_MAGIC && kc <= QK_MAGIC_MAX) {
        s_kc[slot] = KC_NO;
        magic_key((uint8_t)kc);
        return true;
    }
    if (kc >= QK_SPACE_CADET_LEFT_CTRL_PARENTHESIS_OPEN && kc <= QK_SPACE_CADET_RIGHT_SHIFT_ENTER) {
        s_role[slot] = ROLE_SC;
        sc_time      = action_hal_timer();
        action_hal_mods(sc_keys[(uint8_t)(kc - QK_SPACE_CADET_LEFT_CTRL_PARENTHESIS_OPEN)][0], true);
        return true;
    }
    if (kc == QK_TRI_LAYER_LOWER || kc == QK_TRI_LAYER_UPPER) {
        layer_on(kc == QK_TRI_LAYER_LOWER ? 1u : 2u);
        tri_update();
        return true; /* released in release_plain */
    }

    const uint8_t mods = held_mods();
    uint8_t       amods;
    switch (kc) {
        case QK_GRAVE_ESCAPE: {
            /* Escape; ` when Shift or GUI is held, unless a Grave Escape
             * override says Escape for one of the held mods. */
            const uint8_t g  = action_config.grave_esc;
            const uint8_t om = (uint8_t)(((g & GESC_ALT) ? 0x44u : 0u) | ((g & GESC_CTRL) ? 0x11u : 0u) |
                                         ((g & GESC_GUI) ? 0x88u : 0u) | ((g & GESC_SHIFT) ? 0x22u : 0u));
            press_output(slot, pos, ((mods & 0xAAu) && !(mods & om)) ? KC_GRV : KC_ESC);
            return true;
        }
        case QK_REPEAT_KEY:
            s_kc[slot] = KC_NO;
            rep_press(slot, pos, rep_kc, rep_mods);
            return true;
        case QK_ALT_REPEAT_KEY:
            s_kc[slot] = KC_NO;
            amods      = 0;
            kc         = arep_lookup(&amods); /* before amods is read */
            rep_press(slot, pos, kc, amods);
            return true;
        case QK_CAPS_WORD_TOGGLE:
            cw_on ^= 1u;
            cw_time = action_hal_timer();
            break;
        case QK_LAYER_LOCK:
            /* Lock the highest layer that is on (not the default one); on a
             * locked layer, unlock it and turn it off. */
            for (uint8_t l = 8; l-- > 1;) {
                const uint8_t bit = (uint8_t)(1u << l);
                if (layer_state & bit) {
                    if (llock & bit) {
                        llock &= (uint8_t)~bit;
                        layer_off(l);
                    } else {
                        llock |= bit;
                    }
                    break;
                }
            }
            break;
        case QK_COMBO_ON:
            combos_off = 0;
            break;
        case QK_COMBO_OFF:
            combos_off = 1;
            break;
        case QK_COMBO_TOGGLE:
            combos_off ^= 1u;
            break;
        default:
            return false;
    }
    s_kc[slot] = KC_NO; /* nothing to do on release */
    return true;
}
#endif

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
#ifdef ACTION_EXTRAS
            llock &= layer_state;
#endif
        }
    } else if (IS_QK_TO(kc)) {
        const uint8_t l = (uint8_t)(kc & 0x1Fu);
        if (l < layer_count()) {
            layer_state = (uint8_t)(1u << l);
#ifdef ACTION_EXTRAS
            llock &= layer_state;
#endif
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
#ifdef ACTION_EXTRAS
    } else if (extra_press(slot, pos, kc)) {
        /* quantum keycode, handled */
#endif
    } else {
        press_output(slot, pos, kc);
    }
}

static void release_plain(uint8_t slot, uint8_t pos, uint16_t kc)
{
    if (IS_QK_MOMENTARY(kc)) {
        layer_off((uint8_t)(kc & 0x1Fu));
    } else if (IS_QK_LAYER_MOD(kc)) {
        action_hal_mods(mods8((uint8_t)(kc & 0x1Fu)), false);
        layer_off((uint8_t)((kc >> 5) & 0x0Fu));
#ifdef ACTION_EXTRAS
    } else if (kc == QK_TRI_LAYER_LOWER || kc == QK_TRI_LAYER_UPPER) {
        layer_off(kc == QK_TRI_LAYER_LOWER ? 1u : 2u);
        tri_update();
#endif
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

#ifdef ACTION_EXTRAS
    /* Another key breaks a run of one-shot taps (tap toggle) or TT taps. */
    if (!IS_QK_ONE_SHOT_MOD(kc)) {
        osm_taps = 0;
    }
    if (!IS_QK_LAYER_TAP_TOGGLE(kc)) {
        tt_pos = POS_NONE;
    }
#endif

#ifdef ACTION_KEY_OVERRIDE
    if (ko_slot != 0xFFu && !((uint8_t)(ko_e[KO_SUPPRESS] >> 8) & KO_OPT_NO_UNREGISTER_ON_OTHER)) {
        ko_end(s_pos[ko_slot], false);
    }
#endif

    mark_interrupted(pos);

    /* One-shot layer: it applied to this key's lookup, now it is spent --
     * unless this key is itself a layer key (stack one-shots / MO). */
    if (osl_layer != 0xFFu && kc != KC_NO && !IS_QK_ONE_SHOT_LAYER(kc) &&
        !IS_QK_MOMENTARY(kc) && !IS_QK_ONE_SHOT_MOD(kc)) {
        layer_off(osl_layer);
        osl_layer = 0xFFu;
    }

#ifdef ACTION_TAP_DANCE
    if (IS_QK_TAP_DANCE(kc) && action_hal_tap_dance((uint8_t)kc, td_e)) {
        td_pos   = pos;
        td_count = 1;
        td_held  = 1;
        td_time  = time;
        return;
    }
#endif

    if (is_dual(kc)) {
        if (qt_pos == pos && elapsed(qt_time) < action_config.quick_tap_term) {
            /* Quick tap: tapped and pressed again right away -> the tap
             * keycode is held (auto-repeat), no hold decision. */
            const uint8_t s = slot_add(pos, kc, ROLE_TAP);
            if (s != 0xFFu) {
                tap_press(s, pos, kc);
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

static void slot_release(uint8_t slot)
{
    const uint8_t  pos  = s_pos[slot];
    const uint16_t kc   = s_kc[slot];
    const uint8_t  role = s_role[slot];
    const uint8_t  intr = s_intr[slot];
    s_pos[slot] = POS_NONE;

    switch (role) {
        case ROLE_PLAIN:
            release_plain(slot, pos, kc);
            break;

        case ROLE_HOLD: {
            hold_off(kc);
            const uint16_t tap = dual_tap_kc(kc);
            if ((action_config.flags & ACTION_RETRO_TAPPING) && !intr && tap != KC_NO) {
                s_pos[slot]  = pos;
                s_role[slot] = ROLE_PLAIN;
                s_intr[slot] = 0;
                s_mods[slot] = 0;
                press_output(slot, pos, tap);
                s_pos[slot] = POS_NONE;
#ifdef ACTION_KEY_OVERRIDE
                if (s_role[slot] == ROLE_KO) {
                    ko_end(pos, false);
                    if (s_mods[slot]) {
                        action_hal_mods(s_mods[slot], false);
                    }
                } else
#endif
                {
                    release_plain(slot, pos, s_kc[slot]);
                }
            }
            break;
        }

        case ROLE_TAP:
            /* s_kc is the tap's output keycode (press_output), or the TT key. */
            if (dual_tap_kc(kc) != KC_NO) {
                send(pos, dual_tap_kc(kc), false);
                qt_pos  = pos;
                qt_time = action_hal_timer();
            }
            if (s_mods[slot]) {
                action_hal_mods(s_mods[slot], false);
            }
            break;

        case ROLE_OSM: {
            const uint8_t m = mods8((uint8_t)(kc & 0x1Fu));
#ifdef ACTION_EXTRAS
            /* One-shot tap toggle: tapped osk_tap_toggle times in a row ->
             * locked on; the next tap unlocks. */
            if (osm_lock & m) {
                osm_lock &= (uint8_t)~m;
                osm_taps = 0;
                action_hal_mods(m, false);
                break;
            }
            if (!intr && action_config.osk_tap_toggle) {
                osm_taps  = (osm_tap_m == m) ? (uint8_t)(osm_taps + 1u) : 1u;
                osm_tap_m = m;
                if (osm_taps >= action_config.osk_tap_toggle) {
                    osm_lock |= m;
                    osm_mods &= (uint8_t)~m;
                    osm_taps = 0;
                    break; /* stays down */
                }
            }
#endif
            action_hal_mods(m, false);
            if (!intr) {
                osm_mods |= m; /* tapped alone: arm for the next key */
                osm_time = action_hal_timer();
            }
            break;
        }

#ifdef ACTION_EXTRAS
        case ROLE_SC: {
            /* Space Cadet: tapped alone within the tapping term -> tap mod +
             * key; otherwise it was just the hold mod. */
            const __code uint8_t *sc = sc_keys[(uint8_t)(kc - QK_SPACE_CADET_LEFT_CTRL_PARENTHESIS_OPEN)];
            if (intr || elapsed(sc_time) >= action_config.tapping_term) {
                action_hal_mods(sc[0], false);
                break;
            }
            if (sc[0] != sc[1]) {
                action_hal_mods(sc[0], false);
                action_hal_mods(sc[1], true);
            }
            send(pos, sc[2], true);
            send(pos, sc[2], false);
            action_hal_mods(sc[1], false);
            break;
        }
#endif

        case ROLE_OSL:
            if (intr) {
                layer_off((uint8_t)(kc & 0x1Fu));
            } else {
                osl_layer = (uint8_t)(kc & 0x1Fu); /* stays on for the next key */
                osl_time  = action_hal_timer();
            }
            break;

#ifdef ACTION_KEY_OVERRIDE
        case ROLE_KO:
            ko_end(pos, false);
            if (s_mods[slot]) {
                action_hal_mods(s_mods[slot], false);
            }
            if (ko_prev_role == ROLE_TAP && dual_tap_kc(ko_trigger) != KC_NO) {
                qt_pos  = pos;
                qt_time = action_hal_timer();
            }
            break;
#endif

        default:
            break;
    }
}

static void do_release(uint8_t pos)
{
    const uint8_t slot = slot_find(pos);
    if (slot != 0xFFu) {
        slot_release(slot);
    }
}

/* -------------------------------------------------------------- tap dance
 *
 * Vial's tap dance (vial-qmk quantum/vial.c on top of QMK process_tap_dance):
 * every press of the dancing key counts; the dance finishes when its tapping
 * term passes after the last press, or at once when another key is pressed
 * (interrupted). What it does then:
 *
 *   1 press, released or interrupted   on tap        (held while the key is)
 *   1 press, still held                on hold, else on tap
 *   2 presses, released                on double tap, else tap + on tap
 *   2 presses, still held              on tap + hold, else tap + (on hold or on tap)
 *   2 presses, interrupted             tap + on tap
 *   3+ presses                         on tap, once per press (3 at the third)
 *
 * The output is a full keycode (layer keys, one-shots, modded keys), pressed
 * through a slot at the dancing key, so the key's release releases it.
 */
#ifdef ACTION_TAP_DANCE
static void td_press(uint8_t pos, uint16_t kc)
{
    if (kc == KC_NO) {
        return;
    }
    const uint8_t slot = slot_add(pos, kc, ROLE_PLAIN);
    if (slot != 0xFFu) {
        press_plain(slot, pos, kc);
    }
}

static void td_tap(uint8_t pos, uint16_t kc)
{
    if (kc == KC_NO) {
        return;
    }
    const uint8_t slot = slot_add(pos, kc, ROLE_PLAIN);
    if (slot != 0xFFu) {
        press_plain(slot, pos, kc);
        slot_release(slot);
    }
}

static uint16_t td_term(void)
{
    return td_e[TD_TERM] ? td_e[TD_TERM] : action_config.tapping_term;
}

static void td_finish(bool interrupted)
{
    const uint8_t  pos  = td_pos;
    const uint16_t tap  = td_e[TD_ON_TAP];
    const uint16_t hold = td_e[TD_ON_HOLD];
    uint16_t       kc   = KC_NO;
    td_pos              = POS_NONE;

    if (td_count == 1u) {
        kc = (interrupted || !td_held || hold == KC_NO) ? tap : hold;
    } else if (td_count == 2u) {
        if (interrupted) {
            td_tap(pos, tap);
            kc = tap;
        } else if (td_held) {
            if (td_e[TD_ON_TAP_HOLD] != KC_NO) {
                kc = td_e[TD_ON_TAP_HOLD];
            } else {
                td_tap(pos, tap);
                kc = (hold != KC_NO) ? hold : tap;
            }
        } else if (td_e[TD_ON_DOUBLE] != KC_NO) {
            kc = td_e[TD_ON_DOUBLE];
        } else {
            td_tap(pos, tap);
            kc = tap;
        }
    }
    /* Held: stays down until the key is released. Released: a tap. */
    if (td_held) {
        td_press(pos, kc);
    } else {
        td_tap(pos, kc);
    }
}

static void td_again(uint16_t time)
{
    if (td_count < 0xFFu) {
        td_count++;
    }
    td_held = 1;
    td_time = time;
    if (td_count >= 3u) {
        const uint8_t n = (td_count == 3u) ? 3u : 1u;
        for (uint8_t i = 0; i < n; i++) {
            td_tap(td_pos, td_e[TD_ON_TAP]);
        }
    }
}
#endif

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
    const uint8_t s = slot_add(tk_pos, tk_kc, ROLE_TAP);
    if (s != 0xFFu) {
        tap_press(s, tk_pos, tk_kc);
        if (wb_count && s_mods[s]) {
#ifdef ACTION_KEY_OVERRIDE
            const uint8_t drop = (s_role[s] == ROLE_KO) ? (uint8_t)(s_mods[s] & ~ko_lifted) : s_mods[s];
#else
            const uint8_t drop = s_mods[s];
#endif
            if (drop) {
                action_hal_mods(drop, false);
            }
            s_mods[s] = 0;
        }
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
#ifdef ACTION_TAP_DANCE
        /* (A dance and an undecided tap-hold key never overlap: a tap-hold
         * press interrupts the dance, and a TD press during a tap-hold
         * decision is buffered like any other key.) */
        if (td_pos != POS_NONE) {
            if (pos == td_pos) {
                if (down) {
                    td_again(time);
                } else {
                    td_held = 0;
                }
                return;
            }
            if (down) {
                td_finish(true);
            }
        }
#endif
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

/* ------------------------------------------------------------------ combos
 *
 * A stage in front of the queue. A press whose keycode is an input of some
 * combo is held back (cb_*) while more presses may complete a combo:
 *   - the held-back keys are exactly a combo's inputs, and no bigger combo
 *     could still form: the combo fires;
 *   - the combo term runs out, any key is released, or a press that fits no
 *     combo arrives: the best complete combo fires if there is one,
 *     else the held-back presses go on as ordinary keys, in order.
 * A fired combo presses its virtual key; the release of the FIRST of its keys
 * releases it, and the releases of all its keys are swallowed (QMK default).
 */
#ifdef ACTION_COMBO
/* Number of inputs of combo idx (loaded into ce), 0 if unused. */
static uint8_t combo_load(uint8_t idx)
{
    if (!action_hal_combo(idx, ce) || ce[COMBO_OUTPUT] == KC_NO) {
        return 0;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < COMBO_INPUTS; i++) {
        if (ce[i] != KC_NO) {
            n++;
        }
    }
    return n < 2u ? 0 : n;
}

static bool ce_has(uint16_t kc)
{
    for (uint8_t i = 0; i < COMBO_INPUTS; i++) {
        if (ce[i] == kc) {
            return true;
        }
    }
    return false;
}

/* Inputs of combo idx if the held-back keys plus `extra` (KC_NO: none) are
 * all inputs of it, else 0. */
static uint8_t combo_fits(uint8_t idx, uint16_t extra)
{
    const uint8_t n = combo_load(idx);
    if (n == 0 || (extra != KC_NO && !ce_has(extra))) {
        return 0;
    }
    for (uint8_t i = 0; i < cb_n; i++) {
        if (!ce_has(cb_kc[i])) {
            return 0;
        }
    }
    return n;
}

static bool combo_any_fits(uint16_t extra)
{
    for (uint8_t idx = 0; idx < COMBO_MAX; idx++) {
        if (combo_fits(idx, extra)) {
            return true;
        }
    }
    return false;
}

/* The first combo whose inputs are exactly the held-back keys (0xFF: none);
 * *bigger: some combo with more inputs could still form. */
static uint8_t combo_complete(bool *bigger)
{
    uint8_t found = 0xFFu;
    *bigger       = false;
    for (uint8_t idx = 0; idx < COMBO_MAX; idx++) {
        const uint8_t n = combo_fits(idx, KC_NO);
        if (n == cb_n) {
            if (found == 0xFFu) {
                found = idx;
            }
        } else if (n > cb_n) {
            *bigger = true;
        }
    }
    return found;
}

static void combo_fire(uint8_t idx)
{
    for (uint8_t i = 0; i < cb_n; i++) {
        for (uint8_t s = 0; s < COMBO_SWLEN; s++) {
            if (sw_pos[s] == POS_NONE) {
                sw_pos[s] = cb_pos[i];
                sw_idx[s] = idx;
                break;
            }
        }
    }
    combo_down |= (uint16_t)(1u << idx);
    q_push_back(POS(COMBO_ROW, idx), 1u, cb_time[cb_n - 1u]);
    cb_n = 0;
}

/* Settle the held-back keys: fire the best complete combo, or let them go. */
static void combo_settle(void)
{
    bool          bigger;
    const uint8_t idx = combo_complete(&bigger);
    if (idx != 0xFFu) {
        combo_fire(idx);
        return;
    }
    for (uint8_t i = 0; i < cb_n; i++) {
        q_push_back(cb_pos[i], 1u, cb_time[i]);
    }
    cb_n = 0;
}

/* A key that belongs to a fired combo was released: swallowed, and the first
 * one releases the combo. False if pos is not such a key. */
static bool combo_swallow(uint8_t pos, uint16_t time)
{
    for (uint8_t s = 0; s < COMBO_SWLEN; s++) {
        if (sw_pos[s] == pos) {
            const uint8_t  idx = sw_idx[s];
            const uint16_t bit = (uint16_t)(1u << idx);
            sw_pos[s]          = POS_NONE;
            if (combo_down & bit) {
                combo_down &= (uint16_t)~bit;
                q_push_back(POS(COMBO_ROW, idx), 0u, time);
            }
            return true;
        }
    }
    return false;
}

static void combo_input(uint8_t pos, uint8_t down, uint16_t time)
{
    if (!down) {
        /* Any release ends the wait (keeps the event order intact). */
        if (cb_n) {
            combo_settle();
        }
        if (!combo_swallow(pos, time)) {
            q_push_back(pos, 0u, time);
        }
        return;
    }

    uint16_t kc = resolve(pos);
    if (cb_n) {
        bool dup = false;
        for (uint8_t i = 0; i < cb_n; i++) {
            dup |= (cb_kc[i] == kc);
        }
        if (dup || kc == KC_NO || cb_n >= COMBO_BUF || !combo_any_fits(kc)) {
            combo_settle(); /* this press starts afresh below */
            process();
            kc = resolve(pos);
        }
    }
    if (cb_n == 0 && (
#    ifdef ACTION_EXTRAS
                         combos_off ||
#    endif
                         kc == KC_NO || !combo_any_fits(kc))) {
        q_push_back(pos, 1u, time);
        return;
    }
    cb_pos[cb_n]  = pos;
    cb_kc[cb_n]   = kc;
    cb_time[cb_n] = time;
    cb_n++;

    bool          bigger;
    const uint8_t idx = combo_complete(&bigger);
    if (idx != 0xFFu && !bigger) {
        combo_fire(idx);
    }
}
#endif

void action_event(uint8_t row, uint8_t col, bool pressed)
{
#ifdef ACTION_COMBO
#    ifdef ACTION_EXTRAS
    if (combos_off && cb_n == 0 && combo_down == 0) {
        q_push_back(POS(row, col), pressed ? 1u : 0u, action_hal_timer());
    } else
#    endif
        combo_input(POS(row, col), pressed ? 1u : 0u, action_hal_timer());
#else
    q_push_back(POS(row, col), pressed ? 1u : 0u, action_hal_timer());
#endif
    process();
}

void action_task(void)
{
#ifdef ACTION_KEY_OVERRIDE
    if (ko_slot != 0xFFu && !ko_mods_match()) {
        ko_end(s_pos[ko_slot], !((uint8_t)(ko_e[KO_SUPPRESS] >> 8) & KO_OPT_NO_REREGISTER_TRIGGER));
    }
#endif
#ifdef ACTION_COMBO
    if (cb_n && elapsed(cb_time[0]) >= action_config.combo_term) {
        combo_settle();
        process();
    }
#endif
    if (tapping && elapsed(tk_time) >= action_config.tapping_term) {
        decide_hold();
        process();
    }
#ifdef ACTION_TAP_DANCE
    if (td_pos != POS_NONE && elapsed(td_time) >= td_term()) {
        td_finish(false);
    }
#endif
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
    if (tapping || wb_count || osm_mods || osl_layer != 0xFFu
#ifdef ACTION_KEY_OVERRIDE
        || ko_slot != 0xFFu
#endif
    ) {
        return false;
    }
#ifdef ACTION_TAP_DANCE
    if (td_pos != POS_NONE) {
        return false;
    }
#endif
#ifdef ACTION_COMBO
    if (cb_n || combo_down) {
        return false;
    }
    for (uint8_t i = 0; i < COMBO_SWLEN; i++) {
        if (sw_pos[i] != POS_NONE) {
            return false;
        }
    }
#endif
    for (uint8_t i = 0; i < SLOTS; i++) {
        if (s_pos[i] != POS_NONE) {
            return false;
        }
    }
    return true;
}
