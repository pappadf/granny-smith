<script lang="ts">
  import type { Snippet } from 'svelte';
  import Disclosure from '@/components/ui/Disclosure.svelte';
  import Badge from '@/components/ui/Badge.svelte';

  interface Props {
    title: string;
    open: boolean;
    onToggle: () => void;
    count?: number;
    /** Header-right snippet (e.g. upload button). */
    actions?: Snippet;
    children: Snippet;
  }
  let { title, open, onToggle, count, actions, children }: Props = $props();
</script>

<!-- The toggle is a real button, reachable by Tab and Enter/Space, and the
     header actions are its siblings, never nested inside it.  The divider
     between consecutive sections is in styles/components.css (.gs-section +
     .gs-section).  Legacy hooks: section, gs-collapsible, header, toggle,
     title, count. -->
<section class="gs-section section gs-collapsible" class:open>
  <header class="gs-section__header header">
    <button type="button" class="gs-section__toggle toggle" onclick={onToggle} aria-expanded={open}>
      <Disclosure class="twistie" {open} />
      <span class="gs-section__title title">{title}</span>
      {#if typeof count === 'number'}
        <Badge variant="count" class="gs-section__count count">{count}</Badge>
      {/if}
    </button>
    {#if actions}
      <span class="gs-section__actions actions">{@render actions()}</span>
    {/if}
  </header>
  {#if open}
    <div class="gs-section__body body">
      {@render children()}
    </div>
  {/if}
</section>

<style>
  .gs-section {
    display: flex;
    flex-direction: column;
  }
  .gs-section__header {
    height: var(--gs-section-header-height);
    display: flex;
    align-items: center;
    padding: 0 var(--gs-space-2) 0 0;
    user-select: none;
    background: var(--gs-section-header-bg);
  }
  .gs-section__header:hover {
    background: var(--gs-row-hover);
  }
  /* Fills the header, so a click anywhere but the actions toggles. */
  .gs-section__toggle {
    flex: 1 1 auto;
    min-width: 0;
    height: 100%;
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    padding: 0 0 0 var(--gs-space-2);
    border: none;
    background: transparent;
    color: inherit;
    font: inherit;
    text-align: left;
    cursor: pointer;
  }
  .gs-section__toggle:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .gs-section__title {
    font-size: var(--gs-heading-font-size);
    font-weight: var(--gs-heading-weight);
    text-transform: var(--gs-heading-transform);
    letter-spacing: var(--gs-heading-tracking);
    color: var(--gs-section-title-fg);
    flex: 1 1 auto;
  }
  .gs-section__toggle > :global(.gs-section__count) {
    margin-right: var(--gs-space-1);
  }
  .gs-section__actions {
    display: inline-flex;
    align-items: center;
  }
  .gs-section__body {
    display: flex;
    flex-direction: column;
  }
</style>
