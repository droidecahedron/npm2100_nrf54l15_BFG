/*
 * npm2100_nrf54l15_BFG
 * ble_periph_pmic.c
 * fuel gauging, for ref see npm2100_fuel_gauge
 * auth: ddhd
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm2100_vbat.h>
#include <zephyr/drivers/mfd/npm2100.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/util.h>

#include "pmic.h"

#include <nrf_fuel_gauge.h>

#define PMIC_THREAD_STACK_SIZE 1024
#define PMIC_THREAD_PRIORITY 5
#define PMIC_SLEEP_INTERVAL_MS 1000

LOG_MODULE_REGISTER(pmic, LOG_LEVEL_INF);

K_MSGQ_DEFINE(pmic_msgq, sizeof(struct pmic_report_msg), 8, 4);

static const struct device *regulators = DEVICE_DT_GET(DT_NODELABEL(npm2100ek_regulators));

static const struct device *vbat = DEVICE_DT_GET(DT_NODELABEL(npm2100ek_vbat));
static enum battery_type battery_model;
static bool fuel_gauge_initialized;

static const char *const battery_model_str[] = {
    [BATTERY_TYPE_ALKALINE_AA] = "Alkaline AA",     [BATTERY_TYPE_ALKALINE_AAA] = "Alkaline AAA",
    [BATTERY_TYPE_ALKALINE_2SAA] = "Alkaline 2SAA", [BATTERY_TYPE_ALKALINE_2SAAA] = "Alkaline 2SAAA",
    [BATTERY_TYPE_ALKALINE_LR44] = "Alkaline LR44", [BATTERY_TYPE_LITHIUM_CR2032] = "Lithium CR2032"};

    static int64_t ref_time;

static const struct battery_model_primary battery_models[] = {
	[BATTERY_TYPE_ALKALINE_AA] = {
		#include <battery_models/primary_cell/AA_Alkaline.inc>
	},
	[BATTERY_TYPE_ALKALINE_AAA] = {
		#include <battery_models/primary_cell/AAA_Alkaline.inc>
	},
	[BATTERY_TYPE_ALKALINE_2SAA] = {
		#include <battery_models/primary_cell/2SAA_Alkaline.inc>
	},
	[BATTERY_TYPE_ALKALINE_2SAAA] = {
		#include <battery_models/primary_cell/2SAAA_Alkaline.inc>
	},
	[BATTERY_TYPE_ALKALINE_LR44] = {
		#include <battery_models/primary_cell/LR44.inc>
	},
	[BATTERY_TYPE_LITHIUM_CR2032] = {
		#include <battery_models/primary_cell/CR2032.inc>
	},
};

/* Basic assumption of average battery current.
 * Using a non-zero value improves the fuel gauge accuracy, even if the number is not exact.
 */
static const float battery_current[] = {
	[BATTERY_TYPE_ALKALINE_AA] = 5e-3f,
	[BATTERY_TYPE_ALKALINE_AAA] = 5e-3f,
	[BATTERY_TYPE_ALKALINE_2SAA] = 5e-3f,
	[BATTERY_TYPE_ALKALINE_2SAAA] = 5e-3f,
	[BATTERY_TYPE_ALKALINE_LR44] = 1.5e-3f,
	[BATTERY_TYPE_LITHIUM_CR2032] = 1.5e-3f,
};

static enum battery_type selected_battery_model;

