#include "indicators.h"
#include "sh68f90.h"
#include "pwm.h"
#include "kbdef.h"
#include "matrix.h"
#include "settings.h"
#include "tick.h"
#include "led_effect.h"
#include "user_matrix.h"
#include "aula_rgb.h"
#include "aula_fx.h"
#include "aula_encoder.h"
#include "aula_macro.h"
#include "aula_status.h"
#ifdef RF_EUART0
#    include "aula_rf.h"
#endif

/*
 * AULA F75 backlight.
 *
 * TOPOLOGY. The fifteen matrix columns serve as LED multiplexing selectors,
 * and the eighteen PWM channels are the six LED rows multiplied by R/G/B.
 * This is the opposite of the NuPhy Air60, where PWM drives the columns. The
 * only other keyboard in SMK built like this is the genesis-thor-300, which
 * also shares its columns between key scanning and display -- this file
 * follows its structure, replacing its frame dithering with hardware PWM.
 *
 * SCHEDULING. `src/smk/tick.c` alternates one matrix scan and
 * `LED_SUBFRAMES_PER_SCAN` LED subframes, all inside the Timer2 ISR. We set
 * this count to `MATRIX_COLS`, giving one full LED sweep between two key
 * scans. The two cannot overlap: LED current couples into row sensing, which
 * `matrix.c` avoids by bracketing its scan with `indicators_pwm_disable()` /
 * `indicators_pwm_enable()`.
 *
 * BUDGET. Everything below runs inside the ISR, within a 400 us slot
 * (`RELOAD_LED_SUBFRAME`). Hence **only one** effect evaluation per subframe:
 * the genesis-thor-300 measured that six -- one per row -- "does not fit and
 * starves the USB interrupt". A full animation frame therefore takes
 * 6 x 15 = 90 subframes, or six matrix scans.
 *
 * None of this has been executed: no firmware has ever been flashed to the
 * device.
 */

#define LED_ROWS MATRIX_ROWS
#define LED_COLS MATRIX_COLS

#include LED_GEOMETRY_HEADER
_Static_assert(LED_GEOMETRY_ROWS == LED_ROWS && LED_GEOMETRY_COLS == LED_COLS,
               "generated LED geometry size does not match the key matrix");

/*
 * PWM clock divider: the low three bits of `PWM00CON`, which the factory
 * firmware sets to 0x89 at 0x68D3 -- i.e. PWM_MODE_ENABLE | PWM_SS | 0b001.
 */
#ifndef LED_PWM_MASTER
#    define LED_PWM_CLK_DIV 0b001
#    define LED_PWM_MASTER  (uint8_t)(PWM_MODE_ENABLE | PWM_SS | LED_PWM_CLK_DIV)
#endif

/*
 * Brightness. The factory firmware keeps a ten-step gain table at CODE 0x2937
 * (00 08 10 18 20 28 32 3c 46 50) which it applies by multiplying and then
 * dividing by 80. `led_effect_rgb()` instead expects a 0-255 factor applied
 * via an eight-bit shift: the table below is therefore the factory table
 * pre-scaled (gain x 255 / 80), avoiding a per-cell division inside the ISR.
 */
static const __code uint8_t led_brightness_gain[LED_BRIGHTNESS_LEVELS] = {
    0, 25, 51, 76, 102, 127, 159, 191, 223, 255,
};

/* Animation step per frame, indexed by `user_settings.led_speed`. */
static const __code uint8_t led_speeds[] = {1, 2, 4, 8, 16};
#define LED_SPEED_LEVELS (sizeof(led_speeds))

/*
 * Effects. The first four come from the shared `led_effect.c`; we borrow its
 * GEOMETRY (`led_effect_index`) but not its colors, which use linear
 * interpolation across three sectors. The hue comes from the factory color
 * wheel, whose ramps are perceptual.
 *
 * The fifth is specific to this keyboard: the factory firmware's reactive
 * engine (`fcn.000064F1`, effect index 12), where each key holds its own color
 * and intensity, and intensity decays from frame to frame. It is the F75's
 * signature effect, and the only one that needs to know WHICH key was pressed.
 */
#define AULA_FX_REACTIVE  ((uint8_t)(FX_COUNT + 0)) /*  4  0x64F1 */
#define AULA_FX_RAIN      ((uint8_t)(FX_COUNT + 1)) /*  5  0x9B2B */
#define AULA_FX_TWINKLE   ((uint8_t)(FX_COUNT + 2)) /*  6  0xAC1C -> 0x4EA9 */
#define AULA_FX_SNAKE     ((uint8_t)(FX_COUNT + 3)) /*  7  0x8DDF */
#define AULA_FX_SNAKE_RGB ((uint8_t)(FX_COUNT + 4)) /*  8  0x1D05, effect 0x26 -- adapted */
#define AULA_FX_RIPPLE    ((uint8_t)(FX_COUNT + 5)) /*  9  0x746A / 0x1D8D */
#define AULA_FX_LAKE      ((uint8_t)(FX_COUNT + 6)) /* 10  ORIGINAL, no factory equivalent */
#define AULA_FX_KEYWAVE   ((uint8_t)(FX_COUNT + 7)) /* 11  0x82A5 */
#define AULA_FX_VRAINBOW  ((uint8_t)(FX_COUNT + 8)) /* 12  0x9659 */
#define AULA_FX_GAMING    ((uint8_t)(FX_COUNT + 9)) /* 13  0x1DAB -> 0x95A4 */
#define AULA_FX_SHIMMER   ((uint8_t)(FX_COUNT + 10))/* 14  0x60C9, factory effect 6 */
#define AULA_FX_OFF       ((uint8_t)(FX_COUNT + 11))/* 15 */

/*
 * The first six in this list (REACTIVE through RIPPLE) share the SAME
 * mechanism, which is that of the factory firmware: a per-key intensity plane
 * that decays from frame to frame, and a seeder that resets keys to full
 * brightness. The factory firmware stores the base color at XRAM 0x0428, the
 * intensity at 0x0017, and its renderer 0x62E6 computes `base * intensity >> 5`
 * into 0x0152 -- the exact two-plane model this file already maintains for the
 * reactive engine. Only the SEEDER changes from one effect to another:
 *
 *   REACTIVE   pressed keys                   (already in place)
 *   RAIN       one droplet per column, columns restarted at random
 *   TWINKLE    randomly picked keys
 *   SNAKE      a dot snaking across the grid and bouncing back
 *   SNAKE_RGB  same, with randomly picked hue (factory color mode 7)
 *   RIPPLE     one ring per frame expanding from (col 7, row 2)
 *
 * The last three are stateless: pure functions of position and phase.
 *
 * ONE DELIBERATE ADAPTATION: SNAKE_RGB. In the factory firmware, effect 0x26
 * is not a permanent effect but a TRANSITION: 0x9184 saves the current effect
 * in XRAM 0x0E24 before setting 0x26, and handler 0x1D37 restores that saved
 * effect as soon as the snake's vertical direction returns to zero, i.e. when
 * the sweep has finished traversing the grid. The factory uses it as a mode
 * change flourish.
 *
 * SMK has no concept of transient effects and our list is cycled via key
 * presses, so we keep it permanent. The rendering is transcribed; its DURATION
 * is our choice.
 */

/*
 * Reactive engine state. The factory firmware uses 378 bytes for color and
 * 126 for intensity; here a color wheel hue index suffices, fitting in one
 * byte per key instead of three.
 */
/*
 * Intensity plane decay, in units per frame. It scales with speed, like the
 * seeders' frame divider: `4 << speed` yields 4, 8, 16, 32, 64 while the
 * divider yields 16, 8, 4, 2, 1. Their product stays constant, so the TRAIL
 * keeps the same length -- roughly four keys -- at all speeds, and only the
 * motion speeds up.
 */
/*
 * Intensity plane decay, per animation FRAME rather than per sweep.
 *
 * It no longer depends on speed, following the factory firmware: its renderer
 * computes `base * intensity >> 5`, so intensity only carries THIRTY-ONE
 * levels, and the reactive engine `0x64F1` subtracts exactly one per rendered
 * frame. 255 / 31 = 8. All speed responsiveness is carried by the frame period
 * below, just like in the factory firmware.
 */
#define SPARK_DECAY ((uint8_t)8)

/* Fade step per frame by Effect Speed. The frame gate cannot go faster than one sweep (38 ms) and the reactive periods are all below that, so speed must act on the decay step. Index 2 = previous fixed value. */
static const __code uint8_t spark_decay_react[5] = { 3, 5, 8, 14, 24 };
static const __code uint8_t spark_decay_lake[5]  = { 8, 14, 24, 36, 48 };
static __xdata uint8_t spark_hue[LED_COLS][LED_ROWS];
static __xdata uint8_t spark_val[LED_COLS][LED_ROWS];
static __xdata uint8_t spark_prev[LED_COLS];

