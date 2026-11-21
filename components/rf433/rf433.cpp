#include "rf433.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome {
namespace rf433 {

static const char *const TAG = "rf433";

static const uint16_t PT2262_PULSE = 350;
static const int PT2262_REPEATS = 8;
static const uint16_t AC_PULSE = 260;
static const int AC_REPEATS = 5;

// Gleicher Code innerhalb dieser Zeit = Wiederholung desselben Tastendrucks
static const uint32_t REPEAT_WINDOW_MS = 600;

const char *protocol_name(Protocol protocol) { return protocol == Protocol::AC ? "ac" : "pt2262"; }

std::string code_string(const Message &msg) {
  char buf[16];
  if (msg.protocol == Protocol::PT2262) {
    snprintf(buf, sizeof(buf), "%06X", (unsigned) msg.code);
  } else if (msg.group) {
    snprintf(buf, sizeof(buf), "%07X:G", (unsigned) msg.code);
  } else {
    snprintf(buf, sizeof(buf), "%07X:%u", (unsigned) msg.code, msg.unit);
  }
  return buf;
}

// --------------------------------------------------
// Setup
// --------------------------------------------------

void Rf433Component::setup() {
  this->self_test_ok_ = this->self_test_();
  if (!this->self_test_ok_)
    ESP_LOGE(TAG, "Selbsttest der Kodierung fehlgeschlagen");
  this->hub_->add_pulse_listener([this](bool level, uint16_t duration) { this->on_pulse_(level, duration); });
  if (this->mqtt_discovery_)
    this->mqtt_setup_();
}

void Rf433Component::dump_config() {
  ESP_LOGCONFIG(TAG, "RF433 (PT2262, AC):");
  ESP_LOGCONFIG(TAG, "  Sendefrequenz: %.2f MHz", this->frequency_);
  ESP_LOGCONFIG(TAG, "  Selbsttest Kodierung: %s", this->self_test_ok_ ? "ok" : "FEHLER");
  ESP_LOGCONFIG(TAG, "  Unbekannte Signale ausgeben: %s", YESNO(this->dump_unknown_));
  ESP_LOGCONFIG(TAG, "  MQTT-Discovery: %s, neue Geraete anlegen: %s, bekannte Eintraege: %u",
                YESNO(this->mqtt_discovery_), YESNO(this->auto_add_), (unsigned) this->known_.size());
}

// Kodiert je einen Frame und schickt ihn durch den Dekoder
bool Rf433Component::self_test_() {
  bool ok = true;
  Message msg;

  {
    std::vector<uint16_t> pulses;
    encode_pt2262(0, PT2262_PULSE, pulses);  // liefert den Sync vor dem eigentlichen Frame
    encode_pt2262(0xEE6104, PT2262_PULSE, pulses);
    Pt2262Decoder decoder;
    bool found = false;
    bool level = true;
    for (uint16_t duration : pulses) {
      if (decoder.feed(level, duration, msg) && msg.code == 0xEE6104)
        found = true;
      level = !level;
    }
    ok = ok && found;
  }

  {
    std::vector<uint16_t> pulses;
    encode_ac(0x1A2B3C4, 3, false, true, AC_PULSE, pulses);
    AcDecoder decoder;
    bool found = false;
    bool level = true;
    for (uint16_t duration : pulses) {
      if (decoder.feed(level, duration, msg) && msg.code == 0x1A2B3C4 && msg.unit == 3 && msg.on && !msg.group)
        found = true;
      level = !level;
    }
    ok = ok && found;
  }
  return ok;
}

// --------------------------------------------------
// Senden
// --------------------------------------------------

void Rf433Component::send_pt2262(uint32_t code, uint16_t pulse, int repeats) {
  if (pulse == 0)
    pulse = PT2262_PULSE;
  if (repeats <= 0)
    repeats = PT2262_REPEATS;
  std::vector<uint16_t> pulses;
  encode_pt2262(code, pulse, pulses);
  ESP_LOGD(TAG, "Sende pt2262 %06X (T = %u µs, %dx)", (unsigned) code, pulse, repeats);
  this->hub_->transmit_ook(this->frequency_, pulses, repeats);
}

void Rf433Component::send_ac(uint32_t address, uint8_t unit, bool group, bool on, uint16_t pulse, int repeats) {
  if (pulse == 0)
    pulse = AC_PULSE;
  if (repeats <= 0)
    repeats = AC_REPEATS;
  std::vector<uint16_t> pulses;
  encode_ac(address, unit, group, on, pulse, pulses);
  ESP_LOGD(TAG, "Sende ac %07X:%u %s (T = %u µs, %dx)", (unsigned) address, unit, ONOFF(on), pulse, repeats);
  this->hub_->transmit_ook(this->frequency_, pulses, repeats);
}

// --------------------------------------------------
// Empfang
// --------------------------------------------------

void Rf433Component::on_pulse_(bool level, uint16_t duration) {
  Message msg;
  if (this->pt2262_.feed(level, duration, msg))
    this->on_decoded_(msg);
  if (this->ac_.feed(level, duration, msg))
    this->on_decoded_(msg);
  HidekiPacket packet;
  if (this->hideki_.feed(level, duration, packet))
    this->on_hideki_(packet);
  OregonPacket oregon;
  if (this->oregon_.feed(level, duration, oregon))
    this->on_oregon_(oregon);
  if (this->dump_unknown_)
    this->track_unknown_(level, duration);
}

void Rf433Component::on_decoded_(const Message &msg) {
  uint32_t now = millis();
  this->burst_decoded_ = true;

  if (this->last_valid_ && msg.same_as(this->last_) && now - this->last_time_ < REPEAT_WINDOW_MS) {
    this->last_time_ = now;
    if (this->last_reported_)
      return;
    this->last_reported_ = true;
  } else {
    // Erster Frame: noch nicht melden, Rauschen ergibt selten zweimal denselben Code
    this->last_ = msg;
    this->last_time_ = now;
    this->last_valid_ = true;
    this->last_reported_ = false;
    return;
  }

  std::string code = code_string(msg);
  if (msg.protocol == Protocol::AC) {
    ESP_LOGI(TAG, "Empfangen: ac %s %s (T = %u µs)", code.c_str(), ONOFF(msg.on), msg.pulse);
  } else {
    ESP_LOGI(TAG, "Empfangen: pt2262 %s (T = %u µs)", code.c_str(), msg.pulse);
  }
  this->receive_callback_.call(protocol_name(msg.protocol), code, msg.protocol == Protocol::AC ? msg.on : true);
  for (auto *listener : this->listeners_)
    listener->on_rf433_message(msg);
  if (this->mqtt_discovery_)
    this->mqtt_on_message_(msg);
}

// Wettersensor. Jede Aussendung enthaelt dasselbe Paket mehrfach.
void Rf433Component::on_hideki_(const HidekiPacket &packet) {
  uint32_t now = millis();
  this->burst_decoded_ = true;
  // Die Wiederholungen unterscheiden sich nur im Zaehler (Byte 3) und den Pruefsummen
  if (packet.xor_ok && packet.crc_ok && packet.data[1] == this->last_hideki_.data[1] &&
      now - this->last_hideki_time_ < 2000)
    return;
  if (packet.xor_ok && packet.crc_ok) {
    this->last_hideki_ = packet;
    this->last_hideki_time_ = now;
  }

  std::string hex;
  char buf[4];
  for (uint8_t i = 0; i < packet.length; i++) {
    snprintf(buf, sizeof(buf), "%02X ", packet.data[i]);
    hex += buf;
  }
  if (!packet.xor_ok || !packet.crc_ok) {
    ESP_LOGD(TAG, "hideki %s- Pruefsumme falsch, verworfen", hex.c_str());
    return;
  }

  uint8_t type = packet.data[3] & 0x1F;
  if (type != 0x1E || packet.length < 10) {
    ESP_LOGI(TAG, "Empfangen: hideki %s- Sensortyp 0x%02X wird noch nicht ausgewertet", hex.c_str(), type);
    return;
  }

  // Temperatur/Feuchte (TS04): Temperatur als BCD in Zehntelgrad, Bit 7 = positiv, Bit 6 = Batterie ok
  uint8_t channel = packet.data[1] >> 5;
  if (channel >= 5)
    channel -= 1;
  float temperature =
      ((packet.data[5] & 0x0F) * 100 + (packet.data[4] >> 4) * 10 + (packet.data[4] & 0x0F)) / 10.0f;
  if (!(packet.data[5] & 0x80))
    temperature = -temperature;
  bool battery_ok = packet.data[5] & 0x40;
  uint8_t humidity = (packet.data[6] >> 4) * 10 + (packet.data[6] & 0x0F);

  ESP_LOGI(TAG, "Empfangen: hideki Kanal %u: %.1f °C, %u %%, Batterie %s", channel, temperature, humidity,
           battery_ok ? "ok" : "schwach");
  if (this->mqtt_discovery_)
    this->mqtt_on_hideki_(channel, temperature, humidity, battery_ok);
}

// Oregon Scientific. Ausgewertet werden bisher nur die UV-Sensoren, alles andere erscheint mit Rohdaten im Log.
void Rf433Component::on_oregon_(const OregonPacket &packet) {
  uint32_t now = millis();
  this->burst_decoded_ = true;
  if (packet.count == this->last_oregon_.count &&
      memcmp(packet.nibbles, this->last_oregon_.nibbles, packet.count) == 0 && now - this->last_oregon_time_ < 2000)
    return;
  this->last_oregon_ = packet;
  this->last_oregon_time_ = now;

  std::string hex;
  char buf[4];
  for (uint8_t i = 0; i < packet.count; i++) {
    snprintf(buf, sizeof(buf), "%X", packet.nibbles[i]);
    hex += buf;
  }
  uint16_t sensor_id = packet.sensor_id();
  uint8_t channel = packet.nibbles[4];
  bool battery_ok = !(packet.nibbles[7] & 0x4);
  ESP_LOGI(TAG, "Empfangen: oregon v%u %04X Kanal %u, Nibbles %s, Pruefsumme %s (Index %d)", packet.version,
           sensor_id, channel, hex.c_str(), packet.checksum_at >= 0 ? "ok" : "FEHLT", packet.checksum_at);
  if (packet.checksum_at < 0)
    return;

  // UV-Index: UVN800 (Kennung D874) und UVR128 (EC70)
  int uv_index = -1;
  if (sensor_id == 0xD874 && packet.count > 11) {
    uv_index = packet.nibbles[11] * 10 + packet.nibbles[8];
  } else if (sensor_id == 0xEC70 && packet.count > 9) {
    uv_index = packet.nibbles[9] * 10 + packet.nibbles[8];
  }
  if (uv_index < 0 || uv_index > 25)
    return;
  ESP_LOGI(TAG, "  UV-Index %d, Batterie %s", uv_index, battery_ok ? "ok" : "schwach");
  if (this->mqtt_discovery_)
    this->mqtt_on_uv_(sensor_id, channel, uv_index, battery_ok);
}

// Sammelt zusammenhaengende Pulsfolgen (150 µs .. 5 ms). Endet eine Folge mit mindestens
// 40 Pulsen, ohne dass ein Dekoder etwas erkannt hat, wird sie ins Log geschrieben.
// Positive Werte = High, negative = Low, jeweils in µs.
void Rf433Component::track_unknown_(bool level, uint16_t duration) {
  if (duration >= 150 && duration <= 5000) {
    if (this->burst_count_ < BURST_MAX)
      this->burst_[this->burst_count_] = level ? (int16_t) duration : -(int16_t) duration;
    this->burst_count_++;
    return;
  }

  uint32_t now = millis();
  if (this->burst_count_ >= 40 && !this->burst_decoded_ && now - this->last_dump_ >= 2000) {
    this->last_dump_ = now;
    std::string line;
    char buf[8];
    uint16_t shown = this->burst_count_ < BURST_MAX ? this->burst_count_ : BURST_MAX;
    for (uint16_t i = 0; i < shown; i++) {
      snprintf(buf, sizeof(buf), "%d ", this->burst_[i]);
      line += buf;
    }
    ESP_LOGI(TAG, "Unbekannt, %u Pulse: %s", this->burst_count_, line.c_str());
  }
  this->burst_count_ = 0;
  this->burst_decoded_ = false;
}

// --------------------------------------------------
// MQTT-Discovery: Geraete erscheinen von selbst in Home Assistant
//
// Topics (Beispiele):
//   homeassistant/switch/rf433/ac_1a2b3c4_3/config   Discovery, retained
//   rf433/ac/1A2B3C4/3/set | state                    selbstlernende Steckdose
//   rf433/pt2262/451551/350/set                       350 = Pulslaenge der Fernbedienung in µs
//   rf433/pt2262/451551/state | event
// PT2262-Steckdosen: Ein-Code endet auf 1 oder 5, der zugehoerige Aus-Code auf 4.
// Alle anderen PT2262-Codes werden als einzelne Taste angelegt (senden + Empfangs-Event).
// --------------------------------------------------

void Rf433Component::set_auto_add(bool enabled) {
  this->auto_add_ = enabled;
  ESP_LOGI(TAG, "Neue Geraete automatisch anlegen: %s", ONOFF(enabled));
}

#ifdef USE_MQTT

static const char *const DISCOVERY_PREFIX = "homeassistant";
static const char *const NODE = "rf433";
static const size_t MAX_KNOWN = 150;

static std::string config_topic(const char *component, const std::string &object_id) {
  return std::string(DISCOVERY_PREFIX) + "/" + component + "/" + NODE + "/" + object_id + "/config";
}

static bool pt2262_is_on_code(uint32_t code) { return (code & 0xF) == 0x1 || (code & 0xF) == 0x5; }
static bool pt2262_is_off_code(uint32_t code) { return (code & 0xF) == 0x4; }

static std::string hex6(uint32_t code) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%06X", (unsigned) code);
  return buf;
}

