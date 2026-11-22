<script lang="ts">
  import Hint from '@/components/ui/Hint.svelte';
  import Button from '@/components/ui/Button.svelte';
  import Checkbox from '@/components/ui/Checkbox.svelte';
  import Select from '@/components/ui/Select.svelte';
  import { logs, setLogsPaused, setCategoryShown } from '@/state/logs.svelte';
  import { visibleEntries, entryText } from '@/lib/logView';
  import { copyText } from '@/lib/clipboard';
  import { showNotification } from '@/state/toasts.svelte';
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
  // What the view shows: the buffer, or the lines it held when paused,
  // through the level and category filters (lib/logView).
  const source = $derived(logs.frozen ?? logs.entries);
  const shown = $derived(visibleEntries(source, { maxLevel: logs.maxLevel, hidden: logs.hidden }));
  const scroll = stickToBottom(() => shown.length);
  $effect(() => {
    if (logs.autoscroll) scroll.follow();
  });

  const cats = $derived([...new Set(logs.entries.map((e) => e.cat))].sort());
  const catCount = $derived(cats.length);
  const hiddenCount = $derived(cats.filter((c) => logs.hidden[c]).length);

  // The level filter's choices: all, or up to 0..9.
  const LEVELS = ['all', ...Array.from({ length: 10 }, (_, i) => String(i))];
  let levelChoice = $state(logs.maxLevel === null ? 'all' : String(logs.maxLevel));
  $effect(() => {
    logs.maxLevel = levelChoice === 'all' ? null : Number(levelChoice);
  });

  let catsOpen = $state(false);

  async function copyShown() {
    const ok = await copyText(shown.map(entryText).join('\n') + '\n');
    showNotification(
      ok ? `Copied ${shown.length} log lines` : 'The browser refused the clipboard',
      ok ? 'info' : 'error',
    );
  }
</script>

<div class="logs-view">
  <div class="logs-filter" role="toolbar" aria-label="Log view filters">
    <span class="lbl">Levels</span>
    <Select size="inline" bind:value={levelChoice} aria-label="Show log levels up to">
      {#each LEVELS as l (l)}<option value={l}>{l === 'all' ? 'all' : `≤ ${l}`}</option>{/each}
    </Select>
    <Button class="filter-btn" onclick={() => (catsOpen = !catsOpen)} aria-expanded={catsOpen}>
      Categories{hiddenCount ? ` (${hiddenCount} hidden)` : ''}
    </Button>
    <Checkbox
      size="sm"
      class="pause-toggle"
      title="Hold the view still; lines go on collecting"
      checked={logs.frozen !== null}
      onchange={setLogsPaused}
      label="pause"
    />
    <Button class="filter-btn" onclick={copyShown} title="Copy the lines shown">Copy</Button>
  </div>
  {#if catsOpen}
    <div class="logs-cats" aria-label="Categories shown">
      {#if cats.length === 0}
        <span class="lbl">No categories have logged yet.</span>
      {/if}
      {#each cats as c (c)}
        <Checkbox
          size="sm"
          class="cat-toggle"
          checked={!logs.hidden[c]}
          onchange={(v: boolean) => setCategoryShown(c, v)}
          label={c}
        />
      {/each}
    </div>
  {/if}
  <div
    class="logs-scroll"
    data-striped={striped || undefined}
    {@attach logs.autoscroll ? scroll.attach : undefined}
  >
    {#if shown.length === 0}
      <Hint class="logs-empty" inset="view">
        No log lines yet. Boot a machine and bring a category up with <code
          >log.set &lt;category&gt; &lt;level&gt;</code
        >
        in the terminal, or use the <strong>Levels</strong> button above.
      </Hint>
    {:else}
      {#each shown as entry, i (i)}
        <LogLine {entry} />
      {/each}
    {/if}
  </div>
  <Hint as="div" inset="statusline" class="logs-status">
    {shown.length === logs.entries.length
      ? `${logs.entries.length} lines`
      : `${shown.length} of ${logs.entries.length} lines`} · {catCount} categories · autoscroll: {logs.autoscroll
      ? 'on'
      : 'off'}{logs.frozen ? ' · paused' : ''}
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
  .logs-filter,
  .logs-cats {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--gs-space-1-5);
    padding: var(--gs-space-1) var(--gs-space-2);
    border-bottom: var(--gs-border-width) solid var(--gs-border);
    font-size: var(--gs-font-size-xs);
  }
  .lbl {
    color: var(--gs-text-muted);
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
