// SETs from the Compressor, EQ and Loop tabs and the Morph box: a parameter's value heard while its slider or graph
// handle moves, coalesced to the latest code per parameter with one request in flight, and shown at once (the pedal's
// VALUES arrive every 100 ms).
import { useState } from "preact/hooks";
import type { ParamInfo, ValuesMessage } from "./link/protocol";
import type { Action, State } from "./state";
import { useCoalesced } from "./useCoalesced";
import type { Pedal } from "./usePedal";

/** A code set here is shown for this long before the pedal's VALUES take over again. */
const LOCAL_HOLD_MS = 250;

/** The pedal's parameter table, its live assignments and values, and the code shown for each parameter. */
export interface ParamTable {
  params: readonly ParamInfo[];
  values: ValuesMessage;
  /** Per parameter id: a code just set here, else the pedal's. */
  codes: readonly number[];
}

export function useParamEdits(
  s: State,
  serial: Pedal["serial"],
  dispatch: (a: Action) => void,
): {
  /** Sets a parameter on the pedal (SET). */
  setParam: (id: number, code: number) => void;
  /** null until DUMP has been read. */
  table: ParamTable | null;
} {
  const [local, setLocal] = useState<ReadonlyMap<number, { code: number; t: number }>>(new Map());
  // A failed SET (the link dropped) is dropped: the pedal's VALUES show what it holds.
  const push = useCoalesced<number, number>(serial, (session, id, code) =>
    session.set(id, code).then(() => dispatch({ type: "settingsEdited", edited: true })),
  );
  const setParam = (id: number, code: number) => {
    if (!s.link.linked || !s.synced) return;
    setLocal((m) => new Map(m).set(id, { code, t: Date.now() }));
    push(id, code);
  };
  const { params, values } = s;
  if (params === null || values === null) return { setParam, table: null };
  const now = Date.now();
  const codes = params.map((p) => {
    const set = local.get(p.id);
    return set !== undefined && now - set.t < LOCAL_HOLD_MS ? set.code : (values.codes[p.id] ?? p.defCode);
  });
  return { setParam, table: { params, values, codes } };
}
