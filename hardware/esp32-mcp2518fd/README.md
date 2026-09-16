# BusLume ESP32 + MCP2518FD standalone CAN controller

This directory contains the tested BusLume firmware and wiring documentation for the standalone ESP32 CAN/CAN FD prototype.

The device can work autonomously from its OLED and joystick, or as a USB/Bluetooth SLCAN-compatible adapter for BusLume and compatible host software.

## Files

- [`BusLume_MCP2518FD_Full.ino`](BusLume_MCP2518FD_Full.ino) - full working ESP32 firmware.
- [`docs/BusLume_Wiring_and_Net_Labels_EN.pdf`](docs/BusLume_Wiring_and_Net_Labels_EN.pdf) - illustrated English wiring guide, schematic and exact net-label tables.
- [`docs/BusLume_Schematic.pdf`](docs/BusLume_Schematic.pdf) - original EasyEDA schematic export.

## Hardware

- ESP32 DevKit V1, 30-pin, ESP32-WROOM-32.
- MCP2518FD SPI CAN FD controller module with a 20 MHz oscillator and onboard CAN transceiver.
- 128x64 I2C OLED supported by U8g2.
- Bare PS2 analog joystick mechanism with push switch.
- Two normally open push buttons.

The MCP2518FD module shown in the documentation already has a green CAN terminal and an onboard 120-ohm termination option. Do not add a second CAN terminal or a second termination resistor unless your own module differs.

## Wiring and net labels

| ESP32 pin | Net label | MCP2518FD / peripheral |
|---|---|---|
| GPIO18 | `SPI_SCK` | MCP2518FD `SCK` |
| GPIO23 | `SPI_MOSI` | MCP2518FD `SDI` |
| GPIO19 | `SPI_MISO` | MCP2518FD `SDO` |
| GPIO5 | `CAN_CS` | MCP2518FD `nCS` |
| GPIO27 | `CAN_INT` | MCP2518FD `INT` |
| GPIO21 | `I2C_SDA` | OLED `SDA` |
| GPIO22 | `I2C_SCL` | OLED `SCL` |
| GPIO34 | `JOY_X` | joystick X-axis wiper |
| GPIO35 | `JOY_Y` | joystick Y-axis wiper |
| GPIO32 | `JOY_SW` | joystick switch to GND |
| GPIO25 | `BTN_BACK` | Back button to GND |
| GPIO26 | `BTN_SEND` | periodic Send All button to GND |
| 3V3 | `+3V3` | logic, OLED and joystick supply |
| 5V / VIN | `+5V` | MCP2518FD module 5 V input |
| GND | `GND` | common ground |

Follow the labels printed on the actual MCP2518FD module. Header order and pin numbering can differ between sellers. The PDF is the authoritative illustrated reference for this tested assembly.

## Firmware features

- Classical CAN 2.0A/2.0B operation through the MCP2518FD.
- Selectable arbitration rates from 5 kbit/s to 1 Mbit/s.
- USB Serial at 115200 baud.
- Bluetooth Classic SPP, device name `BusLume-ESP32`.
- Lawicel/SLCAN-compatible host commands.
- Offline OLED menus in the order Monitor, Transmitter and Saved.
- Single-shot transmission and periodic transmission from the current editor frame.
- Persistent saved-frame list in ESP32 Preferences.
- Receive monitor, transmission counters and error recovery.
- Host connection screen and local-control lock while USB or Bluetooth is active.

## Local controls

### Transmitter

- Joystick left/right: select ID, DLC, DATA, period or bitrate.
- Joystick up/down: change the selected value; stronger Y-axis deflection repeats faster.
- Short joystick press: send the current frame once.
- Long joystick press: save the current frame; the popup appears after the save completes.
- GPIO26 button: start or stop periodic transmission of the current editor frame.
- While periodic transmission is active, editing is locked to period and bitrate to prevent accidental ID/DATA changes.

### Saved

- Joystick up/down: choose a saved frame.
- Short joystick press: load the selected frame into Transmitter.
- Long joystick press: delete the selected frame and show the trash popup.

### Navigation

- GPIO25 button: Back.

## Required Arduino libraries

- [ACAN2517FD](https://github.com/pierremolinaro/ACAN2517FD)
- [U8g2](https://github.com/olikraus/u8g2)

`BluetoothSerial`, `Preferences`, `Wire` and `SPI` are supplied by the ESP32 Arduino core.

## Build and upload

1. Install the ESP32 board package in Arduino IDE.
2. Install ACAN2517FD and U8g2 through Library Manager or from the links above.
3. Open `BusLume_MCP2518FD_Full.ino`.
4. Select **ESP32 Dev Module**.
5. Upload the sketch and open Serial Monitor at **115200 baud**.

The tested module uses a 20 MHz oscillator:

```cpp
static constexpr auto MCP2518_OSC = ACAN2517FDSettings::OSC_20MHz;
```

If your module has a different oscillator marking, select the corresponding ACAN2517FD setting before use.

## CAN-bus connection

Connect CANH, CANL and GND to the other CAN node. All nodes must use the same bitrate. Fit 120-ohm termination only at the two physical ends of the bus.

For first power-up, test on a bench CAN network. Incorrect frames sent to a vehicle can cause faults, unexpected operation or damage.

## Project status

This is the working prototype firmware verified with autonomous OLED operation and with BusLume over USB and Bluetooth. It is published as a reproducible development reference; module revisions and footprints must still be checked against the actual hardware before PCB fabrication.