/*
 * Must be cleared along with the framebuffer on an effect change: otherwise
 * re-entering reactive mode shows keys still fading out from last time, and a
 * stale bit in `spark_prev` swallows the first real press of any key that was
 * held down when leaving the mode.
 */
static void spark_reset(void)
{
    uint8_t col;
    uint8_t row;

    for (col = 0; col < LED_COLS; col++) {
        spark_prev[col] = 0;
        for (row = 0; row < LED_ROWS; row++) {
            spark_val[col][row] = 0;
        }
    }
}

static uint8_t led_col;   /* column displayed by the current subframe */
static uint8_t led_phase; /* animation phase, natural wrap at 256 */

#ifdef MATRIX_INTERLEAVED_SCAN
static volatile __bit   display_enabled;
static volatile uint8_t regen_steps_pending;
static uint8_t          react_col;
#endif

/*
 * Second phase counter, wrapping at 192 -- and this is a BUG FIX.
 *
 * `led_phase` is a byte: it wraps at 256. Consumers whose modulus divides 256
 * do not notice -- the wave wraps at 128, SMK's color wheel works on 0-255.
 * But the vertical rainbow wraps at **192** and shimmer wraps at 192 or 96:
 * `256 % 192 = 64`, so on every wrap of `led_phase` their hue JUMPS by a third
 * of the wheel. The factory firmware does not have this flaw because it stores
 * phase PER KEY and increments it directly modulo 192 or 96.
 *
 * This counter wraps at 192, which is divisible by 96: both moduli are
 * seamless. Two counters are better than one wrong one.
 */
#define FX_PHASE192 192u
static uint8_t fx_phase192;
static uint8_t regen_row; /* regeneration cursor, one cell per subframe */
static uint8_t regen_col;

/* ---------------------------------------------------------- factory seeders */

/*
 * Seeder state. Twenty-two bytes in total: the factory firmware spends far
 * more, because it also stores each key's base color, which the hue plane
 * above replaces at a third of the cost.
 */
static __xdata uint8_t rain_row[LED_COLS]; /* 0-5, or 0xFF when the column is idle */
static uint8_t         snake_col;
static uint8_t         snake_row;
static uint8_t         snake_right; /* current horizontal direction */
static uint8_t         snake_down;  /* current vertical direction */
static uint8_t         ripple_ring;
/*
 * THE LAKE -- a wave radiating from the pressed key.
 *
 * WARNING: THIS EFFECT IS NOT A TRANSCRIPTION. The factory firmware has nothing like
 * it: its concentric ripple (`0x746A`) radiates from a FIXED center tabulated
 * at `CODE 0x2959`, and its reactive engine (`0x64F1`) lights up the pressed
 * key without propagating it. This effect combines both and is an original
 * creation.
 *
 * Four simultaneous drops. Each tracks its origin, its age in frames, and its
 * starting hue. Every frame, age advances by one step; a cell is re-seeded to
 * full brightness when its DISTANCE to the origin equals the exact age -- the
 * wavefront. The intensity plane decay, already in place, forms the trailing
 * wake behind it, like a ripple on water.
 *
 * Distance weights rows TWICE as much as columns: the generated geometry
 * spaces columns by 17 units and rows by 42, so one row step equals roughly
 * two and a half column steps. Without this weighting the wave would be a
 * squashed diamond.
 *
 * The cost fits within the ISR budget: the check runs ONE cell at a time
 * inside `led_regen_one()`, i.e. four subtractions per subframe -- rather than
 * scanning all ninety cells at once.
 */
#define LAKE_DROPS    4
#define LAKE_MAX_AGE  25 /* 14 columns + 2 x 5 rows, plus one step of margin */
#define LAKE_HUE_STEP 3  /* hue shifts as it travels outward: 24 x 3 = one third of the wheel */

static __xdata uint8_t drop_col[LAKE_DROPS];
static __xdata uint8_t drop_row[LAKE_DROPS];
static __xdata uint8_t drop_age[LAKE_DROPS]; /* 0 = free slot */
static __xdata uint8_t drop_hue[LAKE_DROPS];

static uint8_t         fx_ms_acc;    /* milliseconds accumulated since the last frame */
static __bit           spark_decay_due; /* elapsed frame authorizes decay */

/* Lights a key at full brightness, if the grid has a key at that position. */
static void spark_seed(uint8_t col, uint8_t row, uint8_t hue)
{
    if (col >= LED_COLS || row >= LED_ROWS) {
        return;
    }
    if (aula_fx_key_id(col, row) == AULA_FX_NO_KEY) {
        return;
    }
    spark_hue[col][row] = hue;
    spark_val[col][row] = 255;
}

/*
 * Rain -- transcribed from 0x9B2B.
 *
 * A randomly picked column is restarted if idle, then each active column moves
 * down one row and turns off past the sixth. The factory firmware writes this
 * exact loop: `if state < 6: paint, state++; else state = 0xFF`, preceded by
 * `if state[random] == 0xFF: state[random] = 0`.
 */
static void rain_step(void)
{
    const uint8_t seed = (uint8_t)(aula_fx_rand() % LED_COLS);
    uint8_t       col;

    if (rain_row[seed] == 0xff) {
        rain_row[seed] = 0;
    }
    for (col = 0; col < LED_COLS; col++) {
        const uint8_t row = rain_row[col];

        if (row < LED_ROWS) {
            spark_seed(col, row, (uint8_t)(fx_phase192 + col));
            rain_row[col] = (uint8_t)(row + 1);
        } else {
            rain_row[col] = 0xff;
        }
    }
}

/*
 * Twinkle -- 0xAC1C, whose renderer 0x4EA9 picks a random position from table
 * 0x2EED and lights it up. Two keys per frame: the factory firmware loops over
 * a counter whose value is not fixed in the disassembly notes, and two gives a
 * density matching video footage of the keyboard.
 */
static void twinkle_step(void)
{
    uint8_t n;

    for (n = 0; n < 2; n++) {
        const uint8_t r = aula_fx_rand();

        spark_seed((uint8_t)(r % LED_COLS), (uint8_t)((r >> 4) % LED_ROWS), fx_phase192);
    }
}

/*
 * Snake -- transcribed from 0x8DDF.
 *
 * The dot advances horizontally; upon reaching the edge it reverses direction
 * AND steps down (or up) one row; upon reaching the top or bottom it also
 * reverses its vertical direction. The factory firmware keeps the four
 * variables at XRAM 0x0ECA-0x0ECD.
 *
 * `random` distinguishes SNAKE from SNAKE_RGB: effect 0x26 forces color mode
 * 7, which picks a random hue on every key (0x1D2B).
 */
static void snake_step(uint8_t random_hue)
{
    spark_seed(snake_col, snake_row, random_hue ? aula_fx_rand() : fx_phase192);

    if (snake_right) {
        if (snake_col + 1u < LED_COLS) {
            snake_col++;
            return;
        }
        snake_right = 0;
    } else {
        if (snake_col != 0) {
            snake_col--;
            return;
        }
        snake_right = 1;
    }

    if (snake_down) {
        if (snake_row + 1u < LED_ROWS) {
            snake_row++;
        } else {
            snake_down = 0;
        }
    } else {
        if (snake_row != 0) {
            snake_row--;
        } else {
            snake_down = 1;
        }
    }
}

/*
 * Concentric ripple -- rings from CODE 0x2959, one per frame, with hue
 * advancing by 13 steps per ring as documented in the notes on 0x746A.
 *
 * Identifiers equal `col * 8 + row`; those referring to a column beyond the
 * fifteenth come from a wider model and are discarded by `spark_seed`.
 */
static void ripple_step(void)
{
    uint8_t slot;

    for (slot = 0; slot < AULA_FX_RING_SLOTS; slot++) {
        const uint8_t id = aula_fx_ring(ripple_ring, slot);

        /*
         * `0xFF` marks an empty slot in the ring -- but the factory table also
         * contains keys that THIS keyboard does not have: eight identifiers in
         * columns 15 and 16 (`0x7A`-`0x7D`, `0x80`-`0x85`), belonging to the
         * twenty-one-column model from the same family. `spark_seed()` filters
         * them out via its column bound; this is intentional, not an accident.
         */
        if (id == AULA_FX_NO_KEY) {
            continue;
        }
        spark_seed((uint8_t)(id >> 3), (uint8_t)(id & 7u),
                   (uint8_t)(fx_phase192 + (uint8_t)(ripple_ring * 13u)));
    }
    if (++ripple_ring >= AULA_FX_RINGS) {
        ripple_ring = 0;
    }
}

