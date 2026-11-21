#include "somfy_rts.h"
#include "somfy_rts_cover.h"

#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <nvs.h>
#include "esp_timer.h"
#include "soc/gpio_reg.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace somfy_rts {

static const char *const TAG = "somfy_rts";

// Gleicher Namespace wie Somfy_Remote_Lib (NVSRollingCodeStorage) im alten Arduino-Sketch,
// damit die Rolling Codes nach dem Umstieg weiterlaufen.
static const char *const NVS_NAMESPACE = "somfy";

static const uint16_t SYMBOL = 640;

const char *command_name(uint8_t cmd) {
  switch (cmd) {
    case 0x1:
      return "MY";
    case 0x2:
      return "UP";
    case 0x3:
      return "MY_UP";
    case 0x4:
      return "DOWN";
    case 0x5:
      return "MY_DOWN";
    case 0x6:
      return "UP_DOWN";
    case 0x8:
      return "PROG";
    case 0x9:
      return "SUN_FLAG";
    case 0xA:
      return "FLAG";
    default:
      return "UNKNOWN";
  }
}

// --------------------------------------------------
// Empfang: Flanken von GDO2 per Interrupt in einen Ringpuffer
// --------------------------------------------------

static const uint16_t RX_BUF_SIZE = 1024;
static volatile uint16_t rx_buf[RX_BUF_SIZE];  // Bit 15 = Pegel, Bit 0..14 = Dauer in µs
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;
static volatile uint32_t rx_last_edge = 0;
static uint8_t rx_pin = 2;

static void IRAM_ATTR on_rx_edge() {
  uint32_t now = (uint32_t) esp_timer_get_time();
  uint32_t duration = now - rx_last_edge;
  rx_last_edge = now;

  // Gespeichert wird der Pegel, der gerade zu Ende gegangen ist
  uint32_t in = rx_pin < 32 ? REG_READ(GPIO_IN_REG) >> rx_pin : REG_READ(GPIO_IN1_REG) >> (rx_pin - 32);
  uint16_t level = (in & 1) ? 0 : 1;
  if (duration > 0x7FFF)
    duration = 0x7FFF;

  uint16_t next = (rx_head + 1) % RX_BUF_SIZE;
  if (next == rx_tail)
    return;  // Puffer voll -> verwerfen
  rx_buf[rx_head] = (uint16_t) duration | (level << 15);
  rx_head = next;
}

void SomfyRtsHub::rx_start_() {
  pinMode(this->gdo0_pin_, INPUT);  // im RX-Modus treibt der CC1101 GDO0 selbst
  pinMode(this->gdo2_pin_, INPUT);
  ELECHOUSE_cc1101.SetRx(this->rx_frequency_);
  rx_last_edge = (uint32_t) esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(this->gdo2_pin_), on_rx_edge, CHANGE);
}

void SomfyRtsHub::rx_stop_() { detachInterrupt(digitalPinToInterrupt(this->gdo2_pin_)); }

// --------------------------------------------------
// Setup
// --------------------------------------------------

void SomfyRtsHub::setup() {
  rx_pin = this->gdo2_pin_;

  ELECHOUSE_cc1101.setSpiPin(this->sck_pin_, this->miso_pin_, this->mosi_pin_, this->cs_pin_);
  ELECHOUSE_cc1101.setGDO(this->gdo0_pin_, this->gdo2_pin_);
  ELECHOUSE_cc1101.Init();

  if (!ELECHOUSE_cc1101.getCC1101()) {
    // Nicht hängen bleiben: WLAN und API laufen trotzdem, damit man das Log sieht
    ESP_LOGE(TAG, "CC1101 antwortet nicht - Verkabelung pruefen!");
    this->mark_failed();
    return;
  }

  ELECHOUSE_cc1101.setMHZ(this->frequency_);
  ELECHOUSE_cc1101.setPA(this->tx_power_);
  ELECHOUSE_cc1101.setRxBW(this->rx_bandwidth_);
  this->radio_ok_ = true;

  this->rx_start_();
}

