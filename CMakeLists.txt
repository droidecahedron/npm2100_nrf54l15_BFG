cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(npm2100_nrf54l15_BFG)

zephyr_include_directories(src/common src/ble src/pmic src/adc) # (inc .)

# NORDIC SDK APP START
target_sources(app PRIVATE
	src/main.c
	src/ble/ble_periph_pmic.c
	src/pmic/pmic.c
	src/adc/npm_adc.c
)
