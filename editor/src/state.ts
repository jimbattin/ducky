// The editor's view of the pedal: what its messages and the sync leave behind (App's reducer).
import type {
  IdentityMessage,
  ParamInfo,
  PedalMessage,
  SlotMessage,
  StatusMessage,
  ValuesMessage,
} from "./link/protocol";
import { type LinkSnapshot, NO_LINK } from "./midi";
import { SLOT_COUNT } from "./preset/bank";
import type { SlotInfo } from "./preset/info";

const LOG_LINES = 1000;
/** The pedal's boot line, also HELLO's first answer: after a restart its WATCH is off and its slots may differ. */
export const BOOT_LINE = /^ducky /;

export interface LogLine {
  t: number;
  text: string;
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
  /** The underrun total grew at the last STATUS. */
  xrun: boolean;
  log: LogLine[];
  /** The pedal's parameter table (DUMP), by id; null until read. */
  params: ParamInfo[] | null;
  /** Why the table could not be read (older firmware). */
  paramsError: string | null;
  /** The live assignments and values: DUMP's, then each VALUES of the stream. */
  values: ValuesMessage | null;
  /** An assignment or a value was set from the editor since the pedal booted or its settings were saved. */
  settingsEdited: boolean;
}

export type Action =
  | { type: "link"; snap: LinkSnapshot }
  | { type: "msg"; msg: PedalMessage; now: number }
  | { type: "identity"; identity: IdentityMessage }
  | { type: "slots"; slots: SlotMessage[] }
  | { type: "info"; slot: number; info: SlotInfo }
  | { type: "synced" }
  | { type: "syncError"; text: string }
  | { type: "params"; params: ParamInfo[]; values: ValuesMessage }
  | { type: "paramsError"; text: string }
  | { type: "settingsEdited"; edited: boolean };

const NO_INFOS: readonly (SlotInfo | null)[] = Array(SLOT_COUNT).fill(null);

export const initial: State = {
  link: NO_LINK,
  identity: null,
  synced: false,
  syncError: null,
  slots: null,
  infos: NO_INFOS,
  status: null,
  xrun: false,
  log: [],
  params: null,
  paramsError: null,
  values: null,
  settingsEdited: false,
};

function applyMessage(s: State, m: PedalMessage, now: number): State {
  switch (m.type) {
    case "log":
      return {
        ...s,
        log: [...s.log, { t: now, text: m.text }].slice(-LOG_LINES),
        // A restarted pedal is back on its saved settings.
        settingsEdited: s.settingsEdited && !BOOT_LINE.test(m.text),
      };
    case "values":
      return { ...s, values: m };
    case "status":
      return { ...s, status: m, xrun: s.status !== null && m.underruns > s.status.underruns };
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
    case "params":
      return { ...s, params: a.params, values: a.values, paramsError: null };
    case "paramsError":
      return { ...s, paramsError: a.text };
    case "settingsEdited":
      return a.edited === s.settingsEdited ? s : { ...s, settingsEdited: a.edited };
  }
}
