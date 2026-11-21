// The firmware update over USB-MIDI, checked end to end on the attached pedal (docs/MIDI.md "Firmware update"),
// unattended: `make firmware-check`. It builds the xfade, weight and bench images and the editor, puts the working
// tree's xfade image on the pedal with tools/flash_when_ready.sh (the attached pedal may run a build without the
// update), then:
// - tools/pedal.ts firmware, weight then xfade, --cycles times; the presets, the playing slot and the state survive;
// - the host refuses the bench image (no tag);
// - the pedal refuses an incomplete image, a wrong CRC, a missing tag and another board, and keeps running;
// - in Chrome (CDP, tools/cdp.ts) the editor uploads a .nam + .wav preset to the highest empty slot, then updates the
//   firmware to weight and back to xfade.
// Then the scratch slot is erased and `pedal.ts list` must read as before. The pedal ends on the xfade image.
// Needs the pedal on --port or DK_USB_PORT running Ducky, its MIDI port free (close editor tabs), Chrome (CHROME
// overrides /usr/bin/google-chrome-stable) and an empty slot. Each step prints one line; a failure prints
// `firmware check: FAILED: <step>: <why>` and exits 1.
//
// Usage: bun tools/firmware_check.ts [--port BUS-PORT] [--headful] [--cycles N]

import { resolve } from "node:path";
import { $ } from "bun";
import { crc32 } from "../editor/src/crc32";
import { firmwareKind, firmwareProblem } from "../editor/src/link/firmware";
import {
  CHUNK_BYTES,
  CMD,
  CMD_NAMES,
  fwBeginMsg,
  fwChunkMsg,
  fwCommitMsg,
  parseMessage,
  RESULT,
  RESULT_NAMES,
  type ResultMessage,
} from "../editor/src/link/protocol";
import { PedalSession } from "../editor/src/link/session";
import { launchChrome } from "./cdp";
import { findSeeds, openRawMidi, type RawMidi, waitForDucky } from "./rawmidi";

const ROOT = resolve(import.meta.dir, "..");
const X = "daisy/build/hothouse-xfade/ducky_xfade.bin";
const W = "daisy/build/hothouse-weight/ducky_weight.bin";
const B = "daisy/build/hothouse-bench/ducky_bench.bin";

function usage(problem: string): never {
  console.error(`firmware check: ${problem}`);
  console.error("usage: bun tools/firmware_check.ts [--port BUS-PORT] [--headful] [--cycles N]");
  process.exit(2);
}

let port = process.env.DK_USB_PORT ?? null;
let headful = false;
let cycles = 2;
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const a = args[i]!;
  if (a === "--port") port = args[++i] ?? usage("--port needs BUS-PORT");
  else if (a === "--headful") headful = true;
  else if (a === "--cycles") {
    cycles = Number(args[++i]);
    if (!Number.isInteger(cycles) || cycles < 1) usage("--cycles needs a positive integer");
  } else usage(`unknown argument ${a}`);
}
if (port === null) usage("needs --port BUS-PORT or DK_USB_PORT (the pedal's USB path, e.g. 3-4)");
const usbPath: string = port;

$.cwd(ROOT);
$.env({ ...process.env, DK_USB_PORT: usbPath });
const t0 = performance.now();
const secs = (since: number) => ((performance.now() - since) / 1000).toFixed(1);
const say = (line: string) => console.log(`firmware check: ${line}`);

class CheckError extends Error {}

/** Runs one step; its failure ends the check. */
async function step<T>(name: string, fn: () => Promise<T>): Promise<T> {
  try {
    return await fn();
  } catch (e) {
    console.error(`firmware check: FAILED: ${name}: ${(e as Error).message}`);
    process.exit(1);
  }
}

async function pedal(...words: string[]): Promise<{ code: number; stdout: string; stderr: string }> {
  const r = await $`bun tools/pedal.ts ${words}`.nothrow().quiet();
  return { code: r.exitCode, stdout: r.stdout.toString(), stderr: r.stderr.toString() };
}

async function list(): Promise<string> {
  const r = await pedal("list");
  if (r.code !== 0) throw new CheckError(`pedal.ts list: ${r.stderr.trim()}`);
  return r.stdout;
}

