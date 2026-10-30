/*
 * Read the paired host's Bluetooth name.
 *
 * Stock ZMK keeps nothing but the peer address per profile, so we fetch the
 * name ourselves: a few seconds after a host connects (peripheral role) read the
 * Generic Access Device Name (0x2A00) with "Read Using Characteristic UUID",
 * keep it in RAM and persist it per slot.  Best effort throughout - a host that
 * refuses the read simply keeps showing its slot number.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <stdio.h>
#include <string.h>

#include <zmk/ble.h>

#include "host_names.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HOST_SLOTS 5
#define HOST_NAME_LEN 20
/* Give HID the link first: the read must never race the connection set-up. */
#define HOST_READ_DELAY_MS 3000

static char names[HOST_SLOTS][HOST_NAME_LEN];
static struct bt_conn *host_conn;
static struct bt_gatt_read_params read_params;
static struct k_work_delayable read_work;
static struct k_work save_work;
static int8_t save_slot;

const char *host_name_get(uint8_t slot) {
    if (slot < 1 || slot > HOST_SLOTS) {
        return "";
    }

    return names[slot - 1];
}

static void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (save_slot < 1 || save_slot > HOST_SLOTS) {
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), "bph/%d", (int)save_slot);
    settings_save_one(key, names[save_slot - 1], strlen(names[save_slot - 1]) + 1);
}

static uint8_t name_read_cb(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                            const void *data, uint16_t length) {
    ARG_UNUSED(conn);
    ARG_UNUSED(params);

    if (err != 0 || data == NULL || length == 0) {
        LOG_DBG("device name unreadable (err %u)", err);
        return BT_GATT_ITER_STOP;
    }

    int slot = zmk_ble_active_profile_index() + 1;
    if (slot < 1 || slot > HOST_SLOTS) {
        return BT_GATT_ITER_STOP;
    }

    char value[HOST_NAME_LEN];
    size_t n = MIN((size_t)length, sizeof(value) - 1);
    memcpy(value, data, n);
    value[n] = '\0';

    /* Some stacks append the trailing NUL and padding. */
    while (n > 0 && (value[n - 1] == '\0' || value[n - 1] == ' ')) {
        value[--n] = '\0';
    }
    if (n == 0 || strcmp(value, names[slot - 1]) == 0) {
        return BT_GATT_ITER_STOP;
    }

    strncpy(names[slot - 1], value, HOST_NAME_LEN - 1);
    LOG_INF("slot %d is \"%s\"", slot, names[slot - 1]);

    save_slot = (int8_t)slot;
    k_work_submit(&save_work);

    return BT_GATT_ITER_STOP;
}

static void read_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (host_conn == NULL) {
        return;
    }

    memset(&read_params, 0, sizeof(read_params));
    read_params.func = name_read_cb;
    read_params.handle_count = 0; /* 0 selects "by UUID" */
    read_params.by_uuid.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    read_params.by_uuid.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    read_params.by_uuid.uuid = BT_UUID_GAP_DEVICE_NAME;

    int err = bt_gatt_read(host_conn, &read_params);
    if (err != 0) {
        LOG_WRN("device name read could not start (%d)", err);
    }
}

static void host_connected(struct bt_conn *conn, uint8_t err) {
    if (err != 0) {
        return;
    }

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_PERIPHERAL) {
        /* The split link runs in the central role; this is not a host. */
        return;
    }

    if (host_conn != NULL) {
        bt_conn_unref(host_conn);
    }
    host_conn = bt_conn_ref(conn);
    k_work_reschedule(&read_work, K_MSEC(HOST_READ_DELAY_MS));
}

static void host_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);

    if (host_conn == conn) {
        bt_conn_unref(host_conn);
        host_conn = NULL;
    }
    (void)k_work_cancel_delayable(&read_work);
}

BT_CONN_CB_DEFINE(beopylar_host_conn) = {
    .connected = host_connected,
    .disconnected = host_disconnected,
};

static int host_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strncmp(name, "bph/", 4) == 0) {
        name += 4;
    }
    if (name[0] < '1' || name[0] > '0' + HOST_SLOTS) {
        return -ENOENT;
    }

    int slot = name[0] - '0';
    char value[HOST_NAME_LEN] = {0};
    ssize_t rc = read_cb(cb_arg, value, MIN(len, sizeof(value) - 1));
    if (rc < 0) {
        return (int)rc;
    }

    strncpy(names[slot - 1], value, HOST_NAME_LEN - 1);
    return 0;
}

/* Its own tree: "bh" belongs to zmk-module-battery-history. */
SETTINGS_STATIC_HANDLER_DEFINE(beopylar_hosts, "bph", NULL, host_settings_set, NULL, NULL);

static int host_names_init(void) {
    k_work_init_delayable(&read_work, read_work_handler);
    k_work_init(&save_work, save_work_handler);
    return 0;
}

SYS_INIT(host_names_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
