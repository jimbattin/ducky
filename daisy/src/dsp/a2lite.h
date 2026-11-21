// The A2-Lite NAM network (docs/NAM_FORMAT.md). Host-portable (no libDaisy, no allocation), covered by
// tests/a2lite_test.cpp.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../memory.h"
#include "blocks.h"

namespace dkdsp::a2 {

// The fixed A2-Lite shape (docs/NAM_FORMAT.md "A2-Lite shape check").
constexpr int kChannels = 3;
constexpr int kLayers = 23;
constexpr int kHeadKernel = 16;
constexpr float kLeakySlope = 0.01f;
constexpr int kHeadLookback = kHeadKernel - 1;
constexpr uint8_t kKernelSizes[kLayers] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
constexpr uint16_t kDilations[kLayers] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239,
                                          1, 13, 1, 3, 7, 17, 41, 101, 239};
constexpr int MaxKernel() {
    int k = 0;
    for (int l = 0; l < kLayers; ++l) k = kKernelSizes[l] > k ? kKernelSizes[l] : k;
    return k;
}
constexpr int kMaxKernel = MaxKernel();

// Frames the engine processes together (A2Lite): 18 conv sums, 3 weights and 6 sources fit the 32 FP registers.
constexpr int kTile = 6;
constexpr size_t kGuard = kTile - 1;
static_assert(kBlockSize % kTile == 0, "a full block is whole tiles");

// Where layer l's history ring and weights live. A ring holds the lookback plus kBlockSize frames (one full chunk),
// 3 floats a frame, so the chunk's writes never overwrite history that a later frame of the same chunk still reads.
// kGuard more frames follow it in memory and mirror its first kGuard frames, so any kTile consecutive frames of the
// ring are contiguous in memory.
struct LayerLayout {
    int kernel;
    int dilation;
    size_t ring_frames;  // without the guard
    size_t w_off;        // floats into the engine weight array
};

// Floats per layer: conv K*9, conv bias 3, mixin 3, layer1x1 9, layer1x1 bias 3.
constexpr size_t LayerWeights(int kernel) { return size_t(9 * kernel + 18); }

constexpr std::array<LayerLayout, kLayers> MakeLayout() {
    std::array<LayerLayout, kLayers> out{};
    size_t w = 3;  // the rechannel comes first
    for (int l = 0; l < kLayers; ++l) {
        const int k = kKernelSizes[l], d = kDilations[l];
        const size_t frames = size_t((k - 1) * d) + kBlockSize;
        out[l] = LayerLayout{k, d, frames, w};
        w += LayerWeights(k);
    }
    return out;
}

constexpr std::array<LayerLayout, kLayers> kLayout = MakeLayout();
constexpr size_t kHeadWOff = kLayout[kLayers - 1].w_off + LayerWeights(kLayout[kLayers - 1].kernel);
constexpr size_t kWeightCount = kHeadWOff + kHeadKernel * kChannels + 1;

// Floats in layer l's ring, guard included.
constexpr size_t RingFloats(int l) { return size_t(kChannels) * (kLayout[l].ring_frames + kGuard); }
constexpr uint32_t kAllLayers = (uint32_t(1) << kLayers) - 1;
// Floats in the rings of the layers whose bit is set in mask.
constexpr size_t PoolFloats(uint32_t mask) {
    size_t n = 0;
    for (int l = 0; l < kLayers; ++l)
        if ((mask >> l & 1u) != 0) n += RingFloats(l);
    return n;
}
// The layers of dilation d, as a mask.
constexpr uint32_t LayersOfDilation(int d) {
    uint32_t mask = 0;
    for (int l = 0; l < kLayers; ++l)
        if (kDilations[l] == d) mask |= uint32_t(1) << l;
    return mask;
}
constexpr size_t kHistFloats = PoolFloats(kAllLayers);

constexpr int ReceptiveField() {
    int rf = kHeadKernel - 1;
    for (int l = 0; l < kLayers; ++l) rf += (kKernelSizes[l] - 1) * kDilations[l];
    return rf;
}
constexpr int kReceptiveField = ReceptiveField();

static_assert(kWeightCount == 1870, "docs/NAM_FORMAT.md: 1870 network weights");
static_assert(kHistFloats == 20994, "6331 lookback frames plus 24 and a 5-frame guard per layer, 3 floats a frame");
static_assert(kReceptiveField == 6346, "docs/NAM_FORMAT.md: receptive field");

}  // namespace dkdsp::a2

