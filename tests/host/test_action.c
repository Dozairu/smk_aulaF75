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
static uint16_t now_ms;
static char     out[1024];

/* ------------------------------------------------------------------ HAL */

uint16_t action_hal_keymap(uint8_t layer, uint8_t row, uint8_t col)
{
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

static void emit(const char *tok)
{
    if (out[0]) {
        strcat(out, " ");
    }
    strcat(out, tok);
}

void action_hal_send(uint16_t kc, bool pressed)
{
    char tok[16];
    snprintf(tok, sizeof tok, "%c%02X", pressed ? '+' : '-', kc);
    emit(tok);
}

void action_hal_mods(uint8_t mods, bool on)
{
    char tok[16];
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
    memset(tds, 0, sizeof tds);
    memset(combos, 0, sizeof combos);
    memset(kos, 0, sizeof kos);
    for (int l = 1; l < LAYERS; l++) {
        for (int r = 0; r < ROWS; r++) {
            for (int c = 0; c < COLS; c++) {
                keymap[l][r][c] = KC_TRNS;
            }
        }
    }
    now_ms = 1000;
    out[0] = 0;
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
    dn(A_); up(A_);
    expect("plain key", "+04 -04");
    expect_idle("plain key");
}

static void t_mods_keycode_passthrough(void)
{
    reset(0);
    keymap[0][0][0] = LCTL(KC_C);
    dn(A_); up(A_);
    expect("modded keycode passes through intact", "+106 -106");
}

static void t_mo(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(FN_); dn(A_); up(A_); up(FN_);
    expect("MO: layer key", "+05 -05");
    dn(A_); up(A_);
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
    dn(A_); dn(FN_); up(A_); up(FN_);
    expect("release resolves to what was pressed", "+04 -04");

    dn(FN_); dn(A_); up(FN_); up(A_);
    expect("layer key released first still releases B", "+05 -05");
    expect_idle("layer change");
}

static void t_transparent(void)
{
    reset(0);
    keymap[0][5][8] = MO(1);
    keymap[0][0][1] = KC_B; /* layer 1 leaves it KC_TRNS */
    dn(FN_); dn(B_); up(B_); up(FN_);
    expect("transparent falls through", "+05 -05");
}

static void t_tg_to_df(void)
{
    reset(0);
    keymap[0][0][2] = TG(2);
    keymap[2][0][2] = TG(2);
    keymap[0][0][0] = KC_A;
    keymap[2][0][0] = KC_X;
    dn(C_); up(C_); dn(A_); up(A_);
    expect("TG on", "+1B -1B");
    dn(C_); up(C_); dn(A_); up(A_);
    expect("TG off", "+04 -04");

    keymap[0][0][2] = TO(3);
    keymap[3][0][0] = KC_Y;
    dn(C_); up(C_); dn(A_); up(A_);
    expect("TO", "+1C -1C");
    expect_idle("TG/TO");
}

static void t_lt_tap(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    dn(DUAL); wait_ms(50); up(DUAL);
    expect("LT tap", "+2C -2C");
    expect_idle("LT tap");
}

static void t_lt_hold(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(DUAL); wait_ms(250); dn(A_); up(A_); up(DUAL);
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
    dn(DUAL); wait_ms(20); dn(A_); wait_ms(20); up(A_); wait_ms(20); up(DUAL);
    expect("LT nested, default -> tap first, then A", "+2C +04 -04 -2C");

    dn(DUAL); wait_ms(20); dn(A_); wait_ms(20); up(DUAL); up(A_);
    expect("LT rolling over -> tap then A", "+2C +04 -2C -04");
    expect_idle("LT rolling");
}

static void t_mt_permissive_hold(void)
{
    reset(ACTION_PERMISSIVE_HOLD);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][0][1] = KC_B;
    dn(DUAL); wait_ms(20); dn(B_); wait_ms(20); up(B_); up(DUAL);
    expect("MT permissive hold -> Ctrl+B", "m+01 +05 -05 m-01");

    dn(DUAL); wait_ms(20); dn(B_); wait_ms(20); up(DUAL); up(B_);
    expect("MT permissive, roll (dual released first) -> tap", "+04 +05 -04 -05");
    expect_idle("MT permissive");
}

