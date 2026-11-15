#!/usr/bin/env -S uv run
"""Write the guitar DI stimulus for the NAM reference renders to renders/stimuli/.

    guitar_di  the first 20 s of the left channel of the 48 kHz, 16-bit stereo WAV given by
               --guitar (default ../spx90/guitar.wav), as mono IEEE float32

The source is pinned by sha256; a different file is refused rather than rendered. The output
uses the 44-byte header of NeuralAmpModelerCore's tools/render.cpp SaveWavFloat32.

Usage:
    uv run tools/make_stimuli.py [--guitar WAV]
"""
import argparse
import array
import hashlib
import pathlib
import struct

REPO = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_GUITAR = REPO.parent / 'spx90' / 'guitar.wav'
GUITAR_SHA256 = 'af49eb8e54e42910af477d1bb9ce165caac1ce70bd06532268449a1a537828f3'
OUT = REPO / 'renders' / 'stimuli' / 'guitar_di.wav'
FS = 48000
FRAMES = 20 * FS


def fail(path, what, found, need):
    raise SystemExit(f'make_stimuli: {path}: {what} is {found}, need {need}')


def read_left(path):
    """Return the first FRAMES left-channel samples of the pinned 16-bit stereo WAV as floats."""
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != GUITAR_SHA256:
        fail(path, 'sha256', digest, GUITAR_SHA256)
    if raw[0:4] != b'RIFF' or raw[8:12] != b'WAVE':
        fail(path, 'header', raw[0:4] + raw[8:12], b'RIFFWAVE')
    fmt = data = None
    pos = 12
    while pos + 8 <= len(raw):
        cid, size = struct.unpack_from('<4sI', raw, pos)
        body = raw[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', body)
        elif cid == b'data':
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        fail(path, 'chunks', 'missing fmt or data', 'both')
    tag, channels, rate, _, _, bits = fmt
    for what, found, need in (('format', tag, 1), ('bits', bits, 16), ('sample rate', rate, FS),
                              ('channels', channels, 2)):
        if found != need:
            fail(path, what, found, need)
    frames = len(data) // 4
    if frames < FRAMES:
        fail(path, 'frame count', frames, f'at least {FRAMES}')
    pcm = array.array('h')  # native order; the tools run on little-endian hosts like the WAV
    pcm.frombytes(data[:FRAMES * 4])
    return array.array('f', (s / 32768 for s in pcm[0::2]))


def write_float32(path, samples):
    data = samples.tobytes()
    header = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(data), b'WAVE', b'fmt ', 16, 3, 1, FS,
                         FS * 4, 4, 32, b'data', len(data))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--guitar', type=pathlib.Path, default=DEFAULT_GUITAR)
    args = ap.parse_args()
    write_float32(OUT, read_left(args.guitar))
    print(f'wrote {OUT.relative_to(REPO)}')


if __name__ == '__main__':
    main()
