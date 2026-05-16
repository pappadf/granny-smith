<script lang="ts" generics="K extends string">
  import type { Snippet } from 'svelte';

  interface Props {
    tabs: ReadonlyArray<{ key: K; label: string }>;
    active: K;
    onSelect: (key: K) => void;
    /** Optional right-edge accessory (e.g. the MMU section's S|U toggle). */
    accessory?: Snippet;
  }
  let { tabs, active, onSelect, accessory }: Props = $props();
</script>

<div class="tab-strip" role="tablist">
  {#each tabs as tab (tab.key)}
    <button
      type="button"
      class="tab"
      class:active={tab.key === active}
      role="tab"
      aria-selected={tab.key === active}
      tabindex={tab.key === active ? 0 : -1}
      onclick={() => onSelect(tab.key)}
    >
      {tab.label}
    </button>
  {/each}
  {#if accessory}
    <span class="accessory">{@render accessory()}</span>
  {/if}
</div>

<style>
  .tab-strip {
    display: flex;
    align-items: center;
    border-bottom: 1px solid var(--gs-border);
    background: var(--gs-bg);
    height: 26px;
    flex-shrink: 0;
  }
  .tab {
    background: transparent;
    border: none;
    color: var(--gs-fg-muted);
    height: 26px;
    padding: 0 12px;
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    cursor: pointer;
    border-bottom: 2px solid transparent;
  }
  .tab:hover {
    color: var(--gs-fg);
  }
  .tab.active {
    color: var(--gs-fg-bright);
    border-bottom-color: var(--gs-focus, #0969da);
  }
  .accessory {
    margin-left: auto;
    padding: 0 8px;
    display: inline-flex;
    align-items: center;
  }
</style>
