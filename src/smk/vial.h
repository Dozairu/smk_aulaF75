#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef VIAL_ENABLE

/* First byte of every Vial command, inside the VIA raw HID channel. */
#    define VIAL_PREFIX 0xFEu

/* Vial protocol 6: current vial-qmk, keycodes v6 (QMK's QK_* numbering). */
#    define VIAL_PROTOCOL_VERSION 6u

/* Handles a 32-byte packet whose data[0] is VIAL_PREFIX, in place. */
void vial_process(__xdata uint8_t *data);

/* True while an unlock is in progress: VIA must then refuse everything except
 * the Vial commands the unlock dialog itself uses (see vial_allowed_during_unlock). */
bool vial_unlock_in_progress(void);
bool vial_allowed_during_unlock(const __xdata uint8_t *data);

/* Gate for the commands vial-qmk refuses while locked (matrix state, macro writes). */
bool vial_unlocked(void);

/* From the tick ISR, ~400 us per call: timebase for the unlock hold. */
void vial_tick(void);

/* QMK settings in the dynamic store (dynamic_vial_store()):
 * defaults when the store is created, and store -> action engine. */
void vial_store_defaults(void);
void vial_store_apply(void);

#endif
