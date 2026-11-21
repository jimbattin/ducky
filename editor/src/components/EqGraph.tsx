import { useMemo, useRef, useState } from "preact/hooks";
import { knobPosition } from "../controls";
import {
  BANDS,
  type Band,
  type BandId,
  bandParamNames,
  cutDb,
  type EqSettings,
  eqCurves,
  eqTotalDb,
  readEqSettings,
} from "../eqModel";
import { fixed, fmtHz, signedDb } from "../format";
import { CODE_MAX, codeOfUnits, paramValue, unitsOf } from "../link/params";
import type { ParamInfo, StatusMessage } from "../link/protocol";
import { GroupBox } from "./GroupBox";

interface Props {
  /** The pedal's last STATUS (Knob 2, the mute); null until one arrives. */
  status: StatusMessage | null;
  /** The pedal's parameter table. */
  params: readonly ParamInfo[];
  /** Per parameter id, the code shown (an edit made here, else the pedal's). */
  codes: readonly number[];
  /** Editing is possible (linked and synced). */
  enabled: boolean;
  /** A parameter set to a code (SET). */
  onSet: (id: number, code: number) => void;
}

// The plot: log frequency 20 Hz - 20 kHz across, +24..-36 dB down, in viewBox units.
const VIEW_W = 800;
const VIEW_H = 320;
const X0 = 48;
const X1 = 788;
const Y0 = 12;
const Y1 = 292;
const F_MIN = 20;
const F_MAX = 20000;
const DB_TOP = 24;
const DB_BOTTOM = -36;
const GRID_HZ: [number, string][] = [
  [20, "20"],
  [50, "50"],
  [100, "100"],
  [200, "200"],
  [500, "500"],
  [1000, "1k"],
  [2000, "2k"],
  [5000, "5k"],
  [10000, "10k"],
  [20000, "20k"],
];
const DB_LINES = [24, 18, 12, 6, 0, -6, -12, -18, -24, -30, -36];
const DB_LABELS = new Set([24, 12, 0, -12, -24, -36]);
/** 1/24 octave from 20 Hz to 20 kHz. */
const FREQS = Array.from({ length: 241 }, (_, i) => F_MIN * (F_MAX / F_MIN) ** (i / 240));
/** A sixth of an octave as a ratio: a frequency arrow step, and a Q wheel notch or PageUp/PageDown. */
const SIXTH_OCTAVE = 2 ** (1 / 6);
const OCTAVE_24TH = 2 ** (1 / 24); // a fine (Shift) frequency step

const xOf = (f: number) => X0 + ((X1 - X0) * Math.log(f / F_MIN)) / Math.log(F_MAX / F_MIN);
const fOf = (x: number) => F_MIN * (F_MAX / F_MIN) ** ((x - X0) / (X1 - X0));
/** Clamped just outside the plot, which the clip path hides. */
const yOf = (db: number) =>
  Y0 + ((Y1 - Y0) * (DB_TOP - Math.max(DB_BOTTOM - 1, Math.min(DB_TOP + 1, db)))) / (DB_TOP - DB_BOTTOM);
const dbOf = (y: number) => DB_TOP - ((y - Y0) * (DB_TOP - DB_BOTTOM)) / (Y1 - Y0);
const fmtDb = (db: number) => (Number.isFinite(db) ? signedDb(db) : "−∞ dB");
const path = (dbs: number[]) =>
  dbs.map((db, i) => `${i === 0 ? "M" : "L"}${xOf(FREQS[i]!).toFixed(1)},${yOf(db).toFixed(1)}`).join("");

interface Plot {
  /** null at Knob 2's off end. */
  total: string | null;
  fill: string | null;
  /** The bands in use, each alone (none while the EQ is off). */
  bands: { id: BandId; d: string }[];
}

/** The bands that shape the response: bells off 0 dB and cuts that are on, while the EQ is on. */
const shownBands = (s: EqSettings): Band[] =>
  !s.enabled
    ? []
    : BANDS.filter((b) =>
        b.kind === "lowCut"
          ? s.low !== null
          : b.kind === "highCut"
            ? s.high !== null
            : s.bells[bellIndex(b)]!.gain !== 0,
      );
