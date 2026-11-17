// The pedal link's lifecycle: the reducer state it feeds, the sync on (re)link with its retries, and the one-at-a-time
// queue every request goes through.
import { useEffect, useReducer, useRef, useState } from "preact/hooks";
import { errorText } from "./format";
import { SLOT_STATUS } from "./link/protocol";
import type { PedalSession } from "./link/session";
import { PedalLink } from "./midi";
import { type Action, initial, reducer, type State } from "./state";

/** A sync that has not completed this long after it started is started again. */
const RETRY_MS = 3000;
/** A boot line this soon after a sync started does not start another (HELLO's answer starts with one). */
const SYNC_GAP_MS = 500;
/** The pedal's boot line, also HELLO's first answer: after a restart its WATCH is off and its slots may differ. */
const BOOT_LINE = /^ducky /;

export interface Pedal {
  s: State;
  link: PedalLink;
  /** Runs fn with the session after every earlier request has settled: the pedal answers one request at a time. */
  serial: <T>(fn: (session: PedalSession) => Promise<T>) => Promise<T>;
  /** LIST, then the info of every ok slot, into the state. Call inside serial. */
  refreshSlots: (session: PedalSession) => Promise<void>;
}

interface Ops {
  serial: Pedal["serial"];
  refreshSlots: Pedal["refreshSlots"];
  /** Identity, the slots, WATCH on; unless one is running. */
  sync: () => void;
  /** Date.now() of the last sync's start. */
  lastSync: () => number;
  syncing: () => boolean;
}

function makeOps(link: PedalLink, dispatch: (a: Action) => void): Ops {
  let tail: Promise<unknown> = Promise.resolve();
  let syncing = false;
  let lastSync = 0;
  const serial = <T>(fn: (session: PedalSession) => Promise<T>): Promise<T> => {
    const run = tail.then(() => fn(link.session));
    tail = run.catch(() => undefined);
    return run;
  };
  const refreshSlots = async (session: PedalSession): Promise<void> => {
    const slots = await session.list();
    dispatch({ type: "slots", slots });
    for (const s of slots)
      if (s.status === SLOT_STATUS.ok) dispatch({ type: "info", slot: s.slot, info: await session.readInfo(s.slot) });
  };
  const sync = (): void => {
    if (syncing) return;
    syncing = true;
    lastSync = Date.now();
    serial(async (session) => {
      dispatch({ type: "identity", identity: await session.identity() });
      await refreshSlots(session);
      await session.watch(true);
      dispatch({ type: "synced" });
    })
      .catch((e: unknown) => dispatch({ type: "syncError", text: errorText(e) }))
      .finally(() => {
        syncing = false;
      });
  };
  return { serial, refreshSlots, sync, lastSync: () => lastSync, syncing: () => syncing };
}

export function usePedal(): Pedal {
  const [link] = useState(() => new PedalLink());
  const [s, dispatch] = useReducer(reducer, initial);
  const [ops] = useState(() => makeOps(link, dispatch));
  const sRef = useRef(s);
  sRef.current = s;

  useEffect(() => {
    const offChange = link.onChange((snap) => dispatch({ type: "link", snap }));
    const offMsg = link.onMessage((msg) => {
      dispatch({ type: "msg", msg, now: Date.now() });
      if (msg.type === "log" && BOOT_LINE.test(msg.text) && Date.now() - ops.lastSync() >= SYNC_GAP_MS) ops.sync();
    });
    void link.start();
    const timer = setInterval(() => {
      const cur = sRef.current;
      if (cur.link.linked && !cur.synced && !ops.syncing() && Date.now() - ops.lastSync() >= RETRY_MS) ops.sync();
    }, 250);
    return () => {
      offChange();
      offMsg();
      clearInterval(timer);
      link.stop();
    };
  }, [link, ops]);

  const linked = s.link.linked;
  useEffect(() => {
    if (linked) ops.sync();
  }, [ops, linked]);

  return { s, link, serial: ops.serial, refreshSlots: ops.refreshSlots };
}
