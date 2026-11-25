<script lang="ts">
  import { type MmuRegister } from '@/bus/mmu';
  import { decodeTc, decodeRootPointer } from '@/lib/mmu';
  import { fmtHex32 } from '@/lib/hex';
  import { machine } from '@/state/machine.svelte';

  // The MMU's own registers, read from the core for whichever kind this
  // machine has (bus/mmu.ts) by MmuSection: with each new debug frame, and
  // a few times a second while the machine runs (`live`).
  interface Props {
    regs: MmuRegister[];
    // Set once a read has come back, so an empty answer is not shown as an
    // endless "Reading…".
    loaded: boolean;
    live?: boolean;
  }
  let { regs, loaded, live = false }: Props = $props();

  const byName = $derived(Object.fromEntries(regs.map((r) => [r.name, r.value])));
  // The 68030's TC / root pointers get their fields decoded as well.
  const pmmu = $derived(machine.mmuKind === '68030_pmmu' && 'tc' in byName);
  const tc = $derived(pmmu ? decodeTc(byName.tc) : null);
  const crp = $derived(pmmu ? decodeRootPointer(byName.crp_hi, byName.crp_lo) : null);
  const srp = $derived(pmmu ? decodeRootPointer(byName.srp_hi, byName.srp_lo) : null);
  const isLisa = $derived(machine.mmuKind === 'lisa_segment');

  const KIND_LABEL: Record<string, string> = {
    '68030_pmmu': '68030 PMMU',
    '68040': '68040 MMU',
    ppc_601: 'PowerPC 601 MMU',
    ppc_604: 'PowerPC 604 MMU',
    lisa_segment: 'Lisa segment MMU',
  };

  // The Lisa's two registers in words.
  function lisaMeaning(name: string, value: number): string | null {
    if (name === 'start')
      return value
        ? 'setup mode: the boot ROM is programming the MMU (low addresses bypass it)'
        : 'translating: every address goes through the segments';
    if (name === 'context')
      return value === 0
        ? 'user programs would run in context 0, the operating system’s'
        : `user programs run in context ${value}; supervisor code always uses context 0 (the operating system)`;
    return null;
  }
</script>

<div class="state-body">
  <p class="summary">
    {KIND_LABEL[machine.mmuKind] ?? 'MMU'}{#if live}<span class="live">&nbsp;· live</span>{/if}
  </p>
  {#if regs.length === 0}
    <p class="summary">{loaded ? 'No MMU registers to show.' : 'Reading the MMU…'}</p>
  {/if}
  {#each regs as r (r.name)}
    <div class="reg-block" data-reg={r.name}>
      <div class="reg-line">
        <span class="reg-name">{r.name.toUpperCase()}</span>{#if isLisa}=
          <span class="reg-hex">{r.value}</span>{:else}= $<span class="reg-hex">
            {fmtHex32(r.value)}
          </span>{/if}
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
      {:else if isLisa && lisaMeaning(r.name, r.value)}
        <div class="reg-decoded">{lisaMeaning(r.name, r.value)}</div>
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
  .live {
    color: var(--gs-success-fg);
  }
  .reg-block {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  .reg-line {
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    color: var(--gs-code-reg-value);
  }
  .reg-name {
    color: var(--gs-code-reg-name);
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
