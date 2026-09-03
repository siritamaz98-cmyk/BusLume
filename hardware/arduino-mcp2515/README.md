# BusLume Arduino + MCP2515 prototype interface

This folder documents the early BusLume CAN interface built around an **Arduino Nano**, an **MCP2515 CAN controller** and an **HC-05 Bluetooth module**. It is intended for development, bench testing and early real-vehicle experiments with the BusLume Android application.

> This is a prototype design, not the dedicated ESP32-based BusLume hardware that is being developed separately.

## 🇺🇦 Опис

Цей варіант інтерфейсу створений як простий та доступний спосіб підключити BusLume до автомобільної CAN-шини під час розробки і тестування.

Основні вузли:

- Arduino Nano;
- MCP2515 CAN-модуль;
- HC-05 Bluetooth;
- USB Serial через Arduino Nano;
- вхід автомобільного живлення з окремим понижувальним перетворювачем до 5 В;
- CAN-H / CAN-L;
- зовнішня або модульна термінація CAN 120 Ω залежно від конкретного MCP2515-модуля.

Принципова схема містить лінії SPI між Arduino та MCP2515, Bluetooth UART і окремий вузол живлення. У вузлі живлення використані LMR16006YQDDCRQ1, MF-RX050/72-0, SMBJ33A, MBRS3100T3G, дросель 6.8 µH та вихідна шина 5 В.

## Arduino Nano ↔ MCP2515

| MCP2515 | Arduino Nano | Function |
| --- | --- | --- |
| INT | D2 | CAN interrupt |
| CS | D10 | SPI chip select |
| SI / MOSI | D11 | SPI MOSI |
| SO / MISO | D12 | SPI MISO |
| SCK | D13 | SPI clock |
| VCC | +5V | Module power |
| GND | GND | Ground |
| CAN_HI | CAN-H | CAN bus high |
| CAN_LO | CAN-L | CAN bus low |

The firmware used with this prototype is configured for an **8 MHz MCP2515 oscillator**.

## HC-05 Bluetooth

| HC-05 | Arduino Nano | Notes |
| --- | --- | --- |
| TXD | D3 | Arduino receives Bluetooth data |
| RXD | D4 | Arduino transmits through a voltage divider to HC-05 RXD |
| VCC | +5V | Module supply |
| GND | GND | Common ground |

The BusLume prototype firmware uses **115200 baud** for both USB Serial and HC-05 communication.

## CAN termination

A CAN bus normally requires **120 Ω termination at each physical end of the bus**. Do not add extra 120 Ω resistors if the vehicle network and the connected interface already provide the required termination.

Before connecting to a vehicle, check the resistance between CAN-H and CAN-L with the network powered down. A correctly terminated two-end bus is typically close to 60 Ω because two 120 Ω terminators are in parallel.

## Power section

The prototype schematic includes an automotive-input power stage that feeds the 5 V rail. It contains input protection and a buck converter stage around **LMR16006YQDDCRQ1**.

This section should be treated as a development design and independently checked before use in a vehicle. Automotive power networks can contain reverse polarity, load-dump and transient conditions that are much harsher than a bench power supply.

## Using the interface with BusLume

1. Connect CAN-H and CAN-L to the target CAN network.
2. Make sure the interface and vehicle share ground when required by the test setup.
3. Verify CAN termination before transmitting.
4. Power the interface from USB for bench tests, or from the protected automotive input when that section has been assembled and verified.
5. Connect BusLume through Bluetooth HC-05 or USB Serial.
6. Select the correct CAN bitrate for the vehicle network.
7. Start with monitoring only. Transmit frames only when you understand their function and possible consequences.

## Hardware files

The hardware package prepared from the EasyEDA Pro project contains:

- `gerber/BusLume_Arduino_MCP2515_Gerber.zip` — PCB manufacturing files;
- `schematic/BusLume_Arduino_MCP2515_Schematic.pdf` — schematic PDF;
- `easyeda/BusLume_Arduino_MCP2515_PCB.epro2` — EasyEDA Pro PCB source;
- `easyeda/BusLume_Arduino_MCP2515_Schematic.epro2` — EasyEDA Pro schematic source;
- `SHA256SUMS.txt` — checksums for the supplied files.

## Safety

> **Experimental hardware. Use at your own risk.**

Automotive CAN networks may control safety-critical systems. Incorrect wiring, power supply faults or transmitting incorrect CAN frames can cause communication faults, unexpected vehicle behaviour or hardware damage. Bench testing is strongly recommended before any real-vehicle transmission tests.

---

**BusLume — illuminate the CAN bus.**
