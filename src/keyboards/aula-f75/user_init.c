#include "kbdef.h"
#include "user_init.h"
#include "pwm.h"
#include "user_matrix.h"
#include "aula_rgb.h"
#include "gpio.h"

/*
 * GPIO initialization transcribed from the consolidated init in the stock
 * firmware, fcn @ 0xA7EC. Values recorded verbatim:
 *
 *   P0CR=0x9C  P1CR=0x3F  P2CR=0x3F  P3CR=0x3F
 *   P4CR=0x4D  P5CR=0x87  P6CR=0xFF  P7CR=0x60
 *   P0PCR=0xF8 P1PCR=0x3F P2PCR=0x3F P3PCR=0x3F
 *   P4PCR=0x6F P5PCR=0x9F P6PCR=0xFF P7PCR=0xDF
 *   P0=0x1C
 *
 * CR bit = 1 means output. We apply the stock configuration in bulk rather than
 * reconstructing it pin by pin: several pins still have an unknown role
 * (P4.1, P4.4, P4.6, P4.7, P5.5, P5.6, P7.7), and leaving them in the stock
 * state is safer than guessing.
 */

void user_gpio_init(void)
{
    GPIO_DIR_WRITE(0, 0x9c);
    GPIO_DIR_WRITE(1, 0x3f);
    GPIO_DIR_WRITE(2, 0x3f);
    GPIO_DIR_WRITE(3, 0x3f);
    GPIO_DIR_WRITE(4, 0x4d);
    GPIO_DIR_WRITE(5, 0x87);
    GPIO_DIR_WRITE(6, 0xff);
    GPIO_DIR_WRITE(7, 0x60);

    GPIO_PULLUP_WRITE(0, 0xf8);
    GPIO_PULLUP_WRITE(1, 0x3f);
    GPIO_PULLUP_WRITE(2, 0x3f);
    GPIO_PULLUP_WRITE(3, 0x3f);
    GPIO_PULLUP_WRITE(4, 0x6f);
    GPIO_PULLUP_WRITE(5, 0x9f);
    GPIO_PULLUP_WRITE(6, 0xff);
    GPIO_PULLUP_WRITE(7, 0xdf);

    GPIO_WRITE(0, 0x1c);

    /* Columns deselected (active-low) before the first scan */
    user_matrix_cols_deselect_all();
}

#ifndef BOOT_CHORD_HEADER
/*
 * The 18 backlight channels: three banks of six, shared period and per-channel
 * DUTY1. The period 0x04B0 and the eighteen DUTY1 constants come from the stock
 * firmware, recorded in its routine 0x6707-0x68D3.
 *
 * The banks are NOT enabled here: `indicators_pwm_enable()` sets
 * `PWM_MODE_ENABLE`, and `indicators_pwm_disable()` clears it before each
 * matrix scan. Enabling them here would leave the LEDs driving during the
 * first scan, before the rendering engine has written anything.
 */
void user_pwm_init(void)
{
    const uint8_t perdh = (uint8_t)(AULA_RGB_PERIOD >> 8);
    const uint8_t perdl = (uint8_t)(AULA_RGB_PERIOD);

    PWM0PERDH = perdh;
    PWM0PERDL = perdl;
    PWM1PERDH = perdh;
    PWM1PERDL = perdl;
    PWM2PERDH = perdh;
    PWM2PERDL = perdl;

    /*
     * DUTY1 = the channel phase, written here and NEVER touched again --
     * this is exactly what the stock firmware does at 0x6713-0x68CD,
     * and the values below are its own, recorded one by one.
     * DUTY2 starts at the same value: zero pulse width, dark panel.
     */
    SET_PWM_DUTY(PWM00, AULA_RGB_PHASE(15), AULA_RGB_PHASE(15));
    SET_PWM_DUTY(PWM01, AULA_RGB_PHASE(16), AULA_RGB_PHASE(16));
    SET_PWM_DUTY(PWM02, AULA_RGB_PHASE(17), AULA_RGB_PHASE(17));
    SET_PWM_DUTY(PWM03, AULA_RGB_PHASE(12), AULA_RGB_PHASE(12));
    SET_PWM_DUTY(PWM04, AULA_RGB_PHASE(13), AULA_RGB_PHASE(13));
    SET_PWM_DUTY(PWM05, AULA_RGB_PHASE(14), AULA_RGB_PHASE(14));
    SET_PWM_DUTY(PWM10, AULA_RGB_PHASE(6), AULA_RGB_PHASE(6));
    SET_PWM_DUTY(PWM11, AULA_RGB_PHASE(7), AULA_RGB_PHASE(7));
    SET_PWM_DUTY(PWM12, AULA_RGB_PHASE(8), AULA_RGB_PHASE(8));
    SET_PWM_DUTY(PWM13, AULA_RGB_PHASE(9), AULA_RGB_PHASE(9));
    SET_PWM_DUTY(PWM14, AULA_RGB_PHASE(10), AULA_RGB_PHASE(10));
    SET_PWM_DUTY(PWM15, AULA_RGB_PHASE(11), AULA_RGB_PHASE(11));
    SET_PWM_DUTY(PWM20, AULA_RGB_PHASE(0), AULA_RGB_PHASE(0));
    SET_PWM_DUTY(PWM21, AULA_RGB_PHASE(1), AULA_RGB_PHASE(1));
    SET_PWM_DUTY(PWM22, AULA_RGB_PHASE(2), AULA_RGB_PHASE(2));
    SET_PWM_DUTY(PWM23, AULA_RGB_PHASE(3), AULA_RGB_PHASE(3));
    SET_PWM_DUTY(PWM24, AULA_RGB_PHASE(4), AULA_RGB_PHASE(4));
    SET_PWM_DUTY(PWM25, AULA_RGB_PHASE(5), AULA_RGB_PHASE(5));
}
#endif

void user_init(void)
{
    user_gpio_init();
#ifndef BOOT_CHORD_HEADER
    user_pwm_init();
#endif
}