function sameList(now: string, before: string): void {
  if (now !== before) throw new CheckError(`the slot list changed:\n--- before\n${before}--- now\n${now}`);
}

/** The pedal gave no Ducky back after a self-written image: reflash it when it sits in the bootloader's DFU mode. */
async function recover(): Promise<never> {
  const dfu = (await $`dfu-util -l`.nothrow().quiet()).stdout.toString();
  if (dfu.split("\n").some((l) => l.includes("0483:df11") && l.includes(`path="${usbPath}"`))) {
    await $`tools/flash_when_ready.sh ${X}`.nothrow().quiet();
    throw new CheckError("pedal stayed in the bootloader after a self-written image; recovered with dfu-util");
  }
  throw new CheckError(
    `pedal gone: tap RESET, then press BOOT while the Seed's LED pulses, then DK_USB_PORT=${usbPath} tools/flash_when_ready.sh ${X}`,
  );
}

/** After a failed firmware step: the failure itself when a Ducky still answers, else recover(). */
async function failedUpdate(e: unknown): Promise<never> {
  if ((await waitForDucky(usbPath, 30000)) === null) await recover();
  throw e;
}

/** Sends msg and reads until the RESULT for cmd. */
async function rawResult(midi: RawMidi, msg: Uint8Array, cmd: number, ms: number): Promise<ResultMessage> {
  await midi.send(msg);
  const deadline = Date.now() + ms;
  for (;;) {
    const raw = await midi.receive(Math.max(0, deadline - Date.now()));
    if (raw === null) throw new CheckError(`${CMD_NAMES[cmd]}: no reply`);
    const m = parseMessage(raw);
    if (m?.type === "result" && m.cmd === cmd) return m;
  }
}

/** FIRMWARE BEGIN with crc, then chunks of image up to `upTo` bytes, then COMMIT's RESULT. */
async function rawUpdate(midi: RawMidi, image: Uint8Array, crc: number, upTo = image.length): Promise<ResultMessage> {
  const begin = await rawResult(midi, fwBeginMsg(image.length, crc), CMD.fwBegin, 1000);
  if (begin.code !== RESULT.ok) throw new CheckError(`FIRMWARE BEGIN: ${RESULT_NAMES[begin.code]}`);
  for (let off = 0; off < upTo; off += CHUNK_BYTES) {
    const r = await rawResult(midi, fwChunkMsg(off, image.subarray(off, off + CHUNK_BYTES)), CMD.fwChunk, 1000);
    if (r.code !== RESULT.ok) throw new CheckError(`FIRMWARE CHUNK at ${off}: ${RESULT_NAMES[r.code]}`);
  }
  return rawResult(midi, fwCommitMsg(), CMD.fwCommit, 30000);
}

const read = async (path: string) => new Uint8Array(await Bun.file(resolve(ROOT, path)).arrayBuffer());

// 1. Preflight.
const { before, scratch } = await step("preflight", async () => {
  const r = await pedal("list");
  if (r.code !== 0)
    throw new CheckError(`${r.stderr.trim()} (is the port busy? close the editor tab and tools/usb_log.py)`);
  const empty = [...r.stdout.matchAll(/^(\d) empty$/gm)].map((m) => Number(m[1]) - 1);
  if (empty.length === 0) throw new CheckError("needs an empty slot");
  return { before: r.stdout, scratch: Math.max(...empty) };
});
say(`pedal on ${usbPath}; scratch slot ${scratch + 1}`);

// 2. Build.
const images = await step("build", async () => {
  for (const cmd of [
    $`make -C daisy -j`,
    $`make -C daisy APP=weight -j`,
    $`make -C daisy APP=bench -j`,
    $`make -C editor html`,
  ]) {
    const r = await cmd.nothrow().quiet();
    if (r.exitCode !== 0) throw new CheckError(r.stderr.toString().trim().split("\n").slice(-10).join("\n"));
  }
  const x = await read(X);
  const w = await read(W);
  const b = await read(B);
  if (firmwareProblem(x) !== null) throw new CheckError(`${X}: ${firmwareProblem(x)}`);
  if (firmwareProblem(w) !== null) throw new CheckError(`${W}: ${firmwareProblem(w)}`);
  if (firmwareKind(x) !== "crossfade" || firmwareKind(w) !== "weight") throw new CheckError("the tags' morph flags");
  if (!firmwareProblem(b)?.includes("no DKFW tag")) throw new CheckError(`${B} is not refused for its missing tag`);
  return { x, w };
});
say(`built: xfade ${images.x.length} bytes, weight ${images.w.length} bytes, bench refused, editor`);