namespace dkdsp {

// Runs the network in the arithmetic order of NeuralAmpModelerCore's A2FastModel<3> (NAM/wavenet/a2_fast.cpp), so the
// output matches its renders to float rounding. Per output the summation order is upstream's: conv bias, taps oldest
// first, inputs 0..2 within a tap, then the mixin; the layer1x1 residual is lb + L0*a0 + L1*a1 + L2*a2.
//
// Process runs chunks of up to kMaxFrames frames, each as BeginChunk (input, rechannel into layer 0's ring), RunLayer
// for every layer in turn over the whole chunk, then EndChunk (head). A layer reads its input x_l from its own ring
// and writes x_{l+1} straight into the next layer's ring; the last layer's residual is unused and not computed. The
// stages are public so the bench (src/bench.cpp) can time each layer; the hot ones are DK_ITCM (memory.h). The rings
// are the caller's memory (Bind), so each build places them where its memory allows. The object has no constructor and
// only zero member initialisers, so it can live in DK_DTCM_BSS.
//
// The code is shaped by one measured property of the Cortex-M7's FPU (docs/M7_PERFORMANCE.md):
// multiply-accumulates run at one a cycle and loads at two a cycle, but a load, store, register move or add that
// follows a multiply-accumulate waits about 3 cycles. So work is done in tiles of a2::kTile frames, as islands: all the
// loads a run needs, then the run of multiply-accumulates and nothing else. Per tap and input that is 3 weights and
// kTile sources, then 3 * kTile multiply-accumulates into sums that stay in registers for the whole tile. The order
// the statements are written in is the order that matters: the device build turns GCC's instruction scheduling off
// (daisy/Makefile), and FpHere pins the values GCC would otherwise load or copy mid-run.
//
// Engine weight layout (the stream's floats, permuted for tap-major reads):
//   [0, 3): rechannel r[c]
//   per layer at kLayout[l].w_off: conv W[k][in][out] (k*9 + in*3 + out), conv bias cb[3], mixin m[3],
//     layer1x1 L[in][out] (in*3 + out), layer1x1 bias lb[3]
//   head at kHeadWOff: H[k][in] (k*3 + in), then the bias hb
class A2Lite {
public:
    static constexpr size_t kMaxFrames = kBlockSize;  // Process splits longer calls into chunks of this size
    static constexpr size_t kBlendRun = 12;           // Blend's island: 12 sums, 12 sources and t in FP registers

    // Where the rings live, once, before the first Load: the layers of far_mask take RingFloats(l) floats each of
    // far_pool, in layer order, and the others of near_pool. near_pool holds a2::PoolFloats(a2::kAllLayers &
    // ~far_mask) floats and far_pool a2::PoolFloats(far_mask) (null for no far layers); both outlive the engine.
    // The output does not depend on the split.
    void Bind(float* near_pool, float* far_pool, uint32_t far_mask) {
        for (int l = 0; l < a2::kLayers; ++l) {
            float*& pool = (far_mask >> l & 1u) != 0 ? far_pool : near_pool;
            ring_[l] = pool;
            pool += a2::RingFloats(l);
        }
    }

    // stream: the 1870 floats of a NAM weight stream without the trailing head_scale (docs/NAM_FORMAT.md "Weight
    // stream"); out: the same floats in the engine layout (kWeightCount of them).
    static void ToEngine(const float* stream, float* out) {
        const float* s = stream;
        for (int c = 0; c < a2::kChannels; ++c) out[c] = *s++;
        for (const a2::LayerLayout& lay : a2::kLayout) {
            float* w = out + lay.w_off;
            const int k_n = lay.kernel;
            for (int o = 0; o < a2::kChannels; ++o)
                for (int i = 0; i < a2::kChannels; ++i)
                    for (int k = 0; k < k_n; ++k) w[k * 9 + i * 3 + o] = *s++;
            float* rest = w + 9 * k_n;  // cb[3], m[3], L[9], lb[3]
            for (int o = 0; o < a2::kChannels; ++o) rest[o] = *s++;
            for (int o = 0; o < a2::kChannels; ++o) rest[3 + o] = *s++;
            for (int o = 0; o < a2::kChannels; ++o)
                for (int i = 0; i < a2::kChannels; ++i) rest[6 + i * 3 + o] = *s++;
            for (int o = 0; o < a2::kChannels; ++o) rest[15 + o] = *s++;
        }
        float* head = out + a2::kHeadWOff;
        for (int j = 0; j < a2::kChannels; ++j)
            for (int k = 0; k < a2::kHeadKernel; ++k) head[k * a2::kChannels + j] = *s++;
        head[a2::kHeadKernel * a2::kChannels] = *s++;
    }

