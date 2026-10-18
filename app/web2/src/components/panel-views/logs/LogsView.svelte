<script lang="ts">
  import Hint from '@/components/ui/Hint.svelte';
  import { logs } from '@/state/logs.svelte';
  import { logsPanelHeader } from './logsHeader.svelte';
  import LogLine from './LogLine.svelte';
  import CategoryLevelsPopover from './CategoryLevelsPopover.svelte';
  import { stickToBottom } from '@/lib/stickToBottom.svelte';

  // Zebra rows (--gs-row-alt): off by default.
  let { striped = false }: { striped?: boolean } = $props();

  const showPopover = $derived(logsPanelHeader.popoverOpen);

  // Autoscroll: while it is on and the list is at the bottom, new lines
  // keep it there; scrolling up unpins it until the user scrolls back
  // down, as in the Terminal (lib/stickToBottom).  Turning it on jumps to
  // the bottom.
  const scroll = stickToBottom(() => logs.entries.length);
  $effect(() => {
    if (logs.autoscroll) scroll.follow();
  });

  const catCount = $derived(new Set(logs.entries.map((e) => e.cat)).size);
</script>

<div class="logs-view">
  <div
    class="logs-scroll"
    data-striped={striped || undefined}
    {@attach logs.autoscroll ? scroll.attach : undefined}
  >
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
