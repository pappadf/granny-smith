<script lang="ts">
  import { layout, PANEL_TABS, type PanelTab } from '@/state/layout.svelte';
  import PanelTabComp from './PanelTab.svelte';
  import { logs, clearLogs, downloadLogs, setAutoscroll } from '@/state/logs.svelte';
  import { toggleLogsPopover } from '../panel-views/logs/logsHeader.svelte';
  import CreateCheckpointButton from '../panel-views/checkpoints/CreateCheckpointButton.svelte';
  import DebugToolbar from '../panel-views/debug/DebugToolbar.svelte';

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
    {#if layout.activeTab === 'logs'}
      <button
        type="button"
        class="action-btn"
        onclick={() => toggleLogsPopover()}
        title="Set per-category log levels"
      >
        Levels
      </button>
      <label class="action-toggle" title="Scroll to newest line automatically">
        <input
          type="checkbox"
          checked={logs.autoscroll}
          onchange={(e) => setAutoscroll((e.target as HTMLInputElement).checked)}
        />
        autoscroll
      </label>
      <button
        type="button"
        class="action-btn"
        onclick={() => clearLogs()}
        title="Clear the log buffer"
      >
        Clear
      </button>
      <button
        type="button"
        class="action-btn"
        onclick={() => downloadLogs()}
        title="Download the log buffer as text"
      >
        Download
      </button>
    {:else if layout.activeTab === 'checkpoints'}
      <CreateCheckpointButton />
    {:else if layout.activeTab === 'debug'}
      <DebugToolbar />
    {/if}
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
    gap: 6px;
    padding: 0 8px;
    flex-shrink: 0;
  }
  .action-btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 8px;
    font-size: 11px;
    cursor: pointer;
  }
  .action-btn:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .action-toggle {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    color: var(--gs-fg-muted);
    font-size: 11px;
    cursor: pointer;
    user-select: none;
  }
  .action-toggle input {
    margin: 0;
  }
</style>
