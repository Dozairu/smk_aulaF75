#include "vial.h"

#ifdef VIAL_ENABLE

#    include "action.h"
#    include "dynamic_keymap.h"
#    include "kbdef.h"
#    include "matrix.h"
#    include "raw_hid.h"
#    include <string.h>

#    include VIAL_DEFINITION_HEADER

/*
 * Vial protocol, version 6 -- the subset that needs nothing beyond what SMK
 * already stores: identity, the compressed definition, the encoder map, and
 * the unlock handshake. Wire format follows vial-qmk quantum/vial.c: the
 * request buffer is edited in place and sent back, integers are little-endian
 * except keycodes, which are big-endian like the rest of VIA.
 *
 * QMK settings (0x09..0x0C) expose the action engine's knobs; only settings
 * the engine implements are listed, and Vial GUI shows only listed ones.
 *
 * Dynamic entries (0x0D): tap dances, combos and key overrides, run by the
 * action engine.
 */

#    define VIAL_GET_KEYBOARD_ID       0x00u
#    define VIAL_GET_SIZE              0x01u
#    define VIAL_GET_DEFINITION        0x02u
#    define VIAL_GET_ENCODER           0x03u
#    define VIAL_SET_ENCODER           0x04u
#    define VIAL_GET_UNLOCK_STATUS     0x05u
#    define VIAL_UNLOCK_START          0x06u
#    define VIAL_UNLOCK_POLL           0x07u
#    define VIAL_LOCK                  0x08u
#    define VIAL_QMK_SETTINGS_QUERY    0x09u
#    define VIAL_QMK_SETTINGS_GET      0x0Au
#    define VIAL_QMK_SETTINGS_SET      0x0Bu
#    define VIAL_QMK_SETTINGS_RESET    0x0Cu
#    define VIAL_DYNAMIC_ENTRY_OP      0x0Du

#    define DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES 0x00u
#    define DYNAMIC_VIAL_TAP_DANCE_GET         0x01u
#    define DYNAMIC_VIAL_TAP_DANCE_SET         0x02u
#    define DYNAMIC_VIAL_COMBO_GET             0x03u
#    define DYNAMIC_VIAL_COMBO_SET             0x04u
#    define DYNAMIC_VIAL_KEY_OVERRIDE_GET      0x05u
#    define DYNAMIC_VIAL_KEY_OVERRIDE_SET      0x06u

/* VIAL_UNLOCK_ROWS / _COLS come from the generated header (board options
 * vial_unlock_rows / vial_unlock_cols); the generator refuses an empty list. */
static const __code uint8_t unlock_rows[] = VIAL_UNLOCK_ROWS;
static const __code uint8_t unlock_cols[] = VIAL_UNLOCK_COLS;

#    define UNLOCK_KEYS ((uint8_t)sizeof(unlock_rows))

_Static_assert(sizeof(unlock_rows) == sizeof(unlock_cols), "vial unlock rows/cols must pair up");
_Static_assert(sizeof(unlock_rows) < 15u, "vial unlock status reports at most 14 keys");

static const __code uint8_t keyboard_uid[8] = VIAL_KEYBOARD_UID;

/* vial-qmk: 50 steps of 100 ms of continuous hold, ~5 s. */
#    define UNLOCK_COUNTER_MAX 50u
/* 100 ms in tick-ISR calls (~400 us each, 2.5 per ms). */
#    define UNLOCK_STEP_TICKS 250u

static __bit                     unlocked;
static __bit                     unlock_busy;
static __xdata uint8_t           unlock_counter;
static __xdata uint16_t          unlock_mark;
static volatile __xdata uint16_t ticks;

void vial_tick(void)
{
    ticks++;
}

static uint16_t ticks_now(void)
{
    uint16_t t;
    __critical
    {
        t = ticks;
    }
    return t;
}

bool vial_unlocked(void)
{
    return unlocked;
}

bool vial_unlock_in_progress(void)
{
    return unlock_busy;
}

bool vial_allowed_during_unlock(const __xdata uint8_t *data)
{
    if (data[0] != VIAL_PREFIX) {
        return false;
    }
    switch (data[1]) {
        case VIAL_GET_KEYBOARD_ID:
        case VIAL_GET_SIZE:
        case VIAL_GET_DEFINITION:
        case VIAL_GET_UNLOCK_STATUS:
        case VIAL_UNLOCK_START:
        case VIAL_UNLOCK_POLL:
            return true;
        default:
            return false;
    }
}

