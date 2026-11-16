// The pedal's link over the Seed's USB MIDI port: SysEx `F0 7D 64 01 <cmd> <payload> F7` (manufacturer 0x7D =
// non-commercial, device 0x64, version 0x01), packed as USB-MIDI event packets. Host-portable (no libDaisy), so the
// host tests (tests/link_test.cpp) cover it. Read the log with tools/usb_log.py; flash with tools/flash_when_ready.sh.
//
// Host -> pedal:
//   01 HELLO                          (no payload) the pedal answers with LOG lines: the boot line
//                                     `ducky <board> seed <version> sysclk N MHz audio N Hz N bit block N <build id>`,
//                                     then the `slots`, `preset`, `model` and `ir` lines (docs/FIRMWARE.md "Log lines").
//   06 DFU                            (no payload) the pedal logs `dfu reset` and restarts into the Daisy bootloader's
//                                     DFU mode (no timeout), ready for tools/flash_when_ready.sh.
//   Any other command with this header logs `link: unknown command CC`. SysEx for other devices (e.g. a flasher
//   broadcasting DFU frames for several devices) is ignored.
//   Identity Request F0 7E <id> 06 01 F7 (Universal Non-Real-Time, any id 00-7F): the pedal answers the Identity
//                                     Reply below.
// Pedal -> host:
//   12 LOG <printable ASCII <= 120>   one log line (boot, the controls and status lines once a second).
//   Identity Reply F0 7E 7F 06 02 7D 64 00 <board> 00 00 00 00 00 <build id> F7: manufacturer 7D, family 64 00
//                                     (device byte 64, then 00), member <board> 00 (1 HotHouse, board.h), version
//                                     00 00 00 00, then the build id: 1-32 printable ASCII bytes, the short git
//                                     commit with `+dirty` when tracked files differ (e.g. `711fa7c+dirty`), or
//                                     `unknown` outside git (daisy/Makefile writes it to $(BUILD_DIR)/build_id.c).
#pragma once

#include <cstddef>
#include <cstdint>

namespace dklink {

constexpr uint8_t kManufacturer = 0x7D, kDevice = 0x64, kVersion = 0x01;
constexpr uint8_t kHello = 0x01, kDfu = 0x06;  // host -> pedal
constexpr uint8_t kLog = 0x12;                 // pedal -> host
constexpr size_t kMaxLog = 120, kMaxBuildId = 32;

class TxBuffer {
public:
    static constexpr size_t kCapacity = 1024;

    // Printable ASCII; other bytes become '?'. Truncated to kMaxLog chars.
    bool AppendLog(const char* text) { return AppendText(kLog, text, kMaxLog); }
    // Identity Reply: board, then buildId (printable ASCII; other bytes become '?'; truncated to kMaxBuildId chars;
    // empty becomes "unknown").
    bool AppendIdentity(uint8_t board, const char* buildId) {
        uint8_t msg[14 + kMaxBuildId + 1] = {0xF0, 0x7E, 0x7F, 0x06, 0x02, kManufacturer, kDevice, 0x00, board,
                                            0x00, 0x00, 0x00, 0x00, 0x00};
        size_t len = 14;
        if (!buildId || !buildId[0]) buildId = "unknown";
        for (size_t i = 0; i < kMaxBuildId && buildId[i]; i++) msg[len++] = Printable(uint8_t(buildId[i]));
        msg[len++] = 0xF7;
        return AppendRaw(msg, len);
    }

    const uint8_t* data() const { return buf_; }
    size_t size() const { return size_; }
    void Clear() { size_ = 0; }

private:
    static uint8_t Printable(uint8_t c) { return c >= 0x20 && c <= 0x7E ? c : uint8_t('?'); }

    // All or nothing: false (and nothing appended) if the message does not fit.
    bool AppendSysEx(uint8_t cmd, const uint8_t* payload, size_t n) {
        uint8_t msg[6 + kMaxLog];
        if (n > kMaxLog) return false;
        size_t len = 0;
        msg[len++] = 0xF0;
        msg[len++] = kManufacturer;
        msg[len++] = kDevice;
        msg[len++] = kVersion;
        msg[len++] = cmd;
        for (size_t i = 0; i < n; i++) msg[len++] = payload[i];
        msg[len++] = 0xF7;
        return AppendRaw(msg, len);
    }
    // A complete F0 ... F7 message (n >= 2), packetized as is. All or nothing.
    bool AppendRaw(const uint8_t* msg, size_t n) {
        if (n < 2 || size_ + (n + 2) / 3 * 4 > kCapacity) return false;
        size_t i = 0;
        for (; n - i > 3; i += 3) Packet(0x04, msg + i, 3);
        Packet(uint8_t(0x04 + (n - i)), msg + i, n - i);  // 1/2/3 -> CIN 5/6/7
        return true;
    }
    void Packet(uint8_t cin, const uint8_t* b, size_t n) {
        uint8_t* q = buf_ + size_;
        q[0] = cin;
        for (size_t i = 0; i < 3; i++) q[1 + i] = i < n ? b[i] : 0;
        size_ += 4;
    }
    bool AppendText(uint8_t cmd, const char* text, size_t max) {
        uint8_t p[kMaxLog];
        size_t n = 0;
        for (; n < max && text[n]; n++) p[n] = Printable(uint8_t(text[n]));
        return AppendSysEx(cmd, p, n);
    }

    uint8_t buf_[kCapacity];
    size_t size_ = 0;
};

// Reassembles SysEx messages from a MIDI byte stream. Realtime bytes (>= F8) are ignored anywhere; F0 starts a new
// message (dropping an unfinished one); any other status byte aborts the message; a message longer than N bytes is
// dropped up to the next F0.
template <size_t N> class SysExAssembler {
public:
    // True when data()/size() hold a complete F0 ... F7 message; valid until the next Feed.
    bool Feed(uint8_t b) {
        if (b >= 0xF8) return false;
        if (b == 0xF0) {
            buf_[0] = b;
            size_ = 1;
            in_ = true;
            return false;
        }
        if (!in_) return false;
        if (b >= 0x80 && b != 0xF7) {
            in_ = false;
            return false;
        }
        if (size_ == N) {
            in_ = false;
            return false;
        }
        buf_[size_++] = b;
        if (b != 0xF7) return false;
        in_ = false;
        return true;
    }
    const uint8_t* data() const { return buf_; }
    size_t size() const { return size_; }

private:
    uint8_t buf_[N];
    size_t size_ = 0;
    bool in_ = false;
};

// A complete message from SysExAssembler is the Universal Non-Real-Time Identity Request F0 7E <id> 06 01 F7 (any id).
inline bool IsIdentityRequest(const uint8_t* m, size_t n) {
    return n == 6 && m[0] == 0xF0 && m[1] == 0x7E && m[2] <= 0x7F && m[3] == 0x06 && m[4] == 0x01 && m[5] == 0xF7;
}

// The command byte of a complete message with this pedal's header (F0 7D 64 01 <cmd> ... F7), else -1 (including
// other devices' frames, e.g. another device's DFU frame, and other versions).
inline int PedalCommand(const uint8_t* m, size_t n) {
    return n >= 6 && m[1] == kManufacturer && m[2] == kDevice && m[3] == kVersion ? m[4] : -1;
}

}  // namespace dklink
