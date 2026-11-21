#include "somfy_rts_cover.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace somfy_rts {

static const char *const TAG = "somfy_rts.cover";

void SomfyRtsCover::setup() {
  auto restore = this->restore_state_();
  if (restore.has_value()) {
    restore->apply(this);
    this->pos_ = this->position;
  }
  this->current_operation = cover::COVER_OPERATION_IDLE;

  this->my_pref_ = global_preferences->make_preference<float>(fnv1_hash("somfy_my_" + this->storage_key_));
  float stored;
  if (this->my_pref_.load(&stored))
    this->my_position_ = stored;

  this->publish_(false);
}

void SomfyRtsCover::dump_config() {
  LOG_COVER("", "Somfy RTS Rollo", this);
  ESP_LOGCONFIG(TAG, "  Adresse: %06X, NVS-Key: %s, naechster Rolling Code: %u", (unsigned) this->address_,
                this->storage_key_.c_str(), this->hub_->peek_rolling_code(this->storage_key_, this->min_rolling_code_));
  ESP_LOGCONFIG(TAG, "  Fahrzeit auf %.1f s / ab %.1f s", this->open_duration_ / 1000.0f,
                this->close_duration_ / 1000.0f);
  if (this->my_position_ >= 0)
    ESP_LOGCONFIG(TAG, "  MY-Position: %.0f %%", this->my_position_ * 100);
  for (uint32_t remote : this->remotes_)
    ESP_LOGCONFIG(TAG, "  Original-Fernbedienung: %06X", (unsigned) remote);
}

cover::CoverTraits SomfyRtsCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(true);  // RTS hat keine Rückmeldung
  return traits;
}

uint32_t SomfyRtsCover::send_(SomfyCommand command, int repeats) {
  return this->hub_->transmit(this->address_, this->storage_key_, this->min_rolling_code_, command, repeats);
}

bool SomfyRtsCover::listens_to(uint32_t address) const {
  for (uint32_t remote : this->remotes_) {
    if (remote == address)
      return true;
  }
  return false;
}

// --------------------------------------------------
// Positionsmodell
// --------------------------------------------------

float SomfyRtsCover::position_at_(uint32_t when) const {
  if (this->direction_ == 0)
    return this->pos_;
  if ((int32_t) (when - this->start_time_) < 0)
    return this->start_pos_;
  float travel = this->direction_ > 0 ? this->open_duration_ : this->close_duration_;
  float p = this->start_pos_ + this->direction_ * (when - this->start_time_) / travel;
  return clamp(p, 0.0f, 1.0f);
}

void SomfyRtsCover::publish_(bool save) {
  this->last_publish_ = millis();
  this->position = roundf(this->position_at_(millis()) * 100) / 100;
  if (this->direction_ > 0) {
    this->current_operation = cover::COVER_OPERATION_OPENING;
  } else if (this->direction_ < 0) {
    this->current_operation = cover::COVER_OPERATION_CLOSING;
  } else {
    this->current_operation = cover::COVER_OPERATION_IDLE;
  }
  this->publish_state(save);
}

void SomfyRtsCover::start_move_(int8_t dir, float target, bool stop_by_self, uint32_t when) {
  this->pos_ = this->position_at_(when);
  this->direction_ = dir;
  this->start_pos_ = this->pos_;
  this->start_time_ = when;
  this->target_ = target;
  this->stop_by_self_ = stop_by_self;
  this->publish_(false);
}

void SomfyRtsCover::stop_move_(uint32_t when) { this->finish_move_(this->position_at_(when)); }

void SomfyRtsCover::finish_move_(float position) {
  this->pos_ = position;
  this->direction_ = 0;
  this->target_ = -1;
  this->publish_(true);
}

// Überträgt einen gesendeten oder empfangenen Befehl ins Positionsmodell
void SomfyRtsCover::apply_command(SomfyCommand command, uint32_t when) {
  switch (command) {
    case SomfyCommand::UP:
      this->start_move_(+1, -1, false, when);
      break;
    case SomfyCommand::DOWN:
      this->start_move_(-1, -1, false, when);
      break;
    case SomfyCommand::MY:
      if (this->direction_ != 0) {
        this->stop_move_(when);
      } else if (this->my_position_ >= 0) {
        float diff = this->my_position_ - this->pos_;
        if (fabsf(diff) >= 0.01f)
          this->start_move_(diff > 0 ? +1 : -1, this->my_position_, false, when);
      } else {
        ESP_LOGW(TAG, "%s: MY-Position unbekannt - mit 'MY-Position speichern' festlegen.", this->get_name().c_str());
      }
      break;
    default:
      break;
  }
}

