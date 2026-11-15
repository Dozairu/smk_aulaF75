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

/* -------------------------------------------------------------- harness */

static int failures;
static int checks;

static void reset(uint8_t flags)
{
    memset(keymap, 0, sizeof keymap); /* KC_NO everywhere */
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

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
