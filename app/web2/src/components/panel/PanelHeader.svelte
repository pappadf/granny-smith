<script lang="ts">
  import { layout, PANEL_TABS, setActiveTab, type PanelTab } from '@/state/layout.svelte';
  import Tabs from '../ui/Tabs.svelte';
  import { logs, clearLogs, downloadLogs, setAutoscroll } from '@/state/logs.svelte';
  import { toggleLogsPopover } from '../panel-views/logs/logsHeader.svelte';
  import CreateCheckpointButton from '../panel-views/checkpoints/CreateCheckpointButton.svelte';
  import DebugToolbar from '../panel-views/debug/DebugToolbar.svelte';
  import Button from '../ui/Button.svelte';
  import Checkbox from '../ui/Checkbox.svelte';

  // Display labels, in this order.  Written in title case; the skin's
  // --gs-tab-transform decides whether they show in capitals.
  const LABELS: Record<PanelTab, string> = {
    terminal: 'Terminal',
    machine: 'System',
    filesystem: 'Filesystem',
    images: 'Images',
    checkpoints: 'Checkpoints',
    debug: 'Debug',
    logs: 'Logs',
  };
</script>

<div class="gs-panel-header">
  <Tabs
    variant="panel"
    class="panel-tabs"
    tabClass="ptab"
    label="Panel views"
    tabs={PANEL_TABS.map((key) => ({ key, label: LABELS[key] }))}
    active={layout.activeTab}
    onSelect={setActiveTab}
  />
  <div class="panel-actions">
    {#if layout.activeTab === 'logs'}
      <Button
        class="action-btn"
        onclick={() => toggleLogsPopover()}
        title="Set per-category log levels"
      >
        Levels
      </Button>
      <Checkbox
        size="sm"
        class="action-toggle"
        title="Scroll to newest line automatically"
        checked={logs.autoscroll}
        onchange={setAutoscroll}
        label="autoscroll"
      />
      <Button class="action-btn" onclick={() => clearLogs()} title="Clear the log buffer">
        Clear
      </Button>
      <Button
        class="action-btn"
        onclick={() => downloadLogs()}
        title="Download the log buffer as text"
      >
        Download
      </Button>
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
  .panel-actions {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1-5);
    padding: 0 var(--gs-space-2);
    flex-shrink: 0;
  }
</style>