/*
 * Called ONCE per animation frame, when the regeneration cursor wraps -- and
 * not from `led_regen_one()`, which visits each column six times per frame.
 * Seeding there would make rain and snake run six times too fast, with no
 * warning in the build log.
 */
/* Weighted distance from a cell to an origin: rows count double. */
static uint8_t lake_dist(uint8_t col, uint8_t row, uint8_t c0, uint8_t r0)
{
    const uint8_t dc = (col > c0) ? (uint8_t)(col - c0) : (uint8_t)(c0 - col);
    const uint8_t dr = (row > r0) ? (uint8_t)(row - r0) : (uint8_t)(r0 - row);

    return (uint8_t)(dc + (uint8_t)(dr << 1));
}

/* A key press spawns a drop: uses a free slot, or overwrites the oldest. */
static void lake_drop(uint8_t col, uint8_t row, uint8_t hue)
{
    uint8_t i, pick = 0, oldest = 0;

    for (i = 0; i < LAKE_DROPS; i++) {
        if (drop_age[i] == 0) {
            pick = i;
            break;
        }
        if (drop_age[i] > oldest) {
            oldest = drop_age[i];
            pick   = i;
        }
    }
    drop_col[pick] = col;
    drop_row[pick] = row;
    drop_hue[pick] = hue;
    drop_age[pick] = 1;
    spark_seed(col, row, hue); /* impact point lights up immediately */
}

/*
 * Full reset on effect change: both the intensity plane AND the seeder state.
 * Without this, rain starts with columns already mid-fall and snake starts
 * with its head left wherever it happened to be.
 */
static void fx_reset(void)
{
    uint8_t col;

    spark_reset();
    for (col = 0; col < LED_COLS; col++) {
        rain_row[col] = 0xff;
    }
    snake_col    = 0;
    snake_row    = 0;
    snake_right  = 1;
    snake_down   = 1;
    ripple_ring     = 0;
    fx_ms_acc       = 0;
    for (col = 0; col < LAKE_DROPS; col++) {
        drop_age[col] = 0;
    }
    spark_decay_due = 0;

    /*
     * Phase resets to zero, just like in the factory firmware. The dispatcher
     * at 0x1AA8 reseeds the selected effect's plane ON EVERY selection -- one
     * branch per effect, ours calling the transposer 0xACA3. Without this
     * line, returning to the wave would resume wherever another effect left
     * the shared counter, instead of starting from the factory phase field.
     */
    led_phase = 0;
    fx_phase192 = 0;
#ifdef MATRIX_INTERLEAVED_SCAN
    ET2                 = 0;
    regen_steps_pending = (uint8_t)(LED_ROWS * LED_COLS);
    ET2                 = 1;
#endif
}

/*
 * FACTORY PERIODS -- extracted from the dump, no longer inferred.
 *
 * `CODE 0x2FE9` holds fifteen 5-byte tables, indexed by speed 0 to 4. The unit
 * is the factory systick, and one tick equals ONE MILLISECOND: the reset
 * shortcut requires its counter to reach 3000 ticks (`0x870C`), and a long
 * press is three seconds.
 *
 * Which table each effect loads is decided by its SEEDER, in dispatch table
 * `0x1744` -- one entry per effect, keyed by the requested effect ID. Hence
 * the mapping below, taken straight from the dump:
 *
 *   our effect          factory effect  engine    factory table
 *   ------------------  --------------  --------  ------------------------------
 *   FX_RADIAL                 1         0x746A    none -- FIXED period 10, hardcoded in `0x1B61`
 *   FX_HORIZONTAL             2         0x7C12    `0x2FF3`
 *   FX_VERTICAL              11         0x6C9B    `0x3016`
 *   FX_SOLID                  3         0x9DA4    `0x2FF8`
 *   AULA_FX_REACTIVE         12         0x64F1    `0x301B`
 *   AULA_FX_RAIN              5         0x9B2B    `0x2FE9`
 *   AULA_FX_TWINKLE           8         0xAC1C    NONE -- choice: uses the other shimmer table (effect 6)
 *   AULA_FX_SNAKE            10         0x8DDF    `0x3011`
 *   AULA_FX_SNAKE_RGB      0x26         0x1D05    NONE -- choice: uses the snake table, same engine
 *   AULA_FX_RIPPLE           17         0x746A    `0x302F`
 *   AULA_FX_LAKE              -             -     NONE -- original: uses the ripple table
 *   AULA_FX_KEYWAVE          15         0x82A5    `0x3025`
 *   AULA_FX_VRAINBOW         16         0x9659    `0x302A`
 *   AULA_FX_GAMING         0x20         0x95A4    NONE -- static image: choice, uses the solid table
 *   AULA_FX_SHIMMER           6         0x60C9    `0x3002`
 *
 * Three entries are therefore a CHOICE rather than a transcription; they are
 * marked as such. The remaining ten are exact bytes from the dump.
 */
static const __code uint8_t fx_period_ms[AULA_FX_OFF][LED_SPEED_LEVELS] = {
    { 10,  10,  10,  10,  10}, /* FX_RADIAL     -- fixed period from 0x1B61    */
    { 45,  35,  25,  15,   5}, /* FX_HORIZONTAL -- CODE 0x2FF3                 */
    { 30,  24,  18,  12,   6}, /* FX_VERTICAL   -- CODE 0x3016                 */
    { 45,  35,  25,  15,   6}, /* FX_SOLID      -- CODE 0x2FF8                 */
    { 32,  24,  18,  16,   6}, /* REACTIVE      -- CODE 0x301B                 */
    {120, 100,  80,  50,  20}, /* RAIN          -- CODE 0x2FE9                 */
    { 20,  15,  10,   5,   1}, /* TWINKLE       -- CODE 0x3002, choice         */
    {120,  90,  70,  45,   1}, /* SNAKE         -- CODE 0x3011                 */
    {120,  90,  70,  45,   1}, /* SNAKE_RGB     -- snake table, choice         */
    { 50,  40,  30,  20,   8}, /* RIPPLE        -- CODE 0x302F                 */
    { 50,  40,  30,  20,   8}, /* LAKE          -- original: ripple table      */
    { 32,  24,  16,   8,   1}, /* KEYWAVE       -- CODE 0x3025                 */
    { 46,  36,  26,  16,   6}, /* VRAINBOW      -- CODE 0x302A                 */
    { 45,  35,  25,  15,   6}, /* GAMING        -- static image, choice        */
    { 20,  15,  10,   5,   1}, /* SHIMMER       -- CODE 0x3002                 */
};

/*
 * Duration of one regeneration sweep, in milliseconds.
 *
 * `tick.c` alternates ONE matrix scan and `LED_SUBFRAMES_PER_SCAN` = 15 LED
 * subframes. One subframe lasts 400 us (`RELOAD_LED_SUBFRAME`) and the scan
 * slot takes roughly 420 us, giving 6.42 ms per group of fifteen. One cell is
 * regenerated per subframe, so all 90 cells require six groups: 38.5 ms.
 */
#define LED_SWEEP_MS ((uint8_t)38)

/*
 * WARNING: WHAT THIS ARCHITECTURE CANNOT RENDER.
 *
 * The factory firmware redraws the entire panel every frame and can therefore
 * go down to one millisecond. Here a frame COSTS one full regeneration sweep,
 * 38.5 ms: any shorter period is clamped to "one frame". The table above is
 * not invalidated -- it preserves the relative pacing between effects, and
 * both rain and snake keep their speed gradient -- but fast periods get
 * clamped at the bottom, which is our floor, not the factory firmware's.
 *
 * That is also why `led_speeds[]` continues to vary the phase STEP: the
 * factory keeps a fixed step and varies the frame rate, whereas we can only
 * vary the frame rate by a factor of three. Without a variable step, SPD_UP
 * and SPD_DN would be barely noticeable. That is a deliberate design choice.
 */
