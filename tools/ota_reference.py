#!/usr/bin/env -S uv run
"""Write the OTA compressor's reference fixtures to tests/fixtures/ota/.

The model is docs/ota-compressor-spec.md appendix B, copied verbatim (float64). The inputs are
tools/comp_reference.py's, bit for bit (tests/fixtures/comp/*.in.wav), so they are not written
again; the firmware kernel (daisy/src/dsp/ota_compressor.h) runs on exactly the reference's input.

    step_30_300.s100.ref.wav   Sustain 100 %, Level 0 dB, direct, Balance 1 on step_30_300
    step_30_300.s50.ref.wav    the same at Sustain 50 %
    step_10_1000.s100.ref.wav  Sustain 100 % on step_10_1000
    di.ref.wav                 Sustain 75 %, Level +3 dB, blend, Balance 0.325 on di
    static.csv                 the wet output's amplitude (mV) after 1.5 s of a 330 Hz sine, per
                               Sustain and input level

Outputs are mono IEEE float32 WAVs at 48 kHz (full scale 1.0 = 1.8395 V at the jack).

Usage:
    uv run tools/ota_reference.py
"""
import array
import math
import pathlib

from comp_reference import DI_BALANCE, DI_SAMPLES, pluck, sine, step_input
from make_stimuli import write_float32

REPO = pathlib.Path(__file__).resolve().parents[1]
OUT = REPO / 'tests' / 'fixtures' / 'ota'

# ---- docs/ota-compressor-spec.md appendix B, verbatim ----

# Reduced model of the MXR Dyna Comp / Ross Compressor: reference implementation.
# float64, 48 kHz, every state per sample. Internal signals are AC volts with the circuit's polarity.

FS = 48000.0
T = 1.0 / FS
KVFS = 1.8395                 # volts per unit sample: 0 dBFS = +4.5 dBu
VT2 = 0.051704                # the CA3080's 2 VT
KBUF, FIN = 0.92238, 41.81    # Q1 into the OTA's inverting input: gain, input high-pass (Hz)
K0, FP = 2.0 / 17.0, 9018.9   # the input shelf: low-frequency gain, pole (Hz)
RL, C5 = 134.5e3, 1e-9        # the OTA's load: R8 with Q2's base, C5
KE, KC = 0.98120, 1.1552      # Q2: emitter over the OTA's output, collector over emitter
FOUT = 53.05                  # C9 into R14 + RV2
CLIP_LO, CLIP_HI = -1.244, 2.944
VCC, R13, C8, VSAT = 9.0, 150e3, 10e-6, 0.1
VOFF, BETA_F = 1.1499, 109.32  # Q3 and the bias pin: offset (V), Q3's beta
BETA_D, VBE, RBE = 149.6, 0.399, 268.0  # detector transistor: beta, knee (V), base slope (ohm)
VD, RD = 0.543, 25.0          # clamp diode: knee (V), slope (ohm)
C7, G0 = 10e-9, 1.0 / 1e6     # detector coupling: C6/C7, R11/R12
A6 = 2.0 * C5 * FS            # C5 as a trapezoidal conductance: 96 uS
GLD = 1.0 / RL
A = T / C7
IK0 = 1.0 / (1.0 + A * G0)
IK1 = 1.0 / (1.0 + A * (G0 + 1.0 / RBE))
IK2 = 1.0 / (1.0 + A * (G0 + 1.0 / RD))
U1 = VBE * (1.0 + A * G0)
U2 = -VD * (1.0 + A * G0)


def one_pole(fc):             # TPT one-pole: G = g / (1 + g), g = tan(pi fc / fs)
    g = math.tan(math.pi * fc / FS)
    return g / (1.0 + g)


def tanh(x):                  # Pade (7, 6), clamped to +-1
    x2 = x * x
    y = x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2))) / (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)))
    return min(max(y, -1.0), 1.0)


class Half:
    """One detector transistor (Q4 or Q5) behind its coupling capacitor and clamp diode."""

    def __init__(self):
        self.vc = 0.0             # volts across C6 or C7

    def step(self, vs):           # vs drives the coupling; returns the collector current
        u = vs - self.vc          # C7 dvc/dt = G0 vb + f(vb), vb = vs - vc: backward Euler, solved exactly
        vb = min(max(u, U2), U1) * IK0 + max(u - U1, 0.0) * IK1 + min(u - U2, 0.0) * IK2
        self.vc = vs - vb
        return BETA_D * max(vb - VBE, 0.0) / RBE


