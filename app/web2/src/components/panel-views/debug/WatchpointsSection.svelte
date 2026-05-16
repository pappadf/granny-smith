<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import {
    watchpoints,
    addWatchpoint,
    removeWatchpoint,
    toggleWatchpoint,
    type Watchpoint,
  } from '@/bus/mockWatchpoints.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32, parseHex } from '@/lib/hex';

  let showAdd = $state(false);
  let addLo = $state('');
  let addHi = $state('');
  let addMode = $state<'r' | 'w' | 'rw'>('rw');

  function commitAdd() {
    const lo = parseHex(addLo);
    const hi = parseHex(addHi);
    if (lo === null || hi === null || hi < lo) {
      showNotification('Invalid watchpoint range', 'error');
      return;
    }
    addWatchpoint(lo, hi, addMode);
    showNotification(
      `Watchpoint set on $${fmtHex32(lo)} – $${fmtHex32(hi)} (${addMode.toUpperCase()})`,
      'info',
    );
    addLo = '';
    addHi = '';
    addMode = 'rw';
    showAdd = false;
  }

  function cancelAdd() {
    addLo = '';
    addHi = '';
    addMode = 'rw';
    showAdd = false;
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      commitAdd();
    } else if (ev.key === 'Escape') {
      cancelAdd();
    }
  }

  function onRowContext(w: Watchpoint, ev: MouseEvent) {
    ev.preventDefault();
    const items: ContextMenuItem[] = [
      { label: w.enabled ? 'Disable' : 'Enable', action: () => toggleWatchpoint(w.id) },
      { sep: true },
      { label: 'Remove', danger: true, action: () => removeWatchpoint(w.id) },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  function rangeLabel(w: Watchpoint): string {
    if (!machine.mmuEnabled) return `$${fmtHex32(w.lo)} – $${fmtHex32(w.hi)}`;
    const lr = mmuLookup(w.lo);
    const phyLo = lr.valid && lr.phys !== undefined ? fmtHex32(lr.phys) : '!';
    const phyHi = lr.valid && lr.phys !== undefined ? fmtHex32(lr.phys + (w.hi - w.lo)) : '!';
    const tag = lr.valid ? (lr.kind ?? 'PT') : 'INVALID';
    return `L:$${fmtHex32(w.lo)} – $${fmtHex32(w.hi)}  P:$${phyLo} – $${phyHi}  ${tag}`;
  }
</script>

<CollapsibleSection
  title="Watchpoints"
  open={debug.sections.watchpoints}
  onToggle={() => toggleSection('watchpoints')}
>
  {#snippet actions()}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <span
      role="button"
      tabindex="-1"
      class="add-btn"
      title="Add watchpoint"
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
        placeholder="lo ($hex)"
        bind:value={addLo}
        onkeydown={onKey}
        aria-label="Low address"
      />
      <input
        type="text"
        class="add-addr"
        placeholder="hi ($hex)"
        bind:value={addHi}
        onkeydown={onKey}
        aria-label="High address"
      />
      <div class="mode-toggle" role="group" aria-label="Watch mode">
        {#each ['r', 'w', 'rw'] as m (m)}
          <button
            type="button"
            class="mode-btn"
            class:active={addMode === m}
            onclick={() => (addMode = m as 'r' | 'w' | 'rw')}
          >
            {m.toUpperCase()}
          </button>
        {/each}
      </div>
      <button type="button" class="btn" onclick={commitAdd}>Add</button>
      <button type="button" class="btn" onclick={cancelAdd}>Cancel</button>
    </div>
  {/if}
  {#if watchpoints.entries.length === 0 && !showAdd}
    <p class="hint">No watchpoints. Click + to add one. (Mock — Phase 7 wires firing on access.)</p>
  {:else}
    {#each watchpoints.entries as w (w.id)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div class="wp-row" oncontextmenu={(ev) => onRowContext(w, ev)}>
        <span class="enable">{w.enabled ? '●' : '○'}</span>
        <span class="range">{rangeLabel(w)}</span>
        <span class="mode">{w.mode.toUpperCase()}</span>
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
    flex-wrap: wrap;
    gap: 6px;
    padding: 6px 12px;
    align-items: center;
  }
  .add-addr {
    width: 12ch;
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
  .add-addr:focus {
    border-color: var(--gs-focus);
  }
  .mode-toggle {
    display: inline-flex;
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    overflow: hidden;
    height: 22px;
  }
  .mode-btn {
    background: transparent;
    color: var(--gs-fg-muted);
    border: none;
    padding: 0 8px;
    font-size: 11px;
    cursor: pointer;
  }
  .mode-btn.active {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
    color: var(--gs-fg-bright);
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
  .wp-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    color: var(--gs-fg);
  }
  .wp-row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .enable {
    color: var(--gs-fg-muted);
  }
  .mode {
    color: var(--gs-fg-muted);
    font-weight: 600;
  }
</style>
