// Resampling an IR to 48 kHz. Spec: docs/FILTERS.md "IR converter".
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/ir_convert.ts.

import { SAMPLE_RATE } from "../device";

const ZERO_CROSSINGS = 32;
const BETA = 8.6; // Kaiser window

// Modified Bessel function of the first kind, order 0.
function besselI0(z: number): number {
  const half = z / 2;
  let sum = 1;
  let term = 1;
  for (let k = 1; ; k++) {
    term *= half / k;
    const t2 = term * term;
    sum += t2;
    if (t2 < 1e-12 * sum) return sum;
  }
}

export function resampledLength(frames: number, rate: number): number {
  return Math.ceil((frames * SAMPLE_RATE) / rate);
}

// Direct windowed-sinc evaluation in float64: lowpass at 0.95 × the lower Nyquist, Kaiser β 8.6, 32 zero crossings
// each side. Returns min(resampledLength, maxOut) samples; the gain is not normalised.
export function resampleTo48k(x: Float32Array, rate: number, maxOut: number): Float64Array {
  const n = Math.min(resampledLength(x.length, rate), maxOut);
  const y = new Float64Array(n);
  if (rate === SAMPLE_RATE) {
    y.set(x.subarray(0, n));
    return y;
  }
  const fc = (0.95 * Math.min(rate, SAMPLE_RATE)) / 2;
  const c = (2 * fc) / rate; // cycles per source sample
  const halfWidth = ZERO_CROSSINGS / c;
  const i0Beta = besselI0(BETA);
  for (let i = 0; i < n; i++) {
    const t = (i * rate) / SAMPLE_RATE;
    const first = Math.max(Math.ceil(t - halfWidth), 0);
    const last = Math.min(Math.floor(t + halfWidth), x.length - 1);
    let acc = 0;
    for (let m = first; m <= last; m++) {
      const d = t - m;
      const r = d / halfWidth;
      const arg = Math.PI * c * d;
      const sinc = arg === 0 ? 1 : Math.sin(arg) / arg;
      acc += x[m]! * c * sinc * (besselI0(BETA * Math.sqrt(Math.max(0, 1 - r * r))) / i0Beta);
    }
    y[i] = acc;
  }
  return y;
}
