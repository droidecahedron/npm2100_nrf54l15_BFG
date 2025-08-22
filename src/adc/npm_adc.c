/*
 * npm2100_nrf54l15_BFG
 * saadc.c
 * adc module that samples the npm2100 output regulator voltages
 * auth: ddhd
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "npm_adc.h"

LOG_MODULE_REGISTER(adc, LOG_LEVEL_INF);

#define ADC_THREAD_STACK_SIZE 1024
#define ADC_THREAD_PRIORITY 5
#define ADC_SAMPLE_INTERVAL K_SECONDS(1)

// define variable for each channel, get ADC channel specs from devicetree
#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

// can also use zbus, or pass with work container w/ kernel work item.
// 8 messages, 4-byte alignment
K_MSGQ_DEFINE(adc_msgq, sizeof(struct adc_sample_msg), 8, 4);

// Task dedicated to sampling the ADC
void adc_sample_thread(void)
{
    int err;
    int16_t buf[2];
    struct adc_sequence sequence = {
        .channels = BIT(adc_channels[0].channel_id) | BIT(adc_channels[1].channel_id),
        .buffer = buf,
        .buffer_size = sizeof(buf),
        .resolution = 14,
        .calibrate = true,
    };
    struct adc_sample_msg msg;

    // Setup each channel
    for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++)
    {
        if (!adc_is_ready_dt(&adc_channels[i]))
        {
            LOG_ERR("ADC controller device %s not ready", adc_channels[i].dev->name);
            return;
        }
        err = adc_channel_setup_dt(&adc_channels[i]);
        if (err < 0)
        {
            LOG_ERR("Could not setup channel #%d (%d)", i, err);
            return;
        }
    }

    for (;;)
    {
        err = adc_read(adc_channels->dev, &sequence);
        if (err < 0)
        {
            LOG_ERR("Could not read both channels (%d)", err);
        }
        else
        {
            // Convert raw values to mV for both channels
            for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++)
            {
                int32_t val_mv = (int32_t)buf[i]; // You can check buf[i] for a raw sample.
                err = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
                msg.channel_mv[i] = (err < 0) ? -1 : val_mv;
            }
        }
        LOG_INF("ADC Thread sent: Ch0=%d mV, Ch1=%d mV", msg.channel_mv[0], msg.channel_mv[1]);
        k_msgq_put(&adc_msgq, &msg, K_FOREVER);
        k_sleep(ADC_SAMPLE_INTERVAL);
    }
}

K_THREAD_DEFINE(adc_sample_thread_id, ADC_THREAD_STACK_SIZE, adc_sample_thread, NULL, NULL, NULL, ADC_THREAD_PRIORITY,
                0, 0);