static void t_mt_hold_on_other(void)
{
    reset(ACTION_HOLD_ON_OTHER_KEY_PRESS);
    keymap[0][3][0] = MT(MOD_LSFT, KC_A);
    keymap[0][0][1] = KC_B;
    dn(DUAL); wait_ms(10); dn(B_);
    expect("MT hold-on-other: decided at the press", "m+02 +05");
    up(B_); up(DUAL);
    expect("MT hold-on-other: release", "-05 m-02");
    expect_idle("MT hold-on-other");
}

static void t_mt_right_mods(void)
{
    reset(0);
    keymap[0][3][0] = MT(MOD_RALT, KC_A);
    dn(DUAL); wait_ms(300); up(DUAL);
    expect("MT right-hand mod -> 0x40", "m+40 m-40");
}

static void t_retro_tapping(void)
{
    reset(ACTION_RETRO_TAPPING);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    dn(DUAL); wait_ms(300); up(DUAL);
    expect("retro tapping: long lone hold still taps", "m+01 m-01 +04 -04");
}

static void t_quick_tap(void)
{
    reset(0);
    keymap[0][3][0] = LT(1, KC_SPC);
    dn(DUAL); wait_ms(30); up(DUAL); wait_ms(50);
    dn(DUAL); wait_ms(400);
    expect("quick tap: second press holds the tap key", "+2C -2C +2C");
    up(DUAL);
    expect("quick tap: release", "-2C");

    wait_ms(300);
    dn(DUAL); wait_ms(400); up(DUAL);
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
    dn(DUAL); wait_ms(10); dn(A_);
    expect("buffered: nothing yet", "");
    wait_ms(250);
    expect("timeout replays A on layer 1", "+05");
    up(A_); up(DUAL);
    expect("release", "-05");
    expect_idle("buffer order");
}

static void t_osm(void)
{
    reset(0);
    keymap[0][0][2] = OSM(MOD_LSFT);
    keymap[0][0][0] = KC_A;
    dn(C_); up(C_);
    expect("OSM tap: mods only while held", "m+02 m-02");
    dn(A_); up(A_);
    expect("OSM applies to next key only", "m+02 +04 -04 m-02");
    dn(A_); up(A_);
    expect("OSM spent", "+04 -04");

    dn(C_); up(C_); wait_ms(6000); dn(A_); up(A_);
    expect("OSM times out", "m+02 m-02 +04 -04");

    dn(C_); dn(A_); up(A_); up(C_);
    expect("OSM held = plain mod", "m+02 +04 -04 m-02");
    dn(A_); up(A_);
    expect("held OSM does not arm", "+04 -04");
    expect_idle("OSM");
}

static void t_osl(void)
{
    reset(0);
    keymap[0][0][2] = OSL(1);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_); up(C_); dn(A_); up(A_);
    expect("OSL: next key on layer", "+05 -05");
    dn(A_); up(A_);
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
    dn(C_); wait_ms(300); dn(A_); up(A_); up(C_);
    expect("TT hold = momentary", "+05 -05");
    dn(A_); up(A_);
    expect("TT hold released", "+04 -04");
    dn(C_); up(C_); dn(A_); up(A_);
    expect("TT tap toggles on", "+05 -05");
    wait_ms(300);
    dn(C_); up(C_); dn(A_); up(A_);
    expect("TT tap toggles off", "+04 -04");
}

static void t_lm(void)
{
    reset(0);
    keymap[0][0][2] = LM(1, MOD_LCTL);
    keymap[0][0][0] = KC_A;
    keymap[1][0][0] = KC_B;
    dn(C_); dn(A_); up(A_); up(C_);
    expect("LM: layer + mod", "m+01 +05 -05 m-01");
}

