import { useRef, useState } from "preact/hooks";
import { Details } from "./components/Details";
import { EqGraph } from "./components/EqGraph";
import { FrontPanel } from "./components/FrontPanel";
import { GroupBox } from "./components/GroupBox";
import { ImportBox } from "./components/ImportBox";
import { LogView } from "./components/LogView";
import { SlotTable } from "./components/SlotTable";
import { NO_STATUS, type Status, StatusLine } from "./components/StatusLine";
import { type NavTab, TabNav } from "./components/TabNav";
import { Toolbar } from "./components/Toolbar";
import { dfuMsg } from "./link/protocol";
import type { PedalSession } from "./link/session";
import { NAME_MAX, sanitiseName } from "./name";
import { errorText, usePedal } from "./usePedal";

type TabId = "presets" | "eq" | "log";
const TABS: readonly NavTab<TabId>[] = [
  { id: "presets", label: "Presets" },
  { id: "eq", label: "EQ" },
  { id: "log", label: "Log" },
];
const OK = 0;

export function App() {
  const { s, link, serial, refreshSlots } = usePedal();
  const [tab, setTab] = useState<TabId>("presets");
  const [busy, setBusy] = useState<string | null>(null);
  const busyRef = useRef(false);
  const [status, setStatus] = useState<Status>(NO_STATUS);
  const [selected, setSelected] = useState(0);

  const linked = s.link.linked;
  const ready = linked && s.synced && busy === null;
  const playing = s.status ? s.status.slot : (s.slots?.find((x) => x.playing)?.slot ?? null);

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
  /** A slot change, then LIST and the infos again whatever its outcome: the table shows what the flash holds. */
  const slotChange = (label: string, fn: (session: PedalSession) => Promise<string>) =>
    act(label, () =>
      serial(async (session) => {
        let text: string;
        try {
          text = await fn(session);
        } catch (e) {
          await refreshSlots(session).catch(() => undefined);
          throw e;
        }
        await refreshSlots(session);
        return text;
      }),
    );

  const onUpload = (slot: number, image: Uint8Array, name: string) =>
    void slotChange("UPLOADING… 0 %", async (session) => {
      const r = await session.upload(slot, image, (sent, size) =>
        setBusy(`UPLOADING… ${Math.floor((100 * sent) / size)} %`),
      );
      return `Uploaded "${name}" to slot ${slot + 1} (flash ${r.flashMs} ms)`;
    });
  const onRename = (slot: number, input: string) => {
    const name = sanitiseName(input.trim());
    if (name === "") {
      setStatus({ text: `A name needs 1-${NAME_MAX} characters`, error: true });
      return;
    }
    if (name === s.slots?.[slot]?.name) return;
    void slotChange("RENAMING…", async (session) => {
      await session.rename(slot, name);
      return `Renamed slot ${slot + 1} to "${name}"`;
    });
  };
  const onSwap = (a: number, b: number) =>
    void slotChange("SWAPPING…", async (session) => {
      await session.swap(a, b);
      setSelected(b);
      return `Swapped slots ${Math.min(a, b) + 1} and ${Math.max(a, b) + 1}`;
    });
  const onErase = (slot: number) => {
    const sm = s.slots?.[slot];
    const question = sm?.status === OK ? `Erase slot ${slot + 1} "${sm.name}"?` : `Erase slot ${slot + 1}?`;
    if (!window.confirm(question)) return;
    void slotChange("ERASING…", async (session) => {
      await session.erase(slot);
      return `Erased slot ${slot + 1}`;
    });
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
  const info = s.infos[selected] ?? null;
  const presets = !linked ? (
    <p class="tab-empty">Waiting for the pedal.</p>
  ) : s.slots === null ? (
    <p class="tab-empty">{s.syncError ?? "Reading the pedal…"}</p>
  ) : (
    <>
      <SlotTable
        slots={s.slots}
        playing={playing}
        selected={selected}
        onSelect={setSelected}
        enabled={ready}
        onSwap={onSwap}
        onRename={onRename}
        onErase={onErase}
      />
      {sel?.status === OK && (
        <GroupBox title={`Slot ${selected + 1}`}>
          {info ? <Details model={info.model} ir={info.ir} /> : <p class="tab-empty">Reading…</p>}
        </GroupBox>
      )}
      <ImportBox slots={s.slots} selected={selected} enabled={ready} onUpload={onUpload} />
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
