/** The progress, result or error of the last store action. */
export interface Status {
  text: string;
  error: boolean;
}
export const NO_STATUS: Status = { text: "", error: false };

/** The window's status bar; it keeps its height while the text is empty. */
export function StatusLine({ text, error }: Status) {
  return (
    <p class={error ? "statusbar error" : "statusbar"} role="status">
      {text || "\u00a0"}
    </p>
  );
}