static bool unlock_keys_held(void)
{
    for (uint8_t i = 0; i < UNLOCK_KEYS; i++) {
        if (!(matrix_get_col(unlock_cols[i]) & (uint8_t)(1u << unlock_rows[i]))) {
            return false;
        }
    }
    return true;
}

static void unlock_poll(void)
{
    if (!unlock_busy) {
        return;
    }
    if (!unlock_keys_held()) {
        unlock_counter = UNLOCK_COUNTER_MAX;
        return;
    }
    const uint16_t now = ticks_now();
    if ((uint16_t)(now - unlock_mark) > UNLOCK_STEP_TICKS) {
        unlock_mark = now;
        if (--unlock_counter == 0) {
            unlock_busy = 0;
            unlocked    = 1;
        }
    }
}

/* ------------------------------------------------------------ QMK settings
 *
 * Stored in the Vial area of the dynamic store, little-endian:
 *   [0..1] tapping term   [2..3] quick tap term   [4..5] one-shot timeout
 *   [6]    flags (ACTION_PERMISSIVE_HOLD | ...)    [7]    marker
 *   [8..9] combo term
 * QSIDs and wire widths follow vial-gui's qmk_settings.json: integers are
 * u16 LE, the three tap-hold booleans are each their own QSID carrying 0/1.
 */
#    define QS_TAPPING_TERM   0u
#    define QS_QUICK_TAP_TERM 2u
#    define QS_OSK_TIMEOUT    4u
#    define QS_FLAGS          6u
#    define QS_MARKER         7u
#    define QS_MARKER_VALUE   0x51u
#    define QS_COMBO_TERM     8u

#    define QSID_COMBO_TERM        2u
#    define QSID_OSK_TIMEOUT       6u
#    define QSID_TAPPING_TERM      7u
#    define QSID_PERMISSIVE_HOLD   22u
#    define QSID_HOLD_ON_OTHER_KEY 23u
#    define QSID_RETRO_TAPPING     24u
#    define QSID_QUICK_TAP_TERM    25u

/* Ascending: the query pages through them in order. */
static const __code uint8_t qsids[] = {
    QSID_COMBO_TERM,      QSID_OSK_TIMEOUT,   QSID_TAPPING_TERM,  QSID_PERMISSIVE_HOLD,
    QSID_HOLD_ON_OTHER_KEY, QSID_RETRO_TAPPING, QSID_QUICK_TAP_TERM,
};

static uint16_t qs_rd16(uint8_t off)
{
    const __xdata uint8_t *s = dynamic_vial_store();
    return (uint16_t)s[off] | (uint16_t)((uint16_t)s[off + 1u] << 8);
}

static void qs_wr16(uint8_t off, uint16_t v)
{
    __xdata uint8_t *s = dynamic_vial_store();
    s[off]      = (uint8_t)v;
    s[off + 1u] = (uint8_t)(v >> 8);
}

/* ---------------------------------- tap dances, combos, key overrides
 *
 * All three are tables of 10-byte entries, five u16 LE, in the Vial area -- the
 * vial-qmk structs, which are also the wire format of get/set:
 *   tap dance (VIAL_TD_OFFSET):    on tap, on hold, on double tap,
 *                                  on tap + hold, tapping term
 *   combo     (VIAL_COMBO_OFFSET): input 1..4 (KC_NO = unused), output
 *   key override (VIAL_KO_OFFSET):  trigger, replacement, layer mask,
 *                                  trigger mods, negative mods, suppressed
 *                                  mods, options (the last four are bytes)
 */
static __xdata uint8_t *entry_ptr(uint8_t table, uint8_t index)
{
    return dynamic_vial_store() + table + (uint16_t)index * VIAL_ENTRY_BYTES;
}

static void entry_read(uint8_t table, uint8_t index, __xdata uint16_t *entry)
{
    const __xdata uint8_t *p = entry_ptr(table, index);
    for (uint8_t i = 0; i < VIAL_ENTRY_BYTES / 2u; i++, p += 2) {
        entry[i] = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
    }
}

bool action_hal_tap_dance(uint8_t index, __xdata uint16_t *entry)
{
    if (index >= VIAL_TAP_DANCE_ENTRIES) {
        return false;
    }
    entry_read(VIAL_TD_OFFSET, index, entry);
    return true;
}

bool action_hal_combo(uint8_t index, __xdata uint16_t *entry)
{
    if (index >= VIAL_COMBO_ENTRIES) {
        return false;
    }
    entry_read(VIAL_COMBO_OFFSET, index, entry);
    return true;
}

