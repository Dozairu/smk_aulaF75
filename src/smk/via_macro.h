#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(VIA_ENABLE) && defined(DYNAMIC_KEYMAP_ENABLE)

#    define VIA_MACRO_ENABLE 1

/* Starts playing VIA macro `index` (0..DYNAMIC_MACRO_COUNT-1). Ignored while another macro is running or when the slot is empty. */
void via_macro_play(uint8_t index);

/* Called from the tick dispatcher: counts down the delay between macro actions. */
void via_macro_tick(void);

/* Called from the main loop: runs the next due macro action. */
void via_macro_task(void);

#endif
