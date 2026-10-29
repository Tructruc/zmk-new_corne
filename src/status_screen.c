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
 *     | [#########]   88%  |
 *     | ------------------ |
 *     |       BASE         |      active layer, filled badge
 *     | ----               |
 *     | USB           B2   |      output and BLE profile
 *     | ----               |
 *     |  ALT    SFT        |      held modifiers (polled, ZMK does not raise
 *     | ----               |      a modifier event in this revision)
 *     |  * R 87%           |      right half heard within the last 90 s
 *     | ----               |
 *     | WPM            84  |
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
#include <zmk/wpm.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

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
    bool endpoint_usb;
    uint8_t profile; /* 1-based, as printed next to the output */
    bool profile_bonded;
    const char *layer_name;
    zmk_mod_flags_t mods;
    uint8_t wpm;
    bool link;            /* peripheral: link to the central */
    uint8_t peer_battery; /* central: last battery level reported by the other half */
    bool peer_heard;
    int64_t peer_ts;
};

static struct display_state shown;

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

static void render_central(void) {
    char text[16];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    /* Battery and, next to it, the level as a number. */
    draw_battery(2, 2, 60, 14, shown.battery);
    snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
    draw_text(text, 2, 18, 44, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);
    if (shown.charging) {
        draw_bolt(50, 20);
    }

    draw_line(2, 40, 66, 40, 1, false);

    /* Active layer. */
    const char *layer = (shown.layer_name != NULL && shown.layer_name[0] != '\0')
                            ? shown.layer_name
                            : "?";
    fill_rect(2, 44, 64, 24, FG_COLOR, 4);
    draw_text(layer, 2, 46, 64, &lv_font_montserrat_16, BG_COLOR, LV_TEXT_ALIGN_CENTER);

    /* Output and profile. */
    stroke_rect(2, 72, 64, 20, 4, 1);
    draw_text(shown.endpoint_usb ? "USB" : "BLE", 8, 74, 28, &lv_font_montserrat_14, FG_COLOR,
              LV_TEXT_ALIGN_LEFT);
    if (shown.profile_bonded) {
        snprintf(text, sizeof(text), "B%u", (unsigned)shown.profile);
    } else {
        snprintf(text, sizeof(text), "--");
    }
    draw_text(text, 36, 74, 26, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_RIGHT);

    /* Held modifiers: the two that change what the layout types. */
    draw_mod_chip(2, 96, 30, 20, "ALT", (shown.mods & (MOD_LALT | MOD_RALT)) != 0);
    draw_mod_chip(36, 96, 30, 20, "SFT", (shown.mods & (MOD_LSFT | MOD_RSFT)) != 0);

    /* The other half: lit dot once it has reported in recently. */
    bool peer = shown.peer_heard && (k_uptime_get() - shown.peer_ts) < PEER_STALE_MS;
    draw_dot(8, 128, 4, peer);
    if (peer && shown.peer_battery > 0) {
        snprintf(text, sizeof(text), "R %u%%", (unsigned)shown.peer_battery);
    } else {
        snprintf(text, sizeof(text), "R --");
    }
    draw_text(text, 16, 118, 50, &lv_font_montserrat_14, FG_COLOR, LV_TEXT_ALIGN_LEFT);

#if IS_ENABLED(CONFIG_ZMK_WPM)
    draw_text("WPM", 2, 143, 30, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_LEFT);
    snprintf(text, sizeof(text), "%u", (unsigned)shown.wpm);
    draw_text(text, 34, 138, 32, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_RIGHT);
#endif
}

#endif /* !BEOPYLAR_PERIPHERAL */

/* ------------------------------------------------------------------------- */
/* Peripheral half                                                           */
/* ------------------------------------------------------------------------- */

#if BEOPYLAR_PERIPHERAL

static void render_peripheral(void) {
    char text[16];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    /* The block is centred vertically: nothing else happens on this half. */
    draw_battery(2, 37, 60, 14, shown.battery);
    snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
    draw_text(text, 2, 53, 44, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);
    if (shown.charging) {
        draw_bolt(50, 55);
    }

    draw_line(2, 76, 66, 76, 1, false);

    draw_text("RIGHT", 2, 82, 64, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);

    if (shown.link) {
        fill_rect(2, 98, 64, 24, FG_COLOR, 4);
        draw_text("LINKED", 2, 100, 64, &lv_font_montserrat_16, BG_COLOR, LV_TEXT_ALIGN_CENTER);
    } else {
        stroke_rect(2, 98, 64, 24, 4, 1);
        draw_text("WAIT", 2, 100, 64, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_CENTER);
    }
}

#endif /* BEOPYLAR_PERIPHERAL */

static void render(void) {
    if (canvas == NULL || panel == NULL) {
        return;
    }

#if BEOPYLAR_PERIPHERAL
    render_peripheral();
#else
    render_central();
#endif

    present();
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
};

static struct output_ui_state output_ui_get_state(const zmk_event_t *eh) {
    return (struct output_ui_state){
        .endpoint = zmk_endpoints_selected(),
        .profile = (uint8_t)(zmk_ble_active_profile_index() + 1),
        .bonded = !zmk_ble_active_profile_is_open(),
    };
}

static void output_ui_update_cb(struct output_ui_state state) {
    shown.endpoint_usb = state.endpoint.transport == ZMK_TRANSPORT_USB;
    shown.profile = state.profile;
    shown.profile_bonded = state.bonded;
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
    shown.layer_name = state.name;
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

static void mod_poll_handler(struct k_work *work) {
    bool dirty = false;

    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    if (mods != shown.mods) {
        shown.mods = mods;
        dirty = true;
    }

    if (shown.peer_heard && (k_uptime_get() - shown.peer_ts) >= PEER_STALE_MS) {
        shown.peer_heard = false;
        dirty = true;
    }

    if (dirty) {
        k_work_submit_to_queue(zmk_display_work_q(), &render_work);
    }

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

#if IS_ENABLED(CONFIG_ZMK_WPM)

struct wpm_ui_state {
    uint8_t wpm;
};

static struct wpm_ui_state wpm_ui_get_state(const zmk_event_t *eh) {
    return (struct wpm_ui_state){
        .wpm = (uint8_t)zmk_wpm_get_state(),
    };
}

static void wpm_ui_update_cb(struct wpm_ui_state state) {
    if (state.wpm != shown.wpm) {
        shown.wpm = state.wpm;
        render();
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(wpm_ui, struct wpm_ui_state, wpm_ui_update_cb, wpm_ui_get_state)

ZMK_SUBSCRIPTION(wpm_ui, zmk_wpm_state_changed);

#endif /* IS_ENABLED(CONFIG_ZMK_WPM) */

#else /* BEOPYLAR_PERIPHERAL */

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
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    shown.charging = zmk_usb_is_powered();
#endif

#if !BEOPYLAR_PERIPHERAL
    shown.endpoint_usb = zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
    shown.profile = (uint8_t)(zmk_ble_active_profile_index() + 1);
    shown.profile_bonded = !zmk_ble_active_profile_is_open();

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    shown.layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));
    shown.mods = zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peer_level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &peer_level) == 0 && peer_level > 0) {
        shown.peer_battery = peer_level;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
    shown.wpm = (uint8_t)zmk_wpm_get_state();
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
#else
    output_ui_init();
    layer_ui_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    peer_ui_init();
#endif
#if IS_ENABLED(CONFIG_ZMK_WPM)
    wpm_ui_init();
#endif
#endif

    render();

#if !BEOPYLAR_PERIPHERAL
    k_work_schedule(&mod_poll, K_MSEC(MOD_POLL_MS));
#endif

    return screen;
}
