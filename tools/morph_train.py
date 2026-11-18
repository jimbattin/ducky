#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy", "torch"]
#
# [tool.uv.sources]
# torch = { index = "pytorch-cpu" }
#
# [[tool.uv.index]]
# name = "pytorch-cpu"
# url = "https://download.pytorch.org/whl/cpu"
# explicit = true
# ///
"""Train and measure a preset morph's path (docs/MORPH.md "The trained path").

A morph blends model A's weights into model B's along the morph parameter. This tool trains K waypoint
networks so the piecewise-linear weight path A -> W1 .. WK -> B follows the crossfade of the two models'
outputs, and writes them as a path file for `bun tools/pedal.ts morph N M --path FILE` (`--train` runs
this tool itself). The ends stay the two models; the pedal's cost does not change.

    uv run tools/morph_train.py A.bin B.bin                 measure the straight blend
    uv run tools/morph_train.py A.bin B.bin -o OUT.path     train, measure, write the path file
    uv run tools/morph_train.py A.bin B.bin --path F.path   measure a path file
    uv run tools/morph_train.py --check                     the network and the fit against tests/fixtures

A.bin and B.bin are Ducky model binaries (docs/NAM_FORMAT.md "Ducky model binary"; `bun
tools/nam_convert.ts X.nam`) or preset images holding one.

Training signals: synthetic plucked notes and chords at four levels, and, when --di names a file that
exists (default renders/stimuli/guitar_di.wav, `uv run tools/make_stimuli.py`), its first three quarters
at four gains about 9 dB apart: a normalised recording down to a quiet guitar at the pedal's input. Train with a
DI: a path trained on the plucks alone follows real playing less closely, and its stored fit flatters it
(docs/MORPH.md "The trained path"). Measured on: the analysis' stimulus (editor/src/morph/stimulus.ts,
never trained on), whose fit is the one the morph block stores, and the DI's last quarter at 0, -12 and
-24 dB.

Per signal and blend: the fit (1 - the mean blend error at the eighths, as editor/src/morph/align.ts), and
over the fifteen sixteenths the worst blend error (the energy of blend - crossfade over the crossfade's,
means removed), the worst level error (AC RMS against the crossfade's, dB), the worst DC (the blend's mean,
dB) and the worst shape error (1 - correlation squared: what a perfect gain would leave).

Path file, little-endian: "DKMP", u32 version 1, u32 K, u32 A's CRC field, u32 B's CRC field, f32 the
path's fit, f32 the straight blend's fit, then K waypoints of 1870 f32: weight streams in A's coordinates
with the head scale folded in (daisy/src/morph.h).
"""
import argparse
import math
import pathlib
import struct
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

REPO = pathlib.Path(__file__).resolve().parents[1]
SAMPLE_RATE = 48000

# The A2-Lite shape (docs/NAM_FORMAT.md): 23 layers of 3 channels, a 16-tap head.
KERNELS = [6] * 14 + [15, 15] + [6] * 7
DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
WEIGHT_COUNT = 1870
HEAD_BASE = 1821
HEAD_KERNEL = 16
LAYER_BASES = [3 + sum(9 * k + 18 for k in KERNELS[:l]) for l in range(len(KERNELS))]
# The samples before an output sample that reach it.
RECEPTIVE = sum((k - 1) * d for k, d in zip(KERNELS, DILATIONS)) + HEAD_KERNEL - 1

# The model binary (editor/src/nam/convert.ts).
MODEL_BIN_SIZE = 7908
OFF_FLAGS, OFF_WEIGHTS, OFF_HEAD_SCALE, OFF_LOUDNESS, OFF_INPUT_LEVEL, OFF_NAME, OFF_CRC = 6, 92, 7572, 7576, 7580, 7584, 7904
FLAG_HAS_LOUDNESS, FLAG_HAS_INPUT_LEVEL = 1, 2
# The pedal's gains (editor/src/controls.ts levelMatchDb, inputCalDb).
LEVEL_TARGET_DB, INPUT_LEVEL_DBU, DEFAULT_MODEL_INPUT_DBU = -18.0, 4.5, 12.0

