# Micronova Pellet Stove Controller (ESP8266) — Technical Documentation

> ESP8266 firmware that bridges a Micronova pellet stove and Home Assistant over MQTT: stove control, pellet hopper level measurement, MQTT autodiscovery, web configuration page and OTA updates.

This document describes the firmware **version 0.6** (`botero2_Nov25_hostname.ino`).

## Table of contents

1. [Project overview](#1-project-overview)
2. [Serial communication](#2-serial-communication)
3. [Read and write logic](#3-read-and-write-logic)
4. [Functions and their usage](#4-functions-and-their-usage)
5. [Focus: MQTT autodiscovery](#5-focus-mqtt-autodiscovery)
6. [Appendix: known issues](#6-appendix-known-issues)

---

## 1. Project overview

### Purpose

This firmware turns an ESP8266 into a gateway between a **pellet stove fitted with a Micronova board** and **Home Assistant** (HA). It can:

- read the stove state (off, ignition, heating, alarm…), the ambient temperature, the power level and the temperature setpoint;
- control the stove (on, off, standby, power level, temperature setpoint);
- measure the pellet level in the hopper with a VL53L0X distance sensor;
- declare all its sensors and controls to HA automatically (MQTT discovery);
- be configured and updated without a cable (web page, OTA).

### Architecture

```
 ┌──────────────┐  serial, 1200 baud  ┌─────────────┐   Wi-Fi / MQTT   ┌──────────────┐
 │ Micronova    │◄───────────────────►│  ESP8266    │◄────────────────►│ MQTT broker  │
 │ stove board  │   (optocoupler)     │ (this code) │                  │ + Home       │
 │              │                     │             │   HTTP (80)      │ Assistant    │
 └──────────────┘                     │  VL53L0X ───┤◄────────────────►│ web browser  │
                                      │  (I²C)      │   OTA            └──────────────┘
                                      └─────────────┘
```

### Hardware and pinout

| Function                | Pin | Constant in the code |
|-------------------------|-----|----------------------|
| Stove link, receive     | D3  | `RX_PIN`             |
| Stove link, transmit    | D4  | `TX_PIN`             |
| Receive enable          | D2  | `ENABLE_RX`          |
| I²C SDA (VL53L0X)       | D6  | `SDA`                |
| I²C SCL (VL53L0X)       | D1  | `SCL`                |

### Libraries

`ESP8266WiFi`, `PubSubClient` (MQTT), `SoftwareSerial` (stove link), `Wire` and `Adafruit_VL53L0X` (sensor), `ESP8266WebServer` and `ESP8266HTTPUpdateServer` (web page and browser updates), `ArduinoOTA` (updates from the IDE), `EEPROM` (hostname).

### Files

| File                 | Role                                                           |
|----------------------|----------------------------------------------------------------|
| `*.ino`              | The firmware                                                   |
| `secrets.h`          | Wi-Fi, MQTT and OTA credentials (not versioned)                |
| `secrets.example.h`  | Template for `secrets.h`                                       |

### Runtime cycle

1. **Startup** (`setup`): read the hostname from EEPROM, connect to Wi-Fi, start the web server and OTA, connect to MQTT, publish the autodiscovery messages, subscribe to commands, initialize the VL53L0X sensor.
2. **Loop** (`loop`), continuously: keep the MQTT connection alive, process incoming messages (commands), serve web and OTA requests.
3. **Every 60 seconds**: poll the stove and the sensor (`getStates`), publish a heartbeat (`pong`). After more than 10 failed distance measurements in a row, the ESP restarts.

---

## 2. Serial communication

### Parameters

| Parameter        | Value                                                      |
|------------------|------------------------------------------------------------|
| Baud rate        | 1200                                                       |
| Frame format     | 8 data bits, no parity, **2 stop bits** (`SWSERIAL_8N2`)   |
| Implementation   | `SoftwareSerial` (`StoveSerial`), 256-byte buffer          |
| Pins             | RX = D3, TX = D4                                           |

The ESP8266 has a single hardware serial port, kept for debugging (`Serial`, 115200 baud). The link to the stove therefore uses a software serial port on D3/D4.

### The `ENABLE_RX` pin (D2)

According to the code comments, this pin drives the LED of an optocoupler: when `HIGH`, the LED is off and reception is blocked. It is `HIGH` by default (at startup and after each read). It goes `LOW` right after a read request is sent, to let the stove's answer through, and returns to `HIGH` inside `checkStoveReply()`.

### The protocol

The link is **request / response**: the stove only talks when it is queried.

**Read: 2 bytes sent, 2 bytes received**

```
ESP → stove:  [read code] [address]
stove → ESP:  [checksum]  [value]
```

- The read code is `0x00` for RAM (`_RAMR`) or `0x20` for EEPROM (`_EEPROMR`).
- The checksum of the answer is `code + address + value` (8-bit).

Example, reading the ambient temperature (RAM `0x01`), answer for 19.5 °C (value 39 = `0x27`):

```
ESP → stove:  00 01
stove → ESP:  28 27        (0x28 = 0x00 + 0x01 + 0x27)
```

**Write: 4 bytes sent, no answer read**

```
ESP → stove:  [write code] [address] [value] [checksum]
```

- The write code is `0x80` for RAM (`_RAMW`) or `0xA0` for EEPROM (`_EEPROMW`).
- The checksum is `code + address + value`, truncated to 8 bits (`(char)(…)` in the code).

Example, setting the setpoint to 19 °C. The stove stores the value ×2, so 38 = `0x26` at EEPROM address `0x6B`:

```
ESP → stove:  A0 6B 26 31   (0xA0 + 0x6B + 0x26 = 0x131, truncated to 0x31)
```

The code does not read any acknowledgement after a write. It simply waits 2 seconds (or re-reads the states) to see the result.

### Memory map in use

**RAM (read side)**

| Address | Constant         | Content                                   | Unit / conversion        |
|---------|------------------|-------------------------------------------|--------------------------|
| `0x01`  | `ambTempAddr`    | Ambient temperature                       | value ÷ 2 = °C           |
| `0x03`  | `stovePwrAddr`   | Actual power level                        | 1 = Eco, 2 to 6 = P1 to P5 |
| `0x09`  | `blowTempAddr`   | Blow (outlet air) temperature             | raw                      |
| `0x21`  | `stoveStateAddr` | Stove state (written to switch on, off, standby) | code 0 to 13      |
| `0x37`  | `extractorAddr`  | Extractor speed                           | raw                      |
| `0x59`  | `blowSpeedAddr`  | Blower speed                              | raw                      |
| `0x5C`  | `fumeTempAddr`   | Flue gas temperature                      | raw                      |
| `0x63`  | `visPelletAddr`  | Pellet auger speed                        | raw                      |
| `0x75`  | `fixVacuumAddr`  | Fixed vacuum (draft)                      | raw                      |
| `0x76`  | `readVacuumAddr` | Measured vacuum (draft)                   | raw                      |
| `0x77`  | `multVacuumAddr` | Vacuum multiplier                         | raw                      |
| `0x9A`  | `cardTempAddr`   | Board temperature                         | value ÷ 2 = °C           |

**Stove EEPROM (read and write)**

| Address | Constant      | Content              | Note                          |
|---------|---------------|----------------------|-------------------------------|
| `0x6B`  | `tempSetAddr` | Temperature setpoint | stored value = °C × 2         |
| `0xD0`  | `pwrSetAddr`  | Power setpoint       | 0 to 5                        |

**Stove state codes (RAM `0x21`)**

| Code | State                    | Code | State                      |
|------|--------------------------|------|----------------------------|
| 0    | Off                      | 7    | Heating                    |
| 1    | Initial ventilation      | 8    | Burn pot cleaning          |
| 2    | Resistance preheating    | 9    | Final cleaning             |
| 3    | Pellet preloading        | 10   | Waiting for standby        |
| 4    | Waiting for flame        | 11   | Standby                    |
| 5    | Waiting for ignition     | 12   | Pellet alarm               |
| 6    | Ignition                 | 13   | Alarm (to be investigated) |

### Predefined write frames

The frames are precomputed as constants, so nothing has to be recalculated:

| Constant                         | Frame            | Effect                       |
|----------------------------------|------------------|------------------------------|
| `stoveOn`                        | `80 21 01 A2`    | Switch on                    |
| `stoveStb`                       | `80 21 0B AC`    | Switch to standby            |
| `forceOff`                       | `80 21 00 A1`    | Switch off                   |
| `stovePWA`, `stovePW1`…`stovePW5`| `A0 D0 0n …`     | Set the power level (n = 0 to 5) |

---

## 3. Read and write logic

### 3.1 Reading

Every read follows the same sequence, in the `getXxx()` functions:

```
1. StoveSerial.write(read code)           // 0x00 (RAM) or 0x20 (EEPROM)
2. delay(1)
3. StoveSerial.write(address)
4. digitalWrite(ENABLE_RX, LOW)           // opens reception
5. delay(80)                              // lets the stove answer
6. checkStoveReply()                      // reads and interprets the 2 bytes
```

**`checkStoveReply()`**:

1. Reads at most 2 bytes from the serial buffer (`stoveRxData[0]` = checksum, `[1]` = value).
2. Sets `ENABLE_RX` back to `HIGH`.
3. If exactly 2 bytes were received, it **works out the address** from the answer with `param = checksum − value`. Since the checksum is `code + address + value`, `param` equals `code + address`: the address itself for a RAM read (code `0x00`), or `0x20 + address` for EEPROM.
4. A `switch (param)` routes the value to the right variable, applies the conversion (÷ 2 for temperatures) and **publishes it to MQTT** (retained).

Thanks to this, an answer does not have to be matched with the request that triggered it: it carries its own address.

| Received value (`param`)    | Variable     | MQTT topic published                                |
|-----------------------------|--------------|-----------------------------------------------------|
| `0x01`                      | `ambTemp`    | `micronova/ambtemp` (°C)                            |
| `0x03`                      | `stovePower` | `micronova/stovepower` (Eco, P1…P5)                 |
| `0x21`                      | `stoveState` | `micronova/stovestate` (text) and `micronova/onoff` |
| `0x6B + 0x20` (EEPROM)      | `tempSet`    | `micronova/tempset` (°C), `micronova/settemp` (select state) |
| `0xD0 + 0x20` (EEPROM)      | `pwrSet`     | `micronova/pwrset`                                  |
| `0x09`, `0x5C`, `0x9A`, `0x37`, `0x59`, `0x63`, `0x75`, `0x76`, `0x77` | various | `blowtemp`, `fumetemp`, `cardtemp`, `extractor`, `blowspeed`, `vispellet`, `fixvac`, `readvac`, `multvac` |

**Periodic cycle**: every 60 s, `getStates()` calls `getStovePower`, `getStoveState`, `getAmbTemp`, `getTempSet`, `getPowerSet`, then `getQte`. The other reads (flue gas, blower, auger, vacuum…) exist in the code but are not called automatically.

### 3.2 Writing (commands)

Commands arrive over MQTT on the topic **`micronova/cmd`**. The `callback()` function looks at the first character of the message to choose the action:

| Payload     | Action                    | Detail                                                                  |
|-------------|---------------------------|-------------------------------------------------------------------------|
| `ON`        | Switch on                 | Sends `stoveOn` if the state is 0 (off) or above 10                     |
| `ST`        | Standby                   | Sends `stoveStb` if the state is between 1 and 9                        |
| `OF…`       | Switch off                | Sends `forceOff` whatever the state                                     |
| `P0`…`P5`   | Power level               | Picks the frame `stovePWA`, `stovePW1`…`stovePW5`                       |
| `T<n>`      | Temperature setpoint      | `n` is read with `atof`, rounded, then written as is to EEPROM `0x6B`   |

**Key point about the setpoint**: the value sent in `T<n>` is the value **as the stove stores it**, i.e. the temperature × 2. Choosing 19 °C sends `T38`, 18.5 °C sends `T37`. The multiplication is done by the select's `command_template` in HA (`T{{ ((value | float) * 2) | int }}`). When reading, the firmware does the inverse (÷ 2) before publishing.

After a write, the firmware waits 2 seconds (`ON`, `ST`, `OF`, `P`) or 10 ms (`T`). For `ON`, `ST`, `OF` and `T`, it then calls `getStates()` to republish the real stove state; for `P`, that call is commented out in the code, so the new power level is only read back on the next cycle. For `ON`, `ST` and `OF`, it also immediately publishes a provisional state on `micronova/onoff` (`Marche`, `Attente`, `Eteint`).

### 3.3 Hopper level measurement

`getQte()` reads the VL53L0X, a distance sensor mounted above the pellets:

```
quantity (%) = (hopper_height − distance) / hopper_height × 100
```

- `hauteur_cuve` (hopper height) is 500 mm by default and can be set from the web page (100 to 2000 mm).
- A measurement is only taken **every 11 cycles** (about 11 minutes), using the `measure_sleep` counter.
- A distance above 1000 mm counts as an error (`count_reboot`). More than 10 consecutive failed measurements restart the ESP; a valid measurement resets the counter. An "out of range" sensor status (status 4) is only logged and does not count.
- The distance is published on `micronova/distance` at every measurement. The quantity is only published on `micronova/qte` if it changed by at least 10 points since the last published value.

### 3.4 Local storage (ESP EEPROM)

Only the hostname is kept in EEPROM (64 bytes reserved):

| Byte | Content                                  |
|------|------------------------------------------|
| 0    | Marker `0xA5` (a hostname is stored)     |
| 1    | Hostname length (1 to 32)                |
| 2…   | Hostname characters                      |

Without a valid marker, or if the content is not a valid hostname, the default value `micronova` is used.

---

## 4. Functions and their usage

### Startup and main loop

| Function       | Usage |
|----------------|-------|
| `setup()`      | Initializes the pins and serial ports, loads the hostname, connects to Wi-Fi, starts the web server (routes `/`, `/set`, `/hostname`, `/reboot`, `/update`) and OTA, connects to MQTT, initializes the VL53L0X sensor. |
| `loop()`       | Restores the MQTT connection if needed, processes MQTT (`client.loop()`), web requests (`webServer.handleClient()`) and OTA (`ArduinoOTA.handle()`). Every 60 s: `getStates()`, heartbeat, failure counter check. |

### Network and MQTT

| Function            | Usage |
|---------------------|-------|
| `setup_wifi()`      | Applies the hostname, connects to Wi-Fi (loops until connected), prints the IP and publishes it on the log topic. |
| `reconnect_mqtt()`  | Sets the server and the callback, connects to the broker (retry every 5 s), enlarges the buffer to 1024 bytes, **publishes the autodiscovery messages**, then subscribes to the `cmd` and `hauteur` topics. |
| `callback()`        | Receives MQTT messages and runs the commands (see 3.2). |
| `mqttPublish()`     | Publishes a message with a serial trace and a success indication. |
| `Write_Log()`       | Publishes a message on `micronova/log` (not retained) and prints it on the serial port. |

### Stove communication

| Function                | Usage |
|-------------------------|-------|
| `checkStoveReply()`     | Reads and interprets a stove answer, then publishes the value (see 3.1). |
| `getStoveState()`       | Reads the stove state (RAM `0x21`). |
| `getAmbTemp()`          | Reads the ambient temperature (RAM `0x01`). |
| `getStovePower()`       | Reads the actual power level (RAM `0x03`). |
| `getTempSet()`          | Reads the temperature setpoint (EEPROM `0x6B`). |
| `getPowerSet()`         | Reads the power setpoint (EEPROM `0xD0`). |
| `getFumeTemp()`, `getBlowTemp()`, `getCardTemp()`, `getExtractor()`, `getBlowSpeed()`, `getVisPellet()`, `getFixVacuum()`, `getReadVacuum()`, `getMultVacuum()` | Additional reads (not called by default). |
| `getQte()`              | Measures the distance with the VL53L0X and computes the hopper level (see 3.3). |
| `getStates()`           | Groups the reads of the periodic cycle. |

### Web page

| Function            | Usage |
|---------------------|-------|
| `htmlPage()`        | Builds the page: latest measurements (height, quantity, setpoint, temperature, state, version, hostname), forms for height, hostname and reboot, link to `/update`. |
| `handleRoot()`      | Route `GET /`: sends the page. |
| `handleSet()`       | Route `POST /set`: stores the hopper height (value strictly between 100 and 2000). |
| `handleHostname()`  | Route `POST /hostname`: validates, stores in EEPROM and restarts to apply the new name. |
| `handleReboot()`    | Route `POST /reboot`: restarts the ESP. |
| `httpUpdater`       | Route `/update`: firmware update from the browser, protected by a user name and password. |

### Hostname

| Function              | Usage |
|-----------------------|-------|
| `isValidHostname()`   | Checks 1 to 32 characters: letters, digits and hyphens, no hyphen at the start or the end. |
| `loadHostname()`      | Reads the hostname from EEPROM, or takes the default value. |
| `saveHostname()`      | Writes the hostname to EEPROM. |

---

## 5. Focus: MQTT autodiscovery

### Principle

Instead of declaring every entity by hand in HA, the firmware **publishes the description** of its sensors and controls itself, on special topics. HA watches them (prefix `homeassistant/`) and automatically creates the matching entities, grouped under a single device.

### Discovery topic structure

```
homeassistant/<component>/<node_id>/<object_id>/config
```

Examples from the code:

```
homeassistant/switch/Micronova/Controller/config
homeassistant/sensor/Micronova/Temperature/config
homeassistant/select/micronova/Controller/config
```

The message is a **JSON** document published with the **retain** flag (`retain = true`): the broker keeps it and sends it to HA each time HA starts, even if the ESP is switched off.

### When and how it is published

Publishing happens in `reconnect_mqtt()`, **on every connection to the broker**:

1. Connect to the broker (client id `ESPClient`).
2. **`client.setBufferSize(1024)`**: essential, because PubSubClient's default size (256 bytes) is smaller than the discovery messages, which would otherwise be dropped without any visible error.
3. For each entity: `client.publish(topic, json, true)`, then `client.loop()` and `delay(150)` so neither the broker nor the ESP is overloaded.
4. Subscribe to the command topic.

Republishing on every connection is harmless: the messages are identical to the previous ones and HA recreates nothing.

### The "device" block

Every entity carries the same device description (the `device_information` macro), which groups them in HA:

```json
{
  "manufacturer": "Philibert Cheminot",
  "identifiers": ["42396f39-80d2-493b-8e8e-31a70e700bc6"],
  "model": "Micronova Controller",
  "name": "Micronova Controller",
  "sw_version": "0.6"
}
```

`identifiers` is the key that gathers the entities under one device. `sw_version` takes the value of the `FW_VERSION` constant.

### Declared entities

| Component | Name          | State topic             | Command topic     | Notes |
|-----------|---------------|-------------------------|-------------------|-------|
| `select`  | Temperature   | `micronova/settemp`     | `micronova/cmd`   | Options 17 to 21 in 0.5 steps. `command_template`: `T{{ ((value \| float) * 2) \| int }}` |
| `switch`  | Controller    | `micronova/onoff`       | `micronova/cmd`   | `payload_on` = `ON`, `payload_off` = `OFF` |
| `sensor`  | Controller    | `micronova/pong`        | —                 | Heartbeat ("Connecté") |
| `sensor`  | Temperature   | `micronova/ambtemp`     | —                 | °C, `device_class: temperature` |
| `sensor`  | Consigne      | `micronova/tempset`     | —                 | °C, `device_class: temperature` |
| `sensor`  | State         | `micronova/stovestate`  | —                 | Stove state text |
| `sensor`  | Distance      | `micronova/distance`    | —                 | mm, `device_class: distance` |
| `sensor`  | Quantite      | `micronova/qte`         | —                 | % |
| `sensor`  | log           | `micronova/log`         | —                 | Log messages |
| `sensor`  | hauteur       | `micronova/hauteur`     | —                 | Hopper height |

### Anatomy of a message (example: temperature sensor)

```json
{
  "name": "Temperature",
  "state_topic": "micronova/ambtemp",
  "qos": 0,
  "device_class": "temperature",
  "state_class": "measurement",
  "unit_of_measurement": "°C",
  "icon": "mdi:thermometer",
  "unique_id": "9db3245e-6ace-4d14-ac07-844cd68d245d",
  "device": { "...": "see the device block" }
}
```

- `state_topic`: the topic HA reads the value from.
- `device_class` and `state_class`: let HA handle the unit, the display and long-term statistics.
- **`unique_id`**: the entity's stable identifier. HA uses it to find its `entity_id`, its history and its automations. **It must never change.**

### Special case: the setpoint select

The select works in °C, whereas the stove stores the setpoint ×2:

- **Command**: the `command_template` multiplies the value by 2 before sending it (`19` becomes `T38`).
- **State**: the firmware reads the setpoint back from the stove's EEPROM, then publishes on `micronova/settemp` a text that matches an option **exactly** (`19` or `19.5`). Without that exact match, the select would show "unknown".

The code also contains, commented out, a variant using a `number` entity (slider from 15 to 25 in 0.5 steps). A `number` is a possible alternative to the select if you want to drive the setpoint from a thermostat card.

### Changing or removing an entity

| Situation | What to do |
|-----------|------------|
| Change an option, a name, an icon | Edit the JSON: at the next startup, HA updates the entity (reload the MQTT integration if needed). |
| Change the `unique_id` or the discovery topic | HA will see **a new entity** and keep the old one as "unavailable": avoid this. |
| Remove an entity | Publish an **empty, retained message** on its `…/config` topic (for example with MQTT Explorer), then remove the `publish` call from the code. |

---

## 6. Appendix: known issues

These points come from reading the code. They are not fixed in version 0.6.

1. **Hopper height is not kept.** `hauteur_cuve` goes back to 500 mm at every restart. The MQTT `H` command meant to restore it reads the value without assigning it, and only accepts 3-character messages.
2. **Possible crash on `P`.** A `P` message followed by a character other than 0 to 5 leaves a null pointer (`stovePW`) that is then used for the write.
3. **Name of the "hauteur" sensor.** It is declared with the name `log`, like the log sensor: probably a copy-paste leftover.
4. **Switch states.** The switch expects `ON` / `OFF` on `micronova/onoff` (`state_on` / `state_off`), while the firmware publishes `Marche`, `Eteint`, `Arrêt` or `Attente`. To check: HA may ignore these messages and not reflect the real state.
5. **Blocking waits.** Each read blocks for 80 ms, each command for 2 seconds, the sensor measurement adds 200 ms, and MQTT reconnection waits up to 5 seconds per attempt. During these pauses, the web page, MQTT and OTA do not respond.
6. **Unused reads.** The functions reading flue gas, blower, auger and vacuum values are not called in the periodic cycle.