// 3. Bootstrap: the working tree's xfade image over DFU.
await step("bootstrap", async () => {
  const r = await $`tools/flash_when_ready.sh ${X}`.nothrow().quiet();
  if (r.exitCode !== 0) throw new CheckError(`flash_when_ready.sh: ${r.stderr.toString().trim()}`);
  const deadline = Date.now() + 30000;
  for (;;) {
    const l = await pedal("list");
    if (l.code === 0) return sameList(l.stdout, before);
    if (Date.now() > deadline) throw new CheckError("no Ducky within 30 s of the DFU flash");
    await Bun.sleep(500);
  }
});
say("bootstrap: xfade image flashed over DFU, slots unchanged");

// 4. CLI cycles.
for (let c = 1; c <= cycles; c++) {
  for (const [file, morph] of [
    [W, "weight"],
    [X, "crossfade"],
  ] as const) {
    const line = await step(`cycle ${c}: pedal.ts firmware ${file}`, async () => {
      const r = await pedal("firmware", file);
      if (r.code !== 0) await failedUpdate(new CheckError(`exit ${r.code}: ${r.stderr.trim()}`));
      const back = new RegExp(`^firmware: .*back as build \\S+ ${morph}$`, "m").exec(r.stdout);
      if (back === null) throw new CheckError(`expected back as ${morph}:\n${r.stdout}`);
      sameList(await list(), before);
      return back[0];
    });
    say(`cycle ${c}: ${line}`);
  }
}

// 5. The host refuses an untagged image.
await step("host refusal", async () => {
  const r = await pedal("firmware", B);
  if (r.code !== 1 || !r.stderr.includes("no DKFW tag"))
    throw new CheckError(`expected exit 1 and "no DKFW tag", got exit ${r.code}: ${r.stderr.trim()}`);
});
say("host refusal: bench image refused (no DKFW tag)");

// 6. The pedal refuses bad images and keeps running.
await step("pedal refusals", async () => {
  const seed = findSeeds().find((s) => s.usbPath === usbPath);
  if (seed === undefined) throw new CheckError(`no Seed at ${usbPath}`);
  const midi = await openRawMidi(seed.dev);
  try {
    const session = new PedalSession(midi);
    const id = await session.identity();
    const x = images.x;
    const crc = crc32(x);
    const tag = x.slice();
    tag[0x400] = tag[0x400]! ^ 0xff;
    const board = x.slice();
    new DataView(board.buffer).setUint32(0x408, 2, true);
    const cases: [string, () => Promise<ResultMessage>, number, number | null][] = [
      ["incomplete", () => rawUpdate(midi, x, crc, CHUNK_BYTES), RESULT.incomplete, null],
      ["crc", () => rawUpdate(midi, x, (crc ^ 1) >>> 0), RESULT.invalid, 1],
      ["tag", () => rawUpdate(midi, tag, crc32(tag)), RESULT.invalid, 3],
      ["board", () => rawUpdate(midi, board, crc32(board)), RESULT.invalid, 4],
    ];
    for (const [name, run, code, value] of cases) {
      const r = await run();
      if (r.code !== code || (value !== null && r.value !== value))
        throw new CheckError(`${name}: COMMIT answered ${RESULT_NAMES[r.code]} ${r.value}`);
      say(`pedal refusal ${name}: COMMIT ${RESULT_NAMES[r.code]}${value === null ? "" : ` ${r.value}`}`);
    }
    await Bun.sleep(1000);
    const after = await session.identity();
    if (after.build !== id.build || after.morph !== id.morph)
      throw new CheckError(`Identity changed: ${id.build} ${id.morph} -> ${after.build} ${after.morph}`);
  } finally {
    midi.close();
  }
  sameList(await list(), before);
});
say("pedal refusals: nothing restarted, slots unchanged");

