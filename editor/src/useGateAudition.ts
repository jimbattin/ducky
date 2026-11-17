// GATE TRY auditions: the gate box's changes heard at once on the playing slot, coalesced to the latest value with
// one request in flight, and sent again when a preset switch or a resync reloads the stored gate.
import { useEffect, useRef } from "preact/hooks";
import type { Drafts } from "./draft";
import type { GateSettings } from "./preset/gate";
import type { State } from "./state";
import type { Pedal } from "./usePedal";

export function useGateAudition(
  s: State,
  playing: number | null,
  drafts: Drafts,
  serial: Pedal["serial"],
): {
  /** Auditions a gate on the pedal when the slot is the playing one. */
  tryGate: (slot: number, gate: GateSettings) => void;
  /** A dropped draft gate on the playing slot: the pedal goes back to the stored one. */
  restoreGate: (slot: number) => void;
} {
  // One serial job sends the latest pending settings until none is left.
  const pending = useRef<{ slot: number; gate: GateSettings } | null>(null);
  const sending = useRef(false);
  const pushTry = (slot: number, gate: GateSettings) => {
    pending.current = { slot, gate };
    if (sending.current) return;
    sending.current = true;
    serial(async (session) => {
      for (let p = pending.current; p !== null; p = pending.current) {
        pending.current = null;
        // slot-state after an FS2 press moved playback elsewhere is expected: the pedal plays the stored gate.
        await session.gateTry(p.slot, p.gate).catch(() => undefined);
      }
    })
      .catch(() => undefined)
      .finally(() => {
        sending.current = false;
      });
  };
  const tryGate = (slot: number, gate: GateSettings) => {
    if (slot === playing && s.link.linked && s.synced) pushTry(slot, gate);
  };
  const restoreGate = (slot: number) => {
    const stored = s.infos[slot]?.gate;
    if (drafts[slot]?.gate && stored) tryGate(slot, stored);
  };
  // A preset switch or a resync reloads the stored gate on the pedal: an unsaved draft gate is auditioned again.
  // biome-ignore lint/correctness/useExhaustiveDependencies: only a new playing slot or a resync re-sends the draft
  useEffect(() => {
    const g = playing === null ? null : drafts[playing]?.gate;
    if (playing !== null && g) tryGate(playing, g);
  }, [s.synced, playing]);
  return { tryGate, restoreGate };
}
