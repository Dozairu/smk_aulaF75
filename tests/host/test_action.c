/*
 * Host unit tests for src/smk/action.c (layers, tap-hold, one-shot).
 *
 * Built with the host compiler by meson (`meson test -C <builddir> action`).
 * The HAL is faked: a 4-layer keymap, a settable millisecond clock, and a
 * recorder that turns every output into a token, so each test compares the
 * exact output sequence, e.g. "+A -A" or "m+01 +B -B m-01".
 *
 *   +XX / -XX   action_hal_send(keycode 0xXX..., pressed / released)
 *   m+MM / m-MM action_hal_mods(mask 0xMM, on / off)
 */

#include "action.h"
#include "keycodes.h"

#include <stdio.h>
#include <string.h>

#define ROWS   6
#define COLS   15
#define LAYERS 4

static uint16_t keymap[LAYERS][ROWS][COLS];
static uint16_t enc_keymap[LAYERS][2];
static uint16_t now_ms;
static char     out[1024];

/* ------------------------------------------------------------------ HAL */

uint16_t action_hal_keymap(uint8_t layer, uint8_t row, uint8_t col)
{
    if (row == ACTION_ENC_ROW) {
        return (layer < LAYERS && col < 2u) ? enc_keymap[layer][col] : KC_NO;
    }
    return (layer < LAYERS && row < ROWS && col < COLS) ? keymap[layer][row][col] : KC_NO;
}

uint8_t action_hal_layer_count(void)
{
    return LAYERS;
}

uint16_t action_hal_timer(void)
{
    return now_ms;
}

/* The host's view, as the report carries it: a press sets the keycode, a
 * release clears it; the mods are a mask. Used by the stress test. */
static bool    host_key[0x10000];
static uint8_t host_mods;

static void emit(const char *tok)
{
    if (strlen(out) + strlen(tok) + 2u >= sizeof out) {
        return; /* long runs (stress test): keep only the start */
    }
    if (out[0]) {
        strcat(out, " ");
    }
    strcat(out, tok);
}

void action_hal_send(uint16_t kc, bool pressed)
{
    char tok[16];
    /* As matrix.c send_keycode(): a modifier keycode and a modded keycode's
     * mods live in the same mask as action_hal_mods(). */
    if (IS_MODIFIER_KEYCODE(kc)) {
        const uint8_t m = (uint8_t)(1u << (kc & 7u));
        host_mods       = pressed ? (uint8_t)(host_mods | m) : (uint8_t)(host_mods & ~m);
    } else if (IS_QK_MODS(kc)) {
        const uint8_t m5     = (uint8_t)QK_MODS_GET_MODS(kc);
        const uint8_t m      = (m5 & 0x10u) ? (uint8_t)((m5 & 0x0Fu) << 4) : m5;
        host_mods            = pressed ? (uint8_t)(host_mods | m) : (uint8_t)(host_mods & ~m);
        host_key[kc & 0xFFu] = pressed;
    } else {
        host_key[kc] = pressed;
    }
    snprintf(tok, sizeof tok, "%c%02X", pressed ? '+' : '-', kc);
    emit(tok);
}

void action_hal_mods(uint8_t mods, bool on)
{
    char tok[16];
    host_mods = on ? (uint8_t)(host_mods | mods) : (uint8_t)(host_mods & ~mods);
    snprintf(tok, sizeof tok, "m%c%02X", on ? '+' : '-', mods);
    emit(tok);
}

void action_hal_event_pos(uint8_t row, uint8_t col)
{
    (void)row;
    (void)col;
}

#define TD_COUNT 2
static uint16_t tds[TD_COUNT][TD_FIELDS];

bool action_hal_tap_dance(uint8_t index, uint16_t *entry)
{
    if (index >= TD_COUNT) {
        return false;
    }
    memcpy(entry, tds[index], sizeof tds[index]);
    return true;
}

#define KO_COUNT 4
static uint16_t kos[KO_COUNT][KO_FIELDS];

bool action_hal_key_override(uint8_t index, uint16_t *entry)
{
    if (index >= KO_COUNT) {
        return false;
    }
    memcpy(entry, kos[index], sizeof kos[index]);
    return true;
}

static int config_changes;

void action_hal_config_changed(void)
{
    config_changes++;
}

#define COMBO_COUNT 4
static uint16_t combos[COMBO_COUNT][COMBO_FIELDS];

bool action_hal_combo(uint8_t index, uint16_t *entry)
{
    if (index >= COMBO_COUNT) {
        return false;
    }
    memcpy(entry, combos[index], sizeof combos[index]);
    return true;
}

/* -------------------------------------------------------------- harness */

static int failures;
static int checks;

static void reset(uint8_t flags)
{
    memset(keymap, 0, sizeof keymap); /* KC_NO everywhere */
    memset(host_key, 0, sizeof host_key);
    host_mods = 0;
    memset(tds, 0, sizeof tds);
    memset(combos, 0, sizeof combos);
    memset(kos, 0, sizeof kos);
    config_changes = 0;
    for (int l = 1; l < LAYERS; l++) {
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                keymap[l][r][c] = KC_TRNS;
            }
        }
        enc_keymap[l][0] = KC_TRNS;
        enc_keymap[l][1] = KC_TRNS;
    }
    enc_keymap[0][0] = KC_NO;
    enc_keymap[0][1] = KC_NO;
    now_ms           = 1000;
    out[0]           = 0;
    action_init();
    action_config_defaults();
    action_config.flags = flags;
}

static void dn(int r, int c)
{
    action_event((uint8_t)r, (uint8_t)c, true);
}

static void up(int r, int c)
{
    action_event((uint8_t)r, (uint8_t)c, false);
}

static void wait_ms(uint16_t ms)
{
    while (ms--) {
        now_ms++;
        action_task();
    }
}

static void expect(const char *name, const char *want)
{
    checks++;
    if (strcmp(out, want) != 0) {
        failures++;
        printf("FAIL %s\n  want: %s\n  got:  %s\n", name, want, out);
    } else {
        printf("ok   %s\n", name);
    }
    out[0] = 0;
}

static void expect_idle(const char *name)
{
    checks++;
    if (!action_idle()) {
        failures++;
        printf("FAIL %s: engine not idle at the end\n", name);
    }
}

/* Positions used below. */
#define A_   0, 0
#define B_   0, 1
#define C_   0, 2
#define FN_  5, 8
#define DUAL 3, 0

/* ---------------------------------------------------------------- tests */

