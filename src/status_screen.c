/*
 * Beopylar status screen for the eyelash_corne nice!view panels.
 *
 * The stock nice!view widget rotates three 68x68 canvases behind each other on
 * a 160x68 panel, so most of the right side is clipped and the rest is three
 * cramped tiles. This replaces it with one 160x68 canvas that is laid out by
 * hand:
 *
 *   CENTRAL (left half)                                PERIPHERAL (right half)
 *   +---------------------------------------------+    +----------------------+
 *   | [==] 87%   [ BASE ]               (USB)     |    | RIGHT                |
 *   | ------------------------------------------- |    | [====] 64%           |
 *   | [CMD] [CTL] [ALT] [SFT]          WPM        |    |                      |
 *   |                                    72       |    |   * LINKED           |
 *   |  ~~~~ WPM history sparkline ~~~~            |    +----------------------+
 *   +---------------------------------------------+
 *
 * Everything is redrawn into the canvas buffer on each state change and the
 * canvas is invalidated once, so LVGL only repaints what changed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <dt-bindings/zmk/modifiers.h>

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
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#endif

/* ------------------------------------------------------------------------- */
/* Layout                                                                    */
/* ------------------------------------------------------------------------- */

#define SCREEN_W 160
#define SCREEN_H 68

/* Number of samples kept for the WPM sparkline. */
#define WPM_POINTS 12

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

static lv_obj_t *canvas;
static lv_color_t canvas_buf[SCREEN_W * SCREEN_H];

/* Everything the two renders need, already resolved from the ZMK APIs so the
 * drawing code never has to call back into the system from the display thread. */
struct display_state {
    uint8_t battery;
    bool usb;
    uint8_t profile; /* 1-based, as printed on the pill */
    bool profile_bonded;
    bool profile_connected;
    const char *layer_name;
    zmk_mod_flags_t mods;
    uint8_t wpm;
    uint8_t wpm_history[WPM_POINTS];
    bool link;
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

static void __unused draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, bool round) {
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

static void draw_dot(int32_t x, int32_t y, int32_t r, bool filled) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = FG_COLOR;
    dsc.width = filled ? 2 * r : 1;
    dsc.rounded = 1;
    lv_canvas_draw_arc(canvas, x, y, r, 0, 360, &dsc);
}

static void draw_battery(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t level) {
    uint8_t clamped = level > 100 ? 100 : level;

    /* Body, terminal nub, then the level as a filled bar inside the body. */
    stroke_rect(x, y, w, h, 2, 1);
    fill_rect(x + w, y + h / 2 - 2, 2, 4, FG_COLOR, 0);

    int32_t inner_w = (w - 5) * clamped / 100;
    if (inner_w > 0) {
        fill_rect(x + 2, y + 2, inner_w, h - 4, FG_COLOR, 1);
    }
}

/* A modifier indicator: filled while held, outlined while released. */
static void __unused draw_mod_pill(int32_t x, int32_t y, int32_t w, int32_t h, const char *label,
                          bool active) {
    if (active) {
        fill_rect(x, y, w, h, FG_COLOR, 3);
        draw_text(label, x, y + 2, w, &lv_font_unscii_8, BG_COLOR, LV_TEXT_ALIGN_CENTER);
    } else {
        stroke_rect(x, y, w, h, 3, 1);
        draw_text(label, x, y + 2, w, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);
    }
}

/* ------------------------------------------------------------------------- */
/* Central half                                                              */
/* ------------------------------------------------------------------------- */

#if !BEOPYLAR_PERIPHERAL

