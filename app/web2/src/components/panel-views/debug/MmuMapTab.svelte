<script lang="ts">
  import Badge from '@/components/ui/Badge.svelte';
  import Button from '@/components/ui/Button.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';
  import { mapRange, MAP_LIMIT, type MapRun } from '@/bus/mmu';
  import { debug, inspectMmuWalk } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { fmtSize, runEnd } from '@/lib/mmu';

  // The mapped address ranges, from the core's machine.cpu.mmu.map: runs
  // that translate linearly with the same via and access.  Blank End scans
  // to the end of the address space.
  let startInput = $state(fmtHex32(0));
  let endInput = $state('');
  let range = $state<{ start: number; end: number | undefined }>({ start: 0, end: undefined });
  let rows = $state<MapRun[] | null>(null);

  // Re-map whenever the range, the S/U choice or the machine state changes.
  $effect(() => {
    void debugFrame.current;
    const { start, end } = range;
    const sup = debug.mmuSupervisor;
    void mapRange(start, end, sup).then((r) => (rows = r));
  });

  function scan() {
    const s = parseHex(startInput);
    const e = endInput.trim() === '' ? undefined : parseHex(endInput);
    if (s === null || e === null) return;
    range = { start: s, end: e };
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
    <TextInput
      class="addr"
      hex
      widthCh={10}
      bind:value={startInput}
      onkeydown={onKey}
      aria-label="Range start"
    />
    <span class="lbl">End:</span>
    <TextInput
      class="addr"
      hex
      widthCh={10}
      bind:value={endInput}
      onkeydown={onKey}
      placeholder="(all)"
      aria-label="Range end (exclusive)"
    />
    <Button class="btn" onclick={scan}>Scan</Button>
  </div>
  {#if rows === null}
    <p class="hint">Reading the MMU…</p>
  {:else if rows.length === 0}
    <p class="hint">Nothing is mapped in this range.</p>
  {:else}
    {#each rows as r, i (i)}
      <button
        type="button"
        class="map-row"
        title="Walk L:${fmtHex32(r.start)}"
        onclick={() => inspectMmuWalk(r.start)}
      >
        <span class="map-l">L:${fmtHex32(r.start)}–${fmtHex32(runEnd(r.start, r.size) - 1)}</span>
        <span class="map-p">P:${fmtHex32(r.phys)}–${fmtHex32(r.phys + r.size - 1)}</span>
        <span class="map-meta">
          {fmtSize(r.size)}
          <Badge class="tag" intent="info">{r.via.toUpperCase()}</Badge>
          <Badge class="tag" intent="neutral">{r.access}</Badge>
          {#if r.space}<Badge class="tag" intent="success">{r.space}</Badge>{/if}
        </span>
      </button>
    {/each}
    {#if rows.length >= MAP_LIMIT}
      <p class="hint">
        The first {MAP_LIMIT} runs; scan again from ${fmtHex32(
          runEnd(rows[rows.length - 1].start, rows[rows.length - 1].size),
        )} for more.
      </p>
    {/if}
  {/if}
</div>

<style>
  .map-body {
    padding: var(--gs-space-2) var(--gs-space-3);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
  }
  .map-header {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1-5);
    flex-wrap: wrap;
    margin-bottom: var(--gs-space-1);
  }
  .lbl,
  .hint {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    margin: 0;
  }
  .map-row {
    display: grid;
    grid-template-columns: auto auto 1fr;
    column-gap: var(--gs-space-3);
    align-items: center;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: 1.7;
    color: var(--gs-text);
    background: none;
    border: none;
    padding: 0;
    text-align: left;
    cursor: pointer;
  }
  .map-row:hover {
    background: var(--gs-row-hover);
  }
  .map-meta {
    color: var(--gs-text-muted);
    display: inline-flex;
    gap: var(--gs-space-1);
    align-items: center;
  }
</style>
