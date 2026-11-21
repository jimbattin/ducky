import type { JSX } from "preact";

interface Box {
  label: string;
  sub?: string;
  jack?: boolean;
}

const IN: Box = { label: "Input tip", sub: "guitar", jack: true };
const OUT: Box = { label: "Output tip", sub: "amp", jack: true };
const COMP: Box = { label: "Compressor" };
const TRIM: Box = { label: "Knob 1", sub: "trim" };
const MODEL: Box = { label: "Model" };
const IR: Box = { label: "IR" };
const EQ: Box = { label: "EQ" };
const RETURN: Box = { label: "Return level" };

/** The main lane per loop_mode index (MISO, pre-model, post-model), left to right. */
const LANES: readonly (readonly Box[])[] = [
  [IN, COMP, TRIM, MODEL, IR, EQ, { label: "Knob 2", sub: "volume × gate" }, OUT],
  [IN, COMP, { label: "Send level" }, RETURN, TRIM, MODEL, IR, EQ, { label: "Knob 2", sub: "volume × gate" }, OUT],
  [
    IN,
    COMP,
    TRIM,
    MODEL,
    { label: "Send level", sub: "× gate" },
    RETURN,
    IR,
    EQ,
    { label: "Knob 2", sub: "volume" },
    OUT,
  ],
];
const RING: Box = { label: "Output ring", sub: "same signal", jack: true };
const PEDALS: Box = { label: "Your loop pedals", sub: "send: output ring → return: input ring" };

// In viewBox units: the main lane across X0..X1 at MAIN_Y, the lower lane at LOW_Y.
const VIEW_W = 960;
const VIEW_H = 190;
const X0 = 8;
const X1 = 952;
const GAP = 16;
const MAIN_Y = 24;
const LOW_Y = 116;
const BOX_H = 48;

const words = (b: Box): string => (b.sub ? `${b.label} (${b.sub})` : b.label);

function BoxShape({ b, x, y, w, kind }: { b: Box; x: number; y: number; w: number; kind?: string }) {
  const cx = x + w / 2;
  return (
    <g class={kind ?? (b.jack ? "jack" : undefined)}>
      <rect class="loop-box" x={x} y={y} width={w} height={BOX_H} rx={3} />
      <text class="loop-label" x={cx} y={b.sub ? y + 21 : y + 29} text-anchor="middle">
        {b.label}
      </text>
      {b.sub && (
        <text class="loop-sub" x={cx} y={y + 37} text-anchor="middle">
          {b.sub}
        </text>
      )}
    </g>
  );
}

/** The signal flow of loop_mode `mode` (0 MISO, 1 pre-model, 2 post-model): which jack carries what, in order. */
export function LoopDiagram({ mode }: { mode: number }) {
  const lane = LANES[mode] ?? LANES[0]!;
  const n = lane.length;
  const w = (X1 - X0 - GAP * (n - 1)) / n;
  const xOf = (i: number) => X0 + i * (w + GAP);
  const send = lane.findIndex((b) => b.label === "Send level");
  const midY = MAIN_Y + BOX_H / 2;
  const ret = send + 1;

  const arrows = lane
    .slice(0, -1)
    .flatMap((b, i) =>
      i === send ? [] : [<line key={b.label} class="loop-arrow" x1={xOf(i) + w} y1={midY} x2={xOf(i + 1)} y2={midY} />],
    );
  const main = lane.map(words);
  const reach = (w + GAP) / 2; // the loop box overhangs the send and return boxes by this, for its sub-label
  let label: string;
  let lower: JSX.Element;
  if (send < 0) {
    const knob2 = n - 2;
    const ring = n - 1;
    const kx = xOf(knob2) + w / 2;
    label = `Signal flow: ${main.join(" → ")}; Knob 2 also feeds ${words(RING)}.`;
    lower = (
      <>
        <path class="loop-arrow" d={`M ${kx} ${MAIN_Y + BOX_H} V ${LOW_Y + BOX_H / 2} H ${xOf(ring)}`} fill="none" />
        <BoxShape b={RING} x={xOf(ring)} y={LOW_Y} w={w} />
      </>
    );
  } else {
    const sx = xOf(send) + w / 2;
    const rx = xOf(ret) + w / 2;
    label = `Signal flow: ${main.slice(0, ret).join(" → ")} → ${words(PEDALS)} → ${main.slice(ret).join(" → ")}.`;
    lower = (
      <>
        <line class="loop-arrow" x1={sx} y1={MAIN_Y + BOX_H} x2={sx} y2={LOW_Y} />
        <BoxShape b={PEDALS} x={xOf(send) - reach} y={LOW_Y} w={xOf(ret) + w - xOf(send) + 2 * reach} kind="loop" />
        <line class="loop-arrow" x1={rx} y1={LOW_Y} x2={rx} y2={MAIN_Y + BOX_H} />
      </>
    );
  }

  return (
    <svg viewBox={`0 0 ${VIEW_W} ${VIEW_H}`} role="img" aria-label={label}>
      <defs>
        <marker
          id="loop-head"
          viewBox="0 0 8 8"
          refX={8}
          refY={4}
          markerWidth={8}
          markerHeight={8}
          markerUnits="userSpaceOnUse"
          orient="auto"
        >
          <path class="loop-head" d="M 0 0 L 8 4 L 0 8 z" />
        </marker>
      </defs>
      {lane.map((b, i) => (
        <BoxShape key={b.label} b={b} x={xOf(i)} y={MAIN_Y} w={w} />
      ))}
      {arrows}
      {lower}
    </svg>
  );
}
