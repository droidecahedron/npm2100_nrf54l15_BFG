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
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
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
#include "pmic.h"
#include "threads.h"
#include "tsync.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);
K_MSGQ_DEFINE(ble_cfg_pmic_msgq, sizeof(int32_t), 8, 4);
K_SEM_DEFINE(sem_ble_ready, 0, 1);

#define BLE_STATE_LED DK_LED2

#define BLE_NOTIFY_INTERVAL K_MSEC(1000)
#define BLE_THREAD_STACK_SIZE 1024
#define BLE_THREAD_PRIORITY 5

#define MAXLEN CONFIG_BT_CTLR_DATA_LENGTH_MAX - 4

#define BT_UUID_PMIC_HUB BT_UUID_DECLARE_128(PMIC_HUB_SERVICE_UUID)
#define BT_UUID_PMIC_HUB_RD_ALL BT_UUID_DECLARE_128(PMIC_RD_ALL_CHARACTERISTIC_UUID)
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

void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    double connection_interval = interval * 1.25; // in ms
    uint16_t supervision_timeout = timeout * 10;  // in ms
    LOG_INF("Connection parameters updated: interval %.2f ms, latency %d intervals, timeout %d ms", connection_interval,
            latency, supervision_timeout);
}

void on_le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
    // PHY Updated
    if (param->tx_phy == BT_CONN_LE_TX_POWER_PHY_1M)
    {
        LOG_INF("PHY updated. New PHY: 1M");
    }
    else if (param->tx_phy == BT_CONN_LE_TX_POWER_PHY_2M)
    {
        LOG_INF("PHY updated. New PHY: 2M");
    }
    else if (param->tx_phy == BT_CONN_LE_TX_POWER_PHY_CODED_S8)
    {
        LOG_INF("PHY updated. New PHY: Long Range");
    }
}

void on_le_data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
    uint16_t tx_len = info->tx_max_len;
    uint16_t tx_time = info->tx_max_time;
    uint16_t rx_len = info->rx_max_len;
    uint16_t rx_time = info->rx_max_time;
    LOG_INF("Data length updated. Length %d/%d bytes, time %d/%d us", tx_len, rx_len, tx_time, rx_time);
}

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

    // We can't write values bigger than 3000 mV. Just look at the first two bytes,
    int32_t requested_lsldo_mv = bcd2bin(buffer[0]) * 100 + bcd2bin(buffer[1]);
    if (requested_lsldo_mv < 800 || requested_lsldo_mv > 3000)
    {
        // the devicetree will assert this with regulator-min/max-microvolt but check anyway.
        LOG_ERR("requested lsldo voltage out of bounds (800-3000mv)");
    }
    else
    {
        LOG_INF("REQUESTED LSLDO VOLTAGE (mV): %d", requested_lsldo_mv);
        k_msgq_put(&ble_cfg_pmic_msgq, &requested_lsldo_mv, K_NO_WAIT);
    }

    return len;
}

