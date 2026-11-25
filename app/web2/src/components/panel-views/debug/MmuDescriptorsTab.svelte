<script lang="ts">
  import Button from '@/components/ui/Button.svelte';
  import Select from '@/components/ui/Select.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';
  import { descriptorFormats, readDescriptors, walkAddr, type Descriptor } from '@/bus/mmu';
  import { mmuView } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { machine } from '@/state/machine.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { showNotification } from '@/state/toasts.svelte';
  import { formatEntry, formatLisaSegment } from '@/lib/mmu';

  // Raw descriptors decoded by the core's machine.cpu.mmu.descriptor: PMMU
  // or 68040 table entries and PowerPC page-table entries at a physical
  // address, or the Lisa's segment descriptors by segment number.
  const COUNT = 16;

  const formats = $derived(descriptorFormats(machine.mmuKind));
  const isLisa = $derived(machine.mmuKind === 'lisa_segment');

  let addrInput = $state('');
  let format = $state('');
  let at = $state<number | null>(null);
  let rows = $state<Descriptor[] | null>(null);

  // Seed from the PC's walk: the last descriptor it read (the PC's own page
  // descriptor, or the PTE in its group), in the format the walk read it as
  // -- a 68030 level carries desc_lo only when its descriptors are long; the
  // 68040's level names are its formats.  The Lisa's step names the segment.
  // A walk that read no descriptor (translation off, a TT or BAT hit) starts
  // at 0.
  async function fromPc() {
    const pc = debugFrame.current?.pc;
    if (pc === undefined) return;
    const { supervisor, context } = mmuView();
    const w = await walkAddr(pc, supervisor, false, context);
    const leaf = isLisa
      ? w?.steps.find((s) => s.step === 'segment')
      : w?.steps.findLast((s) => s.fields.addr !== undefined);
    let fmt = formats[0] ?? '';
    if (leaf && machine.mmuKind === '68030_pmmu')
      fmt = leaf.fields.desc_lo !== undefined ? 'long' : 'short';
    if (leaf && machine.mmuKind === '68040' && formats.includes(String(leaf.fields.name)))
      fmt = String(leaf.fields.name);
    format = fmt;
    const a = !leaf
      ? 0
      : isLisa
        ? Number(leaf.fields.index ?? 0)
        : Number(leaf.fields.addr) + 8 * Number(leaf.fields.slot ?? 0);
    addrInput = isLisa ? String(a) : fmtHex32(a);
    at = a >>> 0;
  }

  // First visit: start at the PC's walk.
  $effect(() => {
    if (at === null && debugFrame.current) void fromPc();
  });

  // Re-read whenever the address, the format or the machine state changes.
  $effect(() => {
    void debugFrame.current;
    const a = at;
    const f = format;
    const { context } = mmuView();
    if (a === null) return;
    void readDescriptors(
      a,
      COUNT,
      formats.length ? f || formats[0] : undefined,
      isLisa ? context : undefined,
    ).then((r) => (rows = r));
  });

  function read() {
    const v = isLisa ? Number.parseInt(addrInput, 10) : parseHex(addrInput);
    if (v === null || Number.isNaN(v)) {
      showNotification(`Invalid ${isLisa ? 'segment' : 'address'}`, 'error');
      return;
    }
    at = v;
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      read();
    }
  }
</script>

<div class="desc-body">
  <div class="desc-header">
    <span class="lbl">{isLisa ? 'Segment:' : 'Physical:'}</span>
    <TextInput
      class="addr"
      hex={!isLisa}
      widthCh={10}
      bind:value={addrInput}
      onkeydown={onKey}
      aria-label={isLisa ? 'First segment number' : 'Physical address of the first descriptor'}
    />
    {#if formats.length > 1}
      <Select size="inline" bind:value={format} aria-label="Descriptor format">
        {#each formats as f (f)}<option value={f}>{f}</option>{/each}
      </Select>
    {/if}
    <Button class="btn" onclick={read}>Read</Button>
    <Button class="btn" onclick={fromPc}>From PC</Button>
  </div>
  {#if rows === null}
    <p class="hint">Reading the MMU…</p>
  {:else if isLisa}
    {#if rows.length}
      <p class="hint">
        Context {rows[0].context}{rows[0].context === 0 ? ' (the operating system)' : ''}
      </p>
    {/if}
    <table class="seg-table" aria-label="Segment descriptors">
      <thead>
        <tr>
          <th>Seg</th><th>Logical</th><th>Type</th><th>Length</th><th>Physical</th><th>Raw</th>
        </tr>
      </thead>
      <tbody>
        {#each rows as d, i (i)}
          {@const r = formatLisaSegment(d)}
          <tr class="seg-row" data-type={d.type} data-present={r.present ? 'yes' : 'no'}>
            <td>{r.segment}</td>
            <td>{r.logical}</td>
            <td>{r.type}</td>
            <td>{r.length}</td>
            <td>{r.physical}</td>
            <td class="raw">{r.raw}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  {:else}
    {#each rows as d, i (i)}
      {@const e = formatEntry(String(d.type ?? ''), d)}
      <div class="desc-row" data-type={d.type}>
        <span class="desc-main">
          {isLisa ? `seg ${d.segment}` : ''}{e.main ? ` ${e.main}` : ''}
        </span>
        {#if e.extra.length}<span class="desc-extra">{e.extra.join(' · ')}</span>{/if}
      </div>
    {/each}
  {/if}
</div>

<style>
  .desc-body {
    padding: var(--gs-space-2) var(--gs-space-3);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
  }
  .desc-header {
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
  .desc-row {
    display: flex;
    flex-direction: column;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: 1.5;
    color: var(--gs-text);
  }
  .seg-table {
    border-collapse: collapse;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
  }
  .seg-table th {
    color: var(--gs-text-muted);
    font-weight: normal;
    text-align: left;
    padding-right: var(--gs-space-3);
  }
  .seg-table td {
    color: var(--gs-text);
    padding-right: var(--gs-space-3);
    white-space: nowrap;
  }
  .seg-row[data-present='no'] td,
  .seg-table td.raw {
    color: var(--gs-text-muted);
  }
  .desc-row[data-type='invalid'] {
    color: var(--gs-text-muted);
  }
  .desc-extra {
    color: var(--gs-text-muted);
    padding-left: var(--gs-space-3);
    overflow-wrap: anywhere;
  }
</style>
