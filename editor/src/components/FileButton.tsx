interface Props {
  label: string;
  /** The file input's accept list: ".nam", ".wav", ".bin". */
  accept: string;
  small?: boolean;
  disabled?: boolean;
  onFile: (f: File) => void;
}

/** A file picker styled as a button; it resets after each pick, so the same file can be picked again. */
export function FileButton({ label, accept, small, disabled, onFile }: Props) {
  return (
    <label class={small ? "btn small file-btn" : "btn file-btn"}>
      {label}
      <input
        type="file"
        accept={accept}
        class="visually-hidden"
        disabled={disabled}
        onChange={(e) => {
          const f = e.currentTarget.files?.[0];
          e.currentTarget.value = "";
          if (f) onFile(f);
        }}
      />
    </label>
  );
}
