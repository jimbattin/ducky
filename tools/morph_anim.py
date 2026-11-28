#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Animate a preset morph as the morph knob turns (docs/MORPH.md), either of the two ways the pedal plays it.

    uv run tools/morph_anim.py A.bin B.bin -o OUT.mp4                  the weight build, a straight blend
    uv run tools/morph_anim.py A.bin B.bin --path F.path -o OUT.mp4    the weight build, a trained path
    uv run tools/morph_anim.py A.bin B.bin --xfade -o OUT.mp4          the default image: both models at once

A.bin and B.bin are Ducky model binaries (docs/NAM_FORMAT.md "Ducky model binary"). OUT is .mp4 or .gif
(ffmpeg writes it). --path takes a path file of tools/morph_train.py -o.

Both frames have the knob and the blend position it maps to (the 2 % end zones, daisy/src/morph.h
BlendPosition), and draw a network's 1,870 weights as cells, in the weight stream's order, head scale
folded in.

The weight build ("What a blend is"): one network, its weights shifting from A's to B's, and the weights
that travel furthest as lines over the knob's travel, straight between the path's nodes. The blend drawn
is the unaligned one (the block's identity transform), which is what the analysis keeps for captures of
different amps and what a trained path uses.

The crossfade ("On the pedal: both models at once"): two networks whose weights stay put, each one's
level-matched output of a plucked note at its own input calibration, the equal-power gains on t² (the pedal's
daisy/src/ducky.cpp stepMorph: the target fades in slowly), and their mix.
"""
import argparse
import math
import pathlib
import struct
import subprocess
import sys

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.patches import Circle, Rectangle, Wedge

SAMPLE_RATE = 48000
# The A2-Lite shape, the model binary and the path file (tools/morph_train.py).
KERNELS = [6] * 14 + [15, 15] + [6] * 7
DILATIONS = [1, 3, 7, 17, 41, 101, 239] * 2 + [1, 13] + [1, 3, 7, 17, 41, 101, 239]
WEIGHT_COUNT, HEAD_BASE, HEAD_KERNEL = 1870, 1821, 16
LAYER_BASES = [3 + sum(9 * k + 18 for k in KERNELS[:l]) for l in range(len(KERNELS))]
RECEPTIVE = sum((k - 1) * d for k, d in zip(KERNELS, DILATIONS)) + HEAD_KERNEL - 1
MODEL_BIN_SIZE = 7908
OFF_FLAGS, OFF_WEIGHTS, OFF_HEAD_SCALE, OFF_NAME, OFF_CRC = 6, 92, 7572, 7584, 7904
FLAG_HAS_LOUDNESS, FLAG_HAS_INPUT_LEVEL = 1, 2
LEVEL_TARGET_DB, INPUT_LEVEL_DBU, DEFAULT_MODEL_INPUT_DBU = -18.0, 4.5, 12.0
PATH_MAGIC = b'DKMP'
END_ZONE = 0.02  # dkmorph::kEndZone

FPS = 30
HOLD_START, HOLD_END = 0.6, 1.0  # seconds the knob rests at each end
GRID = (22, 85)                  # rows x columns: 1,870 cells
TRACES = 12                      # the weight build's weights drawn as lines
# The crossfade's signal: a plucked A2 at half scale, and the part of it drawn.
PLUCK_HZ, PLUCK_LEVEL, PLUCK_SECONDS = 110.0, 0.5, 0.3
WINDOW = slice(round(0.25 * SAMPLE_RATE), round(0.275 * SAMPLE_RATE))
BG, FG, MUTED, ACCENT = '#14161a', '#e8e6e3', '#8a8f98', '#ffb000'
COOL, WARM = '#3aa0ff', '#ff7a1a'


class Model:
    def __init__(self, path):
        raw = pathlib.Path(path).read_bytes()
        at = raw.find(b'DKNM')
        if at < 0 or len(raw) < at + MODEL_BIN_SIZE:
            sys.exit(f'{path}: no model binary')
        b = raw[at:at + MODEL_BIN_SIZE]
        flags, = struct.unpack_from('<H', b, OFF_FLAGS)
        head_scale, loudness, input_level = struct.unpack_from('<fff', b, OFF_HEAD_SCALE)
        self.weights = np.frombuffer(b, '<f4', WEIGHT_COUNT, OFF_WEIGHTS).astype(np.float64)
        self.weights[HEAD_BASE:] *= head_scale
        self.level_db = LEVEL_TARGET_DB - loudness if flags & FLAG_HAS_LOUDNESS else 0.0
        self.cal_db = INPUT_LEVEL_DBU - (input_level if flags & FLAG_HAS_INPUT_LEVEL else DEFAULT_MODEL_INPUT_DBU)
        self.crc, = struct.unpack_from('<I', b, OFF_CRC)
        self.name = b[OFF_NAME:OFF_NAME + 64].split(b'\0')[0].decode('ascii', 'replace')

    def render(self, u):
        """The level-matched output on the signal u at the model's input calibration."""
        return 10 ** (self.level_db / 20) * forward(self.weights, 10 ** (self.cal_db / 20) * u)


def forward(w, u):
    """The network w (head scale folded in) on the signal u (T + RECEPTIVE samples): T samples.

    As tools/morph_train.py forward: u's first RECEPTIVE samples are the history of the first output.
    """
    t = len(u) - RECEPTIVE
    x = w[0:3, None] * u
    h = 0
    for l, (k, d) in enumerate(zip(KERNELS, DILATIONS)):
        base = LAYER_BASES[l]
        rest = base + 9 * k  # cb[3], m[3], L[9], lb[3]
        cut = (k - 1) * d
        n = x.shape[1] - cut
        taps = w[base:rest].reshape(3, 3, k)
        z = sum(taps[:, :, i] @ x[:, i * d:i * d + n] for i in range(k)) + w[rest:rest + 3, None]
        u = u[cut:]
        z = z + w[rest + 3:rest + 6, None] * u
        a = np.where(z > 0, z, 0.01 * z)
        h = h + a[:, -(t + HEAD_KERNEL - 1):]
        x = x[:, cut:] + w[rest + 15:rest + 18, None] + w[rest + 6:rest + 15].reshape(3, 3) @ a
    head = w[HEAD_BASE:HEAD_BASE + 48].reshape(3, HEAD_KERNEL)
    return sum(head[:, i] @ h[:, i:i + t] for i in range(HEAD_KERNEL)) + w[HEAD_BASE + 48]


def pluck():
    """A Karplus-Strong note after RECEPTIVE samples of silence."""
    period = round(SAMPLE_RATE / PLUCK_HZ)
    ring = np.random.default_rng(1).uniform(-1, 1, period)
    out = np.empty(round(PLUCK_SECONDS * SAMPLE_RATE))
    for i in range(len(out)):
        out[i] = ring[i % period]
        ring[i % period] = 0.498 * (out[i] + ring[(i + 1) % period])
    return np.concatenate([np.zeros(RECEPTIVE), PLUCK_LEVEL * out / np.abs(out).max()])


def read_waypoints(path, a, b):
    raw = pathlib.Path(path).read_bytes()
    if raw[:4] != PATH_MAGIC or len(raw) < 28:
        sys.exit(f'{path}: not a path file')
    version, k, crc_a, crc_b = struct.unpack_from('<IIII', raw, 4)
    if version != 1 or not 1 <= k <= 3 or len(raw) != 28 + 4 * k * WEIGHT_COUNT:
        sys.exit(f'{path}: version {version}, {k} waypoints, {len(raw)} bytes')
    if (crc_a, crc_b) != (a.crc, b.crc):
        sys.exit(f'{path}: trained for other models')
    return np.frombuffer(raw, '<f4', k * WEIGHT_COUNT, 28).astype(np.float64).reshape(k, WEIGHT_COUNT)


def blend_position(v):
    return min(max((v - END_ZONE) / (1 - 2 * END_ZONE), 0.0), 1.0)


def segment_at(t, segments):
    s = min(int(t * segments), segments - 1)
    return s, t * segments - s


def knob_value(time, sweep):
    """The morph parameter (0..1) at a time: at rest, a sweep that eases in and out, at rest."""
    x = min(max((time - HOLD_START) / sweep, 0.0), 1.0)
    return x * x * (3 - 2 * x)


class Stage:
    """The frame both animations share: the title, the knob with the path's nodes as ticks, the readout
    under it and the two models' names. set_knob(v) turns the knob."""

    def __init__(self, title, subtitle, a, b, knots, labels):
        plt.rcParams.update({'font.family': 'Noto Sans', 'text.color': FG, 'axes.edgecolor': MUTED,
                             'axes.labelcolor': MUTED, 'xtick.color': MUTED, 'ytick.color': MUTED})
        self.fig = fig = plt.figure(figsize=(12.8, 7.2), dpi=100, facecolor=BG)
        fig.text(0.03, 0.935, title, fontsize=24, weight='bold')
        fig.text(0.03, 0.895, subtitle, fontsize=13, color=MUTED)

        kx = fig.add_axes([0.02, 0.44, 0.25, 0.4])  # 270 degrees of travel
        kx.set_xlim(-1.5, 1.5)
        kx.set_ylim(-1.35, 1.5)
        kx.set_aspect('equal')
        kx.axis('off')
        kx.add_patch(Wedge((0, 0), 1.18, -45, 225, width=0.07, color='#2a2e35'))
        self.travel = kx.add_patch(Wedge((0, 0), 1.18, 225, 225, width=0.07, color=ACCENT))
        for v, label in zip(knots, labels):
            r = math.radians(self.angle(v))
            kx.plot([1.24 * math.cos(r), 1.3 * math.cos(r)], [1.24 * math.sin(r), 1.3 * math.sin(r)], color=MUTED,
                    lw=1.5)
            kx.text(1.45 * math.cos(r), 1.45 * math.sin(r), label, ha='center', va='center', fontsize=12, color=MUTED)
        kx.add_patch(Circle((0, 0), 1.0, color='#2a2e35'))
        kx.add_patch(Circle((0, 0), 0.86, color='#3a3f48'))
        self.pointer, = kx.plot([], [], color=ACCENT, lw=6, solid_capstyle='round')
        kx.text(0, -1.22, 'MORPH', ha='center', fontsize=12, color=MUTED)
        self.percent = kx.text(0, -0.12, '', ha='center', fontsize=26, weight='bold')

        self.readout = fig.text(0.035, 0.40, '', fontsize=13, va='top', linespacing=1.7, family='DejaVu Sans Mono')
        fig.text(0.035, 0.17, f'A  {a.name}\nB  {b.name}', fontsize=10, color=MUTED, va='top', linespacing=1.7)

    @staticmethod
    def angle(v):
        return 225 - 270 * v

    def set_knob(self, v):
        r = math.radians(self.angle(v))
        self.pointer.set_data([0.45 * math.cos(r), 0.8 * math.cos(r)], [0.45 * math.sin(r), 0.8 * math.sin(r)])
        self.travel.set_theta1(self.angle(v))
        self.percent.set_text(f'{100 * v:.0f} %')

    def weight_cells(self, rect, weights, peak):
        """Axes at rect with a network's weights as cells; returns set(weights). The colour takes the square
        root of a weight's size against peak, which keeps the small weights visible."""
        shade = lambda w: (np.sign(w) * np.sqrt(np.abs(w) / peak)).reshape(GRID)
        ax = self.fig.add_axes(rect)
        cmap = LinearSegmentedColormap.from_list('weights', [COOL, '#1c2026', WARM])
        cells = ax.imshow(shade(weights), cmap=cmap, vmin=-1, vmax=1, aspect='auto', interpolation='nearest')
        ax.axis('off')
        return lambda w: cells.set_data(shade(w))

    def travel_axes(self, rect, knots, labels):
        """Axes at rect over the knob's travel: the nodes as ticks, the end zones shaded; and its cursor."""
        ax = self.fig.add_axes(rect, facecolor=BG)
        for v in knots:
            ax.axvline(v, color=MUTED, lw=0.6, ls=':')
        for x in (0, 1 - END_ZONE):
            ax.add_patch(Rectangle((x, 0), END_ZONE, 1, transform=ax.get_xaxis_transform(), color=MUTED, alpha=0.25,
                                   lw=0))
        ax.set_xlim(0, 1)
        ax.set_xticks(knots, labels)
        ax.tick_params(length=0, labelsize=11)
        for side in ('top', 'right'):
            ax.spines[side].set_visible(False)
        return ax, ax.axvline(0, color=ACCENT, lw=2)

    def encode(self, out, seconds, draw):
        """Writes the animation: draw(v) sets the frame for the knob at v."""
        width, height = self.fig.canvas.get_width_height()
        codec = ['-vf', 'split[a][b];[a]palettegen[p];[b][p]paletteuse'] if out.endswith('.gif') else \
                ['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18']
        ffmpeg = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgba',
                                   '-s', f'{width}x{height}', '-r', str(FPS), '-i', '-', *codec, out],
                                  stdin=subprocess.PIPE)
        for frame in range(round(seconds * FPS)):
            v = knob_value(frame / FPS, seconds - HOLD_START - HOLD_END)
            self.set_knob(v)
            draw(v)
            self.fig.canvas.draw()
            ffmpeg.stdin.write(self.fig.canvas.buffer_rgba())
        ffmpeg.stdin.close()
        if ffmpeg.wait():
            sys.exit(f'ffmpeg: exit {ffmpeg.returncode}')
        print(f'{out}: {seconds:g} s, {width}x{height}')


