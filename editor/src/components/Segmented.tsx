import type { ComponentChildren } from "preact";

interface Props {
  label: string;
  /** The option names in value order, shown left to right. */
  options: readonly string[];
  /** Index into options; -1 = unknown. */
  value: number;
  disabled?: boolean;
  /** Under each option: the toggle position that selects it (togglePositions); none for no caption row. */
  captions?: readonly string[];
  onSelect: (index: number) => void;
}

/** A Platinum bevel-button group: one pressed button per choice. */
export function Segmented({ label, options, value, disabled, captions, onSelect }: Props) {
  return (
    <Captioned captions={captions}>
      <div class="segmented" role="radiogroup" aria-label={label}>
        {options.map((name, i) => (
          // biome-ignore lint/a11y/useSemanticElements: a bevel button in a segmented group, not a form field
          <button
            key={name}
            type="button"
            role="radio"
            aria-checked={i === value}
            data-on={i === value}
            disabled={disabled}
            title={segmentTitle(name, captions?.[i])}
            onClick={() => {
              if (i !== value) onSelect(i);
            }}
          >
            {name}
          </button>
        ))}
      </div>
    </Captioned>
  );
}

/** A segment's tooltip: its name, and the toggle position that gives it when captioned. */
export const segmentTitle = (name: string, caption: string | undefined): string =>
  caption ? `${name}: switch ${caption}` : name;

/** A segmented group with a caption under each segment: the toggle position that gives it. */
export function Captioned({ captions, children }: { captions?: readonly string[]; children: ComponentChildren }) {
  if (!captions) return <>{children}</>;
  return (
    <div class="segmented-captioned">
      {children}
      <div class="segmented-captions" aria-hidden="true">
        {captions.map((c, i) => (
          // biome-ignore lint/suspicious/noArrayIndexKey: fixed positions; two may share a name
          <span key={i}>{c}</span>
        ))}
      </div>
    </div>
  );
}
