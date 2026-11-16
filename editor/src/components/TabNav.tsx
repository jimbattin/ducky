import { Lamp } from "./Lamp";

export interface NavTab<T extends string> {
  id: T;
  label: string;
  /** A lamp beside the label: the section is in use on the pedal. */
  lit?: boolean;
}

interface Props<T extends string> {
  tabs: readonly NavTab<T>[];
  active: T;
  onSelect: (id: T) => void;
}

/** Horizontal WAI-ARIA tab list; the panel is the element with id "tabpanel". Left/Right cycle, Home/End jump. */
export function TabNav<T extends string>({ tabs, active, onSelect }: Props<T>) {
  const onKeyDown = (e: KeyboardEvent) => {
    const i = tabs.findIndex((t) => t.id === active);
    const n = tabs.length;
    let to: number;
    switch (e.key) {
      case "ArrowRight":
        to = (i + 1) % n;
        break;
      case "ArrowLeft":
        to = (i - 1 + n) % n;
        break;
      case "Home":
        to = 0;
        break;
      case "End":
        to = n - 1;
        break;
      default:
        return;
    }
    const next = tabs[to];
    if (!next) return;
    e.preventDefault();
    onSelect(next.id);
    document.getElementById(`tab-${next.id}`)?.focus();
  };
  return (
    <nav class="tabnav" aria-label="Sections">
      <div role="tablist" aria-orientation="horizontal" onKeyDown={onKeyDown}>
        {tabs.map((t) => (
          <button
            key={t.id}
            type="button"
            role="tab"
            id={`tab-${t.id}`}
            class="tab"
            aria-selected={t.id === active}
            aria-controls="tabpanel"
            tabIndex={t.id === active ? 0 : -1}
            onClick={() => onSelect(t.id)}
          >
            {t.lit !== undefined && <Lamp color={t.lit ? "green" : null} />}
            {t.label}
            {t.lit && <span class="visually-hidden"> (active on the pedal)</span>}
          </button>
        ))}
      </div>
    </nav>
  );
}
