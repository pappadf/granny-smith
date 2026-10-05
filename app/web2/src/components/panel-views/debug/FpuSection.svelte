<script lang="ts">
  import SectionHeading from '@/components/ui/SectionHeading.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32 } from '@/lib/hex';

  // The section's presence is gated on the static capability machine.fpu
  // (capabilities.cpu.fpu), so it appears immediately for FPU machines and
  // never for 68000 machines (Plus / SE).  The data comes from the shared
  // frame's FPU block, in one shape for every architecture: 68K fp0-fp7 and
  // FPCR/FPSR/FPIAR, or PPC fpr0-fpr31 and FPSCR.
  const fpu = $derived(debugFrame.current?.fpu ?? null);

  // Changed since the previous distinct frame, by register; persists until
  // the next.  A frame identical to the previous is the same stop fetched
  // again (see RegistersSection) and keeps the marks.
  let prev: { data: string[]; control: Record<string, number> } | null = null;
  let dataChanged = $state<boolean[]>([]);
  let ctlChanged = $state<Record<string, boolean>>({});
  $effect(() => {
    const next = fpu;
    if (!next) {
      prev = null;
      dataChanged = [];
      ctlChanged = {};
      return;
    }
    const data = next.data.map((r) => r.hex);
    const control = Object.fromEntries(next.control.map((c) => [c.name, c.value]));
    if (
      prev &&
      data.length === prev.data.length &&
      data.every((h, i) => h === prev!.data[i]) &&
      next.control.every((c) => prev!.control[c.name] === c.value)
    )
      return;
    dataChanged = prev ? data.map((h, i) => h !== prev!.data[i]) : [];
    ctlChanged = prev
      ? Object.fromEntries(next.control.map((c) => [c.name, prev!.control[c.name] !== c.value]))
      : {};
    prev = { data, control };
  });
</script>

{#if machine.fpu}
  <CollapsibleSection title="FPU" open={debug.sections.fpu} onToggle={() => toggleSection('fpu')}>
    {#if machine.status === 'running'}
      <Hint class="fpu-hint" inset="block">Pause the machine to inspect FPU state.</Hint>
    {:else if fpu}
      <div class="fpu-group">
        <SectionHeading level="h4" class="fpu-group-title">Data</SectionHeading>
        <div class="fpu-rows">
          {#each fpu.data as r, i (i)}
            <span class="fpu-name" class:changed={dataChanged[i]}>{fpu.prefix}{i}</span>
            <span class="fpu-hex" class:changed={dataChanged[i]}>{r.hex}</span>
            <span class="fpu-val" class:changed={dataChanged[i]} title={r.val}>{r.val}</span>
          {/each}
        </div>
      </div>
      <div class="fpu-group">
        <SectionHeading level="h4" class="fpu-group-title">Control</SectionHeading>
        <div class="fpu-ctl">
          {#each fpu.control as c (c.name)}
            <div class="fpu-ctl-row" class:changed={ctlChanged[c.name]}>
              <span class="fpu-name">{c.name.toUpperCase()}</span>
              <span class="fpu-hex">{fmtHex32(c.value)}</span>
            </div>
          {/each}
        </div>
      </div>
    {:else}
      <Hint class="fpu-hint" inset="block">No machine running.</Hint>
    {/if}
  </CollapsibleSection>
{/if}

<style>
  .fpu-group {
    padding: var(--gs-space-1-5) var(--gs-space-3);
  }
  /* Data register grid: name | raw hex | decimal value. Hex is fixed-
     width (20 chars + underscore = 21 ch), value gets the remaining
     row so very-long decimals can shrink with ellipsis. */
  .fpu-rows {
    display: grid;
    grid-template-columns: auto auto 1fr;
    column-gap: var(--gs-space-4);
    row-gap: 0;
    align-items: center;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: 18px;
  }
  /* Control registers stack as plain flex rows — each label sits
     directly next to its value, no grid-track stretching. */
  .fpu-ctl {
    display: flex;
    flex-direction: column;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: 18px;
  }
  .fpu-ctl-row {
    display: flex;
    align-items: baseline;
    gap: var(--gs-space-2);
  }
  .fpu-name {
    color: var(--gs-code-reg-name);
    text-align: right;
    min-width: 4ch;
  }
  .fpu-hex {
    color: var(--gs-code-reg-value);
    text-transform: uppercase; /* hex digits */
    white-space: nowrap;
  }
  .fpu-val {
    color: var(--gs-code-reg-value);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }
  /* Diff highlight on changed register cells; applies to the data
     register's three columns (name + hex + val) and to a control
     register row as a whole. */
  .fpu-name.changed,
  .fpu-hex.changed,
  .fpu-val.changed,
  .fpu-ctl-row.changed {
    background: var(--gs-code-changed-bg);
    border-radius: var(--gs-radius-xs);
  }
</style>
