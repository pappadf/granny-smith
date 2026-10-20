<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import Badge from '@/components/ui/Badge.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import { tick } from 'svelte';
  import {
    addBreakpoint,
    listBreakpoints,
    removeBreakpointAt,
    type DebugFrameRow,
  } from '@/bus/debug';
  import { debugFrame, ROWS_BEFORE_PC } from '@/state/debugFrame.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug, bumpBreakpoints, inspectMmuWalk, inspectMemoryAt } from '@/state/debug.svelte';
  import { fmtHex32 } from '@/lib/hex';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';
  import { readMetric } from '@/lib/tokens';

  // The PC sits on this (1-indexed) line after a refresh: the shared frame
  // is fetched with ROWS_BEFORE_PC rows ahead of the PC, re-synchronised by
  // the core so a row always lands exactly on it (68K instructions are
  // variable-length).  Before, this pane chose its window from the PREVIOUS
  // PC, so after a breakpoint hit or a far jump it showed the old code with
  // no PC marker, and decoding from pc-16 could step over the PC.
  const PC_ANCHOR_LINE = ROWS_BEFORE_PC + 1;

  const rows = $derived<DebugFrameRow[]>(debugFrame.current?.rows ?? []);
  const pc = $derived(debugFrame.current?.pc ?? 0);

  // A new frame: record the PC and anchor it once the rows are in the DOM.
  $effect(() => {
    void debugFrame.current;
    debug.currentPc = pc;
    void tick().then(anchorPcRow);
  });

  // Scroll the pane so the PC row sits at the configured anchor line.
  // No-op when PC isn't in the current window (start-of-day before the
  // first frame loads).
  function anchorPcRow(): void {
    if (!paneEl) return;
    const pcIdx = rows.findIndex((r) => r.addr === pc);
    if (pcIdx < 0) return;
    // Rows are --gs-size-row tall (`.row` below); read at use, so a skin's
    // row height is honoured.
    const target = (pcIdx - (PC_ANCHOR_LINE - 1)) * readMetric('--gs-size-row', 22);
    paneEl.scrollTop = Math.max(0, target);
  }

  // The breakpoints by address (true: enabled), for the row markers and
  // the context menu; listed again with each frame and after any add or
  // remove.  Only the latest listing is kept.
  let bps = $state<Map<number, boolean>>(new Map());
  let bpGen = 0;
  $effect(() => {
    void debugFrame.current;
    void debug.breakpointsGen;
    const gen = ++bpGen;
    void listBreakpoints().then((list) => {
      if (gen === bpGen) bps = new Map(list.map((b) => [b.addr, b.enabled]));
    });
  });

  function bpState(addr: number): 'breakpoint' | 'breakpoint-off' | undefined {
    const on = bps.get(addr >>> 0);
    return on === undefined ? undefined : on ? 'breakpoint' : 'breakpoint-off';
  }

  function bannerLabel(): string {
    const model = machine.model ?? 'Machine';
    // The frame is read while paused; running, there is no current PC to
    // show (it read "PC at $00000000").
    if (machine.status === 'running') return `${model} · running`;
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
    // Add on a row without a breakpoint, Remove on one with.
    const items: ContextMenuItem[] = [
      bps.has(row.addr >>> 0)
        ? {
            label: `Remove breakpoint at $${fmtHex32(row.addr)}`,
            action: async () => {
              const ok = await removeBreakpointAt(row.addr);
              if (!ok) showNotification('Failed to remove breakpoint', 'error');
              bumpBreakpoints();
            },
          }
        : {
            label: `Add breakpoint at $${fmtHex32(row.addr)}`,
            action: async () => {
              const added = await addBreakpoint(row.addr);
              if (added !== true) {
                showNotification(`Failed to add breakpoint: ${added}`, 'error');
                return;
              }
              showNotification(`Breakpoint set at $${fmtHex32(row.addr)}`, 'info');
              bumpBreakpoints();
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

  // The address tags' intents: transparent translation, page table, none.
  const TAG_INTENT: Record<string, 'success' | 'info' | 'danger'> = {
    tt: 'success',
    pt: 'info',
    invalid: 'danger',
  };
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
    <Hint class="hint" inset="pane">Pause the machine to see the disasm listing.</Hint>
  {:else if rows.length === 0}
    <Hint class="hint" inset="pane"
      >{debugFrame.loading ? 'Disassembling…' : 'No machine running.'}</Hint
    >
  {:else}
    {#each rows as row, i (row.addr * 100 + i)}
      {@const isPc = row.addr === pc}
      {@const addr = rowAddrLabel(row)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="row"
        class:pc={isPc}
        class:selected={selectedIdx === i}
        data-state={bpState(row.addr)}
        oncontextmenu={(ev) => onRowContext(row, ev)}
      >
        <span class="marker"
          >{#if isPc}<Icon name="debug-stackframe" size="md" />{:else if bpState(row.addr)}<Icon
              name="circle-filled"
              size="xs"
            />{/if}</span
        >
        <!-- Address group is one grid cell so the mnem/ops columns
             stay aligned across rows even when addr-p / tag are
             absent. Without the wrapper, missing optional spans
             would shift mnem to an earlier column on some rows. -->
        <span class="addr">
          <span class="addr-l">{addr.logical}</span>
          {#if addr.physical}<span class="addr-p">{addr.physical}</span>{/if}
          {#if addr.tag}<Badge
              class="tag tag-{addr.tag.toLowerCase()}"
              intent={TAG_INTENT[addr.tag.toLowerCase()] ?? 'neutral'}>{addr.tag}</Badge
            >{/if}
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
    background: var(--gs-surface-app);
    font-family: var(--gs-font-mono);
    /* 11 px matches the body-text baseline used by section headers
       and the MMU descriptor lines; disasm rows shouldn't read larger
       than the surrounding chrome. */
    font-size: var(--gs-font-size-xs);
  }
  .banner {
    position: sticky;
    top: 0;
    z-index: var(--gs-z-raised);
    /* Opaque so disasm rows scrolling underneath don't bleed
       through; tinted border-left preserves the blue indicator. */
    background: var(--gs-surface-raised);
    border-left: var(--gs-border-width-strong) solid var(--gs-code-banner-rule);
    border-bottom: var(--gs-border-width) solid var(--gs-border);
    color: var(--gs-text);
    font-size: var(--gs-font-size-xs);
    padding: var(--gs-space-1) var(--gs-space-3);
  }
  .row {
    /* Four stable columns: PC marker, address group (logical / phys /
       tag inline), mnemonic, operands. mnem auto-sizes to the longest
       mnemonic across all rows, so operands always start at the same
       x. ops gets `1fr` to take the rest. */
    display: grid;
    grid-template-columns: 14px auto auto 1fr;
    column-gap: var(--gs-space-2);
    align-items: center;
    height: var(--gs-size-row);
    padding: 0 var(--gs-space-2) 0 var(--gs-space-1);
    line-height: var(--gs-size-row);
    cursor: default;
    white-space: nowrap;
  }
  .addr {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-1-5);
  }
  .row:hover {
    background: var(--gs-row-hover);
  }
  .row.pc {
    background: var(--gs-code-pc-row-bg);
  }
  .row.selected {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .disasm-pane:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .marker {
    display: inline-flex;
    justify-content: center;
    color: var(--gs-code-pc-marker);
  }
  .addr-l,
  .addr-p {
    color: var(--gs-code-address);
    text-transform: uppercase; /* hex digits */
  }
  .mnem {
    color: var(--gs-code-mnemonic);
  }
  .ops {
    color: var(--gs-code-operand);
  }
  /* A breakpoint row: its marker (the PC's, on the PC row) in the
     breakpoint colour. */
  .row:global([data-state='breakpoint']) .marker {
    color: var(--gs-code-breakpoint);
  }
  .row:global([data-state='breakpoint-off']) .marker {
    color: var(--gs-code-breakpoint-off);
  }
</style>