static void t_plain(void)
{
    reset(0);
    keymap[0][0][0] = KC_A;
    dn(A_);
    up(A_);
    expect("plain key", "+04 -04");
    expect_idle("plain key");
}

static void t_mods_keycode_passthrough(void)
{
    reset(0);
    keymap[0][0][0] = LCTL(KC_C);
    dn(A_);
    up(A_);
    expect("modded keycode passes through intact", "+106 -106");
}

static void t_mo(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(FN_);
    dn(A_);
    up(A_);
    up(FN_);
    expect("MO: layer key", "+05 -05");
    dn(A_);
    up(A_);
    expect("MO: back to base", "+04 -04");
    expect_idle("MO");
}

static void t_release_after_layer_change(void)
{
    /* The old engine re-resolved on release: A pressed on base, Fn pressed,
     * A released -> released B. The press cache must release A. */
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(A_);
    dn(FN_);
    up(A_);
    up(FN_);
    expect("release resolves to what was pressed", "+04 -04");

    dn(FN_);
    dn(A_);
    up(FN_);
    up(A_);
    expect("layer key released first still releases B", "+05 -05");
    expect_idle("layer change");
}

static void t_transparent(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[0][0][1] = KC_B; /* layer 1 leaves it KC_TRNS */
    dn(FN_);
    dn(B_);
    up(B_);
    up(FN_);
    expect("transparent falls through", "+05 -05");
}

static void t_tg_to_df(void)
{
    reset(0);
    keymap[0][0][2] = TG(2);
    keymap[2][0][2] = TG(2);
    keymap[0][0][0] = KC_A;
    keymap[2][0][0] = KC_X;
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("TG on", "+1B -1B");
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("TG off", "+04 -04");

    keymap[0][0][2] = TO(3);
    keymap[3][0][0] = KC_Y;
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("TO", "+1C -1C");
    expect_idle("TG/TO");
}

static void t_lt_tap(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    dn(DUAL);
    wait_ms(50);
    up(DUAL);
    expect("LT tap", "+2C -2C");
    expect_idle("LT tap");
}

static void t_lt_hold(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(DUAL);
    wait_ms(250);
    dn(A_);
    up(A_);
    up(DUAL);
    expect("LT hold (timeout)", "+05 -05");
    expect_idle("LT hold");
}

static void t_lt_rolling_default(void)
{
    /* Default mode: other key pressed AND released inside the term, then the
     * dual key released, all within the term -> tap + plain key (rolling). */
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(DUAL);
    wait_ms(20);
    dn(A_);
    wait_ms(20);
    up(A_);
    wait_ms(20);
    up(DUAL);
    expect("LT nested, default -> tap first, then A", "+2C +04 -04 -2C");

    dn(DUAL);
    wait_ms(20);
    dn(A_);
    wait_ms(20);
    up(DUAL);
    up(A_);
    expect("LT rolling over -> tap then A", "+2C +04 -2C -04");
    expect_idle("LT rolling");
}

static void t_mt_permissive_hold(void)
{
    reset(ACTION_PERMISSIVE_HOLD);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][0][1] = KC_B;
    dn(DUAL);
    wait_ms(20);
    dn(B_);
    wait_ms(20);
    up(B_);
    up(DUAL);
    expect("MT permissive hold -> Ctrl+B", "m+01 +05 -05 m-01");

    dn(DUAL);
    wait_ms(20);
    dn(B_);
    wait_ms(20);
    up(DUAL);
    up(B_);
    expect("MT permissive, roll (dual released first) -> tap", "+04 +05 -04 -05");
    expect_idle("MT permissive");
}

static void t_mt_hold_on_other(void)
{
    reset(ACTION_HOLD_ON_OTHER_KEY_PRESS);
    keymap[0][3][0] = MT(MOD_LSFT, KC_A);
    keymap[0][0][1] = KC_B;
    dn(DUAL);
    wait_ms(10);
    dn(B_);
    expect("MT hold-on-other: decided at the press", "m+02 +05");
    up(B_);
    up(DUAL);
    expect("MT hold-on-other: release", "-05 m-02");
    expect_idle("MT hold-on-other");
}

static void t_mt_right_mods(void)
{
    reset(0);
    keymap[0][3][0] = MT(MOD_RALT, KC_A);
    dn(DUAL);
    wait_ms(300);
    up(DUAL);
    expect("MT right-hand mod -> 0x40", "m+40 m-40");
}

static void t_retro_tapping(void)
{
    reset(ACTION_RETRO_TAPPING);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    dn(DUAL);
    wait_ms(300);
    up(DUAL);
    expect("retro tapping: long lone hold still taps", "m+01 m-01 +04 -04");
}

static void t_quick_tap(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    dn(DUAL);
    wait_ms(30);
    up(DUAL);
    wait_ms(50);
    dn(DUAL);
    wait_ms(400);
    expect("quick tap: second press holds the tap key", "+2C -2C +2C");
    up(DUAL);
    expect("quick tap: release", "-2C");

    wait_ms(300);
    dn(DUAL);
    wait_ms(400);
    up(DUAL);
    expect("after quick-tap term: hold again", "");
    expect_idle("quick tap");
}

static void t_buffer_order_on_timeout(void)
{
    /* Press LT, press A, wait past the term: hold decided, A replayed on the
     * layer. Nothing may come out before the decision. */
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(DUAL);
    wait_ms(10);
    dn(A_);
    expect("buffered: nothing yet", "");
    wait_ms(250);
    expect("timeout replays A on layer 1", "+05");
    up(A_);
    up(DUAL);
    expect("release", "-05");
    expect_idle("buffer order");
}

static void t_osm(void)
{
    reset(0);
    keymap[0][0][2] = OSM(MOD_LSFT);
    keymap[0][0][0] = KC_A;
    dn(C_);
    up(C_);
    expect("OSM tap: mods only while held", "m+02 m-02");
    dn(A_);
    up(A_);
    expect("OSM applies to next key only", "m+02 +04 -04 m-02");
    dn(A_);
    up(A_);
    expect("OSM spent", "+04 -04");

    dn(C_);
    up(C_);
    wait_ms(6000);
    dn(A_);
    up(A_);
    expect("OSM times out", "m+02 m-02 +04 -04");

    dn(C_);
    dn(A_);
    up(A_);
    up(C_);
    expect("OSM held = plain mod", "m+02 +04 -04 m-02");
    dn(A_);
    up(A_);
    expect("held OSM does not arm", "+04 -04");
    expect_idle("OSM");
}

