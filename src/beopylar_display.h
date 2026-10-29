/*
 * Beopylar status screen: entry points the rest of the firmware may call.
 *
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * Advance the status screen to its next page.
 *
 * Safe to call from any context: the repaint itself is queued on the display
 * work queue.  On the peripheral half this does nothing, because that screen
 * only ever shows the battery and the link.
 */
void beopylar_display_next_page(void);