// 7. Chrome: the editor uploads a preset, then updates the firmware to weight and back.
await step("chrome", async () => {
  const html = Bun.file(resolve(ROOT, "editor/dist/editor.html"));
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch: () => new Response(html, { headers: { "content-type": "text/html; charset=utf-8" } }),
  });
  const origin = `http://127.0.0.1:${server.port}`;
  const chrome = await launchChrome({ headless: !headful });
  const linked = `document.querySelector(".link-status")?.textContent === "LINKED"`;
  const build = `(document.querySelector(".toolbar-build")?.textContent ?? "")`;
  const bar = `(document.querySelector(".statusbar")?.textContent ?? "")`;
  const ready = `${linked} && !document.querySelector('.toolbar input[accept=".bin"]').disabled`;
  // A firmware step's failure, judged once Chrome has let go of the MIDI port.
  let updateError: unknown = null;
  try {
    await chrome.grantMidi(origin);
    const page = await chrome.open(origin);
    /** Waits for expr; fails at once when the status bar shows an error. */
    const settle = async (expr: string, ms: number, what: string) => {
      await page.waitFor(`(${expr}) || document.querySelector(".statusbar.error")`, ms, what);
      const error = await page.eval<string | null>(`document.querySelector(".statusbar.error")?.textContent ?? null`);
      if (error !== null) throw new CheckError(`${what}: ${error}`);
    };
    await page.waitFor(
      `${linked} && ${build} !== "" && document.querySelector('button[aria-label="Select slot 1"]')`,
      30000,
      "the editor linked and synced",
    );
    say(`chrome: editor LINKED, ${await page.eval<string>(build)}`);

    const n = scratch + 1;
    await page.click(`button[aria-label="Select slot ${n}"]`);
    await page.setFile('input[type=file][accept=".nam"]', resolve(ROOT, "tests/fixtures/nam/A2.nam"));
    await page.waitFor(`document.querySelectorAll('.edit-part[data-kind="new"]').length === 1`, 10000, "the model");
    await page.setFile('input[type=file][accept=".wav"]', resolve(ROOT, "tests/fixtures/ir/decay.wav"));
    await page.waitFor(`document.querySelectorAll('.edit-part[data-kind="new"]').length === 2`, 10000, "the IR");
    // The slot table's Save: the one beside the "N slot(s) changed" count.
    const save = `[...document.querySelectorAll(".edit-actions")].find((g) => /changed$/.test(g.querySelector(".edit-count")?.textContent ?? ""))?.querySelector("button:last-child")`;
    await page.waitFor(`${save} && !${save}.disabled`, 10000, "the slots' Save");
    await page.eval(`${save}.click()`);
    await settle(`${bar}.startsWith("Saved slot ${n}")`, 60000, `Saved slot ${n}`);
    say(`chrome: ${await page.eval<string>(bar)}`);

    for (const [file, isWeight] of [
      [W, true],
      [X, false],
    ] as const) {
      await page.waitFor(ready, 30000, "the Firmware button enabled");
      await page.setFile('.toolbar input[type=file][accept=".bin"]', resolve(ROOT, file));
      try {
        // The busy label first: the status bar may still show the previous update's result.
        await settle(`${bar}.startsWith("FIRMWARE")`, 10000, `the update of ${file} started`);
        await settle(`${bar}.startsWith("Firmware written")`, 60000, `Firmware written (${file})`);
        const written = await page.eval<string>(bar);
        await page.waitFor(
          `${linked} && ${build} !== "" && ${isWeight ? "" : "!"}${build}.endsWith(", weight morph")`,
          60000,
          `the toolbar's ${isWeight ? "weight" : "crossfade"} build`,
        );
        say(`chrome: ${written}; back as ${await page.eval<string>(build)}`);
      } catch (e) {
        updateError = e;
        break;
      }
    }
  } finally {
    await chrome.close();
    server.stop(true);
  }
  if (updateError !== null) await failedUpdate(updateError);
  await Bun.sleep(1000);
});

// 8. Verify and restore.
await step("verify and restore", async () => {
  const n = scratch + 1;
  const l = await list();
  if (!new RegExp(`^${n} ok .* ir (?!none)`, "m").test(l))
    throw new CheckError(`slot ${n} holds no model with an IR:\n${l}`);
  const e = await pedal("erase", String(n));
  if (e.code !== 0) throw new CheckError(`erase ${n}: ${e.stderr.trim()}`);
  sameList(await list(), before);
});
say(`ok (${cycles} cycles, ${secs(t0)} s)`);
