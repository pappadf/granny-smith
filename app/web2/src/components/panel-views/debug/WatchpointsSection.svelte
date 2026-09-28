<script lang="ts">
  // The Watchpoints section: debug.watchpoints, a memory logpoint that stops
  // the machine after the accessing instruction (#180).  Same shape as the
  // Breakpoints section; a row is an address range, its access mode and hits.
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { listWatchpoints, addWatchpoint, removeWatchpoint, type Watchpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';

  let rows = $state<Watchpoint[]>([]);
  let showAdd = $state(false);
  let addAddr = $state('');
  let addMode = $state<'write' | 'read' | 'rw'>('write');
  let addWidth = $state<'' | 'b' | 'w' | 'l'>('l');

  // Only the latest listing is shown (see BreakpointsSection).
  let listGen = 0;
  async function refresh() {
    const gen = ++listGen;
    const list = await listWatchpoints();
    if (gen === listGen) rows = list;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    if (debug.sections.watchpoints) void refresh();
  });

  function onAddrKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      void commitAdd();
    } else if (ev.key === 'Escape') {
      cancelAdd();
    }
  }

  async function commitAdd() {
    const v = parseHex(addAddr);
    if (v === null) {
      showNotification('Invalid watchpoint address', 'error');
      return;
    }
    const ok = await addWatchpoint(v, addMode, addWidth);
    if (!ok) {
      showNotification('Failed to add watchpoint', 'error');
      return;
    }
    showNotification(`Watchpoint set at $${fmtHex32(v)}`, 'info');
    addAddr = '';
    showAdd = false;
    await refresh();
  }

  function cancelAdd() {
    addAddr = '';
    showAdd = false;
  }

  function onRowContext(row: Watchpoint, ev: MouseEvent) {
    ev.preventDefault();
    const items: ContextMenuItem[] = [
      {
        label: 'Remove',
        danger: true,
        action: () => void doRemove(row),
      },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function doRemove(row: Watchpoint) {
    const ok = await removeWatchpoint(row.id);
    if (!ok) {
      showNotification('Failed to remove watchpoint', 'error');
      return;
    }
    await refresh();
  }

  // "$0000016A" for one address, "$0000016A-$0000016D" for a range.
  function rangeLabel(r: Watchpoint): string {
    return r.end !== r.addr ? `$${fmtHex32(r.addr)}-$${fmtHex32(r.end)}` : `$${fmtHex32(r.addr)}`;
  }
</script>

<CollapsibleSection
  title="Watchpoints"
  open={debug.sections.watchpoints}
  onToggle={() => toggleSection('watchpoints')}
>
  {#snippet actions()}
    <button
      type="button"
      class="add-btn"
      title="Add watchpoint"
      aria-label="Add watchpoint"
      onclick={() => (showAdd = true)}>+</button
    >
  {/snippet}
  {#if showAdd}
    <div class="add-row">
      <input
        type="text"
        class="add-addr"
        placeholder="address ($hex)"
        bind:value={addAddr}
        onkeydown={onAddrKey}
        aria-label="Watchpoint address"
      />
      <select class="add-mode" bind:value={addMode} aria-label="Watchpoint access">
        <option value="write">write</option>
        <option value="read">read</option>
        <option value="rw">read/write</option>
      </select>
      <select class="add-mode" bind:value={addWidth} aria-label="Watchpoint width">
        <option value="b">byte</option>
        <option value="w">word</option>
        <option value="l">long</option>
      </select>
      <button type="button" class="btn" onclick={commitAdd}>Add</button>
      <button type="button" class="btn" onclick={cancelAdd}>Cancel</button>
    </div>
  {/if}
  {#if rows.length === 0 && !showAdd}
    <p class="hint">No watchpoints. Click + to add one.</p>
  {:else}
    {#each rows as r (r.id)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div class="wp-row" oncontextmenu={(ev) => onRowContext(r, ev)}>
        <span class="enable">{r.enabled ? '●' : '○'}</span>
        <span class="addr">{rangeLabel(r)}</span>
        <span class="mode">{r.mode}</span>
        {#if r.hits > 0}
          <span class="hits">{r.hits}×</span>
        {/if}
      </div>
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .add-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 18px;
    height: 18px;
    padding: 0;
    border: none;
    background: transparent;
    color: var(--gs-fg-muted);
    cursor: pointer;
    font-size: 14px;
    line-height: 1;
  }
  .add-btn:hover,
  .add-btn:focus-visible {
    color: var(--gs-fg-bright);
  }
  .add-row {
    display: flex;
    gap: 6px;
    padding: 6px 12px;
  }
  .add-addr {
    width: 12ch;
  }
  .add-mode {
    flex: 1 1 auto;
  }
  .add-addr,
  .add-mode {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 6px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    outline: none;
  }
  .add-addr:focus,
  .add-mode:focus {
    border-color: var(--gs-focus);
  }
  .btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 8px;
    font-size: 11px;
    cursor: pointer;
  }
  .btn:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .hint {
    color: var(--gs-fg-muted);
    font-size: 11px;
    padding: 6px 12px;
  }
  .wp-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    color: var(--gs-fg);
  }
  .wp-row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .enable {
    color: var(--gs-fg-muted);
    width: 1ch;
  }
  .mode {
    color: var(--gs-fg-muted);
    font-style: italic;
  }
  .hits {
    color: var(--gs-fg-muted);
  }
</style>