static void t_osl(void)
{
    reset(0);
    keymap[0][0][2] = OSL(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("OSL: next key on layer", "+05 -05");
    dn(A_);
    up(A_);
    expect("OSL spent", "+04 -04");
    expect_idle("OSL");
}

static void t_tt(void)
{
    reset(0);
    keymap[0][0][2] = TT(1);
    keymap[1][0][2] = TT(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_);
    wait_ms(300);
    dn(A_);
    up(A_);
    up(C_);
    expect("TT hold = momentary", "+05 -05");
    dn(A_);
    up(A_);
    expect("TT hold released", "+04 -04");
    action_config.tapping_toggle = 1;
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("TT tap toggles on", "+05 -05");
    wait_ms(300);
    dn(C_);
    up(C_);
    dn(A_);
    up(A_);
    expect("TT tap toggles off", "+04 -04");

    action_config.tapping_toggle = 5; /* QMK default */
    wait_ms(300);
    for (int i = 0; i < 4; i++) {
        dn(C_);
        up(C_);
    }
    dn(A_);
    up(A_);
    expect("TT: 4 taps of 5 do not toggle", "+04 -04");
    for (int i = 0; i < 5; i++) {
        dn(C_);
        up(C_);
    }
    dn(A_);
    up(A_);
    expect("TT: 5 taps toggle", "+05 -05");
}

static void t_lm(void)
{
    reset(0);
    keymap[0][0][2] = LM(1, MOD_LCTL);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_);
    dn(A_);
    up(A_);
    up(C_);
    expect("LM: layer + mod", "m+01 +05 -05 m-01");
}

static void t_two_duals(void)
{
    /* Home-row-mod style: two MT keys rolled quickly -> both taps, in order. */
    reset(0);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][3][1] = MT(MOD_LSFT, KC_S);
    dn(3, 0);
    wait_ms(30);
    dn(3, 1);
    wait_ms(30);
    up(3, 0);
    wait_ms(30);
    up(3, 1);
    expect("two MT rolled -> a then s", "+04 -04 +16 -16");
    expect_idle("two duals");
}

static void t_slots_full(void)
{
    reset(0);
    for (int c = 0; c < 12; c++) {
        keymap[0][1][c] = (uint16_t)(KC_A + c);
    }
    for (int c = 0; c < 12; c++) {
        dn(1, c);
    }
    for (int c = 0; c < 12; c++) {
        up(1, c);
    }
    expect("11th+ simultaneous key ignored, no stuck keys", "+04 +05 +06 +07 +08 +09 +0A +0B +0C +0D -04 -05 -06 -07 -08 -09 -0A -0B -0C -0D");
    expect_idle("slots full");
}

/* ------------------------------------------------------------ tap dance */

#define TDK 2, 0 /* the dancing key */

/* TD(0): tap A, hold LCtrl, double tap B, tap + hold C, global term. */
static void td_setup(void)
{
    reset(0);
    keymap[0][2][0]        = TD(0);
    keymap[0][2][1]        = TD(1);
    keymap[0][0][1]        = KC_X;
    tds[0][TD_ON_TAP]      = KC_A;
    tds[0][TD_ON_HOLD]     = KC_LCTL;
    tds[0][TD_ON_DOUBLE]   = KC_B;
    tds[0][TD_ON_TAP_HOLD] = KC_C;
}

static void t_td_basic(void)
{
    td_setup();
    dn(TDK);
    up(TDK);
    expect("TD tap: nothing before the term", "");
    wait_ms(250);
    expect("TD tap", "+04 -04");

    dn(TDK);
    wait_ms(250);
    expect("TD hold: on hold, held", "+E0");
    up(TDK);
    expect("TD hold: released with the key", "-E0");

    dn(TDK);
    up(TDK);
    dn(TDK);
    up(TDK);
    wait_ms(250);
    expect("TD double tap", "+05 -05");

    dn(TDK);
    up(TDK);
    dn(TDK);
    wait_ms(250);
    expect("TD tap + hold", "+06");
    up(TDK);
    expect("TD tap + hold released", "-06");

    dn(TDK);
    up(TDK);
    dn(TDK);
    up(TDK);
    dn(TDK);
    up(TDK);
    expect("TD triple tap: on tap x3 at the third press", "+04 -04 +04 -04 +04 -04");
    dn(TDK);
    up(TDK);
    wait_ms(250);
    expect("TD 4th tap: one more, nothing at the end", "+04 -04");
    expect_idle("TD basic");
}

static void t_td_interrupt(void)
{
    td_setup();
    dn(TDK);
    dn(B_);
    up(B_);
    up(TDK);
    expect("TD interrupted while held: on tap, held with the key", "+04 +1B -1B -04");

    dn(TDK);
    up(TDK);
    dn(B_);
    up(B_);
    expect("TD tapped, then another key", "+04 -04 +1B -1B");

    dn(TDK);
    up(TDK);
    dn(TDK);
    up(TDK);
    dn(B_);
    up(B_);
    expect("TD two taps interrupted: tap + tap", "+04 -04 +04 -04 +1B -1B");

    dn(TDK);
    dn(2, 1);
    up(2, 1);
    up(TDK);
    wait_ms(250);
    expect("TD interrupted by another TD (empty: nothing)", "+04 -04");
    expect_idle("TD interrupt");
}

static void t_td_fallbacks(void)
{
    td_setup();
    tds[0][TD_ON_HOLD]     = KC_NO;
    tds[0][TD_ON_DOUBLE]   = KC_NO;
    tds[0][TD_ON_TAP_HOLD] = KC_NO;
    dn(TDK);
    wait_ms(250);
    expect("TD no hold: hold gives tap", "+04");
    up(TDK);
    dn(TDK);
    up(TDK);
    dn(TDK);
    up(TDK);
    wait_ms(250);
    expect("TD no double: tap twice", "-04 +04 -04 +04 -04");
    dn(TDK);
    up(TDK);
    dn(TDK);
    wait_ms(250);
    up(TDK);
    expect("TD no tap+hold: tap, then tap held", "+04 -04 +04 -04");
    tds[0][TD_ON_HOLD] = KC_LCTL;
    dn(TDK);
    up(TDK);
    dn(TDK);
    wait_ms(250);
    up(TDK);
    expect("TD no tap+hold: tap, then hold", "+04 -04 +E0 -E0");
    expect_idle("TD fallbacks");
}

