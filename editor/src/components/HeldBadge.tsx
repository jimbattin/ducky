import type { Control } from "../link/params";

const NOUN: Record<Control["kind"], string> = { knob: "knob", switch: "switch", expression: "expression pedal" };

/** Marks a held control: its parameter was set elsewhere and waits for the control to move (docs/MIDI.md "Controls"). */
export function HeldBadge({ kind }: { kind: Control["kind"] }) {
  return (
    <span class="badge" title={`Set from the editor or another control: takes over when this ${NOUN[kind]} moves`}>
      Held
    </span>
  );
}