    // weights: a stream as ToEngine takes it, i.e. dkmodel::Model::weights. Permutes them into the engine layout,
    // then Reset().
    void Load(const float* weights, float head_scale) {
        ToEngine(weights, w_);
        head_scale_ = head_scale;
        Reset();
    }

    // The network a + t d, between the calls of Process: a and d are kWeightCount floats each in the engine layout
    // (ToEngine) with the head scale folded into the head (morph.h), so the head scale becomes 1. The histories are
    // kept: the networks a blend moves between share their coordinates. Runs of kBlendRun elements as islands
    // (docs/M7_PERFORMANCE.md): the loads, the multiply-adds, then the stores.
    DK_ITCM void Blend(const float* a, const float* d, float t) {
        size_t i = 0;
        for (; i + kBlendRun <= a2::kWeightCount; i += kBlendRun) {
            float s[kBlendRun], x[kBlendRun];
#pragma GCC unroll 12
            for (size_t j = 0; j < kBlendRun; ++j) s[j] = FpHere(a[i + j]);
#pragma GCC unroll 12
            for (size_t j = 0; j < kBlendRun; ++j) x[j] = FpHere(d[i + j]);
#pragma GCC unroll 12
            for (size_t j = 0; j < kBlendRun; ++j) s[j] += t * x[j];
#pragma GCC unroll 12
            for (size_t j = 0; j < kBlendRun; ++j) w_[i + j] = s[j];
        }
        for (; i < a2::kWeightCount; ++i) w_[i] = a[i] + t * d[i];
        head_scale_ = 1.0f;
    }

    // Fills every history with the settled zero-input state, the state NAM's prewarm reaches. Uses RunLayer, so the
    // arithmetic is Process's.
    void Reset() {
        u_[0] = 0.0f;
        float x[a2::kChannels];  // the settled input of the layer being filled
        for (int c = 0; c < a2::kChannels; ++c) {
            x[c] = w_[c] * u_[0];  // keeps the signed zeros NAM's rechannel produces
            head_hist_[a2::kChannels * a2::kHeadLookback + c] = 0.0f;
        }
        for (int l = 0; l < a2::kLayers; ++l) {
            const a2::LayerLayout& lay = a2::kLayout[l];
            float* ring = ring_[l];
            for (size_t f = 0; f < lay.ring_frames + a2::kGuard; ++f)
                for (int c = 0; c < a2::kChannels; ++c) ring[f * a2::kChannels + c] = x[c];
            wp_[l] = 0;
            if (l + 1 == a2::kLayers) {
                RunLayer(l, 1);
                break;
            }
            wp_[l + 1] = 0;
            RunLayer(l, 1);  // writes the next layer's settled input into its frame 0
            const float* next = ring_[l + 1];
            for (int c = 0; c < a2::kChannels; ++c) x[c] = next[c];
        }
        for (int f = 0; f < a2::kHeadLookback; ++f)
            for (int c = 0; c < a2::kChannels; ++c)
                head_hist_[f * a2::kChannels + c] = head_hist_[a2::kChannels * a2::kHeadLookback + c];
    }

    // out[i] = model(in[i]). Any n (0 is a no-op); in and out may alias. Subnormal inputs are flushed to zero, so no
    // history ever holds a subnormal.
    DK_ITCM void Process(const float* in, float* out, size_t n) {
        for (size_t done = 0; done < n;) {
            const size_t m = n - done < kMaxFrames ? n - done : kMaxFrames;
            BeginChunk(in + done, m);
            for (int l = 0; l < a2::kLayers; ++l) RunLayer(l, m);
            EndChunk(out + done, m);
            done += m;
        }
    }

    // The stages of one chunk of m frames, 1 <= m <= kMaxFrames (not checked): BeginChunk, RunLayer(0..kLayers-1) in
    // order, EndChunk. Public for the bench's per-layer timing; Process is the only other caller.