static std::string pt2262_object(uint32_t code) {
  char buf[20];
  snprintf(buf, sizeof(buf), "pt2262_%06x", (unsigned) code);
  return buf;
}

static std::vector<std::string> split_topic(const std::string &topic) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = topic.find('/', start);
    if (pos == std::string::npos) {
      parts.push_back(topic.substr(start));
      return parts;
    }
    parts.push_back(topic.substr(start, pos - start));
    start = pos + 1;
  }
}

static void publish(const std::string &topic, const std::string &payload, bool retain) {
  mqtt::global_mqtt_client->publish(topic, payload, retain ? 1 : 0, retain);
}

void Rf433Component::mqtt_setup_() {
  // Bekannte Geraete: der Broker liefert die gespeicherten Discovery-Nachrichten beim Abonnieren.
  // Leere Nachricht = Geraet wurde geloescht (z. B. in Home Assistant).
  mqtt::global_mqtt_client->subscribe(std::string(DISCOVERY_PREFIX) + "/+/" + NODE + "/+/config",
                                      [this](const std::string &topic, const std::string &payload) {
                                        if (payload.empty()) {
                                          this->known_.erase(topic);
                                        } else if (this->known_.size() < MAX_KNOWN) {
                                          this->known_.insert(topic);
                                        }
                                      });
  mqtt::global_mqtt_client->subscribe(std::string(NODE) + "/+/+/+/set",
                                      [this](const std::string &topic, const std::string &payload) {
                                        this->mqtt_on_command_(topic, payload);
                                      });
}

