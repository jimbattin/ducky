// Preset morph: a preset's model A blended, weight by weight, into another preset's model B re-expressed in A's
// channel coordinates, along a straight line or along a trained path through up to kMaxWaypoints waypoint networks.
// Host-portable (no libDaisy, no allocation), covered by tests/morph_test.cpp. The transform and the block are the
// contract with editor/src/morph/align.ts (which fits the transform) and editor/src/preset/morph.ts;
// tests/fixtures/morph pins the two languages together.
//
// An A2-Lite network (docs/NAM_FORMAT.md "Forward pass") computes the same output when
//   - its hidden channels (the activations a, summed into the head) are permuted and scaled by positive factors:
//     a' = G a, with G[perm[j]][j] = scale[j]. A leaky ReLU commutes with a positive scale;
//   - its residual path (the layer inputs x) goes through any invertible matrix: x' = M x.
// The weights that do it: r' = M r; per layer and tap W' = G W M^-1, cb' = G cb, m' = G m, L' = M L G^-1, lb' = M lb;
// per head tap H' = s H G^-1, hb' = s hb, with the head scale s folded in (the result's head scale is 1).
//
// Morph block (kBlockBytes, little-endian; in a slot after the model and the IR, preset.h): u8 target (slot 0..7),
// u8 perm[3], u32 the target model binary's CRC field, f32 scale[3], f32 m[9] (row-major), f32 fit, f32 unaligned
// fit (each 0..1, for display: how closely the blends this transform gives, and the blends of the identity, follow
// a crossfade of the two models' outputs; the host keeps the identity unless the fitted transform does better).
//
// Path: a slot's K waypoints (0..kMaxWaypoints, after its block, preset.h) are networks in A's coordinates, each a
// weight stream with the head scale folded in, as Apply(kIdentity, ..) gives. The nodes are A, the waypoints in
// stored order, then the aligned B: K + 1 segments of equal length over the blend position, each a straight blend of
// its two nodes (SegmentAt). K = 0 is the straight blend. A path's block holds the identity transform, the path's
// fit and the straight unaligned blend's fit (tools/morph_train.py trains the waypoints).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "binary.h"
#include "dsp/a2lite.h"
#include "memory.h"
#include "model.h"

namespace dkmorph {

struct Transform {
    uint8_t perm[3];  // B's hidden channel j becomes channel perm[j],
    float scale[3];   // scaled by scale[j] > 0
    float m[9];       // M, row-major: B's residual path becomes M x
};

constexpr Transform kIdentity = {{0, 1, 2}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};

struct Block {
    uint8_t target;      // the slot (0..7) the preset morphs with when the block was written
    uint32_t model_crc;  // dkmodel::Model::crc of that slot's model then
    Transform t;         // that model into the preset's own model's coordinates
    float fit, unaligned_fit;
};

// Not kBlockSize: that is dkdsp's audio block.
constexpr size_t kBlockBytes = 64;
constexpr int kMaxWaypoints = 3;             // a path's waypoints at most
constexpr size_t kWaypointBytes = 4 * dkdsp::a2::kWeightCount;  // one waypoint in a slot
constexpr int kMaxSegments = kMaxWaypoints + 1;
constexpr int kTargets = 8;                  // dkpreset::kSlots (preset.h includes this file)
constexpr float kScaleMax = 16.0f;           // a scale lies in [1 / kScaleMax, kScaleMax]
constexpr double kMinDeterminant = 1e-6;     // of m
constexpr float kEndZone = 0.02f;            // the morph parameter's travel at each end that is one model alone

namespace detail {

constexpr size_t kOffPerm = 1, kOffModelCrc = 4, kOffScale = 8, kOffM = 20, kOffFit = 56, kOffUnalignedFit = 60;
static_assert(kOffUnalignedFit + 4 == kBlockBytes, "the fits end the block");

// 3x3 matrices, row-major, in double.
inline double Det(const double* a) {
    return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
}

inline void Inverse(const double* a, double* out) {
    const double d = Det(a);
    out[0] = (a[4] * a[8] - a[5] * a[7]) / d;
    out[1] = (a[2] * a[7] - a[1] * a[8]) / d;
    out[2] = (a[1] * a[5] - a[2] * a[4]) / d;
    out[3] = (a[5] * a[6] - a[3] * a[8]) / d;
    out[4] = (a[0] * a[8] - a[2] * a[6]) / d;
    out[5] = (a[2] * a[3] - a[0] * a[5]) / d;
    out[6] = (a[3] * a[7] - a[4] * a[6]) / d;
    out[7] = (a[1] * a[6] - a[0] * a[7]) / d;
    out[8] = (a[0] * a[4] - a[1] * a[3]) / d;
}

inline void Mul(const double* a, const double* b, double* out) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
}

// out[0..2] = a v[0..2].
inline void MulVector(const double* a, const float* v, float* out) {
    for (int r = 0; r < 3; ++r) out[r] = float(a[3 * r] * v[0] + a[3 * r + 1] * v[1] + a[3 * r + 2] * v[2]);
}

}  // namespace detail

