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
    color: var(--gs-tab-inactive-fg);
    font-size: 11px;
    text-transform: uppercase;
    padding: 0 10px;
    line-height: 18px;
    height: 31px;
    display: inline-flex;
    align-items: center;
    align-self: center;
    cursor: pointer;
    white-space: nowrap;
  }
  .ptab:hover,
  .ptab.active {
    color: var(--gs-tab-active-fg);
  }
  .ptab.active::after {
    content: '';
    position: absolute;
    left: 10px;
    right: 10px;
    bottom: 4px;
    height: 0;
    border-top: 1px solid var(--gs-tab-active-fg);
    pointer-events: none;
  }
</style>