void SomfyRtsCover::remote_my_released(bool long_press, uint32_t when) {
  if (long_press) {
    this->set_my_position(this->pos_);
  } else {
    this->apply_command(SomfyCommand::MY, when);
  }
}

void SomfyRtsCover::loop() {
  if (this->direction_ == 0)
    return;
  uint32_t now = millis();

  if (this->target_ >= 0) {
    // STOP so früh senden, dass der Motor genau am Ziel anhält
    float p = this->position_at_(now + (this->stop_by_self_ ? MOTOR_REACTION_MS : 0));
    bool reached = this->direction_ > 0 ? p >= this->target_ : p <= this->target_;
    if (reached) {
      if (this->stop_by_self_)
        this->send_(SomfyCommand::MY);
      this->finish_move_(this->target_);
      return;
    }
  }

  float p = this->position_at_(now);
  if ((this->direction_ > 0 && p >= 1.0f) || (this->direction_ < 0 && p <= 0.0f)) {
    this->finish_move_(this->direction_ > 0 ? 1.0f : 0.0f);
    return;
  }

  if (now - this->last_publish_ >= 1000)
    this->publish_(false);
}

// --------------------------------------------------
// Befehle aus Home Assistant
// --------------------------------------------------

void SomfyRtsCover::control(const cover::CoverCall &call) {
  if (call.get_stop())
    this->apply_command(SomfyCommand::MY, this->send_(SomfyCommand::MY));
  if (call.get_position().has_value())
    this->move_to_(*call.get_position());
}

void SomfyRtsCover::move_to_(float target) {
  target = clamp(target, 0.0f, 1.0f);
  ESP_LOGD(TAG, "%s -> %.0f %%", this->get_name().c_str(), target * 100);

  if (target >= 1.0f) {
    this->apply_command(SomfyCommand::UP, this->send_(SomfyCommand::UP));
    return;
  }
  if (target <= 0.0f) {
    this->apply_command(SomfyCommand::DOWN, this->send_(SomfyCommand::DOWN));
    return;
  }

  float current = this->position_at_(millis());
  if (fabsf(target - current) < 0.01f)
    return;
  int8_t dir = target > current ? +1 : -1;

  if (this->direction_ == dir) {
    // fährt schon in die richtige Richtung -> nur Ziel setzen
    this->target_ = target;
    this->stop_by_self_ = true;
    return;
  }

  SomfyCommand command = dir > 0 ? SomfyCommand::UP : SomfyCommand::DOWN;
  this->start_move_(dir, target, true, this->send_(command));
}

void SomfyRtsCover::prog() {
  ESP_LOGI(TAG, "%s: PROG senden (am Motor anlernen)", this->get_name().c_str());
  this->send_(SomfyCommand::PROG);
}

void SomfyRtsCover::goto_my() {
  if (this->direction_ != 0) {
    // Fährt gerade: erstes MY stoppt, zweites fährt zur MY-Position
    this->stop_move_(this->send_(SomfyCommand::MY));
    delay(500);
  }
  this->apply_command(SomfyCommand::MY, this->send_(SomfyCommand::MY));
}

void SomfyRtsCover::save_my() {
  if (this->direction_ != 0)
    this->stop_move_(this->send_(SomfyCommand::MY));
  ESP_LOGI(TAG, "%s: MY lang druecken (speichern) ...", this->get_name().c_str());
  this->send_(SomfyCommand::MY, MY_SAVE_REPEATS);
  this->set_my_position(this->pos_);
}

void SomfyRtsCover::set_my_position(float position) {
  this->my_position_ = clamp(position, 0.0f, 1.0f);
  this->my_pref_.save(&this->my_position_);
  ESP_LOGI(TAG, "%s: MY-Position = %.0f %%", this->get_name().c_str(), this->my_position_ * 100);
}

}  // namespace somfy_rts
}  // namespace esphome
