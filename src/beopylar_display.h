/*
 * Shared between the status screen and the behavior that flips its pages.
 *
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zmk/event_manager.h>

/* Move the manual page by `delta` (-1 = previous, +1 = next). */
void beopylar_display_shift_page(int8_t delta);

/* A peripheral half has neither a keymap nor BLE manager state, so the central
 * mirrors the few fields its screen needs over the split relay. */
struct beopylar_mirror_state {
    char layer[12];
    uint8_t profile;
    bool usb;
};

ZMK_EVENT_DECLARE(beopylar_mirror_state);

/* The peripheral's own verdict on its battery, relayed to the central so page 2
 * shows exactly what the right half shows - and nothing while it charges. */
struct beopylar_peer_status {
    uint8_t battery;
    bool charging;
    bool settling;
};

ZMK_EVENT_DECLARE(beopylar_peer_status);
