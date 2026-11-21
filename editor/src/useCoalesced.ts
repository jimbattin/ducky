// Requests that follow a control while it moves (GATE TRY, SET): each key's latest value, one request in flight.
import { useRef } from "preact/hooks";
import type { PedalSession } from "./link/session";
import type { Pedal } from "./usePedal";

/**
 * A sender of values by key through serial: one serial job sends each key's latest value until none is pending, so a
 * value pushed while a request is in flight replaces the key's pending one. A failed send is dropped (the pedal's
 * stream shows what it holds). send must not read render state: the job keeps the send of the push that started it.
 */
export function useCoalesced<K, V>(
  serial: Pedal["serial"],
  send: (session: PedalSession, key: K, value: V) => Promise<unknown>,
): (key: K, value: V) => void {
  const pending = useRef(new Map<K, V>());
  const sending = useRef(false);
  return (key, value) => {
    pending.current.set(key, value);
    if (sending.current) return;
    sending.current = true;
    serial(async (session) => {
      for (let next = pending.current.entries().next(); !next.done; next = pending.current.entries().next()) {
        const [k, v] = next.value;
        pending.current.delete(k);
        await send(session, k, v).catch(() => undefined);
      }
    })
      .catch(() => undefined)
      .finally(() => {
        sending.current = false;
      });
  };
}
