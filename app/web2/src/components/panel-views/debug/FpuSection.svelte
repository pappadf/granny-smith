<script lang="ts">
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { loadDebugFrame, type FpuFrame } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { fmtHex32 } from '@/lib/hex';

  // The section's presence is gated on the static capability machine.fpu
  // (capabilities.cpu.fpu), so it appears immediately for FPU machines and
  // never for 68000 machines (Plus / SE) — no waiting for the first debug
  // frame to carry an `fpu` block. The register data still comes from the
  // frame's optional `fpu` block once the machine is paused.
  let fpu = $state<FpuFrame | null>(null);
  // Per-register diff masks vs the previous refresh. Persist until the
  // next refresh produces a new diff.
  let fpChanged = $state<boolean[]>([]);
  let ctlChanged = $state<{ fpcr: boolean; fpsr: boolean; fpiar: boolean }>({
    fpcr: false,
    fpsr: false,
    fpiar: false,
  });

  async function refresh() {
    const frame = await loadDebugFrame();
    if (!frame) return;
    const next = frame.fpu ?? null;
    if (fpu && next) {
      // Compare hex strings — covers any change in the underlying
      // 80-bit register without dragging the host float conversion
      // into the diff path.
      const fpMask = new Array<boolean>(8);
      for (let i = 0; i < 8; i++) {
        fpMask[i] = (fpu.fp[i]?.hex ?? '') !== (next.fp[i]?.hex ?? '');
      }
      fpChanged = fpMask;
      ctlChanged = {
        fpcr: fpu.fpcr !== next.fpcr,
        fpsr: fpu.fpsr !== next.fpsr,
        fpiar: fpu.fpiar !== next.fpiar,
      };
    } else {
      fpChanged = [];
      ctlChanged = { fpcr: false, fpsr: false, fpiar: false };
    }
    fpu = next;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    void debug.refreshGen;
    if (machine.status === 'paused' || machine.status === 'running') void refresh();
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
          {#each fpu.fp as r, i (i)}
            <span class="fpu-name" class:changed={fpChanged[i]}>FP{i}</span>
            <span class="fpu-hex" class:changed={fpChanged[i]}>{r.hex}</span>
            <span class="fpu-val" class:changed={fpChanged[i]} title={r.val}>{r.val}</span>
          {/each}
        </div>
      </div>
      <div class="fpu-group">
        <h4 class="fpu-group-title">Control</h4>
        <div class="fpu-ctl">
          <div class="fpu-ctl-row" class:changed={ctlChanged.fpcr}>
            <span class="fpu-name">FPCR</span>
            <span class="fpu-hex">{fmtHex32(fpu.fpcr)}</span>
          </div>
          <div class="fpu-ctl-row" class:changed={ctlChanged.fpsr}>
            <span class="fpu-name">FPSR</span>
            <span class="fpu-hex">{fmtHex32(fpu.fpsr)}</span>
          </div>
          <div class="fpu-ctl-row" class:changed={ctlChanged.fpiar}>
            <span class="fpu-name">FPIAR</span>
            <span class="fpu-hex">{fmtHex32(fpu.fpiar)}</span>
          </div>
        </div>
      </div>
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
