<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';

  // A flat list row: `row` density (the images list) or `compact` (the
  // debugger's breakpoint, watchpoint and call-stack lists, usually `mono`).
  // A selected row shows the focused-list colour while focus is in its list
  // and the inactive colour otherwise.  `indent` lines the row up under a
  // section's disclosure.  The root takes role and handlers; `class`
  // carries legacy hooks (image-row, bp-row, wp-row, frame-row).
  interface Props extends HTMLAttributes<HTMLDivElement> {
    selected?: boolean;
    hover?: boolean;
    mono?: boolean;
    indent?: boolean;
    density?: 'row' | 'compact';
    class?: string;
    children: Snippet;
  }
  let {
    selected = false,
    hover = true,
    mono = false,
    indent = false,
    density = 'row',
    class: cls = '',
    children,
    ...rest
  }: Props = $props();
</script>

<div
  {...rest}
  class="gs-list-row {cls}"
  class:selected
  class:gs-list-row--hover={hover}
  class:gs-list-row--mono={mono}
  class:gs-list-row--indent={indent}
  data-density={density}
  data-selected={selected || undefined}
>
  {@render children()}
</div>

<style>
  .gs-list-row {
    display: flex;
    align-items: center;
    color: var(--gs-text);
  }
  .gs-list-row[data-density='row'] {
    gap: var(--gs-row-gap);
    height: var(--gs-row-height);
    padding: 0 var(--gs-row-padding-x);
    font-size: var(--gs-row-font-size);
    cursor: pointer;
    user-select: none;
  }
  .gs-list-row[data-density='compact'] {
    gap: var(--gs-list-row-gap-compact);
    padding: var(--gs-list-row-padding-compact);
    font-size: var(--gs-list-row-font-size-compact);
  }
  .gs-list-row--indent {
    padding-left: var(--gs-list-row-indent);
  }
  .gs-list-row--mono {
    font-family: var(--gs-font-mono);
  }
  .gs-list-row--hover:hover,
  .gs-list-row--hover[data-force-state='hover'] {
    background: var(--gs-row-hover);
  }
  .gs-list-row[data-selected] {
    background: var(--gs-row-selected-inactive);
    color: var(--gs-row-selected-inactive-fg);
  }
  :global(:focus-within) > .gs-list-row[data-selected] {
    background: var(--gs-row-selected);
    color: var(--gs-row-selected-fg);
  }
  .gs-list-row:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
</style>
