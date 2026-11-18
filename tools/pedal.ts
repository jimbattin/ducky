// Manage the pedal over its USB MIDI link (docs/MIDI.md) while it plays: list, upload, erase, rename and swap preset
// slots, set a preset's noise gate and morph target, list, set, assign and save the parameters, and stream its
// status. Slots are 1-8.
// MODEL and IR are read as tools/slot_source.ts does (.nam and .wav converted, anything else read as a binary); the
// name defaults to the model's. The Seed is found by its USB path (--port, else DK_USB_PORT, else the only Seed
// attached); its Identity must report a Ducky (device 64).
// Linux (ALSA rawmidi, tools/rawmidi.ts); the MIDI port opens exclusively, so close other MIDI programs first.
//
// Usage: bun tools/pedal.ts [--port BUS-PORT] COMMAND
//   list                                     the 8 slots (* marks the playing one)
//   upload N MODEL [--ir IR] [--name NAME]   write a preset to slot N
//   erase N | rename N NAME | swap A B
//   gate N on|off [THRESHOLD_DB ATTACK_MS HOLD_MS RELEASE_MS RATIO]
//                                            the preset's noise gate: on/off alone keeps the stored settings, e.g.
//                                            `gate 1 on -55 0.5 20 100 4` (threshold in dBFS, ratio N for N:1)
//   morph N M|none                           make slot N's preset morph with slot M's (the `morph` parameter blends
//                                            N's model into M's): reads both models, fits M's to N's and stores the
//                                            fit in slot N; none removes it. Run it again after uploading over M
//   info N                                   the preset in slot N: its name, model metadata, IR, gate and morph
//                                            target (read back)
//   param list | param get NAME | param set NAME VALUE | param assign CONTROL NAME|none | param save
//                                            the parameters (docs/MIDI.md "Parameters"): each one's value and
//                                            controls; VALUE in the parameter's unit, a choice's label or index, or
//                                            `off`; CONTROL k3-k6 or sw1-sw3; save keeps the layout and the values
//                                            for the next power-up
//   status [SECONDS]                         STATUS and LOG lines for SECONDS (default 5) or until Ctrl-C

import { accepts, CONTROLS, formatParam, parseParamValue } from "../editor/src/link/params";
import { type ParamInfo, SLOT_STATUS, SLOT_STATUS_NAMES, type StatusMessage } from "../editor/src/link/protocol";
import { LinkError, PedalSession } from "../editor/src/link/session";
import { saveMorph } from "../editor/src/morph/save";
import { type GateSettings, gateProblem } from "../editor/src/preset/gate";
import { resolveMorph } from "../editor/src/preset/morph";
import { findSeeds, openRawMidi } from "./rawmidi";
import { loadSlotSource, SlotSourceError } from "./slot_source";

const USAGE = `usage: bun tools/pedal.ts [--port BUS-PORT] COMMAND
  list
  upload N MODEL [--ir IR] [--name NAME]
  erase N
  info N
  rename N NAME
  swap A B
  gate N on|off [THRESHOLD_DB ATTACK_MS HOLD_MS RELEASE_MS RATIO]
  morph N M|none
  param list | param get NAME | param set NAME VALUE | param assign CONTROL NAME|none | param save
  status [SECONDS]`;

function usage(problem: string): never {
  console.error(`pedal: ${problem}`);
  console.error(USAGE);
  process.exit(2);
}

function fail(message: string): never {
  console.error(`pedal: ${message}`);
  process.exit(1);
}

// Arguments: --port anywhere; upload's --ir and --name anywhere after the command.
const args = process.argv.slice(2);
let port = process.env.DK_USB_PORT ?? null;
let irArg: string | null = null;
let nameArg: string | null = null;
const words: string[] = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i]!;
  const value = (what: string): string => {
    const v = args[++i];
    if (v === undefined) usage(`${a} needs ${what}`);
    return v;
  };
  if (a === "--port") port = value("BUS-PORT");
  else if (a === "--ir") irArg = value("IR");
  else if (a === "--name") nameArg = value("NAME");
  else if (a.startsWith("--")) usage(`unknown option ${a}`);
  else words.push(a);
}
const [command, ...rest] = words;
if (command === undefined) usage("no command");
if ((irArg !== null || nameArg !== null) && command !== "upload") usage("--ir and --name are for upload");

