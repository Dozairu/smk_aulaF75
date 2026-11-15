#pragma once

#include <stdint.h>

#ifdef VIALRGB_ENABLE

/*
 * VialRGB (vial-qmk quantum/vialrgb.c, protocol 1): Vial GUI's Lighting tab.
 * Carried by the VIA lighting commands (0x07 set / 0x08 get / 0x09 save)
 * with a value id >= 0x40 in data[1]; via.c routes those here. The keyboard
 * implements these, mapping VialRGB effect ids and HSV onto its own engine.
 */
#    define VIALRGB_PROTOCOL_VERSION 1u
#    define VIALRGB_FIRST_ID         0x40u

#    define VIALRGB_GET_INFO        0x40u
#    define VIALRGB_GET_MODE        0x41u
#    define VIALRGB_GET_SUPPORTED   0x42u
#    define VIALRGB_GET_NUMBER_LEDS 0x43u
#    define VIALRGB_GET_LED_INFO    0x44u
#    define VIALRGB_SET_MODE        0x41u
#    define VIALRGB_DIRECT_FASTSET  0x42u

void vialrgb_get_value(__xdata uint8_t *data);
void vialrgb_set_value(__xdata uint8_t *data);
void vialrgb_save(void);

/* Stored lighting state -> engine, at boot (after the dynamic store loads). */
void vialrgb_apply(void);

#endif
