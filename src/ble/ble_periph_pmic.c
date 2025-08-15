/*
 * npm2100_nrf54l15_BFG
 * ble_periph_pmic.c
 * ble application, receives pmic information and sends to BLE.
 * auth: ddhd
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <soc.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/types.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#include <zephyr/sys/printk.h>

#include <dk_buttons_and_leds.h>

#include "ble_periph_pmic.h"
#include "npm_adc.h"
#include "threads.h"
#include "tsync.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

#define BLE_STATE_LED DK_LED2

#define BLE_NOTIFY_INTERVAL K_MSEC(500)
#define BLE_THREAD_STACK_SIZE 1024
#define BLE_THREAD_PRIORITY 5

#define BT_UUID_PMIC_HUB BT_UUID_DECLARE_128(PMIC_HUB_SERVICE_UUID)
#define BT_UUID_PMIC_HUB_BOOST_RD_MV BT_UUID_DECLARE_128(BOOST_RD_MV_CHARACTERISTIC_UUID)
#define BT_UUID_PMIC_HUB_LSLDO_RD_MV BT_UUID_DECLARE_128(LSLDO_RD_MV_CHARACTERISTIC_UUID)
#define BT_UUID_PMIC_HUB_LSLDO_WR_MV BT_UUID_DECLARE_128(LSLDO_WR_MV_CHARACTERISTIC_UUID)
#define BT_UUID_PMIC_HUB_BATT_RD BT_UUID_DECLARE_128(BATT_RD_CHARACTERISTIC_UUID)

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME // from prj.conf
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_le_adv_param *adv_param =
    BT_LE_ADV_PARAM((BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY), /* Connectable advertising and use
                                                                          identity address */
                    800,   /* Min Advertising Interval 500ms (800*0.625ms) 16383 max*/
                    801,   /* Max Advertising Interval 500.625ms (801*0.625ms) 16384 max*/
                    NULL); /* Set to NULL for undirected advertising */

static struct k_work adv_work;
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, PMIC_HUB_SERVICE_UUID),
};
/*This function is called whenever the Client Characteristic Control Descriptor
(CCCD) has been changed by the GATT client, for each of the characteristics*/
static void on_cccd_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    switch (value)
    {
    case BT_GATT_CCC_NOTIFY:
        break;
    case 0:
        break;
    default:
        LOG_ERR("Error, CCCD has been set to an invalid value");
    }
}

// fn called when lsldo wr characteristic has been written to by a client
static ssize_t on_receive_lsldo_wr(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
                                   uint16_t offset, uint8_t flags)
{
    const uint8_t *buffer = buf;

    printk("Received lsldo wr data, handle %d, conn %p, data: 0x", attr->handle, conn);
    for (uint8_t i = 0; i < len; i++)
    {
        printk("%02X", buffer[i]);
    }
    printk("\n");

    return len;
}

BT_GATT_SERVICE_DEFINE(
    pmic_hub, BT_GATT_PRIMARY_SERVICE(BT_UUID_PMIC_HUB),

    BT_GATT_CHARACTERISTIC(BT_UUID_PMIC_HUB_BOOST_RD_MV, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, NULL, NULL, NULL),
    BT_GATT_CCC(on_cccd_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(BT_UUID_PMIC_HUB_LSLDO_RD_MV, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, NULL, NULL, NULL),
    BT_GATT_CCC(on_cccd_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(BT_UUID_PMIC_HUB_LSLDO_WR_MV, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, NULL, on_receive_lsldo_wr, NULL),

    BT_GATT_CHARACTERISTIC(BT_UUID_PMIC_HUB_BATT_RD, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, NULL, NULL, NULL),
    BT_GATT_CCC(on_cccd_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

// BT globals and callbacks
struct bt_conn *m_connection_handle = NULL;
static void adv_work_handler(struct k_work *work)
{
    int err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err)
    {
        LOG_INF("Advertising failed to start (err %d)", err);
        return;
    }

    LOG_INF("Advertising successfully started");
}

static void advertising_start(void)
{
    k_work_submit(&adv_work);
}

static void recycled_cb(void)
{
    LOG_INF("Connection object available from previous conn. Disconnect is "
            "complete!");
    advertising_start();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_WRN("Connection failed (err %u)", err);
        return;
    }
    m_connection_handle = conn;
    LOG_INF("Connected");
    dk_set_led_on(BLE_STATE_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason %u)", reason);
    m_connection_handle = NULL;
    dk_set_led_off(BLE_STATE_LED);
}

struct bt_conn_cb connection_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled_cb,
};

static void ble_report_boost_mv(struct bt_conn *conn, const uint32_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[2];
    struct bt_gatt_notify_params params = {
        .uuid = BT_UUID_PMIC_HUB_BOOST_RD_MV, .attr = attr, .data = data, .len = len, .func = NULL};

    if (bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY))
    {
        if (bt_gatt_notify_cb(conn, &params))
        {
            LOG_ERR("Error, unable to send notification");
        }
    }
    else
    {
        LOG_WRN("Warning, notification not enabled for boost mv characteristic");
    }
}