def weight_morph(a, b, waypoints):
    """The weight build: returns the stage and its draw(v)."""
    nodes = np.vstack([a.weights, waypoints, b.weights])
    segments = len(nodes) - 1
    labels = ['A'] + [f'W{i + 1}' for i in range(len(waypoints))] + ['B']
    knots = [END_ZONE + (1 - 2 * END_ZONE) * i / segments for i in range(len(nodes))]  # knob values of the nodes
    traced = np.sort(np.argsort(np.abs(np.diff(nodes, axis=0)).sum(axis=0))[-TRACES:])

    stage = Stage('Weight morph', 'one network runs; the knob moves its weights from A’s to B’s'
                  + (f' through {len(waypoints)} trained waypoints' if len(waypoints) else ' along a straight line'),
                  a, b, knots, labels)
    fig = stage.fig
    set_cells = stage.weight_cells([0.30, 0.50, 0.67, 0.33], nodes[0], np.abs(nodes).max())
    fig.text(0.30, 0.845, 'the engine’s 1,870 weights (23 layers, then the head)', fontsize=12)
    fig.text(0.97, 0.845, 'negative', fontsize=11, color=COOL, ha='right')
    fig.text(0.905, 0.845, 'positive', fontsize=11, color=WARM, ha='right')

    tx, cursor = stage.travel_axes([0.30, 0.10, 0.67, 0.31], knots, labels)
    tx.plot([0, *knots, 1], nodes[[0, *range(len(nodes)), -1]][:, traced], lw=1.2, alpha=0.75)
    tx.set_ylabel('weight', fontsize=11)
    fig.text(0.30, 0.425, f'the {TRACES} weights that travel furthest: straight between nodes, exact in the 2 % end zones',
             fontsize=12)
    fig.text(0.635, 0.03, 'knob travel', fontsize=11, color=MUTED, ha='center')
    dots = tx.scatter(np.zeros(TRACES), nodes[0, traced], s=28, color=FG, zorder=3)

    def draw(v):
        t = blend_position(v)
        s, f = segment_at(t, segments)
        w = nodes[s] + f * (nodes[s + 1] - nodes[s])
        stage.readout.set_text(f'blend position t = {t:.3f}\n'
                               f'segment {labels[s]} → {labels[s + 1]}, f = {f:.3f}\n'
                               f'w = {labels[s]} + f · ({labels[s + 1]} − {labels[s]})\n'
                               f'level match {(1 - t) * a.level_db + t * b.level_db:+.2f} dB\n'
                               f'input cal   {(1 - t) * a.cal_db + t * b.cal_db:+.2f} dB')
        set_cells(w)
        cursor.set_xdata([v, v])
        dots.set_offsets(np.column_stack([np.full(TRACES, v), w[traced]]))

    return stage, draw


