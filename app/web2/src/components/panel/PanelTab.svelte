<script lang="ts">
  import { setActiveTab, type PanelTab } from '@/state/layout.svelte';

  interface Props {
    tab: PanelTab;
    label: string;
    active: boolean;
  }
  let { tab, label, active }: Props = $props();
</script>

<button
  type="button"
  class="ptab"
  class:active
  data-tab={tab}
  role="tab"
  aria-selected={active}
  onclick={() => setActiveTab(tab)}
>
  {label}
</button>

<style>
  .ptab {
    position: relative;
    background: transparent;
    border: none;
    color: var(--gs-tab-fg);
    font-size: var(--gs-font-size-xs);
    text-transform: var(--gs-caps-transform);
    padding: 0 var(--gs-space-2-5);
    line-height: 18px;
    height: var(--gs-size-tab);
    display: inline-flex;
    align-items: center;
    align-self: center;
    cursor: pointer;
    white-space: nowrap;
  }
  .ptab:hover,
  .ptab.active {
    color: var(--gs-tab-fg-selected);
  }
  .ptab.active::after {
    content: '';
    position: absolute;
    left: 10px;
    right: 10px;
    bottom: 4px;
    height: 0;
    border-top: var(--gs-border-width) solid var(--gs-tab-fg-selected);
    pointer-events: none;
  }
  .ptab:focus {
    outline: none;
  }
  .ptab:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: -2px;
  }
</style>
