<script lang="ts">
  import { logs } from '@/state/logs.svelte';
  import { logsPanelHeader } from './logsHeader.svelte';
  import LogLine from './LogLine.svelte';
  import CategoryLevelsPopover from './CategoryLevelsPopover.svelte';

  let listEl = $state<HTMLDivElement | null>(null);
  const showPopover = $derived(logsPanelHeader.popoverOpen);

  // Autoscroll: any time the entries array changes (push or splice) and
  // autoscroll is on, snap the list to the bottom.
  $effect(() => {
    void logs.entries.length;
    if (!logs.autoscroll || !listEl) return;
    queueMicrotask(() => {
      if (listEl) listEl.scrollTop = listEl.scrollHeight;
    });
  });

  const catCount = $derived(new Set(logs.entries.map((e) => e.cat)).size);
</script>

<div class="logs-view">
  <div class="logs-scroll" bind:this={listEl}>
    {#if logs.entries.length === 0}
      <p class="logs-empty">
        No log lines yet. Boot a machine and bring a category up with <code
          >log &lt;cat&gt; &lt;level&gt;</code
        >
        in the terminal, or use the <strong>Levels</strong> button above.
      </p>
    {:else}
      {#each logs.entries as entry, i (i)}
        <LogLine {entry} />
      {/each}
    {/if}
  </div>
  <div class="logs-status">
    {logs.entries.length} lines · {catCount} categories · autoscroll: {logs.autoscroll
      ? 'on'
      : 'off'}
  </div>
</div>

<CategoryLevelsPopover open={showPopover} onClose={() => (logsPanelHeader.popoverOpen = false)} />

<style>
  .logs-view {
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .logs-scroll {
    flex: 1 1 auto;
    overflow-y: auto;
    min-height: 0;
    padding: 4px 0;
    background: var(--gs-bg);
  }
  .logs-empty {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 16px;
    line-height: 1.5;
  }
  .logs-status {
    flex: 0 0 auto;
    padding: 4px 12px;
    border-top: 1px solid var(--gs-border);
    color: var(--gs-fg-muted);
    font-size: 11px;
    background: var(--gs-bg-alt, var(--gs-bg));
  }
</style>
