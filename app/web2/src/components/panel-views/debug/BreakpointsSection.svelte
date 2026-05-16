<script lang="ts">
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { listBreakpoints, addBreakpoint, removeBreakpoint, type Breakpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32, parseHex } from '@/lib/hex';

  let rows = $state<Breakpoint[]>([]);
  let showAdd = $state(false);
  let addAddr = $state('');
  let addCond = $state('');

  async function refresh() {
    rows = await listBreakpoints();
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
    const ok = await removeBreakpoint(row.addr);
    if (!ok) {
      showNotification('Failed to remove breakpoint', 'error');
      return;
    }
    await refresh();
  }

  function labelFor(addr: number): string {
    if (!machine.mmuEnabled) return `$${fmtHex32(addr)}`;
    const r = mmuLookup(addr);
    const phys = r.valid && r.phys !== undefined ? fmtHex32(r.phys) : '!';
    const tag = r.valid ? (r.kind ?? 'PT') : 'INVALID';
    return `L:$${fmtHex32(addr)}  P:$${phys}  ${tag}`;
  }
</script>

<CollapsibleSection
  title="Breakpoints"
  open={debug.sections.breakpoints}
  onToggle={() => toggleSection('breakpoints')}
>
  {#snippet actions()}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <span
      role="button"
      tabindex="-1"
      class="add-btn"
      title="Add breakpoint"
      onclick={(ev: MouseEvent) => {
        ev.stopPropagation();
        showAdd = true;
      }}>+</span
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
    color: var(--gs-fg-muted);
    cursor: pointer;
    font-size: 14px;
    line-height: 1;
  }
  .add-btn:hover {
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
    font-size: 12px;
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
    font-size: 12px;
    padding: 6px 12px;
  }
  .bp-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
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