static std::string device_json(const std::string &object_id, const std::string &name, const char *model) {
  return "\"dev\":{\"ids\":[\"rf433_" + object_id + "\"],\"name\":\"" + name + "\",\"mdl\":\"" + model +
         "\",\"mf\":\"RF433\"}";
}

static std::string availability_json() {
  return "\"avty_t\":\"" + mqtt::global_mqtt_client->get_topic_prefix() + "/status\",";
}

void Rf433Component::mqtt_discover_switch_(const std::string &object_id, const std::string &name, const char *model,
                                           const std::string &base_topic, const std::string &command_topic) {
  ESP_LOGI(TAG, "Neues Geraet: %s", name.c_str());
  std::string topic = config_topic("switch", object_id);
  publish(topic,
          "{\"name\":null,\"uniq_id\":\"rf433_" + object_id + "\",\"cmd_t\":\"" + command_topic + "\",\"stat_t\":\"" +
              base_topic + "/state\"," + availability_json() + device_json(object_id, name, model) + "}",
          true);
  this->known_.insert(topic);
}

void Rf433Component::mqtt_discover_button_(const std::string &object_id, const std::string &name,
                                           const std::string &base_topic, const std::string &command_topic) {
  ESP_LOGI(TAG, "Neues Geraet: %s", name.c_str());
  std::string device = device_json(object_id, name, "PT2262");

  std::string topic = config_topic("button", object_id);
  publish(topic,
          "{\"name\":\"Senden\",\"uniq_id\":\"rf433_" + object_id + "_tx\",\"cmd_t\":\"" + command_topic + "\"," +
              availability_json() + device + "}",
          true);
  this->known_.insert(topic);

  topic = config_topic("event", object_id);
  publish(topic,
          "{\"name\":\"Empfangen\",\"uniq_id\":\"rf433_" + object_id + "_rx\",\"stat_t\":\"" + base_topic +
              "/event\",\"evt_typ\":[\"press\"]," + availability_json() + device + "}",
          true);
  this->known_.insert(topic);
}

