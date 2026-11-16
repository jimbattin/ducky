type LampColor = "green" | "amber" | "red";

/** A status lamp, off when color is null. Decorative: the caller states its meaning in text. */
export function Lamp({ color }: { color: LampColor | null }) {
  return <span class="lamp" data-color={color ?? undefined} aria-hidden="true" />;
}
