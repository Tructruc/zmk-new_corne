/*
 * Beopylar status screen for the nice!view panels.
 *
 * The nice!view is a 160x68 strip that sits vertically on the eyelash_corne, so
 * the screen is *designed* on a 68x160 portrait canvas and copied into the panel
 * buffer rotated by 90 degrees (see present()).  Drawing in portrait keeps the
 * layout readable while the copy loop keeps the rotation exact and testable.
 *
 * Central half (left):
 *
 *     +--------------------+      battery bar, percentage, charging bolt
 *     | [#########]   88%  |      (ZMK has no charge state, so the bolt means
 *     | ------------------ |      "USB is powered")
 *     |       BASE         |      active layer, filled badge
 *     | ------------------ |
 *     | USB           B2   |      output and BLE profile
 *     | ------------------ |
 *     |  CTL     |  GUI    |      held modifiers, polled every 250 ms because
 *     |  ALT     |  SFT    |      this ZMK raises no modifier event
 *     | ------------------ |
 *     |  * R 87%           |      right half heard within the last 90 s
 *     +--------------------+
 *
 * Peripheral half (right): battery, charging bolt and the link to the central.
 *
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) ||                           \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT) && !BEOPYLAR_PERIPHERAL)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_BEOPYLAR_HOST_NAMES)
#include "host_names.h"
#endif

#include "beopylar_display.h"

ZMK_EVENT_IMPL(beopylar_mirror_state);

LV_IMG_DECLARE(beopylar_bolt);

/* ------------------------------------------------------------------------- */
/* Layout                                                                    */
/* ------------------------------------------------------------------------- */

/* Panel, in the orientation the display driver talks to it. */
#define PANEL_W 160
#define PANEL_H 68

/* Design canvas: the panel rotated a quarter turn, which is how it is mounted. */
#define DESIGN_W 68
#define DESIGN_H 160

/* How often the modifier state is sampled; ZMK raises no modifier event. */
#define MOD_POLL_MS 250

/* The right half reports its battery on every ZMK battery interval; if nothing
 * arrives for this long it is treated as out of touch. */
#define PEER_STALE_MS 90000
/* After the lead is pulled the cell still reads high for a while (surface
 * charge), so keep drawing the unknown battery until this has passed
 * instead of printing a number nobody should trust. */
#define BATTERY_SETTLE_MS 60000

/* Pages. STATUS is what the screen shows by default; LINK and KEYS are reached
 * with the &display_next key on FN and wrap back to STATUS. */
enum beopylar_page {
    PAGE_STATUS = 0,
    PAGE_LINK,
    PAGE_MANUAL_COUNT,
};

/* How many keystrokes the KEYS page remembers. */

/* sentinels for display_state.peer_age_min */
#define PEER_AGE_NEVER 0xFF
#define PEER_AGE_STALE 0xFE

/* The right half of a split board has no keymap state of its own to show. */
#define BEOPYLAR_PERIPHERAL                                                                        \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#if IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED)
#define BG_COLOR lv_color_black()
#define FG_COLOR lv_color_white()
#else
#define BG_COLOR lv_color_white()
#define FG_COLOR lv_color_black()
#endif

static lv_obj_t *canvas; /* portrait scratch canvas, everything is drawn here */
static lv_obj_t *panel;  /* what the display driver renders */
static lv_color_t design_buf[DESIGN_W * DESIGN_H];
static lv_color_t panel_buf[PANEL_W * PANEL_H];

/* Everything the two renders need, already resolved from the ZMK APIs so the
 * drawing code never has to call back into the system from the display thread. */
struct display_state {
    uint8_t battery;
    bool charging;
    bool settling;
    int64_t settle_ts;
    bool endpoint_usb;
    uint8_t profile; /* 1-based, as printed next to the output */
    bool profile_bonded;
    char layer_name[12];
    char host[16];
    zmk_mod_flags_t mods;
    bool link;            /* peripheral: link to the central */
    uint8_t peer_battery; /* central: last battery level reported by the other half */
    bool peer_heard;
    int64_t peer_ts;
    uint8_t peer_age_min;        /* minutes since the last peer report, PEER_AGE_* */
    uint8_t profile_bonded_mask; /* bit i: profile i bonded */
    uint8_t profile_connected_mask;
    bool pairing; /* central: a BLE profile is open and waiting for a host */
};