static int read_sensors(const struct device *vbat, float *voltage, float *temp)
{
	struct sensor_value value;
	int ret;

	ret = sensor_sample_fetch(vbat);
	if (ret < 0) {
		return ret;
	}

	sensor_channel_get(vbat, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	*voltage = (float)value.val1 + ((float)value.val2 / 1000000);

	sensor_channel_get(vbat, SENSOR_CHAN_DIE_TEMP, &value);
	*temp = (float)value.val1 + ((float)value.val2 / 1000000);

	return 0;
}

int fuel_gauge_init(const struct device *vbat, enum battery_type battery)
{
	struct nrf_fuel_gauge_init_parameters parameters = {
		.model_primary = &battery_models[battery],
		.i0 = 0.0f,
		.opt_params = NULL,
	};
	int ret;

	LOG_INF("nRF Fuel Gauge version: %s\n", nrf_fuel_gauge_version);

	ret = read_sensors(vbat, &parameters.v0, &parameters.t0);
	if (ret < 0) {
		return ret;
	}

	ret = nrf_fuel_gauge_init(&parameters, NULL);
	if (ret < 0) {
		return ret;
	}

	ref_time = k_uptime_get();
	selected_battery_model = battery;

	return 0;
}

int fuel_gauge_update(const struct device *vbat)
{
	float voltage;
	float temp;
	float soc;
	float delta;
	int ret;
    struct pmic_report_msg pmic_ble_report;

	ret = read_sensors(vbat, &voltage, &temp);
	if (ret < 0) {
		LOG_INF("Error: Could not read from vbat device\n");
		return ret;
	}

	delta = (float)k_uptime_delta(&ref_time) / 1000.f;

	soc = nrf_fuel_gauge_process(
		voltage, battery_current[selected_battery_model], temp, delta, NULL);
	
    LOG_INF("PMIC Thread sending: V: %.3f, T: %.2f, SoC: %.2f", (double)voltage, (double)temp, (double)soc);
    pmic_ble_report.batt_voltage = voltage;
    pmic_ble_report.temp = temp;
    pmic_ble_report.batt_soc = soc;
    k_msgq_put(&pmic_msgq, &pmic_ble_report, K_FOREVER);

	return 0;
}

int pmic_fg_thread(void)
{
    if (IS_ENABLED(CONFIG_BATTERY_MODEL_ALKALINE_AA))
    {
        battery_model = BATTERY_TYPE_ALKALINE_AA;
    }
    else if (IS_ENABLED(CONFIG_BATTERY_MODEL_ALKALINE_AAA))
    {
        battery_model = BATTERY_TYPE_ALKALINE_AAA;
    }
    else if (IS_ENABLED(CONFIG_BATTERY_MODEL_ALKALINE_2SAA))
    {
        battery_model = BATTERY_TYPE_ALKALINE_2SAA;
    }
    else if (IS_ENABLED(CONFIG_BATTERY_MODEL_ALKALINE_2SAAA))
    {
        battery_model = BATTERY_TYPE_ALKALINE_2SAAA;
    }
    else if (IS_ENABLED(CONFIG_BATTERY_MODEL_ALKALINE_LR44))
    {
        battery_model = BATTERY_TYPE_ALKALINE_LR44;
    }
    else if (IS_ENABLED(CONFIG_BATTERY_MODEL_LITHIUM_CR2032))
    {
        battery_model = BATTERY_TYPE_LITHIUM_CR2032;
    }
    else
    {
        LOG_INF("Configuration error: no battery model selected.");
        return 0;
    }

    fuel_gauge_initialized = false;

    if (!device_is_ready(vbat))
    {
        LOG_INF("vbat device not ready.");
        return 0;
    }
    LOG_INF("PMIC device ok");

    for (;;)
    {
        if (!fuel_gauge_initialized)
        {
            int err;

            err = fuel_gauge_init(vbat, battery_model);
            if (err < 0)
            {
                LOG_INF("Could not initialise fuel gauge.");
                return 0;
            }
            LOG_INF("Fuel gauge initialised for %s battery.", battery_model_str[battery_model]);

            fuel_gauge_initialized = true;
        }
        fuel_gauge_update(vbat);
        k_msleep(PMIC_SLEEP_INTERVAL_MS);
    }
}

//wait forever 
int pmic_reg_thread(void)
{
    int err, requested_lsldo_uv;
    int requested_lsldo_mv = -1;
    for(;;)
    {
        k_msgq_get(&pmic_msgq, &requested_lsldo_mv, K_FOREVER); // suspend till msg avail
        requested_lsldo_uv = requested_lsldo_mv*1000;  // api wants uV
        err = regulator_set_voltage(regulators, requested_lsldo_uv, requested_lsldo_uv);
        if(err)
        {
            LOG_ERR("Failed to set regulator voltage: %d uV", requested_lsldo_uv);
        }
        else
        {
            LOG_INF("LSLDO Voltage set to: %d uV", requested_lsldo_uv);
        }
    }
}

K_THREAD_DEFINE(pmic_reg_thread_id, PMIC_THREAD_STACK_SIZE, pmic_reg_thread, NULL, NULL, NULL, PMIC_THREAD_PRIORITY, 0,
                0);
K_THREAD_DEFINE(pmic_fg_thread_id, PMIC_THREAD_STACK_SIZE, pmic_fg_thread, NULL, NULL, NULL, PMIC_THREAD_PRIORITY, 0,
                0);