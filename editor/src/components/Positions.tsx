import { Captioned } from "./Segmented";

interface Props {
  label: string;
  /** The position names, down first (STATUS's order: 0 down, 1 middle, 2 up). */
  options: readonly string[];
  /** Index into options; -1 = unknown. */
  value: number;
  /** Under each name: its position (down first), when the names are a parameter's values; none for no caption row. */
  captions?: readonly string[];
}

/** A toggle switch's positions as a Platinum segmented group, the current one pressed; read-only. */
export function Positions({ label, options, value, captions }: Props) {
  return (
    <Captioned captions={captions}>
      {/* biome-ignore lint/a11y/useSemanticElements: a row of position labels, not a form fieldset */}
      <div class="segmented" role="group" aria-label={label}>
        {options.map((name, i) => {
          const title = captions?.[i] ? `${name}: switch ${captions[i]}` : name;
          return (
            // biome-ignore lint/suspicious/noArrayIndexKey: the positions are fixed (down, middle, up); two may share a name
            <span key={i} data-on={i === value} aria-current={i === value ? "true" : undefined} title={title}>
              {name}
            </span>
          );
        })}
      </div>
    </Captioned>
  );
}
