<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { peekBytes, peekPhysBytes } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32, parseHex } from '@/lib/hex';

  let bytes = $state<Uint8Array | null>(null);
  let loading = $state(false);
  // inputValue tracks debug.memoryAddress but is mutable so the user can
  // type into it before committing with Enter / Go.
  // eslint-disable-next-line svelte/prefer-writable-derived
  let inputValue = $state(fmtHex32(debug.memoryAddress));

  $effect(() => {
    inputValue = fmtHex32(debug.memoryAddress);
  });

  $effect(() => {
    if (debug.sections.memory) void refresh();
  });

  // Re-fetch on pause/step transitions.
  $effect(() => {
    void machine.status;
    if (debug.sections.memory) void refresh();
  });

  async function refresh() {
    loading = true;
    try {
      const data =
        debug.memoryMode === 'physical'
          ? await peekPhysBytes(debug.memoryAddress, 128)
          : await peekBytes(debug.memoryAddress, 128);
      bytes = data;
    } finally {
      loading = false;
    }
  }

  function commitAddress() {
    const v = parseHex(inputValue);
    if (v === null) return;
    debug.memoryAddress = v;
    void refresh();
  }

  function onAddrKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      commitAddress();
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      inputValue = fmtHex32(debug.memoryAddress);
    }
  }

  function setMode(mode: 'logical' | 'physical') {
    debug.memoryMode = mode;
    void refresh();
  }

  function rowBytes(rowIndex: number): number[] {
    if (!bytes) return Array(16).fill(0);
    const start = rowIndex * 16;
    return Array.from({ length: 16 }, (_, i) => bytes![start + i] ?? 0);
  }

  function ascii(byte: number): string {
    if (byte >= 0x20 && byte < 0x7f) return String.fromCharCode(byte);
    return '.';
  }

  function rowLogicalLabel(rowIndex: number): string {
    const a = ((debug.memoryAddress + rowIndex * 16) >>> 0) & 0xffffffff;
    if (machine.mmuEnabled && debug.memoryMode === 'logical') {
      const r = mmuLookup(a);
      const phys = r.valid && r.phys !== undefined ? fmtHex32(r.phys) : '!';
      const tag = r.valid ? (r.kind ?? 'PT') : 'INVALID';
      return `L:$${fmtHex32(a)}  P:$${phys}  ${tag}`;
    }
    return `$${fmtHex32(a)}`;
  }
</script>

<CollapsibleSection
  title="Memory"
  open={debug.sections.memory}
  onToggle={() => toggleSection('memory')}
>
  <div class="mem-header">
    <span class="mem-label">Address:</span>
    <input
      type="text"
      class="mem-addr"
      bind:value={inputValue}
      onkeydown={onAddrKey}
      aria-label="Memory address"
    />
    <button type="button" class="mem-btn" onclick={commitAddress}>Go</button>
    {#if machine.mmuEnabled}
      <span class="mem-sep"></span>
      <span class="mem-label">Mode:</span>
      <div class="mem-mode" role="group" aria-label="Memory access mode">
        <button
          type="button"
          class="mem-mode-btn"
          class:active={debug.memoryMode === 'logical'}
          onclick={() => setMode('logical')}
        >
          Logical
        </button>
        <button
          type="button"
          class="mem-mode-btn"
          class:active={debug.memoryMode === 'physical'}
          onclick={() => setMode('physical')}
        >
          Physical
        </button>
      </div>
    {/if}
  </div>
  <div class="mem-body">
    {#if loading && !bytes}
      <p class="mem-hint">Reading…</p>
    {:else if !bytes}
      <p class="mem-hint">No machine running.</p>
    {:else}
      {#each [0, 1, 2, 3, 4, 5, 6, 7] as i (i)}
        <div class="mem-row">
          <span class="mem-row-addr">{rowLogicalLabel(i)}</span>
          <span class="mem-row-bytes">
            {#each rowBytes(i) as b, j (j)}
              <span class="mem-byte">{b.toString(16).padStart(2, '0').toUpperCase()}</span>
            {/each}
          </span>
          <span class="mem-row-ascii">{rowBytes(i).map(ascii).join('')}</span>
        </div>
      {/each}
    {/if}
  </div>
</CollapsibleSection>

<style>
  .mem-header {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 6px 12px;
    flex-wrap: wrap;
  }
  .mem-label {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
  .mem-addr {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 6px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    width: 10ch;
    outline: none;
    text-transform: uppercase;
  }
  .mem-addr:focus {
    border-color: var(--gs-focus);
  }
  .mem-btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 8px;
    font-size: 11px;
    cursor: pointer;
  }
  .mem-btn:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .mem-sep {
    flex: 0 0 1px;
    height: 14px;
    background: var(--gs-border);
    margin: 0 4px;
  }
  .mem-mode {
    display: inline-flex;
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    overflow: hidden;
    height: 22px;
  }
  .mem-mode-btn {
    background: transparent;
    color: var(--gs-fg-muted);
    border: none;
    padding: 0 8px;
    font-size: 11px;
    cursor: pointer;
  }
  .mem-mode-btn.active {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
    color: var(--gs-fg-bright);
  }
  .mem-body {
    padding: 4px 12px 8px;
  }
  .mem-hint {
    color: var(--gs-fg-muted);
    font-size: 12px;
  }
  .mem-row {
    display: grid;
    grid-template-columns: minmax(0, max-content) 1fr auto;
    column-gap: 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    line-height: 1.6;
  }
  .mem-row-addr {
    color: var(--gs-fg-muted);
    white-space: nowrap;
  }
  .mem-row-bytes {
    display: inline-flex;
    gap: 4px;
    flex-wrap: nowrap;
    color: var(--gs-fg);
  }
  .mem-row-ascii {
    color: var(--gs-fg-muted);
    white-space: pre;
  }
</style>
