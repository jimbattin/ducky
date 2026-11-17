// UPLOAD BEGIN / CHUNK / COMMIT's staging (link.h, docs/MIDI.md "Upload"): one preset image assembled from CRC-checked
// chunks into a caller-provided buffer. Host-portable, covered by tests/upload_test.cpp. Main loop only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "binary.h"
#include "link.h"
#include "preset.h"

namespace dklink {

class Upload {
public:
    // buf: dkpreset::kSizeWithIr bytes. Only the pointer is stored (buf may be in SDRAM, not yet up at construction).
    explicit Upload(uint8_t* buf) : buf_(buf) {}

    // BEGIN's payload `<slot> <size u21>`. Always discards the previous upload; kBadRequest unless the payload is 4
    // bytes, slot < kSlots and size is kSizeNoIr or kSizeWithIr.
    Result Begin(const uint8_t* p, size_t n) {
        Clear();
        if (n != 4) return Result::kBadRequest;
        const uint32_t size = GetSeptets(p + 1, 3);
        if (p[0] >= dkpreset::kSlots || (size != dkpreset::kSizeNoIr && size != dkpreset::kSizeWithIr))
            return Result::kBadRequest;
        slot_ = p[0];
        size_ = size;
        return Result::kOk;
    }

    // CHUNK's payload `<offset u21> <crc32 u32> <Pack7 data>`. A chunk at or below next() is (re)written, so a resent
    // chunk is idempotent; one past next() is kOffset.
    Result Chunk(const uint8_t* p, size_t n) {
        if (!active()) return Result::kNoUpload;
        if (n < 3 + 5 + 2 || n - 8 > Packed7Size(kChunkBytes)) return Result::kBadRequest;
        uint8_t data[kChunkBytes];
        size_t len = 0;
        if (!Unpack7(p + 8, n - 8, data, len) || len == 0 || len > kChunkBytes) return Result::kBadRequest;
        if (dkbin::Crc32(data, len) != GetSeptets(p + 3, 5)) return Result::kCrc;
        const uint32_t offset = GetSeptets(p, 3);
        if (offset > next_) return Result::kOffset;
        if (offset + len > size_) return Result::kBadRequest;
        std::memcpy(buf_ + offset, data, len);
        if (offset + len > next_) next_ = uint32_t(offset + len);
        return Result::kOk;
    }

    // kNoUpload, kIncomplete (bytes still missing) or kOk.
    Result Ready() const {
        if (!active()) return Result::kNoUpload;
        return next_ < size_ ? Result::kIncomplete : Result::kOk;
    }

    void Clear() {
        slot_ = -1;
        size_ = next_ = 0;
    }

    bool active() const { return slot_ >= 0; }
    int slot() const { return slot_; }
    uint32_t size() const { return size_; }
    uint32_t next() const { return next_; }
    uint8_t* data() { return buf_; }

private:
    uint8_t* buf_;
    int slot_ = -1;
    uint32_t size_ = 0, next_ = 0;
};

}  // namespace dklink
