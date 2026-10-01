<script lang="ts">
  import { layout, PANEL_TABS, setActiveTab, type PanelTab } from '@/state/layout.svelte';
  import Tabs from '../ui/Tabs.svelte';
  import { logs, clearLogs, downloadLogs, setAutoscroll } from '@/state/logs.svelte';
  import { toggleLogsPopover } from '../panel-views/logs/logsHeader.svelte';
  import CreateCheckpointButton from '../panel-views/checkpoints/CreateCheckpointButton.svelte';
  import DebugToolbar from '../panel-views/debug/DebugToolbar.svelte';
  import Button from '../ui/Button.svelte';
  import Checkbox from '../ui/Checkbox.svelte';
  import IconButton from '../ui/IconButton.svelte';
  import { createCheckpoint } from '../panel-views/checkpoints/CreateCheckpointButton.svelte';
  import { openContextMenu, type ContextMenuItem } from '../common/ContextMenu.svelte';
  import { machine } from '@/state/machine.svelte';
  import { continueExec, pauseExec, stepInto, stopMachine, restart } from '@/bus/debug';

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

  // When the panel is too narrow for the active tab beside the view's
  // actions, the actions fold into one "⋯" menu.  Each view's inline width
  // is remembered from when it last showed inline (it is shown inline once
  // to learn it).
  let headerEl = $state<HTMLDivElement | null>(null);
  let actionsEl = $state<HTMLDivElement | null>(null);
  let headerWidth = $state(Infinity);
  let tabsMin = $state(0);
  let actionWidths = $state<Partial<Record<PanelTab, number>>>({});
  // A skin may put the actions on a row of their own (Aqua): then they only
  // fold when they are wider than the header itself.
  let ownRow = $state(false);
  // What the header spends on neither the tabs nor the actions (padding,
  // gaps), learnt while the actions show inline.
  let overhead = $state(0);
  const hasActions = $derived(
    layout.activeTab === 'logs' ||
      layout.activeTab === 'checkpoints' ||
      layout.activeTab === 'debug',
  );
  const folded = $derived.by(() => {
    const w = actionWidths[layout.activeTab];
    if (!hasActions || w === undefined) return false;
    return ownRow ? w > headerWidth : headerWidth - w - overhead < tabsMin;
  });

  $effect(() => {
    if (!headerEl || typeof ResizeObserver === 'undefined') return;
    const el = headerEl;
    const ro = new ResizeObserver(() => (headerWidth = el.getBoundingClientRect().width));
    ro.observe(el);
    return () => ro.disconnect();
  });
  $effect(() => {
    if (!actionsEl || typeof ResizeObserver === 'undefined') return;
    const el = actionsEl;
    const tab = layout.activeTab;
    const ro = new ResizeObserver(() => {
      if (!el.offsetWidth) return;
      actionWidths[tab] = el.offsetWidth;
      const strip = headerEl?.querySelector<HTMLElement>('[role="tablist"]');
      ownRow =
        !!strip &&
        (el.offsetTop >= strip.offsetTop + strip.offsetHeight ||
          strip.offsetTop >= el.offsetTop + el.offsetHeight);
      if (strip && !ownRow && headerEl) {
        const cs = getComputedStyle(headerEl);
        const pad = (parseFloat(cs.paddingLeft) || 0) + (parseFloat(cs.paddingRight) || 0);
        const ms = getComputedStyle(strip);
        overhead = pad + (parseFloat(ms.marginLeft) || 0) + (parseFloat(ms.marginRight) || 0);
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  });

  function actionItems(): ContextMenuItem[] {
    switch (layout.activeTab) {
      case 'logs':
        return [
          { label: 'Levels…', action: () => toggleLogsPopover() },
          {
            label: 'Autoscroll',
            checked: logs.autoscroll,
            action: () => setAutoscroll(!logs.autoscroll),
          },
          { sep: true },
          { label: 'Clear', action: () => clearLogs() },
          { label: 'Download', icon: 'download', action: () => downloadLogs() },
        ];
      case 'checkpoints':
        return [{ label: 'Create Checkpoint', action: () => void createCheckpoint() }];
      case 'debug':
        return [
          machine.status === 'running'
            ? { label: 'Pause', icon: 'pause', action: () => void pauseExec() }
            : { label: 'Continue', icon: 'play', action: () => void continueExec() },
          {
            label: 'Step Into',
            icon: 'step-into',
            disabled: machine.status !== 'paused',
            action: () => void stepInto(1),
          },
          { label: 'Stop', icon: 'stop', action: () => void stopMachine() },
          { label: 'Restart', icon: 'restart', action: () => void restart() },
        ];
      default:
        return [];
    }
  }

  function openActions(ev: MouseEvent) {
    const r = (ev.currentTarget as HTMLElement).getBoundingClientRect();
    openContextMenu(actionItems(), r.left, r.bottom);
  }
</script>

<div class="gs-panel-header" bind:this={headerEl}>
  <Tabs
    bind:minWidth={tabsMin}
    variant="panel"
    class="panel-tabs"
    tabClass="ptab"
    label="Panel views"
    tabs={PANEL_TABS.map((key) => ({ key, label: LABELS[key] }))}
    active={layout.activeTab}
    onSelect={setActiveTab}
  />
  {#if folded}
    <div class="panel-actions">
      <IconButton
        class="actions-menu"
        tone="panel"
        icon="ellipsis"
        label="View actions"
        aria-haspopup="menu"
        onclick={openActions}
      />
    </div>
  {:else}
    <div class="panel-actions" bind:this={actionsEl}>
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
  {/if}
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