const bellIndex = (b: Band): number => Number(b.letter) - 1;

/** The response curves as SVG paths: the whole chain, its fill to 0 dB, each band in use alone. */
function plotPaths(s: EqSettings): Plot {
  const c = eqCurves(s, FREQS);
  const bands = shownBands(s).map(({ id }) => ({ id, d: path(c[id]) }));
  if (!Number.isFinite(s.outDb)) return { total: null, fill: null, bands };
  const total = path(c.total);
  const zeroY = yOf(0);
  return { total, fill: `M${X0},${zeroY}${total.replace("M", "L")}L${X1},${zeroY}Z`, bands };
}

/** A parameter with its shown code and that code's value in its unit (a choice: its index). */
interface Row {
  p: ParamInfo;
  code: number;
  units: number;
}

/** A band's handle: its rows, where it sits and what it reads. */
interface Handle {
  band: Band;
  /** A bell's gain; null for a cut. */
  gain: Row | null;
  freq: Row;
  /** A bell's Q or a cut's resonance. */
  q: Row;
  /** A cut's slope (dB/oct); null for a bell. */
  slope: number | null;
  /** A cut at its off end. */
  off: boolean;
  x: number;
  y: number;
  text: string;
}

/** Every band's handle from the settings and the rows behind them; [] when the table lacks a band's parameter. */
function handles(s: EqSettings, params: readonly ParamInfo[], codes: readonly number[]): Handle[] {
  const out: Handle[] = [];
  for (const band of BANDS) {
    const rows = bandParamNames(band).map((name): Row | null => {
      const p = params.find((x) => x.name === name);
      const code = p === undefined ? undefined : codes[p.id];
      if (p === undefined || code === undefined) return null;
      return { p, code, units: p.taper === "choice" ? paramValue(p, code) : unitsOf(p, paramValue(p, code)) };
    });
    if (rows.some((r) => r === null)) return [];
    const [a, b, c] = rows as [Row, Row, Row];
    if (band.kind === "bell") {
      const bell = s.bells[bellIndex(band)]!;
      const text = `${fmtDb(bell.gain)} at ${fmtHz(bell.hz)}, Q ${bell.q.toFixed(2)}`;
      out.push({ band, gain: a, freq: b, q: c, slope: null, off: false, x: xOf(bell.hz), y: yOf(bell.gain), text });
      continue;
    }
    const hp = band.kind === "lowCut";
    const cut = hp ? s.low : s.high;
    if (cut === null) {
      const x = xOf(hp ? a.p.min : a.p.max);
      out.push({ band, gain: null, freq: a, q: b, slope: null, off: true, x, y: yOf(0), text: "Off" });
      continue;
    }
    const text = `${cut.slope} dB/oct at ${fmtHz(cut.hz)}${cut.slope === 6 ? "" : `, Q ${cut.q.toFixed(2)}`}`;
    const y = yOf(cutDb(cut, hp, cut.hz));
    out.push({ band, gain: null, freq: a, q: b, slope: cut.slope, off: false, x: xOf(cut.hz), y, text });
  }
  return out;
}

/**
 * The EQ tab's response graph: low-cut, bells 1-3 and high-cut from their parameters' shown values, with Knob 2's
 * output volume, and a handle per band. Drag a bell (frequency and gain) or a cut (frequency; past its off end it is
 * off); the wheel sets a bell's Q or a cut's resonance; a double-click resets a bell's gain or turns a cut off.
 */