static void t_two_duals(void)
{
    /* Home-row-mod style: two MT keys rolled quickly -> both taps, in order. */
    reset(0);
    keymap[0][3][0] = MT(MOD_LCTL, KC_A);
    keymap[0][3][1] = MT(MOD_LSFT, KC_S);
    dn(3, 0); wait_ms(30); dn(3, 1); wait_ms(30); up(3, 0); wait_ms(30); up(3, 1);
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
    expect("11th+ simultaneous key ignored, no stuck keys",
           "+04 +05 +06 +07 +08 +09 +0A +0B +0C +0D -04 -05 -06 -07 -08 -09 -0A -0B -0C -0D");
    expect_idle("slots full");
}

/* ------------------------------------------------------------ tap dance */

#define TDK 2, 0 /* the dancing key */

/* TD(0): tap A, hold LCtrl, double tap B, tap + hold C, global term. */
static void td_setup(void)
{
    reset(0);
    keymap[0][2][0] = TD(0);
    keymap[0][2][1] = TD(1);
    keymap[0][0][1] = KC_X;
    tds[0][TD_ON_TAP]      = KC_A;
    tds[0][TD_ON_HOLD]     = KC_LCTL;
    tds[0][TD_ON_DOUBLE]   = KC_B;
    tds[0][TD_ON_TAP_HOLD] = KC_C;
}

static void t_td_basic(void)
{
    td_setup();
    dn(TDK); up(TDK);
    expect("TD tap: nothing before the term", "");
    wait_ms(250);
    expect("TD tap", "+04 -04");

    dn(TDK); wait_ms(250);
    expect("TD hold: on hold, held", "+E0");
    up(TDK);
    expect("TD hold: released with the key", "-E0");

    dn(TDK); up(TDK); dn(TDK); up(TDK); wait_ms(250);
    expect("TD double tap", "+05 -05");

    dn(TDK); up(TDK); dn(TDK); wait_ms(250);
    expect("TD tap + hold", "+06");
    up(TDK);
    expect("TD tap + hold released", "-06");

    dn(TDK); up(TDK); dn(TDK); up(TDK); dn(TDK); up(TDK);
    expect("TD triple tap: on tap x3 at the third press", "+04 -04 +04 -04 +04 -04");
    dn(TDK); up(TDK); wait_ms(250);
    expect("TD 4th tap: one more, nothing at the end", "+04 -04");
    expect_idle("TD basic");
}

static void t_td_interrupt(void)
{
    td_setup();
    dn(TDK); dn(B_); up(B_); up(TDK);
    expect("TD interrupted while held: on tap, held with the key",
           "+04 +1B -1B -04");

    dn(TDK); up(TDK); dn(B_); up(B_);
    expect("TD tapped, then another key", "+04 -04 +1B -1B");

    dn(TDK); up(TDK); dn(TDK); up(TDK); dn(B_); up(B_);
    expect("TD two taps interrupted: tap + tap", "+04 -04 +04 -04 +1B -1B");

    dn(TDK); dn(2, 1); up(2, 1); up(TDK); wait_ms(250);
    expect("TD interrupted by another TD (empty: nothing)", "+04 -04");
    expect_idle("TD interrupt");
}

static void t_td_fallbacks(void)
{
    td_setup();
    tds[0][TD_ON_HOLD]     = KC_NO;
    tds[0][TD_ON_DOUBLE]   = KC_NO;
    tds[0][TD_ON_TAP_HOLD] = KC_NO;
    dn(TDK); wait_ms(250);
    expect("TD no hold: hold gives tap", "+04");
    up(TDK);
    dn(TDK); up(TDK); dn(TDK); up(TDK); wait_ms(250);
    expect("TD no double: tap twice", "-04 +04 -04 +04 -04");
    dn(TDK); up(TDK); dn(TDK); wait_ms(250); up(TDK);
    expect("TD no tap+hold: tap, then tap held", "+04 -04 +04 -04");
    tds[0][TD_ON_HOLD] = KC_LCTL;
    dn(TDK); up(TDK); dn(TDK); wait_ms(250); up(TDK);
    expect("TD no tap+hold: tap, then hold", "+04 -04 +E0 -E0");
    expect_idle("TD fallbacks");
}