void SomfyRtsHub::dump_config() {
  ESP_LOGCONFIG(TAG, "Somfy RTS (CC1101):");
  ESP_LOGCONFIG(TAG, "  SPI: SCK %u, MISO %u, MOSI %u, CS %u", this->sck_pin_, this->miso_pin_, this->mosi_pin_,
                this->cs_pin_);
  ESP_LOGCONFIG(TAG, "  GDO0 (TX) %u, GDO2 (RX) %u", this->gdo0_pin_, this->gdo2_pin_);
  ESP_LOGCONFIG(TAG, "  Frequenz: %.2f MHz, Sendeleistung: %d dBm", this->frequency_, this->tx_power_);
  ESP_LOGCONFIG(TAG, "  Empfang: %.2f MHz, Bandbreite %.0f kHz", this->rx_frequency_, this->rx_bandwidth_);
  if (this->is_failed())
    ESP_LOGE(TAG, "  CC1101 nicht gefunden!");
}

// --------------------------------------------------
// Rolling Code (NVS, kompatibel zu NVSRollingCodeStorage)
// --------------------------------------------------

uint16_t SomfyRtsHub::peek_rolling_code(const std::string &storage_key, uint16_t min_code) {
  uint16_t code = 1;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_u16(handle, storage_key.c_str(), &code) != ESP_OK)
      code = 1;
    nvs_close(handle);
  }
  return std::max(code, min_code);
}

uint16_t SomfyRtsHub::next_rolling_code_(const std::string &storage_key, uint16_t min_code) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS nicht verfuegbar (%s)", esp_err_to_name(err));
    return min_code;
  }
  uint16_t code;
  if (nvs_get_u16(handle, storage_key.c_str(), &code) != ESP_OK)
    code = 1;
  code = std::max(code, min_code);
  nvs_set_u16(handle, storage_key.c_str(), code + 1);
  nvs_commit(handle);
  nvs_close(handle);
  return code;
}

// --------------------------------------------------
// Senden (Frame-Aufbau wie Somfy_Remote_Lib)
// --------------------------------------------------

static void build_frame(uint8_t *frame, uint32_t address, uint8_t command, uint16_t code) {
  frame[0] = 0xA7;          // Schlüssel, egal
  frame[1] = command << 4;  // Taste, die unteren 4 Bit sind die Prüfsumme
  frame[2] = code >> 8;     // Rolling Code (Big Endian)
  frame[3] = code;
  frame[4] = address >> 16;
  frame[5] = address >> 8;
  frame[6] = address;

  // Prüfsumme: XOR aller Nibbles
  uint8_t checksum = 0;
  for (int i = 0; i < 7; i++)
    checksum ^= frame[i] ^ (frame[i] >> 4);
  frame[1] |= checksum & 0x0F;

  // Obfuskierung: XOR mit dem vorherigen Byte
  for (int i = 1; i < 7; i++)
    frame[i] ^= frame[i - 1];
}

void SomfyRtsHub::send_frame_(const uint8_t *frame, uint8_t sync) {
  const uint8_t pin = this->gdo0_pin_;
  auto high = [pin](uint32_t us) {
    digitalWrite(pin, HIGH);
    delayMicroseconds(us);
  };
  auto low = [pin](uint32_t us) {
    digitalWrite(pin, LOW);
    delayMicroseconds(us);
  };

  if (sync == 2) {
    // Nur beim ersten Frame: Weckpuls und Pause
    noInterrupts();
    high(9415);
    low(9565);
    interrupts();
    delay(80);
  }

  noInterrupts();
  // Hardware-Sync: 2x beim ersten Frame, 7x bei den Wiederholungen
  for (int i = 0; i < sync; i++) {
    high(4 * SYMBOL);
    low(4 * SYMBOL);
  }
  // Software-Sync
  high(4550);
  low(SYMBOL);
  // Daten, MSB zuerst, Manchester: 1 = low->high, 0 = high->low
  for (int i = 0; i < 56; i++) {
    if ((frame[i / 8] >> (7 - (i % 8))) & 1) {
      low(SYMBOL);
      high(SYMBOL);
    } else {
      high(SYMBOL);
      low(SYMBOL);
    }
  }
  low(415);
  interrupts();
  delay(30);
}