/*
primary
rd all
rd boost
rd lsldo
wr lsldo
rd batt
*/
BT_GATT_SERVICE_DEFINE(
    pmic_hub, BT_GATT_PRIMARY_SERVICE(BT_UUID_PMIC_HUB),
    BT_GATT_CHARACTERISTIC(BT_UUID_PMIC_HUB_RD_ALL, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, NULL, NULL, NULL),
    BT_GATT_CCC(on_cccd_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
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
static struct bt_gatt_exchange_params exchange_params;
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

static void update_phy(struct bt_conn *conn)
{
    int err;
    const struct bt_conn_le_phy_param preferred_phy = {
        .options = BT_CONN_LE_PHY_OPT_CODED_S8,
        .pref_rx_phy = BT_GAP_LE_PHY_CODED,
        .pref_tx_phy = BT_GAP_LE_PHY_CODED,
    };
    err = bt_conn_le_phy_update(conn, &preferred_phy);
    if (err)
    {
        LOG_ERR("bt_conn_le_phy_update() returned %d", err);
    }
}

static void update_data_length(struct bt_conn *conn)
{
    int err;
    struct bt_conn_le_data_len_param my_data_len = {
        .tx_max_len = CONFIG_BT_CTLR_DATA_LENGTH_MAX,
        .tx_max_time = BT_GAP_DATA_TIME_MAX,
    };
    err = bt_conn_le_data_len_update(m_connection_handle, &my_data_len);
    if (err)
    {
        LOG_ERR("data_len_update failed (err %d)", err);
    }
}

static void exchange_func(struct bt_conn *conn, uint8_t att_err, struct bt_gatt_exchange_params *params)
{
    LOG_INF("MTU exchange %s", att_err == 0 ? "successful" : "failed");
    if (!att_err)
    {
        uint16_t payload_mtu = bt_gatt_get_mtu(conn) - 3; // 3 bytes used for Attribute headers.
        LOG_INF("New MTU: %d bytes", payload_mtu);
    }
}

static void update_mtu(struct bt_conn *conn)
{
    int err;
    exchange_params.func = exchange_func;

    err = bt_gatt_exchange_mtu(conn, &exchange_params);
    if (err)
    {
        LOG_ERR("bt_gatt_exchange_mtu failed (err %d)", err);
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_WRN("Connection failed (err %u)", err);
        return;
    }
    m_connection_handle = bt_conn_ref(conn);
    LOG_INF("Connected");

    struct bt_conn_info info;
    err = bt_conn_get_info(m_connection_handle, &info);
    if (err)
    {
        LOG_ERR("bt_conn_get_info() returned %d", err);
        return;
    }
    double connection_interval = info.le.interval * 1.25; // in ms
    uint16_t supervision_timeout = info.le.timeout * 10;  // in ms
    LOG_INF("Connection parameters: interval %.2f ms, latency %d intervals, timeout %d ms", connection_interval,
            info.le.latency, supervision_timeout);

    update_phy(m_connection_handle);
    k_sleep(K_MSEC(1000)); // Delay added to avoid link layer collisions.
    update_data_length(m_connection_handle);
    update_mtu(m_connection_handle);

    dk_set_led_on(BLE_STATE_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason %u)", reason);
    bt_conn_unref(m_connection_handle);
    m_connection_handle = NULL;
    dk_set_led_off(BLE_STATE_LED);
}

struct bt_conn_cb connection_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled_cb,
    .le_param_updated = on_le_param_updated,
    .le_phy_updated = on_le_phy_updated,
    .le_data_len_updated = on_le_data_len_updated,
};

static void ble_report_pmic_stat(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[2];
    struct bt_gatt_notify_params params = {
        .uuid = BT_UUID_PMIC_HUB_RD_ALL, .attr = attr, .data = data, .len = len, .func = NULL};

    if (bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY))
    {
        if (bt_gatt_notify_cb(conn, &params))
        {
            LOG_ERR("Error, unable to send notification");
        }
    }
    else
    {
        LOG_WRN("Warning, notification not enabled for pmic stat characteristic");
    }
}

static void ble_report_boost_mv(struct bt_conn *conn, const uint32_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[5];
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
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[8];

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

static void ble_report_batt_soc(struct bt_conn *conn, const uint32_t *data, uint16_t len)
{
    const struct bt_gatt_attr *attr = &pmic_hub.attrs[13];
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

    if (bt_init() != 0)
    {
        LOG_ERR("unable to initialize BLE!");
    }
    k_sem_give(&sem_ble_ready);
    struct adc_sample_msg adc_msg;
    struct pmic_report_msg pmic_msg;
    for (;;)
    {
        // Wait indefinitely for msg's from other modules
        k_msgq_get(&adc_msgq, &adc_msg, K_FOREVER);
        // msg.channel_mv[0] and msg.channel_mv[1] contain latest ADC results
        LOG_INF("BLE thread rx from ADC: Ch0(BOOST)=%d mV Ch1(LDOLS)=%d mV", adc_msg.channel_mv[0],
                adc_msg.channel_mv[1]);

        k_msgq_get(&pmic_msgq, &pmic_msg, K_FOREVER);
        LOG_INF("BLE thread rx from PMIC: V: %.2f T: %.2f SoC: %.2f ", pmic_msg.batt_voltage, pmic_msg.temp,
                pmic_msg.batt_soc);

        if (m_connection_handle) // if ble connection present
        {
            ble_report_boost_mv(m_connection_handle, &adc_msg.channel_mv[0], sizeof(adc_msg.channel_mv[0]));
            ble_report_lsldo_mv(m_connection_handle, &adc_msg.channel_mv[1], sizeof(adc_msg.channel_mv[1]));
            uint32_t battcharge = pmic_msg.batt_soc;
            ble_report_batt_soc(m_connection_handle, &battcharge, sizeof(battcharge));
            static uint8_t ble_pmic_stat[MAXLEN]; // string to hold plaintext pmic report
            int len = snprintf(ble_pmic_stat, MAXLEN,
                               "BATT: %.2f%% , BATTV: %.2fV , TEMP: %.2fC  | LDO: %dmV , BOOST: %dmV", pmic_msg.batt_soc,
                               pmic_msg.batt_voltage, pmic_msg.temp, adc_msg.channel_mv[1], adc_msg.channel_mv[0]);
            if (!(len >= 0 && len < MAXLEN))
            {
                LOG_ERR("ble pmic report too large. (%d)", len);
            }
            else
            {
                ble_report_pmic_stat(m_connection_handle, ble_pmic_stat, len);
            }
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
