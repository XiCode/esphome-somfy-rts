# Somfy RTS für ESPHome (ESP32 + CC1101)

*[English version](README.md)*

ESPHome-Komponenten, die Somfy-RTS-Rollläden und -Markisen mit einem ESP32 und einem CC1101-Funkmodul steuern. Ohne Somfy-Hub und ohne Cloud.

Der ESP32 funkt als eigene virtuelle Fernbedienung und **hört gleichzeitig mit**. Drückt jemand die Original-Fernbedienung an der Wand, zeigt Home Assistant trotzdem den richtigen Zustand.

Derselbe CC1101 kann außerdem billige Funksteckdosen auf 433,92 MHz schalten und mithören sowie einige Wettersensoren lesen (Komponente `rf433`, optional).

## Funktionen

**`somfy_rts`**
- Mehrere Rollos, jedes mit eigener virtueller Fernbedienung
- Auf, Ab, Stop und Position in Prozent (geschätzt über die Fahrzeit)
- MY-Lieblingsposition anfahren und neu speichern
- Original-Fernbedienungen werden erkannt und ins Positionsmodell übernommen, auch als Gruppe für mehrere Rollos
- Event `esphome.somfy_remote` bei jedem Tastendruck einer Somfy-Fernbedienung
- Rolling Codes im NVS, mit `min_rolling_code` als Sicherheitsnetz

**`rf433`** (optional, nutzt denselben CC1101)
- PT2262 / EV1527 (Steckdosen mit festem Code) und AC (KlikAanKlikUit, Intertechno und Kompatible): schalten und mithören
- Temperatur-/Feuchtesensoren mit Hideki-Protokoll (TFA, Technoline, Cresta, Bresser)
- Neue Steckdosen und Sensoren erscheinen über MQTT-Discovery von selbst in Home Assistant, ähnlich wie beim RFXtrx
- UV-Sensoren UVN800 und UVR128 von Oregon Scientific

## Hardware

- ESP32-Board, z. B. ESP32 DevKit
- CC1101-Modul für **433 MHz**
- Somfy-**RTS**-Motoren (433,42 MHz)

| CC1101 | ESP32  | Funktion |
|--------|--------|----------|
| VCC    | 3V3    | **nur 3,3 V** |
| GND    | GND    | |
| SCK    | GPIO18 | SPI |
| MISO   | GPIO19 | SPI |
| MOSI   | GPIO23 | SPI |
| CSN    | GPIO5  | SPI Chip Select |
| GDO0   | GPIO4  | Senden |
| GDO2   | GPIO2  | Empfangen |

Ein stabiles Netzteil ist wichtig: Beim Verbinden mit dem WLAN und beim Senden zieht der ESP kurz viel Strom.

Ein Gehäuse für den 3D-Druck liegt in [`enclosure/`](enclosure/) (STL und 3MF).

## Einrichten