// Validates and decodes the block p[0, kBlockBytes): a target slot, a permutation, scales in range, an invertible m
// and finite fits. *out is written only on true.
inline bool ParseBlock(const uint8_t* p, Block* out) {
    using namespace detail;
    using dkbin::LeF32;
    Block b{};
    b.target = p[0];
    if (b.target >= kTargets) return false;
    unsigned seen = 0;
    for (int j = 0; j < 3; ++j) {
        b.t.perm[j] = p[kOffPerm + j];
        if (b.t.perm[j] > 2) return false;
        seen |= 1u << b.t.perm[j];
    }
    if (seen != 7u) return false;
    b.model_crc = dkbin::Le32(p + kOffModelCrc);
    for (int j = 0; j < 3; ++j) {
        b.t.scale[j] = LeF32(p + kOffScale + 4 * j);
        // Written so a NaN fails.
        if (!(b.t.scale[j] >= 1.0f / kScaleMax && b.t.scale[j] <= kScaleMax)) return false;
    }
    double m[9];
    for (int i = 0; i < 9; ++i) {
        b.t.m[i] = LeF32(p + kOffM + 4 * i);
        if (!std::isfinite(b.t.m[i])) return false;
        m[i] = b.t.m[i];
    }
    if (!(std::fabs(Det(m)) >= kMinDeterminant)) return false;
    b.fit = LeF32(p + kOffFit);
    b.unaligned_fit = LeF32(p + kOffUnalignedFit);
    if (!std::isfinite(b.fit) || !std::isfinite(b.unaligned_fit)) return false;
    *out = b;
    return true;
}

// The kBlockBytes-byte block of b.
inline void PutBlock(uint8_t* out, const Block& b) {
    using namespace detail;
    out[0] = b.target;
    for (int j = 0; j < 3; ++j) out[kOffPerm + j] = b.t.perm[j];
    dkbin::PutLe32(out + kOffModelCrc, b.model_crc);
    for (int j = 0; j < 3; ++j) dkbin::PutLeF32(out + kOffScale + 4 * j, b.t.scale[j]);
    for (int i = 0; i < 9; ++i) dkbin::PutLeF32(out + kOffM + 4 * i, b.t.m[i]);
    dkbin::PutLeF32(out + kOffFit, b.fit);
    dkbin::PutLeF32(out + kOffUnalignedFit, b.unaligned_fit);
}