static void t_td_term_and_layers(void)
{
    td_setup();
    tds[0][TD_TERM] = 500;
    dn(TDK);
    up(TDK);
    wait_ms(300);
    expect("TD own term: still waiting at 300 ms", "");
    wait_ms(250);
    expect("TD own term: done at 500 ms", "+04 -04");

    tds[0][TD_TERM]    = 0;
    tds[0][TD_ON_HOLD] = MO(1);
    keymap[0][0][0]    = KC_A;
    keymap[1][0][0]    = KC_B;
    dn(TDK);
    wait_ms(250);
    dn(A_);
    up(A_);
    up(TDK);
    dn(A_);
    up(A_);
    expect("TD hold = MO(1)", "+05 -05 +04 -04");

    keymap[0][2][2] = TD(9); /* no such entry: inert */
    dn(2, 2);
    up(2, 2);
    expect("TD past the entry count", "+5709 -5709");
    expect_idle("TD term and layers");
}

/* --------------------------------------------------------------- combos */

#define J_ 1, 0
#define K_ 1, 1
#define L_ 1, 2
#define X_ 1, 3

/* J K L X on row 1; combo 0: J + K -> Esc. */
static void combo_setup(void)
{
    reset(0);
    keymap[0][1][0]         = KC_J;
    keymap[0][1][1]         = KC_K;
    keymap[0][1][2]         = KC_L;
    keymap[0][1][3]         = KC_X;
    combos[0][0]            = KC_J;
    combos[0][1]            = KC_K;
    combos[0][COMBO_OUTPUT] = KC_ESC;
}

static void t_combo_basic(void)
{
    combo_setup();
    dn(J_);
    dn(K_);
    expect("combo fires when complete", "+29");
    up(J_);
    expect("combo released by its first key", "-29");
    up(K_);
    expect("second key's release swallowed", "");

    dn(J_);
    up(J_);
    expect("combo key tapped alone", "+0D -0D");

    dn(J_);
    wait_ms(60);
    expect("combo key held past the term", "+0D");
    dn(K_);
    up(K_);
    up(J_);
    expect("late second key types normally", "+0E -0E -0D");

    dn(J_);
    dn(X_);
    up(X_);
    up(J_);
    expect("combo key, then a non-combo key", "+0D +1B -1B -0D");

    dn(K_);
    dn(J_);
    up(K_);
    up(J_);
    expect("combo keys in either order", "+29 -29");

    dn(J_);
    dn(K_);
    dn(X_);
    up(X_);
    up(K_);
    up(J_);
    expect("other keys while a combo is held", "+29 +1B -1B -29");
    expect_idle("combo basic");
}

static void t_combo_overlap(void)
{
    combo_setup();
    combos[1][0]            = KC_J;
    combos[1][1]            = KC_K;
    combos[1][2]            = KC_L;
    combos[1][COMBO_OUTPUT] = KC_B;

    dn(J_);
    dn(K_);
    expect("J+K waits: J+K+L could still form", "");
    dn(L_);
    expect("J+K+L fires", "+05");
    up(L_);
    up(K_);
    up(J_);
    expect("J+K+L released", "-05");

    dn(J_);
    dn(K_);
    wait_ms(60);
    expect("J+K fires at the combo term", "+29");
    up(J_);
    up(K_);
    expect("J+K released", "-29");

    dn(J_);
    dn(K_);
    up(K_);
    expect("J+K fires when a key is released", "+29 -29");
    up(J_);
    expect_idle("combo overlap");
}

static void t_combo_outputs(void)
{
    combo_setup();
    combos[0][COMBO_OUTPUT] = LCTL(KC_C);
    dn(J_);
    dn(K_);
    up(J_);
    up(K_);
    expect("combo output: modded keycode", "+106 -106");

    combos[0][COMBO_OUTPUT] = MO(1);
    keymap[1][1][3]         = KC_B;
    dn(J_);
    dn(K_);
    dn(X_);
    up(X_);
    up(J_);
    up(K_);
    dn(X_);
    up(X_);
    expect("combo output: MO(1)", "+05 -05 +1B -1B");

    combos[0][COMBO_OUTPUT] = MT(MOD_LCTL, KC_A);
    dn(J_);
    dn(K_);
    up(J_);
    up(K_);
    expect("combo output: mod-tap tapped", "+04 -04");
    wait_ms(300); /* past the quick tap term of the tap above */
    dn(J_);
    dn(K_);
    wait_ms(250);
    expect("combo output: mod-tap held", "m+01");
    up(K_);
    up(J_);
    expect("combo output: mod-tap released", "m-01");
    expect_idle("combo outputs");
}

/* -------------------------------------------------------- key overrides */

#define BSPC_ 0, 0
#define SFT_  0, 1
#define X0_   0, 2
#define ESC_  0, 3
#define CTL_  0, 4

#define HID_LCTL 0x01u
#define HID_LSFT 0x02u
#define HID_RSFT 0x20u

/* Shift + Backspace -> Delete, Shift taken off while it is down. */
static void ko_setup(void)
{
    reset(0);
    keymap[0][0][0]        = KC_BSPC;
    keymap[0][0][1]        = KC_LSFT;
    keymap[0][0][2]        = KC_X;
    keymap[0][0][3]        = KC_ESC;
    keymap[0][0][4]        = KC_LCTL;
    kos[0][KO_TRIGGER]     = KC_BSPC;
    kos[0][KO_REPLACEMENT] = KC_DEL;
    kos[0][KO_LAYERS]      = 0xFFFF;
    kos[0][KO_MODS]        = HID_LSFT;
    kos[0][KO_SUPPRESS]    = HID_LSFT | (uint16_t)((KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) << 8);
}

static void t_ko_basic(void)
{
    ko_setup();
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    expect("KO: shift+bspc -> del, shift lifted and put back", "+E1 m-02 +4C -4C m+02 -E1");

    dn(BSPC_);
    up(BSPC_);
    expect("KO: bspc alone", "+2A -2A");

    dn(SFT_);
    dn(BSPC_);
    up(SFT_);
    wait_ms(1);
    up(BSPC_);
    expect("KO: shift released first -> trigger again, shift stays up", "+E1 m-02 +4C -E1 -4C +2A -2A");

    dn(SFT_);
    dn(BSPC_);
    dn(X0_);
    up(X0_);
    up(BSPC_);
    up(SFT_);
    expect("KO: another key ends it", "+E1 m-02 +4C -4C m+02 +1B -1B -2A -E1");

    kos[1][KO_TRIGGER]     = KC_ESC;
    kos[1][KO_REPLACEMENT] = LSFT(KC_GRV);
    kos[1][KO_LAYERS]      = 0xFFFF;
    kos[1][KO_MODS]        = HID_LSFT;
    kos[1][KO_SUPPRESS]    = HID_LSFT | (uint16_t)((KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) << 8);
    dn(SFT_);
    dn(ESC_);
    up(ESC_);
    up(SFT_);
    expect("KO: shift+esc -> ~ (modded replacement)", "+E1 m-02 +235 -235 m+02 -E1");
    expect_idle("KO basic");
}

