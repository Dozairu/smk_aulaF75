#include "aula_rgb.h"
#include "sh68f90.h"
#include "pwm.h"

/*
 * Channel load order -- extracted from fcn @ 0x6E61 in the stock firmware and
 * cross-checked against the SH68F90 CV2.0 datasheet register map
 * (XDATA table 0xFF80-0xFFFF).
 *
 * The stock firmware reads 36 consecutive bytes from the framebuffer (XRAM base
 * 0x05A2 + col * 36) and writes them in this exact order, alternating between
 * the two DPTRs via INSCON. Each channel receives DUTY2 in BIG-ENDIAN order:
 * high byte first.
 *
 *   bytes  0-11  ->  PWM20..PWM25   (P1.0..P1.5)
 *   bytes 12-23  ->  PWM10..PWM15   (P2.0..P2.5)
 *   bytes 24-29  ->  PWM03,04,05    (P3.3..P3.5)
 *   bytes 30-35  ->  PWM00,01,02    (P3.0..P3.2)
 *
 * Note the ROTATION within the P3 group: PWM03-05 are loaded BEFORE PWM00-02.
 * That is the only irregularity in the table, and it is intentional in the
 * stock firmware.
 *
 * CHANNEL <-> (ROW, COLOR) MAPPING -- established.
 *
 * The OpenRGB driver for this keyboard (SinowealthKeyboard10cController::SetLEDsDirect)
 * sends colors linearly, 3 bytes per LED, at offset 0x08 + i*3, where i is the
 * LED index in the layout table, i.e. col * 6 + row. Per column, the host
 * therefore sends 18 bytes in the order row0 R,G,B then row1 R,G,B, etc. --
 * hence:
 *
 *     channel = row * 3 + color
 *
 * Combined with the load order extracted above:
 *
 *     rows 0-1  ->  P1.0 .. P1.5   (PWM20..PWM25)
 *     rows 2-3  ->  P2.0 .. P2.5   (PWM10..PWM15)
 *     row  4    ->  P3.3 .. P3.5   (PWM03..PWM05)
 *     row  5    ->  P3.0 .. P3.2   (PWM00..PWM02)
 *
 * Two rows per port, six pins each: the regularity of the mapping is compelling
 * in itself, and explains the apparent rotation in the P3 group.
 *
 * THIS IS NO LONGER AN INFERENCE. The stock converter at 0x765B reads the 8-bit
 * framebuffer at `0x0152 + col * 18 + row * 3 + color` and writes the 16-bit
 * buffer at `0x05A2 + col * 36 + row * 6 + color * 2`: the EXACT SAME
 * `(row, color)` indexing on both sides, with no permutation. Furthermore, the
 * phase table at `CODE 0x2922`, also indexed by `row * 3 + color`, appears
 * byte-for-byte in the DUTY1 registers set at init -- permuted by the exact
 * load order above. The mapping is read directly from the binary, not guessed;
 * the previous note calling for hardware verification is no longer needed.
 */

/*
 * The cache holds ALREADY converted duty cycles, not raw intensities.
 *
 * Conversion costs a multiplication; doing it here when writing a pixel means
 * six conversions per subframe (one effect cell), versus eighteen if done when
 * loading a column -- and column loading runs inside the ISR.
 */
static AULA_RGB_XDATA uint16_t duty_cache[AULA_RGB_COLS][AULA_RGB_CHANNELS];

/*
 * 8-bit -> 16-bit conversion, transcribed from the stock converter at 0x76C3:
 *
 *     DUTY2 = (value << 2) + channel phase
 *
 * The PWM pulse spans from DUTY1 to DUTY2 and DUTY1 stays fixed at the
 * channel's phase (see `aula_rgb.h`), so the active pulse width is exactly
 * `value << 2` and the phase simply shifts the leading edge within the period.
 *
 * DELIBERATE DEPARTURE FROM STOCK -- clamping. At value 255, the sum reaches
 * 1020 + phase, up to 1217 for the highest phase (0xC5), which is 17 ticks
 * BEYOND the 1200-tick period. The stock firmware does not clamp it; we do,
 * because `led_effect_rgb()` genuinely outputs 255 and a DUTY2 exceeding the
 * period has undefined behavior according to the datasheet. Clamping only kicks
 * in starting at value 251 and only on the three channels of row 5.
 */
uint16_t aula_rgb_duty(uint8_t channel, uint8_t value)
{
    const uint16_t duty = (uint16_t)(((uint16_t)value << 2) + AULA_RGB_PHASE(channel));

    return (duty > AULA_RGB_PERIOD) ? AULA_RGB_PERIOD : duty;
}


/* -------------------------------------------------------------- stock wheel */

