// The pedal's USB MIDI link in the browser: Web MIDI ports, SysEx reassembly, the HELLO keep-alive, the link status,
// and a Transport for the request/reply session (link/session.ts).
import { helloMsg, type PedalMessage, parseMessage, SysExAssembler } from "./link/protocol";
import { PedalSession, type Transport } from "./link/session";

export interface PortInfo {
  id: string;
  name: string;
}
export interface LinkSnapshot {
  status: string;
  linked: boolean;
  inputs: PortInfo[];
  outputs: PortInfo[];
  inputId: string;
  outputId: string;
}

export const NO_RESPONSE = "NO RESPONSE";
/** The snapshot before start(): no MIDI access yet. */
export const NO_LINK: LinkSnapshot = {
  status: "CONNECTING…",
  linked: false,
  inputs: [],
  outputs: [],
  inputId: "",
  outputId: "",
};

// A slot write (COMMIT, ERASE, RENAME, SWAP) blocks the pedal's main loop for up to about a second; WATCH's STATUS
// every 100 ms keeps the link fresh otherwise.
const RX_TIMEOUT_MS = 4000;
const HELLO_PERIOD_MS = 1000;
const MAX_FRAME = 4096;
/** Messages kept for the session between its requests (the oldest go first). */
const QUEUE_MAX = 256;

export class PedalLink {
  private access: MIDIAccess | null = null;
  private input: MIDIInput | null = null;
  private output: MIDIOutput | null = null;
  private status = NO_LINK.status;
  private lastRx = 0;
  private lastHello = 0;
  private timer: ReturnType<typeof setInterval> | undefined;
  private stopped = false;
  private msgListeners = new Set<(m: PedalMessage) => void>();
  private snapListeners = new Set<(s: LinkSnapshot) => void>();
  private assembler = new SysExAssembler(MAX_FRAME);
  private inputPref = "";
  private outputPref = "";
  private queue: Uint8Array[] = [];
  private waiter: ((m: Uint8Array | null) => void) | null = null;

  /**
   * Every complete message, raw, for the session: a send drops what is queued (stale replies), a receive takes the
   * oldest or waits for the next. One request at a time (usePedal's serial), so one waiter at most.
   */
  readonly transport: Transport = {
    send: async (msg) => {
      this.queue = [];
      this.send(msg);
    },
    receive: (timeoutMs) => {
      const head = this.queue.shift();
      if (head) return Promise.resolve(head);
      return new Promise((resolve) => {
        const timer = setTimeout(
          () => {
            this.waiter = null;
            resolve(null);
          },
          Math.max(0, timeoutMs),
        );
        this.waiter = (m) => {
          clearTimeout(timer);
          this.waiter = null;
          resolve(m);
        };
      });
    },
  };
  /** LOG lines reach onMessage's listeners, so the session has no onLog. */
  readonly session = new PedalSession(this.transport);

  onMessage(f: (m: PedalMessage) => void): () => void {
    this.msgListeners.add(f);
    return () => this.msgListeners.delete(f);
  }
  onChange(f: (s: LinkSnapshot) => void): () => void {
    this.snapListeners.add(f);
    return () => this.snapListeners.delete(f);
  }

  get linked(): boolean {
    return !!this.input && !!this.output && Date.now() - this.lastRx < RX_TIMEOUT_MS;
  }

  private snapshot(): LinkSnapshot {
    return {
      status: this.status,
      linked: this.linked,
      inputs: this.access ? [...this.access.inputs.values()].map(portInfo) : [],
      outputs: this.access ? [...this.access.outputs.values()].map(portInfo) : [],
      inputId: this.input?.id ?? "",
      outputId: this.output?.id ?? "",
    };
  }

  async start(): Promise<void> {
    if (!navigator.requestMIDIAccess) {
      this.setStatus(
        window.isSecureContext ? "WEB MIDI UNAVAILABLE - USE CHROME" : "WEB MIDI NEEDS HTTPS OR LOCALHOST",
      );
      return;
    }
    let access: MIDIAccess;
    try {
      access = await navigator.requestMIDIAccess({ sysex: true });
    } catch {
      if (!this.stopped) this.setStatus("NO MIDI ACCESS - CONNECT THE PEDAL, ALLOW MIDI, RELOAD");
      return;
    }
    if (this.stopped) return;
    this.access = access;
    access.onstatechange = () => this.choosePorts();
    this.choosePorts();
    this.timer = setInterval(() => this.tick(), 250);
  }

  /** Ends the link: no more HELLOs or MIDI handlers; a waiting receive gets null. */
  stop(): void {
    this.stopped = true;
    clearInterval(this.timer);
    this.timer = undefined;
    if (this.access) this.access.onstatechange = null;
    if (this.input) this.input.onmidimessage = null;
    this.queue = [];
    this.waiter?.(null);
  }

  selectInput(id: string): void {
    this.inputPref = id;
    this.choosePorts();
  }
  selectOutput(id: string): void {
    this.outputPref = id;
    this.choosePorts();
  }

  send(bytes: Uint8Array): void {
    if (this.output) {
      try {
        this.output.send(bytes);
      } catch {
        /* port vanished; statechange re-chooses */
      }
    }
  }

  private choosePorts(): void {
    if (!this.access) return;
    const pick = <T extends MIDIPort>(ports: Iterable<T>, pref: string): T | null => {
      const list = [...ports].filter((p) => p.state !== "disconnected");
      return list.find((p) => p.id === pref) ?? list.find((p) => /daisy/i.test(p.name ?? "")) ?? list[0] ?? null;
    };
    const input = pick(this.access.inputs.values(), this.inputPref);
    const output = pick(this.access.outputs.values(), this.outputPref);
    const inChanged = input !== this.input;
    if (inChanged) {
      if (this.input) this.input.onmidimessage = null;
      this.input = input;
      this.assembler = new SysExAssembler(MAX_FRAME);
      if (input) input.onmidimessage = (e) => e.data && this.rx(e.data);
    }
    const outChanged = output !== this.output;
    this.output = output;
    if (inChanged || outChanged) this.lastRx = 0;
    this.hello();
    // The ports may have changed under an unchanged status: publish either way.
    this.status = this.linkStatus();
    this.publish();
  }

  private rx(data: Uint8Array): void {
    for (const raw of this.assembler.feed(data)) {
      this.lastRx = Date.now();
      this.updateStatus();
      if (this.waiter) this.waiter(raw);
      else {
        this.queue.push(raw);
        if (this.queue.length > QUEUE_MAX) this.queue.shift();
      }
      const msg = parseMessage(raw);
      if (msg) for (const f of this.msgListeners) f(msg);
    }
  }

  private hello(): void {
    this.lastHello = Date.now();
    this.send(helloMsg());
  }

  private tick(): void {
    const now = Date.now();
    if (now - this.lastRx >= RX_TIMEOUT_MS && now - this.lastHello >= HELLO_PERIOD_MS) this.hello();
    this.updateStatus();
  }

  private linkStatus(): string {
    if (!this.input || !this.output) return "NO MIDI DEVICE";
    return this.linked ? "LINKED" : NO_RESPONSE;
  }

  private updateStatus(): void {
    this.setStatus(this.linkStatus());
  }

  /** Publishes a new status; "LINKED" is the only status with `linked` set, so an unchanged status is an unchanged link. */
  private setStatus(s: string): void {
    if (s === this.status) return;
    this.status = s;
    this.publish();
  }

  private publish(): void {
    const snap = this.snapshot();
    for (const f of this.snapListeners) f(snap);
  }
}

const portInfo = (p: MIDIPort): PortInfo => ({ id: p.id, name: p.name ?? p.id });
