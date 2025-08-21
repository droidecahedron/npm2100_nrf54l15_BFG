# npm2100_nrf54l15_BFG

<p align="center">
  <br>
  <img width="45%" height="707" alt="image" src="https://github.com/user-attachments/assets/36d7e173-b39f-4e94-a655-fa172a1f2846" />
  </br>
  This application demonstrates using an nRF54L15 as a Bluetooth Low Energy (BLE) peripheral,  powered by an nPM2100 power management IC (PMIC) and primary cell battery, to be a wireless battery fuel gauge and PMIC controller.
</p>

# Requirements
## Hardware
- **nRF54L15 DK**
 
  <img src="https://github.com/user-attachments/assets/a302e826-4cca-405f-9982-6da59a2ac740" width=15% >
- **nPM2100 EK**

   <img src="https://github.com/user-attachments/assets/b0fb8940-37a9-4051-812e-5c0ff0203e20" width=15% >

- **AA Battery** (other types work as well, see the `Battery Configuration` section) 

- 6x Female-Female jumper wires

## Software
- nRF Connect SDK `V3.0.2` (or grab a tagged release `.hex` and program your DK with nrfutil device or nrf programmer in nrf desktop or jlink)
- nRF Connect for Mobile (Link) for iOS/Android

# Overview
The nRF54L15 is a very low power wireless SoC, and the nPM2100 is an efficient boost regulator for primary-cell batteries.
The nPM2100 also has two output regulators, a boost and LDO/load switch.

The nPM2100EK and primary cell battery provides power (via BOOST) to the nRF54L15DK, and the nRF54L15DK performs measurements and configurations and acts as a BLE peripheral.

The 54L15 uses two on-chip ADC channels and measures the voltage of the BOOST and LDO/LS output of the nPM2100, and reports these statistics to a central device (likely a phone) via BLE. It also reports logs by default, so if you connect a USB cable you can read those logs out.

## Block Diagram
### Overall application
<img width="790" height="463" alt="image" src="https://github.com/user-attachments/assets/e7d47d5b-4196-4515-892e-253b9c534f30" />

### Fuel gauge block diagram
<img width="493" height="153" alt="image" src="https://github.com/user-attachments/assets/311fb2f6-b230-432c-b026-525681e19da5" />

## BLE Data
The regulator output voltages and battery percentage have independent characteristics to read over BLE, but for demonstration purposes there is also an overall encapsulating characeristic to read all the values out as a string. (See the `Example Output` section for more information.) Below is a table summary:

Statistic|Permissions|Unit|Individual Characteristic
---|---|---|----
Battery Percentage|Read|%|Yes
LDO/LS Output Voltage|Read and Write|mV|Yes
BOOST Output Voltage|Read|mV|Yes
Battery Voltage|Read|V|No
Battery Temp|Read|deg C|No

# Setting up 
## Boards

## Battery Configuration

## Building and running

# Example output

# Software Description
