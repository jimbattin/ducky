// UPLOAD BEGIN / CHUNK / COMMIT's staging (link.h, docs/MIDI.md "Upload"): one preset image assembled from CRC-checked
// chunks into a caller-provided buffer; FIRMWARE BEGIN / CHUNK / COMMIT's likewise, for a firmware image
// (fw_image.h, docs/MIDI.md "Firmware update"). Host-portable, covered by tests/upload_test.cpp. Main loop only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "binary.h"
#include "fw_image.h"
#include "link.h"
#include "preset.h"

namespace dklink {

static_assert(dkfwimg::kMinImageBytes == 1040 && dkfwimg::kMaxImageBytes == 491520, "kCommands' FIRMWARE BEGIN text");

// CHUNK's payload `<offset u21> <crc32 u32> <Pack7 data>` into buf, an image of size bytes whose next expected offset
// is next. A chunk at or below next is (re)written, so a resent chunk is idempotent; one past next is kOffset.
inline Result StageChunk(uint8_t* buf, uint32_t size, uint32_t& next, const uint8_t* p, size_t n) {
    if (n < 3 + 5 + 2 || n - 8 > Packed7Size(kChunkBytes)) return Result::kBadRequest;
    uint8_t data[kChunkBytes];
    size_t len = 0;
    if (!Unpack7(p + 8, n - 8, data, len) || len == 0 || len > kChunkBytes) return Result::kBadRequest;
    if (dkbin::Crc32(data, len) != GetSeptets(p + 3, 5)) return Result::kCrc;
    const uint32_t offset = GetSeptets(p, 3);
    if (offset > next) return Result::kOffset;
    if (offset + len > size) return Result::kBadRequest;
    std::memcpy(buf + offset, data, len);
    if (offset + len > next) next = uint32_t(offset + len);
    return Result::kOk;
}

// The staging both uploads share: an image of size() bytes assembled in a caller-provided buffer, CHUNK by CHUNK.
class Staging {
public:
    // CHUNK's payload (StageChunk); kNoUpload unless a BEGIN succeeded.
    Result Chunk(const uint8_t* p, size_t n) {
        if (!active_) return Result::kNoUpload;
        return StageChunk(buf_, size_, next_, p, n);
    }

    // kNoUpload, kIncomplete (bytes still missing) or kOk.
    Result Ready() const {
        if (!active_) return Result::kNoUpload;
        return next_ < size_ ? Result::kIncomplete : Result::kOk;
    }

    void Clear() {
        active_ = false;
        size_ = next_ = 0;
    }

    bool active() const { return active_; }
    uint32_t size() const { return size_; }
    uint32_t next() const { return next_; }
    uint8_t* data() { return buf_; }
    const uint8_t* data() const { return buf_; }

protected:
    // Only the pointer is stored (buf may be in SDRAM, not yet up at construction).
    explicit Staging(uint8_t* buf) : buf_(buf) {}

    // Starts staging an image of size bytes.
    void Open(uint32_t size) {
        active_ = true;
        size_ = size;
        next_ = 0;
    }

private:
    uint8_t* buf_;
    bool active_ = false;
    uint32_t size_ = 0, next_ = 0;
};

class Upload : public Staging {
public:
    // buf: dkpreset::kSizeMax bytes.
    explicit Upload(uint8_t* buf) : Staging(buf) {}

    // BEGIN's payload `<slot> <size u21>`. Always discards the previous upload; kBadRequest unless the payload is 4
    // bytes, slot < kSlots and size is a preset image's (dkpreset::IsImageSize).
    Result Begin(const uint8_t* p, size_t n) {
        Clear();
        if (n != 4) return Result::kBadRequest;
        const uint32_t size = GetSeptets(p + 1, 3);
        if (p[0] >= dkpreset::kSlots || !dkpreset::IsImageSize(size)) return Result::kBadRequest;
        slot_ = p[0];
        Open(size);
        return Result::kOk;
    }

    // COMMIT's check of the staged image (Ready() is kOk): dkpreset::Parse's status into *out, except kMorph for a
    // preset whose morph block targets the slot it is staged for.
    dkpreset::Status Validate(dkpreset::Preset* out) const {
        const dkpreset::Status st = dkpreset::Parse(data(), size(), out);
        if (st == dkpreset::Status::kOk && out->has_morph && out->morph.target == slot_) return dkpreset::Status::kMorph;
        return st;
    }

    // BEGIN's slot; valid while active().
    int slot() const { return slot_; }

private:
    int slot_ = -1;
};

class FirmwareUpload : public Staging {
public:
    // buf: dkfwimg::kMaxImageBytes bytes.
    explicit FirmwareUpload(uint8_t* buf) : Staging(buf) {}

    // BEGIN's payload `<size u21> <crc32 u32>`. Always discards the previous image; kBadRequest unless the payload is
    // 8 bytes and kMinImageBytes <= size <= kMaxImageBytes.
    Result Begin(const uint8_t* p, size_t n) {
        Clear();
        if (n != 8) return Result::kBadRequest;
        const uint32_t size = GetSeptets(p, 3);
        if (size < dkfwimg::kMinImageBytes || size > dkfwimg::kMaxImageBytes) return Result::kBadRequest;
        crc_ = GetSeptets(p + 3, 5);
        Open(size);
        return Result::kOk;
    }

    // COMMIT's check of the staged image (Ready() is kOk) against BEGIN's CRC and this pedal's board.
    dkfwimg::Check Validate(uint32_t board) const { return dkfwimg::CheckImage(data(), size(), crc_, board); }

private:
    uint32_t crc_ = 0;  // BEGIN's; read only after a successful Begin
};

}  // namespace dklink
