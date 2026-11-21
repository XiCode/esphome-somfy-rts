#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

#include "../rf433.h"

namespace esphome {
namespace rf433 {

// Funksteckdose: sendet beim Schalten und uebernimmt den Zustand, wenn die Original-Fernbedienung
// gedrueckt wird. Die Steckdose meldet nichts zurueck, der Zustand ist also angenommen.
class Rf433Switch : public switch_::Switch, public Component, public Rf433Device, public Rf433Listener {
 public:
  void dump_config() override;
  bool assumed_state() override { return true; }
  void on_rf433_message(const Message &msg) override;

 protected:
  void write_state(bool state) override;
};

}  // namespace rf433
}  // namespace esphome
