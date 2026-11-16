import { useLayoutEffect, useMemo, useRef } from "preact/hooks";
import type { LogLine } from "../state";

interface Props {
  lines: readonly LogLine[];
}

const TIME = new Intl.DateTimeFormat("en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit" });

/**
 * The pedal's log (boot, loads, stores, one status line a second), oldest first, each line with its local receive time.
 * It follows new lines while scrolled to the end; scrolling up holds the view until it is back at the end.
 */
export function LogView({ lines }: Props) {
  const box = useRef<HTMLPreElement>(null);
  const follow = useRef(true);
  // The app renders several times a second; format the (up to 1000) lines only when they change.
  const text = useMemo(() => lines.map(({ t, text }) => `${TIME.format(t)}  ${text}`).join("\n"), [lines]);
  // biome-ignore lint/correctness/useExhaustiveDependencies: runs after each new text to keep the end in view
  useLayoutEffect(() => {
    const el = box.current;
    if (el && follow.current) el.scrollTop = el.scrollHeight;
  }, [text]);
  if (lines.length === 0) return <p class="tab-empty">No log lines yet.</p>;
  return (
    <pre
      ref={box}
      class="log"
      role="log"
      onScroll={(e) => {
        const el = e.currentTarget;
        follow.current = el.scrollHeight - el.scrollTop - el.clientHeight < 4;
      }}
    >
      {text}
    </pre>
  );
}