static void t_ko_conditions(void)
{
    ko_setup();
    kos[0][KO_MODS] = HID_LSFT | (uint16_t)(HID_LCTL << 8); /* negative: ctrl */
    dn(CTL_);
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    up(CTL_);
    expect("KO: negative mod blocks it", "+E0 +E1 +2A -2A -E1 -E0");

    ko_setup();
    kos[0][KO_LAYERS] = 0x0002; /* layer 1 only */
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    expect("KO: not on this layer", "+E1 +2A -2A -E1");

    ko_setup();
    kos[0][KO_MODS] = HID_LSFT | HID_RSFT; /* both shifts needed */
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    expect("KO: all trigger mods needed", "+E1 +2A -2A -E1");
    kos[0][KO_SUPPRESS] |= (uint16_t)(KO_OPT_ONE_MOD << 8);
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    expect("KO: one of the trigger mods (one-mod option)", "+E1 m-02 +4C -4C m+02 -E1");

    ko_setup();
    kos[0][KO_SUPPRESS] = HID_LSFT | (uint16_t)(KO_OPT_ACT_TRIGGER_DOWN << 8);
    dn(SFT_);
    dn(BSPC_);
    up(BSPC_);
    up(SFT_);
    expect("KO: disabled entry", "+E1 +2A -2A -E1");

    ko_setup();
    kos[0][KO_SUPPRESS] |= (uint16_t)(KO_OPT_NO_REREGISTER_TRIGGER << 8);
    dn(SFT_);
    dn(BSPC_);
    up(SFT_);
    wait_ms(1);
    up(BSPC_);
    expect("KO: no re-register option", "+E1 m-02 +4C -E1 -4C -2A");
    expect_idle("KO conditions");
}

/* ------------------------------------------------- QMK quantum keycodes */

static void expect_true(const char *name, bool cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", name);
    } else {
        printf("ok   %s\n", name);
    }
    out[0] = 0;
}

#define D_ 0, 3
#define E_ 0, 4

static void t_magic(void)
{
    reset(0);
    keymap[0][0][0] = KC_CAPS;
    keymap[0][0][1] = KC_LCTL;
    keymap[0][0][2] = QK_MAGIC + 0x00; /* swap Caps Lock and Left Control */
    keymap[0][0][3] = QK_MAGIC + 0x13; /* toggle NKRO */
    keymap[0][0][4] = KC_LGUI;
    dn(C_);
    up(C_);
    expect_true("magic key: config changed and stored", config_changes == 1 && (action_config.magic & MAGIC_SWAP_CAPS_LCTL));
    dn(A_);
    up(A_);
    dn(B_);
    up(B_);
    expect("magic: Caps <-> LCtrl", "+E0 -E0 +39 -39");
    dn(D_);
    up(D_);
    expect_true("magic: NKRO toggled off", !(action_config.magic & MAGIC_NKRO));
    action_config.magic = MAGIC_NO_GUI;
    dn(E_);
    up(E_);
    expect("magic: GUI disabled", "");
    action_config.magic = MAGIC_SWAP_LALT_LGUI;
    dn(E_);
    up(E_);
    expect("magic: LGUI -> LAlt", "+E2 -E2");
    expect_idle("magic");
}

static void t_grave_escape(void)
{
    reset(0);
    keymap[0][0][0] = QK_GRAVE_ESCAPE;
    keymap[0][0][1] = KC_LSFT;
    dn(A_);
    up(A_);
    expect("GESC alone: Esc", "+29 -29");
    dn(B_);
    dn(A_);
    up(A_);
    up(B_);
    expect("GESC with Shift: `", "+E1 +35 -35 -E1");
    action_config.grave_esc = GESC_SHIFT;
    dn(B_);
    dn(A_);
    up(A_);
    up(B_);
    expect("GESC with Shift, shift override: Esc", "+E1 +29 -29 -E1");
    expect_idle("grave escape");
}

static void t_space_cadet(void)
{
    reset(0);
    keymap[0][0][0] = QK_SPACE_CADET_LEFT_SHIFT_PARENTHESIS_OPEN;
    keymap[0][0][1] = KC_X;
    keymap[0][0][2] = QK_SPACE_CADET_LEFT_CTRL_PARENTHESIS_OPEN;
    dn(A_);
    up(A_);
    expect("SC_LSPO tapped: (", "m+02 +26 -26 m-02");
    dn(A_);
    wait_ms(250);
    up(A_);
    expect("SC_LSPO held past the term: just Shift", "m+02 m-02");
    dn(A_);
    dn(B_);
    up(B_);
    up(A_);
    expect("SC_LSPO + another key: Shift", "m+02 +1B -1B m-02");
    dn(C_);
    up(C_);
    expect("SC_LCPO tapped: Ctrl off, Shift + 9", "m+01 m-01 m+02 +26 -26 m-02");
    expect_idle("space cadet");
}

static void t_caps_word(void)
{
    reset(0);
    keymap[0][0][0] = QK_CAPS_WORD_TOGGLE;
    keymap[0][0][1] = KC_X;
    keymap[0][0][2] = KC_1;
    keymap[0][0][3] = KC_SPC;
    keymap[0][0][4] = KC_MINS;
    dn(A_);
    up(A_);
    dn(B_);
    up(B_);
    dn(C_);
    up(C_);
    dn(E_);
    up(E_);
    expect("Caps Word: letters and - shifted, digits not", "m+02 +1B -1B m-02 +1E -1E m+02 +2D -2D m-02");
    dn(D_);
    up(D_);
    dn(B_);
    up(B_);
    expect("Caps Word: ended by space", "+2C -2C +1B -1B");
    dn(A_);
    up(A_);
    wait_ms(6000);
    dn(B_);
    up(B_);
    expect("Caps Word: idle timeout", "+1B -1B");
    expect_idle("caps word");
}

