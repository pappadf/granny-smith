<script lang="ts">
  import { layout, PANEL_TABS, type PanelTab } from '@/state/layout.svelte';
  import PanelTabComp from './PanelTab.svelte';

  // Display labels — spec §4 fixes this order and casing.
  const LABELS: Record<PanelTab, string> = {
    terminal: 'TERMINAL',
    machine: 'MACHINE',
    filesystem: 'FILESYSTEM',
    images: 'IMAGES',
    checkpoints: 'CHECKPOINTS',
    debug: 'DEBUG',
    logs: 'LOGS',
  };
</script>

<div class="gs-panel-header">
  <div class="panel-tabs" role="tablist" aria-label="Panel views">
    {#each PANEL_TABS as tab (tab)}
      <PanelTabComp {tab} label={LABELS[tab]} active={layout.activeTab === tab} />
    {/each}
  </div>
  <div class="panel-actions">
    <!-- Per-view action buttons land with their view components in later phases. -->
  </div>
</div>

<style>
  .gs-panel-header {
    height: 35px;
    flex: 0 0 35px;
    display: flex;
    align-items: stretch;
    background: var(--gs-bg);
    user-select: none;
    overflow: hidden;
  }
  .panel-tabs {
    display: flex;
    flex: 1 1 auto;
    min-width: 0;
    overflow-x: auto;
    scrollbar-width: thin;
  }
  .panel-tabs::-webkit-scrollbar {
    height: 3px;
  }
  .panel-tabs::-webkit-scrollbar-thumb {
    background: var(--gs-scrollbar-thumb);
  }
  .panel-actions {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 0 6px;
    flex-shrink: 0;
  }
</style>