PATH_MAGIC, PATH_VERSION, MAX_WAYPOINTS = b'DKMP', 1, 3
# Where a morph is measured: the fit's positions (align.ts BLEND_POSITIONS) and the table's.
EIGHTHS = [i / 8 for i in range(1, 8)]
SIXTEENTHS = [i / 16 for i in range(1, 16)]

# Training.
CHUNK = 8192            # output samples of one training excerpt
POSITIONS_PER_STEP = 6  # excerpts a step, each at its own blend position
DEFAULT_STEPS = 8000
LEARNING_RATE = 1e-3
DI_GAINS = (1.0, 0.35, 0.12, 0.04)
PLUCK_LEVELS = (0.7, 0.25, 0.4, 0.1)
# An excerpt's error, its mean removed, counts against its own crossfade's AC power plus this share of its
# signal's mean power: quiet passages weigh nearly as loud ones, silence does not divide by zero. The error's
# mean (DC) counts against the loudest signal's mean power: an offset matters by its size, not against how
# quietly the guitar is played.
LOSS_FLOOR = 0.05
DI_TRAIN_SHARE = 0.75
DEFAULT_DI = REPO / 'renders' / 'stimuli' / 'guitar_di.wav'


class Model:
    """A model binary: its weight stream with the head scale folded in, its gains and its CRC field."""

    def __init__(self, path):
        raw = pathlib.Path(path).read_bytes()
        at = raw.find(b'DKNM')
        if at < 0 or len(raw) < at + MODEL_BIN_SIZE:
            sys.exit(f'{path}: no model binary')
        b = raw[at:at + MODEL_BIN_SIZE]
        flags, = struct.unpack_from('<H', b, OFF_FLAGS)
        head_scale, loudness, input_level = struct.unpack_from('<fff', b, OFF_HEAD_SCALE)
        w = np.frombuffer(b, '<f4', WEIGHT_COUNT, OFF_WEIGHTS).astype(np.float64)
        w[HEAD_BASE:] *= head_scale
        self.weights = torch.tensor(w)
        self.level_db = LEVEL_TARGET_DB - loudness if flags & FLAG_HAS_LOUDNESS else 0.0
        self.cal_db = INPUT_LEVEL_DBU - (input_level if flags & FLAG_HAS_INPUT_LEVEL else DEFAULT_MODEL_INPUT_DBU)
        self.crc, = struct.unpack_from('<I', b, OFF_CRC)
        self.name = b[OFF_NAME:OFF_NAME + 64].split(b'\0')[0].decode('ascii', 'replace')


def forward(w, u):
    """The networks w (P x WEIGHT_COUNT, head scale 1) on the signals u (P x (T + RECEPTIVE)): P x T.

    The convolutions take no padding, so u's first RECEPTIVE samples are the history of the first output:
    zeros there give the settled zero-input state NAM's prewarm leaves.
    """
    p = w.shape[0]
    t = u.shape[1] - RECEPTIVE
    x = w[:, 0:3, None] * u[:, None, :]
    h = 0
    for l, (k, d) in enumerate(zip(KERNELS, DILATIONS)):
        base = LAYER_BASES[l]
        rest = base + 9 * k  # cb[3], m[3], L[9], lb[3]
        z = F.conv1d(x.reshape(1, 3 * p, -1), w[:, base:rest].reshape(3 * p, 3, k), w[:, rest:rest + 3].reshape(-1),
                     dilation=d, groups=p).reshape(p, 3, -1)
        cut = (k - 1) * d
        u = u[:, cut:]
        a = F.leaky_relu(z + w[:, rest + 3:rest + 6, None] * u[:, None, :], 0.01)
        h = h + a[:, :, -(t + HEAD_KERNEL - 1):]
        if l + 1 < len(KERNELS):
            x = x[:, :, cut:] + w[:, rest + 15:rest + 18, None] + torch.bmm(w[:, rest + 6:rest + 15].reshape(p, 3, 3), a)
    y = F.conv1d(h.reshape(1, 3 * p, -1), w[:, HEAD_BASE:HEAD_BASE + 48].reshape(p, 3, HEAD_KERNEL),
                 w[:, HEAD_BASE + 48], groups=p)
    return y.reshape(p, t)


