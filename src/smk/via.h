#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIA_ENABLE

#    ifdef VIAL_ENABLE
/* Vial GUI only accepts VIA protocol 9 (vial-gui SUPPORTED_VIA_PROTOCOL). */
#        define VIA_PROTOCOL_VERSION 0x0009u
#    else
#        define VIA_PROTOCOL_VERSION 0x000Du
#    endif

bool via_process_packet(__xdata uint8_t *data);

#endif