static struct display_state shown;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT) && !BEOPYLAR_PERIPHERAL
/* One mirror event carries everything the right half cannot look up itself:
 * it has no keymap and no BLE manager, so layer/output state has to be sent. */
ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(beopylar_mirror_state, MIR, )
#endif

/* What the last paint actually showed, so an unchanged screen is not redrawn:
 * a repaint of the nice!view is a visible flash. */
static struct display_state painted;
static uint8_t painted_page = 0xFF;

/* See enum beopylar_page. */
static uint8_t manual_page = PAGE_STATUS;

/* Last keystrokes, newest first, with the modifiers that were held. */

/* ------------------------------------------------------------------------- */
/* Drawing primitives                                                        */
/* ------------------------------------------------------------------------- */

static void fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t color, int32_t radius) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_canvas_draw_rect(canvas, x, y, w, h, &dsc);
}

static void stroke_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = FG_COLOR;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_width = width;
    dsc.radius = radius;
    lv_canvas_draw_rect(canvas, x, y, w, h, &dsc);
}

static void draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, bool round) {
    lv_point_t points[2] = {{x1, y1}, {x2, y2}};
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = FG_COLOR;
    dsc.width = width;
    dsc.round_start = round ? 1 : 0;
    dsc.round_end = round ? 1 : 0;
    lv_canvas_draw_line(canvas, points, 2, &dsc);
}

static void draw_text(const char *text, int32_t x, int32_t y, int32_t max_w,
                      const lv_font_t *font, lv_color_t color, lv_text_align_t align) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.align = align;
    lv_canvas_draw_text(canvas, x, y, max_w, &dsc, text);
}

#if !BEOPYLAR_PERIPHERAL
static void draw_dot(int32_t x, int32_t y, int32_t r, bool filled) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = FG_COLOR;
    dsc.width = filled ? 2 * r : 1;
    dsc.rounded = 1;
    lv_canvas_draw_arc(canvas, x, y, r, 0, 360, &dsc);
}
#endif

/* Battery body with a terminal nub and the level as a filled bar. */
/* The ADC is looking at the charger while it is plugged in, so the level is
 * not known. Draw the shell with a dithered fill: it reads as "unknown"
 * rather than as a percentage nobody should trust. */
static void draw_battery_unknown(int32_t x, int32_t y, int32_t w, int32_t h) {
    stroke_rect(x, y, w, h, 3, 1);
    fill_rect(x + w + 1, y + h / 2 - 2, 2, 4, FG_COLOR, 0);
    for (int32_t i = x + 3; i < x + w - 3; i += 3) {
        fill_rect(i, y + 3, 1, h - 6, FG_COLOR, 0);
    }
}

static void draw_battery(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t level) {
    uint8_t clamped = level > 100 ? 100 : level;

    stroke_rect(x, y, w, h, 2, 1);
    fill_rect(x + w + 1, y + h / 2 - 3, 3, 6, FG_COLOR, 0);

    int32_t inner_w = (w - 6) * clamped / 100;
    if (inner_w > 0) {
        fill_rect(x + 2, y + 2, inner_w, h - 4, FG_COLOR, 1);
    }
}

static void draw_bolt(int32_t x, int32_t y) {
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, x, y, &beopylar_bolt, &img_dsc);
}

#if !BEOPYLAR_PERIPHERAL
/* A modifier indicator: filled while held, outlined while released. */
static void draw_mod_chip(int32_t x, int32_t y, int32_t w, int32_t h, const char *label,
                          bool active) {
    if (active) {
        fill_rect(x, y, w, h, FG_COLOR, 3);
        draw_text(label, x, y + 2, w, &lv_font_montserrat_14, BG_COLOR, LV_TEXT_ALIGN_CENTER);
    } else {
        stroke_rect(x, y, w, h, 3, 1);
        draw_text(label, x, y + 2, w, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_CENTER);
    }
}
#endif

/* Copy the portrait design into the landscape panel buffer, rotating it a
 * quarter turn counter-clockwise: design (dx, dy) lands at panel (159 - dy, dx). */
