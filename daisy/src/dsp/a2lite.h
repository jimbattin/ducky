// The A2-Lite NAM network (docs/NAM_FORMAT.md). Host-portable (no libDaisy, no allocation), covered by
// tests/a2lite_test.cpp.
#pragma once

#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "dsp/blocks.h"

namespace dkdsp::a2 {

// The fixed A2-Lite shape (docs/NAM_FORMAT.md "A2-Lite shape check").
constexpr int kChannels = 3;
constexpr int kLayers = 23;
constexpr int kHeadKernel = 16;
constexpr float kLeakySlope = 0.01f;
constexpr uint8_t kKernelSizes[kLayers] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
constexpr uint16_t kDilations[kLayers] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239,
                                          1, 13, 1, 3, 7, 17, 41, 101, 239};

// Where layer l's history ring and weights live. A ring holds the lookback plus the current frame, 3 floats a frame.
struct LayerLayout {
    int kernel;
    int dilation;
    size_t ring_frames;
    size_t hist_off;  // floats into the history array
    size_t w_off;     // floats into the engine weight array
};

// Floats per layer: conv K*9, conv bias 3, mixin 3, layer1x1 9, layer1x1 bias 3.
constexpr size_t LayerWeights(int kernel) { return size_t(9 * kernel + 18); }

constexpr std::array<LayerLayout, kLayers> MakeLayout() {
    std::array<LayerLayout, kLayers> out{};
    size_t hist = 0, w = 3;  // the rechannel comes first
    for (int l = 0; l < kLayers; ++l) {
        const int k = kKernelSizes[l], d = kDilations[l];
        const size_t frames = size_t((k - 1) * d + 1);
        out[l] = LayerLayout{k, d, frames, hist, w};
        hist += kChannels * frames;
        w += LayerWeights(k);
    }
    return out;
}

constexpr std::array<LayerLayout, kLayers> kLayout = MakeLayout();
constexpr size_t kHeadWOff = kLayout[kLayers - 1].w_off + LayerWeights(kLayout[kLayers - 1].kernel);
constexpr size_t kWeightCount = kHeadWOff + kHeadKernel * kChannels + 1;
constexpr size_t kHistFloats = kLayout[kLayers - 1].hist_off + kChannels * kLayout[kLayers - 1].ring_frames;

constexpr int ReceptiveField() {
    int rf = kHeadKernel - 1;
    for (int l = 0; l < kLayers; ++l) rf += (kKernelSizes[l] - 1) * kDilations[l];
    return rf;
}
constexpr int kReceptiveField = ReceptiveField();

static_assert(kWeightCount == 1870, "docs/NAM_FORMAT.md: 1870 network weights");
static_assert(kHistFloats == 19062, "6354 history frames");
static_assert(kReceptiveField == 6346, "docs/NAM_FORMAT.md: receptive field");

}  // namespace dkdsp::a2

namespace dkdsp {

// Runs the network sample by sample in the arithmetic order of NeuralAmpModelerCore's A2FastModel<3>
// (NAM/wavenet/a2_fast.cpp), so the output matches its renders to float rounding.
//
// Engine weight layout (the stream's floats, permuted for tap-major reads):
//   [0, 3): rechannel r[c]
//   per layer at kLayout[l].w_off: conv W[k][in][out] (k*9 + in*3 + out), conv bias cb[3], mixin m[3],
//     layer1x1 L[in][out] (in*3 + out), layer1x1 bias lb[3]
//   head at kHeadWOff: H[k][in] (k*3 + in), then the bias hb
class A2Lite {
public:
    static constexpr size_t kMaxFrames = kBlockSize;  // Process splits longer calls into chunks of this size

    // weights: the 1870 floats of the NAM weight stream without the trailing head_scale (docs/NAM_FORMAT.md
    // "Weight stream"), i.e. dkmodel::Model::weights. Permutes them into the engine layout, then Reset().
    void Load(const float* weights, float head_scale) {
        const float* s = weights;
        for (int c = 0; c < 3; ++c) w_[c] = *s++;
        for (const a2::LayerLayout& lay : a2::kLayout) {
            float* w = w_ + lay.w_off;
            const int k_n = lay.kernel;
            for (int o = 0; o < 3; ++o)
                for (int i = 0; i < 3; ++i)
                    for (int k = 0; k < k_n; ++k) w[k * 9 + i * 3 + o] = *s++;
            float* rest = w + 9 * k_n;  // cb[3], m[3], L[9], lb[3]
            for (int o = 0; o < 3; ++o) rest[o] = *s++;
            for (int o = 0; o < 3; ++o) rest[3 + o] = *s++;
            for (int o = 0; o < 3; ++o)
                for (int i = 0; i < 3; ++i) rest[6 + i * 3 + o] = *s++;
            for (int o = 0; o < 3; ++o) rest[15 + o] = *s++;
        }
        float* head = w_ + a2::kHeadWOff;
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < a2::kHeadKernel; ++k) head[k * 3 + j] = *s++;
        head[a2::kHeadKernel * 3] = *s++;
        head_scale_ = head_scale;
        Reset();
    }

