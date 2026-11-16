import type { ComponentChildren } from "preact";

interface Props {
  title: ComponentChildren;
  /** Extra class on the box. */
  class?: string;
  children: ComponentChildren;
}

/** An etched group box with its title on the top edge. */
export function GroupBox({ title, class: extra, children }: Props) {
  return (
    <section class={extra ? `group ${extra}` : "group"}>
      <h2 class="group-title">{title}</h2>
      {children}
    </section>
  );
}
