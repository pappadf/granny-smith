<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';

  // A tinted box that sets content apart, by intent (the command browser's
  // usage pane is `info`).  `edge="top"` draws only the top rule, for a box
  // docked under a list.  `class` carries legacy hooks (details).
  interface Props extends HTMLAttributes<HTMLElement> {
    intent?: 'info' | 'success' | 'warning' | 'danger';
    edge?: 'all' | 'top';
    as?: string;
    class?: string;
    children: Snippet;
  }
  let {
    intent = 'info',
    edge = 'all',
    as = 'section',
    class: cls = '',
    children,
    ...rest
  }: Props = $props();
</script>

<svelte:element this={as} {...rest} class="gs-callout {cls}" data-intent={intent} data-edge={edge}>
  {@render children()}
</svelte:element>

<style>
  .gs-callout {
    background: var(--gs-callout-bg);
  }
  .gs-callout[data-intent='success'] {
    background: var(--gs-callout-success-bg);
  }
  .gs-callout[data-intent='warning'] {
    background: var(--gs-callout-warning-bg);
  }
  .gs-callout[data-intent='danger'] {
    background: var(--gs-callout-danger-bg);
  }
  .gs-callout[data-edge='all'] {
    border: var(--gs-border-width) solid var(--gs-callout-border);
    border-radius: var(--gs-radius-md);
  }
  .gs-callout[data-edge='top'] {
    border-top: var(--gs-border-width) solid var(--gs-callout-border);
  }
</style>
