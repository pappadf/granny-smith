<script lang="ts">
  import { mmuDescriptors } from '@/bus/mockMmu';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { debug, inspectMemoryAt } from '@/state/debug.svelte';

  let addrInput = $state(fmtHex32(0x001fe000));
  let countInput = $state('16');
  let rows = $state(mmuDescriptors(0x001fe000, 16));

  function decode() {
    const a = parseHex(addrInput);
    const c = parseInt(countInput, 10);
    if (a === null || !Number.isFinite(c) || c <= 0) return;
    rows = mmuDescriptors(a, Math.min(c, 256));
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      decode();
    }
  }

  function followNext(addr: number) {
    addrInput = fmtHex32(addr);
    rows = mmuDescriptors(addr, parseInt(countInput, 10) || 16);
  }

  function jumpToPhys(addr: number) {
    debug.memoryMode = 'physical';
    inspectMemoryAt(addr);
  }
</script>

<div class="desc-body">
  <div class="desc-header">
    <span class="lbl">Address:</span>
    <input
      type="text"
      class="addr"
      bind:value={addrInput}
      onkeydown={onKey}
      aria-label="Descriptor address"
    />
    <span class="lbl">Count:</span>
    <input
      type="text"
      class="count"
      bind:value={countInput}
      onkeydown={onKey}
      aria-label="Descriptor count"
    />
    <button type="button" class="btn" onclick={decode}>Decode</button>
  </div>
  {#each rows as r (r.addr)}
    <div class="desc-row">
      <span class="addr-cell">${fmtHex32(r.addr)}</span>
      <span class="word-cell">${fmtHex32(r.word)}</span>
      <span class="dt-cell">DT={r.dt} ({r.dtLabel})</span>
      {#if r.next !== undefined}
        <button class="link" type="button" onclick={() => followNext(r.next!)}>
          next=$<span class="hex">{fmtHex32(r.next)}</span>
        </button>
      {:else if r.phys !== undefined}
        <button class="link" type="button" onclick={() => jumpToPhys(r.phys!)}>
          phys=$<span class="hex">{fmtHex32(r.phys)}</span>
        </button>
        <span class="flags">
          U={r.flags?.U ?? 0} WP={r.flags?.WP ?? 0} M={r.flags?.M ?? 0} CI={r.flags?.CI ?? 0}
        </span>
      {/if}
    </div>
  {/each}
</div>

<style>
  .desc-body {
    padding: 8px 12px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .desc-header {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 8px;
  }
  .lbl {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
  .addr,
  .count {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 6px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    outline: none;
    text-transform: uppercase;
  }
  .addr {
    width: 10ch;
  }
  .count {
    width: 5ch;
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
  .desc-row {
    display: flex;
    align-items: center;
    gap: 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    line-height: 1.7;
    color: var(--gs-fg);
  }
  .addr-cell {
    color: var(--gs-fg-muted);
  }
  .dt-cell {
    color: var(--gs-fg-muted);
  }
  .link {
    background: transparent;
    color: var(--gs-link, #6aa6ff);
    border: none;
    cursor: pointer;
    font-family: inherit;
    font-size: inherit;
    padding: 0;
  }
  .link:hover {
    text-decoration: underline;
  }
  .hex {
    text-transform: uppercase;
  }
  .flags {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
</style>
