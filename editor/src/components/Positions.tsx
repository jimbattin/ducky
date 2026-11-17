interface Props {
  label: string;
  /** The position names, up first. */
  options: readonly string[];
  /** Index into options; -1 = unknown. */
  value: number;
}

/** A toggle switch's positions as a Platinum segmented group, the current one pressed; read-only. */
export function Positions({ label, options, value }: Props) {
  return (
    // biome-ignore lint/a11y/useSemanticElements: a row of position labels, not a form fieldset
    <div class="segmented" role="group" aria-label={label}>
      {options.map((name, i) => (
        // biome-ignore lint/suspicious/noArrayIndexKey: the positions are fixed (up, middle, down); two may share a name
        <span key={i} data-on={i === value} aria-current={i === value ? "true" : undefined} title={name}>
          {name}
        </span>
      ))}
    </div>
  );
}