const arity: Record<string, [number, number]> = {
  list: [0, 0],
  upload: [2, 2],
  erase: [1, 1],
  info: [1, 1],
  rename: [2, 2],
  swap: [2, 2],
  gate: [2, 7],
  morph: [2, 2],
  param: [1, 3],
  status: [0, 1],
};
const counts = arity[command];
if (counts === undefined) usage(`unknown command ${command}`);
if (rest.length < counts[0] || rest.length > counts[1]) usage(`${command}: wrong number of arguments`);

/** A 1-based slot argument as 0-based. */
function slotArg(text: string): number {
  const n = Number(text);
  if (!Number.isInteger(n) || n < 1 || n > 8) usage(`slot ${text}: must be 1..8`);
  return n - 1;
}

let seconds = 5;
if (command === "status" && rest[0] !== undefined) {
  seconds = Number(rest[0]);
  if (!Number.isFinite(seconds) || seconds <= 0) usage(`status: ${rest[0]}: must be a positive number of seconds`);
}
const slots =
  command === "upload" ||
  command === "erase" ||
  command === "rename" ||
  command === "info" ||
  command === "gate" ||
  command === "morph"
    ? [slotArg(rest[0]!)]
    : [];
if (command === "swap") slots.push(slotArg(rest[0]!), slotArg(rest[1]!));

// morph's target slot (0-based), or "none".
const morphTarget = command === "morph" && rest[1] !== "none" ? slotArg(rest[1]!) : "none";
if (morphTarget === slots[0]) usage("morph: a preset cannot morph with itself");

// gate's on|off and, with all 7 words, the whole gate (null: keep the stored settings).
let gateOn = false;
let gateArg: GateSettings | null = null;
if (command === "gate") {
  if (rest.length !== 2 && rest.length !== 7) usage("gate: give on|off alone or with all five settings");
  if (rest[1] !== "on" && rest[1] !== "off") usage(`gate: ${rest[1]}: must be on or off`);
  gateOn = rest[1] === "on";
  if (rest.length === 7) {
    const [thresholdDb, attackMs, holdMs, releaseMs, ratio] = rest.slice(2).map(Number) as [
      number,
      number,
      number,
      number,
      number,
    ];
    gateArg = { enabled: gateOn, thresholdDb, attackMs, holdMs, releaseMs, ratio };
    const problem = gateProblem(gateArg);
    if (problem !== null) usage(`gate: ${problem}`);
  }
}

// param's subcommand and, for assign, its control (0-6).
let paramControl = -1;
if (command === "param") {
  const arity: Record<string, number> = { list: 1, get: 2, set: 3, assign: 3, save: 1 };
  if (arity[rest[0]!] !== rest.length) usage("param: list, get NAME, set NAME VALUE, assign CONTROL NAME|none or save");
  if (rest[0] === "assign") {
    paramControl = CONTROLS.findIndex((c) => c.key === rest[1]!.toLowerCase());
    if (paramControl < 0) usage("param: CONTROL is k3-k6 or sw1-sw3");
  }
}

// The upload's image, before touching the pedal.
const source =
  command === "upload"
    ? await loadSlotSource(rest[1]!, irArg, nameArg).catch((e: unknown) => {
        if (e instanceof SlotSourceError) fail(`${e.path ?? `slot ${slots[0]! + 1}`}: ${e.message}`);
        throw e;
      })
    : null;
if (source?.warning) console.error(`pedal: slot ${slots[0]! + 1}: warning: ${source.warning}`);

const seeds = findSeeds();
if (port === null && seeds.length > 1)
  usage(`${seeds.length} Seeds attached (${seeds.map((s) => s.usbPath).join(", ")}); pass --port`);
const seed = port === null ? seeds[0] : seeds.find((s) => s.usbPath === port);
if (seed === undefined) fail(`no Seed MIDI port${port === null ? "" : ` at ${port}`} (is tools/usb_log.py running?)`);