static bool fx_frame_advance(void)
{
    const uint8_t period = fx_period_ms[user_settings.led_effect][user_settings.led_speed];

    fx_ms_acc = (uint8_t)(fx_ms_acc + LED_SWEEP_MS);
    if (fx_ms_acc < period) {
        return false;
    }
    fx_ms_acc = (uint8_t)(fx_ms_acc - period);
    if (fx_ms_acc >= period) {
        fx_ms_acc = 0; /* a frame cannot run faster than one sweep */
    }

    /*
     * SCROLL DIRECTION -- transcribed from bit `0x23` of the factory firmware.
     *
     * `0xEDA2` does not only increment: it tests this bit and DECREMENTS when
     * set, wrapping to `modulus - 1`. `rgb_scroll_step` (`0xA3F4`) and the
     * four-direction state machine (`0x5C98`) also read it. Its source is
     * `b0[7]` of the per-effect record (`0x1172`) and, globally, `[0x0315]`
     * bit 7 (`0x1121`).
     *
     * Here it is global, like `[0x0315]`, and stored in
     * `user_settings.ul_brightness` -- a byte reserved by the shared struct
     * for underglow, which this keyboard lacks, and which no shared file
     * touches. Same rationale as for the color mode: avoid growing
     * `user_settings_t`, which would invalidate all saved settings.
     */
    {
        const uint8_t step = led_speeds[user_settings.led_speed];

        if (user_settings.ul_brightness != 0) {
            led_phase   = (uint8_t)(led_phase - step);
            fx_phase192 = (uint8_t)((fx_phase192 + FX_PHASE192 - step) % FX_PHASE192);
        } else {
            led_phase   = (uint8_t)(led_phase + step);
            fx_phase192 = (uint8_t)((fx_phase192 + step) % FX_PHASE192);
        }
    }

    switch (user_settings.led_effect) {
        case AULA_FX_RAIN:      rain_step();     break;
        case AULA_FX_TWINKLE:   twinkle_step();  break;
        case AULA_FX_SNAKE:     snake_step(0);   break;
        case AULA_FX_SNAKE_RGB: snake_step(1);   break;
        case AULA_FX_RIPPLE:    ripple_step();   break;
        case AULA_FX_LAKE: {
            uint8_t i;

            for (i = 0; i < LAKE_DROPS; i++) {
                if (drop_age[i] != 0 && ++drop_age[i] >= LAKE_MAX_AGE) {
                    drop_age[i] = 0; /* the wave has left the keyboard */
                }
            }
            break;
        }
        default:                                 break;
    }
    return true;
}

#define LED_BRIGHTNESS_DEFAULT (LED_BRIGHTNESS_LEVELS - 1)
#define LED_SPEED_DEFAULT      2


/* ------------------------------------------------------------------- output */

void indicators_pwm_enable(void)
{
    /*
     * Channel 0 of each bank carries the enable bit and clock divider; the
     * other five only carry PWM_SS. Same split as on the three other keyboards
     * in SMK.
     */
#ifdef MATRIX_INTERLEAVED_SCAN
    PWM01CON = PWM_SS;
    PWM02CON = PWM_SS;
    PWM03CON = PWM_SS;
    PWM04CON = PWM_SS;
    PWM05CON = PWM_SS;

    PWM11CON = PWM_SS;
    PWM12CON = PWM_SS;
    PWM13CON = PWM_SS;
    PWM14CON = PWM_SS;
    PWM15CON = PWM_SS;

    PWM21CON = PWM_SS;
    PWM22CON = PWM_SS;
    PWM23CON = PWM_SS;
    PWM24CON = PWM_SS;
    PWM25CON = PWM_SS;

    /*
     * Start all three bank counters back-to-back with the identical byte (0x89)
     * in exact 5-cycle lockstep -- matching the 5-cycle stop spacing in
     * `pwm_interrupt_handler()` -- and then arm _INT_PWM0 on bank 0 (0xC9).
     */
    PWM00CON = LED_PWM_MASTER;
    PWM10CON = LED_PWM_MASTER;
    PWM20CON = LED_PWM_MASTER;
    PWM00CON = (uint8_t)(LED_PWM_MASTER | PWM_INT_ENABLE);
    IEN1 |= _EPWM0;
#else
    PWM00CON = LED_PWM_MASTER;
    PWM01CON = PWM_SS;
    PWM02CON = PWM_SS;
    PWM03CON = PWM_SS;
    PWM04CON = PWM_SS;
    PWM05CON = PWM_SS;

    PWM10CON = LED_PWM_MASTER;
    PWM11CON = PWM_SS;
    PWM12CON = PWM_SS;
    PWM13CON = PWM_SS;
    PWM14CON = PWM_SS;
    PWM15CON = PWM_SS;

    PWM20CON = LED_PWM_MASTER;
    PWM21CON = PWM_SS;
    PWM22CON = PWM_SS;
    PWM23CON = PWM_SS;
    PWM24CON = PWM_SS;
    PWM25CON = PWM_SS;
#endif
}

/*
 * Full blanking, which requires two separate actions: parking the PWM banks is
 * not enough, because a parked pin falls back to the port latch value. It is
 * RELEASING THE COLUMNS that actually cuts current -- with no column selected,
 * there is no current path.
 *
 * Called from three unrelated places: `matrix.c` before each scan, `sleep.c`
 * before entering sleep, and `settings_save_pre()` around flash page erases.
 * All three want the same thing: a dark panel and released columns.
 */
/*
 * Saving settings erases a flash page, and that erase runs with interrupts
 * disabled for ~5 ms. Without these two hooks, `tick_dispatch` stays frozen
 * during that time with the last subframe's column STILL selected and all
 * three PWM banks active: that column would conduct for 5 ms instead of
 * 400 us, roughly a dozen times its nominal duty cycle.
 *
 * And this is not a rare event: every change of effect, brightness, speed, or
 * Bluetooth slot calls `settings_mark_dirty()`. Same fix as on the NuPhy
 * Air60, for the same reason.
 */
void settings_save_pre(void)
{
    tick_pause();
    indicators_pwm_disable();
}

void settings_save_post(void)
{
    indicators_pwm_enable();
    tick_resume();
}

void indicators_pwm_disable(void)
{
#ifdef MATRIX_INTERLEAVED_SCAN
    user_matrix_cols_deselect_all();

    PWM00CON = PWM_CON_PARKED;
    PWM10CON = PWM_CON_PARKED;
    PWM20CON = PWM_CON_PARKED;

    PWM01CON = PWM_CON_OFF;
    PWM02CON = PWM_CON_OFF;
    PWM03CON = PWM_CON_OFF;
    PWM04CON = PWM_CON_OFF;
    PWM05CON = PWM_CON_OFF;

    PWM11CON = PWM_CON_OFF;
    PWM12CON = PWM_CON_OFF;
    PWM13CON = PWM_CON_OFF;
    PWM14CON = PWM_CON_OFF;
    PWM15CON = PWM_CON_OFF;

    PWM21CON = PWM_CON_OFF;
    PWM22CON = PWM_CON_OFF;
    PWM23CON = PWM_CON_OFF;
    PWM24CON = PWM_CON_OFF;
    PWM25CON = PWM_CON_OFF;
#else
    PWM00CON = PWM_CON_PARKED;
    PWM10CON = PWM_CON_PARKED;
    PWM20CON = PWM_CON_PARKED;

    user_matrix_cols_deselect_all();
#endif
}

/* ---------------------------------------------------------------- animation */

/*
 * One cell per subframe. The cursor sweeps the grid and advances the phase on
 * each full pass, just like the genesis-thor-300: advancing phase per frame
 * rather than per subframe prevents lowering brightness from also slowing down
 * the effect.
 */
static uint8_t led_scale(uint8_t value, uint8_t gain)
{
    return (uint8_t)(((uint16_t)value * gain) >> 8);
}

/*
 * FACTORY COLOR SELECTION.
 *
 * The factory firmware does not render everything in rainbow mode: each of its
 * effects carries a 4-bit color mode, `b1[3:0]`, latched at XRAM 0x0897, and
 * its rendering logic fits in a single line (`0x60C9`, `0x6C9B`):
 *
 *     color = (mode == 7) ? wheel[phase] : palette[effect][mode]
 *
 * Modes 0 to 6 are the seven fixed colors at `CODE 0xC800` -- identical across
 * all eighteen effects -- while mode 7 is rainbow, the only mode that reads
 * the color wheel. Previously this port only rendered mode 7; all eight are
 * now supported.
 *
 * The mode lives in `user_settings.ul_effect`, a byte reserved by the shared
 * struct for underglow -- which this keyboard does not have. Reusing it avoids
 * growing `user_settings_t`, which would reset ALL saved settings to defaults
 * on first boot (`nvm.c` checks the struct size).
 */
static void fx_color(uint8_t wheel_index, uint8_t out[3])
{
    if (user_settings.ul_effect >= AULA_FX_COLOR_WHEEL) {
        aula_rgb_wheel(wheel_index, out);
    } else {
        aula_fx_color(user_settings.ul_effect, out);
    }
}

