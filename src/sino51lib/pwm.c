#include "pwm_hw.h"

#if PWM_HW_PRESENT

#    include "interrupts.h"
#    if __has_include("kbdef.h")
#        include "kbdef.h"
#    endif
#    ifdef MATRIX_INTERLEAVED_SCAN
#        include "pwm.h"
#    endif

void pwm_interrupt_handler(void) __interrupt(PWM_VECTOR)
{
#    ifdef MATRIX_INTERLEAVED_SCAN
    PWM00CON = PWM_CON_PARKED;
    PWM10CON = PWM_CON_PARKED;
    PWM20CON = PWM_CON_PARKED;
    P6       = 0xFFu;
    P5 |= (uint8_t)KB_C_P5_MASK;
    P4 |= (uint8_t)KB_C_P4_MASK;
#    endif
}

#endif
