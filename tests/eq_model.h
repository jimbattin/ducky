// The EQ's model (docs/EQ.md section 2): each section's analogue prototype (the cuts' one-pole and second-order
// sections, the RBJ cookbook bell) evaluated under the bilinear transform prewarped at its frequency, the five in
// series at 48 kHz. Closed forms, independent of dsp/filters.h's DesignBell and CutFilter: the reference the digital
// EQ is tested against (tests/eq_test.cpp). The editor's model (editor/src/eqModel.ts) is the same.
#pragma once

#include <cmath>
#include <complex>

namespace dktest {

using Cplx = std::complex<double>;
constexpr double kModelFs = 48000.0;
constexpr double kButterworthQ = 0.70710678118654752;

// One bell: gain (dB), centre frequency (Hz) and Q.
struct EqBell {
    double gain = 0.0, hz = 1000.0, q = 1.0;
};
// The EQ's settings; a slope is 6, 12 or 24 (dB/oct). An off cut passes everything; disabled, the EQ is flat.
struct EqSetting {
    bool enabled = true;
    bool lowOn = false;
    double lowHz = 100.0, lowQ = kButterworthQ;
    int lowSlope = 12;
    EqBell bells[3] = {{0.0, 250.0, 1.0}, {0.0, 1000.0, 1.0}, {0.0, 3000.0, 1.0}};
    bool highOn = false;
    double highHz = 5000.0, highQ = kButterworthQ;
    int highSlope = 12;
};

// s = j tan(pi f / fs) / tan(pi f0 / fs): the bilinear transform prewarped at f0.
inline Cplx Warped(double f, double f0) {
    return Cplx(0.0, std::tan(M_PI * f / kModelFs) / std::tan(M_PI * f0 / kModelFs));
}

// The RBJ bell: (s^2 + s A / Q + 1) / (s^2 + s / (A Q) + 1), A = 10^(gain / 40).
inline Cplx BellModel(const EqBell& b, double f) {
    const double A = std::pow(10.0, b.gain / 40.0);
    const Cplx s = Warped(f, b.hz);
    return (s * s + s * (A / b.q) + 1.0) / (s * s + s / (A * b.q) + 1.0);
}

// A cut: one-pole 1 / (1 + s) or s / (1 + s); second order 1 / (s^2 + s / Q + 1) or s^2 / (...); 24 dB is the
// Butterworth section times the Q section.
inline Cplx CutModel(bool highPass, double hz, double q, int slope, double f) {
    const Cplx s = Warped(f, hz);
    const auto second = [&](double qq) { return (highPass ? s * s : Cplx(1.0)) / (s * s + s / qq + 1.0); };
    if (slope == 6) return (highPass ? s : Cplx(1.0)) / (1.0 + s);
    if (slope == 12) return second(q);
    return second(kButterworthQ) * second(q);
}

inline double Db(Cplx h) { return 20.0 * std::log10(std::abs(h)); }

inline Cplx EqModel(const EqSetting& s, double f) {
    if (!s.enabled) return 1.0;
    Cplx h = 1.0;
    if (s.lowOn) h *= CutModel(true, s.lowHz, s.lowQ, s.lowSlope, f);
    for (const EqBell& b : s.bells) h *= BellModel(b, f);
    if (s.highOn) h *= CutModel(false, s.highHz, s.highQ, s.highSlope, f);
    return h;
}

inline double EqModelDb(const EqSetting& s, double f) { return Db(EqModel(s, f)); }

}  // namespace dktest