bool action_hal_key_override(uint8_t index, __xdata uint16_t *entry)
{
    if (index >= VIAL_KEY_OVERRIDE_ENTRIES) {
        return false;
    }
    entry_read(VIAL_KO_OFFSET, index, entry);
    return true;
}

_Static_assert(TD_FIELDS * 2u == VIAL_ENTRY_BYTES && COMBO_FIELDS * 2u == VIAL_ENTRY_BYTES,
               "Vial entries are five u16");
_Static_assert(VIAL_COMBO_ENTRIES <= COMBO_MAX, "the engine addresses at most COMBO_MAX combos");
_Static_assert(VIAL_KEY_OVERRIDE_ENTRIES <= KO_MAX, "the engine addresses at most KO_MAX key overrides");

/* Key override entries that are all zero (never written: storage version 4
 * reserved them zeroed) get vial-qmk's defaults. True if any changed. */
static bool ko_fill_defaults(void)
{
    bool changed = false;
    for (uint8_t i = 0; i < VIAL_KEY_OVERRIDE_ENTRIES; i++) {
        __xdata uint8_t *p   = entry_ptr(VIAL_KO_OFFSET, i);
        uint8_t          any = 0;
        for (uint8_t b = 0; b < VIAL_ENTRY_BYTES; b++) {
            any |= p[b];
        }
        if (!any) {
            p[4]    = 0xFFu; /* layers: all */
            p[5]    = 0xFFu;
            p[9]    = KO_OPT_ACT_TRIGGER_DOWN | KO_OPT_ACT_REQUIRED_MOD_DOWN | KO_OPT_ACT_NEGATIVE_MOD_UP;
            changed = true;
        }
    }
    return changed;
}

static void entries_defaults(void)
{
    /* vial-qmk: everything KC_NO; tap dances get tapping term TAPPING_TERM;
     * key overrides all layers and the three activations, not enabled. */
    memset(entry_ptr(VIAL_TD_OFFSET, 0), 0, DYNAMIC_VIAL_BYTES - VIAL_TD_OFFSET);
    for (uint8_t i = 0; i < VIAL_TAP_DANCE_ENTRIES; i++) {
        __xdata uint8_t *p = entry_ptr(VIAL_TD_OFFSET, i);
        p[8] = (uint8_t)ACTION_TAPPING_TERM_DEFAULT;
        p[9] = (uint8_t)(ACTION_TAPPING_TERM_DEFAULT >> 8);
    }
    ko_fill_defaults();
}

/* QMK settings only (the GUI's "Reset to defaults" button). */
static void qs_defaults(void)
{
    __xdata uint8_t *s = dynamic_vial_store();
    memset(s, 0, VIAL_SETTINGS_BYTES);
    qs_wr16(QS_TAPPING_TERM, ACTION_TAPPING_TERM_DEFAULT);
    qs_wr16(QS_QUICK_TAP_TERM, ACTION_TAPPING_TERM_DEFAULT);
    qs_wr16(QS_OSK_TIMEOUT, ACTION_OSK_TIMEOUT_DEFAULT);
    qs_wr16(QS_COMBO_TERM, ACTION_COMBO_TERM_DEFAULT);
    s[QS_FLAGS]  = 0;
    s[QS_MARKER] = QS_MARKER_VALUE;
}

/* Everything Vial keeps: new store, factory reset. */
void vial_store_defaults(void)
{
    qs_defaults();
    entries_defaults();
}

void vial_store_apply(void)
{
    if (dynamic_vial_store()[QS_MARKER] != QS_MARKER_VALUE) {
        vial_store_defaults();
        dynamic_vial_mark_dirty();
    }
    if (ko_fill_defaults()) {
        dynamic_vial_mark_dirty();
    }
    action_config.tapping_term   = qs_rd16(QS_TAPPING_TERM);
    action_config.quick_tap_term = qs_rd16(QS_QUICK_TAP_TERM);
    action_config.osk_timeout    = qs_rd16(QS_OSK_TIMEOUT);
    action_config.combo_term     = qs_rd16(QS_COMBO_TERM);
    action_config.flags          = dynamic_vial_store()[QS_FLAGS];
}

static uint8_t qsid_flag(uint16_t qsid)
{
    switch (qsid) {
        case QSID_PERMISSIVE_HOLD:   return ACTION_PERMISSIVE_HOLD;
        case QSID_HOLD_ON_OTHER_KEY: return ACTION_HOLD_ON_OTHER_KEY_PRESS;
        case QSID_RETRO_TAPPING:     return ACTION_RETRO_TAPPING;
        default:                     return 0;
    }
}

