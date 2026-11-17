import { useMemo, useState } from "preact/hooks";
import { MID_HZ } from "../controls";
import { type Cut, type EqSettings, eqCurves, eqTotalDb, readEqSettings } from "../eqModel";
import { fixed, fmtHz, signedDb } from "../format";
import type { StatusMessage } from "../link/protocol";
import { GroupBox } from "./GroupBox";

interface Props {
  /** The pedal's last STATUS; null until one arrives. */
  status: StatusMessage | null;
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

const xOf = (f: number) => X0 + ((X1 - X0) * Math.log(f / F_MIN)) / Math.log(F_MAX / F_MIN);
const fOf = (x: number) => F_MIN * (F_MAX / F_MIN) ** ((x - X0) / (X1 - X0));
/** Clamped just outside the plot, which the clip path hides. */
const yOf = (db: number) =>
  Y0 + ((Y1 - Y0) * (DB_TOP - Math.max(DB_BOTTOM - 1, Math.min(DB_TOP + 1, db)))) / (DB_TOP - DB_BOTTOM);
const fmtDb = (db: number) => (Number.isFinite(db) ? signedDb(db) : "−∞ dB");
const path = (dbs: number[]) =>
  dbs.map((db, i) => `${i === 0 ? "M" : "L"}${xOf(FREQS[i]!).toFixed(1)},${yOf(db).toFixed(1)}`).join("");

type SectionId = "low" | "mid" | "high";

interface Marker {
  id: SectionId;
  letter: string;
  hz: number;
  text: string;
}

/** The sections in use, each marked at its frequency. */
function markers(s: EqSettings): Marker[] {
  const m: Marker[] = [];
  const cut = (id: SectionId, letter: string, label: string, c: Cut | null) => {
    if (c) m.push({ id, letter, hz: c.hz, text: `${label} ${fmtHz(c.hz)}, ${c.slope} dB/oct, Q ${c.q.toFixed(2)}` });
  };
  cut("low", "L", "Low-cut", s.lowCut);
  if (s.midDb !== 0) m.push({ id: "mid", letter: "M", hz: MID_HZ, text: `Mid ${fmtDb(s.midDb)} at ${MID_HZ} Hz` });
  cut("high", "H", "High-cut", s.highCut);
  return m;
}

interface Plot {
  /** null at Knob 2's off end. */
  total: string | null;
  fill: string | null;
  sections: { id: SectionId; d: string }[];
}

/** The response curves as SVG paths: the whole chain, its fill to 0 dB, each section in use alone. */
function plotPaths(s: EqSettings): Plot {
  const c = eqCurves(s, FREQS);
  const sections = markers(s).map(({ id }) => ({ id, d: path(c[id]) }));
  if (!Number.isFinite(s.outDb)) return { total: null, fill: null, sections };
  const total = path(c.total);
  const zeroY = yOf(0);
  return { total, fill: `M${X0},${zeroY}${total.replace("M", "L")}L${X1},${zeroY}Z`, sections };
}

/**
 * The EQ tab's response graph: low-cut, mid and high-cut with the output volume, from the pedal's knobs and toggles
 * as STATUS reports them. Read-only: the pedal is what you see is what you get.
 */
export function EqGraph({ status }: Props) {
  const [hoverHz, setHoverHz] = useState<number | null>(null);
  const settings = status && readEqSettings(status.knobs, status.toggles);
  // Keyed on the settings' values: STATUS arrives every 100 ms with a fresh object.
  const settingsKey = settings && JSON.stringify(settings);
  // biome-ignore lint/correctness/useExhaustiveDependencies: settingsKey stands for settings
  const plot = useMemo(() => (settings ? plotPaths(settings) : null), [settingsKey]);

  const onMove = (e: PointerEvent) => {
    const svg = e.currentTarget as SVGSVGElement;
    const m = svg.getScreenCTM();
    if (!m) return;
    const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse());
    setHoverHz(p.x >= X0 && p.x <= X1 && p.y >= Y0 && p.y <= Y1 ? fOf(p.x) : null);
  };

  const outOff = settings !== null && !Number.isFinite(settings.outDb);
  const badge = !settings ? "—" : outOff ? "OUT OFF" : `OUT ${signedDb(settings.outDb)}`;
  const offLabel = !status ? null : status.muted ? "MUTED" : outOff ? "VOLUME OFF" : null;

  return (
    <GroupBox
      class="eq-graph"
      title={
        <>
          Response <span class="badge">{badge}</span>
        </>
      }
    >
      <svg viewBox={`0 0 ${VIEW_W} ${VIEW_H}`} onPointerMove={onMove} onPointerLeave={() => setHoverHz(null)}>
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
            {plot.sections.map((c) => (
              <path key={c.id} class="eq-curve-section" data-section={c.id} d={c.d} />
            ))}
            {plot.fill && <path class="eq-fill-total" d={plot.fill} />}
            {plot.total && <path class="eq-curve-total" data-off={status?.muted} d={plot.total} />}
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
        {settings &&
          markers(settings).map((m) => {
            // On the whole chain's curve; with the volume off, on the EQ's alone.
            const x = xOf(m.hz);
            const y = yOf(eqTotalDb(outOff ? { ...settings, outDb: 0 } : settings, m.hz));
            return (
              <g key={m.id} class="eq-marker" data-section={m.id}>
                <title>{m.text}</title>
                <circle cx={x} cy={y} r={8} />
                <text x={x} y={y - 13} text-anchor="middle">
                  {m.letter}
                </text>
              </g>
            );
          })}
      </svg>
    </GroupBox>
  );
}
