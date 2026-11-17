import type { SlotView } from "../draft";
import { SLOT_STATUS, type SlotMessage } from "../link/protocol";
import { GroupBox } from "./GroupBox";
import { Lamp } from "./Lamp";

interface Props {
  slots: readonly SlotMessage[];
  /** Per slot: what it shows, the unsaved draft's values when it has one. */
  views: readonly SlotView[];
  /** The playing slot, null when dry or unknown. */
  playing: number | null;
  selected: number;
  onSelect: (slot: number) => void;
  /** Slot actions are possible: synced and nothing busy. */
  enabled: boolean;
  onSwap: (a: number, b: number) => void;
  onErase: (slot: number) => void;
  /** Slots with unsaved changes. */
  edits: number;
  onSave: () => void;
  onDiscard: () => void;
}

/** A slot may take part in a SWAP: the pedal refuses a slot that is neither ok nor empty. */
const swappable = (s: SlotMessage | undefined): s is SlotMessage =>
  s !== undefined && (s.status === SLOT_STATUS.ok || s.status === SLOT_STATUS.empty);

/**
 * The 8 slots: the playing lamp, the preset, model and IR names (unsaved changes in amber), the per-slot actions, and
 * Save / Discard for every unsaved change. Clicking a row selects it; the slot number is the selection's keyboard
 * button.
 */
export function SlotTable({
  slots,
  views,
  playing,
  selected,
  onSelect,
  enabled,
  onSwap,
  onErase,
  edits,
  onSave,
  onDiscard,
}: Props) {
  const canSwap = (a: number, b: number): boolean => {
    const sa = slots[a];
    const sb = slots[b];
    return (
      enabled && swappable(sa) && swappable(sb) && !(sa.status === SLOT_STATUS.empty && sb.status === SLOT_STATUS.empty)
    );
  };

  return (
    <GroupBox title="Slots">
      <table class="slots">
        <thead>
          <tr class="slot-row slot-head">
            <th>
              <span class="visually-hidden">Playing</span>
            </th>
            <th>#</th>
            <th>Preset</th>
            <th>Model</th>
            <th>IR</th>
            <th>
              <span class="visually-hidden">Actions</span>
            </th>
          </tr>
        </thead>
        <tbody>
          {slots.map((s) => {
            const k = s.slot;
            const n = k + 1;
            const v = views[k]!;
            return (
              <tr
                key={k}
                class="slot-row"
                data-selected={k === selected}
                data-edited={v.edited}
                onClick={() => onSelect(k)}
              >
                <td>
                  <Lamp color={k === playing ? "green" : null} />
                  {k === playing && <span class="visually-hidden">Playing</span>}
                </td>
                <td>
                  <button
                    type="button"
                    class="slot-num"
                    aria-label={`Select slot ${n}`}
                    aria-pressed={k === selected}
                    onClick={() => onSelect(k)}
                  >
                    {n}
                  </button>
                </td>
                <td class="slot-cell" data-state={v.state} title={v.state === "ok" ? v.name : undefined}>
                  {v.name}
                  {v.edited && <span class="visually-hidden"> (unsaved)</span>}
                </td>
                <td class="slot-cell" title={v.model || undefined}>
                  {v.model}
                </td>
                <td class="slot-cell" title={v.ir || undefined}>
                  {v.ir}
                </td>
                <td class="slot-buttons">
                  <button
                    type="button"
                    class="btn small"
                    aria-label={`Move slot ${n} up`}
                    title="Swap with the slot above"
                    disabled={k === 0 || !canSwap(k, k - 1)}
                    onClick={() => onSwap(k, k - 1)}
                  >
                    ↑
                  </button>
                  <button
                    type="button"
                    class="btn small"
                    aria-label={`Move slot ${n} down`}
                    title="Swap with the slot below"
                    disabled={k === slots.length - 1 || !canSwap(k, k + 1)}
                    onClick={() => onSwap(k, k + 1)}
                  >
                    ↓
                  </button>
                  <button
                    type="button"
                    class="btn small"
                    aria-label={`Erase slot ${n}`}
                    disabled={!enabled || s.status === SLOT_STATUS.empty}
                    onClick={() => onErase(k)}
                  >
                    Erase
                  </button>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <div class="edit-actions">
        <span class="edit-count">
          {edits === 0 ? "No unsaved changes" : `${edits} slot${edits === 1 ? "" : "s"} changed`}
        </span>
        <button type="button" class="btn" disabled={edits === 0} onClick={onDiscard}>
          Discard
        </button>
        <button type="button" class="btn" disabled={!enabled || edits === 0} onClick={onSave}>
          Save
        </button>
      </div>
    </GroupBox>
  );
}