static void t_td_term_and_layers(void)
{
    td_setup();
    tds[0][TD_TERM] = 500;
    dn(TDK); up(TDK); wait_ms(300);
    expect("TD own term: still waiting at 300 ms", "");
    wait_ms(250);
    expect("TD own term: done at 500 ms", "+04 -04");

    tds[0][TD_TERM]    = 0;
    tds[0][TD_ON_HOLD] = MO(1);
    keymap[0][0][0]    = KC_A;
    keymap[1][0][0]    = KC_B;
    dn(TDK); wait_ms(250); dn(A_); up(A_); up(TDK); dn(A_); up(A_);
    expect("TD hold = MO(1)", "+05 -05 +04 -04");

    keymap[0][2][2] = TD(9); /* no such entry: inert */
    dn(2, 2); up(2, 2);
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
    keymap[0][1][0] = KC_J;
    keymap[0][1][1] = KC_K;
    keymap[0][1][2] = KC_L;
    keymap[0][1][3] = KC_X;
    combos[0][0]            = KC_J;
    combos[0][1]            = KC_K;
    combos[0][COMBO_OUTPUT] = KC_ESC;
}

static void t_combo_basic(void)
{
    combo_setup();
    dn(J_); dn(K_);
    expect("combo fires when complete", "+29");
    up(J_);
    expect("combo released by its first key", "-29");
    up(K_);
    expect("second key's release swallowed", "");

    dn(J_); up(J_);
    expect("combo key tapped alone", "+0D -0D");

    dn(J_); wait_ms(60);
    expect("combo key held past the term", "+0D");
    dn(K_); up(K_); up(J_);
    expect("late second key types normally", "+0E -0E -0D");

    dn(J_); dn(X_); up(X_); up(J_);
    expect("combo key, then a non-combo key", "+0D +1B -1B -0D");

    dn(K_); dn(J_); up(K_); up(J_);
    expect("combo keys in either order", "+29 -29");

    dn(J_); dn(K_); dn(X_); up(X_); up(K_); up(J_);
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

    dn(J_); dn(K_);
    expect("J+K waits: J+K+L could still form", "");
    dn(L_);
    expect("J+K+L fires", "+05");
    up(L_); up(K_); up(J_);
    expect("J+K+L released", "-05");

    dn(J_); dn(K_); wait_ms(60);
    expect("J+K fires at the combo term", "+29");
    up(J_); up(K_);
    expect("J+K released", "-29");

    dn(J_); dn(K_); up(K_);
    expect("J+K fires when a key is released", "+29 -29");
    up(J_);
    expect_idle("combo overlap");
}

static void t_combo_outputs(void)
{
    combo_setup();
    combos[0][COMBO_OUTPUT] = LCTL(KC_C);
    dn(J_); dn(K_); up(J_); up(K_);
    expect("combo output: modded keycode", "+106 -106");

    combos[0][COMBO_OUTPUT] = MO(1);
    keymap[1][1][3]         = KC_B;
    dn(J_); dn(K_); dn(X_); up(X_); up(J_); up(K_); dn(X_); up(X_);
    expect("combo output: MO(1)", "+05 -05 +1B -1B");

    combos[0][COMBO_OUTPUT] = MT(MOD_LCTL, KC_A);
    dn(J_); dn(K_); up(J_); up(K_);
    expect("combo output: mod-tap tapped", "+04 -04");
    wait_ms(300); /* past the quick tap term of the tap above */
    dn(J_); dn(K_); wait_ms(250);
    expect("combo output: mod-tap held", "m+01");
    up(K_); up(J_);
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
    keymap[0][0][0] = KC_BSPC;
    keymap[0][0][1] = KC_LSFT;
    keymap[0][0][2] = KC_X;
    keymap[0][0][3] = KC_ESC;
    keymap[0][0][4] = KC_LCTL;
    kos[0][KO_TRIGGER]     = KC_BSPC;
    kos[0][KO_REPLACEMENT] = KC_DEL;
    kos[0][KO_LAYERS]      = 0xFFFF;
    kos[0][KO_MODS]        = HID_LSFT;
    kos[0][KO_SUPPRESS]    = HID_LSFT | (uint16_t)((KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) << 8);
}

