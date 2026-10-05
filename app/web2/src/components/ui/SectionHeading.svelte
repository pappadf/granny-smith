<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';

  // A micro-heading over a group: `h3` (a card or a column) or the smaller
  // `h4` (a register group).  `as` renders another element with the same
  // look (a label span, a list item); `rule` draws a divider above it,
  // except as the first child.  `class` carries legacy hooks.
  interface Props extends HTMLAttributes<HTMLElement> {
    level?: 'h3' | 'h4';
    as?: string;
    rule?: boolean;
    class?: string;
    children: Snippet;
  }
  let { level = 'h3', as, rule = false, class: cls = '', children, ...rest }: Props = $props();
</script>

<svelte:element
  this={as ?? level}
  {...rest}
  class="gs-heading {cls}"
  class:gs-heading--rule={rule}
  data-level={level}
  data-as={as}
>
  {@render children()}
</svelte:element>

<style>
  .gs-heading {
    font-size: var(--gs-heading-font-size);
    font-weight: var(--gs-heading-weight);
    color: var(--gs-heading-fg);
    text-transform: var(--gs-heading-transform);
    letter-spacing: var(--gs-heading-tracking);
  }
  .gs-heading[data-level='h3'] {
    margin: 0 0 var(--gs-heading-gap);
  }
  .gs-heading[data-level='h4'] {
    font-size: var(--gs-heading-font-size-sm);
    margin: var(--gs-heading-sm-margin-top) 0 var(--gs-heading-sm-margin-bottom);
  }
  .gs-heading[data-as] {
    margin: 0;
  }
  /* A divider heading: a quieter weight under a hairline. */
  .gs-heading.gs-heading--rule {
    font-weight: var(--gs-heading-rule-weight);
    padding: var(--gs-space-2) var(--gs-space-3) var(--gs-space-0-5);
    border-top: var(--gs-border-width) solid var(--gs-section-divider);
    margin-top: var(--gs-space-1);
  }
  .gs-heading.gs-heading--rule:first-child {
    border-top: none;
    margin-top: 0;
  }
</style>