static void ble_report_lsldo_mv(struct bt_conn *conn, const uint32_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[5];

    struct bt_gatt_notify_params params = {
        .uuid = BT_UUID_PMIC_HUB_LSLDO_RD_MV, .attr = attr, .data = data, .len = len, .func = NULL};

    if (bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY))
    {
        // Send the notification
        if (bt_gatt_notify_cb(conn, &params))
        {
            LOG_ERR("Error, unable to send notification");
        }
    }
    else
    {
        LOG_WRN("Warning, notification not enabled for lsldo mv characteristic");
    }
}

static void ble_report_batt_mv(struct bt_conn *conn, const uint32_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[9];
    struct bt_gatt_notify_params params = {
        .uuid = BT_UUID_PMIC_HUB_BATT_RD, .attr = attr, .data = data, .len = len, .func = NULL};

    if (bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY))
    {
        if (bt_gatt_notify_cb(conn, &params))
        {
            LOG_ERR("Error, unable to send notification");
        }
    }
    else
    {
        LOG_WRN("Warning, notification not enabled for batt read characteristic");
    }
}

int bt_init(void)
{
    int err;

    // Setting up Bluetooth
    err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return -1;
    }
    LOG_INF("Bluetooth initialized");
    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        settings_load();
    }
    bt_conn_cb_register(&connection_callbacks);
    k_work_init(&adv_work, adv_work_handler);
    advertising_start();

    return 0;
}

void ble_write_thread(void)
{
    LOG_INF("ble write thread: enter");
    k_sem_take(&sem_gpio_ready, K_FOREVER);
    LOG_INF("ble write thread: woken by main");

    if(bt_init() != 0) 
    {
        LOG_ERR("unable to initialize BLE!");
    }
    struct adc_sample_msg msg;
    int32_t dbg_sim_batv = 0;
    for (;;)
    {
        // Wait indefinitely for a new ADC sample message from the queue
        k_msgq_get(&adc_msgq, &msg, K_FOREVER);
        // At this point, msg.channel_mv[0] and msg.channel_mv[1] contain the
        // latest ADC results
        LOG_INF("BLE thread received: Ch0(BOOST)=%d mV, Ch1(LDOLS)=%d mV", msg.channel_mv[0], msg.channel_mv[1]);

        if (m_connection_handle) // if ble connection present
        {
            ble_report_boost_mv(m_connection_handle, &msg.channel_mv[0], sizeof(msg.channel_mv[0]));
            ble_report_lsldo_mv(m_connection_handle, &msg.channel_mv[1], sizeof(msg.channel_mv[1]));
            ble_report_batt_mv(m_connection_handle,&dbg_sim_batv, sizeof(dbg_sim_batv));
            dbg_sim_batv++;
        }
        else
        {
            LOG_INF("BLE Thread does not detect an active BLE connection");
        }

        k_sleep(BLE_NOTIFY_INTERVAL);
    }
}

K_THREAD_DEFINE(ble_write_thread_id, BLE_THREAD_STACK_SIZE, ble_write_thread, NULL, NULL, NULL, BLE_THREAD_PRIORITY, 0,
                0);
