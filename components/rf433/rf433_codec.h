#pragma once

// Kodierung und Dekodierung der 433,92-MHz-Protokolle PT2262 und AC (KlikAanKlikUit/Intertechno,
// selbstlernend). Bewusst ohne ESPHome-Abhaengigkeiten, nur Pulsdauern rein und raus.

#include <cstdint>
#include <vector>

namespace esphome {
namespace rf433 {

enum class Protocol : uint8_t { PT2262 = 0, AC = 1 };

struct Message {
  Protocol protocol{Protocol::PT2262};
  uint32_t code{0};    // PT2262: 24-Bit-Code, AC: 26-Bit-Adresse
  uint8_t unit{0};     // nur AC: 1..16
  bool group{false};   // nur AC: Gruppenbefehl (alle Units der Adresse)
  bool on{false};      // nur AC
  uint16_t pulse{0};   // gemessene Grundpulslaenge T in µs

  bool same_as(const Message &o) const {
    return protocol == o.protocol && code == o.code && unit == o.unit && group == o.group && on == o.on;
  }
};

// --------------------------------------------------
// PT2262 (auch EV1527, SC5262 ...): 24 Bit, danach Sync.
// Bit 0 = 1T high + 3T low, Bit 1 = 3T high + 1T low, Sync = 1T high + 31T low.
// --------------------------------------------------

// Haengt einen Frame an "out" an: abwechselnd High- und Low-Dauer in µs, beginnend mit High
inline void encode_pt2262(uint32_t code, uint16_t t, std::vector<uint16_t> &out) {
  for (int i = 23; i >= 0; i--) {
    if ((code >> i) & 1) {
      out.push_back(3 * t);
      out.push_back(t);
    } else {
      out.push_back(t);
      out.push_back(3 * t);
    }
  }
  out.push_back(t);
  out.push_back(31 * t);
}

class Pt2262Decoder {
 public:
  // level = Pegel, der gerade zu Ende gegangen ist. true, sobald ein Frame vollstaendig ist.
  bool feed(bool level, uint16_t duration, Message &out) {
    if (level) {
      this->high_ = duration;
      return false;
    }
    uint16_t h = this->high_;
    this->high_ = 0;
    if (h == 0) {
      this->count_ = -1;
      return false;
    }

    if (duration >= 2500 && duration >= 8u * h) {
      // Sync: beendet einen Frame und startet den naechsten
      bool done = this->count_ == 24 && h >= 100 && h <= 900;
      if (done) {
        out = Message();
        out.protocol = Protocol::PT2262;
        out.code = this->code_;
        out.pulse = this->sum_ / 96;  // 24 Bit zu je 4T
      }
      this->count_ = 0;
      this->code_ = 0;
      this->sum_ = 0;
      return done;
    }

    if (this->count_ < 0 || this->count_ >= 24 || h < 100 || duration < 100 || h > 2500 || duration > 2500) {
      this->count_ = -1;
      return false;
    }

    // Soll-Verhaeltnis 1:3 bzw. 3:1, akzeptiert wird 1,8 bis 5
    uint32_t bit;
    if (duration * 10u >= h * 18u && duration <= 5u * h) {
      bit = 0;
    } else if (h * 10u >= duration * 18u && h <= 5u * duration) {
      bit = 1;
    } else {
      this->count_ = -1;
      return false;
    }
    this->code_ = (this->code_ << 1) | bit;
    this->sum_ += h + duration;
    this->count_++;
    return false;
  }

 protected:
  uint16_t high_{0};
  int count_{-1};  // -1 = wartet auf Sync
  uint32_t code_{0};
  uint32_t sum_{0};
};

// --------------------------------------------------
// AC: Start = 1T high + 10T low, dann 32 Bit, Stopp = 1T high + 40T low.
// Jedes Bit sind zwei Pulspaare: 0 = (T,T)(T,5T), 1 = (T,5T)(T,T).
// Bits: 26 Adresse, 1 Gruppe, 1 Ein/Aus, 4 Unit.
// --------------------------------------------------

inline void encode_ac(uint32_t address, uint8_t unit, bool group, bool on, uint16_t t, std::vector<uint16_t> &out) {
  uint32_t value = ((address & 0x3FFFFFF) << 6) | (group ? 0x20 : 0) | (on ? 0x10 : 0) | ((unit - 1) & 0x0F);
  out.push_back(t);
  out.push_back(10 * t);
  for (int i = 31; i >= 0; i--) {
    bool bit = (value >> i) & 1;
    out.push_back(t);
    out.push_back(bit ? 5 * t : t);
    out.push_back(t);
    out.push_back(bit ? t : 5 * t);
  }
  out.push_back(t);
  out.push_back(40 * t);
}

class AcDecoder {
 public:
  bool feed(bool level, uint16_t duration, Message &out) {
    if (level) {
      this->high_ = duration;
      return false;
    }
    uint16_t h = this->high_;
    this->high_ = 0;
    if (h < 100 || h > 700) {
      this->count_ = -1;
      return false;
    }
    if (duration >= 1950 && duration <= 3500) {  // Startpuls
      this->count_ = 0;
      this->sum_ = 0;
      return false;
    }
    if (this->count_ < 0)
      return false;

    uint8_t symbol;
    if (duration < 750) {
      symbol = 0;  // kurz
    } else if (duration >= 800 && duration <= 1850) {
      symbol = 1;  // lang
    } else {
      this->count_ = -1;
      return false;
    }
    this->symbols_[this->count_++] = symbol;
    this->sum_ += h;
    if (this->count_ < 64)
      return false;

    this->count_ = -1;
    uint32_t value = 0;
    for (int i = 0; i < 32; i++) {
      uint8_t first = this->symbols_[2 * i];
      uint8_t second = this->symbols_[2 * i + 1];
      if (first == second)
        return false;  // kein gueltiges Ein/Aus-Symbol
      value = (value << 1) | first;
    }
    out = Message();
    out.protocol = Protocol::AC;
    out.code = value >> 6;
    out.group = (value >> 5) & 1;
    out.on = (value >> 4) & 1;
    out.unit = (value & 0x0F) + 1;
    out.pulse = this->sum_ / 64;
    return true;
  }