def from_rest(signal):
    """signal behind RECEPTIVE zeros."""
    return torch.cat([torch.zeros(RECEPTIVE, dtype=signal.dtype), signal])


def plucks(seed, notes, levels, spacing=SAMPLE_RATE // 4, tail=SAMPLE_RATE // 4):
    """Karplus-Strong plucks as editor/src/morph/stimulus.ts makes them: one every `spacing` samples, each a
    note (Hz) or a chord (a tuple of them, plucked together), normalised to its level (levels in turn), ringing
    to the end `tail` samples after the last onset."""
    n = (len(notes) - 1) * spacing + tail
    out = np.zeros(n)
    state = seed

    def rand():  # mulberry32
        nonlocal state
        state = (state + 0x6D2B79F5) & 0xFFFFFFFF
        v = ((state ^ (state >> 15)) * (state | 1)) & 0xFFFFFFFF
        v ^= (v + (((v ^ (v >> 7)) * (v | 61)) & 0xFFFFFFFF)) & 0xFFFFFFFF
        return ((v ^ (v >> 14)) & 0xFFFFFFFF) / 4294967296

    for i, note in enumerate(notes):
        onset = spacing * i
        pluck = np.zeros(n)
        for f in note if isinstance(note, tuple) else (note,):
            period = math.floor(SAMPLE_RATE / f + 0.5)
            buf = np.array([2 * rand() - 1 for _ in range(period)])
            for at in range(onset, n, period):
                m = min(period, n - at)
                pluck[at:at + m] += buf[:m]
                # One pass of the string's in-place update: each sample averages with its successor, the last
                # with the pass's own first.
                new = 0.996 * 0.5 * (buf + np.roll(buf, -1))
                new[-1] = 0.996 * 0.5 * (buf[-1] + new[0])
                buf = new
        out += levels[i % len(levels)] / np.abs(pluck).max() * pluck
    return torch.tensor(out)


def morph_stimulus():
    """editor/src/morph/stimulus.ts morphStimulus(): the signal the stored fit is measured on."""
    return plucks(0x5EED, [82.41, 110, 146.83, 196, 246.94, 329.63, 164.81, 220], (0.5, 0.125))


def training_plucks():
    """Other notes, levels and noise than morph_stimulus: single notes, then chords."""
    notes = [98, 130.81, 174.61, 233.08, 293.66, 392, 123.47, 185, 277.18, 87.31, 440, 155.56, 207.65, 311.13,
             116.54, 369.99,
             (82.41, 123.47), (110, 164.81, 220), (146.83, 220, 293.66), (98, 146.83, 196, 246.94),
             (130.81, 196), (87.31, 130.81, 174.61), (196, 246.94, 293.66, 392), (103.83, 155.56)]
    return plucks(0xBEEF, notes, PLUCK_LEVELS)


def read_wav(path):
    """A WAV's first channel as float64: 16- or 24-bit PCM, or float32; 48 kHz."""
    raw = pathlib.Path(path).read_bytes()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        sys.exit(f'{path}: not a WAV file')
    at, fmt, data = 12, None, None
    while at + 8 <= len(raw):
        size, = struct.unpack_from('<I', raw, at + 4)
        if raw[at:at + 4] == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', raw, at + 8)
        elif raw[at:at + 4] == b'data':
            data = raw[at + 8:at + 8 + size]
        at += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit(f'{path}: no fmt or data chunk')
    kind, channels, rate, _, _, bits = fmt
    if rate != SAMPLE_RATE:
        sys.exit(f'{path}: {rate} Hz, need {SAMPLE_RATE}')
    if kind == 3 and bits == 32:
        x = np.frombuffer(data, '<f4').astype(np.float64)
    elif kind == 1 and bits == 16:
        x = np.frombuffer(data, '<i2') / 32768.0
    elif kind == 1 and bits == 24:
        b = np.frombuffer(data, np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16) - ((b[:, 2] & 0x80) << 17)) / 8388608.0
    else:
        sys.exit(f'{path}: format {kind} with {bits} bits is not supported')
    return torch.tensor(x.reshape(-1, channels)[:, 0].copy())


