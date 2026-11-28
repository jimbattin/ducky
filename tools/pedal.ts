// Manage the pedal over its USB MIDI link (docs/MIDI.md) while it plays: list, upload, erase, rename and swap preset
// slots, set a preset's noise gate and morph target, list, set, assign and save the parameters, stream its status,
// and update its firmware. Slots are 1-8.
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
//   morph N M|none [--train | --path FILE]   make slot N's preset morph with slot M's (the `morph` parameter blends
//                                            N's model into M's): reads both models, fits M's to N's and stores the
//                                            fit in slot N (a straight blend); none removes it. --train reads both
//                                            models, trains a path of waypoints between them (tools/morph_train.py
//                                            under uv, about ten minutes) and stores it in slot N by uploading its
//                                            preset again, unless the straight blend fits at least as well, which
//                                            is then stored instead; --path FILE stores a path file the trainer
//                                            wrote. Run it again after uploading over M
//   info N                                   the preset in slot N: its name, model metadata, IR, gate and morph
//                                            target, with a trained path's waypoint count (read back)
//   param list | param get NAME | param set NAME VALUE | param assign CONTROL NAME|none | param channel omni|1-16 |
//   param midi-in omni|1-16 | param save
//                                            the parameters (docs/MIDI.md "Parameters"): each one's value and
//                                            controls; VALUE in the parameter's unit, a choice's label or index, or
//                                            `off`; CONTROL k3-k6, sw1-sw3, exp or pedal (a BurningBungalow's);
//                                            channel sets the MIDI channel exp's CC 11 and the preset CC 102 are
//                                            taken from on USB; midi-in the channel a BurningBungalow's MIDI IN takes
//                                            CC 11 and CC 102 on; save keeps the layout, the channels and the values
//                                            for the next power-up
//   status [SECONDS]                         STATUS and LOG lines for SECONDS (default 5) or until Ctrl-C
//   firmware FILE.bin                        write a firmware image (daisy/build/<board>-*/ducky_*.bin) and restart
//                                            into it; waits for the pedal to come back

import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { firmwareKind, firmwareProblem } from "../editor/src/link/firmware";
import {
  accepts,
  CONTROLS,
  channelName,
  formatParam,
  isBurningBungalow,
  parseParamValue,
  shownParam,
} from "../editor/src/link/params";
import {
  MAX_CHANNEL,
  OMNI,
  type ParamInfo,
  SLOT_STATUS,
  SLOT_STATUS_NAMES,
  type StatusMessage,
} from "../editor/src/link/protocol";
import { LinkError, PedalSession } from "../editor/src/link/session";
import { fitAlignment } from "../editor/src/morph/align";
import { saveMorph, saveMorphPath } from "../editor/src/morph/save";
import { modelWeights } from "../editor/src/nam/convert";
import { type GateSettings, gateProblem } from "../editor/src/preset/gate";
import { decodePathFile, encodeMorphBlock, type MorphPath, resolveMorph } from "../editor/src/preset/morph";
import { findSeeds, openRawMidi, waitForDucky } from "./rawmidi";
import { loadSlotSource, SlotSourceError } from "./slot_source";

const USAGE = `usage: bun tools/pedal.ts [--port BUS-PORT] COMMAND
  list
  upload N MODEL [--ir IR] [--name NAME]
  erase N
  info N
  rename N NAME
  swap A B
  gate N on|off [THRESHOLD_DB ATTACK_MS HOLD_MS RELEASE_MS RATIO]
  morph N M|none [--train | --path FILE]
  param list | param get NAME | param set NAME VALUE | param assign CONTROL NAME|none | param channel omni|1-16
  param midi-in omni|1-16 | param save
  status [SECONDS]
  firmware FILE.bin`;

function usage(problem: string): never {
  console.error(`pedal: ${problem}`);
  console.error(USAGE);
  process.exit(2);
}

function fail(message: string): never {
  console.error(`pedal: ${message}`);
  process.exit(1);
}

// Arguments: --port anywhere; upload's --ir and --name and morph's --train and --path anywhere after the command.
const args = process.argv.slice(2);
let port = process.env.DK_USB_PORT ?? null;
let irArg: string | null = null;
let nameArg: string | null = null;
let train = false;
let pathArg: string | null = null;
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
  else if (a === "--train") train = true;
  else if (a === "--path") pathArg = value("FILE");
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
  firmware: [1, 1],
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
if ((train || pathArg !== null) && morphTarget === "none") usage("--train and --path are for morph N M");
if (train && pathArg !== null) usage("morph: give --train or --path, not both");

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