export function EqGraph({ status, params, codes, enabled, onSet }: Props) {
  const svgRef = useRef<SVGSVGElement>(null);
  /** The band being dragged and the pointer's offset from its handle's centre at the grab (viewBox units). */
  const drag = useRef<{ band: BandId; dx: number; dy: number } | null>(null);
  const [hoverHz, setHoverHz] = useState<number | null>(null);
  const settings = status ? readEqSettings(params, codes, knobPosition(status.knobs, 1)) : null;
  // Keyed on the settings' values: STATUS and VALUES arrive every 100 ms with fresh objects.
  const settingsKey = settings && JSON.stringify(settings);
  // biome-ignore lint/correctness/useExhaustiveDependencies: settingsKey stands for settings
  const plot = useMemo(() => (settings ? plotPaths(settings) : null), [settingsKey]);
  const hs = settings ? handles(settings, params, codes) : [];

  const toPlot = (e: PointerEvent): { x: number; y: number } | null => {
    const m = svgRef.current?.getScreenCTM();
    if (!m) return null;
    const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse());
    return { x: p.x, y: p.y };
  };
  /** Sends row's code for units (clamped to its range: a cut dragged past its off end is off) when it changes. */
  const setUnits = (row: Row, units: number) => {
    const code = codeOfUnits(row.p, units);
    if (code !== row.code) onSet(row.p.id, code);
  };
  /** The cut's resonance acts: it is on and steeper than 6 dB/oct. */
  const resonant = (h: Handle) => h.gain !== null || (!h.off && h.slope !== 6);
  /** Double-click and Home: a bell's gain to its default, a cut off. */
  const reset = (h: Handle) => {
    if (h.gain !== null) {
      if (h.gain.code !== h.gain.p.defCode) onSet(h.gain.p.id, h.gain.p.defCode);
      return;
    }
    const offCode = h.band.kind === "lowCut" ? 0 : CODE_MAX;
    if (h.freq.code !== offCode) onSet(h.freq.p.id, offCode);
  };

  const onMove = (e: PointerEvent) => {
    const p = toPlot(e);
    if (!p) return;
    const d = drag.current;
    if (d) {
      const h = hs.find((x) => x.band.id === d.band);
      if (!h || !enabled) return;
      setUnits(h.freq, fOf(p.x + d.dx));
      if (h.gain !== null) setUnits(h.gain, dbOf(p.y + d.dy));
      return;
    }
    setHoverHz(p.x >= X0 && p.x <= X1 && p.y >= Y0 && p.y <= Y1 ? fOf(p.x) : null);
  };
  const onKey = (e: KeyboardEvent, h: Handle) => {
    const freqStep = e.shiftKey ? OCTAVE_24TH : SIXTH_OCTAVE;
    const gainStep = e.shiftKey ? 0.1 : 0.5;
    if (e.key === "ArrowLeft") setUnits(h.freq, h.freq.units / freqStep);
    else if (e.key === "ArrowRight") setUnits(h.freq, h.freq.units * freqStep);
    else if (e.key === "ArrowUp" && h.gain !== null) setUnits(h.gain, h.gain.units + gainStep);
    else if (e.key === "ArrowDown" && h.gain !== null) setUnits(h.gain, h.gain.units - gainStep);
    else if (e.key === "PageUp" && resonant(h)) setUnits(h.q, h.q.units * SIXTH_OCTAVE);
    else if (e.key === "PageDown" && resonant(h)) setUnits(h.q, h.q.units / SIXTH_OCTAVE);
    else if (e.key === "Home") reset(h);
    else return;
    e.preventDefault();
  };
  const endDrag = () => {
    drag.current = null;
  };

  const outOff = settings !== null && !Number.isFinite(settings.outDb);
  const eqOff = settings !== null && !settings.enabled;
  const badge = !settings ? "—" : outOff ? "OUT OFF" : `OUT ${signedDb(settings.outDb)}`;
  const offLabel = !status ? null : status.muted ? "MUTED" : outOff ? "VOLUME OFF" : eqOff ? "EQ OFF" : null;

  return (
    <GroupBox
      class="eq-graph"
      title={
        <>
          Response <span class="badge">{badge}</span>
        </>
      }
    >
      <svg
        ref={svgRef}
        viewBox={`0 0 ${VIEW_W} ${VIEW_H}`}
        onPointerMove={onMove}
        onPointerLeave={() => setHoverHz(null)}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
      >
        <title>EQ response</title>
        <defs>
          <clipPath id="eq-plot">
            <rect x={X0} y={Y0} width={X1 - X0} height={Y1 - Y0} />
          </clipPath>
        </defs>
        {GRID_HZ.map(([f, label]) => (
          <g key={f}>
            <line class="eq-grid" x1={xOf(f)} x2={xOf(f)} y1={Y0} y2={Y1} />
            <text class="eq-grid-label" x={xOf(f)} y={Y1 + 18} text-anchor="middle">
              {label}
            </text>
          </g>
        ))}
        {DB_LINES.map((db) => (
          <g key={db}>
            <line class={db === 0 ? "eq-zero" : "eq-grid"} x1={X0} x2={X1} y1={yOf(db)} y2={yOf(db)} />
            {DB_LABELS.has(db) && (
              <text class="eq-grid-label" x={X0 - 8} y={yOf(db) + 4} text-anchor="end">
                {db > 0 ? `+${db}` : fixed(db, 0)}
              </text>
            )}
          </g>
        ))}
        {!settings && (
          <text class="eq-readout" x={(X0 + X1) / 2} y={(Y0 + Y1) / 2} text-anchor="middle">
            —
          </text>
        )}
        {plot && (
          <g clip-path="url(#eq-plot)">
            {plot.bands.map((c) => (
              <path key={c.id} class="eq-curve-band" data-band={c.id} d={c.d} />
            ))}
            {plot.fill && <path class="eq-fill-total" d={plot.fill} />}
            {plot.total && <path class="eq-curve-total" data-off={status?.muted || eqOff} d={plot.total} />}
          </g>
        )}
        {offLabel && (
          <text class="eq-off-label" x={X0 + 10} y={Y0 + 20}>
            {offLabel}
          </text>
        )}
        {settings && hoverHz !== null && (
          <g>
            <line class="eq-cursor" x1={xOf(hoverHz)} x2={xOf(hoverHz)} y1={Y0} y2={Y1} />
            <text class="eq-readout" x={X1 - 8} y={Y0 + 20} text-anchor="end">
              {`${fmtHz(hoverHz)} · ${fmtDb(eqTotalDb(settings, hoverHz))}`}
            </text>
          </g>
        )}
        {hs.map((h) => {
          const value = h.gain ?? h.freq;
          const interactive = enabled
            ? {
                role: "slider" as const,
                tabindex: 0, // an SVG attribute is case-sensitive: preact would write "tabIndex", which focuses nothing
                "aria-label": h.band.label,
                "aria-valuemin": value.p.min,
                "aria-valuemax": value.p.max,
                "aria-valuenow": value.units,
                "aria-valuetext": h.text,
                onPointerDown: (e: PointerEvent) => {
                  const p = toPlot(e);
                  if (!p) return;
                  (e.currentTarget as Element).setPointerCapture(e.pointerId);
                  drag.current = { band: h.band.id, dx: h.x - p.x, dy: h.y - p.y };
                  setHoverHz(null);
                },
                onWheel: (e: WheelEvent) => {
                  if (!resonant(h) || e.deltaY === 0) return;
                  e.preventDefault();
                  setUnits(h.q, e.deltaY < 0 ? h.q.units * SIXTH_OCTAVE : h.q.units / SIXTH_OCTAVE);
                },
                onDblClick: () => reset(h),
                onKeyDown: (e: KeyboardEvent) => onKey(e, h),
              }
            : { "aria-hidden": true };
          return (
            <g
              key={h.band.id}
              class="eq-handle"
              data-band={h.band.id}
              data-enabled={enabled}
              data-off={h.off}
              {...interactive}
            >
              <title>{`${h.band.label}: ${h.text}`}</title>
              <circle cx={h.x} cy={h.y} r={8} />
              <text x={h.x} y={h.y - 13} text-anchor="middle">
                {h.band.letter}
              </text>
            </g>
          );
        })}
      </svg>
    </GroupBox>
  );
}
