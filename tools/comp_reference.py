#!/usr/bin/env -S uv run
"""Write the compressor's reference fixtures to tests/fixtures/comp/.

The model is docs/compressor-spec.md appendix B, copied verbatim (float64). Every input sample is
rounded to float32 before it is written and before the model sees it, so the firmware kernel
(daisy/src/dsp/compressor.h) runs on exactly the reference's input.

    step_30_300.in.wav         1 s of 330 Hz: 30 mV, 300 mV from 0.1 s to 0.5 s, then 30 mV
    step_10_1000.in.wav        the same with 10 mV and 1 V
    di.in.wav                  1.5 s of synthetic plucked notes (decaying harmonics)
    step_30_300.s100.ref.wav   Sustain 100 %, Level 0 dB, direct, Balance 1 on step_30_300
    step_30_300.s50.ref.wav    the same at Sustain 50 %
    step_10_1000.s100.ref.wav  Sustain 100 % on step_10_1000
    di.ref.wav                 Sustain 100 %, Level +3 dB, blend, Balance 0.325 on di
    static.csv                 the wet output's amplitude (mV) after 1.5 s of a 330 Hz sine, per
                               Sustain and input level

Inputs and outputs are mono IEEE float32 WAVs at 48 kHz (full scale 1.0 = 1.8395 V at the jack).

Usage:
    uv run tools/comp_reference.py
"""
import array
import math
import pathlib
import struct

from make_stimuli import write_float32

REPO = pathlib.Path(__file__).resolve().parents[1]
OUT = REPO / 'tests' / 'fixtures' / 'comp'

# ---- docs/compressor-spec.md appendix B, verbatim ----

# Reduced model of the Dinosaural OTC-201 / Aion Convex: reference implementation.
# float64, 48 kHz, 24-sample blocks. Internal signals are volts with the circuit's polarity.

FS = 48000.0
T = 1.0 / FS
NB = 24                       # samples per block
TB = NB * T
KVFS = 1.8395                 # volts per unit sample: 0 dBFS = +4.5 dBu
R11, R12, C5, C6 = 10e3, 8.2e3, 10e-9, 330e-12
A2 = 2.0 * C6 * FS            # C6 as a trapezoidal conductance: 31.68 uS
CLIP_LO, CLIP_HI = -2.85, 2.50


def one_pole(fc):             # TPT one-pole: G = g / (1 + g), g = tan(pi fc / fs)
    g = math.tan(math.pi * fc / FS)
    return g / (1.0 + g)


class Half:
    """One detector pair, its reservoir, LED and cell."""

    def __init__(self, v1, d):
        self.v1, self.d = v1, d   # knee start and half-width, volts
        self.w = 0.95             # volts across the LED and its 2k2 (long silence)
        self.i = 0.0              # LED current, amps
        self.n = 1e-6             # cell state, mA
        self.q = 0.0              # charge collected this block, coulombs

    def pair(self, u):            # pair current for a base drive u: quadratic knee, then 1/190 ohm
        t = min(max(u - self.v1, 0.0), 2.0 * self.d)
        return (t * t / (4.0 * self.d) + max(u - self.v1 - 2.0 * self.d, 0.0)) / 190.0

    def block(self):              # once per block: returns the cell's conductance
        self.w = min(self.w + (self.q - self.i * TB) / 10e-6, 7.8)
        self.q = 0.0
        i = max(self.i, 1e-12)    # LED: w = 2202.3 i + 34.12 mV ln(1 + i / 2.1e-22)
        for _ in range(3):
            f = 2202.3 * i + 0.03412 * math.log1p(i / 2.1e-22) - self.w
            i = max(i - f / (2202.3 + 0.03412 / (i + 2.1e-22)), 1e-15)
        self.i = i
        led = i * 1e3             # mA
        if led > self.n:
            self.n += TB * (led - self.n) / 0.010
        else:
            self.n -= TB * (self.n - led) * (1.0 / 0.43 + 6.0 * self.n)
        self.n = max(self.n, 1e-6)
        return 1.0 / (75.0 + 1711.0 * self.n ** -1.42)


