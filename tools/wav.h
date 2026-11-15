// WAV file reading and float32 writing for the host tools and tests. Host only.
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dkwav {

struct Audio {
    std::vector<float> samples;  // channel 0 only
    uint32_t rate = 0;
    uint16_t channels = 0;
};

namespace detail {

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void Put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
inline void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace detail

// PCM 16/24/32-bit (scaled by 1/2^(bits-1), as NAM's AudioDSPTools wav.cpp does), IEEE float 32-bit, and
// WAVE_FORMAT_EXTENSIBLE (0xFFFE) whose subformat GUID starts with 1 or 3. Walks chunks (skips unknown ones, honours
// odd-size padding). Returns false with *err set.
inline bool Read(const std::string& path, Audio* out, std::string* err) {
    using namespace detail;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *err = "cannot open";
        return false;
    }
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        *err = "not a RIFF/WAVE file";
        return false;
    }
    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    for (size_t pos = 12; pos + 8 <= b.size();) {
        const uint8_t* chunk = b.data() + pos;
        const size_t size = Le32(chunk + 4);
        const size_t avail = b.size() - pos - 8;
        const size_t body = size < avail ? size : avail;  // tolerate a truncated last chunk
        if (std::memcmp(chunk, "fmt ", 4) == 0 && body >= 16) {
            tag = Le16(chunk + 8);
            channels = Le16(chunk + 10);
            rate = Le32(chunk + 12);
            bits = Le16(chunk + 22);
            if (tag == 0xFFFE && body >= 40) tag = Le16(chunk + 32);  // the subformat GUID's first two bytes
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = chunk + 8;
            data_size = body;
        }
        pos += 8 + size + (size & 1);
    }
    if (!have_fmt) {
        *err = "no fmt chunk";
        return false;
    }
    if (!data) {
        *err = "no data chunk";
        return false;
    }
    const bool pcm = tag == 1 && (bits == 16 || bits == 24 || bits == 32);
    const bool ieee = tag == 3 && bits == 32;
    if ((!pcm && !ieee) || channels == 0) {
        *err = "unsupported format " + std::to_string(tag) + "/" + std::to_string(bits) + " bit";
        return false;
    }
    const size_t bytes = bits / 8, frame = bytes * channels, frames = data_size / frame;
    const float scale = 1.0f / float(1u << (bits - 1));
    out->rate = rate;
    out->channels = channels;
    out->samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = data + i * frame;
        if (ieee) {
            const uint32_t u = Le32(p);
            std::memcpy(&out->samples[i], &u, sizeof u);
        } else {
            // Sign-extend from the top byte.
            uint32_t u = 0;
            for (size_t k = 0; k < bytes; ++k) u |= uint32_t(p[k]) << (8 * (k + 4 - bytes));
            out->samples[i] = float(int32_t(u) >> (8 * (4 - bytes))) * scale;
        }
    }
    return true;
}

// Canonical 44-byte header, format 3, mono, 32-bit float. Returns false with *err on I/O failure.
inline bool WriteFloat32(const std::string& path, const std::vector<float>& mono, uint32_t rate, std::string* err) {
    using namespace detail;
    const uint32_t data_size = uint32_t(mono.size() * 4);
    std::vector<uint8_t> b;
    b.reserve(44 + data_size);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    Put32(b, 36 + data_size);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Put32(b, 16);
    Put16(b, 3);         // IEEE float
    Put16(b, 1);         // mono
    Put32(b, rate);
    Put32(b, rate * 4);  // byte rate
    Put16(b, 4);         // block align
    Put16(b, 32);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    Put32(b, data_size);
    for (float v : mono) {
        uint32_t u;
        std::memcpy(&u, &v, sizeof u);
        Put32(b, u);
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    if (!f) {
        *err = "cannot write";
        return false;
    }
    return true;
}

}  // namespace dkwav