void Rf433Component::mqtt_on_message_(const Message &msg) {
  if (!mqtt::global_mqtt_client->is_connected())
    return;

  if (msg.protocol == Protocol::AC) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%07X", (unsigned) msg.code);
    std::string address = buf;
    const std::string state = msg.on ? "ON" : "OFF";

    if (msg.group) {
      // Gruppenbefehl: alle bekannten Units dieser Adresse
      snprintf(buf, sizeof(buf), "ac_%07x_", (unsigned) msg.code);
      std::string prefix = std::string(DISCOVERY_PREFIX) + "/switch/" + NODE + "/" + buf;
      for (const auto &topic : this->known_) {
        if (topic.compare(0, prefix.size(), prefix) != 0)
          continue;
        int unit = atoi(topic.c_str() + prefix.size());
        publish(std::string(NODE) + "/ac/" + address + "/" + std::to_string(unit) + "/state", state, true);
      }
      return;
    }

    snprintf(buf, sizeof(buf), "ac_%07x_%u", (unsigned) msg.code, msg.unit);
    std::string object_id = buf;
    std::string base = std::string(NODE) + "/ac/" + address + "/" + std::to_string(msg.unit);
    if (!this->known_.count(config_topic("switch", object_id))) {
      if (!this->auto_add_)
        return;
      this->mqtt_discover_switch_(object_id, "AC " + code_string(msg), "AC", base, base + "/set");
    }
    publish(base + "/state", state, true);
    return;
  }

  // PT2262
  if (pt2262_is_off_code(msg.code)) {
    // Aus-Code einer Steckdose: gehoert zum Ein-Code mit Endung 1 oder 5
    for (uint32_t nibble : {0x1u, 0x5u}) {
      uint32_t on_code = (msg.code & ~0xFu) | nibble;
      if (this->known_.count(config_topic("switch", pt2262_object(on_code)))) {
        publish(std::string(NODE) + "/pt2262/" + hex6(on_code) + "/state", "OFF", true);
        return;
      }
    }
    if (this->auto_add_)
      ESP_LOGI(TAG, "pt2262 %s sieht nach einem Aus-Code aus - zum Anlegen die Ein-Taste druecken",
               hex6(msg.code).c_str());
    return;
  }

  uint16_t pulse = ((msg.pulse + 5) / 10) * 10;
  std::string base = std::string(NODE) + "/pt2262/" + hex6(msg.code);
  std::string command = base + "/" + std::to_string(pulse) + "/set";
  std::string object_id = pt2262_object(msg.code);

  if (pt2262_is_on_code(msg.code)) {
    if (!this->known_.count(config_topic("switch", object_id))) {
      if (!this->auto_add_)
        return;
      this->mqtt_discover_switch_(object_id, "PT2262 " + hex6(msg.code), "PT2262", base, command);
    }
    publish(base + "/state", "ON", true);
    return;
  }

  // Einzelner Code: Taste
  if (!this->known_.count(config_topic("event", object_id))) {
    if (!this->auto_add_)
      return;
    this->mqtt_discover_button_(object_id, "PT2262 " + hex6(msg.code), base, command);
  }
  publish(base + "/event", "{\"event_type\":\"press\"}", false);
}

