<script lang="ts">
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { listBreakpoints, addBreakpoint, removeBreakpoint, type Breakpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { translateMany, addrLabel, type Translation } from '@/bus/mmu';
  import { fmtHex32, parseHex } from '@/lib/hex';

  let rows = $state<Breakpoint[]>([]);
  let showAdd = $state(false);
  let addAddr = $state('');
  let addCond = $state('');

  // Only the latest listing is shown.  A listing is several round trips, so
  // one started before a remove (the refresh after a repeated add, say) can
  // finish after the remove's own refresh and would put the row back.
  let listGen = 0;
  async function refresh() {
    const gen = ++listGen;
    const list = await listBreakpoints();
    if (gen === listGen) rows = list;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    if (debug.sections.breakpoints) void refresh();
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
      showNotification('Invalid breakpoint address', 'error');
      return;
    }
    const ok = await addBreakpoint(v, addCond.trim() || undefined);
    if (!ok) {
      showNotification('Failed to add breakpoint', 'error');
      return;
    }
    showNotification(`Breakpoint set at $${fmtHex32(v)}`, 'info');
    addAddr = '';
    addCond = '';
    showAdd = false;
    await refresh();
  }

  function cancelAdd() {
    addAddr = '';
    addCond = '';
    showAdd = false;
  }

  function onRowContext(row: Breakpoint, ev: MouseEvent) {
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

  async function doRemove(row: Breakpoint) {
    const ok = await removeBreakpoint(row.id);
    if (!ok) {
      showNotification('Failed to remove breakpoint', 'error');
      return;
    }
    await refresh();
  }

  // Real translations for the L:/P: labels (the core's, bus/mmu.ts).
  let xl = $state<Record<number, Translation>>({});
  $effect(() => {
    const addrs = rows.map((r) => r.addr);
    if (!machine.mmuEnabled || addrs.length === 0) return;
    void translateMany(addrs).then((m) => (xl = m));
  });

  function labelFor(addr: number): string {
    if (!machine.mmuEnabled) return `$${fmtHex32(addr)}`;
    return addrLabel(addr, xl[addr >>> 0]);
  }
</script>

<CollapsibleSection
  title="Breakpoints"
  open={debug.sections.breakpoints}
  onToggle={() => toggleSection('breakpoints')}
>
  {#snippet actions()}
    <button
      type="button"
      class="add-btn"
      title="Add breakpoint"
      aria-label="Add breakpoint"
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
        aria-label="Breakpoint address"
      />
      <input
        type="text"
        class="add-cond"
        placeholder="condition (optional)"
        bind:value={addCond}
        onkeydown={onAddrKey}
        aria-label="Breakpoint condition"
      />
      <button type="button" class="btn" onclick={commitAdd}>Add</button>
      <button type="button" class="btn" onclick={cancelAdd}>Cancel</button>
    </div>
  {/if}
  {#if rows.length === 0 && !showAdd}
    <p class="hint">No breakpoints. Click + to add one.</p>
  {:else}
    {#each rows as r (r.id)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div class="bp-row" oncontextmenu={(ev) => onRowContext(r, ev)}>
        <span class="enable">{r.enabled ? '●' : '○'}</span>
        <span class="addr">{labelFor(r.addr)}</span>
        {#if r.condition}
          <span class="cond">if {r.condition}</span>
        {/if}
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
  .add-cond {
    flex: 1 1 auto;
  }
  .add-addr,
  .add-cond {
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
  .add-cond:focus {
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
  .bp-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    color: var(--gs-fg);
  }
  .bp-row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .enable {
    color: var(--gs-fg-muted);
    width: 1ch;
  }
  .cond {
    color: var(--gs-fg-muted);
    font-style: italic;
  }
  .hits {
    color: var(--gs-fg-muted);
  }
</style>