1. [`example/somfy-rts.yaml`](example/somfy-rts.yaml) und [`example/partitions.csv`](example/partitions.csv) in den ESPHome-Ordner kopieren (Home Assistant: `/config/esphome/`).
2. Die Einträge aus [`example/secrets.example.yaml`](example/secrets.example.yaml) in die eigene `secrets.yaml` übernehmen.
3. Die Rollos anpassen (siehe [Konfiguration](#konfiguration)). Die Komponenten kommen direkt aus diesem Repo:

   ```yaml
   external_components:
     - source: github://XiCode/esphome-somfy-rts@v1.0.0
       components: [somfy_rts, rf433]
   ```

   Eine feste Version angeben, dann kommen Updates nur aufs Gerät, wenn man sie hier ändert. Wer nur Somfy braucht, lässt `rf433` weg.
4. **Einmal per USB** flashen (siehe nächster Abschnitt), danach über **Install → Wirelessly**.
5. Das Gerät in Home Assistant unter **Einstellungen → Geräte & Dienste** hinzufügen.

### Erstes Flashen per USB

Somfy-Motoren nehmen nur Befehle an, deren Rolling Code höher ist als der zuletzt gesehene. Die Codes liegen im Flash des ESP (NVS ab `0x9000`). Wird dieser Bereich gelöscht, müssen alle Rollos neu angelernt werden.

- `partitions.csv` hält den NVS an dieser Stelle (gleiches Layout wie der Arduino-Standard). Ein Umstieg von einem Arduino-Sketch mit `NVSRollingCodeStorage` im Namespace `somfy` ist damit möglich.
- Bei einem neuen ESP ist der Weg egal. Beim **Umstieg** eines ESP, der schon Rolling Codes enthält, **nicht** die „Factory“-Datei bzw. ESPHome Web nehmen, denn die überschreibt den NVS. Stattdessen nur die Einzelteile schreiben. Nach `esphome compile somfy-rts.yaml` liegen sie in `.esphome/build/somfy-rts/build/` (die Liste steht dort auch in `flash_args`):

```bash
esptool --port COM7 write-flash 0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin 0x10000 somfy-rts.bin
```

Unter Windows aus PowerShell kompilieren, nicht aus Git Bash (ESP-IDF lehnt MSYS ab).

## Konfiguration

```yaml
somfy_rts:
  cs_pin: 5
  gdo0_pin: 4
  gdo2_pin: 2
  frequency: 433.42MHz

cover:
  - platform: somfy_rts
    id: rollo1
    name: Rollo 1
    address: 0x1A2B01        # virtuelle Fernbedienung, frei wählen, später nie ändern
    storage_key: remote_1    # Speicherplatz des Rolling Codes, nie ändern
    min_rolling_code: 1      # Sicherheitsnetz, falls der NVS verloren geht
    open_duration: 20s
    close_duration: 18s
    my_position: 50%
    remotes: [0x123ABC]      # Original-Fernbedienungen dieses Rollos
```

| Option | Bedeutung |
|---|---|
| `address` | 24-Bit-Adresse der virtuellen Fernbedienung, je Rollo eindeutig |
| `storage_key` | NVS-Schlüssel für den Rolling Code (max. 15 Zeichen) |
| `min_rolling_code` | Untergrenze für den Rolling Code. Greift nur, wenn der gespeicherte Wert fehlt oder kleiner ist |
| `open_duration`, `close_duration` | Fahrzeit von ganz zu bis ganz auf bzw. umgekehrt, mit der Stoppuhr messen |
| `my_position` | Startwert der MY-Position. Wird sie per Button oder Fernbedienung neu gespeichert, gilt der gespeicherte Wert |
| `remotes` | Adressen der Original-Fernbedienungen. Bei Mehrkanal-Fernbedienungen (z. B. Telis 4) hat jeder Kanal eine eigene Adresse. Eine Adresse darf bei mehreren Rollos stehen |

`address` und `storage_key` nach dem Anlernen **nie mehr ändern**, sonst passt der Rolling Code nicht mehr.

`somfy_rts:` kennt außerdem `sck_pin`, `miso_pin`, `mosi_pin`, `rx_frequency`, `rx_bandwidth` und `tx_power`. `tx_power` ist auf höchstens 10 dBm begrenzt, das ist in der EU die zulässige Grenze für 433 MHz.

> **Achtung beim Umbenennen:** ESPHome leitet die Kennung einer Entität aus ihrem `name:` ab. Ändert man den Namen, legt Home Assistant eine **neue** Entität an. Danach unter **Einstellungen → Entitäten** die alte Entity-ID wieder vergeben.

### Tastendrücke als Event

```yaml
trigger:
  - platform: event
    event_type: esphome.somfy_remote
    event_data:
      remote: "123ABC"
      command: "UP"
```

Mögliche Werte für `command`: `UP`, `DOWN`, `MY`, `PROG`, `MY_UP`, `MY_DOWN`, `UP_DOWN`, `SUN_FLAG`, `FLAG`. Damit das Event ankommt, muss in der ESPHome-Integration „Allow the device to perform Home Assistant actions“ aktiv sein.

## Funksteckdosen und Fernbedienungen (433,92 MHz)

Damit ein Empfänger Somfy (433,42 MHz) und diese Protokolle (433,92 MHz) gleichzeitig hört, wird er breit eingestellt:

```yaml
somfy_rts:
  frequency: 433.42MHz      # Somfy-Sendefrequenz, bleibt
  rx_frequency: 433.67MHz   # Empfang in der Mitte
  rx_bandwidth: 812kHz
```

Das kostet etwas Reichweite. Werden die Somfy-Fernbedienungen danach schlechter erkannt, `rx_bandwidth` verkleinern (z. B. `650kHz`) oder beide Zeilen entfernen. Gesendet wird immer genau auf der Frequenz des jeweiligen Protokolls.

```yaml
rf433:
  id: rf

switch:
  - platform: rf433
    name: Steckdose 1
    protocol: pt2262
    on_code: 0x451551
    off_code: 0x451554
  - platform: rf433
    name: Steckdose 2
    protocol: ac
    address: 0x1A2B3C4
    unit: 3

binary_sensor:
  - platform: rf433
    name: Funktaster
    protocol: pt2262
    on_code: 0x4515C0
```

| Option | Bedeutung |
|---|---|
| `protocol` | `pt2262` oder `ac` |
| `on_code`, `off_code` | Nur `pt2262`: 24-Bit-Code für Ein und Aus. Beim `binary_sensor` darf `off_code` fehlen, dann ist es ein Taster |
| `address`, `unit` | Nur `ac`: Adresse der Fernbedienung und Kanal 1–16, gleiche Schreibweise wie beim RFXtrx (`1A2B3C4:3`) |
| `pulse_length` | Grundpulslänge, ohne Angabe 350 µs (`pt2262`) bzw. 260 µs (`ac`). Der Wert der Original-Fernbedienung steht beim Empfang im Log (`T = …`) |
| `repeats` | Wie oft der Befehl gesendet wird, ohne Angabe 8 (`pt2262`) bzw. 5 (`ac`) |

**Codes herausfinden:** Taste an der Fernbedienung drücken und unter „Last 433 code“ ablesen, z. B. `pt2262 451551 on` oder `ac 1A2B3C4:3 off`. Ein Schalter übernimmt den Zustand, wenn seine Fernbedienung gedrückt wird. Die Steckdosen melden nichts zurück, der Zustand ist angenommen.

Jeder empfangene Code löst außerdem das Event `esphome.rf433` mit `protocol`, `code` und `state` aus. Ein Code zählt erst, wenn er zweimal hintereinander gleich empfangen wurde, so bleibt Rauschen draußen.

`dump_unknown: true` im Block `rf433:` schreibt längere, nicht erkannte Pulsfolgen ins Log. Damit lässt sich prüfen, ob ein Gerät überhaupt auf 433 MHz sendet.

### Geräte automatisch anlegen (MQTT-Discovery)

ESPHome kann zur Laufzeit keine Entitäten anlegen. Mit einem MQTT-Broker (z. B. dem Mosquitto-Add-on) erscheinen neue Steckdosen und Sensoren trotzdem von selbst:

```yaml
mqtt:
  broker: !secret mqtt_broker
  username: !secret mqtt_username
  password: !secret mqtt_password
  discovery: false      # die ESPHome-Entitäten nicht doppelt per MQTT anlegen
  log_topic: null
  reboot_timeout: 0s    # ein Ausfall des Brokers darf die Rollos nicht neu starten

rf433:
  id: rf
  mqtt_discovery: true
```

1. In Home Assistant am Gerät den Schalter „Auto-add new 433 devices“ einschalten.
2. Taste an der Fernbedienung drücken. Unter **Einstellungen → Geräte & Dienste → MQTT** erscheint ein neues Gerät, z. B. „AC 1A2B3C4:3“ oder „PT2262 451551“.
3. Gerät umbenennen, einem Raum zuordnen und das automatische Anlegen wieder ausschalten. Sonst landen auch die Fernbedienungen der Nachbarn in Home Assistant.

| Signal | Wird angelegt als |
|---|---|
| `ac` | Schalter. Ein/Aus steckt im Signal |
| `pt2262`, Code endet auf `1` oder `5` | Schalter. Der Aus-Code ist derselbe Code mit Endung `4` (übliche Steckdosen mit DIP-Schaltern). Zum Anlegen die **Ein**-Taste drücken |
| `pt2262`, anderer Code | Einzelne Taste: ein Button „Senden“ und ein Event „Empfangen“ |
| `hideki`, Temperatur/Feuchte | Gerät „Hideki Kanal N“ mit Temperatur, Luftfeuchtigkeit und Batteriewarnung |

Die angelegten Geräte merkt sich der MQTT-Broker, der ESP speichert dafür nichts. Ein Gerät in Home Assistant löschen entfernt es; der Knopf „Remove auto-added 433 devices“ räumt alle auf einmal ab. Fällt der Broker aus, sind diese Geräte nicht schaltbar, die Rollos schon. Fest in der YAML eingetragene Schalter funktionieren ohne MQTT.

### Wettersensoren (Hideki)

Temperatur-/Feuchtesensoren mit Hideki-Protokoll werden mitgelesen. Der Kanal ist der am Sensor eingestellte, ein Batteriewechsel ändert die Zuordnung nicht. Die Werte gibt es nur über MQTT-Discovery; meldet sich ein Sensor 10 Minuten nicht, zeigt Home Assistant ihn als nicht verfügbar.

### UV-Sensoren (Oregon Scientific)

Die UV-Sensoren UVN800 und UVR128 (Oregon Scientific v2.1/v3.0) erscheinen über MQTT-Discovery als „Oregon UV … Kanal N“ mit UV-Index und Batteriewarnung. Jedes erkannte Oregon-Paket steht mit Kennung und Rohdaten im Log. Rückmeldungen und Logs sind willkommen.

## Bedienung

**Neues Rollo anlernen**
1. Einen Cover-Eintrag mit neuer `address` und neuem `storage_key` anlegen und installieren.
2. An der Original-Fernbedienung PROG etwa 3 s halten, bis das Rollo kurz auf und ab fährt.
3. In Home Assistant „*Rollo* – pair with motor“ drücken. Das Rollo bestätigt mit einer kurzen Fahrt.
4. Einmal ganz auf oder ganz zu fahren, damit die Position stimmt.

**Original-Fernbedienung zuordnen:** Eine Taste drücken, unter „Last remote“ die Adresse ablesen und bei `remotes` eintragen.

**MY-Position:** Kurz MY an der Fernbedienung oder „go to MY“ fährt sie an. Langes Drücken von MY (> 3 s) oder „save MY“ legt die aktuelle Position neu fest.

Die Entitätsnamen stammen aus der Beispiel-YAML und lassen sich dort frei auf Deutsch ändern.

## Fehlersuche

- **Position stimmt nicht:** RTS meldet nichts zurück, die Position ist geschätzt. Eine Fahrt in eine Endlage korrigiert sie. Weichen die Werte ständig ab, die Fahrzeiten neu messen.
- **Rollo reagiert nicht:** Im Log steht beim Start pro Rollo der nächste Rolling Code. Wurde der NVS gelöscht und `min_rolling_code` ist zu niedrig, das Rollo neu anlernen.
- **„CC1101 antwortet nicht“ im Log:** Verkabelung und 3,3-V-Versorgung prüfen. Der ESP bleibt trotzdem im WLAN erreichbar.
- Während der ESP sendet (etwa 0,8 s pro Somfy-Befehl, 6 s bei „MY speichern“, etwa 0,4 s pro Steckdosen-Befehl), empfängt er nicht.
- **Steckdose reagiert nicht:** Im Log steht beim Start „Selbsttest Kodierung: ok“. Den Code und `T` der Original-Fernbedienung aus dem Log übernehmen (`pulse_length`), `repeats` erhöhen.

## Lizenz

[MIT](LICENSE). Nutzt SmartRC-CC1101-Driver-Lib (MIT). Der Frame-Aufbau folgt Somfy_Remote_Lib (Apache 2.0).

Kein offizielles Somfy-Produkt. Somfy ist eine Marke der Somfy SA.