class Otc201:
    def __init__(self, sustain=1.0, level=0, mode="Z", balance=1.0):
        self.gl = {-1: 0.697, 0: 1.0, 1: 1.413}[level]
        self.g_in = one_pole(46.2 if level == 1 else 32.7)            # C2 into the Level stage
        self.g_c5 = one_pole(1.0 / (2.0 * math.pi * R12 * C5))        # 1941 Hz
        self.g_det = one_pole(1.0 / (2.0 * math.pi * 33e-9 * 104.7e3))  # 46.1 Hz
        self.s_in = self.s_c5 = self.s_det = 0.0
        self.on = sustain > 0.0
        self.g_sus = 1.0 / max(sustain * 100e3, 1.0)
        self.pos = Half(0.385, 0.120)     # Q2/Q3: positive output peaks
        self.neg = Half(0.405, 0.135)     # Q4/Q5: negative output peaks
        self.gf = self.gf_target = self.g_sus
        self.vf = self.ig = 0.0
        self.k = 0
        self.mode, self.b = mode, balance

    def step(self, x):
        v = KVFS * x
        a = (v - self.s_in) * self.g_in                 # input high-pass
        lp = a + self.s_in
        self.s_in = lp + a
        vd = -self.gl * 0.9416 * (v - lp)               # dry: buffer and Level stage, inverted
        vp = 0.894 * vd                                 # pin 5
        a = (vp - self.s_c5) * self.g_c5                # C5's voltage
        vc5 = a + self.s_c5
        self.s_c5 = vc5 + a
        ig = vp / R11 + (vp - vc5) / R12                # current in the ground leg
        gf0 = self.gf
        self.gf += (self.gf_target - self.gf) / (NB - self.k)   # ramp to the block's target
        if self.on:                                     # C6 dvf/dt = ig - Gf vf, trapezoidal
            vf = ((A2 - gf0) * self.vf + ig + self.ig) / (A2 + self.gf)
        else:
            vf = 0.0
        vo = min(max(vp + vf, CLIP_LO), CLIP_HI)        # pin 7
        self.vf, self.ig = vo - vp, ig
        a = (vo - self.s_det) * self.g_det              # detector coupling
        lp = a + self.s_det
        self.s_det = lp + a
        u = 0.955 * (vo - lp)
        self.pos.q += self.pos.pair(u) * T
        self.neg.q += self.neg.pair(-u) * T
        self.k += 1
        if self.k == NB:
            self.k = 0
            self.gf_target = self.g_sus + self.pos.block() + self.neg.block()
        mix = (1.0 - self.b) * vd + self.b * vo if self.mode == "SB" else self.b * vo
        return -mix / KVFS, vd, vo                      # output sample, dry and wet in volts

# ---- end of appendix B ----


F0 = 330.0
STATIC_SAMPLES = 72000
STATIC_TAIL = 436                 # about 3 cycles of 330 Hz
STATIC_SUSTAIN = (10, 25, 50, 75, 100)
STATIC_MV = (10, 20, 50, 100, 200, 316, 501, 1000, 1585)
# (onset s, f0 Hz, peak V): low E, A, D, G, then a high E barely above the noise
NOTES = [(0.05, 82.41, 0.53), (0.45, 110.0, 0.15), (0.75, 146.83, 0.05), (0.95, 196.0, 0.4),
         (1.25, 329.63, 0.02)]
HARMONICS = 10
DI_SAMPLES = 72000
DI_BALANCE = (81.0 ** 0.75 - 1.0) / 80.0  # the audio taper at position 0.75: 0.325


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def sine(a, n):
    return f32(a / KVFS * math.sin(2.0 * math.pi * F0 * n / FS))


def step_input(lo, hi):
    return [sine(hi if 4800 <= n < 24000 else lo, n) for n in range(48000)]


def pluck(n):
    t = n / FS
    h = sum(1.0 / k for k in range(1, HARMONICS + 1))
    x = 0.0
    for t0, f0, pk in NOTES:
        if t < t0:
            continue
        dt = t - t0
        attack = min(dt / 0.001, 1.0)
        for k in range(1, HARMONICS + 1):
            x += pk / h / k * attack * math.exp(-dt * math.sqrt(k) / 1.2) * math.sin(2.0 * math.pi * k * f0 * dt)
    return f32(x / KVFS)


def render(model, xs):
    return array.array('f', (model.step(x)[0] for x in xs))


def write(name, samples):
    path = OUT / name
    write_float32(path, samples)
    print(f'wrote {path.relative_to(REPO)}')


def static_rows():
    rows = ['sustain,in_mv,wet_mv']
    for s in STATIC_SUSTAIN:
        for mv in STATIC_MV:
            model = Otc201(sustain=s / 100.0)
            wet = [model.step(sine(mv / 1000.0, n))[2] for n in range(STATIC_SAMPLES)][-STATIC_TAIL:]
            rows.append(f'{s},{mv},{1000.0 * (max(wet) - min(wet)) / 2.0:.4f}')
    return '\n'.join(rows) + '\n'


def main():
    s30 = step_input(0.03, 0.3)
    s10 = step_input(0.01, 1.0)
    di = [pluck(n) for n in range(DI_SAMPLES)]
    write('step_30_300.in.wav', array.array('f', s30))
    write('step_10_1000.in.wav', array.array('f', s10))
    write('di.in.wav', array.array('f', di))
    write('step_30_300.s100.ref.wav', render(Otc201(sustain=1.0, level=0, mode="Z", balance=1.0), s30))
    write('step_30_300.s50.ref.wav', render(Otc201(sustain=0.5, level=0, mode="Z", balance=1.0), s30))
    write('step_10_1000.s100.ref.wav', render(Otc201(sustain=1.0, level=0, mode="Z", balance=1.0), s10))
    write('di.ref.wav', render(Otc201(sustain=1.0, level=1, mode="SB", balance=DI_BALANCE), di))
    path = OUT / 'static.csv'
    path.write_text(static_rows())
    print(f'wrote {path.relative_to(REPO)}')


if __name__ == '__main__':
    main()
