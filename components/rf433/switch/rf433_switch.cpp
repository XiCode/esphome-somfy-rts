#include "rf433_switch.h"

#include "esphome/core/log.h"

namespace esphome {
namespace rf433 {

static const char *const TAG = "rf433.switch";

void Rf433Switch::dump_config() {
  LOG_SWITCH("", "RF433 Schalter", this);
  this->dump_device_(TAG);
}

void Rf433Switch::write_state(bool state) {
  this->transmit_(state);
  this->publish_state(state);
}

void Rf433Switch::on_rf433_message(const Message &msg) {
  bool state;
  if (this->match_(msg, state))
    this->publish_state(state);
}

}  // namespace rf433
}  // namespace esphome