// rf433/ac/<Adresse>/<Unit>/set oder rf433/pt2262/<Code>/<Pulslaenge>/set, Nutzlast ON, OFF oder PRESS
void Rf433Component::mqtt_on_command_(const std::string &topic, const std::string &payload) {
  std::vector<std::string> parts = split_topic(topic);
  if (parts.size() != 5)
    return;
  bool on = payload != "OFF";
  uint32_t code = strtoul(parts[2].c_str(), nullptr, 16);
  int number = atoi(parts[3].c_str());

  if (parts[1] == "ac") {
    if (code > 0x3FFFFFF || number < 1 || number > 16)
      return;
    this->send_ac(code, number, false, on, 0, 0);
    publish(std::string(NODE) + "/ac/" + parts[2] + "/" + parts[3] + "/state", on ? "ON" : "OFF", true);
  } else if (parts[1] == "pt2262") {
    if (code > 0xFFFFFF || number < 100 || number > 1000)
      return;
    if (payload == "PRESS") {
      this->send_pt2262(code, number, 0);
      return;
    }
    this->send_pt2262(on ? code : ((code & ~0xFu) | 0x4), number, 0);
    publish(std::string(NODE) + "/pt2262/" + parts[2] + "/state", on ? "ON" : "OFF", true);
  }
}

