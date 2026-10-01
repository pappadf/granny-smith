<script lang="ts">
  // The command browser's details pane: a method's or attribute's usage
  // text (shell.usage), its signature and example lines coloured by
  // shell.highlight, the argument the console's cursor is in underlined.
  // Shows nothing until the text is there, or for a row without usage.
  import type { BrowserRow } from '@/lib/commandsTree';
  import { highlightUsage, loadUsageInfo, usageRuns, type UsageInfo } from '@/lib/usage';
  import type { HlSpan } from '@/lib/highlight';

  interface Props {
    row: BrowserRow;
    // The argument to underline (an index into the signature's arguments).
    markArg: number | null;
    onClose: () => void;
    onInsert: (row: BrowserRow) => void;
    // The text arrived (the pane grew).
    onShown?: () => void;
  }

  let { row, markArg, onClose, onInsert, onShown }: Props = $props();

  const key = $derived(row.key);
  const path = $derived(row.kind === 'method' || row.kind === 'attr' ? row.path : '');

  // The row's usage and its colours: one load per row, dropped when the row
  // changes before it lands.
  let loaded = $state.raw<{
    key: string;
    usage: UsageInfo;
    hl: Record<number, HlSpan[]>;
  } | null>(null);

  $effect(() => {
    const k = key;
    const p = path;
    let live = true;
    void (async () => {
      const usage = p ? await loadUsageInfo(p) : null;
      if (!live) return;
      loaded = usage ? { key: k, usage, hl: {} } : null;
      if (!usage) return;
      onShown?.();
      const hl = await highlightUsage(usage);
      if (live) loaded = { key: k, usage, hl };
    })();
    return () => {
      live = false;
    };
  });

  const lines = $derived(
    loaded && loaded.key === key ? usageRuns(loaded.usage, loaded.hl, markArg) : null,
  );
  const NEWLINE = '\n';
</script>

{#if lines}
  <section class="details" aria-label="Usage">
    <header class="details-head">
      <span class="details-name">{row.name}</span>
      <button class="details-close" aria-label="Close" title="Close (Esc)" onclick={onClose}
        >×</button
      >
    </header>
    <pre
      class="usage">{#each lines as runs, li (li)}{#if li > 0}{NEWLINE}{/if}{#each runs as r, ri (ri)}{#if r.mark}<mark
              class="usage-arg {r.cls ? `hl-${r.cls}` : ''}">{r.text}</mark
            >{:else if r.cls}<span class="hl-{r.cls}">{r.text}</span
            >{:else}{r.text}{/if}{/each}{/each}</pre>
    {#if row.insert}
      <footer class="details-foot">
        <button
          class="details-insert"
          title="Insert into the console (double-click, or Enter)"
          onclick={() => onInsert(row)}>Insert</button
        >
      </footer>
    {/if}
  </section>
{/if}

<style>
  /* The selection's usage, under the tree.  It sizes to its content up to
     60% of the browser; a longer usage text scrolls inside it, with the
     header and Insert button kept in view. */
  .details {
    flex: 0 0 auto;
    max-height: 60%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    border-top: 1px solid var(--gs-border);
    background: var(--gs-callout-bg);
  }
  .details-head {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 2px 4px 0 10px;
  }
  .details-name {
    font-family: var(--gs-font-mono);
    font-size: 12px;
    color: var(--gs-text-muted);
  }
  .details-close {
    border: none;
    background: transparent;
    color: var(--gs-text-muted);
    font-size: 16px;
    line-height: 1;
    padding: 2px 6px;
    cursor: pointer;
  }
  .details-close:hover {
    color: var(--gs-text);
  }
  .details-foot {
    flex: 0 0 auto;
    display: flex;
    justify-content: flex-end;
    padding: 0 8px 6px;
  }
  .details-insert {
    font-size: 12px;
    padding: 2px 12px;
    border-radius: 3px;
    border: 1px solid var(--gs-accent);
    background: var(--gs-accent-subtle);
    color: var(--gs-text-strong);
    cursor: pointer;
  }
  .usage {
    flex: 0 1 auto;
    min-height: 0;
    margin: 0;
    padding: 4px 10px 6px;
    overflow: auto;
    font-size: 12px;
    line-height: 1.4;
    white-space: pre-wrap;
    color: var(--gs-text);
    font-family: var(--gs-font-mono);
  }
  .usage-arg {
    background: none;
    color: inherit;
    text-decoration: underline;
    text-decoration-thickness: 2px;
  }
  .usage-arg:empty {
    display: none;
  }
</style>