/* --------------------------------------------------- radio diagnostic overlay
 *
 * The wireless link fails in a setup where USB is UNPLUGGED -- keyboard on
 * battery, dongle in the PC. SMK's HID console does not exist there. The
 * ninety LEDs, however, do work: they are the only instrument available where
 * the fault actually occurs.
 *
 * `Fn + R` paints the driver state across keys `1` to `0` (row 1, columns 1 to
 * 10). The overlay bypasses user brightness: it must remain readable even when
 * brightness is set to zero or the effect is turned off.
 */
#ifdef RF_EUART0

static __bit rf_diag_on;

void indicators_toggle_rf_diag(void)
{
    rf_diag_on = !rf_diag_on;
}

/* Returns false if the current cell is not part of the overlay. */
static bool rf_diag_paint(void)
{
    uint8_t field;
    uint8_t v;

    if (regen_row != 1 || regen_col < 1 || regen_col > RF_DIAG_FIELDS) {
        return false;
    }
    field = (uint8_t)(regen_col - 1);
    v     = rf_diag_state[field];

    switch (field) {
        case RF_DIAG_LINK:
            /* off = wired, blue = 2.4 GHz, cyan = Bluetooth */
            aula_rgb_set(regen_row, regen_col, 0, (v == 2) ? 255 : 0, v ? 255 : 0);
            break;
        case RF_DIAG_NAME:
            /* 0 off, 1 yellow, 2 green */
            aula_rgb_set(regen_row, regen_col, (v == 1) ? 255 : 0, v ? 255 : 0, 0);
            break;
        case RF_DIAG_QCOUNT:
        case RF_DIAG_MISSES:
            /* counter: brighter red as the count increases */
            aula_rgb_set(regen_row, regen_col, v ? (uint8_t)(40 + v * 35) : 0, 0, 0);
            break;
        default:
            /* boolean: green = 1, red = 0 */
            aula_rgb_set(regen_row, regen_col, v ? 0 : 255, v ? 255 : 0, 0);
            break;
    }
    return true;
}

/* --------------------------------------------------- battery level overlay
 *
 * `Fn + B` (`BAT_SHOW`) paints the damped battery percentage (`rf_batt_pct`)
 * across keys `1` to `0` (row 1, columns 1 to 10) for ~3 seconds (80 sweeps
 * of ~37.9 ms), bypassing user brightness so it remains readable when RGB is
 * off or dimmed.
 */
#define BAT_SHOW_SWEEPS 80u

static volatile __xdata uint8_t bat_show_sweeps;

void indicators_show_battery(void)
{
    ET2             = 0;
    bat_show_sweeps = (bat_show_sweeps != 0) ? 0 : (uint8_t)BAT_SHOW_SWEEPS;
    ET2             = 1;
}

static bool bat_show_paint(void)
{
    uint8_t pct;
    uint8_t lit;
    uint8_t r = 0;
    uint8_t g = 0;

    if (regen_row != 1 || regen_col < 1 || regen_col > 10) {
        return false;
    }

    pct = rf_batt_pct;
    lit = (pct == 0) ? 1u : (uint8_t)((pct + 9u) / 10u);

    if (regen_col <= lit) {
        if (pct <= 20) {
            r = 255;
        } else if (pct <= 50) {
            r = 255;
            g = 180;
        } else {
            g = 255;
        }
    }
    aula_rgb_set(regen_row, regen_col, r, g, 0);
    return true;
}

#endif /* RF_EUART0 */

/*
 * Auto-typing indicator: a BEATING HEART, in red, across the entire keyboard.
 *
 * Two beats per cycle -- the "lub-dub" of a real heart, not a breathing ramp.
 * The first (systole) hits hard, the second, 227 ms later, hits softer,
 * followed by a long low-glow rest that keeps the active mode visible.
 *
 * TIMING. One table step per full regeneration sweep, i.e. ninety subframes of
 * ~421 us, or ~37.9 ms. Twenty-four steps give 909 ms per heartbeat: **66 beats
 * per minute**, a resting pulse.
 *
 * Why sync to the full sweep rather than an arbitrary rate: the ninety cells
 * are repainted ONE PER SUBFRAME, not all at once. Any rate not aligned with
 * the sweep would change the value mid-repaint, showing a visible seam across
 * the keyboard -- half at the old step, half at the new one. Advancing on
 * sweep wrap ensures the entire panel always displays the same value.
 *
 * VALUES. `aula_rgb_duty()` is linear (`value << 2`), so these are true
 * intensities. The floor at 12 stays visible in the dark without lighting up
 * the room; the peak at 170 pops without dazzling. Like the radio diagnostic,
 * the table bypasses user brightness: the indicator must remain visible even
 * with brightness at zero and effects turned off.
 */
#define MACRO_HEART_STEPS 24

static const __code uint8_t macro_heart[MACRO_HEART_STEPS] = {
    170, 140,  96,  60,  36,  24, /* systole: sharp attack, fast decay */
    120,  92,  62,  40,  26,  18, /* diastole: softer, same profile    */
     14,  12,  12,  12,  12,  12, /* rest: ~455 ms of dim glow         */
     12,  12,  12,  12,  12,  12,
};

static __xdata uint8_t macro_beat;
static __bit           macro_was_on;

/* ------------------------------------------------------------------ Caps Lock
 *
 * The host's HID LED report arrives via `usb_ep0_out_irq()` and lands in
 * `keyboard_state.led_state`: bit 0 Num Lock, **bit 1 Caps Lock**, bit 2
 * Scroll Lock. This keyboard has no dedicated indicator -- the key itself
 * lights up, in white, at its grid position.
 *
 * WARNING: WIRED ONLY. In 2.4 GHz and Bluetooth modes USB is unmounted, and nothing
 * in the EUART0 protocol decoded so far reports the host LED state back.
 * `rf_radio_on()` therefore resets `led_state` to zero: showing nothing is
 * better than showing stale state.
 */
#define HID_LED_CAPS_LOCK (1u << 1)

/*
 * THE INDICATOR IS A DEDICATED LED, AND IT IS NOT AN LED: IT IS A CHANNEL.
 *
 * Established on the device, first by scanning and then by probing channel by
 * channel. Position `[row 0][col 14]` -- electrically the encoder press, top
 * right -- carries a full RGB position whose **three channels drive distinct
 * status indicators** physically located elsewhere on the board:
 *
 *   RED   -> Caps Lock indicator, to the LEFT of the Caps Lock key
 *   GREEN -> indicator between Esc and F1
 *   BLUE  -> the same indicator between Esc and F1
 *
 * The status indicators are single-color LEDs: the factory uses a spare matrix
 * slot as three independent drivers. Writing white here lights both indicators
 * at once -- which is what happened on the first try.
 *
 * Two claims from our earlier sources fall with this measurement:
 *  - "the encoder press has no LED" (layout.c, aula-f75.md): the POSITION does
 *    have one, and even three useful channels; it is the KEY that has nothing
 *    under its cap;
 *  - "this keyboard has no dedicated indicator": it has at least two.
 *
 * This also explains OpenRGB's count, which maps only EIGHTY of the ninety
 * LEDs to a key: the other ten are not all empty holes, some are real LEDs
 * without a keycap above them.
 *
 * Consequence: this cell must be painted explicitly, channel by channel.
 * Otherwise effects blink both status indicators at random -- which is what
 * they did from the very first flash.
 *
 * The Esc/F1 indicator (green and blue) carries status patterns: transport,
 * pairing, sleep, low battery. See `aula_status.c`. Both channels drive the
 * SAME white LED and their intensities add up -- there is neither color nor a
 * second indicator to exploit, only rhythm.
 */
#define CAPS_ROW 0
#define CAPS_COL 14

/* The channel is driven at full scale: the indicator is a single LED behind a
 * diffuser, not a backlit keycap, and its color comes from the LED component
 * itself -- not from us. Like the other overlays, it bypasses user brightness:
 * an indicator that turns invisible at zero brightness is useless, and zero
 * brightness is precisely when Caps Lock is easiest to forget. */
#define CAPS_LEVEL 255

static __bit caps_on;
#ifdef SIGNALRGB_ENABLE
#    define DIRECT_RGB_TIMEOUT_FRAMES 480u /* ~3 s at ~6.2 ms/frame */
static __xdata uint8_t direct_rgb[LED_ROWS][LED_COLS][3];
static volatile bool   direct_active;
static volatile uint16_t direct_timeout;
#endif
#ifdef RF_EUART0
static __xdata uint8_t pair_blink_cnt;
static __xdata uint8_t prev_link_fx = 0xFF;
static __xdata uint8_t flourish_step; /* 0 = idle, 1..30 = mode-switch wipe */
#endif