 protected:
  uint16_t high_{0};
  int count_{-1};  // -1 = wartet auf Startpuls
  uint8_t symbols_[64];
  uint32_t sum_{0};
};

// --------------------------------------------------
// Hideki (Wettersensoren von TFA, Technoline, Cresta, Bresser ...), nur Empfang.
// Differentielles Manchester mit ~490 µs Halbbit: ein langer Puls (~980 µs) = 1,
// zwei kurze Pulse = 0, High und Low zaehlen gleich.
// Jedes Byte: 8 Bit, niederwertigstes zuerst, dann ein Paritaetsbit (gerade Paritaet).
// Paket: 0x9F, Kanal/Kennung, Laenge, Typ, Daten ..., XOR-Pruefsumme, CRC-8.
// --------------------------------------------------

struct HidekiPacket {
  uint8_t data[20];
  uint8_t length{0};  // Anzahl gueltiger Bytes in data
  bool xor_ok{false};
  bool crc_ok{false};
};

class HidekiDecoder {
 public:
  bool feed(bool level, uint16_t duration, HidekiPacket &out) {
    if (duration >= 300 && duration <= 750) {
      if (this->pending_short_) {
        this->pending_short_ = false;
        this->push_(0);
      } else {
        this->pending_short_ = true;
      }
      return false;
    }
    if (duration > 750 && duration <= 1300 && !this->pending_short_) {
      this->push_(1);
      return false;
    }

    // Alles andere beendet die Pulsfolge. Das letzte Low geht in der Pause nach dem Paket auf.
    if (!level && duration > 750)
      this->push_(this->pending_short_ ? 0 : 1);
    bool found = this->parse_(out);
    this->count_ = 0;
    this->pending_short_ = false;
    return found;
  }

 protected:
  void push_(uint8_t bit) {
    if (this->count_ < sizeof(this->bits_))
      this->bits_[this->count_++] = bit;
  }

  bool parse_(HidekiPacket &out) {
    // Kopfbyte 0x9F mit Paritaet, niederwertigstes Bit zuerst
    static const uint8_t HEADER[9] = {1, 1, 1, 1, 1, 0, 0, 1, 0};
    if (this->count_ < 4 * 9)
      return false;
    int start = -1;
    for (int i = 0; i + 9 <= (int) this->count_ && i < 24; i++) {
      bool match = true;
      for (int k = 0; k < 9 && match; k++)
        match = this->bits_[i + k] == HEADER[k];
      if (match) {
        start = i;
        break;
      }
    }
    if (start < 0)
      return false;

    out = HidekiPacket();
    int available = (this->count_ - start) / 9;
    int total = 3;  // steht nach dem Laengenbyte fest
    for (int n = 0; n < total; n++) {
      if (n >= available || n >= (int) sizeof(out.data))
        return false;
      uint8_t value = 0, ones = 0;
      for (int k = 0; k < 9; k++) {
        uint8_t bit = this->bits_[start + 9 * n + k];
        ones += bit;
        if (k < 8)
          value |= bit << k;
      }
      if (ones & 1)
        return false;  // Paritaetsfehler
      out.data[n] = value;
      if (n == 2)
        total = ((value >> 1) & 0x1F) + 3;
    }
    out.length = total;

    uint8_t x = 0;
    for (int n = 1; n < total - 1; n++)
      x ^= out.data[n];
    out.xor_ok = x == 0;

    // CRC-8 ueber alles nach dem Kopfbyte bis einschliesslich XOR-Pruefsumme,
    // niederwertigstes Bit zuerst (Polynom 0x07 gespiegelt = 0xE0), Startwert 0
    uint8_t crc = 0;
    for (int n = 1; n < total - 1; n++) {
      crc ^= out.data[n];
      for (int k = 0; k < 8; k++)
        crc = (crc & 1) ? (uint8_t) ((crc >> 1) ^ 0xE0) : (uint8_t) (crc >> 1);
    }
    out.crc_ok = crc == out.data[total - 1];
    return true;
  }