/*
 * Stock firmware hue wheel, CODE 0x2B2A: 192 triplets, 576 bytes of flash
 * copied verbatim from the dump.
 *
 * Worth every byte: its ramps are NOT linear (1, 3, 5, 7, 10, 14, 18, 22, 26,
 * 32, 38, 44...), forming a perceptual curve. The wheel in
 * `src/smk/led_effect.c` uses linear interpolation across three sectors and
 * does not produce the same colors. Using this table preserves the look of the
 * original firmware.
 *
 * BYTE ORDER -- the reverse-engineering notes leave this open, noting that five
 * engines read the triplet as (R,G,B) and only one, `0x7C12`, reads it as
 * (B,G,R), concluding that "the dump alone cannot settle it".
 *
 * The raw bytes cannot settle it either, and we can now explain WHY: a hue
 * wheel is SYMMETRIC under channel swapping. Read as (R,G,B), the table walks
 * red -> yellow -> green -> cyan -> blue -> magenta; read as (B,G,R), it walks
 * the exact same circle in the opposite direction. Both are valid color wheels,
 * and nothing in the data structure distinguishes them.
 *
 * What remains is the practical consequence: the question is not "which colors"
 * but "which way the rainbow spins". We use (R,G,B), matching five of the six
 * stock engines; getting it wrong only flips the rotation direction.
 */
static const __code uint8_t wheel[AULA_RGB_WHEEL_SIZE][3] = {
    {255,  1,  0}, {255,  3,  0}, {255,  5,  0}, {255,  7,  0},
    {255, 10,  0}, {255, 14,  0}, {255, 18,  0}, {255, 22,  0},
    {255, 26,  0}, {255, 32,  0}, {255, 38,  0}, {255, 44,  0},
    {255, 50,  0}, {255, 57,  0}, {255, 65,  0}, {255, 73,  0},
    {255, 81,  0}, {255, 89,  0}, {255, 99,  0}, {255,109,  0},
    {255,119,  0}, {255,129,  0}, {255,140,  0}, {255,152,  0},
    {255,164,  0}, {255,176,  0}, {255,188,  0}, {255,200,  0},
    {255,213,  0}, {255,227,  0}, {255,241,  0}, {255,255,  0},
    {248,255,  0}, {234,255,  0}, {220,255,  0}, {206,255,  0},
    {194,255,  0}, {182,255,  0}, {170,255,  0}, {158,255,  0},
    {146,255,  0}, {134,255,  0}, {124,255,  0}, {114,255,  0},
    {104,255,  0}, { 94,255,  0}, { 85,255,  0}, { 77,255,  0},
    { 69,255,  0}, { 61,255,  0}, { 53,255,  0}, { 47,255,  0},
    { 41,255,  0}, { 35,255,  0}, { 29,255,  0}, { 24,255,  0},
    { 20,255,  0}, { 16,255,  0}, { 12,255,  0}, {  8,255,  0},
    {  6,255,  0}, {  4,255,  0}, {  2,255,  0}, {  0,255,  0},
    {  0,255,  1}, {  0,255,  3}, {  0,255,  5}, {  0,255,  7},
    {  0,255, 10}, {  0,255, 14}, {  0,255, 18}, {  0,255, 22},
    {  0,255, 26}, {  0,255, 32}, {  0,255, 38}, {  0,255, 44},
    {  0,255, 50}, {  0,255, 57}, {  0,255, 65}, {  0,255, 73},
    {  0,255, 81}, {  0,255, 89}, {  0,255, 99}, {  0,255,109},
    {  0,255,119}, {  0,255,129}, {  0,255,140}, {  0,255,152},
    {  0,255,164}, {  0,255,176}, {  0,255,188}, {  0,255,200},
    {  0,255,213}, {  0,255,227}, {  0,255,241}, {  0,255,255},
    {  0,248,255}, {  0,234,255}, {  0,220,255}, {  0,206,255},
    {  0,194,255}, {  0,182,255}, {  0,170,255}, {  0,158,255},
    {  0,146,255}, {  0,134,255}, {  0,124,255}, {  0,114,255},
    {  0,104,255}, {  0, 94,255}, {  0, 85,255}, {  0, 77,255},
    {  0, 69,255}, {  0, 61,255}, {  0, 53,255}, {  0, 47,255},
    {  0, 41,255}, {  0, 35,255}, {  0, 29,255}, {  0, 24,255},
    {  0, 20,255}, {  0, 16,255}, {  0, 12,255}, {  0,  8,255},
    {  0,  6,255}, {  0,  4,255}, {  0,  2,255}, {  0,  0,255},
    {  1,  0,255}, {  3,  0,255}, {  5,  0,255}, {  7,  0,255},
    { 10,  0,255}, { 14,  0,255}, { 18,  0,255}, { 22,  0,255},
    { 26,  0,255}, { 32,  0,255}, { 38,  0,255}, { 44,  0,255},
    { 50,  0,255}, { 57,  0,255}, { 65,  0,255}, { 73,  0,255},
    { 81,  0,255}, { 89,  0,255}, { 99,  0,255}, {109,  0,255},
    {119,  0,255}, {129,  0,255}, {140,  0,255}, {152,  0,255},
    {164,  0,255}, {176,  0,255}, {188,  0,255}, {200,  0,255},
    {213,  0,255}, {227,  0,255}, {241,  0,255}, {255,  0,255},
    {255,  0,248}, {255,  0,234}, {255,  0,220}, {255,  0,206},
    {255,  0,194}, {255,  0,182}, {255,  0,170}, {255,  0,158},
    {255,  0,146}, {255,  0,134}, {255,  0,124}, {255,  0,114},
    {255,  0,104}, {255,  0, 94}, {255,  0, 85}, {255,  0, 77},
    {255,  0, 69}, {255,  0, 61}, {255,  0, 53}, {255,  0, 47},
    {255,  0, 41}, {255,  0, 35}, {255,  0, 29}, {255,  0, 24},
    {255,  0, 20}, {255,  0, 16}, {255,  0, 12}, {255,  0,  8},
    {255,  0,  6}, {255,  0,  4}, {255,  0,  2}, {255,  0,  0},
};

