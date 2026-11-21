#pragma once

#include <functional>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace somfy_rts {

enum class SomfyCommand : uint8_t {
  MY = 0x1,
  UP = 0x2,
  MY_UP = 0x3,
  DOWN = 0x4,
  MY_DOWN = 0x5,
  UP_DOWN = 0x6,
  PROG = 0x8,
  SUN_FLAG = 0x9,
  FLAG = 0xA,
};

const char *command_name(uint8_t cmd);

// bis der Motor den ersten Frame gehört hat
static const uint32_t MOTOR_REACTION_MS = 250;
// ~6 s MY halten = MY-Position speichern
static const int MY_SAVE_REPEATS = 40;
// so lange muss MY an der Original-FB gehalten werden
static const uint32_t MY_LONG_PRESS_MS = 3000;

class SomfyRtsCover;

// CC1101 + Senden/Empfangen. Die Rollos (SomfyRtsCover) hängen sich hier an.
class SomfyRtsHub : public Component {
 public:
  void set_spi_pins(uint8_t sck, uint8_t miso, uint8_t mosi, uint8_t cs) {
    this->sck_pin_ = sck;
    this->miso_pin_ = miso;
    this->mosi_pin_ = mosi;
    this->cs_pin_ = cs;
  }
  void set_gdo_pins(uint8_t gdo0, uint8_t gdo2) {
    this->gdo0_pin_ = gdo0;
    this->gdo2_pin_ = gdo2;
  }
  void set_frequency(float mhz) { this->frequency_ = mhz; }
  void set_tx_power(int power) { this->tx_power_ = power; }
  // Empfangsfrequenz und -bandbreite duerfen von der Somfy-Sendefrequenz abweichen,
  // damit ein breiter Empfaenger auch 433,92-MHz-Protokolle mithoert.
  void set_rx_frequency(float mhz) { this->rx_frequency_ = mhz; }
  void set_rx_bandwidth(float khz) { this->rx_bandwidth_ = khz; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Sendet einen Befehl und liefert den Zeitpunkt (millis), zu dem der Motor reagiert
  uint32_t transmit(uint32_t address, const std::string &storage_key, uint16_t min_code, SomfyCommand command,
                    int repeats = 4);
  // Nächster Rolling Code, ohne ihn zu verbrauchen
  uint16_t peek_rolling_code(const std::string &storage_key, uint16_t min_code);

  // Sendet beliebige OOK-Pulse (abwechselnd High/Low in µs, beginnend mit High) auf der angegebenen Frequenz
  void transmit_ook(float mhz, const std::vector<uint16_t> &pulses, int repeats);
  // Jeder empfangene Puls (Pegel, Dauer in µs), fuer weitere Protokolle auf demselben Funkmodul
  void add_pulse_listener(std::function<void(bool, uint16_t)> &&callback) {
    this->pulse_callback_.add(std::move(callback));
  }
  bool radio_ok() const { return this->radio_ok_; }

  void register_cover(SomfyRtsCover *cover) { this->covers_.push_back(cover); }
  void add_on_receive_callback(std::function<void(std::string, std::string, uint16_t)> &&callback) {
    this->receive_callback_.add(std::move(callback));
  }

 protected:
  uint16_t next_rolling_code_(const std::string &storage_key, uint16_t min_code);
  void rx_start_();
  void rx_stop_();
  void send_frame_(const uint8_t *frame, uint8_t sync);
  void decode_pulse_(bool level, uint16_t duration);
  void decode_frame_();
  void on_frame_(uint32_t address, uint8_t cmd, uint16_t rolling);
  void check_pending_my_();

  uint8_t sck_pin_{18}, miso_pin_{19}, mosi_pin_{23}, cs_pin_{5};
  uint8_t gdo0_pin_{4}, gdo2_pin_{2};
  float frequency_{433.42f};
  float rx_frequency_{433.42f};
  float rx_bandwidth_{100.0f};
  int tx_power_{10};
  bool radio_ok_{false};

  std::vector<SomfyRtsCover *> covers_;
  CallbackManager<void(std::string, std::string, uint16_t)> receive_callback_;
  CallbackManager<void(bool, uint16_t)> pulse_callback_;

  // Dekoder: 56 Bit Manchester = 112 Halbbits
  struct {
    bool active{false};
    bool skip_sync_low{false};
    uint8_t half[112];
    int count{0};
  } dec_;

  // Jede Taste wird mehrfach mit demselben Rolling Code wiederholt
  uint32_t last_address_{0};
  uint16_t last_rolling_{0};
  uint32_t last_time_{0};

  // MY an der Original-FB: kurz = MY anfahren, lang = MY speichern.
  // Das weiß man erst beim Loslassen, also wird es hier zwischengespeichert.
  struct {
    bool active{false};
    uint32_t address{0};
    uint16_t rolling{0};
    uint32_t first_seen{0};
    uint32_t last_seen{0};
  } pending_my_;
};

class ReceiveTrigger : public Trigger<std::string, std::string, uint16_t> {
 public:
  explicit ReceiveTrigger(SomfyRtsHub *parent) {
    parent->add_on_receive_callback([this](std::string remote, std::string command, uint16_t rolling_code) {
      this->trigger(remote, command, rolling_code);
    });
  }
};

}  // namespace somfy_rts
}  // namespace esphome
