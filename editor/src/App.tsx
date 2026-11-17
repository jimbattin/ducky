import { useEffect, useRef, useState } from "preact/hooks";
import { CompTab } from "./components/CompTab";
import { ControlsTab } from "./components/ControlsTab";
import { EqTab } from "./components/EqTab";
import { FrontPanel } from "./components/FrontPanel";
import { GateEditor } from "./components/GateEditor";
import { LogView } from "./components/LogView";
import { SlotEditor } from "./components/SlotEditor";
import { SlotTable } from "./components/SlotTable";
import { NO_STATUS, type Status, StatusLine } from "./components/StatusLine";
import { type NavTab, TabNav } from "./components/TabNav";
import { Toolbar } from "./components/Toolbar";
import { baseDraft, type Draft, type Drafts, draftProblem, isEdited, saveDraft, slotView } from "./draft";
import { errorText } from "./format";
import { CONTROLS } from "./link/params";
import { dfuMsg, SLOT_STATUS, type SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import { GATE_DEFAULT, type GateSettings, gateEqual } from "./preset/gate";
import { useGateAudition } from "./useGateAudition";
import { useParamEdits } from "./useParamEdits";
import { usePedal } from "./usePedal";

type TabId = "presets" | "controls" | "comp" | "eq" | "log";
const TABS: readonly NavTab<TabId>[] = [
  { id: "presets", label: "Presets" },
  { id: "controls", label: "Controls" },
  { id: "comp", label: "Compressor" },
  { id: "eq", label: "EQ" },
  { id: "log", label: "Log" },
];
const without = (ds: Drafts, ...slots: number[]): Partial<Record<number, Draft>> => {
  const out = { ...ds };
  for (const k of slots) delete out[k];
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
  const { setParam, shownCode } = useParamEdits(s, serial, dispatch);

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
      setDrafts((ds) => {
        const out = without(ds, a, b);
        if (ds[a]) out[b] = ds[a];
        if (ds[b]) out[a] = ds[b];
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
    for (const { slot: x, draft } of edited) {
      const problem = draftProblem(draft);
      if (problem !== null) {
        setSelected(x.slot);
        setStatus({ text: `Slot ${x.slot + 1}: ${problem}`, error: true });
        return;
      }
    }
    const emptied = edited
      .filter(({ slot: x, draft }) => x.status === SLOT_STATUS.ok && draft.model.kind === "none")
      .map(({ slot: x }) => x.slot);
    if (emptied.length > 0 && !window.confirm(`Saving empties ${slotList(emptied)}. Continue?`)) return;
    void slotChange("SAVING…", async (session) => {
      const done: number[] = [];
      for (const { slot: x, draft } of edited) {
        const label = `SAVING SLOT ${x.slot + 1}…`;
        setBusy(label);
        await saveDraft(session, x, draft, (f) => setBusy(`${label} ${Math.floor(100 * f)} %`));
        done.push(x.slot);
        setDrafts((ds) => without(ds, x.slot));
      }
      return `Saved ${slotList(done)}`;
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
  const onAssign = (control: number, id: number | null) =>
    void act("ASSIGNING…", () =>
      serial(async (session) => {
        await session.assign(control, id);
        dispatch({ type: "settingsEdited", edited: true });
        return `${CONTROLS[control]!.id}: ${s.params?.find((p) => p.id === id)?.label ?? "None"}`;
      }),
    );
  const onSaveSettings = () =>
    void act("SAVING CONTROLS…", () =>
      serial(async (session) => {
        const ms = await session.save();
        dispatch({ type: "settingsEdited", edited: false });
        return `Saved the controls (flash ${ms} ms)`;
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
  const presets = !linked ? (
    <p class="tab-empty">Waiting for the pedal.</p>
  ) : s.slots === null || sel === undefined || selDraft === undefined ? (
    <p class="tab-empty">{s.syncError ?? "Reading the pedal…"}</p>
  ) : (
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
        info={s.infos[selected] ?? null}
        draft={selDraft}
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
    </>
  );

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
        dfuEnabled={linked && s.identity !== null && busy === null}
        onDfu={onDfu}
      />
      <div class="window-body">
        <FrontPanel s={s} />
        <div class="workspace">
          <TabNav tabs={TABS} active={tab} onSelect={setTab} />
          <div class="tabpanel" role="tabpanel" id="tabpanel" aria-labelledby={`tab-${tab}`}>
            {tab === "log" ? (
              <LogView lines={s.log} />
            ) : (tab === "eq" || tab === "comp" || tab === "controls") && !linked ? (
              <p class="tab-empty">Waiting for the pedal.</p>
            ) : tab === "comp" ? (
              <CompTab
                status={s.status}
                params={s.params}
                paramsError={s.paramsError}
                values={s.values}
                shownCode={shownCode}
                enabled={ready}
                edited={s.settingsEdited}
                onSet={setParam}
                onSave={onSaveSettings}
              />
            ) : tab === "eq" ? (
              <EqTab
                status={s.status}
                params={s.params}
                paramsError={s.paramsError}
                values={s.values}
                shownCode={shownCode}
                enabled={ready}
                edited={s.settingsEdited}
                onSet={setParam}
                onSave={onSaveSettings}
              />
            ) : tab === "controls" ? (
              <ControlsTab
                params={s.params}
                paramsError={s.paramsError}
                values={s.values}
                shownCode={shownCode}
                enabled={ready}
                edited={s.settingsEdited}
                onAssign={onAssign}
                onSave={onSaveSettings}
              />
            ) : (
              presets
            )}
          </div>
        </div>
      </div>
      <StatusLine text={busy ?? status.text} error={busy === null && status.error} />
    </main>
  );
}