    // Copies the input (flushing subnormals) into u_, so out may alias in, rechannels it into layer 0's ring and
    // clears the chunk's head sums.
    DK_ITCM void BeginChunk(const float* in, size_t m) {
        // A subnormal input becomes the zero of its sign, as x * 0 would make it: on the bits (an exponent field of
        // 0), so it costs integer work and no float compare. Not left to the FPU's flush-to-zero: the audio callback
        // runs without it (docs/M7_PERFORMANCE.md "Core state").
        for (size_t f = 0; f < m; ++f) {
            uint32_t bits;
            std::memcpy(&bits, in + f, sizeof bits);
            if ((bits & 0x7F800000u) == 0) bits &= 0x80000000u;
            std::memcpy(u_ + f, &bits, sizeof bits);
        }
        const float r0 = w_[0], r1 = w_[1], r2 = w_[2];
        const size_t R = a2::kLayout[0].ring_frames, wp = wp_[0];
        float* ring = ring_[0];
        float* h = head_hist_ + 3 * a2::kHeadLookback;
        size_t q = wp;
        for (size_t f = 0; f < m; ++f) {
            const float u = u_[f];
            ring[3 * q + 0] = r0 * u;
            ring[3 * q + 1] = r1 * u;
            ring[3 * q + 2] = r2 * u;
            h[3 * f + 0] = 0.0f;
            h[3 * f + 1] = 0.0f;
            h[3 * f + 2] = 0.0f;
            if (++q == R) q = 0;
        }
        CloseRing(ring, R, wp, m, 0);
    }

    // Layer l over the chunk, whose input the previous stage wrote into ring l from frame wp_[l]: convolves, adds the
    // activation to the head sums and writes the layer1x1 residual into ring l + 1 from frame wp_[l + 1] (LayerTile).
    DK_ITCM void RunLayer(int l, size_t m) {
        const a2::LayerLayout& lay = a2::kLayout[l];
        const size_t R = lay.ring_frames, wp = wp_[l];
        const float* ring = ring_[l];
        float* next = nullptr;
        size_t nR = 0, nwp = 0;
        if (l + 1 < a2::kLayers) {
            next = ring_[l + 1];
            nR = a2::kLayout[l + 1].ring_frames;
            nwp = wp_[l + 1];
        }

        const float* tap[3 * a2::kMaxKernel];  // per tap k and input i (3 k + i): chunk frame 0's source
        for (int k = 0; k < lay.kernel; ++k) {
            const size_t back = size_t((lay.kernel - 1 - k) * lay.dilation);
            const float* t = ring + 3 * (wp >= back ? wp - back : wp + R - back);
            tap[3 * k + 0] = t;
            tap[3 * k + 1] = t + 1;
            tap[3 * k + 2] = t + 2;
        }
        const float* end = ring + 3 * R;
        // q: where chunk frame f goes in the next ring. A tile writes kTile frames in a row, so the one that reaches
        // the ring's end runs on into the guard; those `spill` frames belong at the ring's start (CloseRing).
        size_t f = 0, q = nwp, spill = 0;
        for (; f + a2::kTile <= m; f += a2::kTile) {
            LayerTile<a2::kTile>(lay, tap, end, f, next != nullptr ? next + 3 * q : nullptr);
            q += a2::kTile;
            if (q >= nR) q = spill = q - nR;
        }
        for (; f < m; ++f) {
            LayerTile<1>(lay, tap, end, f, next != nullptr ? next + 3 * q : nullptr);
            if (++q >= nR) q = 0;
        }
        if (next != nullptr) CloseRing(next, nR, nwp, m, spill);
        wp_[l] = wp + m >= R ? wp + m - R : wp + m;
    }

    // The head over the chunk: a 16-tap conv of the head sums (oldest first), scaled; then keeps the newest 15 sums.
    DK_ITCM void EndChunk(float* out, size_t m) {
        size_t f = 0;
        for (; f + a2::kTile <= m; f += a2::kTile) HeadTile<a2::kTile>(out, f);
        for (; f < m; ++f) HeadTile<1>(out, f);
        // The destination is below the source, so the ranges may overlap (m < 15).
        CopyFloats(head_hist_, head_hist_ + 3 * m, 3 * a2::kHeadLookback);
    }

