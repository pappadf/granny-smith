<script lang="ts">
  import { readMmuState, type MmuRegister } from '@/bus/mmu';
  import { decodeTc, decodeRootPointer } from '@/lib/mmu';
  import { fmtHex32 } from '@/lib/hex';
  import { machine } from '@/state/machine.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';

  // The MMU's own registers, read from the core for whichever kind this
  // machine has (bus/mmu.ts), re-read with each new debug frame.
  let regs = $state<MmuRegister[]>([]);
  $effect(() => {
    void debugFrame.current;
    const kind = machine.mmuKind;
    void readMmuState(kind).then((r) => (regs = r));
  });

  const byName = $derived(Object.fromEntries(regs.map((r) => [r.name, r.value])));
  // The 68030's TC / root pointers get their fields decoded as well.
  const pmmu = $derived(machine.mmuKind === '68030_pmmu' && 'tc' in byName);
  const tc = $derived(pmmu ? decodeTc(byName.tc) : null);
  const crp = $derived(pmmu ? decodeRootPointer(byName.crp_hi, byName.crp_lo) : null);
  const srp = $derived(pmmu ? decodeRootPointer(byName.srp_hi, byName.srp_lo) : null);

  const KIND_LABEL: Record<string, string> = {
    '68030_pmmu': '68030 PMMU',
    '68040': '68040 MMU',
    ppc_601: 'PowerPC 601 MMU',
    ppc_604: 'PowerPC 604 MMU',
    lisa_segment: 'Lisa segment MMU',
  };
</script>

<div class="state-body">
  <p class="summary">{KIND_LABEL[machine.mmuKind] ?? 'MMU'}</p>
  {#if regs.length === 0}
    <p class="summary">Reading the MMU…</p>
  {/if}
  {#each regs as r (r.name)}
    <div class="reg-block">
      <div class="reg-line">
        <span class="reg-name">{r.name.toUpperCase()}</span>= $<span class="reg-hex"
          >{fmtHex32(r.value)}</span
        >
      </div>
      {#if tc && r.name === 'tc'}
        <div class="reg-decoded">
          E={tc.E} · SRE={tc.SRE} · FCL={tc.FCL} · PS={tc.PS} ({1 << tc.PS} B) · IS={tc.IS} · TIA={tc.TIA}
          TIB={tc.TIB} TIC={tc.TIC} TID={tc.TID}
        </div>
      {:else if crp && r.name === 'crp_lo'}
        <div class="reg-decoded">
          CRP limit={crp.limit} · DT={crp.dt} · pointer=${fmtHex32(crp.pointer)}
        </div>
      {:else if srp && r.name === 'srp_lo'}
        <div class="reg-decoded">
          SRP limit={srp.limit} · DT={srp.dt} · pointer=${fmtHex32(srp.pointer)}
        </div>
      {/if}
    </div>
  {/each}
</div>

<style>
  .state-body {
    padding: 8px 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .summary {
    color: var(--gs-fg-muted);
    font-size: 11px;
    margin: 0 0 4px 0;
  }
  .reg-block {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .reg-line {
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    color: var(--gs-fg);
  }
  .reg-name {
    color: var(--gs-fg-bright);
    font-weight: 600;
    margin-right: 8px;
  }
  .reg-hex {
    text-transform: uppercase;
  }
  .reg-decoded {
    color: var(--gs-fg-muted);
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    margin-left: 16px;
  }
</style>