static void t_repeat(void)
{
    reset(0);
    keymap[0][0][0] = KC_A;
    keymap[0][0][1] = QK_REPEAT_KEY;
    keymap[0][0][2] = KC_LSFT;
    dn(B_);
    up(B_);
    expect("Repeat with no last key: nothing", "");
    dn(A_);
    up(A_);
    dn(B_);
    up(B_);
    dn(B_);
    up(B_);
    expect("Repeat", "+04 -04 +04 -04 +04 -04");
    dn(C_);
    dn(A_);
    up(A_);
    up(C_);
    dn(B_);
    up(B_);
    expect("Repeat brings the mods back", "+E1 +04 -04 -E1 m+02 +04 -04 m-02");
    expect_idle("repeat");
}

static void t_alt_repeat(void)
{
    reset(0);
    keymap[0][0][0] = KC_LEFT;
    keymap[0][0][1] = QK_ALT_REPEAT_KEY;
    keymap[0][0][2] = KC_LSFT;
    keymap[0][0][3] = KC_Z;
    keymap[0][0][4] = KC_LCTL;
    dn(A_);
    up(A_);
    dn(B_);
    up(B_);
    expect("Alt Repeat: Left -> Right", "+50 -50 +4F -4F");
    dn(C_);
    dn(A_);
    up(A_);
    up(C_);
    dn(B_);
    up(B_);
    expect("Alt Repeat keeps the mods", "+E1 +50 -50 -E1 m+02 +4F -4F m-02");
    dn(E_);
    dn(D_);
    up(D_);
    up(E_);
    dn(B_);
    up(B_);
    expect("Alt Repeat: Ctrl+Z -> Ctrl+Y", "+E0 +1D -1D -E0 m+01 +1C -1C m-01");

    expect_idle("alt repeat");
}

static void t_layer_lock(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[1][0][1] = QK_LAYER_LOCK;
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(FN_);
    dn(B_);
    up(B_);
    up(FN_);
    dn(A_);
    up(A_);
    expect("Layer Lock keeps MO's layer on", "+05 -05");
    dn(B_);
    up(B_);
    dn(A_);
    up(A_);
    expect("Layer Lock again: unlocked, off", "+04 -04");
    expect_idle("layer lock");
}

#define FN2_ 5, 9

static void t_tri_layer(void)
{
    reset(0);
    keymap[0][5][8] = QK_TRI_LAYER_LOWER;
    keymap[0][5][9] = QK_TRI_LAYER_UPPER;
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    keymap[2][0][0] = KC_C;
    keymap[3][0][0] = KC_D;
    dn(FN_);
    dn(A_);
    up(A_);
    expect("tri-layer: lower", "+05 -05");
    dn(FN2_);
    dn(A_);
    up(A_);
    expect("tri-layer: both -> layer 3", "+07 -07");
    up(FN_);
    dn(A_);
    up(A_);
    expect("tri-layer: upper only", "+06 -06");
    up(FN2_);
    dn(A_);
    up(A_);
    expect("tri-layer: none", "+04 -04");
    expect_idle("tri-layer");
}

static void t_combo_toggle(void)
{
    combo_setup();
    keymap[0][0][2] = QK_COMBO_TOGGLE;
    dn(C_);
    up(C_);
    dn(J_);
    dn(K_);
    up(J_);
    up(K_);
    expect("combos off", "+0D +0E -0D -0E");
    dn(C_);
    up(C_);
    dn(J_);
    dn(K_);
    up(J_);
    up(K_);
    expect("combos back on", "+29 -29");
    expect_idle("combo toggle");
}

static void t_osm_tap_toggle(void)
{
    reset(0);
    action_config.osk_tap_toggle = 2;
    keymap[0][0][0]              = OSM(MOD_LSFT);
    keymap[0][0][1]              = KC_X;
    dn(A_);
    up(A_);
    dn(A_);
    up(A_);
    dn(B_);
    up(B_);
    dn(B_);
    up(B_);
    expect("one-shot tapped twice: locked", "m+02 m-02 m+02 +1B -1B +1B -1B");
    dn(A_);
    up(A_);
    expect("tapped again: unlocked", "m+02 m-02");
    expect_idle("one-shot tap toggle");
}

static void t_mt_tap_takes_oneshot(void)
{
    reset(0);
    keymap[0][0][0] = OSM(MOD_LSFT);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    dn(A_);
    up(A_);
    dn(DUAL);
    up(DUAL);
    expect("mod-tap's tap takes an armed one-shot mod", "m+02 m-02 m+02 +04 -04 m-02");
    expect_idle("mod-tap + one-shot");
}

static void t_combo_incomplete_ignored(void)
{
    combo_setup();
    combos[0][1] = KC_NO; /* 1-input combo while editing */
    dn(J_);
    up(J_);
    expect("combo with <2 inputs ignored", "+0D -0D");

    combos[0][1]            = KC_K;
    combos[0][COMBO_OUTPUT] = KC_NO; /* unfinished combo with KC_NO output */
    dn(J_);
    dn(K_);
    up(K_);
    up(J_);
    expect("combo with KC_NO output ignored", "+0D +0E -0E -0D");
    expect_idle("combo incomplete");
}

static void t_osm_roll_mt_no_bleed(void)
{
    reset(0);
    keymap[0][0][0] = OSM(MOD_LSFT);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][0][1] = KC_B;
    keymap[0][0][2] = KC_1;
    dn(A_);
    up(A_);
    dn(DUAL);
    wait_ms(10);
    dn(B_);
    wait_ms(10);
    up(DUAL);
    up(B_);
    expect("OSM does not bleed into rolled key after MT", "m+02 m-02 m+02 +04 m-02 +05 -04 -05");

    reset(0);
    keymap[0][0][0] = QK_CAPS_WORD_TOGGLE;
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][0][2] = KC_1;
    dn(A_);
    up(A_);
    dn(DUAL);
    wait_ms(10);
    dn(C_);
    wait_ms(10);
    up(DUAL);
    up(C_);
    expect("Caps Word shift does not bleed into rolled digit", "m+02 +04 m-02 +1E -04 -1E");

    ko_setup();
    keymap[0][0][0] = OSM(MOD_LSFT);
    keymap[0][3][0] = MT(MOD_LCTL, KC_BSPC);
    keymap[0][0][1] = KC_B;
    dn(A_);
    up(A_);
    dn(DUAL);
    wait_ms(10);
    dn(B_);
    wait_ms(10);
    up(DUAL);
    up(B_);
    expect("OSM + KO on rolled MT does not re-arm Shift", "m+02 m-02 m+02 m-02 +4C -4C +05 -2A -05");
    expect_idle("OSM roll MT");
}