uint32_t SomfyRtsHub::transmit(uint32_t address, const std::string &storage_key, uint16_t min_code,
                               SomfyCommand command, int repeats) {
  if (!this->radio_ok_) {
    ESP_LOGW(TAG, "CC1101 nicht bereit - %s an %06X nicht gesendet", command_name((uint8_t) command),
             (unsigned) address);
    return millis();
  }

  uint16_t code = this->next_rolling_code_(storage_key, min_code);
  uint8_t frame[7];
  build_frame(frame, address, (uint8_t) command, code);
  ESP_LOGD(TAG, "Sende %s an %06X, Code %u", command_name((uint8_t) command), (unsigned) address, code);

  this->rx_stop_();
  ELECHOUSE_cc1101.SetTx(this->frequency_);
  pinMode(this->gdo0_pin_, OUTPUT);
  uint32_t started = millis();
  this->send_frame_(frame, 2);
  for (int i = 0; i < repeats; i++) {
    this->send_frame_(frame, 7);
    App.feed_wdt();  // MY speichern sendet ~6 s am Stück
  }
  digitalWrite(this->gdo0_pin_, LOW);
  this->rx_start_();
  return started + MOTOR_REACTION_MS;
}

void SomfyRtsHub::transmit_ook(float mhz, const std::vector<uint16_t> &pulses, int repeats) {
  if (!this->radio_ok_) {
    ESP_LOGW(TAG, "CC1101 nicht bereit - nichts gesendet");
    return;
  }
  const uint8_t pin = this->gdo0_pin_;
  this->rx_stop_();
  ELECHOUSE_cc1101.SetTx(mhz);
  pinMode(pin, OUTPUT);
  for (int r = 0; r < repeats; r++) {
    noInterrupts();
    bool level = true;
    for (uint16_t duration : pulses) {
      digitalWrite(pin, level ? HIGH : LOW);
      delayMicroseconds(duration);
      level = !level;
    }
    digitalWrite(pin, LOW);
    interrupts();
    App.feed_wdt();
  }
  this->rx_start_();
}

// --------------------------------------------------
// Somfy-RTS-Dekoder
// Frame: Software-Sync (~4550 µs high, 640 µs low), dann 56 Bit Manchester
// mit 640 µs Halbbit. 1 = low->high, 0 = high->low.
// --------------------------------------------------

void SomfyRtsHub::loop() {
  while (rx_tail != rx_head) {
    uint16_t value = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    this->decode_pulse_(value >> 15, value & 0x7FFF);
    this->pulse_callback_.call(value >> 15, value & 0x7FFF);
  }
  this->check_pending_my_();
}

static bool is_software_sync(bool level, uint16_t duration) {
  return level && duration >= 3500 && duration <= 6500;  // Hardware-Sync (2560 µs) liegt sicher darunter
}

void SomfyRtsHub::decode_pulse_(bool level, uint16_t duration) {
  if (!this->dec_.active) {
    if (is_software_sync(level, duration)) {
      this->dec_.active = true;
      this->dec_.skip_sync_low = true;
      this->dec_.count = 0;
    }
    return;
  }

  int units;
  if (duration >= 350 && duration < 950) {
    units = 1;
  } else if (duration >= 950 && duration <= 1700) {
    units = 2;
  } else if (!level && duration > 1700 && this->dec_.count == 111) {
    units = 1;  // letztes Halbbit (low) geht in die Pause nach dem Frame über
  } else {
    this->dec_.active = false;
    this->decode_pulse_(level, duration);  // könnte der nächste Sync sein
    return;
  }

  for (int i = 0; i < units; i++) {
    if (this->dec_.skip_sync_low) {
      // Das 640-µs-Low des Syncs verschmilzt ggf. mit dem ersten Halbbit
      this->dec_.skip_sync_low = false;
      if (level) {
        this->dec_.active = false;
        return;
      }
      continue;
    }
    this->dec_.half[this->dec_.count++] = level;
    if (this->dec_.count == 112) {
      this->decode_frame_();
      this->dec_.active = false;
      return;
    }
  }
}

