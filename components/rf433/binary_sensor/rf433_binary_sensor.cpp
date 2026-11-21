#include "rf433_binary_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace rf433 {

static const char *const TAG = "rf433.binary_sensor";

static const uint32_t BUTTON_HOLD_MS = 500;

void Rf433BinarySensor::setup() { this->publish_initial_state(false); }

void Rf433BinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "RF433 Binaersensor", this);
  this->dump_device_(TAG);
}

void Rf433BinarySensor::on_rf433_message(const Message &msg) {
  bool state;
  if (!this->match_(msg, state))
    return;
  this->publish_state(state);
  if (!this->has_off_)
    this->set_timeout("off", BUTTON_HOLD_MS, [this]() { this->publish_state(false); });
}

}  // namespace rf433
}  // namespace esphome