// param's subcommand and, for assign, its control (0-8); for channel and midi-in, the channel (OMNI or 1-16).
let paramControl = -1;
let paramChannel = -1;
if (command === "param") {
  const arity: Record<string, number> = { list: 1, get: 2, set: 3, assign: 3, channel: 2, "midi-in": 2, save: 1 };
  if (arity[rest[0]!] !== rest.length)
    usage(
      "param: list, get NAME, set NAME VALUE, assign CONTROL NAME|none, channel omni|1-16, midi-in omni|1-16 or save",
    );
  if (rest[0] === "assign") {
    paramControl = CONTROLS.findIndex((c) => c.key === rest[1]!.toLowerCase());
    if (paramControl < 0) usage("param: CONTROL is k3-k6, sw1-sw3, exp or pedal");
  }
  if (rest[0] === "channel" || rest[0] === "midi-in") {
    const text = rest[1]!.toLowerCase();
    if (text === "omni") paramChannel = OMNI;
    else if (/^\d+$/.test(text) && Number(text) >= 1 && Number(text) <= MAX_CHANNEL) paramChannel = Number(text);
    else usage(`param: ${rest[0]} is omni or 1-16`);
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
// --path's path file, before touching the pedal.
const readPath = async (file: string): Promise<MorphPath> =>
  decodePathFile(new Uint8Array(await Bun.file(file).arrayBuffer()));
const givenPath =
  pathArg === null ? null : await readPath(pathArg).catch((e: unknown) => fail(`${pathArg}: ${(e as Error).message}`));
if (source?.warning) console.error(`pedal: slot ${slots[0]! + 1}: warning: ${source.warning}`);
// firmware's image, before touching the pedal.
let firmware: Uint8Array | null = null;
if (command === "firmware") {
  const file = rest[0]!;
  const bytes = await Bun.file(file)
    .arrayBuffer()
    .catch((e: unknown) => fail(`${file}: ${(e as Error).message}`));
  firmware = new Uint8Array(bytes);
  const problem = firmwareProblem(firmware);
  if (problem !== null) fail(`${file}: ${problem}`);
}

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

/**
 * Trains a path from model binary a to model binary b: tools/morph_train.py under uv, from the repo root, on the two
 * binaries in a temporary directory, its progress on this process's stdout and stderr.
 */
async function trainPath(a: Uint8Array, b: Uint8Array): Promise<MorphPath> {
  const dir = await mkdtemp(join(tmpdir(), "ducky-morph-"));
  try {
    const [fileA, fileB, out] = ["A.bin", "B.bin", "OUT.path"].map((f) => join(dir, f)) as [string, string, string];
    await Bun.write(fileA, a);
    await Bun.write(fileB, b);
    const trainer = Bun.spawn(["uv", "run", "tools/morph_train.py", fileA, fileB, "-o", out], {
      cwd: join(import.meta.dir, ".."),
      stdio: ["inherit", "inherit", "inherit"],
    });
    const code = await trainer.exited;
    if (code !== 0) throw new Error(`tools/morph_train.py failed (exit ${code})`);
    return await readPath(out);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

// After a firmware update: its results and when it started, for the line printed once the pedal is back.
let restartedAfter: { flashMs: number; retries: number; t0: number } | null = null;
const midi = await openRawMidi(seed.dev).catch((e: unknown) => fail((e as Error).message));
try {
  const session = new PedalSession(midi, command === "status" ? { onLog: (text) => console.log(text) } : {});
  const identity = await session.identity().catch(() => fail(`no Ducky on ${seed.usbPath}`));
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
          : `morph slot ${target + 1} fit ${percent(morph.fit)} % unaligned ${percent(morph.unalignedFit)} %` +
              (morph.waypoints > 0 ? ` waypoints ${morph.waypoints}` : ""),
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
      if (morphTarget !== "none" && (train || givenPath !== null)) {
        const what = `morph slot ${k + 1} -> ${morphTarget + 1}`;
        if (identity.morph !== "weight")
          console.error(
            "pedal: this firmware crossfades the two models; the path is stored for the weight morph image " +
              "(make -C daisy APP=weight)",
          );
        let path = givenPath;
        if (path === null) {
          const binA = await session.readModel(k);
          const binB = await session.readModel(morphTarget);
          path = await trainPath(binA, binB).catch((e: unknown) => fail((e as Error).message));
          // Two captures that are one network in other channel coordinates: the aligned straight blend is the morph.
          const b = modelWeights(binB);
          const straight = fitAlignment(modelWeights(binA), b);
          if (straight.fit >= path.fit) {
            const flashMs = await session.morph(
              k,
              encodeMorphBlock({ ...straight, target: morphTarget, modelCrc: b.crc }),
            );
            console.log(
              `${what}: straight blend kept, fit ${percent(straight.fit)} % (unaligned ` +
                `${percent(straight.unalignedFit)} %, trained path ${percent(path.fit)} %), flash ${flashMs} ms`,
            );
            break;
          }
        }
        const t0 = performance.now();
        const r = await saveMorphPath(session, k, morphTarget, path).catch((e: unknown) => fail((e as Error).message));
        const secs = ((performance.now() - t0) / 1000).toFixed(1);
        console.log(
          `${what}: trained path, ${r.waypoints} waypoint${r.waypoints === 1 ? "" : "s"}, fit ${percent(r.fit)} % ` +
            `(straight ${percent(r.unalignedFit)} %), flash ${r.flashMs} ms, ${secs} s`,
        );
        break;
      }
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
      if (rest[0] === "channel") {
        await session.expChannel(paramChannel);
        console.log(`exp channel = ${channelName(paramChannel)}: ok`);
        break;
      }
      // MIDI IN and PEDAL are a BurningBungalow's: a HotHouse answers bad-request.
      const bungalowOnly =
        rest[0] === "midi-in"
          ? "midi-in"
          : rest[0] === "assign" && CONTROLS[paramControl]!.kind === "pedal"
            ? CONTROLS[paramControl]!.id
            : null;
      if (bungalowOnly !== null && !isBurningBungalow(identity.board))
        fail(`param: ${bungalowOnly} is a BurningBungalow's; this pedal is board ${identity.board}`);
      if (rest[0] === "midi-in") {
        await session.midiInChannel(paramChannel);
        console.log(`midi in channel = ${channelName(paramChannel)}: ok`);
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
        const value = formatParam(shownParam(d.params, d.values.codes, p), d.values.codes[p.id]!);
        return `${p.name.padEnd(16)}${value.padEnd(14)}${on.join(" ") || "-"}`;
      };
      if (rest[0] === "list") {
        for (const p of d.params) console.log(line(p));
        console.log(`exp channel ${channelName(d.values.expChannel)}`);
        if (isBurningBungalow(identity.board)) console.log(`midi in channel ${channelName(d.values.midiInChannel)}`);
      } else if (rest[0] === "get") {
        console.log(line(named(rest[1]!)));
      } else if (rest[0] === "set") {
        const p = named(rest[1]!);
        const shown = shownParam(d.params, d.values.codes, p);
        const code = parseParamValue(shown, rest[2]!);
        if (code === null) fail(`param: ${rest[2]} is not a value of ${p.name}`);
        console.log(`${p.name} = ${formatParam(shown, await session.set(p.id, code))}`);
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
    case "firmware": {
      const image = firmware!;
      console.log(
        `firmware ${rest[0]}: ${image.length} bytes, ${firmwareKind(image)} image; pedal build ${identity.build} ` +
          `${identity.morph}`,
      );
      const t0 = performance.now();
      const r = await session.updateFirmware(image);
      restartedAfter = { ...r, t0 };
      break;
    }
  }
} catch (e) {
  if (e instanceof LinkError) fail(e.message);
  throw e;
} finally {
  midi.close();
}

if (restartedAfter !== null) {
  const id = await waitForDucky(seed.usbPath, 30000);
  if (id === null) fail(`firmware written, but no Ducky came back on ${seed.usbPath} within 30 s`);
  const secs = ((performance.now() - restartedAfter.t0) / 1000).toFixed(1);
  console.log(
    `firmware: ok, flash ${restartedAfter.flashMs} ms, ${restartedAfter.retries} retries, ${secs} s; ` +
      `back as build ${id.build} ${id.morph}`,
  );
}