static void t_ko_basic(void)
{
    ko_setup();
    dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_);
    expect("KO: shift+bspc -> del, shift lifted and put back", "+E1 m-02 +4C -4C m+02 -E1");

    dn(BSPC_); up(BSPC_);
    expect("KO: bspc alone", "+2A -2A");

    dn(SFT_); dn(BSPC_); up(SFT_); wait_ms(1); up(BSPC_);
    expect("KO: shift released first -> trigger again, shift stays up",
           "+E1 m-02 +4C -E1 -4C +2A -2A");

    dn(SFT_); dn(BSPC_); dn(X0_); up(X0_); up(BSPC_); up(SFT_);
    expect("KO: another key ends it", "+E1 m-02 +4C -4C m+02 +1B -1B -2A -E1");

    kos[1][KO_TRIGGER]     = KC_ESC;
    kos[1][KO_REPLACEMENT] = LSFT(KC_GRV);
    kos[1][KO_LAYERS]      = 0xFFFF;
    kos[1][KO_MODS]        = HID_LSFT;
    kos[1][KO_SUPPRESS]    = HID_LSFT | (uint16_t)((KO_OPT_ENABLED | KO_OPT_ACT_TRIGGER_DOWN) << 8);
    dn(SFT_); dn(ESC_); up(ESC_); up(SFT_);
    expect("KO: shift+esc -> ~ (modded replacement)", "+E1 m-02 +235 -235 m+02 -E1");
    expect_idle("KO basic");
}

static void t_ko_conditions(void)
{
    ko_setup();
    kos[0][KO_MODS] = HID_LSFT | (uint16_t)(HID_LCTL << 8); /* negative: ctrl */
    dn(CTL_); dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_); up(CTL_);
    expect("KO: negative mod blocks it", "+E0 +E1 +2A -2A -E1 -E0");

    ko_setup();
    kos[0][KO_LAYERS] = 0x0002; /* layer 1 only */
    dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_);
    expect("KO: not on this layer", "+E1 +2A -2A -E1");

    ko_setup();
    kos[0][KO_MODS] = HID_LSFT | HID_RSFT; /* both shifts needed */
    dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_);
    expect("KO: all trigger mods needed", "+E1 +2A -2A -E1");
    kos[0][KO_SUPPRESS] |= (uint16_t)(KO_OPT_ONE_MOD << 8);
    dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_);
    expect("KO: one of the trigger mods (one-mod option)", "+E1 m-02 +4C -4C m+02 -E1");

    ko_setup();
    kos[0][KO_SUPPRESS] = HID_LSFT | (uint16_t)(KO_OPT_ACT_TRIGGER_DOWN << 8);
    dn(SFT_); dn(BSPC_); up(BSPC_); up(SFT_);
    expect("KO: disabled entry", "+E1 +2A -2A -E1");

    ko_setup();
    kos[0][KO_SUPPRESS] |= (uint16_t)(KO_OPT_NO_REREGISTER_TRIGGER << 8);
    dn(SFT_); dn(BSPC_); up(SFT_); wait_ms(1); up(BSPC_);
    expect("KO: no re-register option", "+E1 m-02 +4C -E1 -4C -2A");
    expect_idle("KO conditions");
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

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
