import type { ParamTable } from "../useParamEdits";

/** What the Compressor, EQ and Loop tabs and the Morph box take: the table, SET and the settings record's Save. */
export interface SettingsProps {
  table: ParamTable;
  /** The pedal takes requests. */
  enabled: boolean;
  /** An assignment or a value set on the pedal is not saved yet. */
  edited: boolean;
  onSet: (id: number, code: number) => void;
  onSave: () => void;
}

/** The settings record's state and Save (SAVE: the assignments and the values for the next power-up). */
export function SettingsActions({ enabled, edited, onSave }: Pick<SettingsProps, "enabled" | "edited" | "onSave">) {
  return (
    <div class="edit-actions">
      <span class="edit-count">{edited ? "Not saved on the pedal" : "No unsaved changes"}</span>
      <button type="button" class="btn" disabled={!enabled || !edited} onClick={onSave}>
        Save
      </button>
    </div>
  );
}
