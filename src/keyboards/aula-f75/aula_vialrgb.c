/*
 * VialRGB for the AULA F75: Vial GUI's Lighting tab driving the Aula effect
 * engine (layouts/default/indicators.c).
 *
 * Vial only knows QMK's RGB matrix effects, by fixed id and name. Every Aula
 * effect is offered TWICE, under two QMK ids: once in a single color (the
 * color picker; color mode AULA_FX_COLOR_CUSTOM, or a stock fixed color when
 * the picker still shows exactly that one) and once in rainbow (color mode
 * AULA_FX_COLOR_WHEEL). The names are the closest QMK ones; see the table.
 *
 * Brightness: V 0-255 <-> the engine's 10 levels. Speed: 0-255 <-> 5 levels.
 * Hue / saturation of the picker live in the Vial area of the dynamic store
 * (the shared settings struct has no spare byte); the rest in user_settings,
 * as before -- the Fn lighting keys and the VIA custom menu keep working, and
 * Vial reads their changes back.
 *
 * Direct mode (per-LED HSV from the host) is not offered: its frame buffer
 * does not fit in the XRAM that is left.
 */
#ifdef VIALRGB_ENABLE

#    include "vialrgb.h"
#    include "aula_fx.h"
#    include "aula_rgb.h"
#    include "dynamic_keymap.h"
#    include "indicators.h"
#    include "settings.h"
#    include "tick.h"
#    include <string.h>

/* Aula effect numbers (indicators.c, AULA_FX_* / led_effect_t). */
#    define FX_RADIAL   0u
#    define FX_HWAVE    1u
#    define FX_VWAVE    2u
#    define FX_SOLID_   3u
#    define FX_REACTIVE 4u
#    define FX_RAIN     5u
#    define FX_TWINKLE  6u
#    define FX_SNAKE    7u
#    define FX_RIPPLE   8u
#    define FX_LAKE     9u
#    define FX_KEYWAVE  10u
#    define FX_VRAINBOW 11u
#    define FX_GAMING   12u
#    define FX_SHIMMER  13u
#    define FX_OFF_     14u

#    define RAINBOW 0x80u /* flag in the table: the rainbow variant */

/* VialRGB id (vial-qmk vialrgb_effects.inc) -> Aula effect [| RAINBOW].
 * Ascending by id: "supported" pages through it in order. */
static const __code uint8_t effects[][2] = {
    {0, FX_OFF_},                /* OFF                                   */
    {2, FX_SOLID_},              /* SOLID_COLOR                           */
    {3, FX_GAMING},              /* ALPHAS_MODS          Gaming (WASD)    */
    {4, FX_VWAVE},               /* GRADIENT_UP_DOWN     Vertical Wave    */
    {5, FX_HWAVE},               /* GRADIENT_LEFT_RIGHT  Horizontal Wave  */
    {6, FX_SHIMMER},             /* BREATHING            Shimmer          */
    {7, FX_VRAINBOW},            /* BAND_SAT             Vertical Rainbow */
    {10, FX_KEYWAVE},            /* BAND_PINWHEEL_VAL    Key Wave         */
    {12, FX_RADIAL},             /* BAND_SPIRAL_VAL      Radial           */
    {13, FX_SOLID_ | RAINBOW},   /* CYCLE_ALL            Solid            */
    {14, FX_HWAVE | RAINBOW},    /* CYCLE_LEFT_RIGHT     Horizontal Wave  */
    {15, FX_VWAVE | RAINBOW},    /* CYCLE_UP_DOWN        Vertical Wave    */
    {16, FX_VRAINBOW | RAINBOW}, /* RAINBOW_MOVING_CHEVRON Vert. Rainbow  */
    {17, FX_RADIAL | RAINBOW},   /* CYCLE_OUT_IN         Radial           */
    {22, FX_GAMING | RAINBOW},   /* RAINBOW_BEACON       Gaming (WASD)    */
    {23, FX_SNAKE | RAINBOW},    /* RAINBOW_PINWHEELS    Snake            */
    {24, FX_RAIN | RAINBOW},     /* RAINDROPS            Rain             */
    {25, FX_TWINKLE | RAINBOW},  /* JELLYBEAN_RAINDROPS  Twinkle          */
    {26, FX_SHIMMER | RAINBOW},  /* HUE_BREATHING        Shimmer          */
    {28, FX_KEYWAVE | RAINBOW},  /* HUE_WAVE             Key Wave         */
    {30, FX_RAIN},               /* DIGITAL_RAIN         Rain             */
    {31, FX_REACTIVE},           /* SOLID_REACTIVE_SIMPLE Reactive        */
    {32, FX_REACTIVE | RAINBOW}, /* SOLID_REACTIVE       Reactive         */
    {39, FX_RIPPLE | RAINBOW},   /* SPLASH               Ripple           */
    {40, FX_LAKE | RAINBOW},     /* MULTISPLASH          Lake             */
    {41, FX_RIPPLE},             /* SOLID_SPLASH         Ripple           */
    {42, FX_LAKE},               /* SOLID_MULTISPLASH    Lake             */
    {43, FX_TWINKLE},            /* PIXEL_RAIN           Twinkle          */
    {44, FX_SNAKE},              /* PIXEL_FRACTAL        Snake            */
};
#    define EFFECTS ((uint8_t)(sizeof(effects) / sizeof(effects[0])))

