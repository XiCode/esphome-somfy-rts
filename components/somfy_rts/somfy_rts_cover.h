#pragma once

#include <string>
#include <vector>

#include "esphome/components/cover/cover.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "somfy_rts.h"

namespace esphome {
namespace somfy_rts {

// Ein Rollo = eine virtuelle Fernbedienung.
// RTS meldet nichts zurück -> Position wird über die Fahrzeit geschätzt (0 = zu, 1 = auf).
class SomfyRtsCover : public cover::Cover, public Component {
 public:
  void set_hub(SomfyRtsHub *hub) { this->hub_ = hub; }
  void set_address(uint32_t address) { this->address_ = address; }
  void set_storage_key(const std::string &key) { this->storage_key_ = key; }
  void set_min_rolling_code(uint16_t code) { this->min_rolling_code_ = code; }
  void set_open_duration(uint32_t ms) { this->open_duration_ = ms; }
  void set_close_duration(uint32_t ms) { this->close_duration_ = ms; }
  void set_initial_my_position(float position) { this->my_position_ = position; }
  void add_remote(uint32_t address) { this->remotes_.push_back(address); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  cover::CoverTraits get_traits() override;

  // Für Buttons in YAML, z. B. "lambda: id(rollo1).prog();"
  void prog();
  void goto_my();
  void save_my();
  void set_my_position(float position);
  float get_my_position() const { return this->my_position_; }

  // Vom Hub beim Empfang der Original-Fernbedienungen
  bool listens_to(uint32_t address) const;
  bool is_moving() const { return this->direction_ != 0; }
  void apply_command(SomfyCommand command, uint32_t when);
  void remote_my_released(bool long_press, uint32_t when);

 protected:
  void control(const cover::CoverCall &call) override;
  uint32_t send_(SomfyCommand command, int repeats = 4);
  float position_at_(uint32_t when) const;
  void start_move_(int8_t dir, float target, bool stop_by_self, uint32_t when);
  void stop_move_(uint32_t when);
  void finish_move_(float position);
  void move_to_(float target);
  void publish_(bool save);

  SomfyRtsHub *hub_{nullptr};
  uint32_t address_{0};
  std::string storage_key_;
  uint16_t min_rolling_code_{1};
  uint32_t open_duration_{30000};
  uint32_t close_duration_{30000};
  std::vector<uint32_t> remotes_;

  float pos_{0.5f};
  float my_position_{-1};  // -1 = unbekannt
  int8_t direction_{0};    // +1 fährt auf, -1 fährt ab, 0 steht
  float start_pos_{0};
  uint32_t start_time_{0};
  float target_{-1};           // Zielposition, -1 = bis Endlage
  bool stop_by_self_{false};   // true: ESP sendet am Ziel STOP; false: Motor hält selbst (MY)
  uint32_t last_publish_{0};
  ESPPreferenceObject my_pref_;
};

}  // namespace somfy_rts
}  // namespace esphome
