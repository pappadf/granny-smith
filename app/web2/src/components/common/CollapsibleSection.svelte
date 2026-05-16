<script lang="ts">
  import type { Snippet } from 'svelte';

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

<section class="section" class:open>
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <header class="header" onclick={onToggle} role="button" tabindex="-1" aria-expanded={open}>
    <span class="twistie">{open ? '▼' : '▶'}</span>
    <span class="title">{title}</span>
    {#if typeof count === 'number'}
      <span class="count">{count}</span>
    {/if}
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
  .header {
    height: 22px;
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 0 8px;
    cursor: pointer;
    user-select: none;
    background: var(--gs-bg);
  }
  .header:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .twistie {
    display: inline-block;
    width: 14px;
    text-align: center;
    color: var(--gs-fg-muted);
    font-size: 9px;
    flex-shrink: 0;
  }
  .title {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--gs-fg-bright);
    flex: 1 1 auto;
  }
  .count {
    color: var(--gs-fg-muted);
    font-size: 11px;
    margin-right: 4px;
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
