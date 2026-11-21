# Somfy RTS for ESPHome (ESP32 + CC1101)

*[Deutsche Fassung](README.de.md)*

ESPHome external components that control Somfy RTS blinds and awnings with an ESP32 and a CC1101 radio module. No Somfy hub, no cloud.

The ESP32 acts as its own virtual remote **and listens at the same time**: when someone presses an original Somfy remote on the wall, Home Assistant still shows the right state.

The same CC1101 can also switch and receive cheap 433.92 MHz RF sockets and read some weather sensors (component `rf433`, optional).

## Features

**`somfy_rts`**
- Several blinds, each with its own virtual remote
- Up, down, stop and position in percent (estimated from travel time)
- Go to the MY favourite position and save a new one
- Original remotes are recognised and update the position model, including group remotes for several blinds
- Event `esphome.somfy_remote` for every button press of any Somfy remote
- Rolling codes are stored in NVS, with `min_rolling_code` as a safety net

**`rf433`** (optional, uses the same CC1101)
- PT2262 / EV1527 (fixed-code sockets) and AC (KlikAanKlikUit, Intertechno and compatibles): switch and listen
- Hideki temperature/humidity sensors (TFA, Technoline, Cresta, Bresser)
- New sockets and sensors appear in Home Assistant by themselves via MQTT discovery, similar to an RFXtrx
- Oregon Scientific UV sensors UVN800 and UVR128

## Hardware

- ESP32 board, e.g. ESP32 DevKit
- CC1101 module for **433 MHz**
- Somfy **RTS** motors (433.42 MHz)

| CC1101 | ESP32  | Function |
|--------|--------|----------|
| VCC    | 3V3    | **3.3 V only** |
| GND    | GND    | |
| SCK    | GPIO18 | SPI |
| MISO   | GPIO19 | SPI |
| MOSI   | GPIO23 | SPI |
| CSN    | GPIO5  | SPI chip select |
| GDO0   | GPIO4  | transmit |
| GDO2   | GPIO2  | receive |

Use a stable power supply: the ESP draws short current peaks when connecting to Wi-Fi and while transmitting.

A 3D-printable enclosure is in [`enclosure/`](enclosure/) (STL and 3MF).

## Installation

