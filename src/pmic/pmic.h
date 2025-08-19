#ifndef PMIC_H_
#define PMIC_H_

#include <stdint.h>

enum battery_type {
	/* Cylindrical non-rechargeable Alkaline AA */
	BATTERY_TYPE_ALKALINE_AA,
	/* Cylindrical non-rechargeable Alkaline AAA */
	BATTERY_TYPE_ALKALINE_AAA,
	/* Cylindrical non-rechargeable Alkaline 2SAA (2 x AA in series) */
	BATTERY_TYPE_ALKALINE_2SAA,
	/* Cylindrical non-rechargeable Alkaline 2SAAA (2 x AAA in series) */
	BATTERY_TYPE_ALKALINE_2SAAA,
	/* Alkaline coin cell LR44 */
	BATTERY_TYPE_ALKALINE_LR44,
	/* Lithium-manganese dioxide coin cell CR2032 */
	BATTERY_TYPE_LITHIUM_CR2032,
};

struct pmic_report_msg
{
    int32_t batt_voltage;
	int32_t temp;
	int32_t batt_soc;
};

extern struct k_msgq ble_cfg_pmic_msgq;

#endif