import { useEffect, useRef, useState } from "preact/hooks";
import { EqGraph } from "./components/EqGraph";
import { FrontPanel } from "./components/FrontPanel";
import { LogView } from "./components/LogView";
import { SlotEditor } from "./components/SlotEditor";
import { SlotTable } from "./components/SlotTable";
import { NO_STATUS, type Status, StatusLine } from "./components/StatusLine";
import { type NavTab, TabNav } from "./components/TabNav";
import { Toolbar } from "./components/Toolbar";
import { baseDraft, type Draft, draftProblem, isEdited, saveDraft, slotView } from "./draft";
import { dfuMsg } from "./link/protocol";
import type { PedalSession } from "./link/session";
import { errorText, usePedal } from "./usePedal";

type TabId = "presets" | "eq" | "log";
const TABS: readonly NavTab<TabId>[] = [
  { id: "presets", label: "Presets" },
  { id: "eq", label: "EQ" },
  { id: "log", label: "Log" },
];
const OK = 0;

type Drafts = Readonly<Partial<Record<number, Draft>>>;
const without = (ds: Drafts, ...slots: number[]): Partial<Record<number, Draft>> => {
  const out = { ...ds };
  for (const k of slots) delete out[k];
  return out;
};
const slotList = (ks: readonly number[]): string =>
  `slot${ks.length === 1 ? "" : "s"} ${ks.map((k) => k + 1).join(", ")}`;

export function App() {
  const { s, link, serial, refreshSlots } = usePedal();
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
  const edited = (s.slots ?? []).filter((x) => {
    const d = drafts[x.slot];
    return d !== undefined && isEdited(d, x);
  });
  const unsaved = edited.length > 0;

  useEffect(() => {
    if (!unsaved) return;
    const warn = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [unsaved]);

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
    const question = sm?.status === OK ? `Erase slot ${slot + 1} "${sm.name}"?` : `Erase slot ${slot + 1}?`;
    if (!window.confirm(question)) return;
    void slotChange("ERASING…", async (session) => {
      await session.erase(slot);
      setDrafts((ds) => without(ds, slot));
      return `Erased slot ${slot + 1}`;
    });
  };
  const onSave = () => {
    for (const x of edited) {
      const problem = draftProblem(drafts[x.slot]!);
      if (problem !== null) {
        setSelected(x.slot);
        setStatus({ text: `Slot ${x.slot + 1}: ${problem}`, error: true });
        return;
      }
    }
    const emptied = edited.filter((x) => x.status === OK && drafts[x.slot]!.model.kind === "none").map((x) => x.slot);
    if (emptied.length > 0 && !window.confirm(`Saving empties ${slotList(emptied)}. Continue?`)) return;
    const work = edited.map((x) => [x, drafts[x.slot]!] as const);
    void slotChange("SAVING…", async (session) => {
      const done: number[] = [];
      for (const [x, d] of work) {
        const label = `SAVING SLOT ${x.slot + 1}…`;
        setBusy(label);
        await saveDraft(session, x, d, (f) => setBusy(`${label} ${Math.floor(100 * f)} %`));
        done.push(x.slot);
        setDrafts((ds) => without(ds, x.slot));
      }
      return `Saved ${slotList(done)}`;
    });
  };
  const onDiscard = () => {
    if (!window.confirm(`Discard the unsaved changes to ${slotList(edited.map((x) => x.slot))}?`)) return;
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

  const sel = s.slots?.[selected];
  const presets = !linked ? (
    <p class="tab-empty">Waiting for the pedal.</p>
  ) : s.slots === null || sel === undefined ? (
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
        draft={drafts[selected] ?? baseDraft(sel)}
        onChange={(d) => setDrafts((ds) => ({ ...ds, [selected]: d }))}
        onRevert={() => setDrafts((ds) => without(ds, selected))}
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
            {tab === "log" ? <LogView lines={s.log} /> : tab === "eq" ? <EqGraph status={s.status} /> : presets}
          </div>
        </div>
      </div>
      <StatusLine text={busy ?? status.text} error={busy === null && status.error} />
    </main>
  );
}
