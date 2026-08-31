# BusLume

![Android](https://img.shields.io/badge/Android-In%20Development-3DDC84?logo=android&logoColor=white)
![CAN Bus](https://img.shields.io/badge/CAN-Bus-007ACC)
![ESP32](https://img.shields.io/badge/ESP32-Supported-E7352C?logo=espressif&logoColor=white)
![Arduino](https://img.shields.io/badge/Arduino-Nano-00878F?logo=arduino&logoColor=white)

**BusLume** is an open-source Android application and hardware project for working with automotive CAN bus systems.

The project is under active development and focuses on practical CAN monitoring, transmission, trace analysis, signal research, and community-driven CAN data collection.

## Current Status

BusLume is currently in active development and testing.

Public releases use their own release numbering, independent of internal development, beta, and fix builds.

## Features

- CAN Monitor
- CAN frame transmission
- Multiple CAN ID support
- CAN ID library
- CAN trace recording and playback
- Trace analysis tools
- Bluetooth connectivity
- USB connectivity
- GPS-assisted signal research
- Android interface for real-time CAN work

## 📱 Android App Screenshots

<p align="center">
  <img src="monitor.jpg" width="180" alt="BusLume Monitor">
  <img src="transmitter.jpg" width="180" alt="BusLume Transmitter">
  <img src="trace.jpg" width="180" alt="BusLume Trace">
  <img src="library.jpg" width="180" alt="BusLume Library">
  <img src="system.jpg" width="180" alt="BusLume System">
</p>

<p align="center">
  <b>Monitor</b> • <b>Transmitter</b> • <b>Trace</b> • <b>CAN Library</b> • <b>System</b>
</p>

## Feedback

BusLume is being developed in the open so that real-world testing and community feedback can directly influence the project.

Feedback is welcome on bugs, connectivity, CAN monitoring and transmission, Trace analysis, signal identification, UI/UX, hardware compatibility, and feature requests.

Use **GitHub Issues** for bug reports and feature requests.

## Hardware

Dedicated BusLume CAN hardware is also under development.

The current hardware direction includes ESP32, an automotive CAN transceiver, Bluetooth and USB connectivity, protected automotive power input, switchable CAN termination, and dedicated Android integration.

## Roadmap

Planned development includes improved monitoring and transmission, expanded Trace analysis, an expanded CAN signal library, vehicle selection, easier creation and verification of custom CAN signals, dedicated ESP32 hardware, improved stability and UI/UX, and community-driven CAN data collection.

## Contributing

Testing, bug reports, feature suggestions, UI/UX feedback, CAN research, verified signals, and trace contributions are welcome.

## ⚠️ Safety Warning

**BusLume is experimental software. Use it at your own risk.**

The application can interact with automotive CAN bus systems, including transmitting CAN frames.

Do not transmit CAN frames to a vehicle unless you understand their function and possible consequences.

The developer is not responsible for damage, data loss, vehicle malfunction, or unintended vehicle behavior resulting from the use of this software.

For initial testing, using a bench setup or dedicated test hardware is strongly recommended before connecting the application to a real vehicle.

---

**BusLume — Work in Progress**
