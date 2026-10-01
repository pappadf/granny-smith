<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { peekBytes } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { translateMany, addrLabel, type Translation } from '@/bus/mmu';
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
    void debug.refreshGen;
    if (debug.sections.memory) void refresh();
  });

  async function refresh() {
    loading = true;
    try {
      // The space is always explicit: "logical" reads through the CPU's own
      // translation on every architecture (the plain read is physical on a
      // PowerPC machine), "physical" really is physical (it used to be the
      // logical read under a physical label).
      bytes = await peekBytes(
        debug.memoryAddress,
        128,
        physicalAvailable ? debug.memoryMode : 'logical',
      );
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

  // The Lisa's three physical spaces (RAM, I/O, ROM) cannot be named by a
  // bare address, so its Memory pane is logical only.
  const physicalAvailable = $derived(machine.mmuKind !== 'lisa_segment');

  // Real translations for the row labels in logical mode (bus/mmu.ts).
  let xl = $state<Record<number, Translation>>({});
  $effect(() => {
    void bytes;
    if (!machine.mmuEnabled || debug.memoryMode !== 'logical') return;
    const rowsAt = Array.from({ length: 8 }, (_, i) => (debug.memoryAddress + i * 16) >>> 0);
    void translateMany(rowsAt).then((m) => (xl = m));
  });

  function rowLogicalLabel(rowIndex: number): string {
    const a = ((debug.memoryAddress + rowIndex * 16) >>> 0) & 0xffffffff;
    if (machine.mmuEnabled && debug.memoryMode === 'logical') return addrLabel(a, xl[a]);
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
    {#if machine.mmuEnabled && physicalAvailable}
      <span class="mem-sep"></span>
      <span class="mem-label">Mode:</span>
      <div class="mem-mode" role="group" aria-label="Memory access mode">
        <button
          type="button"
          class="mem-mode-btn"
          class:active={debug.memoryMode === 'logical'}
          aria-pressed={debug.memoryMode === 'logical'}
          onclick={() => setMode('logical')}
        >
          Logical
        </button>
        <button
          type="button"
          class="mem-mode-btn"
          class:active={debug.memoryMode === 'physical'}
          aria-pressed={debug.memoryMode === 'physical'}
          onclick={() => setMode('physical')}
        >
          Physical
        </button>
      </div>
    {/if}
  </div>
  <div class="mem-body">
    {#if machine.status === 'running'}
      <p class="mem-hint">Pause the machine to inspect memory.</p>
    {:else if loading && !bytes}
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
    gap: var(--gs-space-1-5);
    padding: var(--gs-space-1-5) var(--gs-space-3);
    flex-wrap: wrap;
  }
  .mem-label {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
  }
  .mem-addr {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: var(--gs-border-width) solid var(--gs-input-border);
    border-radius: var(--gs-radius-xs);
    height: var(--gs-size-control);
    padding: 0 var(--gs-space-1-5);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    width: 10ch;
    outline: none;
    text-transform: uppercase; /* hex digits */
  }
  .mem-addr:focus {
    border-color: var(--gs-focus-ring);
  }
  .mem-btn {
    background: transparent;
    color: var(--gs-text);
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    height: var(--gs-size-control);
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
  }
  .mem-btn:hover {
    background: var(--gs-row-hover);
  }
  .mem-sep {
    flex: 0 0 1px;
    height: 14px;
    background: var(--gs-border);
    margin: 0 var(--gs-space-1);
  }
  .mem-mode {
    display: inline-flex;
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    overflow: hidden;
    height: var(--gs-size-control);
  }
  .mem-mode-btn {
    background: transparent;
    color: var(--gs-text-muted);
    border: none;
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    cursor: pointer;
  }
  .mem-mode-btn.active {
    background: var(--gs-row-selected);
    color: var(--gs-text-strong);
  }
  .mem-body {
    padding: var(--gs-space-1) var(--gs-space-3) var(--gs-space-2);
  }
  .mem-hint {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
  }
  .mem-row {
    /* All three columns are content-width so the ASCII gutter sits
       directly after the hex bytes instead of being pushed to the
       right edge of a stretched 1fr column. */
    display: grid;
    grid-template-columns: auto auto auto;
    column-gap: 18px;
    justify-content: start;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: var(--gs-line-height-code);
  }
  .mem-row-addr {
    color: var(--gs-text-muted);
    white-space: nowrap;
  }
  .mem-row-bytes {
    display: inline-flex;
    gap: var(--gs-space-1);
    flex-wrap: nowrap;
    color: var(--gs-text);
  }
  .mem-row-ascii {
    color: var(--gs-text-muted);
    white-space: pre;
  }
</style>