static uint8_t qsid_u16_offset(uint16_t qsid)
{
    switch (qsid) {
        case QSID_TAPPING_TERM:   return QS_TAPPING_TERM;
        case QSID_QUICK_TAP_TERM: return QS_QUICK_TAP_TERM;
        case QSID_OSK_TIMEOUT:    return QS_OSK_TIMEOUT;
        case QSID_COMBO_TERM:     return QS_COMBO_TERM;
        default:                  return 0xFFu;
    }
}

static void qmk_settings_query(__xdata uint8_t *data)
{
    const uint16_t gt = (uint16_t)data[2] | (uint16_t)((uint16_t)data[3] << 8);
    uint8_t        o  = 0;
    memset(data, 0xFF, RAW_HID_REPORT_SIZE); /* 0xFFFF = end of list */
    for (uint8_t i = 0; i < sizeof(qsids); i++) {
        if (qsids[i] > gt) {
            data[o++] = qsids[i];
            data[o++] = 0;
        }
    }
}

static void qmk_settings_get(__xdata uint8_t *data)
{
    const uint16_t qsid = (uint16_t)data[2] | (uint16_t)((uint16_t)data[3] << 8);
    const uint8_t  off  = qsid_u16_offset(qsid);
    const uint8_t  flag = qsid_flag(qsid);

    memset(data, 0, RAW_HID_REPORT_SIZE);
    if (off != 0xFFu) {
        const uint16_t v = qs_rd16(off);
        data[1] = (uint8_t)v;
        data[2] = (uint8_t)(v >> 8);
    } else if (flag) {
        data[1] = (dynamic_vial_store()[QS_FLAGS] & flag) ? 1u : 0u;
    } else {
        data[0] = 0xFFu; /* unknown setting */
    }
}

static void qmk_settings_set(__xdata uint8_t *data)
{
    const uint16_t qsid = (uint16_t)data[2] | (uint16_t)((uint16_t)data[3] << 8);
    const uint8_t  off  = qsid_u16_offset(qsid);
    const uint8_t  flag = qsid_flag(qsid);

    if (off != 0xFFu) {
        qs_wr16(off, (uint16_t)data[4] | (uint16_t)((uint16_t)data[5] << 8));
    } else if (flag) {
        __xdata uint8_t *s = dynamic_vial_store();
        if (data[4] & 1u) {
            s[QS_FLAGS] |= flag;
        } else {
            s[QS_FLAGS] &= (uint8_t)~flag;
        }
    } else {
        data[0] = 0xFFu;
        return;
    }
    vial_store_apply();
    dynamic_vial_mark_dirty();
    data[0] = 0;
}

/* 0x0D, sub-command in data[2], entry index in data[3]. Replies: data[0] =
 * 0 ok / 0xFF (-1) bad index; a get returns the entry from data[1], a set
 * takes it from data[4]. */
static void dynamic_entry_op(__xdata uint8_t *data)
{
    const uint8_t idx = data[3];
    uint8_t       table, count;
    switch (data[2]) {
        case DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES:
            /* [0] tap dance, [1] combo, [2] key override, [3] alt repeat,
             * [31] feature bits (Caps Word, Layer Lock). */
            memset(data, 0, RAW_HID_REPORT_SIZE);
            data[0] = VIAL_TAP_DANCE_ENTRIES;
            data[1] = VIAL_COMBO_ENTRIES;
            data[2] = VIAL_KEY_OVERRIDE_ENTRIES;
            return;
        case DYNAMIC_VIAL_TAP_DANCE_GET:
        case DYNAMIC_VIAL_TAP_DANCE_SET:
            table = VIAL_TD_OFFSET;
            count = VIAL_TAP_DANCE_ENTRIES;
            break;
        case DYNAMIC_VIAL_COMBO_GET:
        case DYNAMIC_VIAL_COMBO_SET:
            table = VIAL_COMBO_OFFSET;
            count = VIAL_COMBO_ENTRIES;
            break;
        case DYNAMIC_VIAL_KEY_OVERRIDE_GET:
        case DYNAMIC_VIAL_KEY_OVERRIDE_SET:
            table = VIAL_KO_OFFSET;
            count = VIAL_KEY_OVERRIDE_ENTRIES;
            break;
        default:
            data[0] = 0xFFu;
            return;
    }

    if (idx >= count) {
        data[0] = 0xFFu;
    } else if (data[2] & 1u) { /* odd sub-commands are the gets */
        memset(data, 0, RAW_HID_REPORT_SIZE);
        memcpy(&data[1], entry_ptr(table, idx), VIAL_ENTRY_BYTES);
    } else {
        memcpy(entry_ptr(table, idx), &data[4], VIAL_ENTRY_BYTES);
        dynamic_vial_mark_dirty();
        data[0] = 0;
    }
}

