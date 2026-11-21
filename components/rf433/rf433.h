#pragma once

#include <set>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/somfy_rts/somfy_rts.h"

#include "rf433_codec.h"

namespace esphome {
namespace rf433 {

// "EE6104" (PT2262) bzw. "1A2B3C4:3" / "1A2B3C4:G" (AC), gleiche Schreibweise wie beim RFXtrx
std::string code_string(const Message &msg);
const char *protocol_name(Protocol protocol);

class Rf433Listener {
 public:
  virtual void on_rf433_message(const Message &msg) = 0;
};

// PT2262 und AC auf dem CC1101 des Somfy-Hubs: hoert die Pulse mit und sendet ueber den Hub.
class Rf433Component : public Component {
 public:
  void set_hub(somfy_rts::SomfyRtsHub *hub) { this->hub_ = hub; }
  void set_frequency(float mhz) { this->frequency_ = mhz; }
  void set_dump_unknown(bool dump) { this->dump_unknown_ = dump; }
  // Geraete per MQTT-Discovery in Home Assistant anlegen, schalten und den Zustand melden
  void set_mqtt_discovery(bool enabled) { this->mqtt_discovery_ = enabled; }
  // Nur wenn an, werden bisher unbekannte Codes als neue Geraete angelegt
  void set_auto_add(bool enabled);
  bool get_auto_add() const { return this->auto_add_; }
  // Entfernt alle automatisch angelegten Geraete wieder aus Home Assistant
  void remove_discovered();

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void send_pt2262(uint32_t code, uint16_t pulse, int repeats);
  void send_ac(uint32_t address, uint8_t unit, bool group, bool on, uint16_t pulse, int repeats);

  void register_listener(Rf433Listener *listener) { this->listeners_.push_back(listener); }
  void add_on_receive_callback(std::function<void(std::string, std::string, bool)> &&callback) {
    this->receive_callback_.add(std::move(callback));
  }

 protected:
  void on_pulse_(bool level, uint16_t duration);
  void on_decoded_(const Message &msg);
  void on_hideki_(const HidekiPacket &packet);
  void on_oregon_(const OregonPacket &packet);
  void track_unknown_(bool level, uint16_t duration);
  bool self_test_();
  void mqtt_setup_();
  void mqtt_on_message_(const Message &msg);
  void mqtt_on_command_(const std::string &topic, const std::string &payload);
  void mqtt_on_hideki_(uint8_t channel, float temperature, uint8_t humidity, bool battery_ok);
  void mqtt_on_uv_(uint16_t sensor_id, uint8_t channel, uint8_t uv_index, bool battery_ok);
  void mqtt_discover_switch_(const std::string &object_id, const std::string &name, const char *model,
                             const std::string &base_topic, const std::string &command_topic);
  void mqtt_discover_button_(const std::string &object_id, const std::string &name, const std::string &base_topic,
                             const std::string &command_topic);

  somfy_rts::SomfyRtsHub *hub_{nullptr};
  float frequency_{433.92f};
  bool dump_unknown_{false};
  bool self_test_ok_{false};
  bool mqtt_discovery_{false};
  bool auto_add_{false};
  // Discovery-Topics aller angelegten Geraete. Der Broker haelt sie vor (retained),
  // der ESP liest sie beim Verbinden wieder ein und braucht keinen eigenen Speicher dafuer.
  std::set<std::string> known_;

  Pt2262Decoder pt2262_;
  AcDecoder ac_;
  HidekiDecoder hideki_;
  OregonDecoder oregon_;
  OregonPacket last_oregon_;
  uint32_t last_oregon_time_{0};
  HidekiPacket last_hideki_;
  uint32_t last_hideki_time_{0};
  std::vector<Rf433Listener *> listeners_;
  CallbackManager<void(std::string, std::string, bool)> receive_callback_;

  // Fernbedienungen wiederholen jeden Frame: erst der zweite gleiche zaehlt, weitere werden ignoriert
  Message last_;
  uint32_t last_time_{0};
  bool last_valid_{false};
  bool last_reported_{false};

  // dump_unknown: laengere Pulsfolgen, die kein Dekoder erkannt hat
  static const uint8_t BURST_MAX = 64;
  int16_t burst_[BURST_MAX];
  uint16_t burst_count_{0};
  bool burst_decoded_{false};
  uint32_t last_dump_{0};
};

class ReceiveTrigger : public Trigger<std::string, std::string, bool> {
 public:
  explicit ReceiveTrigger(Rf433Component *parent) {
    parent->add_on_receive_callback(
        [this](std::string protocol, std::string code, bool state) { this->trigger(protocol, code, state); });
  }
};

// Gemeinsame Zuordnung fuer Schalter und Binaersensor:
// PT2262 = je ein Code fuer Ein und Aus, AC = Adresse + Unit (Ein/Aus steckt im Frame)
class Rf433Device {
 public:
  void set_parent(Rf433Component *parent) { this->parent_ = parent; }
  void set_pt2262(uint32_t on_code, bool has_off, uint32_t off_code) {
    this->protocol_ = Protocol::PT2262;
    this->on_code_ = on_code;
    this->has_off_ = has_off;
    this->off_code_ = off_code;
  }
  void set_ac(uint32_t address, uint8_t unit) {
    this->protocol_ = Protocol::AC;
    this->on_code_ = address;
    this->unit_ = unit;
    this->has_off_ = true;
  }
  void set_pulse_length(uint16_t us) { this->pulse_ = us; }
  void set_repeats(int repeats) { this->repeats_ = repeats; }

 protected:
  // true, wenn die Nachricht dieses Geraet betrifft; state = Ein oder Aus
  bool match_(const Message &msg, bool &state) const;
  void transmit_(bool state);
  void dump_device_(const char *tag) const;

  Rf433Component *parent_{nullptr};
  Protocol protocol_{Protocol::PT2262};
  uint32_t on_code_{0};
  uint32_t off_code_{0};
  bool has_off_{false};
  uint8_t unit_{1};
  uint16_t pulse_{0};  // 0 = Standard des Protokolls
  int repeats_{0};     // 0 = Standard des Protokolls
};

}  // namespace rf433
}  // namespace esphome
