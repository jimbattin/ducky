// SETs from the EQ tab: a parameter's value heard while its slider or graph handle moves, coalesced to the latest
// code per parameter with one request in flight, and shown at once (the pedal's VALUES arrive every 100 ms).
import { useRef, useState } from "preact/hooks";
import type { Action, State } from "./state";
import type { Pedal } from "./usePedal";

/** A code set here is shown for this long before the pedal's VALUES take over again. */
const LOCAL_HOLD_MS = 250;

export function useParamEdits(
  s: State,
  serial: Pedal["serial"],
  dispatch: (a: Action) => void,
): {
  /** Sets a parameter on the pedal (SET). */
  setParam: (id: number, code: number) => void;
  /** The code to show for a parameter: one just set here, else the pedal's; undefined before the first VALUES. */
  shownCode: (id: number) => number | undefined;
} {
  // One serial job sends each parameter's latest pending code until none is left.
  const pending = useRef(new Map<number, number>());
  const sending = useRef(false);
  const [local, setLocal] = useState<ReadonlyMap<number, { code: number; t: number }>>(new Map());
  const setParam = (id: number, code: number) => {
    if (!s.link.linked || !s.synced) return;
    setLocal((m) => new Map(m).set(id, { code, t: Date.now() }));
    pending.current.set(id, code);
    if (sending.current) return;
    sending.current = true;
    serial(async (session) => {
      for (let next = pending.current.entries().next(); !next.done; next = pending.current.entries().next()) {
        const [nextId, nextCode] = next.value;
        pending.current.delete(nextId);
        // A failed SET (the link dropped) leaves the pedal's VALUES to show what it holds.
        await session.set(nextId, nextCode).then(
          () => dispatch({ type: "settingsEdited", edited: true }),
          () => undefined,
        );
      }
    })
      .catch(() => undefined)
      .finally(() => {
        sending.current = false;
      });
  };
  const shownCode = (id: number): number | undefined => {
    const set = local.get(id);
    return set !== undefined && Date.now() - set.t < LOCAL_HOLD_MS ? set.code : s.values?.codes[id];
  };
  return { setParam, shownCode };
}
