import { useState } from "preact/hooks";
import { MAX_NAME, SLOT_STATUS_NAMES, type SlotMessage } from "../link/protocol";
import { GroupBox } from "./GroupBox";
import { Lamp } from "./Lamp";

interface Props {
  slots: readonly SlotMessage[];
  /** The playing slot, null when dry or unknown. */
  playing: number | null;
  selected: number;
  onSelect: (slot: number) => void;
  /** Slot actions are possible: synced and nothing busy. */
  enabled: boolean;
  onSwap: (a: number, b: number) => void;
  onRename: (slot: number, name: string) => void;
  onErase: (slot: number) => void;
}

const OK = 0;
const EMPTY = 1;

/** A slot may take part in a SWAP: the pedal refuses a slot that is neither ok nor empty. */
const swappable = (s: SlotMessage | undefined): s is SlotMessage =>
  s !== undefined && (s.status === OK || s.status === EMPTY);

/**
 * The 8 slots: the playing lamp, the preset, model and IR names, and the per-slot actions. Clicking a row selects it;
 * the slot number is the selection's keyboard button.
 */
export function SlotTable({ slots, playing, selected, onSelect, enabled, onSwap, onRename, onErase }: Props) {
  const [renaming, setRenaming] = useState<{ slot: number; text: string } | null>(null);
  const canSwap = (a: number, b: number): boolean => {
    const sa = slots[a];
    const sb = slots[b];
    return enabled && swappable(sa) && swappable(sb) && !(sa.status === EMPTY && sb.status === EMPTY);
  };
  const commit = () => {
    if (renaming) onRename(renaming.slot, renaming.text);
    setRenaming(null);
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
            const ok = s.status === OK;
            const state = ok ? "ok" : s.status === EMPTY ? "empty" : "corrupt";
            const editing = renaming?.slot === k;
            return (
              <tr
                key={k}
                class="slot-row"
                aria-selected={k === selected}
                data-selected={k === selected}
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
                {editing ? (
                  <td class="slot-rename">
                    <input
                      type="text"
                      aria-label={`New name for slot ${n}`}
                      maxLength={MAX_NAME}
                      value={renaming.text}
                      // biome-ignore lint/a11y/noAutofocus: the field opens on the Rename button's press
                      autoFocus
                      onInput={(e) => setRenaming({ slot: k, text: e.currentTarget.value })}
                      onKeyDown={(e) => {
                        if (e.key === "Enter") commit();
                        else if (e.key === "Escape") setRenaming(null);
                      }}
                    />
                    <button type="button" class="btn small" onClick={commit}>
                      OK
                    </button>
                    <button type="button" class="btn small" onClick={() => setRenaming(null)}>
                      Cancel
                    </button>
                  </td>
                ) : (
                  <td class="slot-cell" data-state={state} title={ok ? s.name : undefined}>
                    {ok ? s.name : state === "empty" ? "Empty" : `Corrupt (${SLOT_STATUS_NAMES[s.status] ?? s.status})`}
                  </td>
                )}
                <td class="slot-cell" title={ok ? s.model : undefined}>
                  {ok ? s.model : ""}
                </td>
                <td class="slot-cell" title={ok && s.hasIr ? s.ir : undefined}>
                  {ok ? (s.hasIr ? s.ir : "—") : ""}
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
                    aria-label={`Rename slot ${n}`}
                    disabled={!enabled || !ok || editing}
                    onClick={() => setRenaming({ slot: k, text: s.name })}
                  >
                    Rename
                  </button>
                  <button
                    type="button"
                    class="btn small"
                    aria-label={`Erase slot ${n}`}
                    disabled={!enabled || s.status === EMPTY}
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
    </GroupBox>
  );
}