const nameOf = (image: Uint8Array): string => {
  let s = "";
  for (let i = 12; i < 76 && image[i] !== 0; i++) s += String.fromCharCode(image[i]!);
  return s;
};

const pct = (permille: number) => (permille / 10).toFixed(1);
const statusLine = (s: StatusMessage): string =>
  `status load ${s.loadAvg}/${s.loadMax} permille underruns ${s.underruns} slot ${s.slot === null ? "dry" : s.slot + 1} ` +
  `mute ${s.muted ? "on" : "off"} k ${s.knobs.map(pct).join(" ")} sw ${s.toggles.join(" ")} ` +
  `fs ${s.fs1 ? 1 : 0} ${s.fs2 ? 1 : 0} gate in ${s.gateLevelDb.toFixed(1)} dBFS cut ${s.gateCutDb.toFixed(1)} dB` +
  ` comp cut ${s.compCutDb.toFixed(1)} dB`;
const signed = (v: number | null, unit: string): string =>
  v === null ? "none" : `${v >= 0 ? "+" : "-"}${Math.abs(v).toFixed(1)} ${unit}`;
const quoted = (s: string | null): string => (s === null ? "none" : `"${s}"`);
/** A morph fit (0..1) in percent. */
const percent = (fit: number): string => (100 * fit).toFixed(1);

