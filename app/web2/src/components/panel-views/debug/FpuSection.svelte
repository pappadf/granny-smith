<script lang="ts">
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

  // Changed since the previous frame, by register; persists until the next.
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
      <p class="fpu-hint">Pause the machine to inspect FPU state.</p>
    {:else if fpu}
      <div class="fpu-group">
        <h4 class="fpu-group-title">Data</h4>
        <div class="fpu-rows">
          {#each fpu.data as r, i (i)}
            <span class="fpu-name" class:changed={dataChanged[i]}>{fpu.prefix}{i}</span>
            <span class="fpu-hex" class:changed={dataChanged[i]}>{r.hex}</span>
            <span class="fpu-val" class:changed={dataChanged[i]} title={r.val}>{r.val}</span>
          {/each}
        </div>
      </div>
      <div class="fpu-group">
        <h4 class="fpu-group-title">Control</h4>
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
      <p class="fpu-hint">No machine running.</p>
    {/if}
  </CollapsibleSection>
{/if}

<style>
  .fpu-hint {
    color: var(--gs-fg-muted);
    font-size: 11px;
    padding: 8px 16px;
  }
  .fpu-group {
    padding: 6px 12px;
  }
  .fpu-group-title {
    font-size: 10px;
    font-weight: 600;
    color: var(--gs-fg-muted);
    margin: 6px 0 4px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  /* Data register grid: name | raw hex | decimal value. Hex is fixed-
     width (20 chars + underscore = 21 ch), value gets the remaining
     row so very-long decimals can shrink with ellipsis. */
  .fpu-rows {
    display: grid;
    grid-template-columns: auto auto 1fr;
    column-gap: 16px;
    row-gap: 0;
    align-items: center;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    line-height: 18px;
  }
  /* Control registers stack as plain flex rows — each label sits
     directly next to its value, no grid-track stretching. */
  .fpu-ctl {
    display: flex;
    flex-direction: column;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    line-height: 18px;
  }
  .fpu-ctl-row {
    display: flex;
    align-items: baseline;
    gap: 8px;
  }
  .fpu-name {
    color: var(--gs-fg-muted);
    text-align: right;
    min-width: 4ch;
  }
  .fpu-hex {
    color: var(--gs-fg);
    text-transform: uppercase;
    white-space: nowrap;
  }
  .fpu-val {
    color: var(--gs-fg);
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
    background: var(--gs-changed-bg);
    border-radius: 2px;
  }
</style>