/* Hue / saturation the picker shows for the stock fixed colors 0-6. */
static const __code uint8_t fixed_hs[7][2] = {
    {0, 255}, {85, 255}, {170, 255}, {43, 255}, {213, 255}, {128, 255}, {0, 0},
};

#    define SPEED_LEVELS      5u
#    define BRIGHTNESS_LEVELS 10u

static __xdata uint8_t *hs(void)
{
    return dynamic_vial_store() + VIAL_RGB_HUE; /* [0] hue, [1] saturation */
}

/*
 * Picker color -> RGB the factory way: the hue indexes the factory color
 * wheel (aula_rgb_wheel, the one the rainbow effects use; red -> yellow ->
 * green -> cyan -> blue -> magenta from 0, like QMK's hue, so 0-255 maps to
 * 0-191 directly), and saturation blends it toward plain white (255, 255,
 * 255), which is what the stock White color is. No gamma or white balance on
 * top, same as the stock colors.
 */
static void hs_to_rgb(uint8_t h, uint8_t s, __xdata uint8_t *rgb)
{
    uint8_t       c[3];
    const uint8_t w = (uint8_t)(255u - s);
    aula_rgb_wheel((uint8_t)(((uint16_t)h * 3u) >> 2), c);
    for (uint8_t i = 0; i < 3u; i++) {
        rgb[i] = (uint8_t)(c[i] + (uint8_t)(((uint16_t)(uint8_t)(255u - c[i]) * w) / 255u));
    }
}

void vialrgb_apply(void)
{
    hs_to_rgb(hs()[0], hs()[1], aula_fx_custom);
}