static void led_regen_one(void)
{
    const uint8_t gain = led_brightness_gain[user_settings.led_brightness];
    uint8_t       rgb[3];

    /*
     * SUPPRESSION OF KEYLESS POSITIONS, transcribed from the factory engine.
     *
     * Continuous engines do not paint the entire grid: `0x82A5` reads the key
     * position from table A (`CODE 0x2EED`) and ONLY renders if it is less
     * than fifteen, then also checks `rgb_key_suppressed` (`0x598F`); `0x6C9B`
     * performs the same check before writing. Six of the ninety slots in the
     * 6 x 15 grid have no key at all, and this page gets them from
     * `CODE 0xC500`.
     *
     * The intensity-plane seeders already checked for key presence; the
     * continuous paths -- wave, vertical rainbow, solid, SMK geometry -- did
     * not, paying a color lookup for nothing.
     */
    if (aula_fx_key_id(regen_col, regen_row) == AULA_FX_NO_KEY) {
        /* Hoisted to the top of the chain: the overlays below paint
         * unconditionally, and painting these six holes would light up LEDs
         * where there is no keycap. */
        aula_rgb_set(regen_row, regen_col, 0, 0, 0);
        goto advance;
    }

    if (regen_row == CAPS_ROW && regen_col == CAPS_COL) {
        /* Status indicators: never painted by effects or by the auto-typing
         * heartbeat. Red = Caps Lock; green and blue = the Esc/F1 indicator,
         * driven with the same value since both sum onto a single LED. */
        const uint8_t st = aula_status_level();

        aula_rgb_set(regen_row, regen_col, caps_on ? (uint8_t)CAPS_LEVEL : (uint8_t)0, st, st);
        goto advance;
    }

#ifdef RF_EUART0
    if (rf_diag_on && rf_diag_paint()) {
        goto advance; /* cell claimed by the diagnostic overlay */
    }
    if (bat_show_sweeps && bat_show_paint()) {
        goto advance; /* cell claimed by the battery overlay */
    }
    if (rf_bt_pair_slot != 0 && user_settings.battery_indicator_on != 0 &&
        regen_row == 1 && regen_col == rf_bt_pair_slot) {
        const bool on = rf_bt_pair_fast ? ((pair_blink_cnt & 0x04u) == 0)
                                        : ((pair_blink_cnt & 0x08u) == 0);
        aula_rgb_set(regen_row, regen_col, on ? 255u : 0u, on ? 255u : 0u, on ? 255u : 0u);
        goto advance;
    }
    if (flourish_step != 0) {
        const uint8_t head = (uint8_t)((flourish_step - 1u) >> 1);
        if (regen_col == head || (head > 0 && regen_col == (uint8_t)(head - 1u))) {
            aula_fx_color((uint8_t)(regen_col * 16u + regen_row * 8u), rgb);
            aula_rgb_set(regen_row, regen_col, rgb[0], rgb[1], rgb[2]);
        } else {
            aula_rgb_set(regen_row, regen_col, 0, 0, 0);
        }
        goto advance;
    }
#endif

    if (aula_macro_active()) {
        /*
         * Auto-typing indicator: the entire keyboard pulses deep red, and the
         * effect engine is bypassed for the duration. Like the diagnostic, it
         * ignores user brightness -- it must remain visible at zero brightness,
         * and especially when effects are OFF, which is the exact state in
         * which the keyboard is left running unattended.
         */
        aula_rgb_set(regen_row, regen_col, macro_heart[macro_beat], 0, 0);
        goto advance;
    }

#ifdef SIGNALRGB_ENABLE
    if (direct_active) {
        aula_rgb_set(regen_row, regen_col,
                     direct_rgb[regen_row][regen_col][0],
                     direct_rgb[regen_row][regen_col][1],
                     direct_rgb[regen_row][regen_col][2]);
        goto advance;
    }
#endif

    if (user_settings.led_effect >= AULA_FX_OFF) {
        /*
         * Effect off. This case was unreachable before overlays were added:
         * `indicators_update_step()` skipped rendering altogether when the
         * effect was AULA_FX_OFF. It now calls into here to paint the
         * diagnostic and macro indicators, so this path must output black
         * rather than falling through to the geometric branch -- which would
         * pass index 15 to `led_effect_index()`, whose enum stops at 3.
         */
        aula_rgb_set(regen_row, regen_col, 0, 0, 0);
    } else if (user_settings.led_effect >= AULA_FX_REACTIVE &&
        user_settings.led_effect <= AULA_FX_LAKE) {
        /*
         * Wavefront check for the lake effect: is this cell at the exact
         * distance reached by one of the drops? Four comparisons, once per
         * cell per subframe.
         */
        if (user_settings.led_effect == AULA_FX_LAKE) {
            uint8_t i;

            for (i = 0; i < LAKE_DROPS; i++) {
                if (drop_age[i] != 0 &&
                    lake_dist(regen_col, regen_row, drop_col[i], drop_row[i]) == drop_age[i]) {
                    spark_seed(regen_col, regen_row,
                               (uint8_t)(drop_hue[i] + (uint8_t)(drop_age[i] * LAKE_HUE_STEP)));
                }
            }
        }
        /* The six intensity-plane effects: rendering is identical for all of
         * them, only the seeder differs. Decay lives here because each cell is
         * regenerated exactly once per frame. */
        const uint8_t val = spark_val[regen_col][regen_row];

        if (val == 0) {
            aula_rgb_set(regen_row, regen_col, 0, 0, 0);
        } else {
            const uint8_t k = led_scale(val, gain);

            fx_color(spark_hue[regen_col][regen_row], rgb);
            aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], k), led_scale(rgb[1], k),
                         led_scale(rgb[2], k));
            if (spark_decay_due) {
                /*
                 * The lake decays THREE TIMES faster than the reactive engine.
                 * At 8 per frame, the wake lasts 32 frames while the wave takes
                 * 25 frames to cross the keyboard: the wake would blur into the
                 * front and look like an expanding disk rather than a ripple.
                 * At 24, the wake is about ten frames wide -- a ring with a
                 * trailing tail. This is a deliberate CHOICE for this effect.
                 */
                const uint8_t decay =
                    (user_settings.led_effect == AULA_FX_LAKE) ? spark_decay_lake[user_settings.led_speed] : spark_decay_react[user_settings.led_speed];

                spark_val[regen_col][regen_row] = (val > decay) ? (uint8_t)(val - decay) : 0;
            }
        }
    } else if (user_settings.led_effect == AULA_FX_VRAINBOW) {
        /*
         * Scrolling vertical rainbow -- transcribed from 0x9659: hue advances
         * by 25 steps PER ROW, modulo 192, and its base scrolls from frame to
         * frame. Six rows x 25 cover 150 of the 192 entries, spanning nearly a
         * full rainbow from top to bottom of the keyboard.
         */
        const uint8_t hue = (uint8_t)((fx_phase192 + (uint8_t)(regen_row * 25u)) %
                                      AULA_RGB_WHEEL_SIZE);

        fx_color(hue, rgb);
        aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                     led_scale(rgb[2], gain));
    } else if (user_settings.led_effect == AULA_FX_KEYWAVE) {
        /*
         * Wave over the 128-entry palette -- 0x82A5, now fully transcribed:
         * the palette, the one-step-per-frame-per-key advance (0xEDA2), AND
         * the per-key phase field.
         *
         * The phase field was previously noted as our own invention for lack
         * of finding its writer. There is no runtime writer: it is a CONSTANT
         * array that the C initializer places at XDATA 0x0E49 and that 0xACA3
         * transposes into plane 0x0017. `aula_fx_keywave()` carries it as-is.
         *
         * Each key starts at its initial phase and advances by one step per
         * frame, which reduces to `key_phase + frame_counter`. No per-key
         * state needed: the function is pure.
         */
        const uint8_t idx = (uint8_t)(aula_fx_keywave(regen_col, regen_row) + led_phase);

        if (user_settings.ul_effect >= AULA_FX_COLOR_WHEEL) {
            aula_fx_palette(idx, rgb); /* the second factory wheel, 128 hues */
        } else {
            aula_fx_color(user_settings.ul_effect, rgb);
        }
        aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                     led_scale(rgb[2], gain));
    } else if (user_settings.led_effect == AULA_FX_GAMING) {
        /*
         * Static image from CODE 0xCAFC: Esc, W A S D, and the arrow cluster.
         *
         * The hardcoded blue that used to be here was based on a misreading of
         * the table -- three color planes of which only blue was non-zero.
         * `0xCAFC` is actually a one-byte-per-key MASK with no embedded color
         * (see aula_fx.c). The color therefore comes from the color mode, just
         * like every other effect; blue remains available as mode 2.
         */
        if (aula_fx_gaming(regen_col) & (uint8_t)(1u << regen_row)) {
            fx_color(fx_phase192, rgb);
            aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                         led_scale(rgb[2], gain));
        } else {
            aula_rgb_set(regen_row, regen_col, 0, 0, 0);
        }
    } else if (user_settings.led_effect == AULA_FX_SHIMMER) {
        /*
         * Shimmer -- factory effect 6, engine `0x60C9`, transcribed.
         *
         * Each key carries its own phase, seeded by `0xA791` from one of two
         * tables depending on the color mode, and advances by one step per
         * frame via `0xEDA2(phase, 0xC0 or 0x60)`. Since all keys advance by
         * the same step, the current phase equals `seed + frame_counter`: no
         * state to maintain, using the same trick as the wave.
         *
         * In rainbow mode the phase indexes the color wheel modulo 192; in
         * fixed-color mode it indexes the breathing ramp modulo 96, and the
         * fixed color is scaled by that ramp. The factory engine does both of
         * these things, with those exact moduli.
         *
         * `led_phase` advances by `led_speeds[speed]` rather than by one: that
         * is this port's design choice, explained above, and applied here just
         * like on the wave.
         */
        if (user_settings.ul_effect >= AULA_FX_COLOR_WHEEL) {
            const uint16_t p = (uint16_t)aula_fx_shimmer_phase(regen_col, regen_row, 1) + fx_phase192;

            aula_rgb_wheel((uint8_t)(p % AULA_RGB_WHEEL_SIZE), rgb);
            aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                         led_scale(rgb[2], gain));
        } else {
            const uint16_t p = (uint16_t)aula_fx_shimmer_phase(regen_col, regen_row, 0) + fx_phase192;
            const uint8_t  k = aula_fx_breath((uint8_t)(p % AULA_FX_BREATH_SIZE));

            aula_fx_color(user_settings.ul_effect, rgb);
            aula_rgb_set(regen_row, regen_col, led_scale(led_scale(rgb[0], k), gain),
                         led_scale(led_scale(rgb[1], k), gain),
                         led_scale(led_scale(rgb[2], k), gain));
        }
    } else if (user_settings.led_effect == (uint8_t)FX_SOLID) {
        /*
         * Hardcoded white was an invention: factory effect index 3 (`0x9DA4`)
         * is "a single-color matrix, with hue computed outside the loop". That
         * color comes from the color mode -- the color wheel at the current
         * phase in rainbow mode, or the fixed color otherwise. White remains
         * available as mode 6.
         */
        fx_color(fx_phase192, rgb);
        aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                     led_scale(rgb[2], gain));
    } else {
        /*
         * SMK geometry, factory colors: the 0-255 index is rescaled to the 192
         * entries of the color wheel.
         *
         * The column passed in is NOT the electrical column but the SPATIAL
         * column, taken from factory table A (`CODE 0x2EED`). The generated
         * geometry assumes a uniform grid -- `axis_x[col] = col * 17` -- and
         * that assumption fails on row 4, where electrical column 12 sits
         * physically between columns 0 and 1. Without this conversion, the
         * horizontal gradient and radial wave place that key in the wrong
         * spot.
         *
         * The ROW, on the other hand, needs no conversion: factory table B
         * (`CODE 0x2F6B`) is identical to the row index across all 126 slots.
         */
        const uint8_t idx =
            led_effect_index((led_effect_t)user_settings.led_effect, regen_row,
                             aula_fx_render_col(regen_col, regen_row), led_phase);

        fx_color((uint8_t)(((uint16_t)idx * AULA_RGB_WHEEL_SIZE) >> 8), rgb);
        aula_rgb_set(regen_row, regen_col, led_scale(rgb[0], gain), led_scale(rgb[1], gain),
                     led_scale(rgb[2], gain));
    }

