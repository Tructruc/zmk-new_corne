/*
 * The Bluetooth name the paired host gave us, per profile slot.
 *
 * Cosmetic only: an empty string means "not known", never an error.
 */

#pragma once

#include <stdint.h>

/* 1..5 (ZMK's profile slots), anything else returns "". */
const char *host_name_get(uint8_t slot);