  uint8_t bits_[216];
  uint16_t count_{0};
  bool pending_short_{false};
};

// --------------------------------------------------
// Oregon Scientific Version 2.1 und 3.0, nur Empfang.
// Manchester mit 1024 Hz: kurze Pulse ~490 µs, lange ~980 µs.
// v3: Praeambel 24 Einsen, Sync-Nibble 0xA, dann Nibbles (niederwertigstes Bit zuerst).
// v2.1: wie v3, aber jedes Bit wird doppelt gesendet (erst invertiert, dann richtig),
//       Praeambel 16 Einsen.
// Nibbles: 4 Kennung, 1 Kanal, 2 Rolling Code, 1 Flags, Daten ..., 2 Pruefsumme (Summe aller Nibbles davor).
// --------------------------------------------------

struct OregonPacket {
  uint8_t version{0};  // 2 oder 3
  uint8_t nibbles[40];
  uint8_t count{0};
  int8_t checksum_at{-1};  // Index des ersten Pruefsummen-Nibbles, -1 = keine passende Pruefsumme

  uint16_t sensor_id() const {
    return (nibbles[0] << 12) | (nibbles[1] << 8) | (nibbles[2] << 4) | nibbles[3];
  }
};

class OregonDecoder {
 public:
  bool feed(bool level, uint16_t duration, OregonPacket &out) {
    if (duration >= 200 && duration <= 1350) {
      if (this->count_ < sizeof(this->symbols_))
        this->symbols_[this->count_++] = duration > 730 ? 1 : 0;  // 1 = lang
      return false;
    }
    bool found = this->count_ >= 60 && this->decode_(!level && duration > 1350, out);
    this->count_ = 0;
    return found;
  }

 protected:
  bool decode_(bool trailing_low, OregonPacket &out) {
    // Manchester: zwei kurze Pulse = gleiches Bit wie vorher, ein langer = Bit wechselt.
    // Die kurzen Pulse vor dem ersten langen muessen paarweise aufgehen.
    static uint8_t raw[sizeof(symbols_) + 1];
    uint16_t first_long = 0;
    while (first_long < this->count_ && !this->symbols_[first_long])
      first_long++;
    uint16_t n = 0;
    uint8_t bit = 1;
    bool half = false;
    for (uint16_t i = first_long & 1; i < this->count_; i++) {
      if (this->symbols_[i]) {
        if (half)
          break;
        bit ^= 1;
        raw[n++] = bit;
      } else if (half) {
        half = false;
        raw[n++] = bit;
      } else {
        half = true;
      }
    }
    if (trailing_low && n < sizeof(raw))
      raw[n++] = half ? bit : (bit ^ 1);

    if (this->parse_(raw, n, 3, out))
      return true;

    // v2.1: Bitpaare (invertiert, richtig); beide moeglichen Paargrenzen probieren
    static uint8_t data[sizeof(raw) / 2 + 1];
    for (uint16_t phase = 0; phase < 2; phase++) {
      uint16_t m = 0;
      for (uint16_t i = phase; i + 1 < n; i += 2) {
        if (raw[i] == raw[i + 1])
          break;
        data[m++] = raw[i + 1];
      }
      if (m > 0 && data[0] == 0) {
        for (uint16_t i = 0; i < m; i++)
          data[i] ^= 1;
      }
      if (this->parse_(data, m, 2, out))
        return true;
    }
    return false;
  }

  bool parse_(const uint8_t *bits, uint16_t n, uint8_t version, OregonPacket &out) {
    // Praeambel: mindestens 8 Einsen, dann Sync 0101
    uint16_t ones = 0;
    while (ones < n && bits[ones])
      ones++;
    if (ones < 8 || ones + 4 > n)
      return false;
    if (!(bits[ones] == 0 && bits[ones + 1] == 1 && bits[ones + 2] == 0 && bits[ones + 3] == 1))
      return false;

    out = OregonPacket();
    out.version = version;
    for (uint16_t pos = ones + 4; pos + 4 <= n && out.count < sizeof(out.nibbles); pos += 4)
      out.nibbles[out.count++] = bits[pos] | (bits[pos + 1] << 1) | (bits[pos + 2] << 2) | (bits[pos + 3] << 3);
    if (out.count < 10)
      return false;

    uint8_t sum = 0;
    for (uint8_t k = 0; k + 1 < out.count; k++) {
      if (k >= 8 && (out.nibbles[k] | (out.nibbles[k + 1] << 4)) == sum) {
        out.checksum_at = k;
        break;
      }
      sum += out.nibbles[k];
    }
    return true;
  }

  uint8_t symbols_[480];
  uint16_t count_{0};
};

}  // namespace rf433
}  // namespace esphome