advance:
    if (++regen_col >= LED_COLS) {
        regen_col = 0;
        if (++regen_row >= LED_ROWS) {
            regen_row = 0;
            /* The entire panel has just been repainted: the heartbeat can
             * advance by one step without tearing the frame in half. */
            if (++macro_beat >= MACRO_HEART_STEPS) {
                macro_beat = 0;
            }
            aula_status_tick();
#ifdef RF_EUART0
            if (bat_show_sweeps != 0) {
                bat_show_sweeps--;
            }
            pair_blink_cnt++;
            if (flourish_step != 0) {
                if (++flourish_step > (uint8_t)(LED_COLS * 2u)) {
                    flourish_step = 0;
                }
            }
#endif
            /* Phase advances INSIDE the frame gate, as in the factory firmware:
             * factory rendering is gated on `tick >= period`. */
            spark_decay_due = fx_frame_advance();
        }
    }
}

void indicators_pre_update(void)
{
#ifdef MATRIX_INTERLEAVED_SCAN
    /*
     * Park PWM banks and release the previous LED column BEFORE selecting the
     * next column for matrix sensing (`indicators_pwm_disable` already calls
     * `user_matrix_cols_deselect_all`, and columns stay outputs while awake).
     */
    indicators_pwm_disable();
#else
    /*
     * `matrix.c` leaves the columns configured as INPUTS when it finishes
     * (`user_matrix_scan_post`). We must switch them back to outputs before
     * selecting one for display.
     */
    user_matrix_scan_pre();
    indicators_pwm_disable();
#endif
}

/*
 * Keypress detection for the reactive engine: one column per poll. Only rising
 * edges count; holding a key down does not retrigger.
 */
static void led_react_poll(void)
{
#ifdef MATRIX_INTERLEAVED_SCAN
    const uint8_t col   = react_col;
    const uint8_t now   = matrix_get_col(col);
#else
    const uint8_t col   = led_col;
    const uint8_t now   = user_matrix_pressed(col);
#endif
    const uint8_t fresh = (uint8_t)(now & (uint8_t)~spark_prev[col]);

    spark_prev[col] = now;

    if (fresh == 0) {
        return;
    }
    for (uint8_t row = 0; row < LED_ROWS; row++) {
        if (fresh & (uint8_t)(1u << row)) {
            if (user_settings.led_effect == AULA_FX_LAKE) {
                lake_drop(col, row, fx_phase192);
            } else {
                spark_hue[col][row] = fx_phase192;
                spark_val[col][row] = 255;
            }
        }
    }
}

#ifdef MATRIX_INTERLEAVED_SCAN
bool indicators_update_step(keyboard_state_t *keyboard, uint8_t current_step)
{
    (void)keyboard;

    /*
     * `matrix_scan_col(current_step)` has already sampled and deselected
     * `current_step`, so all columns are released and all PWM channels are
     * parked. Load all eighteen duty cycles first, select `current_step`, and
     * only then enable PWM for its hardware-timed 100 us period.
     */
    if (display_enabled) {
        aula_rgb_load_column(current_step);
        user_matrix_col_select(current_step);
        indicators_pwm_enable();
    }

    return (current_step == (uint8_t)(LED_COLS - 1u));
}

void indicators_post_update(void)
{
    /* Called at the 400 us Bresenham sub-tick from tick_dispatch(). */
    aula_encoder_sample();
    aula_macro_tick();
    if (regen_steps_pending < (uint8_t)(LED_ROWS * LED_COLS)) {
        regen_steps_pending++;
    }
}

