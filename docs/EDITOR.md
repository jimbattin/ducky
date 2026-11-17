# Editor

The browser editor manages the pedal's 8 preset slots, assigns its knobs and switches to parameters, sets the compressor, the EQ and the FX loop, and shows its live state over USB-MIDI. It is written in Preact and TypeScript, built with Bun into one self-contained file, `editor/dist/editor.html` (scripts and styles inlined, no external URLs). It needs Chrome: Web MIDI with SysEx. It follows the conventions of bad_tape's editor (`../bad_tape`): the Platinum look, a read-only sidebar of the pedal's controls, and tabs over a panel.

It sets slots (presets), which parameter each of Knobs 3–6 and SW1–SW3 drives, and the parameters' values; while you edit a preset it auditions the playing preset's noise gate. A value set from the editor holds until its control moves, and then the pedal is "what you see is what you get" again. Knobs 1 and 2 are never set.

## Running

```
make -C editor serve      # hot reload at http://localhost:3000/ (PORT=… to move it)
make -C editor html       # editor/dist/editor.html
make -C editor test       # bun:test: the editor's, the converters' and the link's tests
make -C editor check      # tsc, then Biome over editor/ and tools/*.ts
make -C editor fmt        # Biome's fixes and formatting, same files
```

`make -C editor` installs the dependencies on first use (`bun install --frozen-lockfile`).

- **Chrome:** the first visit asks for MIDI with SysEx; allow it. Without Web MIDI the status reads `WEB MIDI UNAVAILABLE - USE CHROME`, or `WEB MIDI NEEDS HTTPS OR LOCALHOST` on a page served over plain HTTP from anywhere but localhost. Refused access reads `NO MIDI ACCESS - CONNECT THE PEDAL, ALLOW MIDI, RELOAD`.
- **Port conflicts:** the page holds the pedal's MIDI port. Close the tab before `tools/pedal.ts` or `tools/usb_log.py` (`docs/FIRMWARE.md` "Port conflicts").
- **Hosting:** the built file runs from any URL, and in an `<iframe>` too, when the frame grants MIDI (`allow="midi"`).

## The window

![Presets tab](images/editor/presets.png)

- **Toolbar:** the link lamp and status (`LINKED` green, `NO RESPONSE` amber, else off: `CONNECTING…`, `NO MIDI DEVICE`, or why Web MIDI is unavailable), the **In** and **Out** port menus, the firmware build from the Identity Reply, and **DFU mode…**.
- **Sidebar:** read-only, from STATUS and VALUES. FS1 mute, FS2's playing slot, Knobs 1 and 2 in the firmware's units (`editor/src/controls.ts` mirrors `daisy/src/dsp/gain.h`), then Knobs 3–6 and the three toggles, each named after the parameter it drives (`None` without one) with that parameter's value, and the CPU load. A knob's bar is the knob's position; its readout is the parameter's value, which differs while the control is **Held**: set from the editor or by another control on the same parameter, and waiting to be moved. A cut filter's frequency reads `Off` at its bypass end. A resonance is dimmed when its slope is 6 dB/oct or its filter is off. The CPU bar's fill is the average load (red from 80 %), its amber marker the peak, and the Overload lamp lights on a new underrun or a full peak.
- **Status bar:** the progress of the running operation (`SAVING SLOT 5… 40 %`), then its result or its error.

## Presets tab

The slot table lists the 8 slots: the playing lamp, the slot number (click it, or the row, to select), the preset, model and IR names, ↑/↓ to swap with a neighbour, and **Erase**. Unsaved changes are amber. **Save** and **Discard** under the table act on every unsaved change at once.

Under the table, the selected slot's editor: its name, its model (**Add .nam…** / **Replace…** / **Remove**) and its IR (**Add .wav…** / **Replace…** / **Remove**). Each part is kept, replaced or removed. Below them are the metadata, level match and input calibration of the result, its warnings, and **Revert slot N**. The warnings are: no loudness, no `input_level_dbu`, a cab model with an IR, and a truncated new IR. Files go through the same converters as the CLI tools (`editor/src/nam/`, `editor/src/ir/`).

![Importing a model and an IR into slot 5](images/editor/import.png)

