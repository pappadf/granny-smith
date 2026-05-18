<script lang="ts">
  import { onMount, tick } from 'svelte';
  import { loadDebugFrame, addBreakpoint, removeBreakpoint, type DebugFrameRow } from '@/bus/debug';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug, inspectMmuWalk, inspectMemoryAt } from '@/state/debug.svelte';
  import { fmtHex32 } from '@/lib/hex';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';

  const ROWS = 32;
  // Bytes to fetch *before* PC. 68K instructions are variable length so
  // PC ends up at a variable row index (usually 4–8). The scroll
  // anchor below normalises that visually — PC always sits at line 5
  // from the top regardless of how many bytes the leading instructions
  // consume.
  const BACK_BYTES = 16;
  // Row position (1-indexed) where the PC row should appear after a
  // refresh. Pre-PC rows stay scrollable for context.
  const PC_ANCHOR_LINE = 5;
  // Single source of truth for row height — must match `.row { height: ... }`.
  const ROW_HEIGHT_PX = 22;

  let rows = $state<DebugFrameRow[]>([]);
  let pc = $state(0);

  async function refresh() {
    // One bridge round-trip: registers + disasm rows + real per-row
    // MMU translation. Replaces the previous ~21-call fan-out
    // (readRegisters + disasmAt + mockMmu lookups per row).
    const start = pc > 0 ? Math.max(0, (pc - BACK_BYTES) >>> 0) : undefined;
    const frame = await loadDebugFrame(start, ROWS);
    if (!frame) return;
    pc = frame.regs.pc;
    debug.currentPc = pc;
    rows = frame.rows;
    // Wait for the DOM to reflect the new rows, then anchor PC at the
    // configured line. Without this, scrollTop is computed against
    // stale layout.
    await tick();
    anchorPcRow();
  }

  // Scroll the pane so the PC row sits at the configured anchor line.
  // No-op when PC isn't in the current window (start-of-day before the
  // first frame loads).
  function anchorPcRow(): void {
    if (!paneEl) return;
    const pcIdx = rows.findIndex((r) => r.addr === pc);
    if (pcIdx < 0) return;
    const target = (pcIdx - (PC_ANCHOR_LINE - 1)) * ROW_HEIGHT_PX;
    paneEl.scrollTop = Math.max(0, target);
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    // Re-fetch on pause / running transitions and after each Step.
    // `refreshGen` is bumped by stepInto so Steps trigger a refresh
    // even when the status is unchanged (paused → paused).
    void machine.status;
    void debug.refreshGen;
    void refresh();
  });

  function bannerLabel(): string {
    const model = machine.model ?? 'Machine';
    if (!machine.mmuEnabled) {
      return `${model} · PC at $${fmtHex32(pc)}`;
    }
    // Find the row at PC in the current frame — its phys/valid come
    // straight from the C-side MMU walk, no second round-trip.
    const pcRow = rows.find((r) => r.addr === pc);
    if (!pcRow || !pcRow.valid) return `${model} · PC at L:$${fmtHex32(pc)} (no MMU mapping)`;
    return `${model} · PC at L:$${fmtHex32(pc)} P:$${fmtHex32(pcRow.phys ?? 0)}`;
  }

  function rowAddrLabel(row: DebugFrameRow): { logical: string; physical?: string; tag?: string } {
    if (!machine.mmuEnabled) {
      return { logical: `$${fmtHex32(row.addr)}` };
    }
    if (!row.valid) return { logical: `L:$${fmtHex32(row.addr)}`, tag: 'INVALID' };
    return {
      logical: `L:$${fmtHex32(row.addr)}`,
      physical: `P:$${fmtHex32(row.phys ?? 0)}`,
    };
  }

  function onRowContext(row: DebugFrameRow, ev: MouseEvent) {
    ev.preventDefault();
    const mmuOn = machine.mmuEnabled;
    const items: ContextMenuItem[] = [
      {
        label: `Add breakpoint at $${fmtHex32(row.addr)}`,
        action: async () => {
          const ok = await addBreakpoint(row.addr);
          if (!ok) showNotification('Failed to add breakpoint', 'error');
        },
      },
      {
        label: `Remove breakpoint at $${fmtHex32(row.addr)}`,
        action: async () => {
          const ok = await removeBreakpoint(row.addr);
          if (!ok) showNotification('Failed to remove breakpoint', 'error');
        },
      },
      { sep: true },
      {
        label: 'Copy logical address',
        action: () => {
          if (navigator.clipboard?.writeText)
            void navigator.clipboard.writeText(`$${fmtHex32(row.addr)}`);
        },
      },
    ];
    if (mmuOn) {
      items.push({
        label: 'Copy physical address',
        action: () => {
          if (row.valid && row.phys !== null && navigator.clipboard?.writeText)
            void navigator.clipboard.writeText(`$${fmtHex32(row.phys)}`);
        },
      });
      items.push({
        label: 'Show MMU walk for this address',
        action: () => inspectMmuWalk(row.addr),
      });
    }
    items.push({ sep: true });
    items.push({
      label: 'Inspect bytes here',
      action: () => inspectMemoryAt(row.addr),
    });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Keyboard navigation: ↑/↓ moves selected row, PgUp/PgDn pages,
  // Home jumps to PC.
  let selectedIdx = $state(-1);
  let paneEl = $state<HTMLDivElement | null>(null);

  // When PC changes, re-center selected on the PC row.
  $effect(() => {
    void pc;
    const pcIdx = rows.findIndex((r) => r.addr === pc);
    if (pcIdx >= 0) selectedIdx = pcIdx;
  });

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Home') {
      const pcIdx = rows.findIndex((r) => r.addr === pc);
      if (pcIdx >= 0) {
        ev.preventDefault();
        selectedIdx = pcIdx;
        scrollRowIntoView(pcIdx);
      }
      return;
    }
    const k = listKeyFromEvent(ev);
    if (!k) return;
    // Ignore horizontal keys — they're not meaningful for a flat row list.
    if (k === 'ArrowLeft' || k === 'ArrowRight') return;
    const next = cycleListSelection(rows.length, selectedIdx, k, { pageSize: 10 });
    if (next === selectedIdx) return;
    ev.preventDefault();
    selectedIdx = next;
    scrollRowIntoView(next);
  }

  function scrollRowIntoView(i: number): void {
    if (!paneEl) return;
    const rowEls = paneEl.querySelectorAll<HTMLElement>('.row');
    rowEls[i]?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
  class="disasm-pane"
  role="application"
  aria-label="Disassembly"
  bind:this={paneEl}
  tabindex="0"
  onkeydown={onKey}
>
  <div class="banner">{bannerLabel()}</div>
  {#if machine.status === 'running'}
    <p class="hint">Pause the machine to see the disasm listing.</p>
  {:else if rows.length === 0}
    <p class="hint">Pause the machine to see the disasm listing.</p>
  {:else}
    {#each rows as row, i (row.addr * 100 + i)}
      {@const isPc = row.addr === pc}
      {@const addr = rowAddrLabel(row)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="row"
        class:pc={isPc}
        class:selected={selectedIdx === i}
        oncontextmenu={(ev) => onRowContext(row, ev)}
      >
        <span class="marker">{isPc ? '►' : ''}</span>
        <!-- Address group is one grid cell so the mnem/ops columns
             stay aligned across rows even when addr-p / tag are
             absent. Without the wrapper, missing optional spans
             would shift mnem to an earlier column on some rows. -->
        <span class="addr">
          <span class="addr-l">{addr.logical}</span>
          {#if addr.physical}<span class="addr-p">{addr.physical}</span>{/if}
          {#if addr.tag}<span class="tag tag-{addr.tag.toLowerCase()}">{addr.tag}</span>{/if}
        </span>
        <span class="mnem">{row.mnem}</span>
        <span class="ops">{row.ops}</span>
      </div>
    {/each}
  {/if}
</div>

<style>
  .disasm-pane {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    /* 11 px matches the body-text baseline used by section headers
       and the MMU descriptor lines; disasm rows shouldn't read larger
       than the surrounding chrome. */
    font-size: 11px;
  }
  .banner {
    position: sticky;
    top: 0;
    z-index: 1;
    /* Opaque so disasm rows scrolling underneath don't bleed
       through; tinted border-left preserves the blue indicator. */
    background: var(--gs-bg-alt);
    border-left: 2px solid var(--gs-focus, #0969da);
    border-bottom: 1px solid var(--gs-border);
    color: var(--gs-fg);
    font-size: 11px;
    padding: 4px 12px;
  }
  .hint {
    color: var(--gs-fg-muted);
    padding: 12px;
    font-size: 11px;
  }
  .row {
    /* Four stable columns: PC marker, address group (logical / phys /
       tag inline), mnemonic, operands. mnem auto-sizes to the longest
       mnemonic across all rows, so operands always start at the same
       x. ops gets `1fr` to take the rest. */
    display: grid;
    grid-template-columns: 14px auto auto 1fr;
    column-gap: 8px;
    align-items: center;
    height: 22px;
    padding: 0 8px 0 4px;
    line-height: 22px;
    cursor: default;
    white-space: nowrap;
  }
  .addr {
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .row.pc {
    background: rgba(80, 140, 220, 0.2);
  }
  .row.selected {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
  .disasm-pane:focus {
    outline: none;
  }
  .disasm-pane:focus-visible {
    outline: 1px solid var(--gs-focus, #0969da);
    outline-offset: -1px;
  }
  .marker {
    color: var(--gs-focus, #0969da);
    text-align: center;
  }
  .addr-l,
  .addr-p {
    color: var(--gs-fg-muted);
    text-transform: uppercase;
  }
  .tag {
    border-radius: 9999px;
    padding: 0 6px;
    font-size: 10px;
    font-weight: 600;
    line-height: 14px;
    height: 14px;
    text-transform: uppercase;
  }
  .tag-tt {
    background: rgba(35, 134, 54, 0.25);
    color: #4ac26b;
  }
  .tag-pt {
    background: rgba(80, 140, 220, 0.25);
    color: #6aa6ff;
  }
  .tag-invalid {
    background: rgba(248, 81, 73, 0.25);
    color: #f48771;
  }
  .mnem {
    color: var(--gs-fg-bright);
  }
  .ops {
    color: var(--gs-fg);
  }
  .cmt {
    color: var(--gs-fg-muted);
    font-style: italic;
  }
</style>
