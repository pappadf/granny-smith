<script lang="ts">
  import Hint from '@/components/ui/Hint.svelte';
  import { logs } from '@/state/logs.svelte';
  import { logsPanelHeader } from './logsHeader.svelte';
  import LogLine from './LogLine.svelte';
  import CategoryLevelsPopover from './CategoryLevelsPopover.svelte';

  // Zebra rows (--gs-row-alt): off by default.
  let { striped = false }: { striped?: boolean } = $props();

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
  <div class="logs-scroll" data-striped={striped || undefined} bind:this={listEl}>
    {#if logs.entries.length === 0}
      <Hint class="logs-empty" inset="view">
        No log lines yet. Boot a machine and bring a category up with <code
          >log.set &lt;category&gt; &lt;level&gt;</code
        >
        in the terminal, or use the <strong>Levels</strong> button above.
      </Hint>
    {:else}
      {#each logs.entries as entry, i (i)}
        <LogLine {entry} />
      {/each}
    {/if}
  </div>
  <Hint as="div" inset="statusline" class="logs-status">
    {logs.entries.length} lines · {catCount} categories · autoscroll: {logs.autoscroll
      ? 'on'
      : 'off'}
  </Hint>
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
    padding: var(--gs-space-1) 0;
    background: var(--gs-surface-app);
  }
  .logs-scroll[data-striped] > :global(:nth-child(even)) {
    background: var(--gs-row-alt);
  }
</style>