def crossfade(a, b):
    """The default image: returns the stage and its draw(v)."""
    knots, labels = [END_ZONE, 1 - END_ZONE], ['A', 'B']
    u = pluck()
    ya, yb = a.render(u)[WINDOW], b.render(u)[WINDOW]
    ms = 1000 * np.arange(len(ya)) / SAMPLE_RATE
    span = 1.08 * max(np.abs(ya).max(), np.abs(yb).max())
    peak = max(np.abs(a.weights).max(), np.abs(b.weights).max())

    stage = Stage('Crossfade morph', 'both networks run, their weights fixed; the knob mixes their outputs at equal power on t²',
                  a, b, knots, labels)
    fig = stage.fig

    def wave(rect, y, colour, ghost):
        ax = fig.add_axes(rect, facecolor=BG)
        ax.plot(ms, y, color=colour, lw=1, alpha=0.3 if ghost else 0)  # the output at full gain
        line, = ax.plot(ms, y, color=colour, lw=1.6)
        ax.set_xlim(ms[0], ms[-1])
        ax.set_ylim(-span, span)
        ax.set_xticks([])
        ax.set_yticks([])
        for spine in ax.spines.values():
            spine.set_visible(False)
        ax.axhline(0, color=MUTED, lw=0.5, zorder=0)
        return line

    columns = [(0.30, a, ya, COOL, 'A'), (0.645, b, yb, WARM, 'B')]
    lines, captions = [], []
    for x, model, y, colour, label in columns:
        fig.text(x, 0.845, f'engine {label}: its 1,870 weights', fontsize=12, color=colour)
        stage.weight_cells([x, 0.65, 0.325, 0.18], model.weights, peak)
        captions.append(fig.text(x, 0.605, '', fontsize=12, color=colour))
        lines.append(wave([x, 0.44, 0.325, 0.15], y, colour, True))

    travel = np.linspace(0, 1, 501)
    angles = np.array([blend_position(v) ** 2 for v in travel]) * math.pi / 2
    gx, cursor = stage.travel_axes([0.30, 0.10, 0.27, 0.25], knots, labels)
    gx.plot(travel, np.cos(angles), color=COOL, lw=1.6)
    gx.plot(travel, np.sin(angles), color=WARM, lw=1.6)
    gx.set_ylim(0, 1.08)
    gx.set_yticks([0, 0.5, 1])
    dots = gx.scatter([0, 0], [1, 0], s=36, color=[COOL, WARM], zorder=3)
    fig.text(0.30, 0.365, 'the gains over the knob’s travel', fontsize=12)
    fig.text(0.435, 0.03, 'knob travel', fontsize=11, color=MUTED, ha='center')

    fig.text(0.645, 0.365, 'the output: gain A · A + gain B · B', fontsize=12)
    mix = wave([0.645, 0.10, 0.325, 0.25], ya, FG, False)
    fig.text(0.8075, 0.03, f'{ms[-1]:.0f} ms of a plucked {PLUCK_HZ:.0f} Hz note', fontsize=11, color=MUTED, ha='center')

    def draw(v):
        t = blend_position(v)
        gains = math.cos(t * t * math.pi / 2), math.sin(t * t * math.pi / 2)
        stage.readout.set_text(f'blend position t = {t:.3f}\n'
                               f'gain A = cos(πt²/2) = {gains[0]:.3f}\n'
                               f'gain B = sin(πt²/2) = {gains[1]:.3f}\n'
                               f'out = gain A·A + gain B·B\n'
                               f'both engines always run')
        for line, caption, gain, (_, _, y, _, label) in zip(lines, captions, gains, columns):
            line.set_ydata(gain * y)
            caption.set_text(f'{label}’s level-matched output × {gain:.2f}')
        mix.set_ydata(gains[0] * ya + gains[1] * yb)
        cursor.set_xdata([v, v])
        dots.set_offsets([[v, gains[0]], [v, gains[1]]])

    return stage, draw


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--path', help='the weight build along a trained path file; without it, the straight blend')
    ap.add_argument('--xfade', action='store_true', help='the default image: both models, their outputs crossfaded')
    ap.add_argument('-o', '--out', required=True, help='.mp4 or .gif')
    ap.add_argument('--seconds', type=float, default=8.0)
    args = ap.parse_args()
    if args.seconds <= HOLD_START + HOLD_END:
        sys.exit(f'--seconds: more than {HOLD_START + HOLD_END}')
    if args.xfade and args.path:
        sys.exit('--path: the crossfade does not use a path')

    a, b = Model(args.a), Model(args.b)
    if args.xfade:
        stage, draw = crossfade(a, b)
    else:
        stage, draw = weight_morph(a, b, read_waypoints(args.path, a, b) if args.path else np.empty((0, WEIGHT_COUNT)))
    stage.encode(args.out, args.seconds, draw)


if __name__ == '__main__':
    main()
