<script lang="ts">
  import { layout, PANEL_TABS, type PanelTab } from '@/state/layout.svelte';
  import PanelTabComp from './PanelTab.svelte';
  import { logs, clearLogs, downloadLogs, setAutoscroll } from '@/state/logs.svelte';
  import { toggleLogsPopover } from '../panel-views/logs/logsHeader.svelte';
  import CreateCheckpointButton from '../panel-views/checkpoints/CreateCheckpointButton.svelte';
  import DebugToolbar from '../panel-views/debug/DebugToolbar.svelte';

  // Display labels, in this order and casing.
  const LABELS: Record<PanelTab, string> = {
    terminal: 'TERMINAL',
    machine: 'SYSTEM',
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
    height: var(--gs-size-toolbar);
    flex: 0 0 var(--gs-size-toolbar);
    display: flex;
    align-items: stretch;
    background: var(--gs-surface-app);
    user-select: none;
    overflow: hidden;
  }
  .panel-tabs {
    display: flex;
    flex: 1 1 auto;
    min-width: 0;
    /* Scroll horizontally with a hidden scrollbar (VS Code style). overflow-y
       must be hidden explicitly: auto on one axis forces visible->auto on the
       other, and on systems with classic (space-taking) scrollbars the
       horizontal bar shrinks the 35px header below the 31px tab height,
       cascading into both scrollbars appearing. */
    overflow-x: auto;
    overflow-y: hidden;
    scrollbar-width: none;
  }
  .panel-tabs::-webkit-scrollbar {
    display: none;
  }
  .panel-actions {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1-5);
    padding: 0 var(--gs-space-2);
    flex-shrink: 0;
  }
  .action-btn {
    background: transparent;
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    height: var(--gs-size-control);
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
  }
  .action-btn:hover {
    background: var(--gs-row-hover);
  }
  .action-toggle {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-1);
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
    user-select: none;
  }
  .action-toggle input {
    margin: 0;
  }
</style>