void indicators_render(void)
{
    if (regen_steps_pending == 0u) {
        return;
    }
    ET2 = 0;
    regen_steps_pending--;
    ET2 = 1;

    caps_on = (user_settings.led_color && (keyboard_state.led_state & HID_LED_CAPS_LOCK)) ? 1 : 0;

    if (aula_macro_active()) {
        if (!macro_was_on) {
            macro_was_on = 1;
            macro_beat   = 0; /* start on a beat, not mid-rest */
        }
    } else {
        macro_was_on = 0;
    }

    if (user_settings.led_effect == AULA_FX_REACTIVE ||
        user_settings.led_effect == AULA_FX_LAKE) {
        led_react_poll();
    }
    if (++react_col >= LED_COLS) {
        react_col = 0;
#    ifdef SIGNALRGB_ENABLE
        if (direct_active) {
            if (direct_timeout != 0u) {
                direct_timeout--;
            }
            if (direct_timeout == 0u) {
                direct_active = false;
            }
        }
#    endif
    }

#    ifdef RF_EUART0
    if (keyboard_state.rf_link != prev_link_fx) {
        if (prev_link_fx != 0xFF) {
            flourish_step = 1;
        }
        prev_link_fx = keyboard_state.rf_link;
    }
    const bool active = aula_macro_active() || caps_on || aula_status_lit() ||
                        rf_diag_on || bat_show_sweeps || flourish_step != 0 ||
                        (rf_bt_pair_slot != 0 && user_settings.battery_indicator_on != 0) ||
                        user_settings.led_effect < AULA_FX_OFF
#        ifdef SIGNALRGB_ENABLE
                        || direct_active
#        endif
        ;
#    else
    const bool active = aula_macro_active() || caps_on || aula_status_lit() ||
                        user_settings.led_effect < AULA_FX_OFF
#        ifdef SIGNALRGB_ENABLE
                        || direct_active
#        endif
        ;
#    endif

    display_enabled = active ? 1 : 0;
    if (active) {
        led_regen_one();
    }
}
#else
bool indicators_update_step(keyboard_state_t *keyboard, uint8_t current_step)
{
    (void)current_step;  /* `tick.c` always passes 0 */

    caps_on = (user_settings.led_color && (keyboard->led_state & HID_LED_CAPS_LOCK)) ? 1 : 0;

    /* The LED subframe is the only regular timebase in the firmware --
     * ~400 us -- and that is the same rate at which the factory firmware
     * samples its encoder from its PWM ISR. */
    aula_encoder_sample();
    aula_macro_tick();

    if (aula_macro_active()) {
        if (!macro_was_on) {
            macro_was_on = 1;
            macro_beat   = 0; /* start on a beat, not mid-rest */
        }
    } else {
        macro_was_on = 0;
    }

    if (user_settings.led_effect == AULA_FX_REACTIVE ||
        user_settings.led_effect == AULA_FX_LAKE) {
        led_react_poll();
    }

#ifdef RF_EUART0
    if (keyboard->rf_link != prev_link_fx) {
        if (prev_link_fx != 0xFF) {
            flourish_step = 1;
        }
        prev_link_fx = keyboard->rf_link;
    }
    if (aula_macro_active() || caps_on || aula_status_lit() ||
        rf_diag_on || bat_show_sweeps || flourish_step != 0 ||
        (rf_bt_pair_slot != 0 && user_settings.battery_indicator_on != 0) ||
        user_settings.led_effect < AULA_FX_OFF
#    ifdef SIGNALRGB_ENABLE
        || direct_active
#    endif
    ) {
#else
    if (aula_macro_active() || caps_on || aula_status_lit() ||
        user_settings.led_effect < AULA_FX_OFF
#    ifdef SIGNALRGB_ENABLE
        || direct_active
#    endif
    ) {
#endif
        led_regen_one();

        /* Load all eighteen duty cycles WHILE the banks are parked, then
         * select the column, and only then re-enable PWM. */
        aula_rgb_load_column(led_col);
        user_matrix_col_select(led_col);
        indicators_pwm_enable();
    }

    if (++led_col >= LED_COLS) {
        led_col = 0;
#ifdef SIGNALRGB_ENABLE
        if (direct_active) {
            if (direct_timeout != 0u) {
                direct_timeout--;
            }
            if (direct_timeout == 0u) {
                direct_active = false;
            }
        }
#endif
        return true; /* frame complete -- this is what `sleep.c` counts */
    }
    return false;
}

void indicators_post_update(void)
{
}
#endif

/* ----------------------------------------------------------------- settings */

void indicators_apply_defaults(void)
{
    user_settings.led_effect     = AULA_FX_LAKE; /* requested as default */
    user_settings.led_brightness = LED_BRIGHTNESS_DEFAULT;
    user_settings.led_speed      = LED_SPEED_DEFAULT;
    user_settings.ul_effect            = AULA_FX_COLOR_WHEEL; /* rainbow, as in factory firmware */
    user_settings.ul_brightness        = 0;                   /* forward direction, bit 0x23 cleared */
    user_settings.battery_indicator_on = 1;                   /* VIA: Status Indicator (Esc/F1) */
    user_settings.led_color            = 1;                   /* VIA: Caps Lock Indicator */
    user_settings.ul_speed             = 1;                   /* VIA: Charging & Low Battery Indicator */
}

void indicators_validate_settings(void)
{
    if (user_settings.led_effect > AULA_FX_OFF) {
        user_settings.led_effect = AULA_FX_LAKE;
    }
    if (user_settings.led_brightness >= LED_BRIGHTNESS_LEVELS) {
        user_settings.led_brightness = LED_BRIGHTNESS_DEFAULT;
    }
    if (user_settings.led_speed >= LED_SPEED_LEVELS) {
        user_settings.led_speed = LED_SPEED_DEFAULT;
    }
    if (user_settings.ul_effect >= AULA_FX_COLOR_MODES) {
        user_settings.ul_effect = AULA_FX_COLOR_WHEEL;
    }
    if (user_settings.ul_brightness > 1) {
        user_settings.ul_brightness = 0;
    }
    if (user_settings.battery_indicator_on > 1) {
        user_settings.battery_indicator_on = 1;
    }
    if (user_settings.led_color > 1) {
        user_settings.led_color = 1;
    }
    if (user_settings.ul_speed > 1) {
        user_settings.ul_speed = 1;
    }
}

void indicators_init(void)
{
    led_col   = 0;
    led_phase = 0;
    fx_phase192 = 0;
    regen_row = 0;
    regen_col = 0;
#ifdef MATRIX_INTERLEAVED_SCAN
    display_enabled     = 0;
    regen_steps_pending = (uint8_t)(LED_ROWS * LED_COLS);
    react_col           = 0;
#endif

    fx_reset();
    aula_rgb_clear();
}

void indicators_start(void)
{
    indicators_validate_settings();
#ifdef MATRIX_INTERLEAVED_SCAN
    user_matrix_sinks_off();
    user_matrix_cols_deselect_all();
    user_matrix_scan_pre();
    indicators_render();
#endif
}

/* --------------------------------------------------------- keyboard actions */

void indicators_toggle_direction(void)
{
    user_settings.ul_brightness = user_settings.ul_brightness ? 0 : 1;
    settings_mark_dirty();
}

void indicators_next_color(void)
{
    if (++user_settings.ul_effect >= AULA_FX_COLOR_MODES) {
        user_settings.ul_effect = 0;
    }
    settings_mark_dirty();
}

void indicators_next_effect(void)
{
#ifdef SIGNALRGB_ENABLE
    direct_active = false;
#endif
    if (++user_settings.led_effect > AULA_FX_OFF) {
        user_settings.led_effect = 0;
    }
    aula_rgb_clear(); /* the previous effect would leave its pixels behind */
    fx_reset();
    settings_mark_dirty();
}

void indicators_prev_effect(void)
{
#ifdef SIGNALRGB_ENABLE
    direct_active = false;
#endif
    if (user_settings.led_effect == 0) {
        user_settings.led_effect = AULA_FX_OFF;
    } else {
        user_settings.led_effect--;
    }
    aula_rgb_clear();
    fx_reset();
    settings_mark_dirty();
}

#ifdef SIGNALRGB_ENABLE
void indicators_direct_set(uint8_t start_idx, uint8_t count, const __xdata uint8_t *rgb)
{
    uint8_t i;

    for (i = 0u; i < count; i++) {
        const uint8_t idx = (uint8_t)(start_idx + i);
        if (idx >= (uint8_t)(LED_ROWS * LED_COLS)) {
            break;
        }
        const uint8_t col = (uint8_t)(idx / LED_ROWS);
        const uint8_t row = (uint8_t)(idx % LED_ROWS);
        direct_rgb[row][col][0] = rgb[0];
        direct_rgb[row][col][1] = rgb[1];
        direct_rgb[row][col][2] = rgb[2];
        rgb += 3;
    }
    __critical
    {
        direct_timeout = DIRECT_RGB_TIMEOUT_FRAMES;
        direct_active  = true;
    }
}

void indicators_direct_clear(void)
{
    __critical
    {
        direct_active  = false;
        direct_timeout = 0u;
    }
    aula_rgb_clear();
}
#endif

void indicators_brightness_up(void)
{
    if (user_settings.led_brightness + 1u >= LED_BRIGHTNESS_LEVELS) {
        return;
    }
    user_settings.led_brightness++;
    settings_mark_dirty();
}

void indicators_brightness_down(void)
{
    if (user_settings.led_brightness == 0) {
        return;
    }
    user_settings.led_brightness--;
    settings_mark_dirty();
}

void indicators_speed_up(void)
{
    if (user_settings.led_speed + 1u >= LED_SPEED_LEVELS) {
        return;
    }
    user_settings.led_speed++;
    settings_mark_dirty();
}

void indicators_speed_down(void)
{
    if (user_settings.led_speed == 0) {
        return;
    }
    user_settings.led_speed--;
    settings_mark_dirty();
}
