<script lang="ts">
  import type { Snippet } from 'svelte';
  import Icon from '@/components/common/Icon.svelte';

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

<!-- The toggle is a real button, reachable by Tab and Enter/Space (the header
     used to be role="button" with tabindex -1, so no keyboard could open a
     section), and the header actions are its siblings, never nested inside
     it. -->
<section class="section gs-collapsible" class:open>
  <header class="header">
    <button type="button" class="toggle" onclick={onToggle} aria-expanded={open}>
      <span class="twistie" class:open><Icon name="chevron" size={12} /></span>
      <span class="title">{title}</span>
      {#if typeof count === 'number'}
        <span class="count">{count}</span>
      {/if}
    </button>
    {#if actions}
      <span class="actions">{@render actions()}</span>
    {/if}
  </header>
  {#if open}
    <div class="body">
      {@render children()}
    </div>
  {/if}
</section>

<style>
  .section {
    display: flex;
    flex-direction: column;
  }
  /* Divider between consecutive sections — no leading border above the
     first one, no trailing border below the last one. `:global` because
     each <section> is the root of its own CollapsibleSection instance,
     so Svelte's per-component scoping would otherwise treat the second
     selector as unmatched. */
  :global(.gs-collapsible + .gs-collapsible) {
    border-top: var(--gs-border-width) solid var(--gs-border);
  }
  .header {
    height: var(--gs-size-row);
    display: flex;
    align-items: center;
    padding: 0 var(--gs-space-2) 0 0;
    user-select: none;
    background: var(--gs-surface-app);
  }
  .header:hover {
    background: var(--gs-row-hover);
  }
  /* Fills the header, so a click anywhere but the actions toggles. */
  .toggle {
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
  .toggle:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .twistie {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: var(--gs-size-icon-md);
    color: var(--gs-text-muted);
    flex-shrink: 0;
    /* Chevron points down when open, rotates to point right when
       collapsed. Matches the codicon-driven VS Code tree pattern. */
    transform: rotate(-90deg);
    transition: transform var(--gs-duration-instant) var(--gs-ease-out);
  }
  .twistie.open {
    transform: rotate(0deg);
  }
  .title {
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
    text-transform: var(--gs-caps-transform);
    letter-spacing: var(--gs-caps-tracking);
    color: var(--gs-text-strong);
    flex: 1 1 auto;
  }
  .count {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    margin-right: var(--gs-space-1);
  }
  .actions {
    display: inline-flex;
    align-items: center;
  }
  .body {
    display: flex;
    flex-direction: column;
  }
</style>
