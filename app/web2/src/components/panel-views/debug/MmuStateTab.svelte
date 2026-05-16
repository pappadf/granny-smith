<script lang="ts">
  import { mmuRegs } from '@/bus/mockMmu';
  import { decodeTc, decodeCrpFromHex } from '@/lib/mmu';
  import { fmtHex32 } from '@/lib/hex';

  const tc = $derived(decodeTc(mmuRegs.tc));
  const crp = $derived(decodeCrpFromHex(mmuRegs.crp));
  const srp = $derived(decodeCrpFromHex(mmuRegs.srp));
  const summary = $derived(
    `MMU ${mmuRegs.enabled ? 'enabled' : 'disabled'} · ${mmuRegs.pageSize} B pages · ${mmuRegs.levels}-level walk · ` +
      (tc.SRE === 1 ? 'split S/U roots (SRE=1)' : 'single root (SRE=0)'),
  );
</script>

<div class="state-body">
  <p class="summary">{summary}</p>
  <div class="reg-block">
    <div class="reg-line">
      <span class="reg-name">TC</span>= $<span class="reg-hex">{fmtHex32(mmuRegs.tc)}</span>
    </div>
    <div class="reg-decoded">
      E={tc.E} · SRE={tc.SRE} · FCL={tc.FCL} · PS={tc.PS} ({1 << tc.PS} B) · IS={tc.IS} · TIA={tc.TIA}
      TIB={tc.TIB} TIC={tc.TIC} TID={tc.TID}
    </div>
  </div>
  <div class="reg-block">
    <div class="reg-line"><span class="reg-name">CRP</span>= ${mmuRegs.crp}</div>
    <div class="reg-decoded">
      limit={crp.limit} · DT={crp.dt} · pointer=$<span class="reg-hex">{fmtHex32(crp.pointer)}</span
      >
    </div>
  </div>
  <div class="reg-block">
    <div class="reg-line"><span class="reg-name">SRP</span>= ${mmuRegs.srp}</div>
    <div class="reg-decoded">
      limit={srp.limit} · DT={srp.dt} · pointer=$<span class="reg-hex">{fmtHex32(srp.pointer)}</span
      >
    </div>
  </div>
  <div class="reg-block">
    <div class="reg-line">
      <span class="reg-name">TT0</span>= $<span class="reg-hex">{fmtHex32(mmuRegs.tt0)}</span>
    </div>
  </div>
  <div class="reg-block">
    <div class="reg-line">
      <span class="reg-name">TT1</span>= $<span class="reg-hex">{fmtHex32(mmuRegs.tt1)}</span>
    </div>
  </div>
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
    font-size: 12px;
    margin: 0 0 4px 0;
  }
  .reg-block {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .reg-line {
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
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