static void present(void) {
    for (int32_t dy = 0; dy < DESIGN_H; dy++) {
        /* A design row becomes a panel column, so the "row length" of the
         * panel buffer here is its WIDTH (160), not its height. */
        int32_t px = PANEL_W - 1 - dy;
        for (int32_t dx = 0; dx < DESIGN_W; dx++) {
            panel_buf[dx * PANEL_W + px] = design_buf[dy * DESIGN_W + dx];
        }
    }
    lv_obj_invalidate(panel);
}

/* ------------------------------------------------------------------------- */
/* Central half                                                              */
/* ------------------------------------------------------------------------- */

#if !BEOPYLAR_PERIPHERAL

/* A profile is open: show the slot number big and tell the user what to do on
 * the host, instead of the usual status rows. */
static void render_pairing(void) {
    char text[16];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    draw_text("PAIRING", 2, 3, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    /* Just the slot: how to pair is a one-off, the number is what you look for
     * in a list of devices. */
    fill_rect(2, 26, 64, 76, FG_COLOR, 6);
    snprintf(text, sizeof(text), "B%u", (unsigned)shown.profile);
    draw_text(text, 2, 52, 64, &lv_font_montserrat_28, BG_COLOR, LV_TEXT_ALIGN_CENTER);
}

static void render_central(void) {
    char text[16];
    char profile[8];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    /* While the lead is in, the ADC measures the charging voltage and reports
     * 100 %, so show the only thing actually known: that it is charging. */
    if (shown.charging || shown.settling) {
        draw_battery_unknown(2, 2, 60, 14);
        if (shown.charging) {
            draw_bolt(27, 17);
        }
    } else {
        draw_battery(2, 2, 60, 14, shown.battery);
        snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
        draw_text(text, 2, 18, 44, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);
    }

    draw_line(2, 40, 66, 40, 1, false);

    /* Active layer. */
    const char *layer = (shown.layer_name[0] != '\0') ? shown.layer_name : "?";
    fill_rect(2, 44, 64, 24, FG_COLOR, 4);
    draw_text(layer, 2, 46, 64, &lv_font_montserrat_16, BG_COLOR, LV_TEXT_ALIGN_CENTER);

    /* Output and profile on one centred line: at 68 px the two separate texts
     * touched, so "USB" ran into the slot number. */
    stroke_rect(2, 72, 64, 20, 4, 1);
    if (shown.profile_bonded) {
        snprintf(profile, sizeof(profile), "B%u", (unsigned)shown.profile);
    } else {
        snprintf(profile, sizeof(profile), "--");
    }
    snprintf(text, sizeof(text), "%s %s", shown.endpoint_usb ? "USB" : "BLE", profile);
    draw_text(text, 2, 75, 64, &lv_font_montserrat_12, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    /* Every modifier, so combinations read at a glance: a chip fills in while
     * that modifier is held. */
    draw_mod_chip(2, 96, 30, 20, "CTL", (shown.mods & (MOD_LCTL | MOD_RCTL)) != 0);
    draw_mod_chip(36, 96, 30, 20, "GUI", (shown.mods & (MOD_LGUI | MOD_RGUI)) != 0);
    draw_mod_chip(2, 118, 30, 20, "ALT", (shown.mods & (MOD_LALT | MOD_RALT)) != 0);
    draw_mod_chip(36, 118, 30, 20, "SFT", (shown.mods & (MOD_LSFT | MOD_RSFT)) != 0);

    draw_line(2, 142, 66, 142, 1, false);

    /* The other half: lit dot once it has reported in recently. */
    bool peer = shown.peer_age_min != PEER_AGE_NEVER && shown.peer_age_min != PEER_AGE_STALE;
    draw_dot(9, 150, 4, peer);
    if (peer && shown.peer_battery > 0) {
        snprintf(text, sizeof(text), "R %u%%", (unsigned)shown.peer_battery);
    } else {
        snprintf(text, sizeof(text), "R --");
    }
    draw_text(text, 18, 144, 48, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_LEFT);
}

/* LINK page: the five BLE profiles, the output in use and what the other half
 * last reported. */
static void render_link(void) {
    char text[24];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    draw_text("LINK", 2, 3, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    for (uint8_t i = 0; i < 5; i++) {
        int32_t cx = 10 + (int32_t)i * 12;
        if (shown.profile_connected_mask & (uint8_t)BIT(i)) {
            draw_dot(cx, 22, 4, true);
        } else if (shown.profile_bonded_mask & (uint8_t)BIT(i)) {
            draw_dot(cx, 22, 4, false);
        }
        if ((uint8_t)(i + 1) == shown.profile) {
            draw_line(cx - 3, 31, cx + 3, 31, 2, true);
        }
    }

    bool active_connected = shown.profile > 0 &&
                            (shown.profile_connected_mask & (uint8_t)BIT(shown.profile - 1)) != 0;
    /* montserrat_12, because "connected" is 9 characters and the 8 px unscii
     * grid would push the last one past the 68 px panel. */
    draw_text(active_connected ? "connected" : "idle", 0, 36, DESIGN_W,
              &lv_font_montserrat_12, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    snprintf(text, sizeof(text), "%s  B%u", shown.endpoint_usb ? "USB" : "BLE",
             (unsigned)shown.profile);
    draw_text(text, 2, 50, 64, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    /* The host's own name once its GATT read has landed; the slot number above
     * is the fallback, not the headline. */
    if (shown.host[0] != '\0') {
        draw_text(shown.host, 0, 66, DESIGN_W, &lv_font_montserrat_12, FG_COLOR,
                  LV_TEXT_ALIGN_CENTER);
    }

    draw_line(2, 82, 66, 82, 1, false);

    if (shown.peer_battery > 0 && shown.peer_age_min != PEER_AGE_STALE) {
        snprintf(text, sizeof(text), "R %u%%", (unsigned)shown.peer_battery);
    } else {
        snprintf(text, sizeof(text), "R --");
    }
    draw_text(text, 2, 88, 64, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    if (shown.peer_age_min == PEER_AGE_NEVER) {
        snprintf(text, sizeof(text), "waiting");
    } else if (shown.peer_age_min == PEER_AGE_STALE) {
        snprintf(text, sizeof(text), "no report");
    } else if (shown.peer_age_min == 0) {
        snprintf(text, sizeof(text), "just now");
    } else {
        snprintf(text, sizeof(text), "%u min ago", (unsigned)shown.peer_age_min);
    }
    draw_text(text, 2, 112, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    snprintf(text, sizeof(text), "PAGE %u/%u", (unsigned)(manual_page + 1),
             (unsigned)PAGE_MANUAL_COUNT);
    draw_text(text, 2, 144, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);
}

#endif /* !BEOPYLAR_PERIPHERAL */

/* ------------------------------------------------------------------------- */
/* Peripheral half                                                           */
/* ------------------------------------------------------------------------- */

#if BEOPYLAR_PERIPHERAL

static void render_peripheral(void) {
    char text[16];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    /* Its own power state: same lie while charging, same honest answer. */
    if (shown.charging || shown.settling) {
        draw_battery_unknown(2, 3, 60, 14);
        if (shown.charging) {
            draw_bolt(27, 18);
        }
    } else {
        draw_battery(2, 3, 60, 14, shown.battery);
        snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
        draw_text(text, 2, 20, 44, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);
    }

    draw_line(2, 42, 66, 42, 1, false);

    /* Mirrored from the central over the split link. */
    if (shown.layer_name[0] != '\0') {
        fill_rect(2, 46, 64, 22, FG_COLOR, 4);
        draw_text(shown.layer_name, 2, 48, 64, &lv_font_montserrat_16, BG_COLOR,
                  LV_TEXT_ALIGN_CENTER);
    }

    if (shown.endpoint_usb) {
        snprintf(text, sizeof(text), "USB");
    } else if (shown.profile > 0) {
        snprintf(text, sizeof(text), "B%u", (unsigned)shown.profile);
    } else {
        snprintf(text, sizeof(text), "--");
    }
    draw_text(text, 2, 74, 64, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    draw_line(2, 94, 66, 94, 1, false);

    if (shown.link) {
        fill_rect(2, 100, 64, 24, FG_COLOR, 4);
        draw_text("LINKED", 2, 103, 64, &lv_font_montserrat_14, BG_COLOR, LV_TEXT_ALIGN_CENTER);
    } else {
        stroke_rect(2, 100, 64, 24, 4, 1);
        draw_text("WAIT", 2, 103, 64, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_CENTER);
    }

    draw_text("RIGHT", 2, 150, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);
}

#endif /* BEOPYLAR_PERIPHERAL */

#if !BEOPYLAR_PERIPHERAL
static void render_central(void);
static void render_link(void);
static void render_pairing(void);
#endif

static void render(void) {
    if (canvas == NULL || panel == NULL) {
        return;
    }

    if (manual_page == painted_page && memcmp(&shown, &painted, sizeof(shown)) == 0) {
        return;
    }

#if BEOPYLAR_PERIPHERAL
    render_peripheral();
#else
    if (shown.pairing) {
        /* Pairing takes the screen over: it is the only thing you need to see
         * for the few seconds it lasts. */
        render_pairing();
    } else {
        switch (manual_page) {
        case PAGE_LINK:
            render_link();
            break;
        default:
            render_central();
            break;
        }
    }
#endif

    present();

    painted = shown;
    painted_page = manual_page;
}

/* ------------------------------------------------------------------------- */
/* State listeners                                                           */
/* ------------------------------------------------------------------------- */

struct battery_ui_state {
    uint8_t level;
};

static struct battery_ui_state battery_ui_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_ui_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
    };
}

static void battery_ui_update_cb(struct battery_ui_state state) {
    shown.battery = state.level;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(battery_ui, struct battery_ui_state, battery_ui_update_cb,
                            battery_ui_get_state)

ZMK_SUBSCRIPTION(battery_ui, zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)

struct usb_ui_state {
    bool powered;
};

static struct usb_ui_state usb_ui_get_state(const zmk_event_t *eh) {
    return (struct usb_ui_state){
        .powered = zmk_usb_is_powered(),
    };
}

static void usb_ui_update_cb(struct usb_ui_state state) {
    if (shown.charging && !state.powered) {
        /* Just unplugged: the reading was the charger's, not the cell's. */
        shown.settling = true;
        shown.settle_ts = k_uptime_get() + BATTERY_SETTLE_MS;
    }
    shown.charging = state.powered;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(usb_ui, struct usb_ui_state, usb_ui_update_cb, usb_ui_get_state)

ZMK_SUBSCRIPTION(usb_ui, zmk_usb_conn_state_changed);

#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

#if !BEOPYLAR_PERIPHERAL

struct output_ui_state {
    struct zmk_endpoint_instance endpoint;
    uint8_t profile;
    bool bonded;
    uint8_t bonded_mask;
    uint8_t connected_mask;
};

static struct output_ui_state output_ui_get_state(const zmk_event_t *eh) {
    struct output_ui_state state = {
        .endpoint = zmk_endpoints_selected(),
        .profile = (uint8_t)(zmk_ble_active_profile_index() + 1),
        .bonded = !zmk_ble_active_profile_is_open(),
    };

    for (uint8_t i = 0; i < 5; i++) {
        if (!zmk_ble_profile_is_open(i)) {
            state.bonded_mask |= (uint8_t)BIT(i);
        }
        if (zmk_ble_profile_is_connected(i)) {
            state.connected_mask |= (uint8_t)BIT(i);
        }
    }

    return state;
}

static void output_ui_update_cb(struct output_ui_state state) {
    shown.endpoint_usb = state.endpoint.transport == ZMK_TRANSPORT_USB;
    shown.profile = state.profile;
    shown.profile_bonded = state.bonded;
    shown.profile_bonded_mask = state.bonded_mask;
    shown.profile_connected_mask = state.connected_mask;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(output_ui, struct output_ui_state, output_ui_update_cb,
                            output_ui_get_state)

ZMK_SUBSCRIPTION(output_ui, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(output_ui, zmk_ble_active_profile_changed);
#endif

struct layer_ui_state {
    const char *name;
};

static struct layer_ui_state layer_ui_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();

    return (struct layer_ui_state){
        .name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

static void layer_ui_update_cb(struct layer_ui_state state) {
    if (state.name == NULL) {
        return;
    }
    strncpy(shown.layer_name, state.name, sizeof(shown.layer_name) - 1);
    shown.layer_name[sizeof(shown.layer_name) - 1] = '\0';
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(layer_ui, struct layer_ui_state, layer_ui_update_cb,
                            layer_ui_get_state)

ZMK_SUBSCRIPTION(layer_ui, zmk_layer_state_changed);

/* ZMK never raises zmk_modifiers_state_changed (see behavior_hold_tap.c), so the
 * modifier state is sampled instead of subscribed to.  The sample runs on the
 * system work queue and only hands a repaint to the display queue when
 * something actually changed. */
static void mod_poll_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(mod_poll, mod_poll_handler);

static void render_work_handler(struct k_work *work) { render(); }

static K_WORK_DEFINE(render_work, render_work_handler);

void beopylar_display_shift_page(int8_t delta) {
    if (canvas == NULL || delta == 0) {
        return;
    }

    int next = ((int)manual_page + delta) % (int)PAGE_MANUAL_COUNT;
    if (next < 0) {
        next += (int)PAGE_MANUAL_COUNT;
    }
    manual_page = (uint8_t)next;
    k_work_submit_to_queue(zmk_display_work_q(), &render_work);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
/* Send the right half what changed since the last publish. Cheap enough to ride
 * on the same 250 ms poll as the rest of the sampled state. */
static void mirror_publish(void) {
    static struct beopylar_mirror_state published;

    struct beopylar_mirror_state next = {0};
    strncpy(next.layer, shown.layer_name, sizeof(next.layer) - 1);
    next.profile = shown.profile;
    next.usb = shown.endpoint_usb;

    if (memcmp(&next, &published, sizeof(next)) == 0) {
        return;
    }
    published = next;
    raise_beopylar_mirror_state(next);
}
#else
static void mirror_publish(void) {}
#endif

static void mod_poll_handler(struct k_work *work) {
    if (shown.settling && k_uptime_get() > shown.settle_ts) {
        shown.settling = false;
        shown.settle_ts = 0;
    }
    bool dirty = false;

    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    if (mods != shown.mods) {
        shown.mods = mods;
        dirty = true;
    }

    /* Pairing has no event of its own, so it rides on the same poll. */
#if IS_ENABLED(CONFIG_ZMK_BLE)
    bool pairing = zmk_ble_active_profile_is_open();
    if (pairing != shown.pairing) {
        shown.pairing = pairing;
        dirty = true;
    }
#endif

    /* Age of the last report from the other half, in whole minutes. Only a
     * change of bucket repaints, so the screen does not flash every second. */
    uint8_t age = PEER_AGE_NEVER;
    if (shown.peer_heard) {
        int64_t ms = k_uptime_get() - shown.peer_ts;
        age = (ms >= PEER_STALE_MS) ? PEER_AGE_STALE : (uint8_t)(ms / 60000);
    }
    if (age != shown.peer_age_min) {
        shown.peer_age_min = age;
        dirty = true;
    }

#if IS_ENABLED(CONFIG_BEOPYLAR_HOST_NAMES)
    const char *host = host_name_get(shown.profile);
    if (strncmp(shown.host, host, sizeof(shown.host) - 1) != 0) {
        strncpy(shown.host, host, sizeof(shown.host) - 1);
        shown.host[sizeof(shown.host) - 1] = '\0';
        dirty = true;
    }
#endif

    if (dirty) {
        k_work_submit_to_queue(zmk_display_work_q(), &render_work);
    }

    mirror_publish();

    k_work_schedule(&mod_poll, K_MSEC(MOD_POLL_MS));
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

struct peer_ui_state {
    uint8_t level;
};

static struct peer_ui_state peer_ui_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    return (struct peer_ui_state){
        .level = (ev != NULL) ? ev->state_of_charge : 0,
    };
}

static void peer_ui_update_cb(struct peer_ui_state state) {
    shown.peer_battery = state.level;
    shown.peer_heard = true;
    shown.peer_ts = k_uptime_get();
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(peer_ui, struct peer_ui_state, peer_ui_update_cb, peer_ui_get_state)

ZMK_SUBSCRIPTION(peer_ui, zmk_peripheral_battery_state_changed);

#endif /* CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING */

#else /* BEOPYLAR_PERIPHERAL */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
/* The central mirrors these over the split link; re-raise them locally so the
 * listeners below can treat them as ordinary ZMK events. */
ZMK_RELAY_EVENT_HANDLE(beopylar_mirror_state, MIR, )
#endif

/* Peripheral side: the mirror event is the only source of truth, because the
 * keymap and the output manager live on the central half. */
struct mirror_ui_state {
    struct beopylar_mirror_state mirror;
    bool valid;
};

static struct mirror_ui_state mirror_ui_get_state(const zmk_event_t *eh) {
    struct mirror_ui_state state = {0};
    const struct beopylar_mirror_state *ev = as_beopylar_mirror_state(eh);

    if (ev != NULL) {
        state.mirror = *ev;
        state.valid = true;
    }
    return state;
}

static void mirror_ui_update_cb(struct mirror_ui_state state) {
    if (!state.valid) {
        return;
    }
    memcpy(shown.layer_name, state.mirror.layer, sizeof(shown.layer_name));
    shown.layer_name[sizeof(shown.layer_name) - 1] = '\0';
    shown.profile = state.mirror.profile;
    shown.endpoint_usb = state.mirror.usb;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(mirror_ui, struct mirror_ui_state, mirror_ui_update_cb,
                            mirror_ui_get_state)

ZMK_SUBSCRIPTION(mirror_ui, beopylar_mirror_state);

struct link_ui_state {
    bool link;
};

static struct link_ui_state link_ui_get_state(const zmk_event_t *eh) {
    return (struct link_ui_state){
        .link = zmk_split_bt_peripheral_is_connected(),
    };
}

static void link_ui_update_cb(struct link_ui_state state) {
    shown.link = state.link;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(link_ui, struct link_ui_state, link_ui_update_cb, link_ui_get_state)

ZMK_SUBSCRIPTION(link_ui, zmk_split_peripheral_status_changed);

void beopylar_display_shift_page(int8_t delta) { ARG_UNUSED(delta); }

#endif /* BEOPYLAR_PERIPHERAL */

/* ------------------------------------------------------------------------- */
/* Screen                                                                    */
/* ------------------------------------------------------------------------- */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    panel = lv_canvas_create(screen);
    lv_canvas_set_buffer(panel, panel_buf, PANEL_W, PANEL_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Scratch canvas the drawing code works on; never shown by itself. */
    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, design_buf, DESIGN_W, DESIGN_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);

    /* Seed the state so the first paint is right before any event arrives.
     * Everything but the battery and the USB state is central-only: endpoint,
     * BLE, keymap and modifier lookups do not exist in a peripheral build. */
    shown.battery = zmk_battery_state_of_charge();
    shown.peer_age_min = PEER_AGE_NEVER;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    shown.charging = zmk_usb_is_powered();
#endif

#if !BEOPYLAR_PERIPHERAL
    shown.endpoint_usb = zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
    shown.profile = (uint8_t)(zmk_ble_active_profile_index() + 1);
    shown.profile_bonded = !zmk_ble_active_profile_is_open();
    shown.pairing = zmk_ble_active_profile_is_open();
    for (uint8_t i = 0; i < 5; i++) {
        if (!zmk_ble_profile_is_open(i)) {
            shown.profile_bonded_mask |= (uint8_t)BIT(i);
        }
        if (zmk_ble_profile_is_connected(i)) {
            shown.profile_connected_mask |= (uint8_t)BIT(i);
        }
    }

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    const char *layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));
    if (layer_name != NULL) {
        strncpy(shown.layer_name, layer_name, sizeof(shown.layer_name) - 1);
    }
    shown.mods = zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peer_level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &peer_level) == 0 && peer_level > 0) {
        shown.peer_battery = peer_level;
    }
#endif

#else
    shown.link = zmk_split_bt_peripheral_is_connected();
#endif

    battery_ui_init();
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    usb_ui_init();
#endif

#if BEOPYLAR_PERIPHERAL
    link_ui_init();
    mirror_ui_init();
#else
    output_ui_init();
    layer_ui_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    peer_ui_init();
#endif
#endif

    render();

#if !BEOPYLAR_PERIPHERAL
    k_work_schedule(&mod_poll, K_MSEC(MOD_POLL_MS));
#endif

    return screen;
}
