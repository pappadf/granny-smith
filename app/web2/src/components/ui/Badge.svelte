<script lang="ts">
  import type { Snippet } from 'svelte';

  // A small label: a `pill` tinted by its intent (the TT / PT / INVALID
  // address tags, the image mount badge) or a `count` (a section's item
  // count; plain text by default, a skin may make it a pill).  `class`
  // carries legacy hooks (tag, tag-tt, badge, count).
  interface Props {
    intent?: 'neutral' | 'info' | 'success' | 'warning' | 'danger';
    variant?: 'pill' | 'count';
    class?: string;
    title?: string;
    children: Snippet;
  }
  let { intent = 'neutral', variant = 'pill', class: cls = '', title, children }: Props = $props();
</script>

<span class="gs-badge {cls}" data-variant={variant} data-intent={intent} {title}
  >{@render children()}</span
>

<style>
  .gs-badge[data-variant='pill'] {
    display: inline-block;
    flex: none;
    height: var(--gs-pill-height);
    line-height: var(--gs-pill-height);
    padding: 0 var(--gs-pill-padding-x);
    border-radius: var(--gs-pill-radius);
    font-size: var(--gs-pill-font-size);
    font-weight: var(--gs-pill-weight);
    letter-spacing: var(--gs-pill-tracking);
    text-transform: var(--gs-pill-transform);
    white-space: nowrap;
    background: var(--gs-control-active);
    color: var(--gs-text);
  }
  .gs-badge[data-variant='pill'][data-intent='info'] {
    background: var(--gs-info-bg);
    color: var(--gs-info-fg);
  }
  .gs-badge[data-variant='pill'][data-intent='success'] {
    background: var(--gs-success-bg);
    color: var(--gs-success-fg);
  }
  .gs-badge[data-variant='pill'][data-intent='warning'] {
    background: var(--gs-warning-bg);
    color: var(--gs-warning-fg);
  }
  .gs-badge[data-variant='pill'][data-intent='danger'] {
    background: var(--gs-danger-bg);
    color: var(--gs-danger-fg);
  }
  .gs-badge[data-variant='count'] {
    color: var(--gs-count-fg);
    font-size: var(--gs-count-font-size);
    background: var(--gs-count-bg);
    padding: var(--gs-count-padding);
    border-radius: var(--gs-count-radius);
  }
</style>