static void draw_wpm_graph(void) {
    const int32_t x0 = 3;
    const int32_t y_base = 66;
    const int32_t y_top = 48;
    const int32_t step = 10;

    draw_line(x0, y_base, x0 + step * (WPM_POINTS - 1), y_base, 1, false);

    uint8_t max = 1;
    for (int i = 0; i < WPM_POINTS; i++) {
        if (shown.wpm_history[i] > max) {
            max = shown.wpm_history[i];
        }
    }
    /* Keep a floor on the scale so a short burst does not fill the whole box. */
    if (max < 20) {
        max = 20;
    }

    lv_point_t points[WPM_POINTS];
    for (int i = 0; i < WPM_POINTS; i++) {
        points[i].x = x0 + i * step;
        points[i].y = y_base - (int32_t)shown.wpm_history[i] * (y_base - y_top) / max;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = FG_COLOR;
    dsc.width = 2;
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_canvas_draw_line(canvas, points, WPM_POINTS, &dsc);

    /* Mark the newest sample. */
    draw_dot(points[WPM_POINTS - 1].x, points[WPM_POINTS - 1].y, 2, true);
}

static void render_central(void) {
    char text[16];

    lv_canvas_fill_bg(canvas, BG_COLOR, LV_OPA_COVER);

    /* Row 1: battery, layer badge, output. */
    draw_battery(3, 4, 26, 13, shown.battery);
    snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
    draw_text(text, 33, 0, 36, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);

    const char *layer = (shown.layer_name != NULL && shown.layer_name[0] != '\0')
                            ? shown.layer_name
                            : "?";
    fill_rect(70, 2, 56, 19, FG_COLOR, 4);
    draw_text(layer, 70, 3, 56, &lv_font_montserrat_16, BG_COLOR, LV_TEXT_ALIGN_CENTER);

    if (shown.usb) {
        fill_rect(128, 3, 29, 17, FG_COLOR, 4);
        draw_text("USB", 128, 8, 29, &lv_font_unscii_8, BG_COLOR, LV_TEXT_ALIGN_CENTER);
    } else if (shown.profile_bonded) {
        snprintf(text, sizeof(text), "B%u", (unsigned)shown.profile);
        if (shown.profile_connected) {
            fill_rect(128, 3, 29, 17, FG_COLOR, 4);
            draw_text(text, 128, 8, 29, &lv_font_unscii_8, BG_COLOR, LV_TEXT_ALIGN_CENTER);
        } else {
            stroke_rect(128, 3, 29, 17, 4, 1);
            draw_text(text, 128, 8, 29, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);
        }
    } else {
        draw_text("--", 128, 8, 29, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_CENTER);
    }

    draw_line(3, 23, 156, 23, 1, false);

    /* Row 2: held modifiers. */
    draw_mod_pill(3, 26, 26, 13, "CMD", (shown.mods & (MOD_LGUI | MOD_RGUI)) != 0);
    draw_mod_pill(31, 26, 26, 13, "CTL", (shown.mods & (MOD_LCTL | MOD_RCTL)) != 0);
    draw_mod_pill(59, 26, 26, 13, "ALT", (shown.mods & (MOD_LALT | MOD_RALT)) != 0);
    draw_mod_pill(87, 26, 26, 13, "SFT", (shown.mods & (MOD_LSFT | MOD_RSFT)) != 0);

#if IS_ENABLED(CONFIG_ZMK_WPM)
    draw_text("WPM", 117, 27, 40, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_RIGHT);
    snprintf(text, sizeof(text), "%u", (unsigned)shown.wpm);
    draw_text(text, 117, 36, 40, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_RIGHT);
    draw_wpm_graph();
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
    stroke_rect(3, 3, 154, 62, 8, 1);

    draw_text("RIGHT", 11, 10, 60, &lv_font_unscii_8, FG_COLOR, LV_TEXT_ALIGN_LEFT);

    draw_battery(11, 28, 56, 22, shown.battery);
    snprintf(text, sizeof(text), "%u%%", (unsigned)shown.battery);
    draw_text(text, 75, 28, 60, &lv_font_montserrat_16, FG_COLOR, LV_TEXT_ALIGN_LEFT);

    draw_dot(98, 54, 3, shown.link);
    draw_text(shown.link ? "LINKED" : "WAIT", 106, 51, 46, &lv_font_unscii_8, FG_COLOR,
              LV_TEXT_ALIGN_LEFT);
}

#endif /* BEOPYLAR_PERIPHERAL */

static void render(void) {
    if (canvas == NULL) {
        return;
    }

#if BEOPYLAR_PERIPHERAL
    render_peripheral();
#else
    render_central();
#endif

    lv_obj_invalidate(canvas);
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
ZMK_SUBSCRIPTION(battery_ui, zmk_usb_conn_state_changed);
#endif

#if !BEOPYLAR_PERIPHERAL

struct output_ui_state {
    struct zmk_endpoint_instance endpoint;
    uint8_t profile;
    bool bonded;
    bool connected;
};

static struct output_ui_state output_ui_get_state(const zmk_event_t *eh) {
    return (struct output_ui_state){
        .endpoint = zmk_endpoints_selected(),
        .profile = (uint8_t)(zmk_ble_active_profile_index() + 1),
        .bonded = !zmk_ble_active_profile_is_open(),
        .connected = zmk_ble_active_profile_is_connected(),
    };
}

static void output_ui_update_cb(struct output_ui_state state) {
    shown.usb = state.endpoint.transport == ZMK_TRANSPORT_USB;
    shown.profile = state.profile;
    shown.profile_bonded = state.bonded;
    shown.profile_connected = state.connected;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(output_ui, struct output_ui_state, output_ui_update_cb,
                            output_ui_get_state)

ZMK_SUBSCRIPTION(output_ui, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(output_ui, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(output_ui, zmk_usb_conn_state_changed);
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

struct mods_ui_state {
    zmk_mod_flags_t mods;
};

static struct mods_ui_state mods_ui_get_state(const zmk_event_t *eh) {
    return (struct mods_ui_state){
        .mods = zmk_hid_get_explicit_mods(),
    };
}

static void mods_ui_update_cb(struct mods_ui_state state) {
    shown.mods = state.mods;
    render();
}

ZMK_DISPLAY_WIDGET_LISTENER(mods_ui, struct mods_ui_state, mods_ui_update_cb, mods_ui_get_state)

ZMK_SUBSCRIPTION(mods_ui, zmk_modifiers_state_changed);

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
        memmove(&shown.wpm_history[0], &shown.wpm_history[1], sizeof(shown.wpm_history) - 1);
        shown.wpm_history[WPM_POINTS - 1] = state.wpm;
        shown.wpm = state.wpm;
    }
    render();
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

    canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(canvas, canvas_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Seed the state so the first paint is right before any event arrives. */
    shown.battery = zmk_battery_state_of_charge();
    shown.usb = zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB;
    shown.profile = (uint8_t)(zmk_ble_active_profile_index() + 1);
    shown.profile_bonded = !zmk_ble_active_profile_is_open();
    shown.profile_connected = zmk_ble_active_profile_is_connected();

    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    shown.layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));
    shown.mods = zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_WPM)
    shown.wpm = (uint8_t)zmk_wpm_get_state();
#endif

#if BEOPYLAR_PERIPHERAL
    shown.link = zmk_split_bt_peripheral_is_connected();
#endif

    battery_ui_init();
#if BEOPYLAR_PERIPHERAL
    link_ui_init();
#else
    output_ui_init();
    layer_ui_init();
    mods_ui_init();
#if IS_ENABLED(CONFIG_ZMK_WPM)
    wpm_ui_init();
#endif
#endif

    render();

    return screen;
}
