#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"

#include "../rf433.h"

namespace esphome {
namespace rf433 {

// Hoert nur mit. Mit Ein- und Aus-Code (bzw. bei ac) bleibt der Zustand stehen,
// mit nur einem pt2262-Code ist es ein Taster, der kurz auf "an" geht.
class Rf433BinarySensor : public binary_sensor::BinarySensor,
                          public Component,
                          public Rf433Device,
                          public Rf433Listener {
 public:
  void setup() override;
  void dump_config() override;
  void on_rf433_message(const Message &msg) override;
};

}  // namespace rf433
}  // namespace esphome