// The network (stream, head_scale) in t's coordinates: out is a weight stream as stream is (a2::kWeightCount floats
// in dkmodel::Model::weights' order) whose head scale is 1. t passes ParseBlock's checks. Computed in double.
inline void Apply(const Transform& t, const float* stream, float head_scale, float* out) {
    using namespace detail;
    namespace a2 = dkdsp::a2;
    double g[9] = {}, g_inv[9] = {}, m[9], m_inv[9];
    for (int j = 0; j < 3; ++j) {
        g[3 * t.perm[j] + j] = t.scale[j];
        g_inv[3 * j + t.perm[j]] = 1.0 / t.scale[j];
    }
    for (int i = 0; i < 9; ++i) m[i] = t.m[i];
    Inverse(m, m_inv);

    MulVector(m, stream, out);  // r' = M r
    double tap[9], half[9], w[9];
    for (const a2::LayerLayout& lay : a2::kLayout) {
        // The stream's layer starts where the engine's does: the conv W[o][i][k] at (o * 3 + i) * K + k.
        const int k_n = lay.kernel;
        const float* s = stream + lay.w_off;
        float* o = out + lay.w_off;
        for (int k = 0; k < k_n; ++k) {
            for (int j = 0; j < 9; ++j) tap[j] = s[j * k_n + k];
            Mul(g, tap, half);
            Mul(half, m_inv, w);  // W' = G W M^-1
            for (int j = 0; j < 9; ++j) o[j * k_n + k] = float(w[j]);
        }
        s += 9 * k_n;  // cb[3], m[3], L[o][i] (o * 3 + i), lb[3]
        o += 9 * k_n;
        MulVector(g, s, o);          // cb' = G cb
        MulVector(g, s + 3, o + 3);  // m' = G m
        for (int j = 0; j < 9; ++j) tap[j] = s[6 + j];
        Mul(m, tap, half);
        Mul(half, g_inv, w);  // L' = M L G^-1
        for (int j = 0; j < 9; ++j) o[6 + j] = float(w[j]);
        MulVector(m, s + 15, o + 15);  // lb' = M lb
    }

    // The head H[j][k] at j * 16 + k: per tap the row over the hidden channels j times G^-1, scaled; then the bias.
    const float* s = stream + a2::kHeadWOff;
    float* o = out + a2::kHeadWOff;
    const double scale = head_scale;
    for (int k = 0; k < a2::kHeadKernel; ++k)
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (int j = 0; j < 3; ++j) sum += s[j * a2::kHeadKernel + k] * g_inv[3 * j + c];
            o[c * a2::kHeadKernel + k] = float(scale * sum);
        }
    o[3 * a2::kHeadKernel] = float(scale * s[3 * a2::kHeadKernel]);
}

// What dkdsp::A2Lite::Blend takes, per segment of the path: the segment's first node and the step to its second, in
// the engine layout, head scales folded in.
struct Weights {
    float a[kMaxSegments][dkdsp::a2::kWeightCount];  // node s
    float d[kMaxSegments][dkdsp::a2::kWeightCount];  // node s + 1 - node s
    int segments;                                    // 1..kMaxSegments: the waypoints + 1
};
static_assert(kWaypointBytes == 7480, "a waypoint is a weight stream");

// The blend position (0: model A, 1: the aligned model B) of the morph parameter's value v (0..1): exactly 0 and 1
// within kEndZone of the ends, so a knob that stops short of its travel's ends still reaches both models, and linear
// between.
DK_ITCM inline float BlendPosition(float v) {
    return std::fmin(std::fmax((v - kEndZone) * (1.0f / (1.0f - 2.0f * kEndZone)), 0.0f), 1.0f);
}

// A blend position's place on a path: the segment and the fraction along it that Blend takes.
struct Segment {
    int s;
    float f;
};

// Where the blend position t (0..1) lies on a path of segments (>= 1) equal segments: a knot between two segments
// is the start (fraction 0) of the later one, and t = 1 the end (fraction 1) of the last.
DK_ITCM inline Segment SegmentAt(float t, int segments) {
    const float x = t * float(segments);
    const int s = int(x) < segments ? int(x) : segments - 1;
    return {s, x - float(s)};
}

// *w for model a morphing into model b, which t brings into a's coordinates, through the count (0..kMaxWaypoints)
// waypoints: count * a2::kWeightCount floats, each waypoint a weight stream in a's coordinates with its head scale
// folded in (null when count is 0). scratch: a2::kWeightCount floats.
inline void Prepare(const Transform& t, const dkmodel::Model& a, const dkmodel::Model& b, const float* waypoints,
                    int count, float* scratch, Weights* w) {
    namespace a2 = dkdsp::a2;
    w->segments = count + 1;
    Apply(kIdentity, a.weights, a.head_scale, scratch);
    dkdsp::A2Lite::ToEngine(scratch, w->a[0]);
    for (int s = 0; s <= count; ++s) {
        // d[s] first takes node s + 1, which also starts segment s + 1 (a[s + 1]), then becomes the step from a[s].
        if (s < count) {
            dkdsp::A2Lite::ToEngine(waypoints + size_t(s) * a2::kWeightCount, w->d[s]);
            std::memcpy(w->a[s + 1], w->d[s], sizeof w->d[s]);
        } else {
            Apply(t, b.weights, b.head_scale, scratch);
            dkdsp::A2Lite::ToEngine(scratch, w->d[s]);
        }
        for (size_t i = 0; i < a2::kWeightCount; ++i) w->d[s][i] -= w->a[s][i];
    }
}

}  // namespace dkmorph