class Morph:
    """The morph of model a into model b as the pedal plays it: the path's nodes (a, the waypoints, b) and the
    level match and the input calibration, which blend in dB."""

    def __init__(self, a, b):
        self.a, self.b = a, b

    def nodes(self, waypoints):
        return torch.cat([self.a.weights[None].to(waypoints.dtype), waypoints, self.b.weights[None].to(waypoints.dtype)])

    def chord(self, k):
        """k waypoints spread evenly on the straight line from a to b."""
        t = torch.arange(1, k + 1, dtype=torch.float64)[:, None] / (k + 1)
        return self.a.weights + t * (self.b.weights - self.a.weights)

    def weights(self, waypoints, positions):
        """The path's weights at each blend position (P x WEIGHT_COUNT)."""
        nodes = self.nodes(waypoints)
        segments = nodes.shape[0] - 1
        at = positions * segments
        s = at.floor().clamp(max=segments - 1).long()
        return nodes[s] + (at - s)[:, None] * (nodes[s + 1] - nodes[s])

    def gains(self, positions):
        cal = 10 ** ((self.a.cal_db + positions * (self.b.cal_db - self.a.cal_db)) / 20)
        level = 10 ** ((self.a.level_db + positions * (self.b.level_db - self.a.level_db)) / 20)
        return cal, level

    def render(self, waypoints, positions, u):
        """The blends' level-matched outputs at positions (P) on the signals u (P x (T + RECEPTIVE))."""
        cal, level = self.gains(positions)
        return level[:, None] * forward(self.weights(waypoints, positions), cal[:, None] * u)

    def teachers(self, u):
        """The two models' level-matched outputs on u (T + RECEPTIVE), each at its own input calibration."""
        with torch.no_grad():
            ends = torch.tensor([0.0, 1.0], dtype=u.dtype)
            none = torch.empty(0, WEIGHT_COUNT, dtype=u.dtype)
            ya, yb = self.render(none, ends, u.expand(2, -1))
        return ya, yb


def decibels(x):
    return 20 * math.log10(max(float(x), 1e-12))


def measure(morph, waypoints, u):
    """The path's blends against the crossfade on u (T + RECEPTIVE), at the sixteenths: per position the blend
    error, the level error (dB), the DC (dB) and the shape error."""
    ya, yb = morph.teachers(u)
    rows = {}
    with torch.no_grad():
        for p in SIXTEENTHS:
            y = morph.render(waypoints, torch.tensor([p], dtype=u.dtype), u[None])[0]
            want = (1 - p) * ya + p * yb
            yc, wc = y - y.mean(), want - want.mean()
            corr = float((yc * wc).sum() / torch.sqrt((yc ** 2).sum() * (wc ** 2).sum()))
            rows[p] = dict(
                error=float(((yc - wc) ** 2).sum() / (wc ** 2).sum()),
                level=decibels(torch.sqrt((yc ** 2).mean())) - decibels(torch.sqrt((wc ** 2).mean())),
                dc=decibels(abs(y.mean())),
                shape=1 - corr * corr,
            )
    return rows


def fit_of(rows):
    """align.ts's fit: 1 - the mean blend error at the eighths, held to 0..1."""
    return min(1.0, max(0.0, 1 - sum(rows[p]['error'] for p in EIGHTHS) / len(EIGHTHS)))


