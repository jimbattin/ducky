// MIDI channel-message parsing for the expression pedal and the preset-select CC (docs/MIDI.md "Controls"): Control
// Change extraction from a raw MIDI byte stream. Host-portable (no libDaisy, no allocation), covered by
// tests/midi_test.cpp.
// libDaisy's MidiUsbTransport delivers the raw bytes of each USB-MIDI packet with the CIN stripped
// (libDaisy/src/hid/usb_midi.cpp), so running status is handled here.
#pragma once

#include <cstdint>

namespace dkmidi {

constexpr uint8_t kExpressionCc = 11;         // MIDI's Expression controller: the exp control slot's position
constexpr uint8_t kPresetCc = 102;            // an undefined controller: value 0-7 selects slot 1-8
constexpr uint8_t kOmni = 0, kChannels = 16;  // a channel setting (EXP CHANNEL): 0 any, 1-16 that channel

// A CC on channel (0-15) counts for the setting (kOmni or 1-16).
constexpr bool ChannelMatches(uint8_t setting, uint8_t channel) { return setting == kOmni || channel + 1 == setting; }

struct Cc {
    uint8_t channel, number, value;  // channel 0-15
};

// Emits every complete Control Change (Bn cc v, running status included). Other channel messages' data bytes are
// consumed by length (8n/9n/An/En: 2, Cn/Dn: 1), never read as CC. Realtime bytes (>= F8) are ignored anywhere.
// F0 enters SysEx: data bytes are ignored until F7 or another status byte. F1-F7 clear running status; F1/F3
// swallow 1 data byte, F2 swallows 2.
class CcParser {
public:
    bool Feed(uint8_t b, Cc& out) {
        if (b >= 0xF8) return false;
        if (b >= 0x80) {
            sysex_ = b == 0xF0;
            count_ = 0;
            if (b >= 0xF0) {
                status_ = 0;
                swallow_ = b == 0xF2 ? 2 : (b == 0xF1 || b == 0xF3) ? 1 : 0;
            } else {
                status_ = b;
                swallow_ = 0;
                need_ = (b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0 ? 1 : 2;
            }
            return false;
        }
        if (sysex_) return false;
        if (swallow_ > 0) {
            --swallow_;
            return false;
        }
        if (status_ == 0) return false;
        data_[count_++] = b;
        if (count_ < need_) return false;
        count_ = 0;
        if ((status_ & 0xF0) != 0xB0) return false;
        out = {uint8_t(status_ & 0x0F), data_[0], data_[1]};
        return true;
    }

private:
    uint8_t status_ = 0, need_ = 2, count_ = 0, swallow_ = 0;
    uint8_t data_[2] = {0, 0};
    bool sysex_ = false;
};

}  // namespace dkmidi