class Ota:
    def __init__(self, sustain=1.0, level=1, mode="direct", balance=1.0):
        self.g_in, self.g_sh, self.g_out = one_pole(FIN), one_pole(FP), one_pole(FOUT)
        self.inv_rs = 1.0 / ((500e3 + 27e3) * (27e3 / 527e3) ** sustain)  # RV1 + R17
        self.gl = (0.70794578, 1.0, 1.41253754)[level]                       # Level -3, 0, +3 dB
        g = self.inv_rs / BETA_F
        self.venv = (VCC / R13 + VOFF * g) / (1.0 / R13 + g)                # C8 at rest
        self.iabc_rest = (self.venv - VOFF) * self.inv_rs
        self.s_in = self.s_sh = self.s_out = self.vo = self.i_prev = 0.0
        self.top, self.bottom = Half(), Half()
        self.mode, self.b = mode, balance

    def step(self, x):
        v = KVFS * x
        a = (v - self.s_in) * self.g_in                 # input high-pass
        lp = a + self.s_in
        self.s_in = lp + a
        vq = KBUF * self.gl * (v - lp)                  # dry: Q1's emitter
        a = (vq - self.s_sh) * self.g_sh                # input shelf
        lp = a + self.s_sh
        self.s_sh = lp + a
        vd = K0 * lp + (vq - lp)                        # OTA drive, inverting input
        iabc = max(self.venv - VOFF, 0.0) * self.inv_rs
        i = -iabc * tanh(vd / VT2)
        vo = ((A6 - GLD) * self.vo + i + self.i_prev) / (A6 + GLD)  # C5 dvo/dt = i - vo / RL, trapezoidal
        self.vo = min(max(vo, CLIP_LO), CLIP_HI)
        self.i_prev = i
        ve = KE * self.vo                               # Q2's emitter; its collector is -KC ve
        ic = self.top.step(-KC * ve) + self.bottom.step(ve)
        self.venv = max(self.venv + (T / C8) * ((VCC - self.venv) / R13 - ic - iabc / BETA_F), VSAT)
        a = (ve - self.s_out) * self.g_out              # output coupling
        lp = a + self.s_out
        self.s_out = lp + a
        wet = ve - lp
        y = ((1.0 - self.b) * vq - self.b * wet) / KVFS if self.mode == "blend" else -self.b * wet / KVFS
        return y, vq, ve                                # output sample, dry and Q2's emitter in volts

# ---- end of appendix B ----


STATIC_SAMPLES = 72000
STATIC_TAIL = 436                 # about 3 cycles of 330 Hz
STATIC_SUSTAIN = (0, 25, 50, 75, 100)
STATIC_MV = (1, 2, 5, 10, 20, 50, 100, 200, 500, 1000)


def render(model, xs):
    return array.array('f', (model.step(x)[0] for x in xs))


def write(name, samples):
    path = OUT / name
    write_float32(path, samples)
    print(f'wrote {path.relative_to(REPO)}')


def static_rows():
    rows = ['sustain_pct,in_mv,wet_mv']
    for s in STATIC_SUSTAIN:
        for mv in STATIC_MV:
            model = Ota(sustain=s / 100.0)  # direct, Balance 1: y is -wet / KVFS
            wet = [-KVFS * model.step(sine(mv / 1000.0, n))[0] for n in range(STATIC_SAMPLES)][-STATIC_TAIL:]
            rows.append(f'{s},{mv},{1000.0 * (max(wet) - min(wet)) / 2.0:.4f}')
    return '\n'.join(rows) + '\n'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    s30 = step_input(0.03, 0.3)
    s10 = step_input(0.01, 1.0)
    di = [pluck(n) for n in range(DI_SAMPLES)]
    write('step_30_300.s100.ref.wav', render(Ota(sustain=1.0), s30))
    write('step_30_300.s50.ref.wav', render(Ota(sustain=0.5), s30))
    write('step_10_1000.s100.ref.wav', render(Ota(sustain=1.0), s10))
    write('di.ref.wav', render(Ota(sustain=0.75, level=2, mode="blend", balance=DI_BALANCE), di))
    path = OUT / 'static.csv'
    path.write_text(static_rows())
    print(f'wrote {path.relative_to(REPO)}')


if __name__ == '__main__':
    main()