def report(label, blends):
    """blends: [(name, measure's rows)]."""
    print(f'{label}')
    print('            fit      worst error  worst level  worst DC  worst shape')
    for name, rows in blends:
        level = max((r['level'] for r in rows.values()), key=abs)
        print(f'  {name:9s} {100 * fit_of(rows):5.1f} %  {max(r["error"] for r in rows.values()):11.3f}  '
              f'{level:+8.1f} dB  {max(r["dc"] for r in rows.values()):5.0f} dB  '
              f'{max(r["shape"] for r in rows.values()):11.3f}')
    for name, rows in blends:
        print(f'  {name} error by sixteenth: ' + ' '.join(f'{rows[p]["error"]:.3f}' for p in SIXTEENTHS))


def train(morph, k, steps, signals, seed):
    """k waypoints, started on the chord, trained to make the path's blends the crossfade of the two models'
    outputs on excerpts of signals, each step at POSITIONS_PER_STEP positions spread over the path."""
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    f32 = torch.float32
    teach = []
    for s in signals:
        u = from_rest(s)
        ya, yb = morph.teachers(u)
        teach.append((u.to(f32), ya.to(f32), yb.to(f32), float(((0.5 * (ya + yb)) ** 2).mean())))
    loudest = max(t[3] for t in teach)
    share = np.array([len(t[1]) for t in teach], dtype=float)
    share /= share.sum()

    waypoints = torch.nn.Parameter(morph.chord(k).to(f32))
    opt = torch.optim.Adam([waypoints], lr=LEARNING_RATE)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, steps, eta_min=LEARNING_RATE / 20)
    t0 = time.time()
    mean = None
    for step in range(steps):
        positions = (np.arange(POSITIONS_PER_STEP) + rng.random(POSITIONS_PER_STEP)) / POSITIONS_PER_STEP
        us, wants, floors = [], [], []
        for p in positions:
            u, ya, yb, power = teach[rng.choice(len(teach), p=share)]
            at = int(rng.integers(0, len(ya) - CHUNK))
            us.append(u[at:at + CHUNK + RECEPTIVE])
            wants.append((1 - p) * ya[at:at + CHUNK] + p * yb[at:at + CHUNK])
            floors.append(LOSS_FLOOR * power)
        want = torch.stack(wants)
        error = morph.render(waypoints, torch.tensor(positions, dtype=f32), torch.stack(us)) - want
        dc = error.mean(dim=1)
        ac = ((error - dc[:, None]) ** 2).mean(dim=1)
        loss = (ac / (want.var(dim=1, correction=0) + torch.tensor(floors, dtype=f32)) + dc ** 2 / loudest).mean()
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_([waypoints], 1.0)
        opt.step()
        sched.step()
        value = loss.item()
        mean = value if mean is None else 0.98 * mean + 0.02 * value
        if step % 500 == 0 or step + 1 == steps:
            print(f'step {step + 1:5d} of {steps}  loss {mean:.4f}  {time.time() - t0:4.0f} s', flush=True)
    # As the slot stores them.
    return waypoints.detach().to(f32).to(torch.float64)


def write_path(path, a, b, waypoints, fit, straight_fit):
    head = PATH_MAGIC + struct.pack('<IIIIff', PATH_VERSION, waypoints.shape[0], a.crc, b.crc, fit, straight_fit)
    pathlib.Path(path).write_bytes(head + waypoints.numpy().astype('<f4').tobytes())


def read_path(path, a, b):
    raw = pathlib.Path(path).read_bytes()
    if raw[:4] != PATH_MAGIC or len(raw) < 28:
        sys.exit(f'{path}: not a path file')
    version, k, crc_a, crc_b = struct.unpack_from('<IIII', raw, 4)
    if version != PATH_VERSION or not 1 <= k <= MAX_WAYPOINTS or len(raw) != 28 + 4 * k * WEIGHT_COUNT:
        sys.exit(f'{path}: version {version}, {k} waypoints, {len(raw)} bytes')
    if (crc_a, crc_b) != (a.crc, b.crc):
        sys.exit(f'{path}: trained for other models')
    return torch.tensor(np.frombuffer(raw, '<f4', k * WEIGHT_COUNT, 28).astype(np.float64).reshape(k, WEIGHT_COUNT))


