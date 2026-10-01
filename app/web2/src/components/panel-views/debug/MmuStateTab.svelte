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
    padding: var(--gs-space-2) var(--gs-space-3);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
  }
  .summary {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    margin: 0 0 var(--gs-space-1) 0;
  }
  .reg-block {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  .reg-line {
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    color: var(--gs-text);
  }
  .reg-name {
    color: var(--gs-text-strong);
    font-weight: var(--gs-font-weight-semibold);
    margin-right: var(--gs-space-2);
  }
  .reg-hex {
    text-transform: uppercase; /* hex digits */
  }
  .reg-decoded {
    color: var(--gs-text-muted);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    margin-left: var(--gs-space-4);
  }
</style>