Edits stay in per-slot drafts (`editor/src/draft.ts`) until Save, which applies each changed slot in turn:

- A removed model erases the slot, after a confirmation.
- A changed gate alone is a GATE: no upload, so no reload fade.
- Otherwise the preset is composed and uploaded. When it keeps the stored model or IR, the whole slot is read back first (READ, CRC-checked by `splitSlot` in `editor/src/preset/bank.ts`). When it keeps neither, only the stored gate is read (8 bytes).

A new model renames the preset while its name is still the old model's. A swap moves the drafts with the slots. Leaving the page with unsaved drafts asks first. After every slot change the table is listed and read again, so it shows what the flash holds even after a failure.

### Noise gate box

Under the slot editor: **On**/**Off** and the five settings as sliders (`editor/src/gateControls.ts` `GATE_PARAMS`: the threshold linear, the rest log), then a meter. The meter shows the input level from STATUS on −90…0 dBFS, with markers where the gate closes (amber) and opens (faint amber). While the slot plays, the lamp line shows the gate's state from its gain reduction: Open, Closing or Closed.

On the playing slot every change is heard at once: it is auditioned with GATE TRY (`editor/src/useGateAudition.ts`), coalesced to the latest value with one request in flight. A preset switch or a resync sends an unsaved gate again; Revert and Discard send the stored one back. A gate edit is part of the slot's draft until Save.

The sliders:

- Arrow keys step one resolution of the setting (1 dB, 0.1 ms, 1 ms, 0.1 of ratio).
- Page Up/Down move 10 % of the travel; Home/End go to the ends.
- Dragging the thumb moves it from where it is; pressing the track jumps there first. Shift drags 10× finer.
- Double-click resets the setting to its default.

## Controls tab

![Controls tab](images/editor/controls.png)

The pedal's parameter table and its live assignments and values, read from the pedal (DUMP, then the VALUES stream): nothing about the parameters is built into the editor except how to draw the EQ (`docs/MIDI.md` "Parameters", "Controls"). The parameters' values are set on the Compressor, EQ and Loop tabs.

- **Controls:** a menu per control, K3–K6 and SW1–SW3, with the value of the parameter it drives. Every control starts unassigned (`None`). A knob takes any parameter; a switch's menu lists only the choice parameters (`eq_enable`, the slopes, `comp_enable`, `comp_level`, `comp_mode`, `loop_mode`). A parameter may be on several controls. Choosing one sends ASSIGN: the parameter keeps its value, and the control is **Held** until it moves.
- **Held** ends when the control moves: a knob by more than 3 % of its travel, a switch to another position. The parameter then follows the control.
- **Save** writes the assignments and the values to the pedal's settings record (SAVE, `docs/FIRMWARE.md` "Settings record"); until then they last until power-off, the line beside the button reads `Not saved on the pedal`, and leaving the page asks first. At power-up a parameter on a control takes the control's position, so a saved value matters for parameters on no control. The Compressor, EQ and Loop tabs have the same Save.
- Firmware from before the parameter table does not answer DUMP: the tab says so, and the rest of the editor works.

## Compressor tab

![Compressor tab](images/editor/compressor.png)

The optical compressor before the input trim (`docs/compressor-spec.md` "DSP model"), set directly: a meter, then a row per parameter.

- **Meter:** the compressor's gain reduction from STATUS (the largest cut of its low-frequency gain below Sustain's per 100 ms) on 0…24 dB, read out as `cut X.X dB`. The lamp line reads `Compressor off`, `Not compressing` (green, a cut under 1 dB) or `Compressing` (amber). With Sustain at 0 dB it always reads 0.
- **Rows:** a Stage box with `comp_enable`; a Compressor box with `comp_sustain` (the stage's gain, 0…+20.8 dB) and `comp_level` (−3, 0, +3 dB); an Output box with `comp_mode` (blend: Balance mixes the dry and the compressed signal; direct: the compressed signal only, Balance its volume) and `comp_balance` (the knob's rotation, 0…100 %; the pedal applies the audio taper). They work as the EQ tab's rows (`editor/src/useParamEdits.ts`); every row but `comp_enable` is dimmed while the compressor is off.
- **Save** as on the Controls tab.

## EQ tab

The EQ (`docs/EQ.md`: low-cut, bells 1–3, high-cut) set directly, as bad_tape's: a response graph with a handle per band, then a row per parameter.

- **Graph:** the response of the five bands plus Knob 2's output volume (`editor/src/eqModel.ts`), drawn from the parameters' shown values (an edit made here at once, else the pedal's VALUES, whichever control set them) and from STATUS for Knob 2 and the mute. The whole chain is filled to 0 dB; each band in use (a bell off 0 dB, a cut that is on) is a thin curve in its handle's colour. The volume is the title's badge, and the gain at the pointer is read out top right. `EQ OFF` (`eq_enable` off) and muted dash the curve; the volume's off end draws only the bands. Knob 1 is not drawn: it sets the drive into the model, not a level after it. Neither are the model and the IR.
- **Handles:** L, 1, 2, 3, H. A bell's sits at its frequency and gain; a cut's on its curve at its cutoff, and an off cut's at its off end on the 0 dB line, faded. Hover one for its settings.
  - Drag a bell: left and right is its frequency, up and down its gain. Drag a cut: its frequency only; dragged to or past its off end (the graph's left edge for the low-cut, the right for the high-cut) it is off.
  - The wheel over a handle: a bell's Q, or a cut's resonance (when it is on and steeper than 6 dB/oct), a sixth of an octave per notch.
  - Double-click: a bell back to 0 dB, a cut off.
  - Keys on a focused handle (Tab to it): Left/Right frequency by a sixth of an octave (Shift: a 24th); Up/Down a bell's gain by 0.5 dB (Shift: 0.1); PageUp/PageDown Q or resonance by a sixth of an octave; Home as a double-click.
  - Each change is sent as SET with the parameter's code (`codeOfUnits`), only when the code changes.
- **Rows:** a Stage box with `eq_enable`, then a box per band (Low-cut, Bell 1–3, High-cut) with its parameters: the controls each is on, a slider (continuous) or a button group (a choice), and its value. A change is sent at once as SET (`editor/src/useParamEdits.ts`: coalesced to the latest value per parameter, one request in flight) and heard at once. It holds every control on that parameter; a parameter on no control keeps the value. The sliders work as the noise gate's; double-click resets to the parameter's default. A resonance is dimmed when it has no effect, and every row but `eq_enable` while the EQ is off.
- **Save** as on the Controls tab.

![EQ tab](images/editor/eq.png)

## Loop tab

![Loop tab](images/editor/loop.png)

The FX loop (`docs/LOOP.md`): which jack carries what in the pedal's loop mode, then the mode and the send and return levels.

- **Signal flow:** a drawing of the chain in the shown `loop_mode` (`LoopDiagram.tsx`), from the input's tip to the output's tip, the jacks outlined amber. In MISO the output's ring carries the same signal. In pre-model and post-model the chain breaks between Send level and Return level, where the loop pedals sit, sent on the output's ring and returned on the input's ring. Under it, what the mode is for and, in the loop modes, how to cable it: an insert cable on each jack.
- **Rows:** a Mode box with `loop_mode` (MISO, pre-model, post-model), and a Levels box with `loop_send` and `loop_return` (−12…+12 dB, 0 dB at noon), dimmed in MISO, where they do nothing. They work as the EQ tab's rows; a mode change is heard at once, through a 5 ms fade out and in.
- **Save** as on the Controls tab.

## Log tab

The pedal's log lines (`docs/FIRMWARE.md` "Log lines"; the last 1,000), each with its local receive time. It follows new lines while scrolled to the end.

![Log tab](images/editor/log.png)

## Link and sync

- **Link:** the editor picks the input and output ports whose name contains `daisy` (the toolbar's In/Out menus override them). It sends HELLO every second until the pedal answers, and reads `LINKED` while anything arrived in the last 4 s. WATCH's STATUS and VALUES every 100 ms keep the link fresh.
- **Sync:** on each link, on the boot line (`ducky …`, so a reflashed or power-cycled pedal is picked up without a reload), and every 3 s until one succeeds. A sync is Identity (device 64), LIST, every ok slot's metadata over READ (`editor/src/preset/info.ts`: 4 READs per slot, 5 with an IR), DUMP (the parameter table and the values), then WATCH on. A boot line also clears `Not saved on the pedal`: a restarted pedal is back on its saved settings.
- **One request at a time:** every request goes through one queue (`usePedal.ts` `serial`), because the pedal answers one request at a time.
- **DFU mode…** sends DFU after a confirmation. `tools/flash_when_ready.sh` then flashes, and the editor relinks and resyncs by itself.

```mermaid
flowchart LR
  WM[Web MIDI] --> PL[midi.ts PedalLink] -->|PedalMessage| UP[usePedal] --> RD[state.ts reducer] --> UI[App and components]
  UI -->|"serial(fn)"| PS[link/session.ts PedalSession] --> TR[PedalLink.transport] --> WM
```

## Source layout

All paths are under `editor/`. Each `*.test.ts` sits beside the module it tests.

| Path | Role |
|---|---|
| `Makefile` | `serve`, `html`, `test`, `check`, `fmt`; `check` and `fmt` also cover `../tools/*.ts`. |
| `build.ts` | Bundles `src/editor.html` into `dist/editor.html` (`make html`). |
| `server.ts` | The dev server with hot reload (`make serve`). |
| `biome.jsonc`, `tsconfig.json` | Lint, format and type-check settings (Code conventions). |
| `package.json`, `bun.lock` | Preact; TypeScript, Biome and Bun's types for development. |
| `src/editor.html`, `src/main.tsx` | The page and its entry point. |
| `src/App.tsx` | The window: tabs (Presets, Controls, Compressor, EQ, Loop, Log), the slot actions (swap, erase, save, discard, DFU), the drafts, and the settings actions (assign, save). |
| `src/ducky.css` | The Platinum styles. |
| `src/midi.ts` | `PedalLink`: Web MIDI ports, SysEx reassembly, the HELLO keep-alive, the link status, the session's transport. |
| `src/usePedal.ts` | The link's lifecycle: the reducer it feeds, the sync and its retries, the `serial` request queue. |
| `src/state.ts` | The reducer: link, identity, slots, slot infos, STATUS (`compCutDb` among its fields), load, log, the parameter table, VALUES, and whether the settings are unsaved. |
| `src/useGateAudition.ts` | GATE TRY auditions of the gate box's changes on the playing slot. |
| `src/useParamEdits.ts` | SETs from the Compressor, EQ and Loop tabs, coalesced per parameter, and the value shown while one is in flight. |
| `src/draft.ts` | Per-slot unsaved drafts, what the slot table shows for them, and Save (`saveDraft`). |
| `src/importer.ts` | `.nam` and `.wav` files through the converters; rejection text and import warnings. |
| `src/controls.ts` | Knob 1's and Knob 2's readouts in the firmware's tapers; a toggle's position; level match and input calibration. |
| `src/eqModel.ts` | The EQ's bands (`BANDS`) and their frequency response from the EQ parameters' values and Knob 2; when a resonance has no effect. |
| `src/gateControls.ts` | The gate box's sliders: labels, tapers, resolutions, readouts. |
| `src/device.ts` | `SAMPLE_RATE`, `NAME_MAX`, `sanitiseName`, `writeAscii`: what the converters, the preset code and the link share about the pedal. |
| `src/crc32.ts` | CRC-32 (zlib), as the firmware checks binaries and link chunks. |
| `src/format.ts` | Display formats: the typographic minus, signed dB, Hz, IR length, error text. Plain TypeScript: `link/params.ts` uses it. |
| `src/testFixtures.ts` | `readFixture`, `readFixtureText`: `tests/fixtures/` for the tests; `PARAMS`: the pedal's parameter table, checked against `docs/MIDI.md` (never bundled). |
| `src/components/Toolbar.tsx` | The link lamp, status, port menus, build and DFU button. |
| `src/components/FrontPanel.tsx` | The sidebar: footswitches, the fixed knobs, each assignable knob and switch with its parameter, CPU load. |
| `src/components/KnobGauge.tsx` | A knob's position as a read-only slider track with ticks. |
| `src/components/Positions.tsx` | A toggle's three positions. |
| `src/components/HeldBadge.tsx` | The Held mark on a control waiting to move. |
| `src/components/ControlsTab.tsx` | The Controls tab: each control's parameter and its value, and Save. |
| `src/components/CompTab.tsx` | The Compressor tab: the meter, the Stage, Compressor and Output boxes of parameter rows, and Save. |
| `src/components/CompMeter.tsx` | The compressor's gain reduction (STATUS `compCutDb`) and its state. |
| `src/components/EqTab.tsx` | The EQ tab: the graph, the Stage box and a box of parameter rows per band, and Save. |
| `src/components/LoopTab.tsx` | The Loop tab: the signal flow and what the mode is for, the Mode and Levels boxes of parameter rows, and Save. |
| `src/components/LoopDiagram.tsx` | The Loop tab's signal-flow drawing for a `loop_mode` (static SVG). |
| `src/components/ParamRow.tsx` | One parameter: label, controls on it, slider or button group, readout. |
| `src/components/SettingsActions.tsx` | The settings record's state line and Save. |
| `src/components/AssignSelect.tsx` | A control's menu of the parameters it accepts. |
| `src/components/Segmented.tsx` | A button group: one pressed button per choice. |
| `src/components/CpuMeter.tsx` | The CPU load meter. |
| `src/components/Meter.tsx` | A bar with a fill and markers over a lamp line; the CPU, gate and compressor meters. |
| `src/components/TabNav.tsx` | The WAI-ARIA tab list (Left/Right cycle, Home/End jump). |
| `src/components/SlotTable.tsx` | The slot table with swap, erase, Save and Discard. |
| `src/components/SlotEditor.tsx` | The selected slot's name, model and IR, its warnings and Revert. |
| `src/components/Details.tsx` | A model's and an IR's metadata. |
| `src/components/GateEditor.tsx` | The noise gate box. |
| `src/components/GateMeter.tsx` | The gate's input level against its thresholds, and its state. |
| `src/components/Slider.tsx` | `Slider` (pointer and keyboard) and `SliderTrack`, which `KnobGauge` shares. |
| `src/components/EqGraph.tsx` | The EQ tab's response graph and its draggable band handles (pointer, wheel, keys). |
| `src/components/LogView.tsx` | The Log tab. |
| `src/components/GroupBox.tsx`, `Lamp.tsx`, `StatusLine.tsx` | An etched group box, a status lamp, the status bar. |
| `src/link/` | The SysEx link (`docs/MIDI.md`): `protocol.ts` builders, codecs and parsing; `session.ts` `PedalSession`; `params.ts` a parameter's code as its value and readout, the controls and what each accepts. Shared with `tools/pedal.ts`. |
| `src/nam/` | The `.nam` → model binary converter (`docs/NAM_FORMAT.md`). Shared with `tools/nam_convert.ts`. |
| `src/ir/` | The `.wav` → IR binary converter, its WAV reader and resampler (`docs/FILTERS.md`). Shared with `tools/ir_convert.ts`. |
| `src/preset/` | `bank.ts` the slot and bank builders, `gate.ts` the gate block, `info.ts` a stored preset's metadata. Shared with `tools/preset_bank.ts` and `tools/pedal.ts`. |

## Code conventions

- **TypeScript:** `strict` with `noUncheckedIndexedAccess`. A `!` marks an index that a check before it guarantees (the wire parsers above all); `biome.jsonc` says why `noNonNullAssertion` is off. Where a type can carry what is known, use the type instead (App's `edited` list and `selDraft`).
- **Biome:** the recommended rules plus the React domain, imports organised, 120 columns. `make -C editor check` must pass with no warnings; a `biome-ignore` states its reason.
- **One module per shared fact:** the sample rate and name rules in `device.ts`, display formats in `format.ts`, the SLOT statuses in `link/protocol.ts` `SLOT_STATUS`, the slot count in `preset/bank.ts` `SLOT_COUNT`, the controls in `link/params.ts` `CONTROLS`. The parameters' names, ranges and labels come from the pedal. No local copies.
- **Shared code stays portable:** `link/`, `nam/`, `ir/`, `preset/`, `device.ts`, `format.ts` and `crc32.ts` are plain TypeScript (no DOM, no Bun APIs), because `tools/*.ts` imports them too.
- **CSS:** each class in `ducky.css` is used by a component, and each class a component uses is styled; shared looks (bevels, wells, ellipsis, focus ring) are one rule with a selector list.
