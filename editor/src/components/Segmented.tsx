interface Props {
  label: string;
  /** The option names in value order, shown left to right. */
  options: readonly string[];
  /** Index into options; -1 = unknown. */
  value: number;
  disabled?: boolean;
  onSelect: (index: number) => void;
}

/** A Platinum bevel-button group: one pressed button per choice. */
export function Segmented({ label, options, value, disabled, onSelect }: Props) {
  return (
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
          title={name}
          onClick={() => {
            if (i !== value) onSelect(i);
          }}
        >
          {name}
        </button>
      ))}
    </div>
  );
}
