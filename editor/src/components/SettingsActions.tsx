interface Props {
  /** The pedal takes requests. */
  enabled: boolean;
  /** An assignment or a value set here is not saved yet. */
  edited: boolean;
  onSave: () => void;
}

/** The settings record's state and Save (SAVE: the assignments and the values for the next power-up). */
export function SettingsActions({ enabled, edited, onSave }: Props) {
  return (
    <div class="edit-actions">
      <span class="edit-count">{edited ? "Not saved on the pedal" : "No unsaved changes"}</span>
      <button type="button" class="btn" disabled={!enabled || !edited} onClick={onSave}>
        Save
      </button>
    </div>
  );
}