void aula_rgb_wheel(uint8_t index, uint8_t out[3])
{
    if (index >= AULA_RGB_WHEEL_SIZE) {
        index = (uint8_t)(index % AULA_RGB_WHEEL_SIZE);
    }
    out[0] = wheel[index][0];
    out[1] = wheel[index][1];
    out[2] = wheel[index][2];
}

void aula_rgb_clear(void)
{
    uint8_t col;
    uint8_t ch;
    for (col = 0; col < AULA_RGB_COLS; col++) {
        for (ch = 0; ch < AULA_RGB_CHANNELS; ch++) {
            /* off = zero-width pulse = DUTY2 set equal to the channel phase */
            duty_cache[col][ch] = AULA_RGB_PHASE(ch);
        }
    }
}

void aula_rgb_set(uint8_t row, uint8_t col, uint8_t red, uint8_t green, uint8_t blue)
{
    if (row >= AULA_RGB_ROWS || col >= AULA_RGB_COLS) {
        return;
    }
    const uint8_t base = (uint8_t)(row * AULA_RGB_COLORS);

    duty_cache[col][base + 0] = aula_rgb_duty((uint8_t)(base + 0), red);
    duty_cache[col][base + 1] = aula_rgb_duty((uint8_t)(base + 1), green);
    duty_cache[col][base + 2] = aula_rgb_duty((uint8_t)(base + 2), blue);
}

/*
 * All eighteen writes are unrolled with `SET_PWM_DUTY_2` rather than driven by
 * an address lookup table. An earlier version kept two
 * `static const __xdata uint16_t[18]` tables -- 72 bytes of XRAM plus their
 * flash initializers -- only to perform writes through a generic pointer. The
 * macros in `pwm.h` compile down to direct writes, while preserving the exact
 * stock firmware load order -- P3 group rotation included.
 */
void aula_rgb_load_column(uint8_t col)
{
    if (col >= AULA_RGB_COLS) {
        return;
    }

    const AULA_RGB_XDATA uint16_t *const d = duty_cache[col];

    SET_PWM_DUTY_2(PWM20, d[0]);  /* row 0 R */
    SET_PWM_DUTY_2(PWM21, d[1]);  /* row 0 G */
    SET_PWM_DUTY_2(PWM22, d[2]);  /* row 0 B */
    SET_PWM_DUTY_2(PWM23, d[3]);  /* row 1 R */
    SET_PWM_DUTY_2(PWM24, d[4]);
    SET_PWM_DUTY_2(PWM25, d[5]);
    SET_PWM_DUTY_2(PWM10, d[6]);  /* row 2 R */
    SET_PWM_DUTY_2(PWM11, d[7]);
    SET_PWM_DUTY_2(PWM12, d[8]);
    SET_PWM_DUTY_2(PWM13, d[9]);  /* row 3 R */
    SET_PWM_DUTY_2(PWM14, d[10]);
    SET_PWM_DUTY_2(PWM15, d[11]);
    SET_PWM_DUTY_2(PWM03, d[12]); /* row 4 R -- P3 group is loaded in the */
    SET_PWM_DUTY_2(PWM04, d[13]); /* order 03,04,05 THEN 00,01,02, the only */
    SET_PWM_DUTY_2(PWM05, d[14]); /* irregularity in the stock table */
    SET_PWM_DUTY_2(PWM00, d[15]); /* row 5 R */
    SET_PWM_DUTY_2(PWM01, d[16]);
    SET_PWM_DUTY_2(PWM02, d[17]);
}