1. Copy [`example/somfy-rts.yaml`](example/somfy-rts.yaml) and [`example/partitions.csv`](example/partitions.csv) into your ESPHome config folder (Home Assistant: `/config/esphome/`).
2. Add the keys from [`example/secrets.example.yaml`](example/secrets.example.yaml) to your `secrets.yaml`.
3. Adapt the blinds (see [Configuration](#configuration)). The components are fetched from this repository:

   ```yaml
   external_components:
     - source: github://XiCode/esphome-somfy-rts@v1.0.0
       components: [somfy_rts, rf433]
   ```

   Pin a version tag so updates only reach your device when you change it. Leave out `rf433` if you only need Somfy.
4. Flash **once via USB** (see next section), afterwards use **Install → Wirelessly**.
5. Add the device in Home Assistant under **Settings → Devices & services**.

### First flash via USB

Somfy motors only accept commands whose rolling code is higher than the last one they saw. The codes live in the ESP's flash (NVS at `0x9000`). If that area is erased, every blind has to be paired again.

- `partitions.csv` keeps the NVS at that address (same layout as the Arduino default, so you can migrate from an Arduino sketch that used `NVSRollingCodeStorage` in namespace `somfy`).
- For a fresh ESP any method works. When **migrating** an ESP that already holds rolling codes, do **not** flash the "factory" image or use ESPHome Web, because they overwrite the NVS. Write only the parts instead. After `esphome compile somfy-rts.yaml` they are in `.esphome/build/somfy-rts/build/` (the list is also in `flash_args` there):

```bash
esptool --port COM7 write-flash 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin 0x10000 somfy-rts.bin
```

On Windows, compile from PowerShell, not Git Bash (ESP-IDF refuses MSYS shells).

## Configuration

```yaml
somfy_rts:
  cs_pin: 5
  gdo0_pin: 4
  gdo2_pin: 2
  frequency: 433.42MHz

cover:
  - platform: somfy_rts
    id: blind1
    name: Blind 1
    address: 0x1A2B01        # virtual remote, pick freely, never change later
    storage_key: remote_1    # NVS key of the rolling code, never change
    min_rolling_code: 1      # safety net in case the NVS is lost
    open_duration: 20s
    close_duration: 18s
    my_position: 50%
    remotes: [0x123ABC]      # original remotes of this blind
```

| Option | Meaning |
|---|---|
| `address` | 24-bit address of the virtual remote, unique per blind |
| `storage_key` | NVS key for the rolling code (max. 15 characters) |
| `min_rolling_code` | Lower bound for the rolling code. Only applies if the stored value is missing or lower |
| `open_duration`, `close_duration` | Travel time fully closed → fully open and back, measure with a stopwatch |
| `my_position` | Initial MY position. Once saved via button or remote, the saved value wins |
| `remotes` | Addresses of the original remotes. Multi-channel remotes (e.g. Telis 4) have one address per channel. An address may appear on several blinds |

Never change `address` or `storage_key` after pairing, otherwise the rolling code no longer matches.

`somfy_rts:` also takes `sck_pin`, `miso_pin`, `mosi_pin`, `rx_frequency`, `rx_bandwidth` and `tx_power`. `tx_power` is capped at 10 dBm, the EU limit for 433 MHz.

> **Renaming:** ESPHome derives an entity's ID from its `name:`. Changing the name creates a **new** entity in Home Assistant. Re-assign the old entity ID afterwards under **Settings → Entities**.

### Button presses as events

```yaml
trigger:
  - platform: event
    event_type: esphome.somfy_remote
    event_data:
      remote: "123ABC"
      command: "UP"
```

Possible `command` values: `UP`, `DOWN`, `MY`, `PROG`, `MY_UP`, `MY_DOWN`, `UP_DOWN`, `SUN_FLAG`, `FLAG`. Enable "Allow the device to perform Home Assistant actions" in the ESPHome integration for the event to arrive.

## RF sockets and remotes (433.92 MHz)

To hear Somfy (433.42 MHz) and these protocols (433.92 MHz) with one receiver, it is tuned wide:

```yaml
somfy_rts:
  frequency: 433.42MHz      # Somfy transmit frequency, unchanged
  rx_frequency: 433.67MHz   # receive in the middle
  rx_bandwidth: 812kHz
```

This costs some range. If Somfy remotes are missed afterwards, reduce `rx_bandwidth` (e.g. `650kHz`) or remove both lines. Transmission always happens on the exact frequency of each protocol.

```yaml
rf433:
  id: rf

switch:
  - platform: rf433
    name: Socket 1
    protocol: pt2262
    on_code: 0x451551
    off_code: 0x451554
  - platform: rf433
    name: Socket 2
    protocol: ac
    address: 0x1A2B3C4
    unit: 3

binary_sensor:
  - platform: rf433
    name: RF button
    protocol: pt2262
    on_code: 0x4515C0
```

| Option | Meaning |
|---|---|
| `protocol` | `pt2262` or `ac` |
| `on_code`, `off_code` | `pt2262` only: 24-bit code for on and off. On a `binary_sensor`, `off_code` may be omitted: it then acts as a push button |
| `address`, `unit` | `ac` only: remote address and channel 1–16, same notation as RFXtrx (`1A2B3C4:3`) |
| `pulse_length` | Base pulse length, default 350 µs (`pt2262`) / 260 µs (`ac`). The value of the original remote is logged on receive (`T = …`) |
| `repeats` | How often a command is sent, default 8 (`pt2262`) / 5 (`ac`) |

**Finding codes:** press a button on the remote and read "Last 433 code", e.g. `pt2262 451551 on` or `ac 1A2B3C4:3 off`. A switch follows its remote. Sockets give no feedback, so the state is assumed.

Every received code also fires `esphome.rf433` with `protocol`, `code` and `state`. A code only counts once it has been received twice in a row, which keeps noise out.

`dump_unknown: true` in `rf433:` logs longer unrecognised pulse trains, handy to check whether a device transmits on 433 MHz at all.

### Auto-discovery via MQTT

ESPHome cannot create entities at runtime. With an MQTT broker (e.g. the Mosquitto add-on), new sockets and sensors still show up on their own:

```yaml
mqtt:
  broker: !secret mqtt_broker
  username: !secret mqtt_username
  password: !secret mqtt_password
  discovery: false      # don't duplicate ESPHome entities via MQTT
  log_topic: null
  reboot_timeout: 0s    # a broker outage must not reboot the blinds controller

rf433:
  id: rf
  mqtt_discovery: true
```

1. Turn on "Auto-add new 433 devices" on the device in Home Assistant.
2. Press a button on the remote. A new device such as "AC 1A2B3C4:3" or "PT2262 451551" appears under **Settings → Devices & services → MQTT**.
3. Rename it, assign a room, and turn auto-add off again, or your neighbours' remotes end up in Home Assistant too.

| Signal | Created as |
|---|---|
| `ac` | Switch, on/off is part of the signal |
| `pt2262`, code ending in `1` or `5` | Switch. The off code is the same code ending in `4` (common DIP-switch sockets). Press the **on** button to add it |
| `pt2262`, other codes | Single button: a "send" button and a "received" event |
| `hideki` temperature/humidity | Device "Hideki channel N" with temperature, humidity and low battery |

The broker remembers the created devices, the ESP stores nothing for them. Deleting a device in Home Assistant removes it; the "Remove auto-added 433 devices" button removes all of them. If the broker is down, these devices can't be switched, the blinds still work. Switches defined in YAML work without MQTT.

### Weather sensors (Hideki)

Hideki temperature/humidity sensors are decoded. The channel is the one set on the sensor, so a battery change keeps the mapping. Values are only available via MQTT discovery; a sensor that is silent for 10 minutes shows as unavailable.

### Oregon Scientific UV sensors

The UV sensors UVN800 and UVR128 (Oregon Scientific v2.1/v3.0) appear via MQTT discovery as "Oregon UV … channel N" with UV index and low battery. Every recognised Oregon packet is logged with ID and raw data. Reports and logs are welcome.

## Operation

**Pairing a new blind**
1. Add a cover entry with a new `address` and `storage_key` and install.
2. Hold PROG on the original remote for about 3 s until the blind jogs.
3. Press "*Blind* – pair with motor" in Home Assistant. The blind confirms with a short jog.
4. Drive fully open or closed once so the position is right.

**Assigning an original remote:** press a button, read the address from "Last remote" and add it to `remotes`.

**MY position:** a short MY on the remote or "go to MY" drives there. A long MY (> 3 s) or "save MY" stores the current position as the new MY.

## Troubleshooting

- **Position is off:** RTS has no feedback, the position is estimated. Driving to an end position corrects it. If it drifts all the time, measure the travel times again.
- **Blind does not react:** the log shows the next rolling code per blind at startup. If the NVS was erased and `min_rolling_code` is too low, pair the blind again.
- **"CC1101 antwortet nicht" (CC1101 not responding) in the log:** check wiring and the 3.3 V supply. The ESP stays reachable via Wi-Fi.
- While transmitting (about 0.8 s per Somfy command, 6 s for "save MY", about 0.4 s per socket command) the ESP does not receive.
- **Socket does not react:** the log shows "Selbsttest Kodierung: ok" (encoder self-test) at startup. Copy the code and `T` of the original remote from the log (`pulse_length`), increase `repeats`.

Log messages and code comments are currently in German.

## License

[MIT](LICENSE). Uses SmartRC-CC1101-Driver-Lib (MIT). The frame format follows Somfy_Remote_Lib (Apache 2.0).

Not an official Somfy product. Somfy is a trademark of Somfy SA.
