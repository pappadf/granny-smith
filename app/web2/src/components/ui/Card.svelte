<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';
  import SectionHeading from './SectionHeading.svelte';

  // A raised panel on the welcome and URL-boot screens, with an optional
  // micro-heading.  `class` carries legacy hooks (card, failed).
  interface Props extends HTMLAttributes<HTMLElement> {
    heading?: string;
    class?: string;
    children: Snippet;
  }
  let { heading, class: cls = '', children, ...rest }: Props = $props();
</script>

<section {...rest} class="gs-card {cls}">
  {#if heading}<SectionHeading class="gs-card__heading card-heading">{heading}</SectionHeading>{/if}
  {@render children()}
</section>

<style>
  .gs-card {
    background: var(--gs-card-bg);
    border: var(--gs-border-width) solid var(--gs-card-border);
    border-radius: var(--gs-card-radius);
    padding: var(--gs-card-padding);
    margin-bottom: var(--gs-space-4);
  }
</style>
