<script lang="ts">
  import { onMount } from 'svelte';
  import { disasmAt, readRegisters, type DisasmRow } from '@/bus/debug';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug, inspectMmuWalk, inspectMemoryAt } from '@/state/debug.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32 } from '@/lib/hex';

  const ROWS = 32;
  // Naive instruction stride — 68K has variable-length instructions; we
  // ask for ROWS rows centered on PC. The shell disasm honors `count`,
  // and the WORD-aligned step approximation gets us close enough for
  // the "centered" listing in Phase 6.
  const BACK_BYTES = 16;

  let rows = $state<DisasmRow[]>([]);
  let pc = $state(0);

  async function refresh() {
    const regs = await readRegisters();
    pc = regs?.pc ?? 0;
    debug.currentPc = pc;
    const start = Math.max(0, (pc - BACK_BYTES) >>> 0);
    rows = await disasmAt(start, ROWS);
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    // Re-fetch on pause / running transitions and after each Step.
    void machine.status;
    void refresh();
  });

  function bannerLabel(): string {
    const model = machine.model ?? 'Machine';
    if (!machine.mmuEnabled) {
      return `${model} · PC at $${fmtHex32(pc)}`;
    }
    const r = mmuLookup(pc);
    if (!r.valid) return `${model} · PC at L:$${fmtHex32(pc)} (no MMU mapping)`;
    return `${model} · PC at L:$${fmtHex32(pc)} P:$${fmtHex32(r.phys ?? 0)} ${r.kind ?? 'PT'}`;
  }

  function rowAddrLabel(row: DisasmRow): { logical: string; physical?: string; tag?: string } {
    if (!machine.mmuEnabled) {
      return { logical: `$${fmtHex32(row.addr)}` };
    }
    const r = mmuLookup(row.addr);
    if (!r.valid) return { logical: `L:$${fmtHex32(row.addr)}`, tag: 'INVALID' };
    return {
      logical: `L:$${fmtHex32(row.addr)}`,
      physical: `P:$${fmtHex32(r.phys ?? 0)}`,
      tag: r.kind ?? 'PT',
    };
  }

  function onRowContext(row: DisasmRow, ev: MouseEvent) {
    ev.preventDefault();
    const mmuOn = machine.mmuEnabled;
    const items: ContextMenuItem[] = [
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
          const r = mmuLookup(row.addr);
          if (r.valid && r.phys !== undefined && navigator.clipboard?.writeText)
            void navigator.clipboard.writeText(`$${fmtHex32(r.phys)}`);
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
</script>

<div class="disasm-pane">
  <div class="banner">{bannerLabel()}</div>
  {#if rows.length === 0}
    <p class="hint">Pause the machine to see the disasm listing.</p>
  {:else}
    {#each rows as row, i (row.addr * 100 + i)}
      {@const isPc = row.addr === pc}
      {@const addr = rowAddrLabel(row)}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div class="row" class:pc={isPc} oncontextmenu={(ev) => onRowContext(row, ev)}>
        <span class="marker">{isPc ? '►' : ''}</span>
        <span class="addr-l">{addr.logical}</span>
        {#if addr.physical}<span class="addr-p">{addr.physical}</span>{/if}
        {#if addr.tag}<span class="tag tag-{addr.tag.toLowerCase()}">{addr.tag}</span>{/if}
        <span class="mnem">{row.mnem}</span>
        <span class="ops">{row.ops}</span>
        {#if row.cmt}<span class="cmt">; {row.cmt}</span>{/if}
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
    font-size: 13px;
  }
  .banner {
    position: sticky;
    top: 0;
    z-index: 1;
    background: rgba(0, 127, 212, 0.1);
    border-left: 2px solid var(--gs-focus, #0969da);
    color: var(--gs-fg);
    font-size: 11px;
    padding: 4px 12px;
  }
  .hint {
    color: var(--gs-fg-muted);
    padding: 12px;
    font-size: 12px;
  }
  .row {
    display: grid;
    grid-template-columns: 14px auto auto auto auto 1fr auto;
    column-gap: 8px;
    align-items: center;
    height: 22px;
    padding: 0 8px 0 4px;
    line-height: 22px;
    cursor: default;
    white-space: nowrap;
  }
  .row:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .row.pc {
    background: rgba(80, 140, 220, 0.2);
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
