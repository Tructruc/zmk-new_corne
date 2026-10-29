/*
 * Shared between the status screen and the behavior that flips its pages.
 *
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Move the manual page by `delta` (-1 = previous, +1 = next). */
void beopylar_display_shift_page(int8_t delta);
