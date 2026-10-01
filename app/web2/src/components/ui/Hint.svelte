<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';

  // A hint, empty or loading note: muted (or error) small text.  `inset`
  // picks the padding: `section` inside a debugger section, `block` for a
  // section's only content, `pane` for a code pane, `list` in place of a
  // list's rows and `view` for a whole empty view (the last two larger);
  // `statusline` is a view's footer line (a rule above, its own fill).
  // `as` renders a `p` by default; `class` carries legacy hooks.
  interface Props extends HTMLAttributes<HTMLElement> {
    tone?: 'muted' | 'error';
    inset?: 'none' | 'section' | 'block' | 'pane' | 'list' | 'view' | 'statusline';
    as?: string;
    class?: string;
    children: Snippet;
  }
  let {
    tone = 'muted',
    inset = 'none',
    as = 'p',
    class: cls = '',
    children,
    ...rest
  }: Props = $props();
</script>

<svelte:element
  this={as}
  {...rest}
  class="gs-hint {cls}"
  data-tone={tone}
  data-inset={inset}
  role={tone === 'error' ? 'alert' : rest.role}
>
  {@render children()}
</svelte:element>

<style>
  .gs-hint {
    color: var(--gs-hint-fg);
    font-size: var(--gs-hint-font-size);
  }
  .gs-hint[data-tone='error'] {
    color: var(--gs-hint-error-fg);
  }
  .gs-hint[data-inset='section'] {
    padding: var(--gs-hint-padding-section);
  }
  .gs-hint[data-inset='block'] {
    padding: var(--gs-hint-padding-block);
  }
  .gs-hint[data-inset='pane'] {
    padding: var(--gs-hint-padding-pane);
  }
  .gs-hint[data-inset='list'] {
    font-size: var(--gs-hint-font-size-view);
    padding: var(--gs-hint-padding-list);
  }
  .gs-hint[data-inset='statusline'] {
    flex: 0 0 auto;
    margin: 0;
    padding: var(--gs-statusline-padding);
    border-top: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-statusline-bg);
  }
  .gs-hint[data-inset='view'] {
    font-size: var(--gs-hint-font-size-view);
    padding: var(--gs-hint-padding-view);
    line-height: var(--gs-line-height-relaxed);
  }
</style>