static void t_retro_tapping_output_stage(void)
{
    reset(ACTION_RETRO_TAPPING);
    keymap[0][0][0] = OSM(MOD_LSFT);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    dn(A_);
    up(A_);
    dn(DUAL);
    wait_ms(300);
    up(DUAL);
    expect("retro tap applies armed OSM", "m+02 m-02 m+01 m-01 m+02 +04 -04 m-02");

    reset(ACTION_RETRO_TAPPING);
    keymap[0][0][0] = QK_CAPS_WORD_TOGGLE;
    keymap[0][0][1] = QK_REPEAT_KEY;
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    dn(A_);
    up(A_);
    dn(DUAL);
    wait_ms(300);
    up(DUAL);
    expect("retro tap goes through Caps Word", "m+01 m-01 m+02 +04 -04 m-02");
    dn(B_);
    up(B_);
    expect("retro tap recorded for Repeat", "m+02 +04 -04 m-02");
    dn(A_);
    up(A_); /* turn Caps Word off */
    expect_idle("retro tap output stage");
}

static void t_layer_lock_cleared_on_tg_to(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[1][0][1] = QK_LAYER_LOCK;
    keymap[1][0][2] = TG(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(FN_);
    dn(B_);
    up(B_);
    up(FN_);
    dn(C_);
    up(C_);
    dn(FN_);
    dn(A_);
    up(A_);
    up(FN_);
    dn(A_);
    up(A_);
    expect("TG off clears Layer Lock so next MO releases cleanly", "+05 -05 +04 -04");

    keymap[1][0][2] = TO(0);
    dn(FN_);
    dn(B_);
    up(B_);
    up(FN_);
    dn(C_);
    up(C_);
    dn(FN_);
    dn(A_);
    up(A_);
    up(FN_);
    dn(A_);
    up(A_);
    expect("TO clears Layer Lock", "+05 -05 +04 -04");
    expect_idle("layer lock cleared");
}

static void t_tt_hold_after_tap(void)
{
    reset(0);
    keymap[0][0][2] = TT(1);
    keymap[1][0][2] = TT(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_);
    up(C_);
    wait_ms(50); /* inside quick_tap_term (200 ms) */
    dn(C_);
    wait_ms(250);
    dn(A_);
    up(A_);
    up(C_);
    expect("TT tap then immediate hold enters layer momentarily", "+05 -05");
    dn(A_);
    up(A_);
    expect("TT hold released back to base", "+04 -04");
    expect_idle("TT hold after tap");
}

static void t_encoder_action_engine(void)
{
    reset(0);
    keymap[0][0][0]  = KC_A;
    keymap[0][3][0]  = LT(1, KC_SPC);
    enc_keymap[0][0] = TG(1);
    enc_keymap[0][1] = KC_VOLU;
    enc_keymap[1][0] = TG(1);
    enc_keymap[1][1] = QK_CAPS_WORD_TOGGLE;
    action_event(ACTION_ENC_ROW, 0, true);
    wait_ms(4);
    action_event(ACTION_ENC_ROW, 0, false);
    action_event(ACTION_ENC_ROW, 1, true);
    wait_ms(4);
    action_event(ACTION_ENC_ROW, 1, false);
    dn(A_);
    up(A_);
    expect("encoder runs TG and Caps Word via action engine", "m+02 +04 -04 m-02");
    action_event(ACTION_ENC_ROW, 1, true);
    wait_ms(4);
    action_event(ACTION_ENC_ROW, 1, false); /* Caps Word off */
    action_event(ACTION_ENC_ROW, 0, true);
    wait_ms(4);
    action_event(ACTION_ENC_ROW, 0, false); /* TG(1) off */

    enc_keymap[1][1] = KC_MNXT;
    dn(DUAL);
    wait_ms(10);
    action_event(ACTION_ENC_ROW, 1, true);
    wait_ms(4);
    action_event(ACTION_ENC_ROW, 1, false);
    wait_ms(250);
    up(DUAL);
    expect("encoder detent buffered during LT resolves on hold layer", "+AB -AB");
    expect_idle("encoder engine");
}

static void t_combo_layer_reresolve(void)
{
    combo_setup();
    combos[0][COMBO_OUTPUT] = MO(1);
    combos[1][0]            = KC_J;
    combos[1][1]            = KC_K;
    combos[1][2]            = KC_L;
    combos[1][COMBO_OUTPUT] = KC_B;
    combos[2][0]            = KC_2;
    combos[2][1]            = KC_3;
    combos[2][COMBO_OUTPUT] = KC_Z;
    keymap[1][1][3]         = KC_2;
    keymap[1][0][0]         = KC_3;

    dn(J_);
    dn(K_);
    dn(X_);   /* settles MO(1), then X_ re-resolves as KC_2 on layer 1 */
    dn(0, 0); /* KC_3 completes combo 2 */
    up(0, 0);
    up(X_);
    up(K_);
    up(J_);
    expect("interrupting key re-resolves on layer activated by settled combo", "+1D -1D");
    expect_idle("combo layer re-resolve");
}

static void t_ko_preserves_tap_quick_tap(void)
{
    ko_setup();
    keymap[0][3][0] = MT(MOD_LCTL, KC_BSPC);
    dn(SFT_);
    dn(DUAL);
    wait_ms(20);
    up(DUAL);
    up(SFT_);
    wait_ms(50);
    dn(DUAL);
    wait_ms(300);
    up(DUAL);
    expect("KO on tap-hold tap still records quick-tap", "+E1 m-02 +4C -4C m+02 -E1 +2A -2A");
    expect_idle("KO quick-tap");
}

/* ---------------------------------------------------------- stress test
 *
 * Random presses / releases at random speeds on a keymap that mixes nearly
 * every feature, many seeds, each with its own tap-hold flags. After each
 * run everything is released and the engine left alone for 10 s; then the
 * host must see no key and no mod down, no layer may be left on, and the
 * engine must be idle. Fed through ASan / UBSan by meson's host build too.
 */
static uint32_t rng;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define FUZZ_KEYS 19
static const uint8_t fuzz_pos[FUZZ_KEYS][2] = {
    {4, 0}, {4, 1}, {4, 2}, {4, 3}, {4, 4}, {4, 5}, {4, 6}, {4, 7}, {4, 8}, {4, 9}, {4, 10}, {4, 11}, {4, 12}, {4, 13}, {4, 14}, {5, 0}, {5, 1}, {5, 2}, {5, 3},
};

static void fuzz_setup(uint8_t flags)
{
    reset(flags);
    const uint16_t base[FUZZ_KEYS] = {
        KC_A, KC_B, MT(MOD_LSFT, KC_D), LT(1, KC_E), OSM(MOD_LCTL), TD(0), KC_J, KC_K, QK_CAPS_WORD_TOGGLE, QK_REPEAT_KEY, QK_GRAVE_ESCAPE, QK_SPACE_CADET_LEFT_SHIFT_PARENTHESIS_OPEN, MO(2), KC_LSFT, KC_BSPC, QK_ALT_REPEAT_KEY, OSL(1), LCTL(KC_V), KC_LEFT,
    };
    for (int i = 0; i < FUZZ_KEYS; i++) {
        keymap[0][fuzz_pos[i][0]][fuzz_pos[i][1]] = base[i];
    }
    keymap[1][4][0]         = KC_1;
    keymap[1][4][6]         = KC_2;
    keymap[2][4][0]         = MT(MOD_LALT, KC_3);
    keymap[2][4][1]         = KC_4;
    tds[0][TD_ON_TAP]       = KC_X;
    tds[0][TD_ON_HOLD]      = KC_LCTL;
    tds[0][TD_ON_DOUBLE]    = KC_Y;
    tds[0][TD_ON_TAP_HOLD]  = MO(1);
    combos[0][0]            = KC_J;
    combos[0][1]            = KC_K;
    combos[0][COMBO_OUTPUT] = KC_ESC;
    combos[1][0]            = KC_A;
    combos[1][1]            = KC_B;
    combos[1][COMBO_OUTPUT] = LCTL(KC_C);
    kos[0][KO_TRIGGER]      = KC_BSPC;
    kos[0][KO_REPLACEMENT]  = KC_DEL;
    kos[0][KO_LAYERS]       = 0xFFFF;
    kos[0][KO_MODS]         = 0x02;
    kos[0][KO_SUPPRESS]     = 0x02 | (uint16_t)((KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) << 8);
}

static bool fuzz_run(uint32_t seed, int steps)
{
    bool held[FUZZ_KEYS] = {false};
    int  nheld           = 0;

    rng = seed * 2654435761u + 1u;
    fuzz_setup((uint8_t)(seed & 7u));
    action_config.tapping_term   = (uint16_t)(120u + rnd() % 200u); /* timings vary too */
    action_config.quick_tap_term = (uint16_t)(rnd() % 250u);
    action_config.combo_term     = (uint16_t)(20u + rnd() % 80u);
    for (int s = 0; s < steps; s++) {
        int k = (int)(rnd() % FUZZ_KEYS);
        if (nheld >= 6) { /* keep chords realistic: let go of something */
            while (!held[k]) {
                k = (int)(rnd() % FUZZ_KEYS);
            }
        }
        held[k] = !held[k];
        nheld += held[k] ? 1 : -1;
        action_event(fuzz_pos[k][0], fuzz_pos[k][1], held[k]);
        const uint32_t r = rnd();
        wait_ms((uint16_t)((r & 7u) == 0 ? 150u + r % 250u : r % 40u));
    }
    for (int k = 0; k < FUZZ_KEYS; k++) {
        if (held[k]) {
            action_event(fuzz_pos[k][0], fuzz_pos[k][1], false);
            wait_ms((uint16_t)(rnd() % 20u));
        }
    }
    wait_ms(10000);

    bool ok = true;
    for (uint32_t kc = 0; kc < 0x10000u; kc++) {
        if (host_key[kc]) {
            printf("FAIL stress seed %u: keycode 0x%04X still down\n", (unsigned)seed, (unsigned)kc);
            ok = false;
        }
    }
    if (host_mods) {
        printf("FAIL stress seed %u: mods 0x%02X still on\n", (unsigned)seed, host_mods);
        ok = false;
    }
    if (action_layer_state() != 1u) {
        printf("FAIL stress seed %u: layers 0x%02X still on\n", (unsigned)seed, action_layer_state());
        ok = false;
    }
    if (!action_idle()) {
        printf("FAIL stress seed %u: engine not idle\n", (unsigned)seed);
        ok = false;
    }
    return ok;
}

#ifndef STRESS_RUNS
#    define STRESS_RUNS 300
#endif

static void t_stress(void)
{
    int bad = 0;
    for (uint32_t seed = 1; seed <= STRESS_RUNS; seed++) {
        if (!fuzz_run(seed, 2000)) {
            bad++;
        }
    }
    checks++;
    if (bad) {
        failures++;
        printf("FAIL stress: %d of %d runs left something behind\n", bad, STRESS_RUNS);
    } else {
        printf("ok   stress: %d random runs x 2000 events, nothing left behind\n", STRESS_RUNS);
    }
    out[0] = 0;
}

int main(void)
{
    t_plain();
    t_mods_keycode_passthrough();
    t_mo();
    t_release_after_layer_change();
    t_transparent();
    t_tg_to_df();
    t_lt_tap();
    t_lt_hold();
    t_lt_rolling_default();
    t_mt_permissive_hold();
    t_mt_hold_on_other();
    t_mt_right_mods();
    t_retro_tapping();
    t_quick_tap();
    t_buffer_order_on_timeout();
    t_osm();
    t_osl();
    t_tt();
    t_lm();
    t_two_duals();
    t_slots_full();
    t_td_basic();
    t_td_interrupt();
    t_td_fallbacks();
    t_td_term_and_layers();
    t_combo_basic();
    t_combo_overlap();
    t_combo_outputs();
    t_ko_basic();
    t_ko_conditions();
    t_magic();
    t_grave_escape();
    t_space_cadet();
    t_caps_word();
    t_repeat();
    t_alt_repeat();
    t_layer_lock();
    t_tri_layer();
    t_combo_toggle();
    t_osm_tap_toggle();
    t_mt_tap_takes_oneshot();
    t_combo_incomplete_ignored();
    t_osm_roll_mt_no_bleed();
    t_retro_tapping_output_stage();
    t_layer_lock_cleared_on_tg_to();
    t_tt_hold_after_tap();
    t_encoder_action_engine();
    t_combo_layer_reresolve();
    t_ko_preserves_tap_quick_tap();
    t_stress();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