// Wettersensor: ein Geraet je Kanal mit Temperatur, Feuchte und Batteriewarnung.
// Meldet sich der Sensor 10 Minuten nicht, zeigt Home Assistant "nicht verfuegbar".
void Rf433Component::mqtt_on_hideki_(uint8_t channel, float temperature, uint8_t humidity, bool battery_ok) {
  if (!mqtt::global_mqtt_client->is_connected())
    return;
  std::string id = "hideki_" + std::to_string(channel);
  std::string state_topic = std::string(NODE) + "/hideki/" + std::to_string(channel) + "/state";

  if (!this->known_.count(config_topic("sensor", id + "_t"))) {
    if (!this->auto_add_)
      return;
    std::string name = "Hideki Kanal " + std::to_string(channel);
    ESP_LOGI(TAG, "Neues Geraet: %s", name.c_str());
    std::string common = "\"stat_t\":\"" + state_topic + "\",\"exp_aft\":600," + availability_json() +
                         device_json(id, name, "Temperatur/Feuchte") + "}";

    std::string topic = config_topic("sensor", id + "_t");
    publish(topic,
            "{\"name\":\"Temperatur\",\"uniq_id\":\"rf433_" + id +
                "_t\",\"dev_cla\":\"temperature\",\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\","
                "\"val_tpl\":\"{{ value_json.temperature }}\"," +
                common,
            true);
    this->known_.insert(topic);

    topic = config_topic("sensor", id + "_h");
    publish(topic,
            "{\"name\":\"Luftfeuchtigkeit\",\"uniq_id\":\"rf433_" + id +
                "_h\",\"dev_cla\":\"humidity\",\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\","
                "\"val_tpl\":\"{{ value_json.humidity }}\"," +
                common,
            true);
    this->known_.insert(topic);

    topic = config_topic("binary_sensor", id + "_b");
    publish(topic,
            "{\"name\":\"Batterie\",\"uniq_id\":\"rf433_" + id +
                "_b\",\"dev_cla\":\"battery\",\"ent_cat\":\"diagnostic\","
                "\"val_tpl\":\"{{ value_json.battery_low }}\"," +
                common,
            true);
    this->known_.insert(topic);
  }

  char buf[80];
  snprintf(buf, sizeof(buf), "{\"temperature\":%.1f,\"humidity\":%u,\"battery_low\":\"%s\"}", temperature, humidity,
           battery_ok ? "OFF" : "ON");
  publish(state_topic, buf, false);
}

