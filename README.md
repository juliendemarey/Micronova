# Micronova Controller (ESP8266)

Firmware for an ESP8266 that controls a pellet stove fitted with a Micronova
board and exposes it to Home Assistant over MQTT. It also measures the
pellet level in the hopper with a distance sensor.

## Features

- Stove control: on, off, standby, power level (P0 to P5) and temperature
  setpoint in 0.5 °C steps
- Periodic polling (every 60 s) of the stove state, ambient temperature,
  power level and setpoint
- Hopper level measured with a VL53L0X sensor (distance in mm and
  remaining quantity in %)
- Home Assistant MQTT discovery: one device with a switch, a setpoint
  select and several sensors
- Web configuration page: hopper height, hostname, reboot
- Default hostname `micronova`, editable from the web page and stored
  in EEPROM
- OTA updates from the Arduino IDE or through the `/update` page,
  password protected

## Hardware

- ESP8266 board (NodeMCU / Wemos D1 mini type)
- Pellet stove with a Micronova board (serial link, 1200 baud, 8N2)
- VL53L0X distance sensor (I²C)

| Function            | Pin |
|---------------------|-----|
| Stove link RX       | D3  |
| Stove link TX       | D4  |
| Receive enable      | D2  |
| I²C SDA             | D6  |
| I²C SCL             | D1  |

Wiring diagram: to be added.

## Libraries

- ESP8266 Arduino core (WiFi, WebServer, ArduinoOTA, EEPROM)
- PubSubClient
- EspSoftwareSerial
- Adafruit_VL53L0X

## Setup

1. Copy `secrets.example.h` to `secrets.h` and fill in your Wi-Fi, MQTT
   broker and OTA credentials.
2. Build and flash over USB the first time.
3. Later updates can be done over the air.

## MQTT commands

Commands are sent to `micronova/cmd`:

| Payload   | Action                                                    |
|-----------|-----------------------------------------------------------|
| `ON`      | Turn on                                                   |
| `OFF`     | Turn off                                                  |
| `ST`      | Switch to standby                                         |
| `P0`…`P5` | Set the power level                                       |
| `T<n>`    | Set the setpoint (n = temperature × 2, e.g. `T38` = 19 °C) |

States are published under `micronova/` (`ambtemp`, `tempset`,
`stovestate`, `onoff`, `distance`, `qte`, `log`, etc.).

## Notes

The stove stores the temperature setpoint multiplied by 2 to handle half
degrees. Memory addresses on the Micronova board may differ between
models: check yours before writing to it.

## License

To be chosen.