    // Fills every history with the settled zero-input state, the state NAM's prewarm reaches.
    void Reset() {
        u_[0] = 0.0f;
        for (int c = 0; c < 3; ++c) {
            x_[c] = w_[c] * u_[0];  // keeps the signed zeros NAM's rechannel produces
            h_[c] = 0.0f;
        }
        for (int l = 0; l < a2::kLayers; ++l) {
            const a2::LayerLayout& lay = a2::kLayout[l];
            float* ring = hist_ + lay.hist_off;
            for (size_t f = 0; f < lay.ring_frames; ++f)
                for (int c = 0; c < 3; ++c) ring[f * 3 + c] = x_[c];
            wp_[l] = 0;
            LayerFrame(l, 0);
        }
        for (int f = 0; f < a2::kHeadKernel; ++f)
            for (int c = 0; c < 3; ++c) head_hist_[f * 3 + c] = h_[c];
        head_wp_ = 0;
    }

    // out[i] = model(in[i]). Any n (0 is a no-op); in and out may alias. Subnormal inputs are flushed to zero, so no
    // history ever holds a subnormal.
    void Process(const float* in, float* out, size_t n) {
        const float* r = w_;
        const float* head = w_ + a2::kHeadWOff;
        const float hb = head[a2::kHeadKernel * 3];
        for (size_t done = 0; done < n;) {
            const size_t m = n - done < kMaxFrames ? n - done : kMaxFrames;
            // Copy the whole chunk's input before writing any output, so in and out may alias.
            for (size_t f = 0; f < m; ++f) {
                const float x = in[done + f];
                u_[f] = std::fabs(x) < FLT_MIN ? x * 0.0f : x;
                for (int c = 0; c < 3; ++c) {
                    x_[f * 3 + c] = r[c] * u_[f];
                    h_[f * 3 + c] = 0.0f;
                }
            }
            for (int l = 0; l < a2::kLayers; ++l)
                for (size_t f = 0; f < m; ++f) LayerFrame(l, f);
            for (size_t f = 0; f < m; ++f) {
                for (int c = 0; c < 3; ++c) head_hist_[head_wp_ * 3 + c] = h_[f * 3 + c];
                float y = hb;
                for (int k = 0; k < a2::kHeadKernel; ++k) {
                    const size_t pos = (head_wp_ + 1 + size_t(k)) % a2::kHeadKernel;  // k frames after the oldest
                    for (int j = 0; j < 3; ++j) y += head[k * 3 + j] * head_hist_[pos * 3 + j];
                }
                out[done + f] = y * head_scale_;
                head_wp_ = (head_wp_ + 1) % a2::kHeadKernel;
            }
            done += m;
        }
    }

    // Every layer and head history value is finite and not subnormal.
    bool StateIsNormal() const {
        auto normal = [](float v) { return std::isfinite(v) && std::fpclassify(v) != FP_SUBNORMAL; };
        for (float v : hist_)
            if (!normal(v)) return false;
        for (float v : head_hist_)
            if (!normal(v)) return false;
        return true;
    }

private:
    // One frame of layer l on x_[f], h_[f] and u_[f]. Shared by Process and Reset so both use identical arithmetic;
    // the summation order per output is upstream's: bias, taps oldest first, inputs 0..2 within a tap.
    [[gnu::always_inline]] inline void LayerFrame(int l, size_t f) {
        const a2::LayerLayout& lay = a2::kLayout[l];
        float* ring = hist_ + lay.hist_off;
        const size_t n = lay.ring_frames, wp = wp_[l];
        const float* w = w_ + lay.w_off;
        const float* cb = w + 9 * lay.kernel;
        const float* mix = cb + 3;
        const float* l1 = cb + 6;
        const float* lb = cb + 15;
        float* x = x_ + f * 3;

        for (int c = 0; c < 3; ++c) ring[wp * 3 + c] = x[c];
        float z[3] = {cb[0], cb[1], cb[2]};
        for (int k = 0; k < lay.kernel; ++k) {
            const size_t back = size_t((lay.kernel - 1 - k) * lay.dilation);
            const size_t pos = wp >= back ? wp - back : wp + n - back;
            const float* src = ring + pos * 3;
            const float* wk = w + k * 9;
            for (int o = 0; o < 3; ++o)
                for (int i = 0; i < 3; ++i) z[o] += wk[i * 3 + o] * src[i];
        }
        float a[3];
        for (int o = 0; o < 3; ++o) {
            z[o] += mix[o] * u_[f];
            a[o] = z[o] >= 0.0f ? z[o] : z[o] * a2::kLeakySlope;
            h_[f * 3 + o] += a[o];
        }
        for (int o = 0; o < 3; ++o) x[o] += lb[o] + l1[0 * 3 + o] * a[0] + l1[1 * 3 + o] * a[1] + l1[2 * 3 + o] * a[2];
        wp_[l] = wp + 1 == n ? 0 : wp + 1;
    }

    float w_[a2::kWeightCount]{};
    float head_scale_ = 0.0f;
    float hist_[a2::kHistFloats]{};                       // layer l's ring at kLayout[l].hist_off, frames of 3 floats
    size_t wp_[a2::kLayers]{};                            // next write frame per ring
    float head_hist_[a2::kHeadKernel * a2::kChannels]{};  // 16-frame ring of h
    size_t head_wp_ = 0;
    float u_[kMaxFrames]{};                     // chunk input (also the mixin condition)
    float x_[kMaxFrames * a2::kChannels]{};     // residual stream, updated in place layer by layer
    float h_[kMaxFrames * a2::kChannels]{};     // head sum
};

}  // namespace dkdsp
