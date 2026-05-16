<script lang="ts">
  import { mmuMap } from '@/bus/mockMmu';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { fmtRangePair } from '@/lib/mmu';

  let startInput = $state(fmtHex32(0x00000000));
  let endInput = $state(fmtHex32(0x1fffffff));
  let rows = $state(mmuMap(0x00000000, 0x1fffffff));

  function scan() {
    const s = parseHex(startInput);
    const e = parseHex(endInput);
    if (s === null || e === null) return;
    rows = mmuMap(s, e);
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      scan();
    }
  }
</script>

<div class="map-body">
  <div class="map-header">
    <span class="lbl">Start:</span>
    <input
      type="text"
      class="addr"
      bind:value={startInput}
      onkeydown={onKey}
      aria-label="Range start"
    />
    <span class="lbl">End:</span>
    <input
      type="text"
      class="addr"
      bind:value={endInput}
      onkeydown={onKey}
      aria-label="Range end"
    />
    <button type="button" class="btn" onclick={scan}>Scan</button>
  </div>
  {#if rows.length === 0}
    <p class="hint">No mapped ranges in this window.</p>
  {:else}
    {#each rows as r, i (i)}
      {@const pair = fmtRangePair(r.lo, r.hi, r.base, true)}
      <div class="map-row">
        <span class="map-l">{pair.l}</span>
        <span class="map-p">{pair.p}</span>
        <span class="map-meta"
          >({pair.size} · {r.kind}{r.flags.length ? ` · ${r.flags.join(' ')}` : ''})</span
        >
      </div>
    {/each}
  {/if}
</div>

<style>
  .map-body {
    padding: 8px 12px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .map-header {
    display: flex;
    align-items: center;
    gap: 6px;
    margin-bottom: 8px;
  }
  .lbl {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
  .addr {
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
  .addr:focus {
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
  }
  .map-row {
    display: grid;
    grid-template-columns: auto auto 1fr;
    column-gap: 16px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    line-height: 1.7;
  }
  .map-l {
    color: var(--gs-fg);
  }
  .map-p {
    color: var(--gs-fg);
  }
  .map-meta {
    color: var(--gs-fg-muted);
  }
</style>