void vialrgb_step(uint8_t which, int8_t delta)
{
    __xdata uint8_t *p = hs() + which;
    int16_t          v = (int16_t)*p + delta;
    if (which) { /* saturation stops at the ends; hue wraps around */
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    *p                      = (uint8_t)v;
    user_settings.ul_effect = AULA_FX_COLOR_CUSTOM;
    vialrgb_apply();
    dynamic_vial_mark_dirty();
    settings_mark_dirty();
}

static uint8_t current_id(void)
{
    const uint8_t fx = user_settings.led_effect;
    const uint8_t rb = (fx != FX_OFF_ && user_settings.ul_effect == AULA_FX_COLOR_WHEEL) ? RAINBOW : 0u;
    for (uint8_t i = 0; i < EFFECTS; i++) {
        if (effects[i][1] == (uint8_t)(fx | rb)) {
            return effects[i][0];
        }
    }
    return 0;
}

void vialrgb_get_value(__xdata uint8_t *data)
{
    __xdata uint8_t *args = data + 2;
    switch (data[1]) {
        case VIALRGB_GET_INFO:
            args[0] = (uint8_t)VIALRGB_PROTOCOL_VERSION;
            args[1] = 0;
            args[2] = 255; /* maximum brightness (V) */
            break;

        case VIALRGB_GET_MODE: {
            const uint8_t mode = user_settings.ul_effect;
            args[0]            = current_id();
            args[1]            = 0;
            args[2]            = (uint8_t)(((uint16_t)user_settings.led_speed * 255u) / (SPEED_LEVELS - 1u));
            if (mode < AULA_FX_COLOR_WHEEL) {
                args[3] = fixed_hs[mode][0];
                args[4] = fixed_hs[mode][1];
            } else {
                args[3] = hs()[0];
                args[4] = hs()[1];
            }
            args[5] = (uint8_t)(((uint16_t)user_settings.led_brightness * 255u) / (BRIGHTNESS_LEVELS - 1u));
            break;
        }

        case VIALRGB_GET_SUPPORTED: {
            /* Ids greater than the u16 LE in args[0..1], 0xFFFF-filled. */
            const uint16_t gt = (uint16_t)args[0] | (uint16_t)((uint16_t)args[1] << 8);
            uint8_t        o  = 0;
            memset(args, 0xFF, 30);
            for (uint8_t i = 0; i < EFFECTS && o < 30u; i++) {
                if (effects[i][0] > gt) {
                    args[o++] = effects[i][0];
                    args[o++] = 0;
                }
            }
            break;
        }

        case VIALRGB_GET_NUMBER_LEDS:
            args[0] = 0; /* no direct mode */
            args[1] = 0;
            break;

        default:
            break;
    }
}

void vialrgb_set_value(__xdata uint8_t *data)
{
    const __xdata uint8_t *args = data + 2;
    if (data[1] != VIALRGB_SET_MODE) {
        return; /* direct fastset: not supported */
    }

    const uint16_t id = (uint16_t)args[0] | (uint16_t)((uint16_t)args[1] << 8);
    const uint8_t  h  = args[3];
    const uint8_t  s  = args[4];
    uint8_t        fx = 0xFFu;
    for (uint8_t i = 0; i < EFFECTS; i++) {
        if (effects[i][0] == id) {
            fx = effects[i][1];
        }
    }

    /* Color: the picker is always stored; the mode follows the variant. */
    hs()[0] = h;
    hs()[1] = s;
    hs_to_rgb(h, s, aula_fx_custom);
    dynamic_vial_mark_dirty();
    if (fx != 0xFFu && (fx & 0x7Fu) != FX_OFF_) {
        const uint8_t mode = user_settings.ul_effect;
        if (fx & RAINBOW) {
            user_settings.ul_effect = AULA_FX_COLOR_WHEEL;
        } else if (!(mode < AULA_FX_COLOR_WHEEL && fixed_hs[mode][0] == h && fixed_hs[mode][1] == s)) {
            /* Keep a stock color the picker still shows unchanged (e.g. only
             * the brightness slider moved); anything else is a custom color. */
            user_settings.ul_effect = AULA_FX_COLOR_CUSTOM;
        }
    }

    user_settings.led_speed      = (uint8_t)(((uint16_t)args[2] * SPEED_LEVELS) >> 8);
    user_settings.led_brightness = (uint8_t)(((uint16_t)args[5] * (BRIGHTNESS_LEVELS - 1u) + 127u) / 255u);

    if (fx != 0xFFu && (uint8_t)(fx & 0x7Fu) != user_settings.led_effect) {
        user_settings.led_effect = (uint8_t)(fx & 0x7Fu);
        indicators_validate_settings();
        tick_pause();
        indicators_init(); /* new effect: fresh state, as the VIA menu does */
        tick_resume();
    } else {
        indicators_validate_settings();
    }
    settings_mark_dirty();
}

void vialrgb_save(void)
{
    settings_mark_dirty();
    dynamic_vial_mark_dirty();
}

#else
typedef int aula_vialrgb_unused; /* ISO C: no empty translation unit */
#endif
