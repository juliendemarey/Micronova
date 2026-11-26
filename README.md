# Micronova Pellet Stove Controller (ESP8266)

ESP8266 firmware that controls a pellet stove fitted with a Micronova board and exposes it to Home Assistant over MQTT. It also measures the pellet level in the hopper with a distance sensor.

> **Based on [philibertc/micronova_controller](https://github.com/philibertc/micronova_controller)** by Philibert Cheminot, which allows to easily control via MQTT any Micronova-equipped pellet stove (MCZ, Extraflame, Laminox and many other brands). See [Credits](#credits) and [License](#license).

## Features

- Stove control: on, off, standby, power level (P0 to P5) and temperature setpoint in 0.5 °C steps
- Periodic polling (every 60 s) of the stove state, ambient temperature, power level and setpoint
- Hopper level measured with a VL53L0X sensor (distance in mm and remaining quantity in %)
- Home Assistant MQTT discovery: one device with a switch, a setpoint select and several sensors
- Web configuration page: hopper height, hostname, firmware version, reboot
- Default hostname `micronova`, editable from the web page and stored in EEPROM
- OTA updates from the Arduino IDE or through the `/update` page, password protected

## Hardware

- ESP8266 board (NodeMCU / Wemos D1 mini type)
- Pellet stove with a Micronova board (serial link, 1200 baud, 8N2)
- VL53L0X distance sensor (I²C)

| Function             | Pin |
|----------------------|-----|
| Stove link RX        | D3  |
| Stove link TX        | D4  |
| Receive enable       | D2  |
| I²C SDA              | D6  |
| I²C SCL              | D1  |

Wiring diagram: to be added. For the original hardware design and its documentation, see the [upstream project](https://github.com/philibertc/micronova_controller).

## Libraries

- ESP8266 Arduino core (WiFi, WebServer, ArduinoOTA, EEPROM)
- PubSubClient
- EspSoftwareSerial
- Adafruit_VL53L0X

## Setup

1. Copy `secrets.example.h` to `secrets.h` and fill in your Wi-Fi, MQTT broker and OTA credentials. `secrets.h` is ignored by git.
2. Build and flash over USB the first time.
3. Later updates can be done over the air (Arduino IDE network port, or the `/update` page with the exported `.bin` file).

## MQTT commands

Commands are sent to `micronova/cmd`:

| Payload   | Action                                                      |
|-----------|-------------------------------------------------------------|
| `ON`      | Turn on                                                     |
| `OFF`     | Turn off                                                    |
| `ST`      | Switch to standby                                           |
| `P0`…`P5` | Set the power level                                         |
| `T<n>`    | Set the setpoint (n = temperature × 2, e.g. `T38` = 19 °C)  |

States are published under `micronova/` (`ambtemp`, `tempset`, `settemp`, `stovestate`, `onoff`, `distance`, `qte`, `log`, etc.).

## Documentation

Detailed technical documentation (serial protocol, read/write logic, functions, MQTT autodiscovery):

- [English](DOCUMENTATION.en.md)
- [Français](DOCUMENTATION.md)

## Notes

The stove stores the temperature setpoint multiplied by 2 to handle half degrees. Memory addresses on the Micronova board may differ between models: check yours before writing to it.

## Credits

This project is a modified version of [philibertc/micronova_controller](https://github.com/philibertc/micronova_controller) by Philibert Cheminot. Many thanks to him for the original work, which this project builds on.

Changes made in this version include: editable hostname stored in EEPROM, OTA updates (Arduino IDE and web page), firmware version display, half-degree setpoint select with a matching state topic, and credentials moved out of the source into `secrets.h`.

The original project also credits ridiculouslab for its analysis of the Micronova serial protocol and pedrware for the mainboard explanations; see the upstream README for details.

## License

This work is licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License](https://creativecommons.org/licenses/by-nc-sa/4.0/), the same license as the original project it is based on.

The software is provided "as is", without warranty of any kind. The authors accept no liability for any damage resulting from its use, including on your stove.