const midi = await openRawMidi(seed.dev).catch((e: unknown) => fail((e as Error).message));
try {
  const session = new PedalSession(midi, command === "status" ? { onLog: (text) => console.log(text) } : {});
  await session.identity().catch(() => fail(`no Ducky on ${seed.usbPath}`));
  switch (command) {
    case "list":
      for (const s of await session.list()) {
        const status = SLOT_STATUS_NAMES[s.status] ?? String(s.status);
        const detail =
          s.status === SLOT_STATUS.ok ? ` "${s.name}" model "${s.model}" ir ${s.hasIr ? `"${s.ir}"` : "none"}` : "";
        console.log(`${s.slot + 1} ${status}${detail}${s.playing ? " *" : ""}`);
      }
      break;
    case "upload": {
      const image = source!.image;
      const t0 = performance.now();
      const r = await session.upload(slots[0]!, image);
      const secs = ((performance.now() - t0) / 1000).toFixed(1);
      console.log(
        `upload slot ${slots[0]! + 1} "${nameOf(image)}" ${image.length} bytes: ok, flash ${r.flashMs} ms, ` +
          `${r.retries} retries, ${secs} s`,
      );
      break;
    }
    case "erase":
      console.log(`erase slot ${slots[0]! + 1}: ok, flash ${await session.erase(slots[0]!)} ms`);
      break;
    case "rename":
      console.log(
        `rename slot ${slots[0]! + 1} "${rest[1]}": ok, flash ${await session.rename(slots[0]!, rest[1]!)} ms`,
      );
      break;
    case "info": {
      const k = slots[0]!;
      const list = await session.list();
      const status = list[k]!.status;
      if (status !== SLOT_STATUS.ok) fail(`slot ${k + 1}: ${SLOT_STATUS_NAMES[status] ?? status}`);
      const info = await session.readInfo(k);
      const { name, model: m, ir, gate: g, morph } = info;
      console.log(`slot ${k + 1} "${name}"`);
      console.log(
        `model "${m.name}" gear type ${m.gearType ?? "none"} make ${quoted(m.gearMake)} model ${quoted(m.gearModel)} ` +
          `modeled by ${quoted(m.modeledBy)}`,
      );
      console.log(`model loudness ${signed(m.loudness, "dB")} input ${signed(m.inputLevelDbu, "dBu")}`);
      console.log(
        ir === null
          ? "ir none"
          : `ir "${ir.name}" taps ${ir.taps} src ${ir.sourceRate} Hz ${ir.sourceChannels} ch ${ir.sourceFrames} ` +
              `frames gain ${signed(ir.gainDb, "dB")}`,
      );
      console.log(
        `gate ${g.enabled ? "on" : "off"} thr ${g.thresholdDb} dBFS attack ${g.attackMs.toFixed(1)} ms hold ` +
          `${g.holdMs} ms release ${g.releaseMs} ms ratio ${g.ratio.toFixed(1)}`,
      );
      if (morph === null) {
        console.log("morph none");
        break;
      }
      // The target is the slot that holds the model the block was fitted to, as the pedal resolves it.
      const infos = [];
      for (const s of list)
        infos.push(s.slot === k ? info : s.status === SLOT_STATUS.ok ? await session.readInfo(s.slot) : null);
      const target = resolveMorph(infos, k);
      console.log(
        target === null
          ? `morph missing slot ${morph.target + 1}`
          : `morph slot ${target + 1} fit ${percent(morph.fit)} % unaligned ${percent(morph.unalignedFit)} %`,
      );
      break;
    }
    case "gate": {
      const k = slots[0]!;
      const g = gateArg ?? { ...(await session.readInfo(k)).gate, enabled: gateOn };
      console.log(`gate slot ${k + 1} ${g.enabled ? "on" : "off"}: ok, flash ${await session.gate(k, g)} ms`);
      break;
    }
    case "morph": {
      const k = slots[0]!;
      const r = await saveMorph(session, k, morphTarget);
      console.log(
        r.target === null
          ? `morph slot ${k + 1} off: flash ${r.flashMs} ms`
          : `morph slot ${k + 1} -> ${r.target + 1}: fit ${percent(r.fit!)} % (unaligned ` +
              `${percent(r.unalignedFit!)} %), flash ${r.flashMs} ms`,
      );
      break;
    }
    case "swap":
      console.log(
        `swap slots ${slots[0]! + 1} ${slots[1]! + 1}: ok, flash ${await session.swap(slots[0]!, slots[1]!)} ms`,
      );
      break;
    case "param": {
      if (rest[0] === "save") {
        console.log(`settings saved: ok, flash ${await session.save()} ms`);
        break;
      }
      const d = await session.dump();
      const named = (name: string): ParamInfo =>
        d.params.find((p) => p.name === name) ?? fail(`param: no parameter ${name}`);
      // NAME, its value, then the controls it is on, `(held)` on one waiting to move.
      const line = (p: ParamInfo): string => {
        const on = d.values.assign.flatMap((id, c) =>
          id === p.id ? [`${CONTROLS[c]!.id}${d.values.held[c] ? " (held)" : ""}`] : [],
        );
        return `${p.name.padEnd(16)}${formatParam(p, d.values.codes[p.id]!).padEnd(14)}${on.join(" ") || "-"}`;
      };
      if (rest[0] === "list") {
        for (const p of d.params) console.log(line(p));
      } else if (rest[0] === "get") {
        console.log(line(named(rest[1]!)));
      } else if (rest[0] === "set") {
        const p = named(rest[1]!);
        const code = parseParamValue(p, rest[2]!);
        if (code === null) fail(`param: ${rest[2]} is not a value of ${p.name}`);
        console.log(`${p.name} = ${formatParam(p, await session.set(p.id, code))}`);
      } else {
        const p = rest[2] === "none" ? null : named(rest[2]!);
        const control = CONTROLS[paramControl]!.id;
        if (!accepts(paramControl, p)) fail(`param: ${control} takes only choice parameters`);
        await session.assign(paramControl, p?.id ?? null);
        console.log(`${control} = ${p?.name ?? "none"}: ok`);
      }
      break;
    }
    case "status": {
      let stop = false;
      process.on("SIGINT", () => {
        stop = true;
      });
      const deadline = Date.now() + seconds * 1000;
      await session.watch(true);
      try {
        while (!stop && Date.now() < deadline) {
          const m = await session.next(Math.min(100, deadline - Date.now()));
          if (m?.type === "status") console.log(statusLine(m));
          else if (m?.type === "log") console.log(m.text);
        }
      } finally {
        await session.watch(false);
      }
      break;
    }
  }
} catch (e) {
  if (e instanceof LinkError) fail(e.message);
  throw e;
} finally {
  midi.close();
}