    // Every layer and head history value is finite and not subnormal.
    bool StateIsNormal() const {
        auto normal = [](float v) { return std::isfinite(v) && std::fpclassify(v) != FP_SUBNORMAL; };
        for (int l = 0; l < a2::kLayers; ++l)
            for (size_t i = 0; i < a2::RingFloats(l); ++i)
                if (!normal(ring_[l][i])) return false;
        for (float v : head_hist_)
            if (!normal(v)) return false;
        return true;
    }

private:
    // After m frames were written into a ring of R frames from frame wp: moves the `spill` frames a tile wrote past
    // the ring's end to its start, then restores the guard as a copy of the ring's first kGuard frames if the chunk
    // wrote any of them.
    [[gnu::always_inline]] static inline void CloseRing(float* ring, size_t R, size_t wp, size_t m, size_t spill) {
        for (size_t i = 0; i < 3 * spill; ++i) ring[i] = ring[3 * R + i];
        if (wp < a2::kGuard || wp + m > R)
            for (size_t i = 0; i < 3 * a2::kGuard; ++i) ring[3 * R + i] = ring[i];
    }

    // Chunk frames [f0, f0 + F) of layer lay. tap[3 k + i]: chunk frame 0's source for tap k, input i, in the layer's
    // ring, which ends at `end`; next: where the frames' residual output goes, or null for the last layer. Each
    // output's sum is accumulated in a register in upstream's order, one fused multiply-add per product on the M7,
    // never a pre-summed expression, which would change the rounding. Statement order is instruction order here (see
    // the class comment): every block below is its loads, then its arithmetic. The loop over outputs is unrolled into
    // three runs, each with its four weights pinned first (FpHere): unpinned, GCC's register allocator moves sums
    // between registers in the middle of a run. A sum that goes back to memory (the head sums, the residual) has its
    // loads grouped in front, then each add beside its store: an add and a store pair in one cycle, a load between
    // them costs one more (2.1 cycles an element against 1). Templates lose DK_ITCM (memory.h): always_inline keeps
    // this in RunLayer.
    template <int F>
    [[gnu::always_inline]] inline void LayerTile(const a2::LayerLayout& lay, const float* const* tap, const float* end,
                                                 size_t f0, float* next) {
        const float* w = w_ + lay.w_off;
        const float* cb = w + 9 * lay.kernel;

        // The conv bias into every sum.
        const float cb0 = cb[0], cb1 = cb[1], cb2 = cb[2];
        float z[F][3];
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) {
            z[f][0] = FpHere(cb0);
            z[f][1] = FpHere(cb1);
            z[f][2] = FpHere(cb2);
        }

        // The taps, oldest first, inputs 0..2 within a tap. Per tap and input: 3 weights and F sources, then 3 F
        // multiply-adds. One loop over all of them: the core predicts no loop exit, and a 3-trip loop over the
        // inputs would mispredict once per tap.
        const float* wi = w;  // W[k][i][0..2]
        const float* src = nullptr;
        for (int j = 0; j < 3 * lay.kernel; ++j) {
            // Input j % 3 of F frames in a row: the guard covers a span that reaches the ring's end. Frames and `end`
            // are 3 floats apart, so the compare is right for inputs 1 and 2 as well.
            src = tap[j] + 3 * f0;
            if (src >= end) src -= 3 * lay.ring_frames;
            const float w0 = wi[0], w1 = wi[1], w2 = wi[2];
            float s[F];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) s[f] = src[3 * f];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) {
                z[f][0] += w0 * s[f];
                z[f][1] += w1 * s[f];
                z[f][2] += w2 * s[f];
            }
            wi += 3;
        }
        // The last source is input 2 of the newest tap, which looks back 0 frames: the tile's own input x_l.
        const float* x = src - 2;

        // The mixin.
        const float* mix = cb + 3;
        const float m0 = mix[0], m1 = mix[1], m2 = mix[2];
        const float slope = FpHere(a2::kLeakySlope);
        float u[F];
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) u[f] = u_[f0 + size_t(f)];
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) {
            z[f][0] += m0 * u[f];
            z[f][1] += m1 * u[f];
            z[f][2] += m2 * u[f];
        }

        // Leaky ReLU as max(z, slope z), which equals z >= 0 ? z : slope z for every z (0 < slope < 1; -0 stays -0)
        // and needs no compare, flags or data-dependent branch. Three products, then three maxima: a maximum right
        // after its own product would wait for it.
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) {
            const float t0 = FpHere(z[f][0] * slope), t1 = FpHere(z[f][1] * slope), t2 = FpHere(z[f][2] * slope);
            z[f][0] = std::fmax(z[f][0], t0);
            z[f][1] = std::fmax(z[f][1], t1);
            z[f][2] = std::fmax(z[f][2], t2);
        }

        // The head sums, two frames at a time: 6 loads, then 6 adds, each with its store.
        float* h = head_hist_ + 3 * (a2::kHeadLookback + f0);
        int hf = 0;