void Rf433Component::mqtt_on_uv_(uint16_t sensor_id, uint8_t channel, uint8_t uv_index, bool battery_ok) {
  if (!mqtt::global_mqtt_client->is_connected())
    return;
  char buf[80];
  snprintf(buf, sizeof(buf), "oregon_%04x_%u", sensor_id, channel);
  std::string id = buf;
  std::string state_topic = std::string(NODE) + "/oregon/" + id.substr(7) + "/state";

  if (!this->known_.count(config_topic("sensor", id + "_uv"))) {
    if (!this->auto_add_)
      return;
    snprintf(buf, sizeof(buf), "Oregon UV %04X Kanal %u", sensor_id, channel);
    std::string name = buf;
    ESP_LOGI(TAG, "Neues Geraet: %s", name.c_str());
    std::string common = "\"stat_t\":\"" + state_topic + "\",\"exp_aft\":900," + availability_json() +
                         device_json(id, name, "UV-Sensor") + "}";

    std::string topic = config_topic("sensor", id + "_uv");
    publish(topic,
            "{\"name\":\"UV-Index\",\"uniq_id\":\"rf433_" + id +
                "_uv\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:weather-sunny\","
                "\"val_tpl\":\"{{ value_json.uv }}\"," +
                common,
            true);
    this->known_.insert(topic);

    topic = config_topic("binary_sensor", id + "_b");
    publish(topic,
            "{\"name\":\"Batterie\",\"uniq_id\":\"rf433_" + id +
                "_b\",\"dev_cla\":\"battery\",\"ent_cat\":\"diagnostic\","
                "\"val_tpl\":\"{{ value_json.battery_low }}\"," +
                common,
            true);
    this->known_.insert(topic);
  }

  snprintf(buf, sizeof(buf), "{\"uv\":%u,\"battery_low\":\"%s\"}", uv_index, battery_ok ? "OFF" : "ON");
  publish(state_topic, buf, false);
}

void Rf433Component::remove_discovered() {
  ESP_LOGI(TAG, "Entferne %u automatisch angelegte Eintraege", (unsigned) this->known_.size());
  std::set<std::string> topics = this->known_;
  for (const auto &topic : topics)
    publish(topic, "", true);
  this->known_.clear();
}

#else  // USE_MQTT

void Rf433Component::mqtt_setup_() {}
void Rf433Component::mqtt_on_message_(const Message &msg) {}
void Rf433Component::mqtt_on_command_(const std::string &topic, const std::string &payload) {}
void Rf433Component::mqtt_on_hideki_(uint8_t channel, float temperature, uint8_t humidity, bool battery_ok) {}
void Rf433Component::mqtt_on_uv_(uint16_t sensor_id, uint8_t channel, uint8_t uv_index, bool battery_ok) {}
void Rf433Component::mqtt_discover_switch_(const std::string &object_id, const std::string &name, const char *model,
                                           const std::string &base_topic, const std::string &command_topic) {}
void Rf433Component::mqtt_discover_button_(const std::string &object_id, const std::string &name,
                                           const std::string &base_topic, const std::string &command_topic) {}
void Rf433Component::remove_discovered() {}

#endif  // USE_MQTT

// --------------------------------------------------
// Geraetezuordnung
// --------------------------------------------------

bool Rf433Device::match_(const Message &msg, bool &state) const {
  if (msg.protocol != this->protocol_)
    return false;
  if (this->protocol_ == Protocol::AC) {
    if (msg.code != this->on_code_ || (!msg.group && msg.unit != this->unit_))
      return false;
    state = msg.on;
    return true;
  }
  if (msg.code == this->on_code_) {
    state = true;
    return true;
  }
  if (this->has_off_ && msg.code == this->off_code_) {
    state = false;
    return true;
  }
  return false;
}

void Rf433Device::transmit_(bool state) {
  if (this->protocol_ == Protocol::AC) {
    this->parent_->send_ac(this->on_code_, this->unit_, false, state, this->pulse_, this->repeats_);
  } else {
    this->parent_->send_pt2262(state ? this->on_code_ : this->off_code_, this->pulse_, this->repeats_);
  }
}

void Rf433Device::dump_device_(const char *tag) const {
  if (this->protocol_ == Protocol::AC) {
    ESP_LOGCONFIG(tag, "  ac, Adresse %07X, Unit %u", (unsigned) this->on_code_, this->unit_);
  } else if (this->has_off_) {
    ESP_LOGCONFIG(tag, "  pt2262, Ein %06X, Aus %06X", (unsigned) this->on_code_, (unsigned) this->off_code_);
  } else {
    ESP_LOGCONFIG(tag, "  pt2262, Code %06X", (unsigned) this->on_code_);
  }
}

}  // namespace rf433
}  // namespace esphome
