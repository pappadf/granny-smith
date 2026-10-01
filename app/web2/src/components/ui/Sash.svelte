<script lang="ts">
  import type { HTMLAttributes } from 'svelte/elements';

  // A drag handle between two areas.  `orientation` is the separator's own
  // (a `vertical` sash is a vertical line and drags sideways).  `line`
  // draws a hairline in its middle; the handle fills on hover and while
  // `active` (data-state=dragging).  The behaviour stays with the owner
  // (PaneSplit, WorkbenchSash), which passes its pointer handlers; `class`
  // carries the legacy hook (pane-sash, workbench-sash).
  interface Props extends HTMLAttributes<HTMLDivElement> {
    orientation: 'vertical' | 'horizontal';
    active?: boolean;
    line?: boolean;
    label: string;
    class?: string;
  }
  let {
    orientation,
    active = false,
    line = false,
    label,
    class: cls = '',
    ...rest
  }: Props = $props();
</script>

<div
  {...rest}
  class="gs-sash {cls}"
  class:active
  class:gs-sash--line={line}
  role="separator"
  aria-orientation={orientation}
  aria-label={label}
  data-state={active ? 'dragging' : undefined}
></div>

<style>
  .gs-sash {
    flex: 0 0 var(--gs-sash-size);
    background: transparent;
    position: relative;
    z-index: var(--gs-z-sash);
    user-select: none;
  }
  .gs-sash[aria-orientation='vertical'] {
    cursor: col-resize;
    width: var(--gs-sash-size);
    height: 100%;
    margin: 0 calc(var(--gs-sash-size) / -2);
  }
  .gs-sash[aria-orientation='horizontal'] {
    cursor: row-resize;
    width: 100%;
    height: var(--gs-sash-size);
    margin: calc(var(--gs-sash-size) / -2) 0;
  }
  /* The hairline in the middle of the hit area, the colour of the section
     dividers so the split reads as part of the same grid. */
  .gs-sash--line::before {
    content: '';
    position: absolute;
    background: var(--gs-sash-line);
    pointer-events: none;
  }
  .gs-sash--line[aria-orientation='vertical']::before {
    top: 0;
    bottom: 0;
    left: 50%;
    width: var(--gs-border-width);
    transform: translateX(-50%);
  }
  .gs-sash--line[aria-orientation='horizontal']::before {
    left: 0;
    right: 0;
    top: 50%;
    height: var(--gs-border-width);
    transform: translateY(-50%);
  }
  .gs-sash:hover,
  .gs-sash[data-state='dragging'] {
    background: var(--gs-sash-hover);
  }
</style>