def check():
    """The network against the committed reference render, and the fit against the one editor/src/morph/align.ts
    stored in the fixture block (tools/make_morph_fixture.ts)."""
    fixtures = REPO / 'tests' / 'fixtures'
    a2 = Model(fixtures / 'nam' / 'A2.bin')
    u = read_wav(fixtures / 'nam' / 'input.wav')
    want = read_wav(fixtures / 'nam' / 'A2.input.ref.wav')
    with torch.no_grad():
        got = forward(a2.weights[None], from_rest(u)[None])[0]
    worst = float((got - want).abs().max())
    print(f'A2.bin on input.wav: within {worst:.2e} of A2.input.ref.wav')

    shuffled = Model(fixtures / 'morph' / 'A2_shuffled.bin')
    stored, = struct.unpack_from('<f', (fixtures / 'morph' / 'A2_shuffled.morph').read_bytes(), 60)
    morph = Morph(a2, shuffled)
    straight = fit_of(measure(morph, morph.chord(0), from_rest(morph_stimulus())))
    print(f'A2 into A2_shuffled, unaligned: fit {straight:.6f}, the fixture block stores {stored:.6f}')
    if worst > 1e-5 or abs(straight - stored) > 1e-5:
        sys.exit('check failed')


def main():
    torch.set_default_dtype(torch.float64)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('a', nargs='?', help="the preset's model")
    ap.add_argument('b', nargs='?', help="its morph target's model")
    ap.add_argument('-o', '--out', help='train and write the path file')
    ap.add_argument('--path', help='measure this path file instead of training')
    ap.add_argument('--waypoints', type=int, default=MAX_WAYPOINTS, choices=range(1, MAX_WAYPOINTS + 1))
    ap.add_argument('--steps', type=int, default=DEFAULT_STEPS)
    ap.add_argument('--di', default=str(DEFAULT_DI), help='a guitar DI recording (48 kHz WAV), used when it exists')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    if args.check:
        return check()
    if args.a is None or args.b is None:
        ap.error('give the two models')
    if args.out and args.path:
        ap.error('-o trains; --path measures')

    a, b = Model(args.a), Model(args.b)
    morph = Morph(a, b)
    print(f'A "{a.name}" crc {a.crc:08x}\nB "{b.name}" crc {b.crc:08x}')
    di = read_wav(args.di) if pathlib.Path(args.di).exists() else None
    split = int(DI_TRAIN_SHARE * len(di)) if di is not None else 0
    measured = [('stimulus (the stored fit)', from_rest(morph_stimulus()))]
    if di is not None:
        held = di[split - RECEPTIVE:]
        measured += [('DI, held out', held), ('DI, held out, -12 dB', 0.25 * held),
                     ('DI, held out, -24 dB', 0.0625 * held)]

    straight = morph.chord(0)
    path = None
    if args.path:
        path = read_path(args.path, a, b)
    elif args.out:
        signals = [training_plucks()] + ([g * di[:split] for g in DI_GAINS] if di is not None else [])
        print(f'training {args.waypoints} waypoints, {args.steps} steps, on plucks'
              + (f' and {split / SAMPLE_RATE:.0f} s of {args.di}' if di is not None
                 else f' only: {args.di} not found, and a path trained without a DI follows real playing less closely'))
        path = train(morph, args.waypoints, args.steps, signals, args.seed)

    fits = {}
    for label, u in measured:
        blends = [('straight', measure(morph, straight, u))]
        if path is not None:
            blends.append(('path', measure(morph, path, u)))
        report(label, blends)
        fits.setdefault('straight', fit_of(blends[0][1]))
        if path is not None:
            fits.setdefault('path', fit_of(blends[1][1]))
    if args.out:
        write_path(args.out, a, b, path, fits['path'], fits['straight'])
        print(f'{args.out}: {path.shape[0]} waypoints, fit {100 * fits["path"]:.1f} % (straight {100 * fits["straight"]:.1f} %)')


if __name__ == '__main__':
    main()
