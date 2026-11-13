#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef VIA_ENABLE

#    define VIA_PROTOCOL_VERSION 0x000Du

bool via_process_packet(__xdata uint8_t *data);

#endif
