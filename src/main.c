/*
 * npm2100_nrf54l15_BFG
 * main.c
 * Bluetooth fuel gauge demo application main file
 * auth: ddhd
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>

#include <zephyr/logging/log.h>

#include "threads.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define DK_STATUS_LED DK_LED1

int main(void)
{
    int err;
    int blink = 0;

    err = dk_leds_init();
    if (err)
    {
        LOG_ERR("LEDs init failed (err %d)", err);
        return -1;
    }
    k_wakeup(ble_thread_id);

    for (;;)
    {
        dk_set_led(DK_STATUS_LED, (++blink) % 2);
        k_sleep(K_MSEC(2000));
    }
    return 0;
}