#pragma GCC unroll 4
        for (; hf + 2 <= F; hf += 2) {
            float* hp = h + 3 * hf;
            float s0 = FpHere(hp[0]), s1 = FpHere(hp[1]), s2 = FpHere(hp[2]);
            float s3 = FpHere(hp[3]), s4 = FpHere(hp[4]), s5 = FpHere(hp[5]);
            s0 += z[hf][0];
            s1 += z[hf][1];
            s2 += z[hf][2];
            s3 += z[hf + 1][0];
            s4 += z[hf + 1][1];
            s5 += z[hf + 1][2];
            hp[0] = s0;
            hp[1] = s1;
            hp[2] = s2;
            hp[3] = s3;
            hp[4] = s4;
            hp[5] = s5;
        }
#pragma GCC unroll 1
        for (; hf < F; ++hf) {
            h[3 * hf + 0] += z[hf][0];
            h[3 * hf + 1] += z[hf][1];
            h[3 * hf + 2] += z[hf][2];
        }

        // The layer1x1 residual, one output at a time: x_{l+1} = x_l + (lb + L0 a0 + L1 a1 + L2 a2).
        if (next == nullptr) return;
        const float* lo = cb + 6;  // for output o: L[0..2][o] at lo[0], lo[3], lo[6] and lb[o] at lo[9]
#pragma GCC unroll 3
        for (int o = 0; o < 3; ++o) {
            const float b = FpHere(lo[9]), c0 = FpHere(lo[0]), c1 = FpHere(lo[3]), c2 = FpHere(lo[6]);
            float t[F];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) t[f] = FpHere(b);
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) t[f] += c0 * z[f][0];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) t[f] += c1 * z[f][1];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) t[f] += c2 * z[f][2];
            float xs[F];  // x_l: its loads after the run, in front of the sums' adds and stores
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) xs[f] = FpHere(x[3 * f]);
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) next[3 * f] = xs[f] + t[f];
            ++lo;
            ++x;
            ++next;
        }
    }

    // Head outputs [f0, f0 + F): each sum in a register, bias then taps oldest first, inputs 0..2 within a tap. Per
    // tap: 3 weights and 3 F head sums, then 3 F multiply-adds.
    template <int F>
    [[gnu::always_inline]] inline void HeadTile(float* out, size_t f0) {
        const float* head = w_ + a2::kHeadWOff;
        const float hb = head[a2::kHeadKernel * 3];
        float y[F];
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) y[f] = FpHere(hb);
        for (int k = 0; k < a2::kHeadKernel; ++k) {
            const float H0 = head[3 * k + 0], H1 = head[3 * k + 1], H2 = head[3 * k + 2];
            const float* hk = head_hist_ + 3 * (f0 + size_t(k));
            float s[3 * F];
#pragma GCC unroll 24
            for (int j = 0; j < 3 * F; ++j) s[j] = FpHere(hk[j]);
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) y[f] += H0 * s[3 * f + 0];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) y[f] += H1 * s[3 * f + 1];
#pragma GCC unroll 8
            for (int f = 0; f < F; ++f) y[f] += H2 * s[3 * f + 2];
        }
        const float scale = head_scale_;
#pragma GCC unroll 8
        for (int f = 0; f < F; ++f) out[f0 + size_t(f)] = y[f] * scale;
    }

    float w_[a2::kWeightCount]{};
    float head_scale_ = 0.0f;
    // Layer l's ring (Bind), frames of 3 floats: the input x_l of every frame it still reads.
    float* ring_[a2::kLayers]{};
    size_t wp_[a2::kLayers]{};  // per ring: the frame the next chunk starts at
    // Head sums, linear: frames [0, 15) are the previous chunk's newest 15, frame 15 + f is chunk frame f's.
    float head_hist_[(a2::kHeadLookback + kMaxFrames) * a2::kChannels]{};
    float u_[kMaxFrames]{};  // chunk input (also the mixin condition)
};

}  // namespace dkdsp
