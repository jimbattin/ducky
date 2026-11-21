// A minimal Chrome DevTools Protocol client (Bun's WebSocket, no dependency) for tools/firmware_check.ts: launches
// Chrome with a throwaway profile, grants Web MIDI, opens pages, evaluates script, sets file inputs. JavaScript
// dialogs (confirm) are accepted.
// CHROME overrides the executable (default /usr/bin/google-chrome-stable).

import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import type { Subprocess } from "bun";

const SEND_TIMEOUT_MS = 30000;

type Params = Record<string, unknown>;
type Pending = { resolve: (r: Params) => void; reject: (e: Error) => void; timer: Timer };

export interface Page {
  /** The value of expr (awaited when a promise), returned by value. */
  eval<T>(expr: string): Promise<T>;
  /** Polls expr every 200 ms until truthy; throws with what and the editor's status bar after timeoutMs. */
  waitFor(expr: string, timeoutMs: number, what: string): Promise<void>;
  /** Sets the file input matching selector to the file at path. */
  setFile(selector: string, path: string): Promise<void>;
  /** Clicks the element matching selector. */
  click(selector: string): Promise<void>;
}

export interface Chrome {
  send(method: string, params?: Params, sessionId?: string): Promise<Params>;
  /** Grants Web MIDI with SysEx to origin. */
  grantMidi(origin: string): Promise<void>;
  open(url: string): Promise<Page>;
  close(): Promise<void>;
}

async function readPort(dir: string): Promise<{ port: number; path: string }> {
  const deadline = Date.now() + 10000;
  for (;;) {
    try {
      const [port, path] = readFileSync(join(dir, "DevToolsActivePort"), "utf8").split("\n");
      if (port && path) return { port: Number(port), path };
    } catch {}
    if (Date.now() > deadline) throw new Error("Chrome wrote no DevToolsActivePort within 10 s");
    await Bun.sleep(100);
  }
}

export async function launchChrome({ headless }: { headless: boolean }): Promise<Chrome> {
  const dir = mkdtempSync(join(tmpdir(), "ducky-chrome-"));
  const exe = process.env.CHROME ?? "/usr/bin/google-chrome-stable";
  const proc: Subprocess = Bun.spawn(
    [
      exe,
      ...(headless ? ["--headless=new"] : []),
      `--user-data-dir=${dir}`,
      "--remote-debugging-port=0",
      "--no-first-run",
      "--no-default-browser-check",
      "about:blank",
    ],
    { stdout: "ignore", stderr: "ignore" },
  );
  const { port, path } = await readPort(dir);
  const ws = new WebSocket(`ws://127.0.0.1:${port}${path}`);
  const opened = Promise.withResolvers<void>();
  ws.onopen = () => opened.resolve();
  ws.onerror = () => opened.reject(new Error("CDP WebSocket failed to open"));
  await opened.promise;

  let nextId = 1;
  const pending = new Map<number, Pending>();
  const listeners: ((method: string, params: Params, sessionId?: string) => void)[] = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(String(ev.data)) as {
      id?: number;
      result?: Params;
      error?: { message: string };
      method?: string;
      params?: Params;
      sessionId?: string;
    };
    if (m.id !== undefined) {
      const p = pending.get(m.id);
      if (p === undefined) return;
      pending.delete(m.id);
      clearTimeout(p.timer);
      if (m.error) p.reject(new Error(m.error.message));
      else p.resolve(m.result ?? {});
    } else if (m.method !== undefined) {
      for (const l of listeners) l(m.method, m.params ?? {}, m.sessionId);
    }
  };

  const send = (method: string, params: Params = {}, sessionId?: string): Promise<Params> => {
    const { promise, resolve: res, reject: rej } = Promise.withResolvers<Params>();
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      rej(new Error(`CDP ${method}: no reply within ${SEND_TIMEOUT_MS / 1000} s`));
    }, SEND_TIMEOUT_MS);
    pending.set(id, { resolve: res, reject: rej, timer });
    ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return promise;
  };

  // Every confirm/alert is accepted.
  listeners.push((method, _params, sessionId) => {
    if (method === "Page.javascriptDialogOpening")
      void send("Page.handleJavaScriptDialog", { accept: true }, sessionId).catch(() => undefined);
  });

  const open = async (url: string): Promise<Page> => {
    const { targetId } = (await send("Target.createTarget", { url: "about:blank" })) as { targetId: string };
    const { sessionId } = (await send("Target.attachToTarget", { targetId, flatten: true })) as { sessionId: string };
    await send("Page.enable", {}, sessionId);
    await send("Runtime.enable", {}, sessionId);
    await send("Page.navigate", { url }, sessionId);

    const evaluate = async <T>(expr: string): Promise<T> => {
      const r = (await send(
        "Runtime.evaluate",
        { expression: expr, returnByValue: true, awaitPromise: true },
        sessionId,
      )) as { result: { value?: T }; exceptionDetails?: { text: string; exception?: { description?: string } } };
      if (r.exceptionDetails)
        throw new Error(`${expr}: ${r.exceptionDetails.exception?.description ?? r.exceptionDetails.text}`);
      return r.result.value as T;
    };
    return {
      eval: evaluate,
      async waitFor(expr, timeoutMs, what) {
        const deadline = Date.now() + timeoutMs;
        for (;;) {
          if (await evaluate<boolean>(`!!(${expr})`).catch(() => false)) return;
          if (Date.now() > deadline) {
            const bar = await evaluate<string>(`document.querySelector(".statusbar")?.textContent ?? ""`).catch(
              () => "",
            );
            throw new Error(`${what}: not within ${timeoutMs / 1000} s (status bar: "${bar}")`);
          }
          await Bun.sleep(200);
        }
      },
      async setFile(selector, path) {
        const { root } = (await send("DOM.getDocument", {}, sessionId)) as { root: { nodeId: number } };
        const { nodeId } = (await send("DOM.querySelector", { nodeId: root.nodeId, selector }, sessionId)) as {
          nodeId: number;
        };
        if (!nodeId) throw new Error(`no element ${selector}`);
        await send("DOM.setFileInputFiles", { nodeId, files: [resolve(path)] }, sessionId);
      },
      async click(selector) {
        const found = await evaluate<boolean>(
          `(() => { const e = document.querySelector(${JSON.stringify(selector)}); if (!e) return false; e.click(); return true; })()`,
        );
        if (!found) throw new Error(`no element ${selector}`);
      },
    };
  };

  return {
    send,
    async grantMidi(origin) {
      await send("Browser.grantPermissions", { origin, permissions: ["midi", "midiSysex"] });
    },
    open,
    async close() {
      await send("Browser.close").catch(() => undefined);
      ws.close();
      proc.kill();
      await proc.exited;
      rmSync(dir, { recursive: true, force: true });
    },
  };
}
