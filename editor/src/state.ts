// The editor's view of the pedal: what its messages and the sync leave behind (App's reducer).
import type { IdentityMessage, PedalMessage, SlotMessage, StatusMessage } from "./link/protocol";
import { type LinkSnapshot, NO_LINK } from "./midi";
import type { SlotInfo } from "./preset/info";

const LOG_LINES = 1000;
const SLOTS = 8;

export interface LogLine {
  t: number;
  text: string;
}

/** STATUS's load, in permille, and its underrun total. */
export interface PedalLoad {
  avg: number;
  max: number;
  underruns: number;
}

export interface State {
  link: LinkSnapshot;
  identity: IdentityMessage | null;
  /** Identity, LIST, slot infos and WATCH done since the last (re)link or boot line. */
  synced: boolean;
  /** The last sync's failure, shown until the next sync succeeds. */
  syncError: string | null;
  slots: SlotMessage[] | null;
  /** Per slot: its info when its status is ok and it has been read; else null. */
  infos: readonly (SlotInfo | null)[];
  status: StatusMessage | null;
  load: PedalLoad | null;
  /** The underrun total grew at the last STATUS. */
  xrun: boolean;
  log: LogLine[];
}

export type Action =
  | { type: "link"; snap: LinkSnapshot }
  | { type: "msg"; msg: PedalMessage; now: number }
  | { type: "identity"; identity: IdentityMessage }
  | { type: "slots"; slots: SlotMessage[] }
  | { type: "info"; slot: number; info: SlotInfo }
  | { type: "synced" }
  | { type: "syncError"; text: string };

const NO_INFOS: readonly (SlotInfo | null)[] = Array(SLOTS).fill(null);

export const initial: State = {
  link: NO_LINK,
  identity: null,
  synced: false,
  syncError: null,
  slots: null,
  infos: NO_INFOS,
  status: null,
  load: null,
  xrun: false,
  log: [],
};

function applyMessage(s: State, m: PedalMessage, now: number): State {
  switch (m.type) {
    case "log":
      return { ...s, log: [...s.log, { t: now, text: m.text }].slice(-LOG_LINES) };
    case "status":
      return {
        ...s,
        status: m,
        load: { avg: m.loadAvg, max: m.loadMax, underruns: m.underruns },
        xrun: s.status !== null && m.underruns > s.status.underruns,
      };
    default:
      return s; // the session's replies: the sync and App's actions dispatch their results
  }
}

export function reducer(s: State, a: Action): State {
  switch (a.type) {
    case "link":
      // Offline the page keeps its last values; a new link syncs again.
      return a.snap.linked ? { ...s, link: a.snap } : { ...s, link: a.snap, synced: false };
    case "msg":
      return applyMessage(s, a.msg, a.now);
    case "identity":
      return { ...s, identity: a.identity };
    case "slots":
      return { ...s, slots: a.slots, infos: NO_INFOS };
    case "info": {
      const infos = s.infos.slice();
      infos[a.slot] = a.info;
      return { ...s, infos };
    }
    case "synced":
      return { ...s, synced: true, syncError: null };
    case "syncError":
      return { ...s, syncError: a.text };
  }
}