void SomfyRtsHub::decode_frame_() {
  uint8_t raw[7] = {0};
  for (int bit = 0; bit < 56; bit++) {
    uint8_t first = this->dec_.half[2 * bit];
    uint8_t second = this->dec_.half[2 * bit + 1];
    if (first == second)
      return;  // kein gültiges Manchester-Symbol
    if (!first && second)
      raw[bit / 8] |= 0x80 >> (bit % 8);
  }

  // Obfuskierung rückgängig machen
  uint8_t frame[7];
  frame[0] = raw[0];
  for (int i = 1; i < 7; i++)
    frame[i] = raw[i] ^ raw[i - 1];

  // Prüfsumme: XOR aller Nibbles muss 0 ergeben
  uint8_t checksum = 0;
  for (int i = 0; i < 7; i++)
    checksum ^= frame[i] ^ (frame[i] >> 4);
  if ((checksum & 0x0F) != 0)
    return;
  if ((frame[0] >> 4) != 0xA)
    return;

  uint8_t cmd = frame[1] >> 4;
  uint16_t rolling = ((uint16_t) frame[2] << 8) | frame[3];
  uint32_t address = ((uint32_t) frame[4] << 16) | ((uint32_t) frame[5] << 8) | frame[6];
  this->on_frame_(address, cmd, rolling);
}

void SomfyRtsHub::on_frame_(uint32_t address, uint8_t cmd, uint16_t rolling) {
  uint32_t now = millis();
  // Jede Taste wird mehrfach mit demselben Rolling Code wiederholt (bei Halten dauerhaft)
  if (address == this->last_address_ && rolling == this->last_rolling_ && now - this->last_time_ < 3000) {
    this->last_time_ = now;
    if (this->pending_my_.active && this->pending_my_.address == address && this->pending_my_.rolling == rolling)
      this->pending_my_.last_seen = now;
    return;
  }
  this->last_address_ = address;
  this->last_rolling_ = rolling;
  this->last_time_ = now;

  char remote[8];
  snprintf(remote, sizeof(remote), "%06X", (unsigned) address);
  ESP_LOGI(TAG, "Empfangen: Fernbedienung %s, Taste %s, Code %u", remote, command_name(cmd), rolling);
  this->receive_callback_.call(remote, command_name(cmd), rolling);

  bool known = false;
  bool any_stationary = false;
  for (auto *cover : this->covers_) {
    if (!cover->listens_to(address))
      continue;
    known = true;
    if (static_cast<SomfyCommand>(cmd) == SomfyCommand::MY && !cover->is_moving()) {
      any_stationary = true;  // kurz oder lang? -> erst beim Loslassen entscheiden
    } else {
      cover->apply_command(static_cast<SomfyCommand>(cmd), now);
    }
  }

  if (any_stationary) {
    this->pending_my_.active = true;
    this->pending_my_.address = address;
    this->pending_my_.rolling = rolling;
    this->pending_my_.first_seen = this->pending_my_.last_seen = now;
  }

  if (!known)
    ESP_LOGI(TAG, "  keinem Rollo zugeordnet - unter 'remotes:' beim Rollo eintragen");
}

void SomfyRtsHub::check_pending_my_() {
  if (!this->pending_my_.active || millis() - this->pending_my_.last_seen < 600)
    return;
  this->pending_my_.active = false;

  bool long_press = this->pending_my_.last_seen - this->pending_my_.first_seen >= MY_LONG_PRESS_MS;
  for (auto *cover : this->covers_) {
    if (!cover->listens_to(this->pending_my_.address) || cover->is_moving())
      continue;
    cover->remote_my_released(long_press, this->pending_my_.first_seen);
  }
}

}  // namespace somfy_rts
}  // namespace esphome
