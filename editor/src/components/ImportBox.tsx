import { useEffect, useRef, useState } from "preact/hooks";
import { importWarnings, loadIr, loadModel, rejectionText } from "../importer";
import type { ConvertedIr } from "../ir/convert";
import { MAX_NAME, SLOT_STATUS_NAMES, type SlotMessage } from "../link/protocol";
import type { ConvertedModel } from "../nam/convert";
import { buildSlot } from "../preset/bank";
import { Details } from "./Details";
import { GroupBox } from "./GroupBox";
import { Lamp } from "./Lamp";

interface Props {
  slots: readonly SlotMessage[];
  /** The selected slot: the target until the user picks another. */
  selected: number;
  /** Uploading is possible: synced and nothing busy. */
  enabled: boolean;
  onUpload: (slot: number, image: Uint8Array, name: string) => void;
}

const OK = 0;

const slotLabel = (s: SlotMessage): string =>
  `Slot ${s.slot + 1}: ${s.status === OK ? s.name : s.status === 1 ? "Empty" : `Corrupt (${SLOT_STATUS_NAMES[s.status] ?? s.status})`}`;

/** Stages a .nam model and an optional .wav IR, shows what the pedal will make of them, and uploads them to a slot. */
export function ImportBox({ slots, selected, enabled, onUpload }: Props) {
  const [model, setModel] = useState<ConvertedModel | null>(null);
  const [ir, setIr] = useState<ConvertedIr | null>(null);
  const [name, setName] = useState("");
  const [target, setTarget] = useState(selected);
  const [error, setError] = useState<string | null>(null);
  const irInput = useRef<HTMLInputElement>(null);

  useEffect(() => setTarget(selected), [selected]);

  const onModelFile = async (file: File | undefined) => {
    setError(null);
    if (!file) return setModel(null);
    try {
      const m = await loadModel(file);
      setModel(m);
      setName(m.info.name);
    } catch (e) {
      setModel(null);
      setError(rejectionText(e));
    }
  };
  const onIrFile = async (file: File | undefined) => {
    setError(null);
    if (!file) return setIr(null);
    try {
      setIr(await loadIr(file));
    } catch (e) {
      setIr(null);
      setError(rejectionText(e));
    }
  };
  const clearIr = () => {
    setIr(null);
    if (irInput.current) irInput.current.value = "";
  };

  const trimmed = name.trim();
  const dest = slots[target];
  const upload = () => {
    if (!model) return;
    if (dest?.status === OK && !window.confirm(`Replace slot ${target + 1} "${dest.name}"?`)) return;
    try {
      onUpload(target, buildSlot(model.bin, ir?.bin ?? null, trimmed), trimmed);
    } catch (e) {
      setError(rejectionText(e));
    }
  };
  const warnings = model ? importWarnings(model.info, ir?.info ?? null) : [];

  return (
    <GroupBox title="Import">
      <div class="import-form">
        <label for="import-model">Model (.nam)</label>
        <div>
          <input
            id="import-model"
            type="file"
            accept=".nam"
            onChange={(e) => void onModelFile(e.currentTarget.files?.[0])}
          />
        </div>
        <label for="import-ir">IR (.wav)</label>
        <div>
          <input
            id="import-ir"
            ref={irInput}
            type="file"
            accept=".wav"
            onChange={(e) => void onIrFile(e.currentTarget.files?.[0])}
          />
          <button type="button" class="btn small" disabled={ir === null} onClick={clearIr}>
            No IR
          </button>
        </div>
        <label for="import-name">Name</label>
        <div>
          <input
            id="import-name"
            type="text"
            maxLength={MAX_NAME}
            value={name}
            onInput={(e) => setName(e.currentTarget.value)}
          />
        </div>
        <label for="import-target">Target</label>
        <div>
          <select id="import-target" value={target} onChange={(e) => setTarget(Number(e.currentTarget.value))}>
            {slots.map((s) => (
              <option key={s.slot} value={s.slot}>
                {slotLabel(s)}
              </option>
            ))}
          </select>
        </div>
      </div>
      {error !== null && (
        <p class="import-error" role="alert">
          {error}
        </p>
      )}
      {model && <Details model={model.info} ir={ir?.info ?? null} />}
      {warnings.length > 0 && (
        <ul class="import-warnings">
          {warnings.map((w) => (
            <li key={w}>
              <Lamp color="amber" />
              {w}
            </li>
          ))}
        </ul>
      )}
      <div class="import-actions">
        <button type="button" class="btn" disabled={!enabled || !model || trimmed === ""} onClick={upload}>
          Upload to slot {target + 1}
        </button>
      </div>
    </GroupBox>
  );
}