void vial_process(__xdata uint8_t *data)
{
    switch (data[1]) {
        case VIAL_GET_KEYBOARD_ID:
            memset(data, 0, RAW_HID_REPORT_SIZE);
            data[0] = VIAL_PROTOCOL_VERSION; /* u32 LE: 6, 0, 0, 0 */
            for (uint8_t i = 0; i < 8u; i++) {
                data[4u + i] = keyboard_uid[i];
            }
            data[12] = 0; /* no VialRGB (yet) */
            break;

        case VIAL_GET_SIZE:
            data[0] = (uint8_t)VIAL_DEFINITION_SIZE;
            data[1] = (uint8_t)(VIAL_DEFINITION_SIZE >> 8);
            data[2] = 0;
            data[3] = 0;
            break;

        case VIAL_GET_DEFINITION: {
            /* Page index, u16 LE (the GUI sends a u32; the high half is 0). */
            const uint16_t page  = (uint16_t)data[2] | (uint16_t)((uint16_t)data[3] << 8);
            const uint16_t start = (uint16_t)(page * RAW_HID_REPORT_SIZE);
            if (page >= (uint16_t)((VIAL_DEFINITION_SIZE + RAW_HID_REPORT_SIZE - 1u) / RAW_HID_REPORT_SIZE)) {
                break; /* past the end: echo, as vial-qmk does */
            }
            uint16_t n = (uint16_t)(VIAL_DEFINITION_SIZE - start);
            if (n > RAW_HID_REPORT_SIZE) {
                n = RAW_HID_REPORT_SIZE;
            }
            for (uint8_t i = 0; i < (uint8_t)n; i++) {
                data[i] = vial_definition[start + i];
            }
            break;
        }

        case VIAL_GET_ENCODER: {
            /* Direction 0 = counter-clockwise, 1 = clockwise, like QMK's encoder map. */
            const uint16_t ccw = dynamic_encoder_get_keycode(data[2], data[3], false);
            const uint16_t cw  = dynamic_encoder_get_keycode(data[2], data[3], true);
            data[0] = (uint8_t)(ccw >> 8);
            data[1] = (uint8_t)ccw;
            data[2] = (uint8_t)(cw >> 8);
            data[3] = (uint8_t)cw;
            break;
        }

        case VIAL_SET_ENCODER:
            dynamic_encoder_set_keycode(data[2], data[3], data[4] != 0,
                                        (uint16_t)((uint16_t)data[5] << 8) | data[6]);
            break;

        case VIAL_GET_UNLOCK_STATUS:
            memset(data, 0xFF, RAW_HID_REPORT_SIZE);
            data[0] = unlocked ? 1 : 0;
            data[1] = unlock_busy ? 1 : 0;
            for (uint8_t i = 0; i < UNLOCK_KEYS; i++) {
                data[2u + 2u * i] = unlock_rows[i];
                data[3u + 2u * i] = unlock_cols[i];
            }
            break;

        case VIAL_UNLOCK_START:
            unlock_busy    = 1;
            unlock_counter = UNLOCK_COUNTER_MAX;
            unlock_mark    = ticks_now();
            break;

        case VIAL_UNLOCK_POLL:
            unlock_poll();
            data[0] = unlocked ? 1 : 0;
            data[1] = unlock_busy ? 1 : 0;
            data[2] = unlock_counter;
            break;

        case VIAL_LOCK:
            unlocked = 0;
            break;

        case VIAL_QMK_SETTINGS_QUERY:
            qmk_settings_query(data);
            break;

        case VIAL_QMK_SETTINGS_GET:
            qmk_settings_get(data);
            break;

        case VIAL_QMK_SETTINGS_SET:
            qmk_settings_set(data);
            break;

        case VIAL_QMK_SETTINGS_RESET:
            qs_defaults();
            vial_store_apply();
            dynamic_vial_mark_dirty();
            break;

        case VIAL_DYNAMIC_ENTRY_OP:
            dynamic_entry_op(data);
            break;

        default:
            break; /* unknown: echo */
    }
}

#endif
