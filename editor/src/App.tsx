import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { CompTab } from "./components/CompTab";
import { ControlsTab } from "./components/ControlsTab";
import { EqTab } from "./components/EqTab";
import { FrontPanel } from "./components/FrontPanel";
import { GateEditor } from "./components/GateEditor";
import { LogView } from "./components/LogView";
import { LoopTab } from "./components/LoopTab";
import { MorphBox } from "./components/MorphBox";
import { SlotEditor } from "./components/SlotEditor";
import { SlotTable } from "./components/SlotTable";
import { NO_STATUS, type Status, StatusLine } from "./components/StatusLine";
import { type NavTab, TabNav } from "./components/TabNav";
import { Toolbar } from "./components/Toolbar";
import {
  baseDraft,
  type Draft,
  type Drafts,
  draftProblem,
  isEdited,
  morphDependents,
  saveDraft,
  slotView,
} from "./draft";
import { errorText } from "./format";
import { firmwareKind, firmwareProblem } from "./link/firmware";
import { CONTROLS } from "./link/params";
import { dfuMsg, OMNI, SLOT_STATUS, type SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import { saveMorph } from "./morph/save";
import { GATE_DEFAULT, type GateSettings, gateEqual } from "./preset/gate";
import { resolveMorph } from "./preset/morph";
import { useGateAudition } from "./useGateAudition";
import { useParamEdits } from "./useParamEdits";
import { usePedal } from "./usePedal";

type TabId = "presets" | "controls" | "comp" | "eq" | "loop" | "log";
const TABS: readonly NavTab<TabId>[] = [
  { id: "presets", label: "Presets" },
  { id: "controls", label: "Controls" },
  { id: "comp", label: "Compressor" },
  { id: "eq", label: "EQ" },
  { id: "loop", label: "Loop" },
  { id: "log", label: "Log" },
];
const without = (ds: Drafts, slot: number): Drafts => {
  const out = { ...ds };
  delete out[slot];
  return out;
};
const slotList = (ks: readonly number[]): string =>
  `slot${ks.length === 1 ? "" : "s"} ${ks.map((k) => k + 1).join(", ")}`;

export function App() {
  const { s, link, serial, refreshSlots, dispatch } = usePedal();
  const [tab, setTab] = useState<TabId>("presets");
  const [busy, setBusy] = useState<string | null>(null);
  const busyRef = useRef(false);
  const [status, setStatus] = useState<Status>(NO_STATUS);
  const [selected, setSelected] = useState(0);
  /** Unsaved edits per slot; Save applies them, Discard drops them. */
  const [drafts, setDrafts] = useState<Drafts>({});

  const linked = s.link.linked;
  const ready = linked && s.synced && busy === null;
  const playing = s.status ? s.status.slot : (s.slots?.find((x) => x.playing)?.slot ?? null);
  const edited: { slot: SlotMessage; draft: Draft }[] = (s.slots ?? []).flatMap((x) => {
    const d = drafts[x.slot];
    return d !== undefined && isEdited(d, x) ? [{ slot: x, draft: d }] : [];
  });
  // Slot drafts, or control assignments and values the pedal has not saved.
  const unsaved = edited.length > 0 || s.settingsEdited;

  useEffect(() => {
    if (!unsaved) return;
    const warn = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [unsaved]);

  const { tryGate, restoreGate } = useGateAudition(s, playing, drafts, serial);
  const { setParam, table } = useParamEdits(s, serial, dispatch);

  // One pedal operation at a time.
  const act = async (label: string, fn: () => Promise<string>): Promise<void> => {
    if (busyRef.current) return;
    busyRef.current = true;
    setStatus(NO_STATUS);
    setBusy(label);
    try {
      setStatus({ text: await fn(), error: false });
    } catch (e) {
      setStatus({ text: errorText(e), error: true });
    } finally {
      busyRef.current = false;
      setBusy(null);
    }
  };
  /** Slot changes, then LIST and the infos again whatever their outcome: the table shows what the flash holds. */
  const slotChange = (label: string, fn: (session: PedalSession) => Promise<string>) =>
    act(label, () =>
      serial(async (session) => {
        try {
          return await fn(session);
        } finally {
          await refreshSlots(session).catch(() => undefined);
        }
      }),
    );

  const onSwap = (a: number, b: number) =>
    void slotChange("SWAPPING…", async (session) => {
      await session.swap(a, b);
      // The drafts follow their presets, and so do the morph targets they name.
      const moved = (k: number) => (k === a ? b : k === b ? a : k);
      setDrafts((ds) => {
        const out: Partial<Record<number, Draft>> = {};
        for (const [k, d] of Object.entries(ds))
          if (d) out[moved(Number(k))] = typeof d.morph === "number" ? { ...d, morph: moved(d.morph) } : d;
        return out;
      });
      setSelected(b);
      return `Swapped slots ${Math.min(a, b) + 1} and ${Math.max(a, b) + 1}`;
    });
  const onErase = (slot: number) => {
    const sm = s.slots?.[slot];
    const question = sm?.status === SLOT_STATUS.ok ? `Erase slot ${slot + 1} "${sm.name}"?` : `Erase slot ${slot + 1}?`;
    if (!window.confirm(question)) return;
    void slotChange("ERASING…", async (session) => {
      await session.erase(slot);
      setDrafts((ds) => without(ds, slot));
      return `Erased slot ${slot + 1}`;
    });
  };
  const onSave = () => {
    const emptied = edited
      .filter(({ slot: x, draft }) => x.status === SLOT_STATUS.ok && draft.model.kind === "none")
      .map(({ slot: x }) => x.slot);
    for (const { slot: x, draft } of edited) {
      const target = draft.model.kind !== "none" && typeof draft.morph === "number" ? draft.morph : null;
      const problem =
        draftProblem(draft) ??
        (target !== null && emptied.includes(target) ? `Slot ${target + 1}, its morph target, is being emptied` : null);
      if (problem !== null) {
        setSelected(x.slot);
        setStatus({ text: `Slot ${x.slot + 1}: ${problem}`, error: true });
        return;
      }
    }
    if (emptied.length > 0 && !window.confirm(`Saving empties ${slotList(emptied)}. Continue?`)) return;
    const infos = s.infos;
    const dependents = morphDependents(s.slots ?? [], infos, drafts);
    void slotChange("SAVING…", async (session) => {
      const done: number[] = [];
      // The morph targets to analyse and write once every draft is written: a target replaced in this save is then
      // analysed as it now is.
      const morphs = new Map<number, number | "none">();
      for (const { slot: x, draft } of edited) {
        const label = `SAVING SLOT ${x.slot + 1}…`;
        setBusy(label);
        const saved = await saveDraft(session, x, draft, resolveMorph(infos, x.slot), (f) =>
          setBusy(`${label} ${Math.floor(100 * f)} %`),
        );
        if (saved.morph !== null) morphs.set(x.slot, saved.morph);
        done.push(x.slot);
        setDrafts((ds) => without(ds, x.slot));
      }
      for (const [k, target] of dependents) if (!morphs.has(k)) morphs.set(k, target);
      let notes = "";
      for (const [k, target] of morphs) {
        if (target !== "none" && emptied.includes(target)) continue; // the pedal turns a morph with an erased slot off
        setBusy(`ANALYSING SLOT ${k + 1}…`);
        const r = await saveMorph(session, k, target);
        // The fit is the weight morph's; the crossfade firmware stores the block for that image and plays both models.
        notes +=
          r.target === null
            ? `; slot ${k + 1} morph off`
            : `; slot ${k + 1} morphs with slot ${r.target + 1}` +
              (s.identity?.morph === "weight" ? ` (fit ${(100 * (r.fit ?? 0)).toFixed(1)} %)` : "");
      }
      return `Saved ${slotList(done)}${notes}`;
    });
  };
  const onDiscard = () => {
    if (!window.confirm(`Discard the unsaved changes to ${slotList(edited.map(({ slot: x }) => x.slot))}?`)) return;
    if (playing !== null) restoreGate(playing);
    setDrafts({});
  };
  const onDfu = () => {
    if (
      !window.confirm("Restart the pedal into DFU mode for a firmware update? Audio stops until it has been flashed.")
    )
      return;
    void act("DFU…", () =>
      serial(async () => {
        link.send(dfuMsg());
        return "DFU sent: the pedal is restarting into its bootloader";
      }),
    );
  };
  const onFirmware = async (file: File) => {
    const image = new Uint8Array(await file.arrayBuffer());
    const problem = firmwareProblem(image);
    if (problem !== null) {
      setStatus({ text: `${file.name}: ${problem}`, error: true });
      return;
    }
    const kind = firmwareKind(image) === "weight" ? "weight morph" : "crossfade";
    const question =
      `Update the pedal's firmware to ${file.name} (${kind} image, ${Math.ceil(image.length / 1024)} KB)? ` +
      "Audio stops while the pedal restarts.";
    if (!window.confirm(question)) return;
    void act("FIRMWARE…", () =>
      serial(async (session) => {
        const r = await session.updateFirmware(image, (sent, size) =>
          setBusy(`FIRMWARE ${Math.floor((100 * sent) / size)} %`),
        );
        return `Firmware written in ${r.flashMs} ms: the pedal is restarting`;
      }),
    );
  };
  const onAssign = (control: number, id: number | null) =>
    void act("ASSIGNING…", () =>
      serial(async (session) => {
        await session.assign(control, id);
        dispatch({ type: "settingsEdited", edited: true });
        return `${CONTROLS[control]!.id}: ${s.params?.find((p) => p.id === id)?.label ?? "None"}`;
      }),
    );
  const onExpChannel = (channel: number) =>
    void act("SETTING EXP CHANNEL…", () =>
      serial(async (session) => {
        await session.expChannel(channel);
        dispatch({ type: "settingsEdited", edited: true });
        return `EXP channel: ${channel === OMNI ? "any" : channel}`;
      }),
    );
  const onMidiInChannel = (channel: number) =>
    void act("SETTING MIDI IN CHANNEL…", () =>
      serial(async (session) => {
        await session.midiInChannel(channel);
        dispatch({ type: "settingsEdited", edited: true });
        return `MIDI IN channel: ${channel === OMNI ? "any" : channel}`;
      }),
    );
  const onSaveSettings = () =>
    void act("SAVING SETTINGS…", () =>
      serial(async (session) => {
        const ms = await session.save();
        dispatch({ type: "settingsEdited", edited: false });
        return `Saved the settings (flash ${ms} ms)`;
      }),
    );

  const sel = s.slots?.[selected];
  // The selected slot's gate: the draft's, else the stored one (null while it is read), GATE_DEFAULT for a new preset.
  const storedGate = sel?.status === SLOT_STATUS.ok ? (s.infos[selected]?.gate ?? null) : GATE_DEFAULT;
  const selDraft: Draft | undefined = sel === undefined ? undefined : (drafts[selected] ?? baseDraft(sel));
  const onGate = (g: GateSettings) => {
    if (sel === undefined || storedGate === null) return;
    setDrafts((ds) => ({
      ...ds,
      [selected]: { ...(ds[selected] ?? baseDraft(sel)), gate: gateEqual(g, storedGate) ? null : g },
    }));
    tryGate(selected, g);
  };
  const settings = { enabled: ready, edited: s.settingsEdited, onSet: setParam, onSave: onSaveSettings };

  const panel = (): ComponentChildren => {
    if (tab === "log") return <LogView lines={s.log} />;
    if (!linked) return <p class="tab-empty">Waiting for the pedal.</p>;
    if (tab === "presets") {
      if (s.slots === null || sel === undefined || selDraft === undefined)
        return <p class="tab-empty">{s.syncError ?? "Reading the pedal…"}</p>;
      return (
        <>
          <SlotTable
            slots={s.slots}
            views={s.slots.map((x) => slotView(x, drafts[x.slot]))}
            playing={playing}
            selected={selected}
            onSelect={setSelected}
            enabled={ready}
            onSwap={onSwap}
            onErase={onErase}
            edits={edited.length}
            onSave={onSave}
            onDiscard={onDiscard}
          />
          <SlotEditor
            key={selected}
            slot={sel}
            slots={s.slots}
            infos={s.infos}
            draft={selDraft}
            morphKind={s.identity?.morph ?? null}
            onChange={(d) => setDrafts((ds) => ({ ...ds, [selected]: d }))}
            onRevert={() => {
              if (selected === playing) restoreGate(selected);
              setDrafts((ds) => without(ds, selected));
            }}
          />
          <GateEditor
            slotNumber={selected + 1}
            gate={selDraft.gate ?? storedGate}
            edited={selDraft.gate !== null}
            enabled={selDraft.model.kind !== "none"}
            playing={selected === playing}
            status={s.status}
            onChange={onGate}
          />
          <MorphBox
            table={table}
            infos={s.infos}
            selected={selected}
            morphKind={s.identity?.morph ?? null}
            {...settings}
          />
        </>
      );
    }
    // The other tabs need the parameter table, which firmware from before it does not have.
    if (table === null) return <p class="tab-empty">{s.paramsError ?? "Reading the pedal…"}</p>;
    switch (tab) {
      case "controls":
        return (
          <ControlsTab
            table={table}
            board={s.identity?.board ?? null}
            enabled={ready}
            edited={s.settingsEdited}
            onAssign={onAssign}
            onExpChannel={onExpChannel}
            onMidiInChannel={onMidiInChannel}
            onSave={onSaveSettings}
          />
        );
      case "comp":
        return <CompTab status={s.status} table={table} {...settings} />;
      case "eq":
        return <EqTab status={s.status} table={table} {...settings} />;
      case "loop":
        return <LoopTab table={table} {...settings} />;
    }
  };

  return (
    <main class="window">
      <header class="titlebar">
        <h1>Ducky</h1>
      </header>
      <Toolbar
        link={s.link}
        identity={s.identity}
        onInput={(id) => link.selectInput(id)}
        onOutput={(id) => link.selectOutput(id)}
        firmwareEnabled={linked && s.identity !== null && busy === null}
        onDfu={onDfu}
        onFirmware={(f) => void onFirmware(f)}
      />
      <div class="window-body">
        <FrontPanel s={s} />
        <div class="workspace">
          <TabNav tabs={TABS} active={tab} onSelect={setTab} />
          <div class="tabpanel" role="tabpanel" id="tabpanel" aria-labelledby={`tab-${tab}`}>
            {panel()}
          </div>
        </div>
      </div>
      <StatusLine text={busy ?? status.text} error={busy === null && status.error} />
    </main>
  );
}
