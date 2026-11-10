#include "aula_fx.h"

/*
 * Stock secondary palette, CODE 0x2D6D. Copied byte-for-byte from the dump.
 *
 * It occupies the 384 bytes between the end of the 192-hue color wheel
 * (0x2B2A + 576 = 0x2D6A) and the start of the ring map -- two contiguous
 * tables, which helped pin down their boundaries.
 */
static const __code uint8_t palette[AULA_FX_PALETTE_SIZE][3] = {
    {  0,  5,255}, {  0, 17,255}, {  0, 29,255}, {  0, 48,255},
    {  0, 65,255}, {  0, 85,255}, {  0,105,255}, {  0,127,255},
    {  0,137,255}, {  0,148,255}, {  0,161,255}, {  0,171,255},
    {  0,182,255}, {  0,192,255}, {  0,202,255}, {  0,211,255},
    {  0,220,255}, {  0,229,255}, {  0,237,255}, {  0,243,255},
    {  0,250,255}, {  0,255,255}, {  0,255,250}, {  0,255,246},
    {  0,255,238}, {  0,255,231}, {  0,255,223}, {  0,255,214},
    {  0,255,205}, {  0,255,195}, {  0,255,186}, {  0,255,175},
    {  0,255,165}, {  0,255,153}, {  0,255,143}, {  0,255,130},
    {  0,255,114}, {  0,255, 93}, {  0,255, 73}, {  0,255, 54},
    {  0,255, 37}, {  0,255, 21}, {  0,255,  8}, {  0,255,  0},
    { 11,255,  0}, { 22,255,  0}, { 36,255,  0}, { 52,255,  0},
    { 70,255,  0}, { 87,255,  0}, {106,255,  0}, {126,255,  0},
    {136,255,  0}, {144,255,  0}, {152,255,  0}, {159,255,  0},
    {168,255,  0}, {176,255,  0}, {183,255,  0}, {192,255,  0},
    {198,255,  0}, {206,255,  0}, {212,255,  0}, {220,255,  0},
    {225,255,  0}, {231,255,  0}, {237,255,  0}, {243,255,  0},
    {247,255,  0}, {251,255,  0}, {255,255,  0}, {255,249,  0},
    {255,242,  0}, {255,236,  0}, {255,225,  0}, {255,215,  0},
    {255,206,  0}, {255,195,  0}, {255,183,  0}, {255,170,  0},
    {255,159,  0}, {255,145,  0}, {255,132,  0}, {255,105,  0},
    {255, 68,  0}, {255, 34,  0}, {255, 10,  0}, {255,  0,  5},
    {255,  0, 17}, {255,  0, 30}, {255,  0, 50}, {255,  0, 69},
    {255,  0, 91}, {255,  0,113}, {255,  0,131}, {255,  0,142},
    {255,  0,155}, {255,  0,165}, {255,  0,177}, {255,  0,189},
    {255,  0,199}, {255,  0,209}, {255,  0,219}, {255,  0,229},
    {255,  0,236}, {255,  0,243}, {255,  0,250}, {255,  0,255},
    {250,  0,255}, {243,  0,255}, {237,  0,255}, {228,  0,255},
    {220,  0,255}, {209,  0,255}, {200,  0,255}, {189,  0,255},
    {178,  0,255}, {168,  0,255}, {158,  0,255}, {143,  0,255},
    {131,  0,255}, {111,  0,255}, { 90,  0,255}, { 69,  0,255},
    { 50,  0,255}, { 31,  0,255}, { 16,  0,255}, {  5,  0,255},
};

/*
 * The seven fixed colors, CODE 0xC800.
 *
 * The stock firmware stores twenty-one bytes per effect -- seven triplets --
 * and all EIGHTEEN effects carry the exact same twenty-one bytes. A single
 * table of seven therefore suffices; the stock firmware's per-effect indexing
 * adds nothing that it actually uses.
 *
 * These are the canonical seven: the three primaries, their three pairwise
 * mixes, then white. Mode 7 does not read this table -- it is rainbow mode,
 * which pulls from the wheel in `aula_rgb_wheel()`.
 */
static const __code uint8_t colors[AULA_FX_COLOR_MODES - 1][3] = {
    {255,   0,   0}, /* red     */
    {  0, 255,   0}, /* green   */
    {  0,   0, 255}, /* blue    */
    {255, 255,   0}, /* yellow  */
    {255,   0, 255}, /* magenta */
    {  0, 255, 255}, /* cyan    */
    {255, 255, 255}, /* white   */
};

