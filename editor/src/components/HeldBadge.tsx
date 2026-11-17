/** Marks a held control: its parameter was set elsewhere and waits for the control to move (docs/MIDI.md "Controls"). */
export function HeldBadge({ knob }: { knob: boolean }) {
  return (
    <span
      class="badge"
      title={`Set from the editor or another control: takes over when this ${knob ? "knob" : "switch"} moves`}
    >
      Held
    </span>
  );
}