void aula_fx_color(uint8_t mode, uint8_t out[3])
{
    const uint8_t i = (mode < (uint8_t)(AULA_FX_COLOR_MODES - 1)) ? mode : 0;

    out[0] = colors[i][0];
    out[1] = colors[i][1];
    out[2] = colors[i][2];
}

void aula_fx_palette(uint8_t index, uint8_t out[3])
{
    const uint8_t i = (uint8_t)(index & (uint8_t)(AULA_FX_PALETTE_SIZE - 1));

    out[0] = palette[i][0];
    out[1] = palette[i][1];
    out[2] = palette[i][2];
}

/*
 * Concentric wave rings, CODE 0x2959.
 *
 * These are NOT geometric distances: ring 8 contains (2,0) and (0,0), which
 * any metric would place much closer to the center. This is a hand-authored
 * propagation order that follows the physical key layout -- all the more
 * reason to copy it verbatim rather than recompute it.
 *
 * A few IDs refer to columns 15 and 16, which do not exist on this keyboard:
 * the stock framebuffer is 21 columns wide and this table is clearly shared
 * with a larger model. The caller filters them out.
 */
static const __code uint8_t rings[AULA_FX_RINGS][AULA_FX_RING_SLOTS] = {
    {0x3a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    {0x39, 0x41, 0x32, 0x42, 0x33, 0x3b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    {0x40, 0x48, 0x31, 0x2a, 0x2b, 0x34, 0x3c, 0x44, 0x43, 0x4a, 0x49, 0xff, 0xff},
    {0x38, 0x29, 0x22, 0x23, 0x2c, 0x2d, 0x50, 0x51, 0x52, 0x4b, 0x4c, 0xff, 0xff},
    {0x30, 0x21, 0x1a, 0x1b, 0x24, 0x58, 0x59, 0x5a, 0x53, 0x54, 0x45, 0xff, 0xff},
    {0x28, 0x19, 0x12, 0x13, 0x1c, 0x60, 0x61, 0x62, 0x5b, 0x5c, 0x4d, 0x63, 0x55},
    {0x20, 0x11, 0x0a, 0x0b, 0x14, 0x15, 0x68, 0x71, 0x69, 0x6a, 0x6b, 0x64, 0x6c},
    {0x18, 0x09, 0x02, 0x03, 0x0c, 0x0d, 0x80, 0x72, 0x73, 0x81, 0x6d, 0x74, 0x65},
    {0x10, 0x00, 0x01, 0x05, 0x04, 0x82, 0x85, 0x7d, 0x7a, 0xff, 0x75, 0x7b, 0x7c},
};

uint8_t aula_fx_ring(uint8_t ring, uint8_t slot)
{
    if (ring >= AULA_FX_RINGS || slot >= AULA_FX_RING_SLOTS) {
        return 0xff;
    }
    return rings[ring][slot];
}

/*
 * Presence map, derived from CODE 0xC500: a row bit is 1 when the grid position
 * carries a key. Six slots out of ninety have none.
 *
 * The stock table holds the full ID of each key; we only care about 0xFF here,
 * hence the reduction to fifteen bytes. Its row 4 permutes the columns,
 * matching row 4 of table 0x2EED -- two independent findings that cross-check
 * on the same row quirk.
 */
static const __code uint8_t present[AULA_FX_COLS] = {
    0x3f, 0x3e, 0x3f, 0x1f, 0x1f, 0x3f, 0x1f, 0x1f, 0x3f, 0x3f, 0x3f, 0x1f, 0x3f, 0x3f, 0x3f,
};

/*
 * THE STOCK KEY MAP -- CODE 0xC500, copied verbatim.
 *
 * Indexed as `col * 6 + row`, it returns the stock key ID, `col * 8 + row`,
 * or `0xFF` when the grid slot has no key. Here it replaces TWO tables that
 * this file previously derived separately -- a presence mask and a spatial
 * column map -- because the dump proves they are one and the same:
 *
 *   - `id == 0xFF` <=> no key. Six positions out of ninety.
 *   - `id >> 3`    == the SPATIAL column: verified equal to the inverse of
 *                     table A (`CODE 0x2EED`) across all fifteen columns.
 *   - `id & 7`     == the row itself across all present entries -- the same
 *                     result as table B (`CODE 0x2F6B`).
 *
 * The eighty-four present IDs are pairwise distinct: it is a bijection over
 * the physical keys.
 *
 * WARNING: THE STOCK TABLE HAS 126 ENTRIES, NOT 90. It describes a TWENTY-ONE-column
 * grid -- the same width as tables A and B -- and goes up to ID `0xA4`,
 * column 20. This keyboard only has fifteen; the remaining six columns belong
 * to a wider model in the same family. That is what the `< 15` check in the
 * stock rendering engine tests, a guard that never triggers here. Only the
 * first fifteen columns are copied.
 *
 * Row 4 permutes: electrical column 12 carries ID `0x0C`, i.e. spatial
 * column 1, and columns 1 through 11 shift by one slot. That is the position
 * of the extra ISO layout key next to Left Shift.
 */
static const __code uint8_t key_id[AULA_FX_COLS][AULA_FX_ROWS] = {
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, /* col  0 */
    {0xff, 0x09, 0x0a, 0x0b, 0x14, 0x0d}, /* col  1 */
    {0x10, 0x11, 0x12, 0x13, 0x1c, 0x15}, /* col  2 */
    {0x18, 0x19, 0x1a, 0x1b, 0x24, 0xff}, /* col  3 */
    {0x20, 0x21, 0x22, 0x23, 0x2c, 0xff}, /* col  4 */
    {0x28, 0x29, 0x2a, 0x2b, 0x34, 0x2d}, /* col  5 */
    {0x30, 0x31, 0x32, 0x33, 0x3c, 0xff}, /* col  6 */
    {0x38, 0x39, 0x3a, 0x3b, 0x44, 0xff}, /* col  7 */
    {0x40, 0x41, 0x42, 0x43, 0x4c, 0x45}, /* col  8 */
    {0x48, 0x49, 0x4a, 0x4b, 0x54, 0x4d}, /* col  9 */
    {0x50, 0x51, 0x52, 0x53, 0x5c, 0x55}, /* col 10 */
    {0x58, 0x59, 0x5a, 0x5b, 0x64, 0xff}, /* col 11 */
    {0x60, 0x61, 0x62, 0x63, 0x0c, 0x65}, /* col 12 */
    {0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d}, /* col 13 */
    {0x70, 0x71, 0x72, 0x73, 0x74, 0x75}, /* col 14 */
};

uint8_t aula_fx_key_id(uint8_t col, uint8_t row)
{
    if (col >= AULA_FX_COLS || row >= AULA_FX_ROWS) {
        return AULA_FX_NO_KEY;
    }
    return key_id[col][row];
}

uint8_t aula_fx_render_col(uint8_t col, uint8_t row)
{
    const uint8_t id = aula_fx_key_id(col, row);

    return (id == AULA_FX_NO_KEY) ? col : (uint8_t)(id >> 3);
}

/*
 * "Gaming" preset mask, reduced from CODE 0xCAFC.
 *
 * WARNING: PROVENANCE CORRECTION. This comment previously said "blue plane, red and
 * green planes are zero, the stock image is blue". That was wrong: `0xCAFC` is
 * not three color planes, but **a single byte per key**, 126 in total, indexed
 * `col * 6 + row`, equal to `0xFF` on lit keys and zero elsewhere. There is NO
 * color in this table -- it is a mask, and the color comes from the color mode
 * just like every other effect.
 *
 * The nine non-zero positions are indices 0, 9, 14, 15, 21, 77, 82, 83, and 89,
 * i.e. (0,0) (1,3) (2,2) (2,3) (3,3) (12,5) (13,4) (13,5) (14,5):
 * Esc, A, W, S, D, and the four arrow keys in layouts/default/layout.c. No
 * coincidence could align nine positions by chance -- that part of the analysis
 * stood, and still stands.
 *
 * The fifteen bytes below are the per-column reduction of those 126 entries,
 * and reconstructing from the dump reproduces them exactly. Columns 15 to 20 of
 * the stock table are all zero.
 */
static const __code uint8_t gaming[AULA_FX_COLS] = {
    0x01, 0x08, 0x0c, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x30, 0x20,
};

uint8_t aula_fx_gaming(uint8_t col)
{
    return (col < AULA_FX_COLS) ? gaming[col] : 0;
}

/*
 * Per-key phase field for effect 15 -- the 90 active entries of the 126 bytes
 * that the C initializer places into XDATA 0x0E49 (source data at CODE 0x9FEA).
 *
 * Reading the Keil init table is self-validating: its three records are 1, 1,
 * and 126 bytes long, and the 0x00 terminator lands right at the end of the
 * third record at 0xA068. No room for an offset error.
 *
 * Stored here as [col][row] -- the stock firmware stores it as [row][col] with
 * a stride of 21, and 0xACA3 transposes it when loading. We transpose it once
 * and for all at compile time.
 *
 * THE SEAM BETWEEN COLUMN 14 AND COLUMN 0 IS EXPECTED, do not "fix" it. The
 * angular sweep wraps around cleanly in the stock firmware's 21-column space
 * thanks to the six columns 15 through 20 (51 51, 4f 4f, 4d 4d ...) that do
 * not exist on this keyboard. Across fifteen columns, the phase jumps from
 * 0x03-0x13 to 0x52-0x44 at the wraparound -- and the stock firmware makes the
 * exact same jump on this hardware.
 */
static const __code uint8_t keywave[AULA_FX_COLS][AULA_FX_ROWS] = {
    {0x52, 0x50, 0x4d, 0x48, 0x45, 0x44}, /* col  0 */
    {0x53, 0x50, 0x4e, 0x47, 0x44, 0x42}, /* col  1 */
    {0x54, 0x51, 0x4e, 0x46, 0x43, 0x40}, /* col  2 */
    {0x55, 0x52, 0x4f, 0x45, 0x42, 0x3e}, /* col  3 */
    {0x57, 0x53, 0x4f, 0x44, 0x40, 0x36}, /* col  4 */
    {0x59, 0x55, 0x50, 0x43, 0x3d, 0x30}, /* col  5 */
    {0x5c, 0x56, 0x52, 0x42, 0x34, 0x29}, /* col  6 */
    {0x62, 0x5f, 0x5a, 0x34, 0x27, 0x1f}, /* col  7 */
    {0x69, 0x6c, 0x7d, 0x17, 0x1b, 0x19}, /* col  8 */
    {0x73, 0x78, 0x06, 0x11, 0x18, 0x17}, /* col  9 */
    {0x78, 0x7f, 0x08, 0x10, 0x16, 0x16}, /* col 10 */
    {0x7c, 0x01, 0x09, 0x0f, 0x13, 0x15}, /* col 11 */
    {0x7f, 0x03, 0x09, 0x0f, 0x13, 0x14}, /* col 12 */
    {0x01, 0x06, 0x0a, 0x0f, 0x12, 0x14}, /* col 13 */
    {0x03, 0x07, 0x0b, 0x0e, 0x12, 0x13}, /* col 14 */
};

/*
 * SHIMMER -- phase fields for stock effect 6, engine `0x60C9`.
 *
 * Its seeder `0xA791` loads the per-key plane from one of TWO tables selected
 * by the color mode `[0x011C]`, and both are already laid out as
 * `col * 6 + row`: no transposition needed, unlike `0x9FEA`.
 *
 *   mode 7, rainbow      ->  CODE 0x2A2E, values 1 to 191
 *   modes 0 to 6, fixed  ->  CODE 0x2AAC, values 0 to  95
 *
 * The ranges match the moduli applied by the engine via `0xEDA2`: `0xC0`
 * = 192 = 191 + 1 for the color wheel, `0x60` = 96 = 95 + 1 for breathing. The
 * seeder picks the table whose range matches the modulus.
 *
 * Both draw a chevron: rows 0 to 2 form one ramp, rows 3 to 5 form the mirror
 * ramp. Only the fifteen columns of this keyboard are copied.
 */
static const __code uint8_t shimmer_wheel[AULA_FX_COLS][AULA_FX_ROWS] = {
    {  1,   1,   1, 191, 191, 191}, /* col  0 */
    {  6,   6,   6, 190, 190, 190}, /* col  1 */
    { 11,  11,  11, 188, 188, 188}, /* col  2 */
    { 16,  16,  16, 185, 185, 185}, /* col  3 */
    { 21,  21,  21, 180, 180, 180}, /* col  4 */
    { 26,  26,  26, 175, 175, 175}, /* col  5 */
    { 31,  31,  31, 170, 170, 170}, /* col  6 */
    { 36,  36,  36, 165, 165, 165}, /* col  7 */
    { 41,  41,  41, 160, 160, 160}, /* col  8 */
    { 46,  46,  46, 155, 155, 155}, /* col  9 */
    { 51,  51,  51, 150, 150, 150}, /* col 10 */
    { 56,  56,  56, 145, 145, 145}, /* col 11 */
    { 61,  61,  61, 140, 140, 140}, /* col 12 */
    { 66,  66,  66, 135, 135, 135}, /* col 13 */
    { 71,  71,  71, 130, 130, 130}, /* col 14 */
};

static const __code uint8_t shimmer_fixed[AULA_FX_COLS][AULA_FX_ROWS] = {
    {  0,   0,   0,  84,  84,  84}, /* col  0 */
    {  7,   7,   7,  77,  77,  77}, /* col  1 */
    { 14,  14,  14,  70,  70,  70}, /* col  2 */
    { 21,  21,  21,  63,  63,  63}, /* col  3 */
    { 28,  28,  28,  56,  56,  56}, /* col  4 */
    { 35,  35,  35,  49,  49,  49}, /* col  5 */
    { 42,  42,  42,  42,  42,  42}, /* col  6 */
    { 49,  49,  49,  35,  35,  35}, /* col  7 */
    { 56,  56,  56,  28,  28,  28}, /* col  8 */
    { 63,  63,  63,  21,  21,  21}, /* col  9 */
    { 70,  70,  70,  14,  14,  14}, /* col 10 */
    { 77,  77,  77,   7,   7,   7}, /* col 11 */
    { 84,  84,  84,  95,  95,  95}, /* col 12 */
    { 91,  91,  91,  91,  91,  91}, /* col 13 */
    { 95,  95,  95,  84,  84,  84}, /* col 14 */
};

uint8_t aula_fx_shimmer_phase(uint8_t col, uint8_t row, uint8_t rainbow)
{
    if (col >= AULA_FX_COLS || row >= AULA_FX_ROWS) {
        return 0;
    }
    return rainbow ? shimmer_wheel[col][row] : shimmer_fixed[col][row];
}

/*
 * Breathing ramp, CODE 0x29CE, 96 entries -- read by `0xA177`, which applies
 * `component * ramp[phase] / 255` to the fixed color.
 *
 * It is a symmetric breathing curve: 22 -> 255 over forty-eight steps, then
 * back down. Its floor is 22 rather than zero: in fixed-color mode, a key never
 * turns completely off.
 *
 * It abuts the next table directly: 0x29CE + 96 = 0x2A2E.
 */
static const __code uint8_t breath[AULA_FX_BREATH_SIZE] = {
     22,  24,  27,  30,  33,  36,  39,  42,  45,  49,  53,  57,  61,  65,  69,  73,
     77,  81,  85,  89,  94,  99, 104, 109, 114, 119, 124, 129, 134, 140, 146, 152,
    158, 164, 170, 176, 182, 188, 194, 200, 206, 213, 220, 227, 234, 241, 248, 255,
    255, 248, 241, 234, 227, 220, 213, 206, 200, 194, 188, 182, 176, 170, 164, 158,
    152, 146, 140, 134, 129, 124, 119, 114, 109, 104,  99,  94,  89,  85,  81,  77,
     73,  69,  65,  61,  57,  53,  49,  45,  42,  39,  36,  33,  30,  27,  24,  22,
};

uint8_t aula_fx_breath(uint8_t phase)
{
    return breath[phase % AULA_FX_BREATH_SIZE];
}

uint8_t aula_fx_keywave(uint8_t col, uint8_t row)
{
    if (col >= AULA_FX_COLS || row >= AULA_FX_ROWS) {
        return 0;
    }
    return keywave[col][row];
}

/*
 * Pseudo-random number generator.
 *
 * BEHAVIOR transcribed, GENERATOR substituted, by design. At 0xA997 the stock
 * firmware uses a 32-bit Galois LFSR (right shift, mask 0xCC4C4ECE, state in
 * XRAM 0x0F67, reseeded to 0xA5A5 if it hits zero), sixteen rounds per call,
 * followed by a signed division at 0x4D52 for the modulo. All of that just to
 * pick "a random column" or "a random hue".
 *
 * Reproducing the polynomial wouldn't yield the same sequence anyway -- that
 * would also require the same seed and call order -- and would waste stack and
 * time inside the ISR. An 8-bit xorshift provides the same observable behavior
 * in eight bytes of code. Same trade-off as in the reactive engine, where we
 * store a hue instead of the stock firmware's three RGB components.
 */
static uint8_t rand_state = 0xa5; /* the only piece kept from the stock seed */

uint8_t aula_fx_rand(void)
{
    rand_state ^= (uint8_t)(rand_state << 1);
    rand_state ^= (uint8_t)(rand_state >> 1);
    rand_state ^= (uint8_t)(rand_state << 2);
    return rand_state;
}
